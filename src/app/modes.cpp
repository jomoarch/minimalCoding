#include "app/modes.hpp"

#include "app/builder.hpp"
#include "app/interactive_ui.hpp"
#include "app/prompt.hpp"
#include "base/color.hpp"
#include "base/path.hpp"
#include "base/text.hpp"
#include "io/source.hpp"
#include "process/runner_single.hpp"

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
namespace coding {

namespace fs = std::filesystem;

namespace {

enum class BuildTarget { Normal, Probe };

bool uses_probe_build(const AppConfig &cfg) {
  return cfg.inject_probe && !cfg.inject_header.empty();
}

bool need_probe_header(const AppConfig &cfg, std::string &error) {
  if (!uses_probe_build(cfg))
    return true;

  std::error_code ec;
  if (!std::filesystem::exists(cfg.inject_header, ec)) {
    error = "injection header not found: " + show_path(cfg.inject_header) +
            " (set [inject].header, or [inject].enabled = false)";
    return false;
  }
  return true;
}

std::string format_time(const std::filesystem::path &path) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
    return {};

  FILETIME local{};
  if (!::FileTimeToLocalFileTime(&data.ftLastWriteTime, &local))
    return {};

  SYSTEMTIME stamp{};
  if (!::FileTimeToSystemTime(&local, &stamp))
    return {};

  char buffer[32]{};
  std::snprintf(
      buffer, sizeof(buffer), "%04u-%02u-%02u %02u:%02u:%02u",
      static_cast<unsigned>(stamp.wYear), static_cast<unsigned>(stamp.wMonth),
      static_cast<unsigned>(stamp.wDay), static_cast<unsigned>(stamp.wHour),
      static_cast<unsigned>(stamp.wMinute),
      static_cast<unsigned>(stamp.wSecond));
  return buffer;
}

void print_source(const PickedSource &pick) {
  std::cout << color::info("[source] ") << show_path(pick.path);
  const std::string stamp = format_time(pick.path);
  if (!stamp.empty())
    std::cout << color::gray(" (modified ", stamp, ")");
  std::cout << '\n';
}

bool pick_target(const AppConfig &cfg, BuildPaths &paths, PickedSource &pick,
                 std::string &error) {
  SourceResult found = pick_source(cfg.source_path);
  if (!found) {
    error = found.message;
    return false;
  }
  pick = found.source;
  paths = build_paths(cfg, pick.path);
  return true;
}

BuildResult build_step(const AppConfig &cfg, const BuildPaths &paths,
                       BuildTarget target) {
  BuildOption opt;
  opt.source_path = paths.source;
  opt.output_path = target == BuildTarget::Probe ? paths.exe_probe : paths.exe;
  opt.args = cfg.args;
  opt.cache_path = cfg.cache_path;
  opt.extra_deps.push_back(cfg.config_path);

  if (target == BuildTarget::Probe && uses_probe_build(cfg)) {
    opt.inject_header = cfg.inject_header;
    opt.extra_deps.push_back(cfg.inject_header);
  }
  opt.force_rebuild = cfg.force_rebuild;

  const BuildResult br = ensure_built(opt);

  if (!br)
    std::cerr << color::err("[compile] Failed:\n", br.message) << "\n";
  return br;
}

void print_build_line(const std::filesystem::path &exe, const BuildResult &s) {
  color::Scope green(std::cout, {color::Code::Green, color::Code::Bold});
  if (s.rebuilt) {
    std::cout << "[build] built -> " << show_path(exe);
  } else {
    std::cout << "[build] up-to-date (" << text::to_text(exe.filename());
    if (!s.reason.empty())
      std::cout << ", " << s.reason;
    std::cout << ')';
  }
  if (s.rebuilt && !s.reason.empty())
    std::cout << " (" << s.reason << ')';
  std::cout << '\n';
}

bool write_text(const fs::path &path, const std::string &text) {
  std::error_code ec;
  if (!path.parent_path().empty())
    fs::create_directories(path.parent_path(), ec);

  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f)
    return false;
  f.write(text.data(), static_cast<std::streamsize>(text.size()));
  return f.good();
}

inline std::string bg(std::string_view s) {
  return color::paint(s, {color::Code::BgWhite, color::Code::Black});
}

std::string format_memory(std::size_t bytes) {
  if (bytes == 0)
    return "0.00  B";

  static const char *units[] = {"  B", " KB", " MB", " GB"};
  constexpr int last_unit = 3;

  double v = static_cast<double>(bytes);
  int i = 0;
  while (v >= 1023.995 && i < last_unit) {
    v /= 1024.0;
    ++i;
  }

  std::ostringstream oss;
  oss << std::fixed << std::setprecision(2) << v << units[i];
  return oss.str();
}

