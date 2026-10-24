#include "base/path.hpp"

#include <cwctype>
#include <system_error>
#include <string>
namespace coding {

namespace {

bool same_part(const std::filesystem::path &a, const std::filesystem::path &b) {
#if defined(_WIN32)
  const std::wstring &x = a.native();
  const std::wstring &y = b.native();
  if (x.size() != y.size())
    return false;
  for (std::size_t i = 0; i < x.size(); ++i) {
    if (std::towlower(x[i]) != std::towlower(y[i]))
      return false;
  }
  return true;
#else
  return a == b;
#endif
}

} // namespace

std::filesystem::path compress_path(const std::filesystem::path &p,
                                    std::error_code &ec, bool resolve_symlinks,
                                    bool make_absolute) {
  ec.clear();
  if (p.empty())
    return {};

  std::filesystem::path result = p;

  if (make_absolute && !result.is_absolute()) {
    std::error_code abs_ec;
    std::filesystem::path abs = std::filesystem::absolute(result, abs_ec);
    if (abs_ec) {
      ec = abs_ec;
    } else {
      result = std::move(abs);
    }
  }

  if (resolve_symlinks) {
    std::error_code weak_ec;
    std::filesystem::path weak =
        std::filesystem::weakly_canonical(result, weak_ec);
    if (weak_ec) {
      if (!ec)
        ec = weak_ec;
      result = result.lexically_normal();
    } else {
      result = std::move(weak);
    }
  } else {
    result = result.lexically_normal();
  }

  if (result.has_filename() && result.filename().empty())
    result = result.parent_path();

  return result;
}

std::filesystem::path compress_path(const std::filesystem::path &p,
                                    bool resolve_symlinks, bool make_absolute) {
  std::error_code ec;
  return compress_path(p, ec, resolve_symlinks, make_absolute);
}

std::string show_path(const std::filesystem::path &p) {
  return compress_path(p).string();
}

std::filesystem::path relative_to(const std::filesystem::path &base,
                                  const std::filesystem::path &path) {
  if (base.empty() || path.empty())
    return {};

  const std::filesystem::path b = compress_path(base, false, true);
  const std::filesystem::path p = compress_path(path, false, true);
  if (b.empty() || p.empty())
    return {};

  if (!same_part(b.root_name(), p.root_name()))
    return {};

  auto bi = b.begin();
  auto pi = p.begin();
  while (bi != b.end()) {
    if ((*bi).empty()) {
      ++bi;
      continue;
    }
    while (pi != p.end() && (*pi).empty())
      ++pi;
    if (pi == p.end())
      return {};
    if (!same_part(*bi, *pi))
      return {};
    ++bi;
    ++pi;
  }

  std::filesystem::path rel;
  for (; pi != p.end(); ++pi) {
    if ((*pi).empty() || *pi == ".")
      continue;
    rel /= *pi;
  }
  return rel;
}

std::string show_relative(const std::filesystem::path &base,
                          const std::filesystem::path &path) {
  const std::filesystem::path rel = relative_to(base, path);
  if (rel.empty())
    return show_path(path);
  return show_path(rel);
}

} // namespace coding