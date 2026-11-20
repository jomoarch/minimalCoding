#include "app/batch.hpp"

#include "app/program_hook.hpp"
#include "base/color.hpp"
#include "base/text.hpp"
#include "process/runner_single.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string_view>
#include <thread>

namespace coding {
namespace app {

namespace {

bool is_digit(char c) { return c >= '0' && c <= '9'; }

struct NameKey {
  std::string class_key;
  std::vector<std::string> digits;
};

NameKey make_name_key(const std::string &stem) {
  NameKey key;
  for (std::size_t i = 0; i < stem.size();) {
    if (is_digit(stem[i])) {
      std::size_t j = i;
      while (j < stem.size() && is_digit(stem[j]))
        ++j;
      std::size_t p = i;
      while (p + 1 < j && stem[p] == '0')
        ++p;
      key.digits.push_back(stem.substr(p, j - p));
      key.class_key += '\x01';
      i = j;
    } else {
      std::size_t j = i;
      while (j < stem.size() && !is_digit(stem[j]))
        ++j;
      key.class_key.append(stem, i, j - i);
      i = j;
    }
  }
  return key;
}

bool compare_name_key(const NameKey &a, const NameKey &b) {
  if (a.class_key != b.class_key)
    return a.class_key < b.class_key;
  for (std::size_t i = 0; i < a.digits.size(); ++i) {
    const std::string &x = a.digits[i];
    const std::string &y = b.digits[i];
    if (x.size() != y.size())
      return x.size() < y.size();
    if (x != y)
      return x < y;
  }
  return false;
}

bool identical(const NameKey &a, const NameKey &b) {
  return a.class_key == b.class_key && a.digits == b.digits;
}

std::string read_file(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string normalize(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
      continue;
    out.push_back(text[i]);
  }

  std::string trimmed;
  trimmed.reserve(out.size());
  std::size_t begin = 0;
  while (begin <= out.size()) {
    const std::size_t nl = out.find('\n', begin);
    const std::size_t end = nl == std::string::npos ? out.size() : nl;
    std::size_t e = end;
    while (e > begin && (out[e - 1] == ' ' || out[e - 1] == '\t'))
      --e;
    trimmed.append(out, begin, e - begin);
    if (nl == std::string::npos)
      break;
    trimmed.push_back('\n');
    begin = nl + 1;
  }
  while (!trimmed.empty() && trimmed.back() == '\n')
    trimmed.pop_back();
  return trimmed;
}

bool same_output(std::string_view actual, std::string_view expected) {
  return normalize(actual) == normalize(expected);
}

std::string format_memory(std::size_t bytes) {
  if (bytes == 0)
    return "0.00  B";

  static const char *units[] = {"  B", " KB", " MB", " GB"};
  double v = static_cast<double>(bytes);
  int i = 0;
  while (v >= 1023.995 && i < 3) {
    v /= 1024.0;
    ++i;
  }

  std::ostringstream oss;
  oss << std::fixed << std::setprecision(2) << v << units[i];
  return oss.str();
}

std::string truncate_to(std::string_view text_in, std::size_t width) {
  std::string out;
  std::size_t used = 0;
  for (std::size_t i = 0; i < text_in.size();) {
    const text::Codepoint cp = text::decode_utf8(text_in, i);
    const text::CharWidth cw = text::measure(text_in, i);
    if (used + cw.columns > width)
      break;
    out.append(text_in.substr(i, cp.length));
    used += cw.columns;
    i += cp.length;
  }
  return out;
}

std::string paint_verdict(std::ostream &os, std::string_view text_in,
                          BatchVerdict verdict) {
  switch (verdict) {
  case BatchVerdict::Accepted:
    return color::paint(os, text_in, {color::Code::BrightGreen});
  case BatchVerdict::WrongAnswer:
    return color::paint(os, text_in, {color::Code::Red});
  case BatchVerdict::RuntimeError:
    return color::paint(os, text_in, {color::Code::BrightMagenta});
  default:
    return std::string(text_in);
  }
}

class BatchDisplay {
public:
  BatchDisplay(const std::vector<BatchCase> &cases,
               std::chrono::milliseconds limit)
      : limit_(limit) {
    rows_.resize(cases.size());
    for (std::size_t i = 0; i < cases.size(); ++i)
      rows_[i].name = cases[i].name;
    if (limit_.count() > 0) {
      digits_ = std::to_string(limit_.count()).size();
      used_w_ = digits_ + 1;
      time_w_ = used_w_ + 3 + digits_ + 3;
    }
    for (const Row &row : rows_)
      name_max_ = std::max(name_max_, text::display_width(row.name));
    name_w_ = name_max_;
    probe_console();
    compute_widths();
  }

