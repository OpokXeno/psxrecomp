#include "config_loader.h"
#undef NDEBUG
#include <cassert>
#include <fstream>

int main(int argc, char **argv) {
    assert(argc == 2);
    const std::filesystem::path path = argv[1];
    for (int fps : {30, 45, 60, 75, 90, 100, 120, 144, 165, 240}) {
        { std::ofstream file(path); file << "[video]\nfps = " << fps << '\n'; }
        auto settings = PSXRecompV4::load_user_settings(path);
        assert(settings.has_fps && settings.fps == fps);
        assert(PSXRecompV4::save_user_settings(path, settings));
        settings = PSXRecompV4::load_user_settings(path);
        assert(settings.has_fps && settings.fps == fps);
    }
    for (int fps : {0, 29, 241, 1000}) {
        { std::ofstream file(path); file << "[video]\nfps = " << fps << '\n'; }
        assert(!PSXRecompV4::load_user_settings(path).has_fps);
    }
    std::filesystem::remove(path);
}
