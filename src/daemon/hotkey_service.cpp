#include "daemon/hotkey_service.hpp"

#include "base/win_error.hpp"

namespace coding {
namespace app {

namespace {

constexpr wchar_t kWindowClass[] = L"minimalCoding.cg_t.hidden";

HotkeyService *g_active = nullptr;

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam,
                             LPARAM lparam) {
  (void)window;
  (void)lparam;

  switch (message) {
  case WM_HOTKEY:
    if (g_active != nullptr)
      g_active->dispatch(static_cast<int>(wparam));
    return 0;
  case WM_QUERYENDSESSION:
    return TRUE;
  case WM_ENDSESSION:
  case WM_CLOSE:
    if (g_active != nullptr)
      g_active->request_stop();
    return 0;
  default:
    return ::DefWindowProcW(window, message, wparam, lparam);
  }
}

BOOL WINAPI console_handler(DWORD type) {
  switch (type) {
  case CTRL_C_EVENT:
  case CTRL_BREAK_EVENT:
  case CTRL_CLOSE_EVENT:
  case CTRL_LOGOFF_EVENT:
  case CTRL_SHUTDOWN_EVENT:
    if (g_active != nullptr)
      g_active->request_stop();
    return TRUE;
  default:
    return FALSE;
  }
}

} // namespace

HotkeyService::~HotkeyService() {

  ::SetConsoleCtrlHandler(console_handler, FALSE);
  unregister_all();
  if (stop_ != nullptr) {
    ::CloseHandle(stop_);
    stop_ = nullptr;
  }
  if (window_ != nullptr) {
    ::DestroyWindow(window_);
    window_ = nullptr;
  }
  ::UnregisterClassW(kWindowClass, ::GetModuleHandleW(nullptr));
  if (g_active == this)
    g_active = nullptr;
}

bool HotkeyService::open(std::string &error) {
  if (window_ != nullptr)
    return true;

  HINSTANCE instance = ::GetModuleHandleW(nullptr);

  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = window_proc;
  window_class.hInstance = instance;
  window_class.lpszClassName = kWindowClass;

  ::RegisterClassExW(&window_class);

  window_ = ::CreateWindowExW(0, kWindowClass, L"cg_t", 0, 0, 0, 0, 0,
                              HWND_MESSAGE, nullptr, instance, nullptr);
  if (window_ == nullptr) {
    error = "cannot create the message window: " + win::last_error_string();
    return false;
  }

  stop_ = ::CreateEventW(nullptr, TRUE, FALSE, kStopEventName);
  if (stop_ == nullptr) {
    error = "cannot create the stop event: " + win::last_error_string();
    ::DestroyWindow(window_);
    window_ = nullptr;
    return false;
  }

  g_active = this;
  ::SetConsoleCtrlHandler(console_handler, TRUE);
  return true;
}

bool HotkeyService::add(const HotkeySlot &slot, std::string &why) {
  if (window_ == nullptr || !slot.enabled)
    return false;

  if (::RegisterHotKey(window_, slot.id, slot.key.mods | kHotkeyNoRepeat,
                       slot.key.vk)) {
    registered_.push_back(slot.id);
    return true;
  }

  const DWORD code = ::GetLastError();
  why = code == ERROR_HOTKEY_ALREADY_REGISTERED
            ? std::string("already taken by another program")
            : win::error_string(code);
  return false;
}

void HotkeyService::unregister_all() noexcept {
  if (window_ != nullptr) {
    for (const int id : registered_)
      ::UnregisterHotKey(window_, id);
  }
  registered_.clear();
}

void HotkeyService::request_stop() noexcept {
  if (stop_ != nullptr)
    ::SetEvent(stop_);
}

void HotkeyService::dispatch(int id) {
  if (on_hotkey_)
    on_hotkey_(id);
}

void HotkeyService::pump_messages() {
  MSG message{};
  while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
    ::TranslateMessage(&message);
    ::DispatchMessageW(&message);
  }
}

bool HotkeyService::run(const HotkeyFn &on_hotkey, const SlotsFn &slots,
                        const SlotDoneFn &on_slot_done, std::string &error) {
  on_hotkey_ = on_hotkey;

  bool ok = true;
  for (;;) {

    const std::vector<HANDLE> slot_handles = slots();

    std::vector<HANDLE> waits;
    waits.reserve(1 + slot_handles.size());
    waits.push_back(stop_);
    for (const HANDLE handle : slot_handles) {
      if (handle != nullptr)
        waits.push_back(handle);
    }

    const DWORD result =
        ::MsgWaitForMultipleObjects(static_cast<DWORD>(waits.size()),
                                    waits.data(), FALSE, INFINITE, QS_ALLINPUT);

    if (result == WAIT_OBJECT_0)
      break;

    if (result > WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + waits.size()) {
      const std::size_t index = result - WAIT_OBJECT_0 - 1;

      std::size_t seen = 0;
      for (std::size_t slot = 0; slot < slot_handles.size(); ++slot) {
        if (slot_handles[slot] == nullptr)
          continue;
        if (seen == index) {
          if (on_slot_done)
            on_slot_done(slot);
          break;
        }
        ++seen;
      }
      continue;
    }

    if (result == WAIT_OBJECT_0 + waits.size()) {
      pump_messages();
      continue;
    }

    if (result == WAIT_FAILED) {
      error = "wait failed: " + win::last_error_string();
      ok = false;
      break;
    }
  }

  on_hotkey_ = nullptr;
  return ok;
}

} // namespace app
} // namespace coding
