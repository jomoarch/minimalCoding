#include "app/build_cache.hpp"

#include "base/path.hpp"
#include "base/text.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string_view>
#include <system_error>
#include <utility>
namespace coding {

namespace fs = std::filesystem;

namespace cache {

namespace {

constexpr std::size_t kSha256HexLength = 64;

bool is_sha256_hex(std::string_view text) noexcept {
  if (text.size() != kSha256HexLength)
    return false;
  for (const char c : text) {
    if (std::isxdigit(static_cast<unsigned char>(c)) == 0)
      return false;
  }
  return true;
}

} // namespace

Table::Table(fs::path path) : path_(std::move(path)) {}

bool Table::load(std::string &warning) {
  entries_.clear();
  warning.clear();

  if (path_.empty())
    return true;

  std::error_code ec;
  if (!fs::exists(path_, ec))
    return true;
  if (!fs::is_regular_file(path_, ec)) {
    warning = show_path(path_) + " is not a regular file, starting empty";
    return false;
  }

  std::ifstream file(path_, std::ios::binary);
  if (!file) {
    warning = "cannot read " + show_path(path_) + ", starting empty";
    return false;
  }

  std::string line;
  std::size_t skipped = 0;
  while (std::getline(file, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();

    const std::string_view view = text::trim(line);
    if (view.empty() || view.front() == '#')
      continue;

    const std::size_t first = view.find_first_of(" \t");
    const std::size_t second = first == std::string_view::npos
                                   ? std::string_view::npos
                                   : view.find_first_of(" \t", first + 1);
    if (first == std::string_view::npos) {
      ++skipped;
      continue;
    }
    if (second == std::string_view::npos)
      continue;

    const std::string source_sha =
        text::to_lower(text::trim(view.substr(0, first)));
    const std::string exe_sha =
        text::to_lower(text::trim(view.substr(first + 1, second - first - 1)));
    const std::string key(text::trim(view.substr(second + 1)));
    if (!is_sha256_hex(source_sha) || !is_sha256_hex(exe_sha) || key.empty() ||
        find(key) != nullptr) {
      ++skipped;
      continue;
    }

    entries_.push_back(Entry{source_sha, exe_sha, key});
  }

  if (skipped != 0)
    warning = std::to_string(skipped) + " unusable line(s) in " +
              show_path(path_) + " were skipped";
  return true;
}

const Entry *Table::find(std::string_view key) const {
  for (const Entry &entry : entries_) {
    if (std::string_view(entry.key) == key)
      return &entry;
  }
  return nullptr;
}

void Table::touch(std::string_view key, std::string_view source_sha256,
                  std::string_view exe_sha256) {
  const std::string name(key);
  entries_.erase(
      std::remove_if(entries_.begin(), entries_.end(),
                     [&name](const Entry &entry) { return entry.key == name; }),
      entries_.end());
  entries_.insert(entries_.begin(), Entry{std::string(source_sha256),
                                          std::string(exe_sha256), name});
}

bool Table::save(std::string &error) const {
  error.clear();

  if (path_.empty())
    return true;

  std::error_code ec;
  if (!path_.parent_path().empty()) {
    fs::create_directories(path_.parent_path(), ec);
    if (ec) {
      error = "cannot create " + show_path(path_.parent_path()) + ": " +
              ec.message();
      return false;
    }
  }

  fs::path temporary = path_;
  temporary += ".tmp";

  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) {
      error = "cannot write " + show_path(temporary);
      return false;
    }
    file << "# coding build cache: <source sha256> <exe sha256> <recipe key>, "
            "most recently built first\n";
    for (const Entry &entry : entries_)
      file << entry.source_sha256 << ' ' << entry.exe_sha256 << ' ' << entry.key
           << '\n';
    file.flush();
    if (!file) {
      error = "cannot write " + show_path(temporary);
      return false;
    }
  }

  fs::rename(temporary, path_, ec);
  if (ec) {
    std::error_code ignored;
    fs::remove(path_, ignored);
    ec.clear();
    fs::rename(temporary, path_, ec);
  }
  if (ec) {
    error = "cannot replace " + show_path(path_) + ": " + ec.message();
    std::error_code ignored;
    fs::remove(temporary, ignored);
    return false;
  }
  return true;
}

} // namespace cache

} // namespace coding
