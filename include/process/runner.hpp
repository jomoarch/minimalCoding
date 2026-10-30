#ifndef RUNNER_HPP
#define RUNNER_HPP

#include "process/status.hpp"

#include <string>
#include <cstddef>
#include <filesystem>
#include <chrono>
namespace coding {

struct [[nodiscard]] RunnerResult {
  RunnerStatus status{RunnerStatus::SystemError};
  std::chrono::milliseconds cpu_time{0};
  std::chrono::milliseconds wall_time{0};
  std::size_t memory_bytes{0};
  unsigned long exit_code{0};
  std::string message;

  bool ok() const noexcept { return status == RunnerStatus::Success; }
  explicit operator bool() const noexcept {
    return status == RunnerStatus::Success;
  }
};

} // namespace coding

#endif // RUNNER_HPP