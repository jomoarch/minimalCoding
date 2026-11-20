#ifndef JOB_LAUNCHER_HPP
#define JOB_LAUNCHER_HPP

#include "base/handle.hpp"

#include <windows.h>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>
namespace coding {
namespace app {

enum class Job { Interactive = 0, Single = 1, Batch = 2 };

inline constexpr int kJobCount = 3;

enum class LaunchResult {
  Started,
  Busy,
  Failed,
};

class JobLauncher {
public:
  JobLauncher() = default;
  ~JobLauncher();
  JobLauncher(const JobLauncher &) = delete;
  JobLauncher &operator=(const JobLauncher &) = delete;

  bool open(std::string &error);

  bool busy(Job job) const;

  LaunchResult launch(Job job, unsigned long long *pid, std::string &error);

  HANDLE handle(Job job) const;

  unsigned long long pid(Job job) const;

  void reap(Job job) noexcept;

  void reap_all() noexcept;

  void shutdown() noexcept;

private:
  std::wstring exe_;
  HandleGuard job_;
  HANDLE jobs_[kJobCount]{nullptr, nullptr, nullptr};
  unsigned long long pids_[kJobCount]{0, 0, 0};
};

} // namespace app
} // namespace coding

#endif // JOB_LAUNCHER_HPP
