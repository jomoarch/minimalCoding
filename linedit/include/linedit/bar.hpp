#ifndef LINEDIT_BAR_HPP
#define LINEDIT_BAR_HPP

#include "linedit/key.hpp"
#include "linedit/session.hpp"

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>

namespace linedit {

class Editor;

class Bar {
public:
  Bar() = default;
  ~Bar();

  Bar(const Bar &) = delete;
  Bar &operator=(const Bar &) = delete;

  bool open(Session &session, std::string &error);
  void close() noexcept;

  bool active() const noexcept { return active_; }

  void write_output(std::string_view data);

  void set_input(std::string_view text, std::size_t cursor_bytes);
  void set_status(std::string_view text);

  void commit_text(std::string_view text);

  void set_compact(bool compact);

  void resize();

  void paint();

  bool input_visible() const;
  void reveal_input();

private:
  struct Pos {
    int row{0};
    int column{0};
  };

  struct Layout {
    bool valid{false};
    std::size_t input_rows{0};
    std::size_t view_start{0};
    std::size_t caret_row{0};
    std::size_t caret_column{0};
    int bar_top{0};
    int status_row{0};
    int bar_rows{0};
  };

  bool place(const Pos &at) const;
  void write_bytes(std::string_view bytes) const;
  bool cursor_now(Pos &out) const;

  Layout layout() const;
  void ensure_output_room(int bar_rows);
  void erase_bar();
  void draw_bar(const Layout &laid);
  void place_caret(const Layout &laid);

  void hide_cursor();
  void show_cursor();
  void recover_anchor();
  bool bar_visible() const;
  bool skip_for_selection() const;
  bool settling() const noexcept;
  void flush_pending();

  void *input_{nullptr};
  void *output_{nullptr};
  bool active_{false};
  std::size_t rows_{0};
  std::size_t columns_{0};
  int buffer_width_{0};
  bool layout_lost_{false};
  bool settling_{false};
  bool width_changed_{false};
  std::chrono::steady_clock::time_point settle_until_{};

  Pos anchor_{};
  std::string pending_;
  std::string input_text_;
  std::size_t input_cursor_bytes_{0};
  std::string status_text_;
  bool compact_{false};

  bool painted_{false};
  std::string painted_input_;
  std::size_t painted_cursor_{0};
  std::string painted_status_;
  bool painted_compact_{false};
  int painted_bar_top_{-1};
  int painted_bar_rows_{0};
  bool cursor_hidden_{false};
};

} // namespace linedit

#endif // LINEDIT_BAR_HPP
