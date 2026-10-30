#include "process/runner_single.hpp"

#include "process/accounting.hpp"

#include "base/handle.hpp"
#include "base/win_error.hpp"
#include "base/color.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>
#include <mutex>
#include <atomic>
namespace coding {

namespace {

inline constexpr char kFramedStdout = '\x01';
inline constexpr char kFramedStderr = '\x02';

std::mutex g_console_mutex;

void write_styled(bool colorize, const color::Style *style, const char *data,
                  std::size_t n) {
  if (n == 0)
    return;

  std::string buf;
  if (colorize && style)
    buf = (*style)(std::string_view(data, n));
  else
    buf.assign(data, n);

  std::lock_guard<std::mutex> lock(g_console_mutex);
  std::cout.flush();

  HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD written = 0;
  if (h && h != INVALID_HANDLE_VALUE &&
      ::WriteFile(h, buf.data(), static_cast<DWORD>(buf.size()), &written,
                  nullptr)) {
    return;
  }
  std::cout.write(buf.data(), static_cast<std::streamsize>(buf.size()));
  std::cout.flush();
}

struct Sink {
  bool echo{true};
  bool colorize{false};
  const color::Style *out_style{nullptr};
  const color::Style *err_style{nullptr};
  std::function<void(const char *, std::size_t, bool)> on_output;

  void write(const char *data, std::size_t n, bool is_stderr) const {
    if (on_output) {
      on_output(data, n, is_stderr);
      return;
    }
    if (!echo)
      return;
    write_styled(colorize, is_stderr ? err_style : out_style, data, n);
  }
};

void pump_both(HANDLE hOut, HANDLE hErr, const Sink &sink,
               std::string *out_sink, bool merge_err) {
  HANDLE handles[2] = {hOut, hErr};
  bool alive[2] = {true, true};

  std::vector<char> buf(4096);

  while (alive[0] || alive[1]) {
    HANDLE wait_arr[2];
    int map[2];
    DWORD cnt = 0;
    for (int i = 0; i < 2; ++i) {
      if (alive[i]) {
        wait_arr[cnt] = handles[i];
        map[cnt] = i;
        ++cnt;
      }
    }
    if (cnt == 0)
      break;

    DWORD r = WaitForMultipleObjects(cnt, wait_arr, FALSE, INFINITE);
    if (r < WAIT_OBJECT_0 || r >= WAIT_OBJECT_0 + cnt)
      break;

    int i = map[r - WAIT_OBJECT_0];

    DWORD n = 0;
    if (!ReadFile(handles[i], buf.data(), static_cast<DWORD>(buf.size()), &n,
                  nullptr) ||
        n == 0) {
      alive[i] = false;
      continue;
    }

    if (out_sink && (i == 0 || merge_err))
      out_sink->append(buf.data(), n);

    sink.write(buf.data(), n, i == 1);
  }
}

class TagSplitter {
public:
  TagSplitter(const Sink &sink, std::string *out_sink, bool merge_err)
      : sink_(sink), out_sink_(out_sink), merge_err_(merge_err) {}

  void feed(const char *data, std::size_t n) {
    bytes_ += n;
    std::size_t start = 0;
    for (std::size_t i = 0; i < n; ++i) {
      const char c = data[i];
      if (c != kFramedStdout && c != kFramedStderr)
        continue;
      if (i != start)
        emit(data + start, i - start);
      current_ = c;
      saw_tag_ = true;
      start = i + 1;
    }
    if (start != n)
      emit(data + start, n - start);
  }

  bool saw_tag() const noexcept { return saw_tag_; }
  std::size_t bytes() const noexcept { return bytes_; }

private:
  void emit(const char *data, std::size_t n) {
    if (current_ == kFramedStderr) {
      if (merge_err_ && out_sink_)
        out_sink_->append(data, n);
      sink_.write(data, n, true);
      return;
    }

    if (out_sink_)
      out_sink_->append(data, n);
    sink_.write(data, n, false);
  }

