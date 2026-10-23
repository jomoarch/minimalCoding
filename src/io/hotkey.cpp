#include "io/hotkey.hpp"

#include "base/text.hpp"

#include <cstdio>
#include <vector>
namespace coding {

namespace {

constexpr unsigned kVkTab = 0x09;
constexpr unsigned kVkReturn = 0x0D;
constexpr unsigned kVkPause = 0x13;
constexpr unsigned kVkCapsLock = 0x14;
constexpr unsigned kVkEscape = 0x1B;
constexpr unsigned kVkSpace = 0x20;
constexpr unsigned kVkPageUp = 0x21;
constexpr unsigned kVkPageDown = 0x22;
constexpr unsigned kVkEnd = 0x23;
constexpr unsigned kVkHome = 0x24;
constexpr unsigned kVkLeft = 0x25;
constexpr unsigned kVkUp = 0x26;
constexpr unsigned kVkRight = 0x27;
constexpr unsigned kVkDown = 0x28;
constexpr unsigned kVkPrintScreen = 0x2C;
constexpr unsigned kVkInsert = 0x2D;
constexpr unsigned kVkDelete = 0x2E;
constexpr unsigned kVkApps = 0x5D;
constexpr unsigned kVkBackspace = 0x08;
constexpr unsigned kVkF1 = 0x70;
constexpr unsigned kVkF24 = 0x87;
constexpr unsigned kVkNumLock = 0x90;
constexpr unsigned kVkScrollLock = 0x91;

struct NamedKey {
  const char *name;
  unsigned vk;
};

constexpr NamedKey kNamed[] = {
    {"backspace", kVkBackspace},
    {"bs", kVkBackspace},
    {"tab", kVkTab},
    {"enter", kVkReturn},
    {"return", kVkReturn},
    {"esc", kVkEscape},
    {"escape", kVkEscape},
    {"space", kVkSpace},
    {"insert", kVkInsert},
    {"ins", kVkInsert},
    {"delete", kVkDelete},
    {"del", kVkDelete},
    {"home", kVkHome},
    {"end", kVkEnd},
    {"pageup", kVkPageUp},
    {"pgup", kVkPageUp},
    {"pagedown", kVkPageDown},
    {"pgdn", kVkPageDown},
    {"left", kVkLeft},
    {"up", kVkUp},
    {"right", kVkRight},
    {"down", kVkDown},
    {"printscreen", kVkPrintScreen},
    {"prtsc", kVkPrintScreen},
    {"pause", kVkPause},
    {"capslock", kVkCapsLock},
    {"numlock", kVkNumLock},
    {"scrolllock", kVkScrollLock},
    {"apps", kVkApps},
    {"menu", kVkApps},
};

unsigned key_from_name(const std::string &name) {
  if (name.size() == 1) {
    const char c = name[0];
    if (c >= 'a' && c <= 'z')
      return static_cast<unsigned>(c - 'a' + 'A');
    if (c >= '0' && c <= '9')
      return static_cast<unsigned>(c);
    return 0;
  }

  if (name.size() >= 2 && name[0] == 'f') {
    int n = 0;
    bool ok = true;
    for (std::size_t i = 1; i < name.size(); ++i) {
      if (name[i] < '0' || name[i] > '9') {
        ok = false;
        break;
      }
      n = n * 10 + (name[i] - '0');
      if (n > 24) {
        ok = false;
        break;
      }
    }
    if (ok && n >= 1)
      return kVkF1 + static_cast<unsigned>(n - 1);
  }

  for (const NamedKey &entry : kNamed) {
    if (name == entry.name)
      return entry.vk;
  }
  return 0;
}

std::string name_from_key(unsigned vk) {
  if (vk >= 'A' && vk <= 'Z')
    return std::string(1, static_cast<char>(vk));
  if (vk >= '0' && vk <= '9')
    return std::string(1, static_cast<char>(vk));
  if (vk >= kVkF1 && vk <= kVkF24)
    return "F" + std::to_string(vk - kVkF1 + 1);

  for (const NamedKey &entry : kNamed) {
    if (entry.vk == vk) {
      return entry.name;
    }
  }

  char buffer[16]{};
  std::snprintf(buffer, sizeof(buffer), "0x%02X", vk);
  return buffer;
}

} // namespace

HotkeyResult parse_hotkey(std::string_view text) {
  HotkeyResult r;

  const std::string_view whole = text::trim(text);
  if (whole.empty()) {
    r.message = "hotkey is empty";
    return r;
  }

  bool have_key = false;
  unsigned vk = 0;
  unsigned mods = 0;

  std::size_t start = 0;
  for (;;) {
    const std::size_t plus = whole.find('+', start);
    const std::string_view raw = plus == std::string_view::npos
                                     ? whole.substr(start)
                                     : whole.substr(start, plus - start);
    const std::string token = text::to_lower(text::trim(raw));

    if (token.empty()) {
      r.message = "empty part in \"" + std::string(whole) + "\"";
      return r;
    }

    if (token == "ctrl" || token == "control") {
      mods |= kHotkeyCtrl;
    } else if (token == "alt") {
      mods |= kHotkeyAlt;
    } else if (token == "shift") {
      mods |= kHotkeyShift;
    } else if (token == "win" || token == "super" || token == "meta") {
      mods |= kHotkeyWin;
    } else {
      const unsigned found = key_from_name(token);
      if (found == 0) {
        r.message = "unknown key name: \"" + token + "\"";
        return r;
      }
      if (have_key) {
        r.message = "more than one key in \"" + std::string(whole) + "\"";
        return r;
      }
      have_key = true;
      vk = found;
    }

    if (plus == std::string_view::npos)
      break;
    start = plus + 1;
  }

  if (!have_key) {
    r.message = "no key in \"" + std::string(whole) + "\"";
    return r;
  }

  if (mods == 0) {
    r.message = "\"" + std::string(whole) +
                "\" has no modifier (Ctrl/Alt/Shift/Win); a bare key would "
                "capture global typing";
    return r;
  }

  if (mods == (kHotkeyCtrl | kHotkeyAlt) && vk == kVkDelete) {
    r.message = "Ctrl+Alt+Del is reserved by Windows and cannot be captured";
    return r;
  }

  r.success = true;
  r.message = "OK";
  r.key.mods = mods;
  r.key.vk = vk;
  return r;
}

std::string format_hotkey(const Hotkey &key) {
  std::vector<std::string> parts;
  if ((key.mods & kHotkeyCtrl) != 0)
    parts.emplace_back("Ctrl");
  if ((key.mods & kHotkeyAlt) != 0)
    parts.emplace_back("Alt");
  if ((key.mods & kHotkeyShift) != 0)
    parts.emplace_back("Shift");
  if ((key.mods & kHotkeyWin) != 0)
    parts.emplace_back("Win");
  parts.push_back(name_from_key(key.vk));

  std::string out;
  for (const std::string &part : parts) {
    if (!out.empty())
      out += '+';
    out += part;
  }
  return out;
}

} // namespace coding
