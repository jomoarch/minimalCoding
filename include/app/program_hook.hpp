#ifndef PROGRAM_HOOK_HPP
#define PROGRAM_HOOK_HPP

#include <functional>
namespace coding {
namespace app {

using ProgramStartedFn = std::function<void(unsigned long pid)>;

void set_program_started_hook(ProgramStartedFn fn);

void report_program_started(void *process_handle);

} // namespace app
} // namespace coding

#endif // PROGRAM_HOOK_HPP
