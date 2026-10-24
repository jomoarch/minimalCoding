#ifndef APP_HPP
#define APP_HPP

#include "app/modes.hpp"
namespace coding {
namespace app {

ModeOutcome run_single();
ModeOutcome run_interactive();

int run_daemon(int argc, char **argv);

} // namespace app
} // namespace coding

#endif // APP_HPP