  const Sink &sink_;
  std::string *out_sink_;
  bool merge_err_;
  char current_{kFramedStdout};
  bool saw_tag_{false};
  std::size_t bytes_{0};
};

void pump_tagged(HANDLE hFramed, HANDLE hRawErr, TagSplitter &splitter,
                 std::string *raw_err_sink) {
  HANDLE handles[2] = {hFramed, hRawErr};
  bool alive[2] = {true, true};

  std::vector<char> buf(4096);

  while (alive[0] || alive[1]) {
    HANDLE wait_arr[2];
    int map[2];
    DWORD cnt = 0;
    for (int i = 0; i < 2; ++i) {
      if (alive[i]) {
        wait_arr[cnt] = handles[i];
        map[cnt] = i;
        ++cnt;
      }
    }
    if (cnt == 0)
      break;

    DWORD r = WaitForMultipleObjects(cnt, wait_arr, FALSE, INFINITE);
    if (r < WAIT_OBJECT_0 || r >= WAIT_OBJECT_0 + cnt)
      break;

    const int i = map[r - WAIT_OBJECT_0];

    DWORD n = 0;
    if (!ReadFile(handles[i], buf.data(), static_cast<DWORD>(buf.size()), &n,
                  nullptr) ||
        n == 0) {
      alive[i] = false;
      continue;
    }

    if (i == 0)
      splitter.feed(buf.data(), n);
    else if (raw_err_sink)
      raw_err_sink->append(buf.data(), n);
  }
}

struct PipeSet {
  HandleGuard out_read, out_write;
  HandleGuard err_read, err_write;
  HandleGuard in_read, in_write;
  HANDLE child_stdin{INVALID_HANDLE_VALUE};
  HANDLE console_in{INVALID_HANDLE_VALUE};
  bool stdin_is_console{false};
  DWORD saved_console_mode{0};
  bool console_mode_saved{false};
  void close_parent_ends() noexcept {
    out_write.reset();
    err_write.reset();
    in_read.reset();
  }
  ~PipeSet() {
    if (console_mode_saved && console_in != INVALID_HANDLE_VALUE)
      SetConsoleMode(console_in, saved_console_mode);
  }
};

struct ChildProcess {
  HandleGuard process;
  HandleGuard thread;
  std::chrono::steady_clock::time_point started;
};

struct OutputContext {
  color::Style stdout_style{color::Code::Green};
  color::Style stderr_style{color::Code::Red};
  Sink sink;
  std::string captured;
  std::string raw_err;
  std::unique_ptr<TagSplitter> splitter;
  std::thread pump;
  ~OutputContext() {
    if (pump.joinable())
      pump.join();
  }
};

bool prepare_environment(const SingleRunOption &opts, SingleRunResult &out) {
  std::error_code ec;
  if (!std::filesystem::exists(opts.exe_path, ec)) {
    out.result.message = "Executable not found: " + opts.exe_path.string();
    return false;
  }
  if (!opts.work_dir.empty()) {
    std::filesystem::create_directories(opts.work_dir, ec);
    if (ec) {
      out.result.message = "Failed to create work_dir: " + ec.message();
      return false;
    }
  }
  return true;
}

HandleGuard create_job(SingleRunResult &out) {
  HandleGuard hJob(CreateJobObjectW(nullptr, nullptr));
  if (!hJob.valid()) {
    out.result.message = "CreateJobObjectW failed: " + win::last_error_string();
    return hJob;
  }

  JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
  jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(hJob.get(), JobObjectExtendedLimitInformation,
                               &jeli, sizeof(jeli))) {
    out.result.message =
        "SetInformationJobObject failed: " + win::last_error_string();
    hJob.reset();
    return hJob;
  }
  return hJob;
}

bool open_pipes(const SingleRunOption &opts, PipeSet &pipes,
                SingleRunResult &out) {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  auto make_pipe = [&](HandleGuard &read_end, HandleGuard &write_end,
                       const char *what) -> bool {
    HANDLE r = INVALID_HANDLE_VALUE;
    HANDLE w = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&r, &w, &sa, 0)) {
      out.result.message = std::string("CreatePipe(") + what +
                           ") failed: " + win::last_error_string();
      return false;
    }
    read_end.reset(r);
    write_end.reset(w);
    SetHandleInformation(read_end.get(), HANDLE_FLAG_INHERIT, 0);
    return true;
  };

  HandleGuard &hOutRead = pipes.out_read;
  HandleGuard &hOutWrite = pipes.out_write;
  HandleGuard &hErrRead = pipes.err_read;
  HandleGuard &hErrWrite = pipes.err_write;
  HandleGuard &hStdinRead = pipes.in_read;
  HandleGuard &hStdinWrite = pipes.in_write;
  HANDLE &hChildStdin = pipes.child_stdin;
  HANDLE &hConsoleIn = pipes.console_in;
  bool &stdin_is_console = pipes.stdin_is_console;
  DWORD &saved_console_mode = pipes.saved_console_mode;
  bool &console_mode_saved = pipes.console_mode_saved;

  if (!make_pipe(hOutRead, hOutWrite, "stdout"))
    return false;
  if (!make_pipe(hErrRead, hErrWrite, "stderr"))
    return false;

  if (opts.stdin_from_console) {
    hConsoleIn = GetStdHandle(STD_INPUT_HANDLE);
    if (hConsoleIn == nullptr || hConsoleIn == INVALID_HANDLE_VALUE) {
      out.result.message = "No console stdin available";
      return false;
    }
    DWORD mode = 0;
    stdin_is_console = GetConsoleMode(hConsoleIn, &mode) != FALSE;

    if (stdin_is_console) {
      saved_console_mode = mode;
      console_mode_saved =
          SetConsoleMode(hConsoleIn, mode | ENABLE_ECHO_INPUT) != FALSE;

      HandleGuard r, w;
      if (!make_pipe(r, w, "stdin"))
        return false;
      hStdinRead = std::move(r);
      hStdinWrite = std::move(w);
      SetHandleInformation(hStdinRead.get(), HANDLE_FLAG_INHERIT,
                           HANDLE_FLAG_INHERIT);
      SetHandleInformation(hStdinWrite.get(), HANDLE_FLAG_INHERIT, 0);
      hChildStdin = hStdinRead.get();
    } else {
      hChildStdin = hConsoleIn;
      SetHandleInformation(hChildStdin, HANDLE_FLAG_INHERIT,
                           HANDLE_FLAG_INHERIT);
    }
  } else {
    HandleGuard r, w;
    if (!make_pipe(r, w, "stdin"))
      return false;
    hStdinRead = std::move(r);
    hStdinWrite = std::move(w);
    SetHandleInformation(hStdinRead.get(), HANDLE_FLAG_INHERIT,
                         HANDLE_FLAG_INHERIT);
    SetHandleInformation(hStdinWrite.get(), HANDLE_FLAG_INHERIT, 0);
    hChildStdin = hStdinRead.get();
  }
  return true;
}

