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

bool newest_under(const fs::path &dir, PickedSource &out, std::string &error) {
  std::error_code ec;
  fs::recursive_directory_iterator it(
      dir, fs::directory_options::skip_permission_denied, ec);
  if (ec) {
    error = "cannot scan " + show_path(dir) + ": " + ec.message();
    return false;
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
        const bool newer = !found || stamp > best.mtime ||
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
        error = "cannot scan " + show_path(dir) + ": " + ec.message();
        return false;
      }
      break;
    }
  }

  if (!found) {
    error = "no .cpp source file under " + show_path(dir);
    return false;
  }
  out = std::move(best);
  return true;
}

} // namespace

SourceResult pick_source(const fs::path &target) {
  SourceResult r;

  if (target.empty()) {
    r.message = "[compiler].source is not set";
    return r;
  }

  std::error_code ec;
  if (!fs::exists(target, ec)) {
    r.message = "source not found: " + show_path(target);
    return r;
  }

  if (fs::is_directory(target, ec)) {
    PickedSource pick;
    std::string error;
    if (!newest_under(target, pick, error)) {
      r.message = error;
      return r;
    }
    r.success = true;
    r.message = "OK";
    r.source = std::move(pick);
    return r;
  }

  if (!fs::is_regular_file(target, ec)) {
    r.message = "source is neither a directory nor a regular file: " +
                show_path(target);
    return r;
  }

  if (!is_cpp_source(target)) {
    r.message = "source is not a .cpp file: " + show_path(target) +
                " (only .cpp files are compiled)";
    return r;
  }

  r.success = true;
  r.message = "OK";
  r.source.path = target;
  std::error_code time_ec;
  r.source.mtime = fs::last_write_time(target, time_ec);
  return r;
}

} // namespace coding
