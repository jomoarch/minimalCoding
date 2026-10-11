#include "process/accounting.hpp"

#include "monitor/monitor.hpp"

#include <string>

namespace coding {

void fill_accounting(RunnerResult &out, void *process, void *job,
                     std::chrono::steady_clock::time_point started) {
  const monitor::Sample s = monitor::sample(process, job, started);

  out.cpu_time = s.cpu_ms();
  out.wall_time = s.wall;
  out.memory_bytes = s.peak_bytes;

  if (s.alive) {
    out.status = RunnerStatus::SystemError;
    out.message = "the child is still running";
    return;
  }

  if (s.exit_code == 0) {
    out.status = RunnerStatus::Success;
    out.message = "Success";
  } else {
    out.status = RunnerStatus::RuntimeError;
    out.message = "Exit code: " + std::to_string(s.exit_code);
  }
}

} // namespace coding