bool spawn_child(const SingleRunOption &opts, PipeSet &pipes, HANDLE job,
                 ChildProcess &child, SingleRunResult &out) {
  std::wstring cmd = L"\"" + opts.exe_path.wstring() + L"\"";
  std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
  cmdBuf.push_back(L'\0');

  std::wstring workdir;
  const wchar_t *cwd = nullptr;
  if (!opts.work_dir.empty()) {
    workdir = opts.work_dir.wstring();
    cwd = workdir.c_str();
  }

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = pipes.child_stdin;
  si.hStdOutput = pipes.out_write.get();
  si.hStdError = pipes.err_write.get();

  PROCESS_INFORMATION pi{};
  DWORD create_flags = CREATE_SUSPENDED | CREATE_NO_WINDOW;

  if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                      create_flags, nullptr, cwd, &si, &pi)) {
    out.result.message = "CreateProcessW failed: " + win::last_error_string();
    return false;
  }
  child.process.reset(pi.hProcess);
  child.thread.reset(pi.hThread);

  if (!AssignProcessToJobObject(job, pi.hProcess)) {
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, INFINITE);
    out.result.message =
        "AssignProcessToJobObject failed: " + win::last_error_string();
    return false;
  }

  pipes.close_parent_ends();
  child.started = std::chrono::steady_clock::now();
  ResumeThread(pi.hThread);

  return true;
}

