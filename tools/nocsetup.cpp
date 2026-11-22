#include "gamesetup.h"

#include <cstdio>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: nocsetup <game folder> <disc image>...\n");
        return 2;
    }
    std::vector<std::filesystem::path> images(argv + 2, argv + argc);
    std::string error;
    bool ok = setup_extract(images, argv[1], [](uint64_t done, uint64_t total, const std::string& file) {
        if (!file.empty()) {
            std::printf("[%3d%%] %s\n", int(done * 100 / total), file.c_str());
        }
    }, error);
    if (!ok) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    std::printf("game folder: %s\n", std::filesystem::absolute(argv[1]).string().c_str());
    return 0;
}
