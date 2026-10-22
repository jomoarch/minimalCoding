#ifndef MODES_HPP
#define MODES_HPP

#include "io/config.hpp"
namespace coding {

struct ModeOutcome {
  int code{0};

  bool wait_key{false};
};

ModeOutcome run_interactive(const AppConfig &cfg);
ModeOutcome run_single_file(const AppConfig &cfg);

} // namespace coding

#endif // MODES_HPP
