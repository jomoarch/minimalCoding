#ifndef CONFIG_HPP
#define CONFIG_HPP

#include "base/result.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>
namespace coding {

struct AppConfig {
  std::filesystem::path config_path;

  std::filesystem::path base;

  std::filesystem::path source_dir;
  std::vector<std::string> args;

  std::filesystem::path coding_dir;
  std::filesystem::path build_dir;
  std::filesystem::path cache_path;
  std::filesystem::path work_dir;

  // inject
  bool inject_probe{true};
  std::filesystem::path inject_header;

  // runner
  std::chrono::milliseconds time_limit{0};
  std::size_t memory_limit_bytes{0};
  bool status_line{true};
  int status_interval_ms{100};

  // io
  std::filesystem::path single_input;
  std::filesystem::path single_output;
  int merge_stderr{-1};
  bool print_input{false};
  bool colorize_output{true};

  // build
  bool force_rebuild{false};
};

struct ConfigResult : ResultBase {
  AppConfig config;
};

struct BuildPaths {
  std::filesystem::path source;
  std::filesystem::path exe;
  std::filesystem::path exe_probe;
};

[[nodiscard]] ConfigResult load_config(const std::filesystem::path &path);

[[nodiscard]] BuildPaths build_paths(const AppConfig &cfg,
                                     const std::filesystem::path &source);

} // namespace coding

#endif // CONFIG_HPP
