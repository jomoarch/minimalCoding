#ifndef PATH_HPP
#define PATH_HPP

#include <filesystem>
#include <string>

namespace coding {

[[nodiscard]] std::filesystem::path
compress_path(const std::filesystem::path &p, std::error_code &ec,
              bool resolve_symlinks = false, bool make_absolute = false);

[[nodiscard]] std::filesystem::path
compress_path(const std::filesystem::path &p, bool resolve_symlinks = false,
              bool make_absolute = false);

[[nodiscard]] std::string show_path(const std::filesystem::path &p);

} // namespace coding

#endif // PATH_HPP