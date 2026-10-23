#include "daemon/daemon_status.hpp"

#include "base/color.hpp"
#include "base/path.hpp"
#include "base/win_error.hpp"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <system_error>
namespace coding {
namespace app {

namespace {

HANDLE g_singleton = nullptr;

} // namespace

SingletonState acquire_singleton(std::string &error) {
  if (g_singleton != nullptr)
    return SingletonState::Acquired;

  g_singleton = ::CreateMutexW(nullptr, FALSE, kSingletonName);
  if (g_singleton == nullptr) {
    error = win::last_error_string();
    return SingletonState::Failed;
  }

  if (::GetLastError() == ERROR_ALREADY_EXISTS)
    return SingletonState::AlreadyRunning;
  return SingletonState::Acquired;
}

void release_singleton() {
  if (g_singleton != nullptr) {
    ::CloseHandle(g_singleton);
    g_singleton = nullptr;
  }
}

bool daemon_running() {
  HANDLE mutex = ::OpenMutexW(SYNCHRONIZE, FALSE, kSingletonName);
  if (mutex == nullptr)
    return false;
  ::CloseHandle(mutex);
  return true;
}

StopSignal send_stop(std::string &error) {
  HANDLE event = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, kStopEventName);
  if (event == nullptr) {
    const DWORD code = ::GetLastError();
    if (code == ERROR_FILE_NOT_FOUND)
      return StopSignal::NotRunning;
    error = win::error_string(code);
    return StopSignal::Failed;
  }

  const BOOL ok = ::SetEvent(event);
  if (!ok)
    error = win::last_error_string();
  ::CloseHandle(event);
  return ok ? StopSignal::Signaled : StopSignal::Failed;
}

void write_status(const AppConfig &cfg, const std::vector<HotkeySlot> &slots) {
  const std::filesystem::path file = cfg.coding_dir / "daemon.status";

  std::error_code ec;
  if (!file.parent_path().empty())
    std::filesystem::create_directories(file.parent_path(), ec);

  std::ofstream out(file, std::ios::trunc);
  if (!out)
    return;

  out << "pid=" << ::GetCurrentProcessId() << "\n";
  out << "config=" << show_path(cfg.config_path) << "\n";
  for (const HotkeySlot &slot : slots) {
    out << slot.label << '=';
    if (!slot.enabled) {
      out << "disabled\n";
      continue;
    }
    out << format_hotkey(slot.key);
    if (slot.registered)
      out << " registered\n";
    else
      out << " FAILED: " << slot.why << '\n';
  }
}

void remove_status(const AppConfig &cfg) {
  std::error_code ec;
  std::filesystem::remove(cfg.coding_dir / "daemon.status", ec);
}

int print_status() {
  const bool running = daemon_running();
  std::cout << "[cg_t] " << (running ? "running" : "not running") << "\n";

  const ConfigResult loaded = load_config("config.toml");
  if (!loaded) {
    std::cerr << color::err("[config] ", loaded.message) << "\n";
    return running ? 0 : 1;
  }
  const AppConfig &cfg = loaded.config;
  std::cout << "[cg_t] config: " << show_path(cfg.config_path) << "\n";

  const std::filesystem::path file = cfg.coding_dir / "daemon.status";
  std::ifstream in(file);
  if (!in) {
    if (running)
      std::cout << "[cg_t] no status file yet (" << show_path(file) << ")\n";
    return running ? 0 : 1;
  }

  std::cout << "[cg_t] " << (running ? "state:" : "last run:") << "\n";
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty())
      std::cout << "  " << line << "\n";
  }
  return running ? 0 : 1;
}

} // namespace app
} // namespace coding
