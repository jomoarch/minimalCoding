#ifndef SOURCE_HPP
#define SOURCE_HPP

#include "base/result.hpp"

#include <filesystem>
namespace coding {

struct PickedSource {
  std::filesystem::path path;
  std::filesystem::file_time_type mtime{};
};

struct SourceResult : ResultBase {
  PickedSource source;
};

[[nodiscard]] SourceResult pick_source(const std::filesystem::path &target);

} // namespace coding

#endif // SOURCE_HPP
