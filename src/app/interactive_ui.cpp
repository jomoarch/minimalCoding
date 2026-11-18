#include "app/interactive_ui.hpp"

#include "app/clipboard.hpp"
#include "app/program_hook.hpp"
#include "base/color.hpp"
#include "base/text.hpp"
#include "linedit/bar.hpp"
#include "linedit/editor.hpp"
#include "linedit/session.hpp"
#include "linedit/status_line.hpp"
#include "monitor/monitor.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace coding {
namespace interactive {

namespace {

constexpr std::size_t kMaxPending = 64u * 1024u * 1024u;
constexpr std::size_t kPoolCap = 64u * 1024u * 1024u;
constexpr std::size_t kWriteSlice = 256u * 1024u;
constexpr auto kDestroyWindow = std::chrono::seconds(1);
constexpr int kMaxKeysPerPass = 4096;

struct OutputChunk {
  std::string text;
  std::size_t raw_bytes{0};
  std::size_t newlines{0};
  char last_raw{0};
};

struct Shared {
  std::mutex m;
  std::condition_variable cv;

  std::deque<OutputChunk> pending;
  std::size_t pending_raw{0};
  std::size_t dropped{0};

  std::string input;
  std::size_t input_at{0};
  bool eof{false};

  std::string recorded_input;

  std::atomic<bool> stop{false};
  std::atomic<bool> done{false};