  ~BatchDisplay() { stop(); }

  void start() {
    if (started_)
      return;
    started_ = true;
    if (!live_)
      return;

    std::lock_guard<std::mutex> lk(mu_);
    std::cout << "\x1b[?25l" << std::flush;
    render_locked();
    render_ = std::thread([this]() {
      while (!stop_.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (stop_.load(std::memory_order_relaxed))
          break;
        std::lock_guard<std::mutex> lk(mu_);
        ++phase_;
        refresh_geometry();
        render_locked();
      }
    });
  }

  void mark_started(std::size_t i) {
    if (i >= rows_.size())
      return;
    std::lock_guard<std::mutex> lk(mu_);
    rows_[i].started = true;
  }

  void tick(std::size_t i, std::chrono::milliseconds cpu) {
    if (i >= rows_.size())
      return;
    std::lock_guard<std::mutex> lk(mu_);
    rows_[i].started = true;
    rows_[i].cpu = cpu;
  }

  void finish(std::size_t i, std::chrono::milliseconds cpu, std::size_t mem,
              BatchVerdict verdict) {
    if (i >= rows_.size())
      return;
    std::lock_guard<std::mutex> lk(mu_);
    rows_[i].started = true;
    rows_[i].done = true;
    rows_[i].cpu = cpu;
    rows_[i].mem = mem;
    rows_[i].verdict = verdict;
  }

  void stop() {
    if (stop_.exchange(true))
      return;
    if (render_.joinable())
      render_.join();

    std::lock_guard<std::mutex> lk(mu_);
    if (!started_)
      return;
    if (live_) {
      refresh_geometry();
      render_locked();
      std::cout << "\x1b[?25h" << std::flush;
    } else {
      print_static_locked();
    }
  }

private:
  struct Row {
    std::string name;
    std::chrono::milliseconds cpu{0};
    std::size_t mem{0};
    BatchVerdict verdict{BatchVerdict::NoAnswer};
    bool started{false};
    bool done{false};
  };

  void probe_console() {
    out_ = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out_ == nullptr || out_ == INVALID_HANDLE_VALUE)
      return;

    DWORD mode = 0;
    if (!GetConsoleMode(out_, &mode))
      return;
    if (!(mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) &&
        !SetConsoleMode(out_, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
      return;

    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(out_, &info))
      return;
    const int w = info.srWindow.Right - info.srWindow.Left + 1;
    const int h = info.srWindow.Bottom - info.srWindow.Top + 1;
    if (w > 0)
      term_w_ = static_cast<std::size_t>(w);
    if (h > 0)
      term_h_ = static_cast<std::size_t>(h);
    live_ = rows_.size() + 1 <= term_h_;
  }

  void refresh_geometry() {
    if (out_ == nullptr || out_ == INVALID_HANDLE_VALUE)
      return;
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(out_, &info))
      return;
    const int w = info.srWindow.Right - info.srWindow.Left + 1;
    const int h = info.srWindow.Bottom - info.srWindow.Top + 1;
    if (w > 0)
      term_w_ = static_cast<std::size_t>(w);
    if (h > 0)
      term_h_ = static_cast<std::size_t>(h);
    compute_widths();
  }

