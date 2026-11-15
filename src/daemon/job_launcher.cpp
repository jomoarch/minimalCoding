#include "daemon/job_launcher.hpp"

#include "base/win_error.hpp"

#include <vector>
namespace coding {
namespace app {

namespace {

std::wstring module_path() {
  std::wstring buffer(MAX_PATH, L'\0');
  for (;;) {
    const DWORD n = ::GetModuleFileNameW(nullptr, buffer.data(),
                                         static_cast<DWORD>(buffer.size()));
    if (n == 0)
      return {};
    if (n < buffer.size()) {
      buffer.resize(n);
      return buffer;
    }
    if (buffer.size() >= 32768)
      return {};
    buffer.resize(buffer.size() * 2);
  }
}

} // namespace

JobLauncher::~JobLauncher() { shutdown(); }

bool JobLauncher::open(std::string &error) {
  exe_ = module_path();
  if (exe_.empty()) {
    error = "cannot determine my own path";
    return false;
  }

  job_.reset(::CreateJobObjectW(nullptr, nullptr));
  if (!job_.valid()) {
    error = "CreateJobObjectW failed: " + win::last_error_string();
    return false;
  }

  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!::SetInformationJobObject(job_.get(), JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits))) {
    error = "SetInformationJobObject failed: " + win::last_error_string();
    return false;
  }
  return true;
}

bool JobLauncher::busy(Job job) const {
  const HANDLE handle = jobs_[static_cast<int>(job)];
  if (handle == nullptr)
    return false;
  DWORD code = 0;
  if (!::GetExitCodeProcess(handle, &code))
    return false;
  return code == STILL_ACTIVE;
}

HANDLE JobLauncher::handle(Job job) const {
  return jobs_[static_cast<int>(job)];
}

unsigned long long JobLauncher::pid(Job job) const {
  return pids_[static_cast<int>(job)];
}

LaunchResult JobLauncher::launch(Job job, unsigned long long *pid,
                                 std::string &error) {
  if (busy(job))
    return LaunchResult::Busy;
  reap(job);

  std::wstring command = L"\"" + exe_ + L"\" --job ";
  command += job == Job::Interactive ? L"i" : L"s";

  std::vector<wchar_t> buffer(command.begin(), command.end());
  buffer.push_back(L'\0');

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION info{};

  const DWORD flags =
      CREATE_SUSPENDED | CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT;
  if (!::CreateProcessW(exe_.c_str(), buffer.data(), nullptr, nullptr, FALSE,
                        flags, nullptr, nullptr, &startup, &info)) {
    error = win::last_error_string();
    return LaunchResult::Failed;
  }

  if (!::AssignProcessToJobObject(job_.get(), info.hProcess)) {
    error = "AssignProcessToJobObject failed: " + win::last_error_string();
    ::TerminateProcess(info.hProcess, 1);
    ::CloseHandle(info.hThread);
    ::CloseHandle(info.hProcess);
    return LaunchResult::Failed;
  }

  ::ResumeThread(info.hThread);
  ::CloseHandle(info.hThread);
  jobs_[static_cast<int>(job)] = info.hProcess;
  pids_[static_cast<int>(job)] = info.dwProcessId;
  if (pid != nullptr)
    *pid = info.dwProcessId;
  return LaunchResult::Started;
}

void JobLauncher::reap(Job job) noexcept {
  HANDLE &handle = jobs_[static_cast<int>(job)];
  if (handle != nullptr) {
    ::CloseHandle(handle);
    handle = nullptr;
  }
  pids_[static_cast<int>(job)] = 0;
}

void JobLauncher::reap_all() noexcept {
  for (int i = 0; i < kJobCount; ++i)
    reap(static_cast<Job>(i));
}

void JobLauncher::shutdown() noexcept {

  job_.reset();
  reap_all();
}

} // namespace app
} // namespace coding
