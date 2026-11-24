#ifndef BUILD_CACHE_HPP
#define BUILD_CACHE_HPP

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
namespace coding {

namespace cache {

struct Entry {
  std::string source_sha256;
  std::string exe_sha256;
  std::string key;
};

class Table {
public:
  explicit Table(std::filesystem::path path = {});

  const std::filesystem::path &path() const noexcept { return path_; }

  bool load(std::string &warning);

  const Entry *find(std::string_view key) const;

  void touch(std::string_view key, std::string_view source_sha256,
             std::string_view exe_sha256);

  bool save(std::string &error) const;

  const std::vector<Entry> &entries() const noexcept { return entries_; }

private:
  std::filesystem::path path_;
  std::vector<Entry> entries_;
};

} // namespace cache

} // namespace coding

#endif // BUILD_CACHE_HPP
