#include "daemon/focus_window.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
namespace coding {
namespace app {

namespace {

bool bring_to_front(HWND window) {
  if (::IsIconic(window) != FALSE)
    ::ShowWindow(window, SW_RESTORE);

  ::SetForegroundWindow(window);
  if (::GetForegroundWindow() == window)
    return true;

  const DWORD target_thread = ::GetWindowThreadProcessId(window, nullptr);
  const DWORD self_thread = ::GetCurrentThreadId();
  if (target_thread != 0 && target_thread != self_thread &&
      ::AttachThreadInput(self_thread, target_thread, TRUE) != FALSE) {
    ::BringWindowToTop(window);
    ::SetForegroundWindow(window);
    ::AttachThreadInput(self_thread, target_thread, FALSE);
  }

  return ::GetForegroundWindow() == window;
}

void flash(HWND window) {
  FLASHWINFO info{};
  info.cbSize = sizeof(info);
  info.hwnd = window;
  info.dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG;
  info.uCount = 3;
  ::FlashWindowEx(&info);
}

} // namespace

bool focus_window(unsigned long long hwnd_value) {
  if (hwnd_value == 0)
    return false;

  HWND window = reinterpret_cast<HWND>(static_cast<UINT_PTR>(hwnd_value));

  if (::IsWindow(window) == FALSE)
    return false;

  if (bring_to_front(window))
    return true;

  flash(window);
  return false;
}

} // namespace app
} // namespace coding
