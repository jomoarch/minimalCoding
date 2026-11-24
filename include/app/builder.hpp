#ifndef BUILDER_HPP
#define BUILDER_HPP

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
namespace coding {

struct BuildOption {
  std::filesystem::path source_path;
  std::filesystem::path output_path;
  std::vector<std::string> args;

  std::filesystem::path base;

  std::vector<std::filesystem::path> extra_deps;

  std::filesystem::path inject_header;

  std::filesystem::path cache_path;

  std::filesystem::path trash_dir;

  std::filesystem::path pch_dir;

  std::string cache_key;

  bool force_rebuild{false};
};

struct BuildResult {
  bool success{false};
  bool rebuilt{false};

  std::chrono::milliseconds compile_time{0};
  std::string message;
  std::string reason;

  bool ok() const noexcept { return success; }
  explicit operator bool() const noexcept { return success; }
};

BuildResult ensure_built(const BuildOption &opts);

[[nodiscard]] std::string
recipe_cache_key(std::string_view base_key, bool probe,
                 const std::vector<std::string> &args,
                 const std::filesystem::path &inject_header);

} // namespace coding

#endif // BUILDER_HPP
