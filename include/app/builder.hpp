#ifndef BUILDER_HPP
#define BUILDER_HPP

#include <filesystem>
#include <string>
#include <vector>
namespace coding {

struct BuildOption {
  std::filesystem::path source_path;
  std::filesystem::path output_path;
  std::vector<std::string> args;

  std::vector<std::filesystem::path> extra_deps;

  std::filesystem::path inject_header;

  std::filesystem::path cache_path;

  bool force_rebuild{false};
};

struct BuildResult {
  bool success{false};
  bool rebuilt{false};
  std::string message;
  std::string reason;

  bool ok() const noexcept { return success; }
  explicit operator bool() const noexcept { return success; }
};

BuildResult ensure_built(const BuildOption &opts);

} // namespace coding

#endif // BUILDER_HPP
