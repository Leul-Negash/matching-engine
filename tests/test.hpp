// Minimal self-contained test framework: TEST(name) { ...; return ok; } with
// CHECK(cond) recording failures. Tests self-register; tests/main.cpp runs them.
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace test {

struct Case {
    std::string name;
    std::function<bool()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

struct Register {
    Register(const char* name, std::function<bool()> fn) { registry().push_back({name, std::move(fn)}); }
};

}  // namespace test

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("    %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            ok = false;                                                      \
        }                                                                    \
    } while (0)

#define TEST(name)                                       \
    static bool name();                                  \
    static test::Register reg_##name(#name, name);       \
    static bool name()
