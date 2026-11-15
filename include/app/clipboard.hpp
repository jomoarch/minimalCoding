#ifndef CLIPBOARD_HPP
#define CLIPBOARD_HPP

#include <string>
#include <string_view>

namespace coding {

namespace clipboard {

std::string text();

bool set_text(std::string_view utf8);

} // namespace clipboard
} // namespace coding

#endif // CLIPBOARD_HPP