void start_output_pump(const SingleRunOption &opts, const PipeSet &pipes,
                       OutputContext &ctx) {
  ctx.sink.echo = opts.echo;
  ctx.sink.colorize = opts.colorize_output;
  ctx.sink.out_style = &ctx.stdout_style;
  ctx.sink.err_style = &ctx.stderr_style;
  ctx.sink.on_output = opts.on_output;

  if (opts.tagged_stream) {
    ctx.splitter = std::make_unique<TagSplitter>(ctx.sink, &ctx.captured,
                                                 opts.merge_stderr);
    ctx.pump =
        std::thread(pump_tagged, pipes.out_read.get(), pipes.err_read.get(),
                    std::ref(*ctx.splitter), &ctx.raw_err);
  } else {
    ctx.pump =
        std::thread(pump_both, pipes.out_read.get(), pipes.err_read.get(),
                    std::cref(ctx.sink), &ctx.captured, opts.merge_stderr);
  }
}

std::thread start_stdin_pump(const SingleRunOption &opts, PipeSet &pipes,
                             std::atomic<bool> &stop) {
  if (opts.stdin_source && pipes.in_write.valid()) {
    return std::thread([&]() {
      std::vector<char> buf(4096);
      while (!stop.load(std::memory_order_relaxed)) {
        const std::size_t want = opts.stdin_source(buf.data(), buf.size());
        if (want == 0)
          break;
        std::size_t done = 0;
        while (done < want) {
          DWORD wrote = 0;
          if (!WriteFile(pipes.in_write.get(), buf.data() + done,
                         static_cast<DWORD>(want - done), &wrote, nullptr) ||
              wrote == 0)
            break;
          done += wrote;
        }
        if (done < want)
          break;
      }
      pipes.in_write.reset();
    });
  }

  if (opts.stdin_from_console && pipes.stdin_is_console &&
      pipes.in_write.valid()) {
    return std::thread([&]() {
      std::vector<char> fbuf(4096);
      std::vector<char> batch;
      constexpr DWORD kIdleMs = 15;
      constexpr std::size_t kMaxBatch = 256 * 1024;
      bool closed = false;
      while (!closed && !stop.load(std::memory_order_relaxed)) {
        const DWORD r = WaitForSingleObject(pipes.console_in, 50);
        if (r == WAIT_TIMEOUT)
          continue;
        if (r != WAIT_OBJECT_0)
          break;
        batch.clear();
        for (;;) {
          DWORD n = 0;
          if (!ReadFile(pipes.console_in, fbuf.data(),
                        static_cast<DWORD>(fbuf.size()), &n, nullptr) ||
              n == 0) {
            closed = true;
            break;
          }
          batch.insert(batch.end(), fbuf.begin(), fbuf.begin() + n);
          if (batch.size() >= kMaxBatch)
            break;
          if (WaitForSingleObject(pipes.console_in, kIdleMs) != WAIT_OBJECT_0)
            break;
        }
        if (batch.empty())
          continue;
        DWORD written = 0;
        if (!WriteFile(pipes.in_write.get(), batch.data(),
                       static_cast<DWORD>(batch.size()), &written, nullptr) ||
            written == 0)
          break;
      }
      pipes.in_write.reset();
    });
  }

  if (pipes.in_write.valid()) {
    std::size_t left = opts.input_text.size();
    const char *p = opts.input_text.data();
    while (left > 0) {
      DWORD written = 0;
      const DWORD chunk =
          static_cast<DWORD>(std::min<std::size_t>(left, 64 * 1024));
      if (!WriteFile(pipes.in_write.get(), p, chunk, &written, nullptr) ||
          written == 0)
        break;
      p += written;
      left -= written;
    }
    pipes.in_write.reset();
  }
  return {};
}

