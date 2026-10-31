#ifndef PCH_HPP
#define PCH_HPP

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>
namespace coding {

namespace pch {

struct Request {
  std::filesystem::path header;
  std::filesystem::path cache_dir;
  std::vector<std::string> args;
};

struct Result {
  bool ok{false};
  std::filesystem::path include_path;
  bool rebuilt{false};
  std::chrono::milliseconds elapsed{0};
  std::string error;
};

Result ensure(const Request &request);

} // namespace pch
} // namespace coding

#endif // PCH_HPP