  std::atomic<void *> process{nullptr};
  std::atomic<void *> job{nullptr};
  std::atomic<bool> started{false};
  std::chrono::steady_clock::time_point start{};
};

std::size_t count_newlines(std::string_view text) {
  std::size_t count = 0;
  for (const char c : text) {
    if (c == '\n')
      ++count;
  }
  return count;
}

std::string format_size(std::uint64_t bytes) {
  static const char *units[] = {"  B", " KB", " MB", " GB", " TB"};
  double value = static_cast<double>(bytes);
  int unit = 0;
  while (value >= 1023.995 && unit < 4) {
    value /= 1024.0;
    ++unit;
  }

  char buffer[48];
  if (unit == 0)
    std::snprintf(buffer, sizeof(buffer), "%llu%s",
                  static_cast<unsigned long long>(bytes), units[0]);
  else
    std::snprintf(buffer, sizeof(buffer), "%.1f%s", value, units[unit]);
  return buffer;
}

std::string fit_width(std::string text, std::size_t width) {
  if (width == 0)
    return {};
  if (coding::text::display_width(text) <= width)
    return text;

  std::string cut;
  std::size_t i = 0;
  while (i < text.size()) {
    const coding::text::CharWidth w = coding::text::measure(text, i);
    if (w.bytes == 0 || coding::text::display_width(cut) + w.columns > width)
      break;
    cut.append(text, i, w.bytes);
    i += w.bytes;
  }
  return cut;
}

void queue_input(Shared &shared, std::string_view text) {
  if (text.empty())
    return;
  {
    std::lock_guard<std::mutex> lock(shared.m);
    shared.input.append(text);
  }
  shared.cv.notify_all();
}

void submit_line(linedit::Editor &editor, Shared &shared, linedit::Bar &bar) {
  const std::string text = editor.buffer + "\n";
  queue_input(shared, text);
  {
    std::lock_guard<std::mutex> lock(shared.m);
    shared.recorded_input += text;
  }
  bar.commit_text(text);
  editor.clear();
  bar.set_input(editor.text(), editor.cursor);
}

void submit_paste(std::string text, linedit::Editor &editor, Shared &shared,
                  linedit::Bar &bar) {
  (void)shared;
  if (text.empty())
    return;
  editor.insert_text(linedit::clean_paste(text));
  bar.set_input(editor.text(), editor.cursor);
}

} // namespace

bool run(const AppConfig &cfg, const SingleRunOption &base,
         SingleRunResult &out) {
  linedit::Session session;
  std::string error;
  if (!linedit::Session::open(session, error, linedit::Screen::Normal))
    return false;

  linedit::Bar bar;
  if (!bar.open(session, error))
    return false;

  const unsigned tick = cfg.status_interval_ms == 0
                            ? 100u
                            : static_cast<unsigned>(cfg.status_interval_ms);

  Shared shared;
  SingleRunOption opt = base;
  opt.stdin_from_console = false;

  opt.on_started = [&shared](void *process, void *job) {
    app::report_program_started(process);
    shared.process.store(process, std::memory_order_relaxed);
    shared.job.store(job, std::memory_order_relaxed);
    shared.start = std::chrono::steady_clock::now();
    shared.started.store(true, std::memory_order_release);
  };

  opt.on_output = [&shared, &base](const char *data, std::size_t n,
                                   bool is_stderr) {
    OutputChunk chunk;
    if (base.colorize_output) {
      const color::Style style(
          {is_stderr ? color::Code::Red : color::Code::Green});
      chunk.text = style(std::string_view(data, n));
    } else {
      chunk.text.assign(data, n);
    }
    chunk.raw_bytes = n;
    chunk.newlines = count_newlines(std::string_view(data, n));
    chunk.last_raw = n != 0 ? data[n - 1] : 0;

    std::lock_guard<std::mutex> lock(shared.m);
    if (shared.pending_raw + n > kMaxPending) {
      shared.dropped += n;
      return;
    }
    shared.pending_raw += n;
    shared.pending.push_back(std::move(chunk));
  };

  opt.stdin_source = [&shared](char *buf, std::size_t n) -> std::size_t {
    std::unique_lock<std::mutex> lock(shared.m);
    for (;;) {
      if (shared.input_at < shared.input.size()) {
        const std::size_t take =
            std::min(n, shared.input.size() - shared.input_at);
        std::memcpy(buf, shared.input.data() + shared.input_at, take);
        shared.input_at += take;
        if (shared.input_at == shared.input.size()) {
          shared.input.clear();
          shared.input_at = 0;
        }
        return take;
      }
      if (shared.eof || shared.stop.load(std::memory_order_relaxed))
        return 0;
      shared.cv.wait(lock, [&shared] {
        return !shared.input.empty() || shared.eof ||
               shared.stop.load(std::memory_order_relaxed);
      });
    }
  };

  opt.stdin_cancel = [&shared] {
    {
      std::lock_guard<std::mutex> lock(shared.m);
      shared.eof = true;
    }
    shared.cv.notify_all();
  };

  SingleRunResult result;
  std::thread worker([&]() {
    result = run_single(opt);
    shared.done.store(true, std::memory_order_relaxed);
  });

  linedit::Editor editor;
  linedit::status::Line spinner;
  monitor::Watch watch;
  bool interrupted = false;
  bool eof_sent = false;

  const auto child_exited = [&shared]() -> bool {
    if (!shared.started.load(std::memory_order_acquire))
      return false;
    HANDLE process =
        static_cast<HANDLE>(shared.process.load(std::memory_order_relaxed));
    return process != nullptr &&
           ::WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
  };

  std::deque<OutputChunk> display;
  std::deque<OutputChunk> pool;
  std::size_t pool_raw = 0;
  std::size_t pool_newlines = 0;
  char pool_last_raw = 0;
  std::size_t dropped_bytes = 0;
  bool holding = false;
  bool releasing = false;
  bool destroy_armed = false;
  std::chrono::steady_clock::time_point destroy_until{};

  const auto pool_lines = [&pool, &pool_newlines, &pool_last_raw]() {
    if (pool.empty())
      return std::size_t{0};
    return pool_newlines + (pool_last_raw != '\n' ? 1 : 0);
  };

  const auto append_pool = [&](OutputChunk &&chunk) {
    if (pool_raw + chunk.raw_bytes > kPoolCap) {
      dropped_bytes += chunk.raw_bytes;
      return;
    }
    pool_raw += chunk.raw_bytes;
    pool_newlines += chunk.newlines;
    pool_last_raw = chunk.last_raw;
    pool.push_back(std::move(chunk));
  };

  const auto take_pool_slice = [&](std::string &text) {
    std::size_t raw = 0;
    while (!pool.empty() && raw < kWriteSlice) {
      text += pool.front().text;
      raw += pool.front().raw_bytes;
      pool_newlines -= pool.front().newlines;
      pool_raw -= pool.front().raw_bytes;
      pool.pop_front();
    }
    pool_last_raw = pool.empty() ? 0 : pool.back().last_raw;
  };

  const auto clear_pool = [&]() {
    pool.clear();
    pool_raw = 0;
    pool_newlines = 0;
    pool_last_raw = 0;
  };

  const auto destroy_pool = [&]() {
    clear_pool();
    dropped_bytes = 0;
    holding = false;
    releasing = false;
    destroy_armed = false;
    bar.set_compact(false);
  };

  const auto compose_status = [&](std::chrono::milliseconds cpu,
                                  std::size_t width) {
    std::string text = "[" + spinner.frame_text() + "] ";
    if (holding || releasing || !pool.empty())
      text += "out " + std::to_string(pool_lines()) + " (" +
              format_size(pool_raw) + ") ";
    if (dropped_bytes != 0)
      text += "drop " + format_size(dropped_bytes) + " ";
    if (destroy_armed)
      text += "destroy? F9 again ";
    text += "cpu " + std::to_string(cpu.count()) + " ms";
    return fit_width(std::move(text), width);
  };

  auto last_status = std::chrono::steady_clock::time_point{};

  while (!shared.done.load(std::memory_order_relaxed)) {
    if (child_exited())
      break;

    const bool output_work = releasing || !display.empty();
    const unsigned wait = output_work ? 0u : tick;
    linedit::Key key = linedit::Key::Text;
    char32_t ch = 0;
    bool got = session.read_any(key, ch, wait);
    const auto now = std::chrono::steady_clock::now();
    if (destroy_armed && now >= destroy_until)
      destroy_armed = false;

    for (int i = 0; got && i < kMaxKeysPerPass; ++i) {
      if (key != linedit::Key::Resize && key != linedit::Key::Paste &&
          ch != 0x03 && ch != 0x04 && !bar.input_visible()) {
        bar.reveal_input();
        if (bar.input_visible()) {
          bar.paint();
          got = session.read_any(key, ch, 0);
          continue;
        }
      }

      if (key == linedit::Key::Resize) {
        bar.resize();
      } else if (key == linedit::Key::Paste) {
        destroy_armed = false;
        submit_paste(clipboard::text(), editor, shared, bar);
      } else if (key == linedit::Key::Hold) {
        destroy_armed = false;
        if (releasing) {
          releasing = false;
          holding = true;
          bar.set_compact(false);
        } else if (holding) {
          holding = false;
          if (!pool.empty()) {
            releasing = true;
            bar.set_compact(true);
          }
        } else {
          holding = true;
        }
      } else if (key == linedit::Key::Destroy) {
        if (destroy_armed) {
          destroy_pool();
        } else if (holding || releasing || !pool.empty()) {
          destroy_armed = true;
          destroy_until = now + kDestroyWindow;
          last_status = std::chrono::steady_clock::time_point{};
        }
      } else {
        if (destroy_armed)
          destroy_armed = false;
        const linedit::Outcome action = editor.feed(key, ch);
        if (action == linedit::Outcome::Submitted) {
          if (child_exited()) {
            editor.clear();
            bar.set_input(editor.text(), editor.cursor);
          } else {
            if (holding || releasing) {
              bar.set_compact(false);
              std::string slice;
              while (!pool.empty()) {
                slice.clear();
                take_pool_slice(slice);
                if (!slice.empty())
                  bar.write_output(slice);
              }
              dropped_bytes = 0;
              releasing = false;
            }
            submit_line(editor, shared, bar);
          }
        } else if (action == linedit::Outcome::Interrupt && !interrupted) {
          interrupted = true;
          {
            std::lock_guard<std::mutex> lock(shared.m);
            shared.eof = true;
          }
          shared.cv.notify_all();
          shared.stop.store(true, std::memory_order_relaxed);
          void *job = shared.job.load(std::memory_order_relaxed);
          if (job != nullptr)
            TerminateJobObject(static_cast<HANDLE>(job), 1);
        } else if (action == linedit::Outcome::EndOfInput && !eof_sent) {
          eof_sent = true;
          {
            std::lock_guard<std::mutex> lock(shared.m);
            shared.eof = true;
          }
          shared.cv.notify_all();
        }
      }
      got = session.read_any(key, ch, 0);
    }

    std::deque<OutputChunk> incoming;
    {
      std::lock_guard<std::mutex> lock(shared.m);
      incoming.swap(shared.pending);
      shared.pending_raw = 0;
    }

    if (holding || releasing) {
      for (auto &chunk : incoming)
        append_pool(std::move(chunk));
    } else {
      for (auto &chunk : incoming)
        display.push_back(std::move(chunk));
    }

    if (releasing) {
      if (pool.empty()) {
        releasing = false;
        dropped_bytes = 0;
        bar.set_compact(false);
      } else {
        std::string slice;
        take_pool_slice(slice);
        if (!slice.empty())
          bar.write_output(slice);
        if (pool.empty()) {
          releasing = false;
          dropped_bytes = 0;
          bar.set_compact(false);
        }
      }
    } else if (!display.empty()) {
      std::string slice;
      std::size_t raw = 0;
      while (!display.empty() && raw < kWriteSlice) {
        slice += display.front().text;
        raw += display.front().raw_bytes;
        display.pop_front();
      }
      if (!slice.empty())
        bar.write_output(slice);
    }

    if (now - last_status >= std::chrono::milliseconds(tick)) {
      last_status = now;
      if (shared.started.load(std::memory_order_acquire)) {
        const monitor::Sample sample = monitor::sample(
            shared.process.load(std::memory_order_relaxed),
            shared.job.load(std::memory_order_relaxed), shared.start);
        spinner.advance(watch.cpu_advanced(sample));
        watch.update(sample);
        const linedit::Size size = session.size();
        bar.set_status(compose_status(sample.cpu_ms(),
                                      size.columns > 1 ? size.columns - 1 : 1));
      }
    }

    bar.set_input(editor.text(), editor.cursor);
    bar.paint();
  }

  worker.join();

  std::deque<OutputChunk> tail;
  {
    std::lock_guard<std::mutex> lock(shared.m);
    tail.swap(shared.pending);
    shared.pending_raw = 0;
  }

  if (holding || releasing) {
    bar.set_compact(false);
    std::string slice;
    while (!pool.empty()) {
      slice.clear();
      take_pool_slice(slice);
      if (!slice.empty())
        bar.write_output(slice);
    }
    holding = false;
    releasing = false;
  }

  for (auto &chunk : display)
    bar.write_output(chunk.text);
  display.clear();
  for (auto &chunk : tail)
    bar.write_output(chunk.text);
  tail.clear();

  bar.close();

  if (shared.dropped != 0)
    std::cerr << "[run] " << shared.dropped
              << " bytes of output were dropped: the console could not keep "
                 "up\n";
  if (interrupted)
    std::cerr << "[run] interrupted\n";

  out = std::move(result);
  {
    std::lock_guard<std::mutex> lock(shared.m);
    out.user_input = std::move(shared.recorded_input);
  }
  return true;
}

} // namespace interactive
} // namespace coding
