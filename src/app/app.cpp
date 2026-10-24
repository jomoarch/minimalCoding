#include "app/app.hpp"

#include "app/prompt.hpp"
#include "base/color.hpp"
#include "io/config.hpp"

#include <windows.h>

#include <iostream>

namespace coding {
namespace app {

namespace {

void use_utf8_console() noexcept { ::SetConsoleOutputCP(CP_UTF8); }

bool load(AppConfig &out) {
  const ConfigResult r = load_config("config.toml");
  if (!r) {
    std::cerr << color::err("[config] ", r.message) << "\n";
    return false;
  }
  out = r.config;
  return true;
}

ModeOutcome settle(const ModeOutcome &outcome) {
  if (outcome.wait_key) {
    if (prompt::interactive_stdin())
      prompt::wait_any_key();
  } else {
    prompt::pause_if_needed(-1);
  }
  return outcome;
}

} // namespace

ModeOutcome run_single() {
  use_utf8_console();
  color::install();
  AppConfig cfg;
  if (!load(cfg))
    return ModeOutcome{2, false, false, false};
  return settle(run_single_file(cfg));
}

ModeOutcome run_interactive() {
  use_utf8_console();
  color::install();
  AppConfig cfg;
  if (!load(cfg))
    return ModeOutcome{2, false, false, false};
  return settle(run_interactive(cfg));
}

} // namespace app
} // namespace coding
