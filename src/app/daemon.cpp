#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "app/app.hpp"

#include "base/color.hpp"
#include "base/path.hpp"
#include "base/win_error.hpp"
#include "io/config.hpp"
#include "io/hotkey.hpp"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace coding {
namespace app {

namespace {

constexpr wchar_t kMutexName[] = L"Local\\minimalCoding.cg_t.singleton";
constexpr wchar_t kStopEventName[] = L"Local\\minimalCoding.cg_t.stop";
constexpr wchar_t kWindowClass[] = L"minimalCoding.cg_t.hidden";

constexpr int kHotkeyInteractive = 1;
constexpr int kHotkeySingle = 2;
constexpr int kHotkeyQuit = 3;

enum class Job { Interactive = 0, Single = 1 };

struct HotkeySlot {
  int id{0};
  const char *label{nullptr};
  std::string text;
  Hotkey key;
  bool enabled{false};
  bool registered{false};
  std::string why;
};

struct Service {
  HANDLE stop{nullptr};
  HWND window{nullptr};
  HINSTANCE instance{nullptr};
  std::wstring exe;
  HANDLE job[2]{nullptr, nullptr};
};

Service g_service;

void usage() {
  std::cout
      << "cg_t - global-hotkey daemon for cg\n"
         "\n"
         "usage:\n"
         "  cg_t               run the daemon (reads [daemon] from "
         "config.toml)\n"
         "  cg_t --status      show whether it is running and which keys work\n"
         "  cg_t --stop        make the running daemon exit\n"
         "  cg_t --job i|s     internal: run one interactive / single task\n"
         "  cg_t --help\n"
         "\n"
         "hotkeys (see [daemon] in config.toml):\n"
         "  interactive  default Ctrl+Alt+I\n"
         "  single       default Ctrl+Alt+S\n"
         "  quit         default Ctrl+Alt+Q   (empty value disables a key)\n";
}

std::wstring module_path() {
  std::wstring buffer(MAX_PATH, L'\0');
  for (;;) {
    const DWORD n = ::GetModuleFileNameW(nullptr, buffer.data(),
                                         static_cast<DWORD>(buffer.size()));
    if (n == 0)
      return {};
    if (n < buffer.size()) {
      buffer.resize(n);
      return buffer;
    }
    if (buffer.size() >= 32768)
      return {};
    buffer.resize(buffer.size() * 2);
  }
}

bool job_alive(Job job) {
  const HANDLE handle = g_service.job[static_cast<int>(job)];
  if (handle == nullptr)
    return false;
  DWORD code = 0;
  if (!::GetExitCodeProcess(handle, &code))
    return false;
  return code == STILL_ACTIVE;
}

void reap(Job job) {
  HANDLE &handle = g_service.job[static_cast<int>(job)];
  if (handle != nullptr) {
    ::CloseHandle(handle);
    handle = nullptr;
  }
}

bool spawn(Job job) {
  if (job_alive(job)) {
    ::Beep(880, 40);
    return false;
  }
  reap(job);

  std::wstring command = L"\"" + g_service.exe + L"\" --job ";
  command += job == Job::Interactive ? L"i" : L"s";

  std::vector<wchar_t> buffer(command.begin(), command.end());
  buffer.push_back(L'\0');

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION info{};

  const DWORD flags = CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT;
  if (!::CreateProcessW(g_service.exe.c_str(), buffer.data(), nullptr, nullptr,
                        FALSE, flags, nullptr, nullptr, &startup, &info)) {
    std::cerr << color::err("[cg_t] cannot start job: ",
                            win::last_error_string())
              << "\n";
    return false;
  }

  ::CloseHandle(info.hThread);
  g_service.job[static_cast<int>(job)] = info.hProcess;
  return true;
}

void pump_messages() {
  MSG message{};
  while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
    ::TranslateMessage(&message);
    ::DispatchMessageW(&message);
  }
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam,
                             LPARAM lparam) {
  (void)window;
  (void)lparam;

  switch (message) {
  case WM_HOTKEY:
    switch (static_cast<int>(wparam)) {
    case kHotkeyInteractive:
      spawn(Job::Interactive);
      return 0;
    case kHotkeySingle:
      spawn(Job::Single);
      return 0;
    case kHotkeyQuit:
      if (g_service.stop != nullptr)
        ::SetEvent(g_service.stop);
      return 0;
    default:
      return 0;
    }
  case WM_QUERYENDSESSION:
    return TRUE;
  case WM_ENDSESSION:
  case WM_CLOSE:
    if (g_service.stop != nullptr)
      ::SetEvent(g_service.stop);
    return 0;
  default:
    return ::DefWindowProcW(window, message, wparam, lparam);
  }
}

BOOL WINAPI console_handler(DWORD type) {
  switch (type) {
  case CTRL_C_EVENT:
  case CTRL_BREAK_EVENT:
  case CTRL_CLOSE_EVENT:
  case CTRL_LOGOFF_EVENT:
  case CTRL_SHUTDOWN_EVENT:
    if (g_service.stop != nullptr)
      ::SetEvent(g_service.stop);
    return TRUE;
  default:
    return FALSE;
  }
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

int stop_daemon() {
  HANDLE event = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, kStopEventName);
  if (event == nullptr) {
    std::cout << color::warn("[cg_t] not running") << "\n";
    return 1;
  }
  ::SetEvent(event);
  ::CloseHandle(event);
  std::cout << color::ok("[cg_t] stopping") << "\n";
  return 0;
}

bool daemon_running() {
  HANDLE mutex = ::OpenMutexW(SYNCHRONIZE, FALSE, kMutexName);
  if (mutex == nullptr)
    return false;
  ::CloseHandle(mutex);
  return true;
}

int status() {
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

int service() {
  g_service.instance = ::GetModuleHandleW(nullptr);
  g_service.exe = module_path();
  if (g_service.exe.empty()) {
    std::cerr << color::err("[cg_t] cannot determine my own path") << "\n";
    return 2;
  }

  HANDLE singleton = ::CreateMutexW(nullptr, FALSE, kMutexName);
  if (singleton == nullptr) {
    std::cerr << color::err("[cg_t] cannot create the singleton mutex: ",
                            win::last_error_string())
              << "\n";
    return 2;
  }
  if (::GetLastError() == ERROR_ALREADY_EXISTS) {
    std::cout << color::info("[cg_t] already running") << "\n";
    ::CloseHandle(singleton);
    return 0;
  }

  const ConfigResult loaded = load_config("config.toml");
  if (!loaded) {
    std::cerr << color::err("[config] ", loaded.message) << "\n";
    ::CloseHandle(singleton);
    return 2;
  }
  const AppConfig &cfg = loaded.config;

  std::vector<HotkeySlot> slots;
  slots.push_back(HotkeySlot{kHotkeyInteractive,
                             "interactive",
                             cfg.hotkey_interactive,
                             {},
                             false,
                             false,
                             {}});
  slots.push_back(HotkeySlot{
      kHotkeySingle, "single", cfg.hotkey_single, {}, false, false, {}});
  slots.push_back(
      HotkeySlot{kHotkeyQuit, "quit", cfg.hotkey_quit, {}, false, false, {}});

  for (HotkeySlot &slot : slots) {
    if (slot.text.empty())
      continue;
    const HotkeyResult parsed = parse_hotkey(slot.text);
    if (!parsed) {
      std::cerr << color::err("[daemon] bad ", slot.label,
                              " hotkey: ", parsed.message)
                << "\n";
      ::CloseHandle(singleton);
      return 2;
    }
    slot.key = parsed.key;
    slot.enabled = true;
  }

  for (std::size_t i = 0; i < slots.size(); ++i) {
    for (std::size_t j = i + 1; j < slots.size(); ++j) {
      if (slots[i].enabled && slots[j].enabled &&
          slots[i].key.same_as(slots[j].key)) {
        std::cerr << color::err("[daemon] ", format_hotkey(slots[i].key),
                                " is assigned to both ", slots[i].label,
                                " and ", slots[j].label)
                  << "\n";
        ::CloseHandle(singleton);
        return 2;
      }
    }
  }

  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = window_proc;
  window_class.hInstance = g_service.instance;
  window_class.lpszClassName = kWindowClass;
  ::RegisterClassExW(&window_class);

  g_service.window =
      ::CreateWindowExW(0, kWindowClass, L"cg_t", 0, 0, 0, 0, 0, HWND_MESSAGE,
                        nullptr, g_service.instance, nullptr);
  if (g_service.window == nullptr) {
    std::cerr << color::err("[cg_t] cannot create the message window: ",
                            win::last_error_string())
              << "\n";
    ::CloseHandle(singleton);
    return 2;
  }

  g_service.stop = ::CreateEventW(nullptr, TRUE, FALSE, kStopEventName);
  if (g_service.stop == nullptr) {
    std::cerr << color::err("[cg_t] cannot create the stop event: ",
                            win::last_error_string())
              << "\n";
    ::DestroyWindow(g_service.window);
    ::CloseHandle(singleton);
    return 2;
  }

  int usable = 0;
  for (HotkeySlot &slot : slots) {
    if (!slot.enabled)
      continue;

    if (::RegisterHotKey(g_service.window, slot.id,
                         slot.key.mods | kHotkeyNoRepeat, slot.key.vk)) {
      slot.registered = true;
      if (slot.id != kHotkeyQuit)
        ++usable;
      continue;
    }

    const DWORD code = ::GetLastError();
    slot.why = code == ERROR_HOTKEY_ALREADY_REGISTERED
                   ? std::string("already taken by another program")
                   : win::error_string(code);
    std::cerr << color::warn("[daemon] cannot register ",
                             format_hotkey(slot.key), " for ", slot.label, ": ",
                             slot.why)
              << "\n";
  }

  if (usable == 0) {
    std::cerr << color::err("[daemon] no run hotkey could be registered")
              << "\n"
              << "[daemon] pick free combinations in the [daemon] section of "
              << show_path(cfg.config_path) << "\n";
    for (HotkeySlot &slot : slots) {
      if (slot.registered) {
        ::UnregisterHotKey(g_service.window, slot.id);
        slot.registered = false;
      }
    }

    write_status(cfg, slots);
    ::CloseHandle(g_service.stop);
    ::DestroyWindow(g_service.window);
    ::CloseHandle(singleton);
    return 2;
  }

  std::cout << color::ok("[cg_t] daemon started") << " (pid "
            << ::GetCurrentProcessId() << ")\n";
  std::cout << "[cg_t] config: " << show_path(cfg.config_path) << "\n";
  for (const HotkeySlot &slot : slots) {
    if (!slot.registered)
      continue;
    std::cout << "[cg_t] " << format_hotkey(slot.key) << " -> " << slot.label
              << "\n";
  }
  std::cout << "[cg_t] press the quit hotkey, or run `cg_t --stop`\n"
            << std::flush;

  write_status(cfg, slots);

  ::SetConsoleCtrlHandler(console_handler, TRUE);

  bool running = true;
  while (running) {
    HANDLE waits[3] = {g_service.stop, nullptr, nullptr};
    Job slot_of[2] = {Job::Interactive, Job::Single};
    DWORD count = 1;
    for (int i = 0; i < 2; ++i) {
      if (g_service.job[i] != nullptr) {
        slot_of[count - 1] = static_cast<Job>(i);
        waits[count++] = g_service.job[i];
      }
    }

    const DWORD result =
        ::MsgWaitForMultipleObjects(count, waits, FALSE, INFINITE, QS_ALLINPUT);

    if (result == WAIT_OBJECT_0) {
      break;
    }
    if (result > WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + count) {
      const DWORD index = result - WAIT_OBJECT_0 - 1;
      if (index < 2)
        reap(slot_of[index]);
      continue;
    }
    if (result == WAIT_OBJECT_0 + count) {
      pump_messages();
      continue;
    }
    if (result == WAIT_FAILED) {
      std::cerr << color::err("[cg_t] wait failed: ", win::last_error_string())
                << "\n";
      break;
    }
  }

  ::SetConsoleCtrlHandler(console_handler, FALSE);
  for (HotkeySlot &slot : slots) {
    if (slot.registered)
      ::UnregisterHotKey(g_service.window, slot.id);
  }
  for (int i = 0; i < 2; ++i)
    reap(static_cast<Job>(i));

  remove_status(cfg);
  std::cout << color::info("[cg_t] stopped") << "\n";

  ::CloseHandle(g_service.stop);
  g_service.stop = nullptr;
  ::DestroyWindow(g_service.window);
  g_service.window = nullptr;
  ::CloseHandle(singleton);
  return 0;
}

} // namespace

int run_daemon(int argc, char **argv) {
  ::SetConsoleOutputCP(CP_UTF8);
  color::install();

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];

    if (arg == "-h" || arg == "--help") {
      usage();
      return 0;
    }
    if (arg == "--stop")
      return stop_daemon();
    if (arg == "--status")
      return status();

    if (arg == "--job") {
      if (i + 1 >= argc) {
        std::cerr << "[cg_t] --job needs i or s\n";
        return 2;
      }
      const std::string_view mode = argv[++i];
      if (mode == "i")
        return ::coding::app::run_interactive();
      if (mode == "s")
        return ::coding::app::run_single();
      std::cerr << "[cg_t] unknown job \"" << std::string(mode) << "\"\n";
      return 2;
    }

    std::cerr << "[cg_t] unknown argument \"" << std::string(arg) << "\"\n";
    usage();
    return 2;
  }

  return service();
}

} // namespace app
} // namespace coding
