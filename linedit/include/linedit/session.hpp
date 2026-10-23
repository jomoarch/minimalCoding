#ifndef LINEDIT_SESSION_HPP
#define LINEDIT_SESSION_HPP

#include "linedit/key.hpp"

#include <string>
#include <string_view>
namespace linedit {

class Session {
public:
  Session() = default;
  ~Session();
  Session(Session &&other) noexcept;
  Session &operator=(Session &&other) noexcept;
  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;

  static bool open(Session &out, std::string &error,
                   Screen screen = Screen::Alternate);

  void close() noexcept;

  Size size() const;
  void write(std::string_view frame) const;

  void *input_handle() const noexcept { return input_; }
  void *output_handle() const noexcept { return output_; }

  bool read(Key &out) const;

  bool read_any(Key &out, char32_t &text) const;

  bool read_any(Key &out, char32_t &text, unsigned timeout_ms) const;

private:
  bool read_key(Key &out, bool raw, char32_t *text, unsigned timeout_ms) const;

  void *input_{nullptr};
  void *output_{nullptr};
  unsigned long saved_input_mode_{0};
  unsigned int saved_output_cp_{0};
  bool active_{false};
  bool alt_{false};
};

} // namespace linedit

#endif // LINEDIT_SESSION_HPP