  void compute_widths() {
    const std::size_t time_part = time_w_ > 0 ? time_w_ + 1 : 0;
    const std::size_t overhead = 7 + time_part + mem_w_ + res_w_;
    std::size_t name = name_max_;
    auto room = [&](std::size_t n) {
      const std::size_t fixed = n + overhead;
      return term_w_ > fixed ? term_w_ - fixed : 0;
    };
    std::size_t avail = room(name);
    while (avail < 4 && name > 1) {
      --name;
      avail = room(name);
    }
    name_w_ = name;
    bar_w_ = std::max<std::size_t>(4, avail > 0 ? avail - 1 : 0);
  }

  std::string bar_for(const Row &row) const {
    std::string bar(bar_w_, ' ');
    if (!row.started)
      return bar;

    if (limit_.count() > 0) {
      const auto limit = static_cast<unsigned long long>(limit_.count());
      auto cpu = static_cast<unsigned long long>(
          std::max<long long>(0, row.cpu.count()));
      unsigned long long filled = bar_w_;
      if (cpu < limit)
        filled = (cpu * bar_w_ + limit / 2) / limit;
      if (filled > bar_w_)
        filled = bar_w_;
      for (std::size_t i = 0; i < filled; ++i)
        bar[i] = '#';
      return bar;
    }

    if (row.done)
      return bar;

    const std::size_t block = std::max<std::size_t>(1, bar_w_ / 2);
    const std::size_t span = bar_w_ > block ? bar_w_ - block : 0;
    std::size_t pos = 0;
    if (span > 0) {
      const std::size_t t = static_cast<std::size_t>(phase_ % (2 * span));
      pos = t <= span ? t : 2 * span - t;
    }
    for (std::size_t k = 0; k < block && pos + k < bar_w_; ++k)
      bar[pos + k] = '#';
    return bar;
  }

  std::string line_for(const Row &row) const {
    std::string line;
    std::string name_field =
        text::pad_right(truncate_to(row.name, name_w_), name_w_);
    if (row.done)
      name_field = paint_verdict(std::cout, name_field, row.verdict);
    line += name_field;
    line += " [";
    line += bar_for(row);
    line += ']';

    if (time_w_ > 0) {
      line += ' ';
      if (row.started) {
        line += text::pad_left(std::to_string(row.cpu.count()), used_w_);
        line += " / ";
        line += text::pad_left(std::to_string(limit_.count()), digits_);
        line += " ms";
      } else {
        line += std::string(time_w_, ' ');
      }
    }

    line += "  ";
    line += row.done ? text::pad_left(format_memory(row.mem), mem_w_)
                     : std::string(mem_w_, ' ');
    line += "  ";

    if (row.done) {
      line += paint_verdict(std::cout,
                            text::pad_left(verdict_name(row.verdict), res_w_),
                            row.verdict);
    } else {
      line += std::string(res_w_, ' ');
    }
    return line;
  }

  void render_locked() {
    std::string frame;
    frame.reserve(rows_.size() * (term_w_ + 16));
    if (rows_printed_ > 0)
      frame += "\x1b[" + std::to_string(rows_printed_) + "A";
    for (const Row &row : rows_) {
      frame += '\r';
      frame += line_for(row);
      frame += "\x1b[K\n";
    }
    std::cout.write(frame.data(), static_cast<std::streamsize>(frame.size()));
    std::cout.flush();
    rows_printed_ = rows_.size();
  }

  void print_static_locked() {
    for (const Row &row : rows_) {
      const std::string line = line_for(row);
      std::cout.write(line.data(), static_cast<std::streamsize>(line.size()));
      std::cout << '\n';
    }
    std::cout.flush();
  }

