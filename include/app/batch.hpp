#ifndef APP_BATCH_HPP
#define APP_BATCH_HPP

#include "io/config.hpp"
#include "process/runner.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace coding {
namespace app {

struct BatchCase {
  std::string name;
  std::filesystem::path input;
  std::filesystem::path expected;
  bool has_answer{false};
};

struct BatchUnit {
  std::string name;
  RunnerResult result;
  std::string captured;
  bool has_answer{false};
  bool accepted{false};
};

enum class BatchVerdict {
  Accepted,
  WrongAnswer,
  NoAnswer,
  TimeLimitExceeded,
  MemoryLimitExceeded,
  RuntimeError,
  SystemError
};

[[nodiscard]] bool pick_folder(const std::wstring &title,
                               const std::filesystem::path &initial,
                               std::filesystem::path &out, std::string &error);
[[nodiscard]] bool list_cases(const std::filesystem::path &dir,
                              std::vector<BatchCase> &cases,
                              std::string &error);
[[nodiscard]] std::vector<BatchUnit>
run_all(const AppConfig &cfg, const std::vector<BatchCase> &cases,
        const std::filesystem::path &exe, std::size_t threads);
[[nodiscard]] BatchVerdict verdict_of(const BatchUnit &unit);
const char *verdict_name(BatchVerdict verdict);
[[nodiscard]] bool save_outputs(const AppConfig &cfg,
                                const std::vector<BatchUnit> &units,
                                std::filesystem::path &folder,
                                std::string &error);

} // namespace app
} // namespace coding

#endif // APP_BATCH_HPP
