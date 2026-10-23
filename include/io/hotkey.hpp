#ifndef HOTKEY_HPP
#define HOTKEY_HPP

#include "base/result.hpp"

#include <string>
#include <string_view>
namespace coding {

enum HotkeyMod : unsigned {
  kHotkeyAlt = 0x0001,
  kHotkeyCtrl = 0x0002,
  kHotkeyShift = 0x0004,
  kHotkeyWin = 0x0008,
};

inline constexpr unsigned kHotkeyNoRepeat = 0x4000;

struct Hotkey {
  unsigned mods{0};
  unsigned vk{0};

  bool valid() const noexcept { return vk != 0; }

  bool same_as(const Hotkey &other) const noexcept {
    return mods == other.mods && vk == other.vk;
  }
};

struct HotkeyResult : ResultBase {
  Hotkey key;
};

[[nodiscard]] HotkeyResult parse_hotkey(std::string_view text);

std::string format_hotkey(const Hotkey &key);

} // namespace coding

#endif // HOTKEY_HPP
