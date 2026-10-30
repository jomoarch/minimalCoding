#include "process/accounting.hpp"

#include "monitor/monitor.hpp"

#include <string>

namespace coding {

void fill_accounting(RunnerResult &out, void *process, void *job,
                     std::chrono::steady_clock::time_point started,
                     std::size_t memory_limit_bytes, bool timed_out) {
  const monitor::Sample s = monitor::sample(process, job, started);

  out.cpu_time = s.cpu_ms();
  out.wall_time = s.wall;
  out.memory_bytes = s.peak_bytes;
  out.exit_code = s.exit_code;

  if (s.alive) {
    out.status = RunnerStatus::SystemError;
    out.message = "the child is still running";
    return;
  }

  if (memory_limit_bytes > 0 && s.peak_bytes > memory_limit_bytes) {
    out.status = RunnerStatus::MemoryLimitExceeded;
    out.message = "Memory limit exceeded";
    return;
  }

  if (timed_out) {
    out.status = RunnerStatus::TimeLimitExceeded;
    out.message = "Time limit exceeded";
    return;
  }

  if (s.exit_code == 0) {
    out.status = RunnerStatus::Success;
    out.message = "Success";
  } else {
    out.status = RunnerStatus::RuntimeError;
    out.message.clear();
  }
}

} // namespace coding