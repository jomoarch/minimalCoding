#ifndef CRASH_CODES_HPP
#define CRASH_CODES_HPP

#include <windows.h>

namespace coding {
namespace crash {

struct Known {
  unsigned long code;
  const char *name;
  const char *symbol;
};

inline constexpr Known kKnownCodes[] = {
    {static_cast<unsigned long>(STATUS_ACCESS_VIOLATION), "access violation",
     "STATUS_ACCESS_VIOLATION"},
    {static_cast<unsigned long>(STATUS_STACK_OVERFLOW), "stack overflow",
     "STATUS_STACK_OVERFLOW"},
    {static_cast<unsigned long>(STATUS_INTEGER_DIVIDE_BY_ZERO),
     "integer divide by zero", "STATUS_INTEGER_DIVIDE_BY_ZERO"},
    {static_cast<unsigned long>(STATUS_STACK_BUFFER_OVERRUN),
     "stack buffer overrun or assertion abort", "STATUS_STACK_BUFFER_OVERRUN"},
    {static_cast<unsigned long>(STATUS_ILLEGAL_INSTRUCTION),
     "illegal instruction", "STATUS_ILLEGAL_INSTRUCTION"},
    {static_cast<unsigned long>(STATUS_INTEGER_OVERFLOW), "integer overflow",
     "STATUS_INTEGER_OVERFLOW"},
    {static_cast<unsigned long>(STATUS_HEAP_CORRUPTION), "heap corruption",
     "STATUS_HEAP_CORRUPTION"},
    {static_cast<unsigned long>(STATUS_IN_PAGE_ERROR), "in-page error",
     "STATUS_IN_PAGE_ERROR"},
    {static_cast<unsigned long>(STATUS_DLL_NOT_FOUND), "dll not found",
     "STATUS_DLL_NOT_FOUND"},
    {static_cast<unsigned long>(STATUS_ENTRYPOINT_NOT_FOUND),
     "entry point not found", "STATUS_ENTRYPOINT_NOT_FOUND"},
};

inline constexpr const Known *find(unsigned long code) noexcept {
  for (const Known &known : kKnownCodes) {
    if (known.code == code)
      return &known;
  }
  return nullptr;
}

} // namespace crash
} // namespace coding

#endif // CRASH_CODES_HPP
