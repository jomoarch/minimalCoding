#include "app/stale_exe.hpp"

#include "base/hash.hpp"
#include "base/path.hpp"
#include "base/win_error.hpp"

#include <cstdio>
#include <ctime>
#include <system_error>
namespace coding {

namespace fs = std::filesystem;

namespace {

fs::path unique_target(const fs::path &dir, const std::string &name) {
  std::error_code ec;
  fs::path target = dir / name;
  for (int n = 2; n < 1000 && fs::exists(target, ec); ++n)
    target = dir / (name + "-" + std::to_string(n));
  return target;
}

} // namespace

namespace stale_exe {

Outcome clear(const fs::path &exe, bool park, const fs::path &trash_dir,
              std::string &note) {
  std::error_code ec;
  if (exe.empty() || !fs::exists(exe, ec))
    return Outcome::Cleared;

  ec.clear();
  fs::remove(exe, ec);
  if (!ec) {
    ec.clear();
    if (!fs::exists(exe, ec))
      return Outcome::Cleared;
  }

  const std::string reason =
      ec ? win::error_string(static_cast<DWORD>(ec.value()))
         : std::string("the file is still open");
  const fs::path dir =
      trash_dir.empty() ? exe.parent_path() / ".trash" : trash_dir;

  if (!park) {
    note = "cannot delete " + show_path(exe) + ": " + reason;
    return Outcome::Blocked;
  }

  ec.clear();
  fs::create_directories(dir, ec);
  if (ec) {
    note = "cannot delete " + show_path(exe) + ": " + reason +
           ", and cannot create " + show_path(dir) + ": " +
           win::error_string(static_cast<DWORD>(ec.value()));
    return Outcome::Blocked;
  }

  std::string name;
  if (!hash::file_sha256(exe, name))
    name = "unreadable-" +
           std::to_string(static_cast<long long>(std::time(nullptr)));

  const fs::path target = unique_target(dir, name);
  ec.clear();
  fs::rename(exe, target, ec);
  if (ec) {
    note = "cannot delete " + show_path(exe) + ": " + reason +
           ", and cannot move it to " + show_path(target) + ": " +
           win::error_string(static_cast<DWORD>(ec.value()));
    return Outcome::Blocked;
  }

  note = "cannot delete " + show_path(exe) + ": " + reason + "; moved it to " +
         show_path(target);
  return Outcome::Parked;
}

} // namespace stale_exe
} // namespace coding