int finish_single(const AppConfig &cfg, const SingleRunResult &run,
                  bool is_interactive) {
  if (!run.warning.empty())
    std::cerr << color::warn("[probe] ", run.warning) << "\n";

  auto status = [&]() -> std::string {
    switch (run.result.status) {
    case RunnerStatus::Success:
      return color::ok("success");
    case RunnerStatus::TimeLimitExceeded:
      return color::err("Time Limit Exceeded");
    case RunnerStatus::MemoryLimitExceeded:
      return color::err("Memory Limit Exceeded");
    case RunnerStatus::RuntimeError:
      return color::err("Runtime Rrror");
    default:
      return color::warn(run.result.message);
    }
  };

  std::ostringstream oss;

  oss << "[result] cpu "
      << bg(std::to_string(run.result.cpu_time.count()) + " ms") << " wall "
      << bg(std::to_string(run.result.wall_time.count()) + " ms") << " mem "
      << bg(format_memory(run.result.memory_bytes)) << "  " << status();

  std::string out = oss.str();
  const std::size_t w = color::visible_width(out);
  std::cout << '\n' << std::string(w, '-') << '\n' << out << '\n';

  if (cfg.single_output.empty()) {
    std::cout << "[save] [io].single_output is not configured, nothing saved\n";
  } else if (prompt::ask_save(cfg.single_output, "output")) {
    if (write_text(cfg.single_output, run.captured)) {
      std::cout << color::ok("[save] saved ", run.captured.size(),
                             " byte(s) -> ", cfg.single_output)
                << "\n";
    } else {
      std::cerr << color::err("[save] Failed to write ", cfg.single_output)
                << "\n";
      return 2;
    }
  }

  if (!is_interactive)
    return run.result.status == RunnerStatus::Success ? 0 : 1;
  if (run.user_input.empty()) {
    std::cout << "[save] your input is empty, nothing saved\n";
  } else if (prompt::ask_save(cfg.single_input, "input")) {
    if (write_text(cfg.single_input, run.user_input)) {
      std::cout << color::ok("[save] saved ", run.user_input.size(),
                             " byte(s) -> ", cfg.single_input)
                << "\n";
    } else {
      std::cerr << color::err("[save] Failed to write ", cfg.single_input)
                << "\n";
      return 2;
    }
  }

  return run.result.status == RunnerStatus::Success ? 0 : 1;
}

} // namespace

ModeOutcome run_interactive(const AppConfig &cfg) {
  ModeOutcome out;

  BuildPaths paths;
  PickedSource pick;
  std::string error;
  if (!pick_target(cfg, paths, pick, error)) {
    std::cerr << color::err("[source] ", error) << "\n";
    out.code = 2;
    out.wait_key = true;
    return out;
  }
  print_source(pick);

  if (!need_probe_header(cfg, error)) {
    std::cerr << color::err("[config] ", error) << "\n";
    out.code = 2;
    return out;
  }

  const BuildResult built = build_step(cfg, paths, BuildTarget::Probe);
  if (!built) {
    out.code = 2;
    return out;
  }
  print_build_line(paths.exe_probe, built);

  std::cout << "[run] interactive mode\n";

  SingleRunOption opt;
  opt.exe_path = paths.exe_probe;
  opt.work_dir = cfg.work_dir;
  opt.echo = true;
  opt.colorize_output = cfg.colorize_output;
  opt.tagged_stream = uses_probe_build(cfg);
  opt.merge_stderr = cfg.merge_stderr == 1;
  opt.time_limit_ms = cfg.time_limit;
  opt.memory_limit_bytes = cfg.memory_limit_bytes;

  SingleRunResult run;
  if (!cfg.status_line || !interactive::run(cfg, opt, run)) {
    opt.stdin_from_console = true;
    run = run_single(opt);
  }
  out.code = finish_single(cfg, run, true);
  return out;
}

ModeOutcome run_single_file(const AppConfig &cfg) {
  ModeOutcome out;

  BuildPaths paths;
  PickedSource pick;
  std::string error;
  if (!pick_target(cfg, paths, pick, error)) {
    std::cerr << color::err("[source] ", error) << "\n";
    out.code = 2;
    out.wait_key = true;
    return out;
  }
  print_source(pick);

  if (!need_probe_header(cfg, error)) {
    std::cerr << color::err("[config] ", error) << "\n";
    out.code = 2;
    return out;
  }

  if (cfg.single_input.empty()) {
    std::cerr << color::err("[io] [io].single_input is required in -s mode")
              << "\n";
    out.code = 2;
    return out;
  }

  std::ifstream in(cfg.single_input, std::ios::binary);
  if (!in) {
    std::cerr << color::err("[io] Cannot open single_input: ", cfg.single_input)
              << "\n";
    out.code = 2;
    return out;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string input = ss.str();

  const BuildResult built = build_step(cfg, paths, BuildTarget::Probe);
  if (!built) {
    out.code = 2;
    return out;
  }
  print_build_line(paths.exe_probe, built);

  const std::size_t input_size = input.size();
  std::cout << color::ok("[io] input <- ", cfg.single_input, " (", input_size,
                         input_size == 1 ? " byte" : " bytes", ")")
            << "\n";

  if (cfg.print_input) {
    std::cout << "--- input ---\n" << input;
    if (!input.empty() && input.back() != '\n')
      std::cout << "\n";
  }

  std::cout << "\n--- output ---\n";

  SingleRunOption opt;
  opt.exe_path = paths.exe_probe;
  opt.work_dir = cfg.work_dir;
  opt.stdin_from_console = false;
  opt.input_text = input;
  opt.echo = true;
  opt.colorize_output = cfg.colorize_output;
  opt.tagged_stream = uses_probe_build(cfg);
  opt.merge_stderr = cfg.merge_stderr == 1;
  opt.time_limit_ms = cfg.time_limit;
  opt.memory_limit_bytes = cfg.memory_limit_bytes;

  out.code = finish_single(cfg, run_single(opt), false);
  return out;
}

} // namespace coding
