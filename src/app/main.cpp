#include "core/version.hpp"
#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("%s %s\n", gao::kProductName.data(), gao::kVersion.data());
        return 0;
    }
    std::printf("usage: gao --version\n");
    return argc > 1 ? 1 : 0;
}