bool wait_for_child(const SingleRunOption &opts, HANDLE process, HANDLE job) {
  if (opts.time_limit_ms.count() == 0) {
    WaitForSingleObject(process, INFINITE);
    return false;
  }
  const DWORD check_interval = 200;
  while (true) {
    DWORD ret = WaitForSingleObject(process, check_interval);
    if (ret == WAIT_OBJECT_0)
      return false;
    FILETIME creation, exit, kernel, user;
    if (GetProcessTimes(process, &creation, &exit, &kernel, &user)) {
      ULARGE_INTEGER k, u;
      k.LowPart = kernel.dwLowDateTime;
      k.HighPart = kernel.dwHighDateTime;
      u.LowPart = user.dwLowDateTime;
      u.HighPart = user.dwHighDateTime;
      unsigned long long cpu_ms = (k.QuadPart + u.QuadPart) / 10000ULL;
      if (cpu_ms >
          static_cast<unsigned long long>(opts.time_limit_ms.count())) {
        TerminateJobObject(job, 1);
        WaitForSingleObject(process, 5000);
        return true;
      }
    }
  }
  return false;
}

constexpr std::size_t kMaxCrashText = 8192;

std::string tail_of_stderr(const std::string &text, std::size_t limit) {
  std::size_t begin = text.size() > limit ? text.size() - limit : 0;
  if (begin != 0) {
    const std::size_t nl = text.find('\n', begin);
    begin = nl == std::string::npos ? text.size() : nl + 1;
  }

  std::string out;
  out.reserve(text.size() - begin);
  for (std::size_t i = begin; i < text.size(); ++i) {
    if (text[i] != '\r')
      out.push_back(text[i]);
  }
  while (!out.empty() && out.back() == '\n')
    out.pop_back();
  return out;
}

void finalize_run(const SingleRunOption &opts, PipeSet &pipes,
                  OutputContext &ctx, std::thread &stdin_pump,
                  std::atomic<bool> &stdin_stop, HANDLE process, HANDLE job,
                  std::chrono::steady_clock::time_point started, bool time_out,
                  SingleRunResult &out) {
  stdin_stop.store(true, std::memory_order_relaxed);
  if (stdin_pump.joinable())
    stdin_pump.join();
  if (ctx.pump.joinable())
    ctx.pump.join();

  pipes.out_read.reset();
  pipes.err_read.reset();

  if (opts.tagged_stream && !ctx.raw_err.empty())
    out.crash_text = tail_of_stderr(ctx.raw_err, kMaxCrashText);

  if (opts.tagged_stream && ctx.splitter && ctx.splitter->bytes() > 0 &&
      !ctx.splitter->saw_tag())
    out.warning =
        "the program produced unframed output; it was probably built without "
        "the injected header (rebuild with --force)";

  fill_accounting(out.result, process, job, started, opts.memory_limit_bytes,
                  time_out);
}

} // namespace

SingleRunResult run_single(const SingleRunOption &opts) {
  SingleRunResult out;
  out.result.status = RunnerStatus::SystemError;

  if (!prepare_environment(opts, out))
    return out;

  HandleGuard hJob = create_job(out);
  if (!hJob)
    return out;

  PipeSet pipes;
  if (!open_pipes(opts, pipes, out))
    return out;

  ChildProcess child;
  if (!spawn_child(opts, pipes, hJob.get(), child, out))
    return out;

  if (opts.on_started)
    opts.on_started(child.process.get(), hJob.get());

  OutputContext ctx;
  start_output_pump(opts, pipes, ctx);

  std::atomic<bool> stdin_stop{false};
  std::thread stdin_pump = start_stdin_pump(opts, pipes, stdin_stop);

  const bool time_out = wait_for_child(opts, child.process.get(), hJob.get());

  finalize_run(opts, pipes, ctx, stdin_pump, stdin_stop, child.process.get(),
               hJob.get(), child.started, time_out, out);

  out.captured = std::move(ctx.captured);
  return out;
}

} // namespace coding
