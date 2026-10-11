#include "app/app.hpp"

#include "app/modes.hpp"
#include "app/prompt.hpp"
#include "base/color.hpp"
#include "io/config.hpp"

#include <iostream>

namespace coding {
namespace app {

namespace {

bool load(AppConfig &out) {
  const ConfigResult r = load_config("config.toml");
  if (!r) {
    std::cerr << color::err("[config] ", r.message) << "\n";
    return false;
  }
  out = r.config;
  return true;
}

} // namespace

int run_single() {
  AppConfig cfg;
  if (!load(cfg))
    return 2;
  const int code = run_single_file(cfg);
  prompt::pause_if_needed(-1);
  return code;
}

int run_interactive() {
  AppConfig cfg;
  if (!load(cfg))
    return 2;
  const int code = run_interactive(cfg);
  prompt::pause_if_needed(-1);
  return code;
}

} // namespace app
} // namespace coding