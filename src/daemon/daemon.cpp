#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "app/app.hpp"

#include "daemon/daemon_status.hpp"
#include "daemon/hotkey_service.hpp"
#include "daemon/job_launcher.hpp"
#include "base/color.hpp"
#include "base/path.hpp"
#include "io/config.hpp"
#include "io/hotkey.hpp"

#include <windows.h>

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace coding {
namespace app {

namespace {

constexpr int kHotkeyInteractive = 1;
constexpr int kHotkeySingle = 2;
constexpr int kHotkeyQuit = 3;

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

bool resolve_slots(const AppConfig &cfg, std::vector<HotkeySlot> &slots,
                   std::string &error) {
  slots.clear();
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
      error = "bad " + slot.label + " hotkey: " + parsed.message;
      return false;
    }
    slot.key = parsed.key;
    slot.enabled = true;
  }

  for (std::size_t i = 0; i < slots.size(); ++i) {
    for (std::size_t j = i + 1; j < slots.size(); ++j) {
      if (slots[i].enabled && slots[j].enabled &&
          slots[i].key.same_as(slots[j].key)) {
        error = format_hotkey(slots[i].key) + " is assigned to both " +
                slots[i].label + " and " + slots[j].label;
        return false;
      }
    }
  }
  return true;
}

int run_service() {
  std::string error;

  switch (acquire_singleton(error)) {
  case SingletonState::Acquired:
    break;
  case SingletonState::AlreadyRunning:
    std::cout << color::info("[cg_t] already running") << "\n";
    return 0;
  case SingletonState::Failed:
    std::cerr << color::err("[cg_t] cannot create the singleton mutex: ", error)
              << "\n";
    return 2;
  }

  const ConfigResult loaded = load_config("config.toml");
  if (!loaded) {
    std::cerr << color::err("[config] ", loaded.message) << "\n";
    release_singleton();
    return 2;
  }
  const AppConfig &cfg = loaded.config;

  std::vector<HotkeySlot> slots;
  if (!resolve_slots(cfg, slots, error)) {
    std::cerr << color::err("[daemon] ", error) << "\n";
    release_singleton();
    return 2;
  }

  JobLauncher launcher;
  if (!launcher.open(error)) {
    std::cerr << color::err("[cg_t] ", error) << "\n";
    release_singleton();
    return 2;
  }

  HotkeyService keys;
  if (!keys.open(error)) {
    std::cerr << color::err("[cg_t] ", error) << "\n";
    release_singleton();
    return 2;
  }

  int usable = 0;
  for (HotkeySlot &slot : slots) {
    if (!slot.enabled)
      continue;

    if (keys.add(slot, slot.why)) {
      slot.registered = true;
      if (slot.id != kHotkeyQuit)
        ++usable;
      continue;
    }

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

    write_status(cfg, slots);
    release_singleton();
    return 2;
  }

  std::cout << color::ok("[cg_t] daemon started") << " (pid "
            << color::info(::GetCurrentProcessId()) << ")\n";
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

  const auto on_hotkey = [&](int id) {
    if (id == kHotkeyQuit) {
      keys.request_stop();
      return;
    }

    const Job job = id == kHotkeySingle ? Job::Single : Job::Interactive;
    std::string why;
    switch (launcher.launch(job, why)) {
    case LaunchResult::Started:
      break;
    case LaunchResult::Busy:
      ::Beep(880, 40);
      break;
    case LaunchResult::Failed:
      std::cerr << color::err("[cg_t] cannot start job: ", why) << "\n";
      break;
    }
  };

  const auto slot_handles = [&]() {
    std::vector<HANDLE> handles;
    handles.reserve(kJobCount);
    for (int i = 0; i < kJobCount; ++i)
      handles.push_back(launcher.handle(static_cast<Job>(i)));
    return handles;
  };

  const auto on_slot_done = [&](std::size_t slot) {
    if (slot < static_cast<std::size_t>(kJobCount))
      launcher.reap(static_cast<Job>(slot));
  };

  if (!keys.run(on_hotkey, slot_handles, on_slot_done, error))
    std::cerr << color::err("[cg_t] ", error) << "\n";

  launcher.reap_all();
  remove_status(cfg);
  std::cout << color::info("[cg_t] stopped") << "\n";
  release_singleton();
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

    if (arg == "--stop") {
      std::string error;
      const StopSignal signal = send_stop(error);
      if (signal == StopSignal::NotRunning) {
        std::cout << color::warn("[cg_t] not running") << "\n";
        return 1;
      }
      if (signal == StopSignal::Failed) {
        std::cerr << color::err("[cg_t] ", error) << "\n";
        return 1;
      }
      std::cout << color::ok("[cg_t] stopping") << "\n";
      return 0;
    }

    if (arg == "--status")
      return print_status();

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

  return run_service();
}

} // namespace app
} // namespace coding
