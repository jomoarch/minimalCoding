#include "app/pch.hpp"

#include "base/hash.hpp"
#include "base/path.hpp"
#include "process/compiler.hpp"

#include <fstream>
#include <iterator>
#include <system_error>
namespace coding {
namespace pch {

namespace {

std::string digest(const std::vector<std::string> &args) {
  hash::Sha256 hasher;
  for (const std::string &arg : args) {
    hasher.write(arg);
    hasher.write_u64(arg.size());
  }
  return hasher.hex();
}

bool read_text(const std::filesystem::path &path, std::string &out) {
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return false;
  out.assign(std::istreambuf_iterator<char>(file),
             std::istreambuf_iterator<char>());
  return true;
}

bool write_text(const std::filesystem::path &path, std::string_view text) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file)
    return false;
  file.write(text.data(), static_cast<std::streamsize>(text.size()));
  return file.good();
}

} // namespace

Result ensure(const Request &request) {
  Result out;

  if (request.header.empty() || request.args.empty() ||
      request.cache_dir.empty()) {
    out.error = "no header, compiler or cache directory";
    return out;
  }

  std::string header_sha;
  if (!hash::file_sha256(request.header, header_sha)) {
    out.error = "cannot hash " + show_path(request.header);
    return out;
  }

  const std::filesystem::path dir =
      request.cache_dir / digest(request.args).substr(0, 12);
  const std::filesystem::path copy = dir / "probe.h";
  const std::filesystem::path gch = dir / "probe.h.gch";
  const std::filesystem::path stamp = dir / ".key";

  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    out.error = "cannot create " + show_path(dir) + ": " + ec.message();
    return out;
  }

  std::string previous;
  read_text(stamp, previous);
  if (previous == header_sha && std::filesystem::exists(copy, ec) &&
      std::filesystem::exists(gch, ec)) {
    out.ok = true;
    out.include_path = copy;
    return out;
  }

  std::filesystem::copy_file(request.header, copy,
                             std::filesystem::copy_options::overwrite_existing,
                             ec);
  if (ec) {
    out.error =
        "cannot copy " + show_path(request.header) + ": " + ec.message();
    return out;
  }

  const auto started = std::chrono::steady_clock::now();
  const CompilerResult built = precompile_header(request.args, copy, gch);
  out.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);

  if (!built) {
    out.error = built.message;
    return out;
  }

  write_text(stamp, header_sha);

  out.ok = true;
  out.rebuilt = true;
  out.include_path = copy;
  return out;
}

} // namespace pch
} // namespace coding
