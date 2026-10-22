#ifndef PROMPT_HPP
#define PROMPT_HPP

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
namespace coding {

namespace prompt {

bool should_pause(int pause) noexcept;

bool interactive_stdin() noexcept;

void wait_any_key();

void pause_if_needed(int pause);

bool ask_save(const std::filesystem::path &target, std::string_view what);

bool ask_yes_no(std::string_view question);

} // namespace prompt

} // namespace coding

#endif // PROMPT_HPP
