#include "linedit/bar.hpp"

#include "linedit/editor.hpp"

#include <windows.h>

#include <algorithm>
#include <string>

namespace linedit {

namespace {

constexpr std::size_t kMinRows = 4;
constexpr std::size_t kMinColumns = 12;
constexpr std::size_t kMaxPending = 64u << 20;

void raw_write(HANDLE h, std::string_view bytes) {
  std::size_t done = 0;
  while (done < bytes.size()) {
    const DWORD want = static_cast<DWORD>(
        std::min<std::size_t>(bytes.size() - done, 1u << 20));
    DWORD wrote = 0;
    if (!WriteFile(h, bytes.data() + done, want, &wrote, nullptr) || wrote == 0)
      return;
    done += wrote;
  }
}

} // namespace

Bar::~Bar() { close(); }

bool Bar::open(Session &session, std::string &error) {
  if (active_)
    return true;

  input_ = session.input_handle();
  output_ = session.output_handle();
  if (input_ == nullptr || output_ == nullptr) {
    error = "the session has no console handles";
    return false;
  }

  const Size size = session.size();
  if (size.rows < kMinRows || size.columns < kMinColumns) {
    error = "the terminal is too small for a status line";
    return false;
  }
  rows_ = size.rows;
  columns_ = size.columns;

  CONSOLE_SCREEN_BUFFER_INFO info{};
  if (GetConsoleScreenBufferInfo(static_cast<HANDLE>(output_), &info))
    buffer_width_ = info.dwSize.X;

  Pos at{};
  if (!cursor_now(at)) {
    error = "cannot read the cursor position";
    return false;
  }
  anchor_ = at;
  active_ = true;
  paint();
  return true;
}

void Bar::close() noexcept {
  if (!active_)
    return;
  if (!pending_.empty())
    flush_pending();
  erase_ours();
  active_ = false;
  place(anchor_);
  show_cursor();
}

void Bar::place(const Pos &at) const {
  CONSOLE_SCREEN_BUFFER_INFO info{};
  if (!GetConsoleScreenBufferInfo(static_cast<HANDLE>(output_), &info))
    return;
  if (at.row < 0 || at.row >= info.dwSize.Y)
    return;
  const int column =
      std::min<int>(std::max<int>(0, at.column), info.dwSize.X - 1);
  const COORD target{static_cast<SHORT>(column), static_cast<SHORT>(at.row)};
  SetConsoleCursorPosition(static_cast<HANDLE>(output_), target);
}

void Bar::write_bytes(std::string_view bytes) const {
  raw_write(static_cast<HANDLE>(output_), bytes);
}

bool Bar::cursor_now(Pos &out) const {
  CONSOLE_SCREEN_BUFFER_INFO info{};
  if (!GetConsoleScreenBufferInfo(static_cast<HANDLE>(output_), &info))
    return false;
  out.row = info.dwCursorPosition.Y;
  out.column = info.dwCursorPosition.X;
  return true;
}

bool Bar::skip_for_selection() const {
  CONSOLE_SELECTION_INFO info{};
  if (!GetConsoleSelectionInfo(&info))
    return false;
  return (info.dwFlags & CONSOLE_SELECTION_IN_PROGRESS) != 0;
}

void Bar::erase_ours() {
  if (!active_)
    return;

  CONSOLE_SCREEN_BUFFER_INFO info{};
  if (!GetConsoleScreenBufferInfo(static_cast<HANDLE>(output_), &info))
    return;

  const SHORT width = info.dwSize.X;
  DWORD written = 0;
  for (int i = 0; i < 2; ++i) {
    const int row = anchor_.row + i;
    if (row < 0 || row >= info.dwSize.Y)
      continue;
    const int from = i == 0 ? anchor_.column : 0;
    if (from >= width)
      continue;
    const COORD at{static_cast<SHORT>(from), static_cast<SHORT>(row)};
    FillConsoleOutputCharacterW(static_cast<HANDLE>(output_), L' ',
                                static_cast<DWORD>(width - from), at, &written);
    FillConsoleOutputAttribute(static_cast<HANDLE>(output_), info.wAttributes,
                               static_cast<DWORD>(width - from), at, &written);
  }
}

void Bar::hide_cursor() {
  if (cursor_hidden_)
    return;
  cursor_hidden_ = true;
  write_bytes("\x1b[?25l");
}

void Bar::show_cursor() {
  if (!cursor_hidden_)
    return;
  cursor_hidden_ = false;
  write_bytes("\x1b[?25h");
}

void Bar::refresh_geometry(std::size_t window_rows, std::size_t window_columns,
                           int buffer_width) {
  rows_ = window_rows;
  columns_ = window_columns;

  if (buffer_width_ != 0 && buffer_width != buffer_width_)
    layout_lost_ = true;
  buffer_width_ = buffer_width;
}

void Bar::append_fresh() {
  Pos at{};
  if (!cursor_now(at))
    return;
  place(at);
  write_bytes("\r\n");

  Pos after{};
  if (!cursor_now(after))
    return;
  anchor_ = after;
  painted_ = false;
}

bool Bar::bar_visible() const {
  CONSOLE_SCREEN_BUFFER_INFO info{};
  if (!GetConsoleScreenBufferInfo(static_cast<HANDLE>(output_), &info))
    return true;

  return anchor_.row >= info.srWindow.Top &&
         anchor_.row <= info.srWindow.Bottom;
}

void Bar::flush_pending() {
  if (pending_.empty())
    return;

  hide_cursor();
  erase_ours();
  place(anchor_);
  write_bytes(pending_);
  pending_.clear();
  painted_ = false;

  Pos after{};
  if (cursor_now(after)) {
    anchor_.row = std::max(0, after.row);
    anchor_.column = std::max(0, after.column);
  }
}

void Bar::write_output(std::string_view data) {
  if (!active_ || data.empty())
    return;

  if (skip_for_selection() || !bar_visible()) {
    if (pending_.size() + data.size() <= kMaxPending)
      pending_.append(data);
    painted_ = false;
    return;
  }

  hide_cursor();

  erase_ours();
  place(anchor_);
  write_bytes(data);

  painted_ = false;

  Pos after{};
  if (cursor_now(after)) {
    anchor_.row = std::max(0, after.row);
    anchor_.column = std::max(0, after.column);
  }
  paint();
}

void Bar::set_input(std::string_view text, std::size_t cursor_bytes) {
  input_text_.assign(text);
  input_cursor_bytes_ = cursor_bytes;
}

void Bar::set_status(std::string_view text) { status_text_.assign(text); }

void Bar::commit_text(std::string_view text) {
  if (!active_ || text.empty())
    return;

  if (skip_for_selection() || !bar_visible()) {
    if (pending_.size() + text.size() + 1 <= kMaxPending) {
      pending_.append(text);
      if (text.back() != '\n')
        pending_.push_back('\n');
    }
    input_text_.clear();
    input_cursor_bytes_ = 0;
    painted_ = false;
    return;
  }

  hide_cursor();
  place(anchor_);
  write_bytes(text);
  if (text.back() != '\n')
    write_bytes("\r\n");

  Pos after{};
  if (cursor_now(after)) {
    anchor_.row = std::max(0, after.row);
    anchor_.column = std::max(0, after.column);
  } else {
    anchor_.column = 0;
  }

  input_text_.clear();
  input_cursor_bytes_ = 0;
  painted_ = false;
  paint();
}

void Bar::resize() {
  if (!active_)
    return;
  painted_ = false;
  paint();
}

void Bar::ensure_room(std::size_t echo_rows) {
  const int need = static_cast<int>(echo_rows) + 1;
  CONSOLE_SCREEN_BUFFER_INFO info{};
  if (!GetConsoleScreenBufferInfo(static_cast<HANDLE>(output_), &info))
    return;

  const int bottom = info.srWindow.Bottom;
  const int last = anchor_.row + need - 1;
  if (last <= bottom && last < info.dwSize.Y)
    return;

  erase_ours();
  place(Pos{bottom, 0});
  int push = last - bottom;
  if (push < 1)
    push = 1;
  for (int i = 0; i < push; ++i)
    write_bytes("\r\n");

  CONSOLE_SCREEN_BUFFER_INFO after{};
  if (GetConsoleScreenBufferInfo(static_cast<HANDLE>(output_), &after)) {
    anchor_.row = after.srWindow.Bottom - need + 1;
    buffer_width_ = after.dwSize.X;
  } else {
    anchor_.row += push;
  }
  if (anchor_.row < 0)
    anchor_.row = 0;
  anchor_.column = 0;
  painted_ = false;
}

void Bar::paint() {
  if (!active_)
    return;
  if (skip_for_selection())
    return;

  HANDLE h = static_cast<HANDLE>(output_);
  CONSOLE_SCREEN_BUFFER_INFO info{};
  if (!GetConsoleScreenBufferInfo(h, &info))
    return;

  refresh_geometry(
      static_cast<std::size_t>(info.srWindow.Bottom - info.srWindow.Top + 1),
      static_cast<std::size_t>(info.srWindow.Right - info.srWindow.Left + 1),
      info.dwSize.X);

  if (!bar_visible()) {

    painted_ = false;
    return;
  }

  if (layout_lost_) {
    layout_lost_ = false;
    append_fresh();
    if (!GetConsoleScreenBufferInfo(h, &info))
      return;
    buffer_width_ = info.dwSize.X;
  }

  if (!pending_.empty())
    flush_pending();

  const auto room_now = [&]() -> std::size_t {
    return columns_ > static_cast<std::size_t>(anchor_.column)
               ? columns_ - static_cast<std::size_t>(anchor_.column)
               : 0;
  };

  Layout laid =
      layout_text(input_text_, input_cursor_bytes_, room_now(), columns_);
  ensure_room(laid.rows.size());
  laid = layout_text(input_text_, input_cursor_bytes_, room_now(), columns_);

  const int status_row = anchor_.row + static_cast<int>(laid.rows.size());

  const bool echo_changed = !painted_ || painted_input_ != input_text_ ||
                            painted_cursor_ != input_cursor_bytes_;
  const bool status_changed = !painted_ || painted_status_ != status_text_ ||
                              painted_status_row_ != status_row;
  if (painted_ && !echo_changed && !status_changed)
    return;

  hide_cursor();

  if (echo_changed) {
    for (std::size_t i = 0; i < laid.rows.size(); ++i) {
      place(
          Pos{anchor_.row + static_cast<int>(i), i == 0 ? anchor_.column : 0});
      write_bytes(laid.rows[i]);
      write_bytes("\x1b[K");
    }
  }

  if (status_changed) {
    place(Pos{status_row, 0});
    write_bytes("\x1b[7m");
    write_bytes(status_text_);
    write_bytes("\x1b[K\x1b[0m");
  }

  const std::size_t area_rows = laid.rows.size() + 1;
  if (painted_ && painted_area_rows_ > area_rows) {
    for (std::size_t r = area_rows; r < painted_area_rows_; ++r) {
      place(Pos{anchor_.row + static_cast<int>(r), 0});
      write_bytes("\x1b[K");
    }
  }

  place(Pos{anchor_.row + static_cast<int>(laid.caret_row),
            static_cast<int>(laid.caret_column)});
  show_cursor();

  painted_ = true;
  painted_input_ = input_text_;
  painted_cursor_ = input_cursor_bytes_;
  painted_status_ = status_text_;
  painted_status_row_ = status_row;
  painted_area_rows_ = area_rows;
}

} // namespace linedit
