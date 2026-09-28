#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    if (argc < 2) return 3;
    if (std::strcmp(argv[1], "--ok") == 0) {
        std::printf("child output\n");
        return 7;
    }
    if (std::strcmp(argv[1], "--slow") == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds{250});
        return 0;
    }
    if (std::strcmp(argv[1], "--stdin") == 0) {
        std::string input;
        std::getline(std::cin, input);
        std::printf("stdin-bytes=%zu\n", input.size());
        return argc > 2 && std::strcmp(argv[2], "sensitive-value") == 0 ? 9 : 0;
    }
    if (std::strcmp(argv[1], "--large") == 0) {
        const std::string block(4096, 'X');
        for (int i = 0; i < 80; ++i) {
            std::cout.write(block.data(), static_cast<std::streamsize>(block.size()));
        }
        std::cout.flush();
        return 0;
    }
    return 4;
}
