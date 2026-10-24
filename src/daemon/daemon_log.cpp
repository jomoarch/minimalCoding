#include "daemon/daemon_log.hpp"

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
  const std::string line = "[" + timestamp() + "] " + std::string(message);

  std::fputs(line.c_str(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);

  if (file_.is_open()) {
    file_ << line << '\n';
    file_.flush();
  }
}

} // namespace app
} // namespace coding
