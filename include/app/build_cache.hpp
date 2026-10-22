#ifndef BUILD_CACHE_HPP
#define BUILD_CACHE_HPP

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
namespace coding {

namespace cache {

struct Entry {
  std::string sha256;
  std::string filename;
};

class Table {
public:
  explicit Table(std::filesystem::path path = {});

  const std::filesystem::path &path() const noexcept { return path_; }

  bool load(std::string &warning);

  const std::string *find(std::string_view filename) const;

  void touch(std::string_view filename, std::string_view sha256);

  bool save(std::string &error) const;

  const std::vector<Entry> &entries() const noexcept { return entries_; }

private:
  std::filesystem::path path_;
  std::vector<Entry> entries_;
};

} // namespace cache

} // namespace coding

#endif // BUILD_CACHE_HPP
