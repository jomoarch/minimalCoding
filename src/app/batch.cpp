#include "app/batch.hpp"

#include "app/program_hook.hpp"
#include "base/color.hpp"
#include "base/text.hpp"
#include "process/runner_single.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <iomanip>
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

void print_row(std::ostream &os, std::string_view line, BatchVerdict verdict) {
  using color::Code;
  switch (verdict) {
  case BatchVerdict::Accepted:
    os << color::paint(os, line, {Code::Bold, Code::Green});
    break;
  case BatchVerdict::WrongAnswer:
    os << color::paint(os, line, {Code::Bold, Code::Red});
    break;
  case BatchVerdict::TimeLimitExceeded:
  case BatchVerdict::MemoryLimitExceeded:
    os << color::paint(os, line, {Code::BgWhite, Code::Black});
    break;
  case BatchVerdict::RuntimeError:
    os << color::paint(os, line, {Code::Bold, Code::Magenta});
    break;
  case BatchVerdict::SystemError:
    os << color::paint(os, line, {Code::Bold, Code::BrightMagenta});
    break;
  case BatchVerdict::NoAnswer:
    os << color::paint(os, line, {Code::BrightBlack});
    break;
  }
  os << '\n';
}

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
      opt.on_started = [](void *process, void *) {
        report_program_started(process);
      };

      SingleRunResult run = run_single(opt);
      unit.result = run.result;
      unit.captured = std::move(run.captured);

      if (unit.has_answer) {
        const std::string expected = read_file(cases[i].expected);
        unit.accepted = same_output(unit.captured, expected);
      }
    }
  };

  std::vector<std::thread> workers;
  workers.reserve(concurrency);
  for (std::size_t t = 0; t < concurrency; ++t)
    workers.emplace_back(worker);
  for (std::thread &w : workers)
    w.join();
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

std::size_t print_results(const std::vector<BatchUnit> &units,
                          std::ostream &os) {
  if (units.empty())
    return 0;

  struct Row {
    std::string name;
    std::string status;
    std::string wall;
    std::string cpu;
    std::string mem;
    std::string message;
    BatchVerdict verdict;
  };

  std::vector<Row> rows;
  rows.reserve(units.size());
  std::size_t w_name = 0, w_status = 0, w_wall = 0, w_cpu = 0, w_mem = 0;
  std::size_t accepted = 0;

  for (const BatchUnit &unit : units) {
    Row row;
    row.name = unit.name;
    row.verdict = verdict_of(unit);
    row.status = verdict_name(row.verdict);
    row.wall = std::to_string(unit.result.wall_time.count()) + " ms";
    row.cpu = std::to_string(unit.result.cpu_time.count()) + " ms";
    row.mem = format_memory(unit.result.memory_bytes);
    row.message = unit.result.message;

    if (row.verdict == BatchVerdict::Accepted)
      ++accepted;

    w_name = std::max(w_name, text::display_width(row.name));
    w_status = std::max(w_status, text::display_width(row.status));
    w_wall = std::max(w_wall, text::display_width(row.wall));
    w_cpu = std::max(w_cpu, text::display_width(row.cpu));
    w_mem = std::max(w_mem, text::display_width(row.mem));
    rows.push_back(std::move(row));
  }

  for (const Row &row : rows) {
    std::string line = text::pad_right(row.name, w_name);
    line += "  ";
    line += text::pad_left(row.status, w_status);
    line += "  ";
    line += text::pad_left(row.wall, w_wall);
    line += "  ";
    line += text::pad_left(row.cpu, w_cpu);
    line += "  ";
    line += text::pad_left(row.mem, w_mem);
    if (!row.message.empty()) {
      line += "  ";
      line += row.message;
    }
    print_row(os, line, row.verdict);
  }

  return accepted;
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
