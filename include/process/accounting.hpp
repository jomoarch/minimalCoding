#ifndef PROCESS_ACCOUNTING_HPP
#define PROCESS_ACCOUNTING_HPP

#include "process/runner.hpp"

#include <chrono>
#include <cstddef>

namespace coding {

void fill_accounting(RunnerResult &out, void *process, void *job,
                     std::chrono::steady_clock::time_point started,
                     std::size_t memory_limit_bytes, bool time_out);

} // namespace coding

#endif // PROCESS_ACCOUNTING_HPP