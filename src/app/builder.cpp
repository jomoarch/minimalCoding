#include "app/builder.hpp"
#include "app/build_cache.hpp"
#include "app/prompt.hpp"
#include "app/stale_exe.hpp"
#include "base/color.hpp"
#include "base/hash.hpp"
#include "base/path.hpp"
#include "process/compiler.hpp"

#include <filesystem>
#include <iostream>
#include <string>
namespace coding {

namespace {

class SourceHasher {
public:
  explicit SourceHasher(const std::filesystem::path &path) : path_(path) {}

  bool ready(std::string &sha) {
    if (!done_) {
      done_ = true;
      ok_ = hash::file_sha256(path_, sha_);
    }
    sha = sha_;
    return ok_;
  }

private:
  std::filesystem::path path_;
  bool done_{false};
  bool ok_{false};
  std::string sha_;
};

std::string filename_of(const std::filesystem::path &path) {
  const auto utf8 = path.filename().u8string();
  return std::string(utf8.begin(), utf8.end());
}

} // namespace

BuildResult ensure_built(const BuildOption &opts) {
  BuildResult r;
  r.success = false;

  std::error_code ec;
  bool need = false;
  std::string reason;

  SourceHasher hasher(opts.source_path);
  std::string sha;

  if (opts.force_rebuild) {
    need = true;
    reason = "forced";
  } else if (!std::filesystem::exists(opts.output_path, ec)) {
    need = true;
    reason = "no exe yet";
  } else {
    auto exe_time = std::filesystem::last_write_time(opts.output_path, ec);
    if (ec) {
      r.message = "Cannot stat exe: " + ec.message();
      return r;
    }

    for (const auto &dep : opts.extra_deps) {
      std::error_code dep_ec;
      if (!std::filesystem::exists(dep, dep_ec))
        continue;
      const auto dep_time = std::filesystem::last_write_time(dep, dep_ec);
      if (!dep_ec && dep_time > exe_time) {
        need = true;
        reason = filename_of(dep) + " is newer";
        break;
      }
    }

    if (!need) {
      std::error_code source_ec;
      const auto source_time =
          std::filesystem::last_write_time(opts.source_path, source_ec);
      if (!source_ec && exe_time > source_time) {
        reason = "exe is newer";
      } else if (!hasher.ready(sha)) {
        need = true;
        reason = "cannot hash the source";
      } else {
        cache::Table table(opts.cache_path);
        std::string warning;
        if (!opts.cache_path.empty()) {
          table.load(warning);
          if (!warning.empty())
            std::cerr << color::warn("[build] ", warning, "\n");
        }
        const std::string *cached =
            opts.cache_path.empty() ? nullptr
                                    : table.find(filename_of(opts.source_path));
        if (cached != nullptr && *cached == sha) {
          reason = "sha256 matches";
        } else {
          need = true;
          reason = cached != nullptr ? "sha256 changed" : "no cache entry";
        }
      }
    }
  }

  if (!need) {
    r.success = true;
    r.rebuilt = false;
    r.reason = reason;
    r.message = "up-to-date";
    return r;
  }

  if (!opts.cache_path.empty())
    hasher.ready(sha);

  std::string note;
  stale_exe::Outcome cleared = stale_exe::clear(opts.output_path, false, note);
  if (cleared == stale_exe::Outcome::Blocked) {
    std::cout << color::err("[build] ", note, "\n") << std::flush;
    if (prompt::ask_yes_no(
            "Move it into " +
            show_path(opts.output_path.parent_path() / ".trash") +
            " under its SHA-256 and carry on?")) {
      cleared = stale_exe::clear(opts.output_path, true, note);
      if (cleared == stale_exe::Outcome::Parked)
        std::cout << color::ok("[build] ", note, "\n") << std::flush;
    }
  } else if (cleared == stale_exe::Outcome::Parked) {
    std::cout << color::ok("[build] ", note, "\n") << std::flush;
  }

  if (cleared == stale_exe::Outcome::Blocked) {
    r.message = note + "\nThe build cannot continue: close whatever is using " +
                opts.output_path.filename().string() + " and try again.";
    if (!prompt::should_pause(-1) && prompt::interactive_stdin())
      prompt::pause_if_needed(1);
    return r;
  }

  CompilerOptions co;
  co.source_path = opts.source_path;
  co.output_path = opts.output_path;
  co.args = opts.args;
  co.inject_header = opts.inject_header;

  auto cr = compile_source(co);
  r.success = cr.success;
  r.rebuilt = true;
  r.reason = reason;
  r.message = cr.message;

  if (r.success && !opts.cache_path.empty()) {
    if (sha.empty()) {
      std::cerr << color::warn("[build] cannot hash ",
                               show_path(opts.source_path),
                               ", the build cache was not updated\n");
    } else {
      cache::Table table(opts.cache_path);
      std::string warning;
      table.load(warning);
      table.touch(filename_of(opts.source_path), sha);
      std::string error;
      if (!table.save(error))
        std::cerr << color::warn("[build] cache not saved: ", error, "\n");
    }
  }
  return r;
}
} // namespace coding