  std::vector<Row> rows_;
  std::chrono::milliseconds limit_{0};
  std::size_t digits_{0};
  std::size_t used_w_{0};
  std::size_t time_w_{0};
  std::size_t name_max_{0};
  std::size_t name_w_{0};
  std::size_t bar_w_{0};
  const std::size_t mem_w_{9};
  const std::size_t res_w_{3};
  std::size_t term_w_{80};
  std::size_t term_h_{25};
  HANDLE out_{nullptr};
  bool live_{false};
  bool started_{false};
  std::mutex mu_;
  std::thread render_;
  std::atomic<bool> stop_{false};
  unsigned long long phase_{0};
  std::size_t rows_printed_{0};
};

} // namespace

bool list_cases(const std::filesystem::path &dir, std::vector<BatchCase> &cases,
                std::string &error) {
  cases.clear();

  std::error_code ec;
  if (!std::filesystem::exists(dir, ec) ||
      !std::filesystem::is_directory(dir, ec)) {
    error = "test case directory not found: " + dir.string();
    return false;
  }

  struct Item {
    std::string name;
    std::filesystem::path input;
    NameKey key;
  };

  std::vector<Item> items;
  for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
    if (ec) {
      error = "cannot list " + dir.string() + ": " + ec.message();
      return false;
    }
    if (!entry.is_regular_file(ec))
      continue;
    if (entry.path().extension() != ".in")
      continue;
    const std::string name = entry.path().stem().string();
    items.push_back({name, entry.path(), make_name_key(name)});
  }

  std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) {
    return compare_name_key(a.key, b.key);
  });

  for (std::size_t i = 1; i < items.size(); ++i) {
    if (identical(items[i - 1].key, items[i].key)) {
      error =
          "duplicate case name: " + items[i - 1].name + " vs " + items[i].name;
      return false;
    }
  }

  cases.reserve(items.size());
  for (const Item &item : items) {
    BatchCase c;
    c.name = item.name;
    c.input = item.input;

    const std::filesystem::path as_ans = dir / (item.name + ".ans");
    const std::filesystem::path as_out = dir / (item.name + ".out");
    if (std::filesystem::is_regular_file(as_ans, ec)) {
      c.expected = as_ans;
      c.has_answer = true;
    } else if (std::filesystem::is_regular_file(as_out, ec)) {
      c.expected = as_out;
      c.has_answer = true;
    }
    cases.push_back(std::move(c));
  }
  return true;
}

std::vector<BatchUnit> run_all(const AppConfig &cfg,
                               const std::vector<BatchCase> &cases,
                               const std::filesystem::path &exe,
                               std::size_t threads) {
  const std::size_t n = cases.size();
  std::vector<BatchUnit> units(n);
  if (n == 0)
    return units;

  BatchDisplay display(cases, cfg.time_limit);
  display.start();

  const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
  const std::size_t cap = threads == 0 ? 1 : threads;
  const std::size_t concurrency =
      std::min({n, static_cast<std::size_t>(hw), cap});

  std::atomic<std::size_t> next{0};
  const auto worker = [&]() {
    for (;;) {
      const std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
      if (i >= n)
        break;

      BatchUnit &unit = units[i];
      unit.name = cases[i].name;
      unit.has_answer = cases[i].has_answer;

      SingleRunOption opt;
      opt.exe_path = exe;
      opt.work_dir = cfg.work_dir / ("b" + std::to_string(i));
      opt.time_limit_ms = cfg.time_limit;
      opt.wall_limit_ms = wall_limit_for(cfg.time_limit);
      opt.memory_limit_bytes = cfg.memory_limit_bytes;
      opt.input_text = read_file(cases[i].input);
      opt.echo = false;
      opt.colorize_output = false;
      opt.tagged_stream = false;
      opt.merge_stderr = cfg.merge_stderr == 1;
      opt.on_started = [&display, i](void *process, void *) {
        display.mark_started(i);
        report_program_started(process);
      };
      if (cfg.time_limit.count() > 0) {
        opt.on_tick = [&display, i](std::chrono::milliseconds cpu,
                                    std::chrono::milliseconds) {
          display.tick(i, cpu);
        };
      }

      SingleRunResult run = run_single(opt);
      unit.result = run.result;
      unit.captured = std::move(run.captured);

      if (unit.has_answer) {
        const std::string expected = read_file(cases[i].expected);
        unit.accepted = same_output(unit.captured, expected);
      }

      display.finish(i, unit.result.cpu_time, unit.result.memory_bytes,
                     verdict_of(unit));
    }
  };

  std::vector<std::thread> workers;
  workers.reserve(concurrency);
  for (std::size_t t = 0; t < concurrency; ++t)
    workers.emplace_back(worker);
  for (std::thread &w : workers)
    w.join();

  display.stop();
  return units;
}

