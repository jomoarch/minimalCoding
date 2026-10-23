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

JobLauncher::~JobLauncher() { reap_all(); }

bool JobLauncher::open(std::string &error) {
  exe_ = module_path();
  if (exe_.empty()) {
    error = "cannot determine my own path";
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

LaunchResult JobLauncher::launch(Job job, std::string &error) {
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

  const DWORD flags = CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT;
  if (!::CreateProcessW(exe_.c_str(), buffer.data(), nullptr, nullptr, FALSE,
                        flags, nullptr, nullptr, &startup, &info)) {
    error = win::last_error_string();
    return LaunchResult::Failed;
  }

  ::CloseHandle(info.hThread);
  jobs_[static_cast<int>(job)] = info.hProcess;
  return LaunchResult::Started;
}

void JobLauncher::reap(Job job) noexcept {
  HANDLE &handle = jobs_[static_cast<int>(job)];
  if (handle != nullptr) {
    ::CloseHandle(handle);
    handle = nullptr;
  }
}

void JobLauncher::reap_all() noexcept {
  for (int i = 0; i < kJobCount; ++i)
    reap(static_cast<Job>(i));
}

} // namespace app
} // namespace coding
