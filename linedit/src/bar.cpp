#include "linedit/bar.hpp"

#include "base/text.hpp"
#include "linedit/editor.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <string>

namespace linedit {

namespace {

constexpr std::size_t kMinRows = 4;
constexpr std::size_t kMinColumns = 12;
constexpr std::size_t kMaxPending = 64u << 20;
constexpr std::size_t kMaxInputRows = 12;
constexpr std::size_t kPromptColumns = 2; // "> "
constexpr auto kSettleDelay = std::chrono::milliseconds(200);

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

std::string clip_width(std::string_view text, std::size_t width) {
  if (width == 0)
    return {};
  if (coding::text::display_width(text) <= width)
    return std::string(text);

  std::string cut;
  std::size_t i = 0;
  while (i < text.size()) {
    const coding::text::CharWidth w = coding::text::measure(text, i);
    if (w.bytes == 0 || coding::text::display_width(cut) + w.columns > width)
      break;
    cut.append(text, i, w.bytes);
    i += w.bytes;
  }
  return cut;
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
    error = "the terminal is too small for the input line";
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
  erase_bar();
  active_ = false;
  place(anchor_);
  show_cursor();
}

bool Bar::place(const Pos &at) const {
  CONSOLE_SCREEN_BUFFER_INFO info{};
  if (!GetConsoleScreenBufferInfo(static_cast<HANDLE>(output_), &info))
    return false;
  if (at.row < 0 || at.row >= info.dwSize.Y)
    return false;
  const int column =
      std::min<int>(std::max<int>(0, at.column), info.dwSize.X - 1);
  const COORD target{static_cast<SHORT>(column), static_cast<SHORT>(at.row)};
  return SetConsoleCursorPosition(static_cast<HANDLE>(output_), target) != 0;
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

bool Bar::settling() const noexcept { return settling_; }

Bar::Layout Bar::layout() const {
  Layout result;

  CONSOLE_SCREEN_BUFFER_INFO info{};
  if (!GetConsoleScreenBufferInfo(static_cast<HANDLE>(output_), &info))
    return result;

  const int window_rows = info.srWindow.Bottom - info.srWindow.Top + 1;
  const int window_columns = info.srWindow.Right - info.srWindow.Left + 1;
  if (window_rows < static_cast<int>(kMinRows) ||
      window_columns < static_cast<int>(kMinColumns))
    return result;

  const std::size_t columns = static_cast<std::size_t>(window_columns);
  std::size_t input_rows = 0;
  std::size_t view_start = 0;
  std::size_t caret_row = 0;
  std::size_t caret_column = 0;

  if (!compact_) {
    const std::size_t first_room =
        columns > kPromptColumns ? columns - kPromptColumns : 1;
    const linedit::Layout text = linedit::layout_text(
        input_text_, input_cursor_bytes_, first_room, columns);

    std::size_t max_input =
        window_rows > 4 ? static_cast<std::size_t>(window_rows - 3) : 1;
    if (max_input > kMaxInputRows)
      max_input = kMaxInputRows;
    if (max_input < 1)
      max_input = 1;

    const std::size_t total = text.rows.size();
    const std::size_t view = std::min(total, max_input);
    std::size_t start = 0;
    if (text.caret_row >= view)
      start = text.caret_row + 1 - view;
    if (start + view > total)
      start = total > view ? total - view : 0;

    input_rows = view;
    view_start = start;
    caret_row = text.caret_row >= start ? text.caret_row - start : 0;
    if (caret_row >= view)
      caret_row = view - 1;
    caret_column = text.caret_column;
  }

  result.input_rows = input_rows;
  result.view_start = view_start;
  result.caret_row = caret_row;
  result.caret_column = caret_column;
  result.bar_rows = compact_ ? 1 : static_cast<int>(1 + input_rows + 1);
  result.bar_top = info.srWindow.Bottom - result.bar_rows + 1;
  result.status_row = info.srWindow.Bottom;
  if (result.bar_top <= info.srWindow.Top)
    return result;

  result.valid = true;
  return result;
}

void Bar::ensure_output_room(int bar_rows) {
  if (bar_rows <= 0)
    return;

  HANDLE h = static_cast<HANDLE>(output_);
  CONSOLE_SCREEN_BUFFER_INFO info{};
  if (!GetConsoleScreenBufferInfo(h, &info))
    return;

  const int bar_top = info.srWindow.Bottom - bar_rows + 1;
  const int desired = anchor_.row + (anchor_.column > 0 ? 1 : 0);
  if (desired <= bar_top)
    return;

  int delta = desired - bar_top;

  if (info.srWindow.Bottom + delta >= info.dwSize.Y) {
    const int want = std::min<int>(info.dwSize.Y + delta + 64, 32000);
    if (want > info.dwSize.Y) {
      COORD size = info.dwSize;
      size.Y = static_cast<SHORT>(want);
      if (!SetConsoleScreenBufferSize(h, size))
        return;
      if (!GetConsoleScreenBufferInfo(h, &info))
        return;
    }
  }

  int room = info.dwSize.Y - 1 - info.srWindow.Bottom;
  if (delta > room)
    delta = room;
  if (delta <= 0)
    return;

  SMALL_RECT window = info.srWindow;
  window.Top = static_cast<SHORT>(window.Top + delta);
  window.Bottom = static_cast<SHORT>(window.Bottom + delta);
  SetConsoleWindowInfo(h, TRUE, &window);
}

void Bar::erase_bar() {
  if (painted_bar_top_ < 0 || painted_bar_rows_ <= 0)
    return;

  HANDLE h = static_cast<HANDLE>(output_);
  CONSOLE_SCREEN_BUFFER_INFO info{};
  if (!GetConsoleScreenBufferInfo(h, &info))
    return;

  DWORD written = 0;
  const int first = painted_bar_top_;
  const int last = painted_bar_top_ + painted_bar_rows_ - 1;
  for (int row = first; row <= last; ++row) {
    if (row < 0 || row >= info.dwSize.Y)
      continue;
    const COORD at{0, static_cast<SHORT>(row)};
    FillConsoleOutputCharacterW(h, L' ', info.dwSize.X, at, &written);
    FillConsoleOutputAttribute(h, info.wAttributes, info.dwSize.X, at,
                               &written);
  }

  painted_bar_top_ = -1;
  painted_bar_rows_ = 0;
}

void Bar::draw_bar(const Layout &laid) {
  if (laid.bar_rows <= 0)
    return;

  HANDLE h = static_cast<HANDLE>(output_);
  CONSOLE_SCREEN_BUFFER_INFO info{};
  if (!GetConsoleScreenBufferInfo(h, &info))
    return;

  const std::size_t columns =
      static_cast<std::size_t>(info.srWindow.Right - info.srWindow.Left + 1);

  if (!compact_) {
    place(Pos{laid.bar_top, 0});
    write_bytes("\x1b[2m");
    std::string line;
    line.reserve(columns * 3);
    for (std::size_t i = 0; i < columns; ++i)
      line += "\xe2\x94\x80";
    write_bytes(line);
    write_bytes("\x1b[0m");

    const std::size_t first_room =
        columns > kPromptColumns ? columns - kPromptColumns : 1;
    const linedit::Layout text = linedit::layout_text(
        input_text_, input_cursor_bytes_, first_room, columns);

    for (std::size_t i = 0; i < laid.input_rows; ++i) {
      place(Pos{laid.bar_top + 1 + static_cast<int>(i), 0});
      const std::size_t logical = laid.view_start + i;
      if (logical == 0)
        write_bytes("\x1b[2m> \x1b[0m");
      if (logical < text.rows.size())
        write_bytes(text.rows[logical]);
      write_bytes("\x1b[K");
    }
  }

  place(Pos{laid.status_row, 0});
  write_bytes("\x1b[7m");
  write_bytes(clip_width(status_text_, columns > 1 ? columns - 1 : 0));
  write_bytes("\x1b[K\x1b[0m");
}

void Bar::place_caret(const Layout &laid) {
  if (compact_ || laid.input_rows == 0) {
    hide_cursor();
    return;
  }
  if (laid.caret_row >= laid.input_rows) {
    hide_cursor();
    return;
  }

  int column = static_cast<int>(laid.caret_column);
  if (laid.view_start == 0 && laid.caret_row == 0)
    column += static_cast<int>(kPromptColumns);
  if (column >= static_cast<int>(columns_))
    column = static_cast<int>(columns_) - 1;
  if (column < 0)
    column = 0;

  if (!place(Pos{laid.bar_top + 1 + static_cast<int>(laid.caret_row), column}))
    return;
  show_cursor();
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

void Bar::recover_anchor() {
  HANDLE h = static_cast<HANDLE>(output_);
  CONSOLE_SCREEN_BUFFER_INFO info{};
  if (!GetConsoleScreenBufferInfo(h, &info))
    return;

  int row = painted_bar_top_ > info.srWindow.Top ? painted_bar_top_ - 1
                                                 : info.srWindow.Top;
  if (row < 0)
    row = 0;
  if (row >= info.dwSize.Y)
    row = info.dwSize.Y - 1;

  const int width = info.dwSize.X;
  if (width <= 0) {
    anchor_ = Pos{row, 0};
    painted_ = false;
    return;
  }

  std::wstring cells(static_cast<std::size_t>(width), L' ');
  DWORD read = 0;
  const COORD at{0, static_cast<SHORT>(row)};
  if (!ReadConsoleOutputCharacterW(h, cells.data(), static_cast<DWORD>(width),
                                   at, &read) ||
      read == 0) {
    anchor_ = Pos{row, 0};
    painted_ = false;
    return;
  }

  int last = -1;
  for (int i = 0; i < static_cast<int>(read); ++i) {
    if (cells[i] != L' ' && cells[i] != L'\0')
      last = i;
  }

  if (last < 0)
    anchor_ = Pos{row, 0};
  else if (last >= width - 1)
    anchor_ = Pos{row + 1, 0};
  else
    anchor_ = Pos{row, last + 1};

  painted_ = false;
}

bool Bar::bar_visible() const {
  if (painted_bar_top_ < 0 || painted_bar_rows_ <= 0)
    return true;

  CONSOLE_SCREEN_BUFFER_INFO info{};
  if (!GetConsoleScreenBufferInfo(static_cast<HANDLE>(output_), &info))
    return true;

  const int top = painted_bar_top_;
  const int bottom = painted_bar_top_ + painted_bar_rows_ - 1;
  return top >= info.srWindow.Top && bottom <= info.srWindow.Bottom;
}

bool Bar::input_visible() const {
  if (compact_)
    return true;
  return bar_visible();
}

void Bar::reveal_input() {
  if (!active_ || compact_)
    return;
  if (painted_bar_top_ < 0 || painted_bar_rows_ <= 0) {
    paint();
    return;
  }

  HANDLE h = static_cast<HANDLE>(output_);
  CONSOLE_SCREEN_BUFFER_INFO info{};
  if (!GetConsoleScreenBufferInfo(h, &info))
    return;

  const int height = info.srWindow.Bottom - info.srWindow.Top + 1;
  if (height <= 0)
    return;

  int top = painted_bar_top_ + painted_bar_rows_ - height;
  if (top + height > info.dwSize.Y)
    top = info.dwSize.Y - height;
  if (top < 0)
    top = 0;
  if (top == info.srWindow.Top) {
    painted_ = false;
    return;
  }

  SMALL_RECT window = info.srWindow;
  window.Top = static_cast<SHORT>(top);
  window.Bottom = static_cast<SHORT>(top + height - 1);
  if (SetConsoleWindowInfo(h, TRUE, &window))
    painted_ = false;
}

void Bar::flush_pending() {
  if (pending_.empty())
    return;

  hide_cursor();
  erase_bar();
  place(anchor_);
  write_bytes(pending_);
  pending_.clear();
  painted_ = false;

  Pos after{};
  if (cursor_now(after))
    anchor_ = after;
}

void Bar::write_output(std::string_view data) {
  if (!active_ || data.empty())
    return;

  if (skip_for_selection() || settling() || !pending_.empty()) {
    if (pending_.size() + data.size() <= kMaxPending)
      pending_.append(data);
    painted_ = false;
    return;
  }

  if (painted_ && painted_bar_top_ >= 0 && !bar_visible()) {
    if (pending_.size() + data.size() <= kMaxPending)
      pending_.append(data);
    return;
  }

  hide_cursor();
  erase_bar();
  place(anchor_);
  write_bytes(data);
  painted_ = false;

  Pos after{};
  if (cursor_now(after))
    anchor_ = after;

  paint();
}

void Bar::set_input(std::string_view text, std::size_t cursor_bytes) {
  input_text_.assign(text);
  input_cursor_bytes_ = cursor_bytes;
}

void Bar::set_status(std::string_view text) { status_text_.assign(text); }

void Bar::set_compact(bool compact) {
  if (compact_ == compact)
    return;
  compact_ = compact;
  painted_ = false;
  if (active_ && !settling())
    paint();
}

void Bar::commit_text(std::string_view text) {
  if (!active_ || text.empty())
    return;

  if (skip_for_selection() || settling() || !pending_.empty()) {
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

  if (painted_ && painted_bar_top_ >= 0 && !bar_visible()) {
    if (pending_.size() + text.size() + 1 <= kMaxPending) {
      pending_.append(text);
      if (text.back() != '\n')
        pending_.push_back('\n');
    }
    input_text_.clear();
    input_cursor_bytes_ = 0;
    return;
  }

  hide_cursor();
  erase_bar();
  place(anchor_);
  write_bytes(text);
  if (text.back() != '\n')
    write_bytes("\r\n");

  Pos after{};
  if (cursor_now(after))
    anchor_ = after;
  else
    anchor_ = Pos{anchor_.row + 1, 0};

  input_text_.clear();
  input_cursor_bytes_ = 0;
  painted_ = false;
  paint();
}

void Bar::resize() { painted_ = false; }

void Bar::paint() {
  if (!active_)
    return;
  if (skip_for_selection())
    return;

  if (painted_ && painted_bar_top_ >= 0 && !bar_visible())
    return;

  HANDLE h = static_cast<HANDLE>(output_);
  CONSOLE_SCREEN_BUFFER_INFO info{};
  if (!GetConsoleScreenBufferInfo(h, &info))
    return;

  const std::size_t window_rows =
      static_cast<std::size_t>(info.srWindow.Bottom - info.srWindow.Top + 1);
  const std::size_t window_columns =
      static_cast<std::size_t>(info.srWindow.Right - info.srWindow.Left + 1);

  if (window_rows != rows_ || window_columns != columns_ ||
      info.dwSize.X != buffer_width_) {
    if (buffer_width_ != 0 && info.dwSize.X != buffer_width_)
      width_changed_ = true;
    buffer_width_ = info.dwSize.X;
    rows_ = window_rows;
    columns_ = window_columns;
    settling_ = true;
    settle_until_ = std::chrono::steady_clock::now() + kSettleDelay;
    return;
  }

  if (settling_) {
    if (std::chrono::steady_clock::now() < settle_until_)
      return;
    settling_ = false;
    if (width_changed_) {
      width_changed_ = false;
      layout_lost_ = true;
    }
    painted_ = false;
  }

  if (layout_lost_) {
    layout_lost_ = false;
    recover_anchor();
  }

  if (!pending_.empty())
    flush_pending();

  Layout laid = layout();
  if (!laid.valid)
    return;

  if (painted_ && painted_bar_top_ == laid.bar_top &&
      painted_bar_rows_ == laid.bar_rows && painted_input_ == input_text_ &&
      painted_cursor_ == input_cursor_bytes_ &&
      painted_status_ == status_text_ && painted_compact_ == compact_) {
    return;
  }

  hide_cursor();
  erase_bar();
  ensure_output_room(laid.bar_rows);

  laid = layout();
  if (!laid.valid)
    return;

  draw_bar(laid);
  place_caret(laid);

  painted_ = true;
  painted_input_ = input_text_;
  painted_cursor_ = input_cursor_bytes_;
  painted_status_ = status_text_;
  painted_compact_ = compact_;
  painted_bar_top_ = laid.bar_top;
  painted_bar_rows_ = laid.bar_rows;
}

} // namespace linedit
