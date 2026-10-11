#include "app/prompt.hpp"

#include "base/color.hpp"
#include "base/text.hpp"

#include <windows.h>
#include <conio.h>

#include <iostream>
#include <string>
namespace coding {

namespace prompt {

bool should_pause(int pause) noexcept {
  if (pause >= 0)
    return pause != 0;

  DWORD list[4] = {};
  const DWORD count = GetConsoleProcessList(list, 4);
  return count <= 1;
}

bool interactive_stdin() noexcept {
  HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
  DWORD mode = 0;
  return hIn != nullptr && hIn != INVALID_HANDLE_VALUE &&
         GetConsoleMode(hIn, &mode) != FALSE;
}

void pause_if_needed(int pause) {
  if (!should_pause(pause))
    return;
  std::cout << "\nPress any key to exit ..." << std::flush;
  _getch();
}

bool ask_yes_no(std::string_view question) {
  HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
  DWORD mode = 0;
  if (hIn != nullptr && hIn != INVALID_HANDLE_VALUE &&
      GetConsoleMode(hIn, &mode))
    FlushConsoleInputBuffer(hIn);

  std::cout << question << " [y/N] " << std::flush;

  std::string line;
  if (!std::getline(std::cin, line))
    return false;

  const std::string answer = text::to_lower(text::trim(line));
  return answer == "y" || answer == "yes";
}

bool ask_save(const std::filesystem::path &target, std::string_view what) {
  HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
  DWORD mode = 0;
  if (hIn != nullptr && hIn != INVALID_HANDLE_VALUE &&
      GetConsoleMode(hIn, &mode))
    FlushConsoleInputBuffer(hIn);

  std::cout << "[save] Save " << what << " to " << target.string()
            << " ? [y/N] " << std::flush;

  std::string line;
  if (!std::getline(std::cin, line)) {
    std::cout << "\n" << color::info("[save] skipped (stdin closed)") << "\n";
    return false;
  }

  const std::string answer = text::to_lower(text::trim(line));
  if (answer != "y" && answer != "yes") {
    std::cout << color::info("[save] skipped") << "\n";
    return false;
  }
  return true;
}

} // namespace prompt
} // namespace coding
