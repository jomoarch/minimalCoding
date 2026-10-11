#ifndef PROCESS_ACCOUNTING_HPP
#define PROCESS_ACCOUNTING_HPP

#include "process/runner.hpp"

#include <chrono>

namespace coding {

void fill_accounting(RunnerResult &out, void *process, void *job,
                     std::chrono::steady_clock::time_point started);

} // namespace coding

#endif // PROCESS_ACCOUNTING_HPP