BatchVerdict verdict_of(const BatchUnit &unit) {
  switch (unit.result.status) {
  case RunnerStatus::TimeLimitExceeded:
    return BatchVerdict::TimeLimitExceeded;
  case RunnerStatus::MemoryLimitExceeded:
    return BatchVerdict::MemoryLimitExceeded;
  case RunnerStatus::RuntimeError:
    return BatchVerdict::RuntimeError;
  case RunnerStatus::SystemError:
    return BatchVerdict::SystemError;
  case RunnerStatus::Success:
    if (!unit.has_answer)
      return BatchVerdict::NoAnswer;
    return unit.accepted ? BatchVerdict::Accepted : BatchVerdict::WrongAnswer;
  }
  return BatchVerdict::SystemError;
}

const char *verdict_name(BatchVerdict verdict) {
  switch (verdict) {
  case BatchVerdict::Accepted:
    return "AC";
  case BatchVerdict::WrongAnswer:
    return "WA";
  case BatchVerdict::NoAnswer:
    return "NA";
  case BatchVerdict::TimeLimitExceeded:
    return "TLE";
  case BatchVerdict::MemoryLimitExceeded:
    return "MLE";
  case BatchVerdict::RuntimeError:
    return "RE";
  case BatchVerdict::SystemError:
    return "SE";
  }
  return "?";
}

bool save_outputs(const AppConfig &cfg, const std::vector<BatchUnit> &units,
                  std::filesystem::path &folder, std::string &error) {
  if (cfg.batch_output.empty()) {
    error = "[batch].output is not configured";
    return false;
  }

  SYSTEMTIME stamp{};
  ::GetLocalTime(&stamp);
  char name[32]{};
  std::snprintf(
      name, sizeof(name), "%04u%02u%02u_%02u%02u%02u",
      static_cast<unsigned>(stamp.wYear), static_cast<unsigned>(stamp.wMonth),
      static_cast<unsigned>(stamp.wDay), static_cast<unsigned>(stamp.wHour),
      static_cast<unsigned>(stamp.wMinute),
      static_cast<unsigned>(stamp.wSecond));

  const std::filesystem::path root = cfg.batch_output;
  std::filesystem::path dir = root / name;

  std::error_code ec;
  for (int i = 1; i < 1000 && std::filesystem::exists(dir, ec); ++i)
    dir = root / (std::string(name) + "_" + std::to_string(i));

  std::filesystem::create_directories(dir, ec);
  if (ec) {
    error = "cannot create " + dir.string() + ": " + ec.message();
    return false;
  }

  for (const BatchUnit &unit : units) {
    const std::filesystem::path file = dir / (unit.name + ".out");
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
      error = "cannot create " + file.string();
      return false;
    }
    out.write(unit.captured.data(),
              static_cast<std::streamsize>(unit.captured.size()));
    if (!out) {
      error = "cannot write " + file.string();
      return false;
    }
  }

  folder = dir;
  return true;
}

} // namespace app
} // namespace coding
