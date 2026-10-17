// MH4N_ASSERT and MH4N_UNREACHABLE: log at Critical (whatever the log filter says) and abort
// (N1-01).
#pragma once

#include <string>

#include <fmt/format.h>

namespace mh4n::detail {

[[noreturn]] void AssertFail(const char* file, int line, const char* what, std::string message);

} // namespace mh4n::detail

// MH4N_ASSERT(cond) or MH4N_ASSERT(cond, "fmt {}", args...). The condition is always evaluated.
#define MH4N_ASSERT(cond, ...)                                                                     \
    do {                                                                                           \
        if (!(cond)) [[unlikely]] {                                                                \
            ::mh4n::detail::AssertFail(__FILE__, __LINE__, "assertion failed: " #cond,             \
                                       std::string(__VA_OPT__(::fmt::format(__VA_ARGS__))));       \
        }                                                                                          \
    } while (0)

#define MH4N_UNREACHABLE()                                                                         \
    ::mh4n::detail::AssertFail(__FILE__, __LINE__, "unreachable code reached", std::string())
