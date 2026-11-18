#ifndef LINEDIT_KEY_HPP
#define LINEDIT_KEY_HPP

#include <cstddef>
namespace linedit {

enum class Key {
  Left,
  Right,
  WordLeft,
  WordRight,
  Enter,
  ShiftEnter,
  Backspace,
  Delete,
  WordBackspace,
  WordDelete,
  Paste,
  Quit,
  EndOfInput,
  Resize,
  Text
};

struct Size {
  std::size_t columns{0};
  std::size_t rows{0};
};

inline constexpr unsigned kWaitForever = 0xFFFFFFFFu;

enum class Screen { Alternate, Normal };

} // namespace linedit

#endif // LINEDIT_KEY_HPP
