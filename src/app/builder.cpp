#include "app/builder.hpp"
#include "app/build_cache.hpp"
#include "app/pch.hpp"
#include "app/prompt.hpp"
#include "app/stale_exe.hpp"
#include "base/color.hpp"
#include "base/hash.hpp"
#include "base/path.hpp"
#include "process/compiler.hpp"

#include <chrono>
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

std::string cache_key_of(const BuildOption &opts) {
  if (!opts.cache_key.empty())
    return opts.cache_key;
  return filename_of(opts.source_path);
}

} // namespace

std::string recipe_cache_key(std::string_view base_key, bool probe,
                             const std::vector<std::string> &args,
                             const std::filesystem::path &inject_header) {
  hash::Sha256 hasher;
  hasher.write(base_key);
  hasher.write_u64(probe ? 1 : 0);
  hasher.write_u64(args.size());
  for (const std::string &arg : args) {
    hasher.write_u64(arg.size());
    hasher.write(arg);
  }
  if (probe) {
    const auto utf8 = inject_header.u8string();
    const std::string header_path(utf8.begin(), utf8.end());
    hasher.write_u64(header_path.size());
    hasher.write(header_path);

    std::string header_sha;
    if (hash::file_sha256(inject_header, header_sha))
      hasher.write(header_sha);
    else
      hasher.write("missing");
  }

  std::string digest = hasher.hex();
  if (digest.empty())
    digest = "nohash";

  std::string key(base_key);
  key += probe ? "#probe:" : "#normal:";
  key += digest;
  return key;
}

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
      if (!hasher.ready(sha)) {
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
        const cache::Entry *cached =
            opts.cache_path.empty() ? nullptr : table.find(cache_key_of(opts));
        if (cached != nullptr && cached->source_sha256 == sha) {
          std::string exe_sha;
          if (!hash::file_sha256(opts.output_path, exe_sha)) {
            need = true;
            reason = "cannot hash the exe";
          } else if (exe_sha != cached->exe_sha256) {
            need = true;
            reason = "exe sha256 changed";
          } else {
            reason = "sha256 matches";
          }
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

  const std::filesystem::path trash =
      opts.trash_dir.empty() ? opts.output_path.parent_path() / ".trash"
                             : opts.trash_dir;

  std::string note;
  stale_exe::Outcome cleared =
      stale_exe::clear(opts.output_path, false, trash, note);
  if (cleared == stale_exe::Outcome::Blocked) {
    std::cout << color::err("[build] ", note, "\n") << std::flush;
    if (prompt::ask_yes_no("Move it into " + show_path(trash) +
                           " under its SHA-256 and carry on?")) {
      cleared = stale_exe::clear(opts.output_path, true, trash, note);
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

  std::filesystem::path inject = opts.inject_header;
  if (!inject.empty() && !opts.pch_dir.empty()) {
    pch::Request request;
    request.header = inject;
    request.cache_dir = opts.pch_dir;
    request.args = opts.args;

    const pch::Result prepared = pch::ensure(request);
    if (prepared.ok) {
      inject = prepared.include_path;
      if (prepared.rebuilt) {
        std::cout << color::gray("[build] precompiled header -> ",
                                 show_relative(opts.base, inject), " (",
                                 prepared.elapsed.count(), " ms)")
                  << "\n"
                  << std::flush;
      }
    } else {
      std::cerr << color::warn(
          "[build] precompiled header skipped: ", prepared.error, "\n");
    }
  }

  CompilerOptions co;
  co.source_path = opts.source_path;
  co.output_path = opts.output_path;
  co.args = opts.args;
  co.inject_header = inject;

  const auto compile_start = std::chrono::steady_clock::now();
  auto cr = compile_source(co);
  r.compile_time = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - compile_start);
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
      std::string exe_sha;
      if (!hash::file_sha256(opts.output_path, exe_sha)) {
        std::cerr << color::warn("[build] cannot hash ",
                                 show_path(opts.output_path),
                                 ", the build cache was not updated\n");
      } else {
        cache::Table table(opts.cache_path);
        std::string warning;
        table.load(warning);
        table.touch(cache_key_of(opts), sha, exe_sha);
        std::string error;
        if (!table.save(error))
          std::cerr << color::warn("[build] cache not saved: ", error, "\n");
      }
    }
  }
  return r;
}
} // namespace coding
