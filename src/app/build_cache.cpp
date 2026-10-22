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

    const std::size_t split = view.find_first_of(" \t");
    if (split == std::string_view::npos) {
      ++skipped;
      continue;
    }

    const std::string sha = text::to_lower(view.substr(0, split));
    const std::string name(text::trim(view.substr(split)));
    if (!is_sha256_hex(sha) || name.empty() || find(name) != nullptr) {
      ++skipped;
      continue;
    }

    entries_.push_back(Entry{sha, name});
  }

  if (skipped != 0)
    warning = std::to_string(skipped) + " unusable line(s) in " +
              show_path(path_) + " were skipped";
  return true;
}

const std::string *Table::find(std::string_view filename) const {
  for (const Entry &entry : entries_) {
    if (std::string_view(entry.filename) == filename)
      return &entry.sha256;
  }
  return nullptr;
}

void Table::touch(std::string_view filename, std::string_view sha256) {
  const std::string name(filename);
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [&name](const Entry &entry) {
                                  return entry.filename == name;
                                }),
                 entries_.end());
  entries_.insert(entries_.begin(), Entry{std::string(sha256), name});
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
    file << "# coding build cache: <sha256> <filename>, most recently built "
            "first\n";
    for (const Entry &entry : entries_)
      file << entry.sha256 << ' ' << entry.filename << '\n';
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
