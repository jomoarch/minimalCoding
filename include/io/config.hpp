#ifndef CONFIG_HPP
#define CONFIG_HPP

#include "base\result.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>
#include <cstddef>
namespace coding {

struct AppConfig {
  std::filesystem::path config_path;

  std::filesystem::path base;

  // compiler
  std::filesystem::path source_path;
  std::filesystem::path exe_path;
  std::filesystem::path exe_path_probe;
  std::vector<std::string> args;

  // inject
  bool inject_probe{true};
  std::filesystem::path inject_header;

  // runner
  std::filesystem::path work_dir;
  std::chrono::milliseconds time_limit{0};
  std::size_t memory_limit_bytes{0};
  bool status_line{true};
  int status_interval_ms{100};

  // io
  std::filesystem::path input_dir;
  std::filesystem::path single_input;
  std::filesystem::path output_dir;
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

[[nodiscard]] ConfigResult load_config(const std::filesystem::path &path);

} // namespace coding

#endif // CONFIG_HPP