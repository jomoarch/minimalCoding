#ifndef SLOT_REPORT_HPP
#define SLOT_REPORT_HPP

#include "daemon/job_launcher.hpp"

#include <windows.h>

#include <string>
namespace coding {
namespace app {

struct SlotReport {
  unsigned long long job_pid{0};
  unsigned long long program_pid{0};
  unsigned long long hwnd{0};
  unsigned long flags{0};
  unsigned long exit_code{0};
};

enum SlotFlag : unsigned long {
  kSlotDone = 1u << 0,
  kSlotHasProgram = 1u << 1,
  kSlotSavedInput = 1u << 2,
  kSlotSavedOutput = 1u << 3,
};

class SlotBoard {
public:
  SlotBoard() = default;
  ~SlotBoard();
  SlotBoard(const SlotBoard &) = delete;
  SlotBoard &operator=(const SlotBoard &) = delete;

  bool open(std::string &error);

  void reset(Job job) noexcept;

  HANDLE signal(Job job) const;

  SlotReport read(Job job) const;

private:
  HANDLE map_[kJobCount]{nullptr, nullptr};
  SlotReport *view_[kJobCount]{nullptr, nullptr};
  HANDLE event_[kJobCount]{nullptr, nullptr};
};

class SlotPublisher {
public:
  SlotPublisher() = default;
  ~SlotPublisher();
  SlotPublisher(const SlotPublisher &) = delete;
  SlotPublisher &operator=(const SlotPublisher &) = delete;

  void open(Job job) noexcept;

  void publish_job(unsigned long long pid, unsigned long long hwnd);
  void publish_program(unsigned long long pid);
  void publish_done(unsigned long exit_code, bool saved_input,
                    bool saved_output);

private:
  void notify() noexcept;

  SlotReport *view_{nullptr};
  HANDLE event_{nullptr};
};

} // namespace app
} // namespace coding

#endif // SLOT_REPORT_HPP
