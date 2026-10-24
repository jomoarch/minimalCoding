#include "daemon/slot_report.hpp"

#include "base/win_error.hpp"

#include <atomic>
#include <cstring>
namespace coding {
namespace app {

namespace {

std::wstring slot_object(Job job, const wchar_t *suffix) {
  std::wstring name = L"Local\\minimalCoding.cg_t.slot.";
  name += job == Job::Interactive ? L"i." : L"s.";
  name += suffix;
  return name;
}

void publish_fence() noexcept {
  std::atomic_thread_fence(std::memory_order_release);
}

void read_fence() noexcept {
  std::atomic_thread_fence(std::memory_order_acquire);
}

} // namespace

SlotBoard::~SlotBoard() {
  for (int i = 0; i < kJobCount; ++i) {
    if (view_[i] != nullptr)
      ::UnmapViewOfFile(view_[i]);
    if (map_[i] != nullptr)
      ::CloseHandle(map_[i]);
    if (event_[i] != nullptr)
      ::CloseHandle(event_[i]);
  }
}

bool SlotBoard::open(std::string &error) {
  for (int i = 0; i < kJobCount; ++i) {
    const Job job = static_cast<Job>(i);

    const std::wstring data_name = slot_object(job, L"data");
    map_[i] =
        ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                             sizeof(SlotReport), data_name.c_str());
    if (map_[i] == nullptr) {
      error = "cannot create the slot mapping: " + win::last_error_string();
      return false;
    }
    view_[i] = static_cast<SlotReport *>(::MapViewOfFile(
        map_[i], FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SlotReport)));
    if (view_[i] == nullptr) {
      error = "cannot map the slot memory: " + win::last_error_string();
      return false;
    }

    *view_[i] = SlotReport{};

    const std::wstring event_name = slot_object(job, L"event");

    event_[i] = ::CreateEventW(nullptr, FALSE, FALSE, event_name.c_str());
    if (event_[i] == nullptr) {
      error = "cannot create the slot event: " + win::last_error_string();
      return false;
    }
  }
  return true;
}

void SlotBoard::reset(Job job) noexcept {
  const int i = static_cast<int>(job);
  if (view_[i] == nullptr)
    return;
  *view_[i] = SlotReport{};
  publish_fence();

  if (event_[i] != nullptr)
    ::ResetEvent(event_[i]);
}

HANDLE SlotBoard::signal(Job job) const {
  return event_[static_cast<int>(job)];
}

SlotReport SlotBoard::read(Job job) const {
  SlotReport copy;
  const int i = static_cast<int>(job);
  if (view_[i] == nullptr)
    return copy;
  read_fence();
  copy = *view_[i];
  return copy;
}

SlotPublisher::~SlotPublisher() {
  if (view_ != nullptr)
    ::UnmapViewOfFile(view_);
  if (event_ != nullptr)
    ::CloseHandle(event_);
}

void SlotPublisher::open(Job job) noexcept {
  const std::wstring data_name = slot_object(job, L"data");
  HANDLE map =
      ::OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, data_name.c_str());
  if (map == nullptr)
    return;

  view_ = static_cast<SlotReport *>(
      ::MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SlotReport)));
  ::CloseHandle(map);
  if (view_ == nullptr)
    return;

  const std::wstring event_name = slot_object(job, L"event");
  event_ = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name.c_str());
}

void SlotPublisher::notify() noexcept {
  publish_fence();
  if (event_ != nullptr)
    ::SetEvent(event_);
}

void SlotPublisher::publish_job(unsigned long long pid,
                                unsigned long long hwnd) {
  if (view_ == nullptr)
    return;
  view_->job_pid = pid;
  view_->hwnd = hwnd;
  view_->flags = 0;
  view_->program_pid = 0;
  view_->exit_code = 0;
  notify();
}

void SlotPublisher::publish_program(unsigned long long pid) {
  if (view_ == nullptr || pid == 0)
    return;
  view_->program_pid = pid;
  view_->flags |= kSlotHasProgram;
  notify();
}

void SlotPublisher::publish_done(unsigned long exit_code, bool saved_input,
                                 bool saved_output) {
  if (view_ == nullptr)
    return;
  view_->exit_code = exit_code;
  view_->flags |= kSlotDone;
  if (saved_input)
    view_->flags |= kSlotSavedInput;
  if (saved_output)
    view_->flags |= kSlotSavedOutput;
  notify();
}

} // namespace app
} // namespace coding
