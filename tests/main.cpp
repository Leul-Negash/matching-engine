// Runs every registered test, or only those whose name contains argv[1].

#include "test.hpp"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    const std::string filter = argc > 1 ? argv[1] : "";
    int run = 0;
    int failed = 0;
    for (const auto& t : test::registry()) {
        if (!filter.empty() && t.name.find(filter) == std::string::npos) continue;
        ++run;
        const bool passed = t.fn();
        std::printf("%s  %s\n", passed ? "PASS" : "FAIL", t.name.c_str());
        std::fflush(stdout);
        if (!passed) ++failed;
    }
    std::printf("\n%d tests, %d failed\n", run, failed);
    return failed == 0 ? 0 : 1;
}
