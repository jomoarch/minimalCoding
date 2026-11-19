#include "daemon/daemon_log.hpp"
#include "base/color.hpp"

#include <windows.h>

#include <cstdio>
#include <system_error>
namespace coding {
namespace app {

namespace {

constexpr std::uintmax_t kMaxBytes = 512u * 1024u;

std::string timestamp() {
  SYSTEMTIME stamp{};
  ::GetLocalTime(&stamp);

  char buffer[32]{};
  std::snprintf(
      buffer, sizeof(buffer), "%04u-%02u-%02u %02u:%02u:%02u",
      static_cast<unsigned>(stamp.wYear), static_cast<unsigned>(stamp.wMonth),
      static_cast<unsigned>(stamp.wDay), static_cast<unsigned>(stamp.wHour),
      static_cast<unsigned>(stamp.wMinute),
      static_cast<unsigned>(stamp.wSecond));
  return buffer;
}

} // namespace

void DaemonLog::open(const std::filesystem::path &file) {
  std::error_code ec;
  if (!file.parent_path().empty())
    std::filesystem::create_directories(file.parent_path(), ec);

  const std::uintmax_t size = std::filesystem::exists(file, ec)
                                  ? std::filesystem::file_size(file, ec)
                                  : 0;
  if (!ec && size > kMaxBytes)
    std::filesystem::remove(file, ec);

  file_.open(file, std::ios::app);
}

void DaemonLog::write(std::string_view message) {
  const std::string stamp = timestamp();
  const std::string line = "[" + stamp + "] " + std::string(message);

  std::string shown;
  shown.reserve(line.size() + 16);
  if (color::enabled()) {
    shown += "\x1b[37m[";
    shown += color::green(stamp);
    shown += "\x1b[37m] ";
  } else {
    shown += '[';
    shown += stamp;
    shown += "] ";
  }
  shown.append(message.data(), message.size());

  std::fputs(shown.c_str(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);

  if (file_.is_open()) {
    file_ << line << '\n';
    file_.flush();
  }
}

} // namespace app
} // namespace coding
