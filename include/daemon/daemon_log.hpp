#ifndef DAEMON_LOG_HPP
#define DAEMON_LOG_HPP

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
namespace coding {
namespace app {

class DaemonLog {
public:
  DaemonLog() = default;
  DaemonLog(const DaemonLog &) = delete;
  DaemonLog &operator=(const DaemonLog &) = delete;

  void open(const std::filesystem::path &file);

  void write(std::string_view message);

private:
  std::ofstream file_;
};

} // namespace app
} // namespace coding

#endif // DAEMON_LOG_HPP
