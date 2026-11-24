#pragma once

#include <cstdio>
#include <cstdlib>

[[noreturn]] inline void checkFail(const char* expression, const char* file, int line) {
    std::fprintf(stderr, "check failed: %s (%s:%d)\n", expression, file, line);
    std::abort();
}

// Always-evaluated check: assert() disappears under NDEBUG.
#define CHECK(condition) ((condition) ? (void)0 : checkFail(#condition, __FILE__, __LINE__))
