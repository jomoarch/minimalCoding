#ifndef DAEMON_STATUS_HPP
#define DAEMON_STATUS_HPP

#include "daemon/hotkey_service.hpp"
#include "io/config.hpp"

#include <string>
#include <vector>
namespace coding {
namespace app {

enum class SingletonState { Acquired, AlreadyRunning, Failed };

SingletonState acquire_singleton(std::string &error);
void release_singleton();

bool daemon_running();

enum class StopSignal { Signaled, NotRunning, Failed };

StopSignal send_stop(std::string &error);

void write_status(const AppConfig &cfg, const std::vector<HotkeySlot> &slots);

void remove_status(const AppConfig &cfg);

int print_status();

} // namespace app
} // namespace coding

#endif // DAEMON_STATUS_HPP
