#include "io/source.hpp"

#include "base/path.hpp"
#include "base/text.hpp"

#include <string>
#include <system_error>
#include <utility>
namespace coding {

namespace fs = std::filesystem;

namespace {

bool is_cpp_source(const fs::path &p) {
  return text::to_lower(p.extension().string()) == ".cpp";
}

bool is_dot_directory(const fs::path &p) {
  const std::string name = p.filename().string();
  return !name.empty() && name.front() == '.';
}

} // namespace

SourceResult newest_source(const fs::path &dir) {
  SourceResult r;

  if (dir.empty()) {
    r.message = "[compiler].source is not set";
    return r;
  }

  std::error_code ec;
  if (!fs::exists(dir, ec)) {
    r.message = "source directory not found: " + show_path(dir);
    return r;
  }
  if (!fs::is_directory(dir, ec)) {
    r.message = "source is not a directory: " + show_path(dir) +
                " (set [compiler].source to a folder)";
    return r;
  }

  fs::recursive_directory_iterator it(
      dir, fs::directory_options::skip_permission_denied, ec);
  if (ec) {
    r.message = "cannot scan " + show_path(dir) + ": " + ec.message();
    return r;
  }
  const fs::recursive_directory_iterator end;

  bool found = false;
  PickedSource best;

  while (it != end) {
    const fs::directory_entry &entry = *it;
    std::error_code type_ec;

    if (entry.is_directory(type_ec)) {
      if (is_dot_directory(entry.path()))
        it.disable_recursion_pending();
    } else if (entry.is_regular_file(type_ec) && is_cpp_source(entry.path())) {
      std::error_code time_ec;
      const fs::file_time_type stamp = entry.last_write_time(time_ec);
      if (!time_ec) {
        const bool newer =
            !found || stamp > best.mtime ||
            (stamp == best.mtime && entry.path() < best.path);
        if (newer) {
          found = true;
          best.path = entry.path();
          best.mtime = stamp;
        }
      }
    }

    it.increment(ec);
    if (ec) {
      if (!found) {
        r.message = "cannot scan " + show_path(dir) + ": " + ec.message();
        return r;
      }
      break;
    }
  }

  if (!found) {
    r.message = "no .cpp source file under " + show_path(dir);
    return r;
  }

  r.success = true;
  r.message = "OK";
  r.source = std::move(best);
  return r;
}

} // namespace coding
