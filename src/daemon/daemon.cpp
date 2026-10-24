#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "app/app.hpp"

#include "app/modes.hpp"
#include "app/program_hook.hpp"
#include "daemon/daemon_log.hpp"
#include "daemon/daemon_status.hpp"
#include "daemon/focus_window.hpp"
#include "daemon/hotkey_service.hpp"
#include "daemon/job_launcher.hpp"
#include "daemon/slot_report.hpp"
#include "base/color.hpp"
#include "base/path.hpp"
#include "io/config.hpp"
#include "io/hotkey.hpp"

#include <windows.h>

#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace coding {
namespace app {

namespace {

constexpr int kHotkeyInteractive = 1;
constexpr int kHotkeySingle = 2;
constexpr int kHotkeyQuit = 3;

constexpr UINT_PTR kTagJobI = 1;
constexpr UINT_PTR kTagJobS = 2;
constexpr UINT_PTR kTagSignalI = 3;
constexpr UINT_PTR kTagSignalS = 4;

const char *describe(Job job) {
  return job == Job::Interactive ? "interactive" : "single";
}

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
         "  quit         default Ctrl+Alt+Q   (empty value disables a key)\n"
         "\n"
         "process log goes to stdout and <base>/.coding/daemon.log\n";
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

int run_job(Job job) {
  SlotPublisher publisher;
  publisher.open(job);
  publisher.publish_job(
      ::GetCurrentProcessId(),
      reinterpret_cast<unsigned long long>(::GetConsoleWindow()));

  set_program_started_hook(
      [&publisher](unsigned long pid) { publisher.publish_program(pid); });

  const ModeOutcome outcome =
      job == Job::Interactive ? run_interactive() : run_single();

  publisher.publish_done(static_cast<unsigned long>(outcome.code),
                         outcome.saved_input, outcome.saved_output);
  return outcome.code;
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

  SlotBoard board;
  if (!board.open(error)) {
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

  DaemonLog log;
  log.open(cfg.coding_dir / "daemon.log");

  std::cout << color::ok("[cg_t] daemon started") << " (pid "
            << color::info(::GetCurrentProcessId()) << ")\n";
  std::cout << "[cg_t] config: " << show_path(cfg.config_path) << "\n";
  for (const HotkeySlot &slot : slots) {
    if (!slot.registered)
      continue;
    std::cout << "[cg_t] " << format_hotkey(slot.key) << " -> " << slot.label
              << "\n";
  }
  std::cout << "[cg_t] press the quit hotkey, or run `cg_t --stop`\n";
  std::cout << "==================================================\n"
            << std::flush;

  write_status(cfg, slots);
  log.write("daemon: started (pid " + std::to_string(::GetCurrentProcessId()) +
            ")");

  unsigned long long logged_program[kJobCount]{0, 0};

  const auto focus_running = [&](Job job) {
    const SlotReport report = board.read(job);
    const bool focused = focus_window(report.hwnd);
    log.write(std::string(describe(job)) + ": already running (pid " +
              std::to_string(launcher.pid(job)) + "), " +
              (focused ? "focused" : "could not raise window"));
  };

  const auto on_hotkey = [&](int id) {
    if (id == kHotkeyQuit) {
      keys.request_stop();
      return;
    }

    const Job job = id == kHotkeySingle ? Job::Single : Job::Interactive;

    if (launcher.busy(job)) {
      focus_running(job);
      return;
    }

    board.reset(job);
    logged_program[static_cast<int>(job)] = 0;

    std::string why;
    unsigned long long pid = 0;
    switch (launcher.launch(job, &pid, why)) {
    case LaunchResult::Started:
      log.write(std::string(describe(job)) + ": job started (pid " +
                std::to_string(pid) + ")");
      break;
    case LaunchResult::Busy:
      focus_running(job);
      break;
    case LaunchResult::Failed:
      log.write(std::string(describe(job)) + ": cannot start job: " + why);
      break;
    }
  };

  const auto waits = [&]() {
    std::vector<std::pair<UINT_PTR, HANDLE>> items;
    items.emplace_back(kTagJobI, launcher.handle(Job::Interactive));
    items.emplace_back(kTagJobS, launcher.handle(Job::Single));
    items.emplace_back(kTagSignalI, board.signal(Job::Interactive));
    items.emplace_back(kTagSignalS, board.signal(Job::Single));
    return items;
  };

  const auto on_wake = [&](UINT_PTR tag) {
    const bool is_job = tag == kTagJobI || tag == kTagJobS;
    const Job job = (tag == kTagJobI || tag == kTagSignalI) ? Job::Interactive
                                                            : Job::Single;
    const int index = static_cast<int>(job);

    const SlotReport report = board.read(job);
    const unsigned long long expected = launcher.pid(job);

    if (!is_job) {

      if (expected == 0 || report.job_pid != expected)
        return;
      if ((report.flags & kSlotHasProgram) != 0 && report.program_pid != 0 &&
          report.program_pid != logged_program[index]) {
        logged_program[index] = report.program_pid;
        log.write(std::string(describe(job)) + ": program started (pid " +
                  std::to_string(report.program_pid) + ")");
      }
      return;
    }

    unsigned long exit_code = 0;
    if (const HANDLE handle = launcher.handle(job); handle != nullptr)
      ::GetExitCodeProcess(handle, &exit_code);

    std::string line = std::string(describe(job)) + ": job finished (exit " +
                       std::to_string(exit_code);

    const bool reported =
        (report.flags & kSlotDone) != 0 && report.job_pid == expected;
    if (!reported) {

      line += ", no report)";
    } else {
      line += ')';
      if (job == Job::Interactive) {
        line += " input saved: ";
        line += (report.flags & kSlotSavedInput) != 0 ? "yes" : "no";
        line += ", output saved: ";
        line += (report.flags & kSlotSavedOutput) != 0 ? "yes" : "no";
      } else {
        line += " output saved: ";
        line += (report.flags & kSlotSavedOutput) != 0 ? "yes" : "no";
      }
    }

    log.write(line);
    logged_program[index] = 0;
    launcher.reap(job);
  };

  if (!keys.run(on_hotkey, waits, on_wake, error))
    std::cerr << color::err("[cg_t] ", error) << "\n";

  launcher.reap_all();
  remove_status(cfg);
  log.write("daemon: stopped");
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
        return run_job(Job::Interactive);
      if (mode == "s")
        return run_job(Job::Single);
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
