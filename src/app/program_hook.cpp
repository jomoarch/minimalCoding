#include "app/program_hook.hpp"

#include <windows.h>

#include <utility>
namespace coding {
namespace app {

namespace {

ProgramStartedFn g_hook;

} // namespace

void set_program_started_hook(ProgramStartedFn fn) { g_hook = std::move(fn); }

void report_program_started(void *process_handle) {
  if (!g_hook || process_handle == nullptr)
    return;
  const DWORD pid = ::GetProcessId(static_cast<HANDLE>(process_handle));
  if (pid != 0)
    g_hook(static_cast<unsigned long>(pid));
}

} // namespace app
} // namespace coding
