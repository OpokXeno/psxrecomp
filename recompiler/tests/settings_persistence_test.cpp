/* settings_persistence_test - settings.toml survives a save/load cycle.
 *
 *   1. every FPS the launcher offers loads back unchanged;
 *   2. a settings file in a non-ASCII directory loads and saves;
 *   3. non-ASCII disc paths round-trip;
 *   4. an accented path (valid in the ANSI code page) no longer corrupts the file;
 *   5. a failed save reports failure and leaves the old file intact, and a
 *      good save leaves no temp file behind.
 */
#include "config_loader.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

static int failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

static std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

/* Built from UTF-8 bytes so the test compiles as C++17 and C++20. */
static fs::path utf8_path(const char* bytes) {
    std::string s(bytes);
#if defined(__cpp_char8_t)
    return fs::path(std::u8string(s.begin(), s.end()));
#else
    return fs::u8path(s);
#endif
}

static void test_launcher_fps_values() {
    const int fps_table[] = {30, 60, 75, 120, 144, 165, 240};
    fs::path dir = fs::temp_directory_path() / "psxrecomp_settings_fps";
    fs::create_directories(dir);
    for (int fps : fps_table) {
        PSXRecompV4::UserSettings s;
        s.fps = fps;
        s.has_fps = true;
        const fs::path p = dir / "settings.toml";
        check(PSXRecompV4::save_user_settings(p, s), "fps save succeeds");
        const auto back = PSXRecompV4::load_user_settings(p);
        check(back.has_fps && back.fps == fps,
              "launcher fps value survives a reload");
    }
    fs::remove_all(dir);
}

static void test_non_ascii_locations() {
    fs::path dir = fs::temp_directory_path() /
        utf8_path("psxrecomp_settings_\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E_\xC3\xBC");
    fs::remove_all(dir);
    fs::create_directories(dir);

    PSXRecompV4::UserSettings s;
    s.fps = 144;
    s.has_fps = true;
    s.disc_path = dir / utf8_path("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E disc.cue");
    s.has_disc_path = true;
    const fs::path p = dir / "settings.toml";
    check(PSXRecompV4::save_user_settings(p, s),
          "save into a non-ASCII directory succeeds");

    const auto back = PSXRecompV4::load_user_settings(p);
    check(!back.parse_error, "settings in a non-ASCII directory parse");
    check(back.has_fps && back.fps == 144,
          "fps loads from a non-ASCII directory");
    check(back.has_disc_path && back.disc_path == s.disc_path,
          "non-ASCII disc path round-trips");
    fs::remove_all(dir);
}

static void test_accented_path_round_trips() {
    /* e-acute fits the ANSI code page, so nothing throws, but it used to be
     * written as the single byte 0xE9: invalid UTF-8, so the reload failed to
     * parse and every setting was lost. */
    fs::path dir = fs::temp_directory_path() / "psxrecomp_settings_accent";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path p = dir / "settings.toml";

    PSXRecompV4::UserSettings s;
    s.fps = 144;
    s.has_fps = true;
    s.disc_path = fs::path("C:/Users") / utf8_path("Jos\xC3\xA9") / "disc.cue";
    s.has_disc_path = true;
    check(PSXRecompV4::save_user_settings(p, s), "save with an accented path");

    const auto back = PSXRecompV4::load_user_settings(p);
    check(!back.parse_error, "settings with an accented path still parse");
    check(back.has_fps && back.fps == 144, "fps survives an accented path");
    check(back.has_disc_path && back.disc_path == s.disc_path,
          "accented disc path round-trips");
    fs::remove_all(dir);
}

static void test_failed_save_keeps_old_file() {
    fs::path dir = fs::temp_directory_path() / "psxrecomp_settings_atomic";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path p = dir / "settings.toml";

    PSXRecompV4::UserSettings s;
    s.fps = 144;
    s.has_fps = true;
    check(PSXRecompV4::save_user_settings(p, s), "first save succeeds");
    const std::string good = slurp(p);
    check(!fs::exists(fs::path(p) += ".tmp"), "no temp file after a good save");

    /* A directory squatting on the temp name makes the new write fail. */
    fs::create_directories(fs::path(p) += ".tmp");
    s.fps = 60;
    check(!PSXRecompV4::save_user_settings(p, s),
          "save reports failure when it cannot write");
    check(slurp(p) == good, "failed save leaves the previous file intact");
    fs::remove_all(dir);
}

static void guarded(void (*fn)(), const char* name) {
    try {
        fn();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s threw: %s\n", name, e.what());
        ++failures;
    }
}

int main() {
    guarded(test_launcher_fps_values, "launcher fps values");
    guarded(test_non_ascii_locations, "non-ASCII locations");
    guarded(test_accented_path_round_trips, "accented path");
    guarded(test_failed_save_keeps_old_file, "failed save");
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("settings_persistence_test: all passed");
    return 0;
}
