#include "app/modes.hpp"

#include "app/builder.hpp"
#include "app/prompt.hpp"
#include "app/interactive_ui.hpp"
#include "process/runner_single.hpp"
#include "base/color.hpp"
#include "base/path.hpp"
#include "base/text.hpp"

#include <windows.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
namespace coding {

namespace fs = std::filesystem;

namespace {

enum class BuildTarget { Normal, Probe };

std::filesystem::path target_exe(const AppConfig &cfg, BuildTarget target) {
  return target == BuildTarget::Probe ? cfg.exe_path_probe : cfg.exe_path;
}

bool uses_probe_build(const AppConfig &cfg) {
  return cfg.inject_probe && !cfg.inject_header.empty();
}

bool need_compiler_inputs(const AppConfig &cfg, std::string &error) {
  if (cfg.source_path.empty()) {
    error = "[compiler].source is not set";
    return false;
  }
  if (cfg.exe_path.empty()) {
    error = "[compiler].output is not set";
    return false;
  }
  if (cfg.work_dir.empty()) {
    error = "[runner].work_dir is not set";
    return false;
  }
  return true;
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
  if (cfg.exe_path_probe == cfg.exe_path) {
    error = "[compiler].output_probe must differ from [compiler].output - the "
            "probe build injects a header that cg_b must not see";
    return false;
  }
  return true;
}

int report_missing(const std::string &error) {
  std::cerr << color::err("[config] ", error) << "\n";
  return 2;
}

BuildResult build_step(const AppConfig &cfg, BuildTarget target) {
  BuildOption opt;
  opt.source_path = cfg.source_path;
  opt.output_path = target_exe(cfg, target);
  opt.args = cfg.args;
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
  if (s.rebuilt)
    std::cout << "[build] built -> " << show_path(exe) << '\n';
  else
    std::cout << "[build] up-to-date (" << exe.filename().string() << ")\n";
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

int run_interactive(const AppConfig &cfg) {
  std::string missing;
  if (!need_compiler_inputs(cfg, missing) || !need_probe_header(cfg, missing))
    return report_missing(missing);

  const BuildResult built = build_step(cfg, BuildTarget::Probe);
  if (!built)
    return 2;
  print_build_line(cfg.exe_path_probe, built);

  std::cout << "[run] interactive mode\n";

  SingleRunOption opt;
  opt.exe_path = cfg.exe_path_probe;
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
  return finish_single(cfg, run, true);
}

int run_single_file(const AppConfig &cfg) {
  std::string missing;
  if (!need_compiler_inputs(cfg, missing) || !need_probe_header(cfg, missing))
    return report_missing(missing);

  if (cfg.single_input.empty()) {
    std::cerr << color::err("[io] [io].single_input is required in -s mode")
              << "\n";
    return 2;
  }

  std::ifstream in(cfg.single_input, std::ios::binary);
  if (!in) {
    std::cerr << color::err("[io] Cannot open single_input: ", cfg.single_input)
              << "\n";
    return 2;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string input = ss.str();

  const BuildResult built = build_step(cfg, BuildTarget::Probe);
  if (!built)
    return 2;
  print_build_line(cfg.exe_path_probe, built);

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
  opt.exe_path = cfg.exe_path_probe;
  opt.work_dir = cfg.work_dir;
  opt.stdin_from_console = false;
  opt.input_text = input;
  opt.echo = true;
  opt.colorize_output = cfg.colorize_output;
  opt.tagged_stream = uses_probe_build(cfg);
  opt.merge_stderr = cfg.merge_stderr == 1;
  opt.time_limit_ms = cfg.time_limit;
  opt.memory_limit_bytes = cfg.memory_limit_bytes;

  return finish_single(cfg, run_single(opt), false);
}

} // namespace coding
