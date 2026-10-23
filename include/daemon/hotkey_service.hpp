#ifndef HOTKEY_SERVICE_HPP
#define HOTKEY_SERVICE_HPP

#include "io/hotkey.hpp"

#include <windows.h>

#include <cstddef>
#include <functional>
#include <string>
#include <vector>
namespace coding {
namespace app {

struct HotkeySlot {
  int id{0};
  std::string label;
  std::string text;
  Hotkey key;
  bool enabled{false};
  bool registered{false};
  std::string why;
};

inline constexpr wchar_t kSingletonName[] =
    L"Local\\minimalCoding.cg_t.singleton";
inline constexpr wchar_t kStopEventName[] = L"Local\\minimalCoding.cg_t.stop";

class HotkeyService {
public:
  HotkeyService() = default;
  ~HotkeyService();
  HotkeyService(const HotkeyService &) = delete;
  HotkeyService &operator=(const HotkeyService &) = delete;

  bool open(std::string &error);

  bool add(const HotkeySlot &slot, std::string &why);

  void unregister_all() noexcept;

  using HotkeyFn = std::function<void(int id)>;

  using SlotsFn = std::function<std::vector<HANDLE>()>;
  using SlotDoneFn = std::function<void(std::size_t slot)>;

  bool run(const HotkeyFn &on_hotkey, const SlotsFn &slots,
           const SlotDoneFn &on_slot_done, std::string &error);

  void request_stop() noexcept;

  void dispatch(int id);

private:
  void pump_messages();

  HWND window_{nullptr};
  HANDLE stop_{nullptr};
  std::vector<int> registered_;
  HotkeyFn on_hotkey_;
};

} // namespace app
} // namespace coding

#endif // HOTKEY_SERVICE_HPP
