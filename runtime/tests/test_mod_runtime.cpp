#include "mod_runtime.h"
#include "mod_packages.h"
#include "iso_reader.h"
#include "psx_sha256.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::array<uint8_t, 2 * 1024 * 1024> ram;
static int failures;
static int activation_calls;
static int plugin_calls;
static int indexed_handler_calls;
static bool indexed_handler_fail;
static bool indexed_handler_invalid_extension;
static fs::path indexed_handler_disc_path;
static uint32_t indexed_handler_base_sector_count;
static PS1::ISOReader* indexed_handler_reader;
static std::vector<PSXRecompV4::ModResolution::IndexedFile> indexed_handler_files;

extern "C" uint8_t psx_read_byte(uint32_t address) {
    return ram[address & 0x1fffffu];
}

extern "C" void psx_write_byte(uint32_t address, uint8_t value) {
    ram[address & 0x1fffffu] = value;
}

extern "C" uint16_t psx_read_half(uint32_t address) {
    const uint32_t offset = address & 0x1fffffu;
    return (uint16_t)(ram[offset] | ((uint16_t)ram[offset + 1] << 8));
}

extern "C" void psx_write_half(uint32_t address, uint16_t value) {
    const uint32_t offset = address & 0x1fffffu;
    ram[offset] = (uint8_t)value;
    ram[offset + 1] = (uint8_t)(value >> 8);
}

extern "C" uint32_t psx_read_word(uint32_t address) {
    const uint32_t offset = address & 0x1fffffu;
    return (uint32_t)ram[offset] |
           ((uint32_t)ram[offset + 1] << 8) |
           ((uint32_t)ram[offset + 2] << 16) |
           ((uint32_t)ram[offset + 3] << 24);
}

extern "C" void psx_write_word(uint32_t address, uint32_t value) {
    const uint32_t offset = address & 0x1fffffu;
    ram[offset] = (uint8_t)value;
    ram[offset + 1] = (uint8_t)(value >> 8);
    ram[offset + 2] = (uint8_t)(value >> 16);
    ram[offset + 3] = (uint8_t)(value >> 24);
}

extern "C" uint32_t psx_mod_memory_alloc(uint32_t, uint32_t) { return 0; }
extern "C" uint32_t psx_mod_gpu_dma_memory_alloc(uint32_t, uint32_t) {
    return 0;
}
extern "C" int psx_ws_x_margin(void) { return 0; }

extern "C" void dirty_ram_mark_executable_range(uint32_t, uint32_t) {}
extern "C" int fntrace_is_game_started(void) { return 1; }

static void test_vblank_plugin(void) {
    plugin_calls++;
}

static void test_activation_plugin(void) {
    activation_calls++;
}

static void check(bool value, const char* message) {
    if (!value) {
        std::cerr << "FAIL: " << message << "\n";
        failures++;
    }
}

static void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path);
    out << text;
}

static void write_bytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write((const char*)bytes.data(), (std::streamsize)bytes.size());
}

static std::string sha256_hex(const std::vector<uint8_t>& bytes) {
    uint8_t digest[32];
    psx_sha256_compute(bytes.data(), bytes.size(), digest);
    static const char hex[] = "0123456789abcdef";
    std::string out(64, '0');
    for (size_t i = 0; i < 32; ++i) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 15];
    }
    return out;
}

static bool test_indexed_file_handler(
    PS1::ISOReader& disc,
    const std::vector<PSXRecompV4::ModResolution::IndexedFile>& files,
    uint32_t base_sector_count, PSXRecompV4::ModVirtualDisc& output,
    std::string* error) {
    indexed_handler_calls++;
    indexed_handler_reader = &disc;
    indexed_handler_disc_path = disc.GetBinPath();
    indexed_handler_base_sector_count = base_sector_count;
    indexed_handler_files = files;

    std::array<uint8_t, 2352> existing{};
    existing.fill(0x31);
    existing[15] = 2;
    existing[18] = 0;
    existing[24] = 0xa5;
    output.raw_sectors[2] = existing;

    std::array<uint8_t, 2352> appended{};
    appended.fill(0x42);
    appended[15] = 2;
    appended[18] = 0;
    appended[24] = 0xb6;
    output.appended_start_lba = base_sector_count;
    output.appended_raw_sectors.push_back(appended);
    output.sector_count = base_sector_count + 1;

    if (indexed_handler_invalid_extension) {
        output.appended_raw_sectors.clear();
        return true;
    }

    if (!indexed_handler_fail) return true;
    output.appended_raw_sectors.push_back(appended);
    output.sector_count = base_sector_count + 2;
    if (error) *error = "deliberate indexed handler failure";
    return false;
}

static void write_indexed_fixture(const fs::path& root,
                                   const std::string& disc_hash,
                                  const std::vector<uint8_t>& payload,
                                  const std::string& extra_manifest = {},
                                  const std::string& extra_state = {}) {
    const fs::path package = root / "packages/runtime.indexed/1.0.0";
    write_bytes(package / "assets/payload.bin", payload);
    write_text(package / "manifest.toml",
        "format_version = 6\n"
        "id = \"runtime.indexed\"\n"
        "version = \"1.0.0\"\n"
        "name = \"Runtime Indexed\"\n"
        "[[target]]\n"
        "game_id = \"SLUS-INDEXED-RUNTIME\"\n"
        "disc_sha256 = \"" + disc_hash + "\"\n"
        "[[feature]]\n"
        "id = \"indexed\"\n"
        "name = \"Indexed\"\n"
        "[[feature]]\n"
        "id = \"conflict\"\n"
        "name = \"Conflict\"\n"
        "[[indexed_file]]\n"
        "feature = \"indexed\"\n"
        "format = \"runtime-indexed\"\n"
        "index = 7\n"
        "file = \"assets/payload.bin\"\n"
        "sha256 = \"" + sha256_hex(payload) + "\"\n"
        "expected_sha256 = \"" + std::string(64, '1') + "\"\n" +
        extra_manifest);
    write_text(root / "state.toml",
        "format_version = 2\n"
        "[[package]]\n"
        "id = \"runtime.indexed\"\n"
        "version = \"1.0.0\"\n"
        "[[feature]]\n"
        "package_id = \"runtime.indexed\"\n"
        "id = \"indexed\"\n"
        "enabled = true\n" + extra_state);
}

int main() {
    const fs::path root = fs::temp_directory_path() / "psxrecomp-mod-runtime-test";
    std::error_code ec;
    fs::remove_all(root, ec);
    const std::vector<uint8_t> stock(8 * 2352, 0);
    std::vector<uint8_t> overlay(3000);
    for (size_t i = 0; i < overlay.size(); ++i)
        overlay[i] = (uint8_t)(i * 17u + 3u);
    const fs::path stock_path = root / "stock.bin";
    write_bytes(stock_path, stock);
    write_bytes(root / "audio.bin", std::vector<uint8_t>(2 * 2352, 0x5a));
    const fs::path cue_path = root / "stock.cue";
    write_text(cue_path,
        "FILE \"stock.bin\" BINARY\n"
        "  TRACK 01 MODE2/2352\n"
        "    INDEX 01 00:00:00\n"
        "FILE \"audio.bin\" BINARY\n"
        "  TRACK 02 AUDIO\n"
        "    INDEX 01 00:00:00\n");
    std::string mounted_disc_hash;
    std::string mounted_disc_error;
    check(PSXRecompV4::mod_runtime_compute_disc_sha256(
              cue_path, mounted_disc_hash, &mounted_disc_error),
          mounted_disc_error.c_str());
    std::string upgraded_bin_hash;
    check(PSXRecompV4::mod_runtime_compute_disc_sha256(
              stock_path, upgraded_bin_hash, &mounted_disc_error) &&
              upgraded_bin_hash == mounted_disc_hash,
          "a BIN upgraded to its owning CUE must share the mounted identity");
    write_bytes(root / "packages/runtime.test/1.0.0/assets/overlay.bin",
                overlay);
    write_text(root / "packages/runtime.test/1.0.0/manifest.toml",
        "format_version = 5\n"
        "id = \"runtime.test\"\n"
        "version = \"1.0.0\"\n"
        "name = \"Runtime Test\"\n"
        "[[target]]\n"
        "game_id = \"SLUS-RUNTIME\"\n"
        "disc_sha256 = \"" + mounted_disc_hash + "\"\n"
        "[[feature]]\n"
        "id = \"main-code\"\n"
        "name = \"Main Code\"\n"
        "[[feature]]\n"
        "id = \"disc-byte\"\n"
        "name = \"Disc Byte\"\n"
        "[[feature]]\n"
        "id = \"asset-overlay\"\n"
        "name = \"Asset Overlay\"\n"
        "[[feature]]\n"
        "id = \"user-byte\"\n"
        "name = \"User Byte\"\n"
        "[[feature]]\n"
        "id = \"dynamic-main\"\n"
        "name = \"Dynamic Main\"\n"
        "[[feature]]\n"
        "id = \"sparse-main\"\n"
        "name = \"Sparse Main\"\n"
        "[[feature]]\n"
        "id = \"sparse-flag\"\n"
        "name = \"Sparse Flag\"\n"
        "[[feature]]\n"
        "id = \"sparse-disc\"\n"
        "name = \"Sparse Disc\"\n"
        "[[feature]]\n"
        "id = \"vblank-plugin\"\n"
        "name = \"VBlank Plugin\"\n"
        "[[option]]\n"
        "feature = \"dynamic-main\"\n"
        "id = \"count\"\n"
        "label = \"Count\"\n"
        "type = \"integer\"\n"
        "min = 0\n"
        "max = 254\n"
        "default = 42\n"
        "[[option]]\n"
        "feature = \"sparse-main\"\n"
        "id = \"frames\"\n"
        "label = \"Frames\"\n"
        "type = \"integer\"\n"
        "min = 0\n"
        "max = 99\n"
        "default = 2\n"
        "[[patch]]\n"
        "feature = \"main-code\"\n"
        "target = \"main_exe\"\n"
        "address = 2147487744\n"
        "expected = \"01020304\"\n"
        "replace = \"a1a2a3a4\"\n"
        "[[patch]]\n"
        "feature = \"disc-byte\"\n"
        "target = \"disc_raw\"\n"
        "offset = 4714\n"
        "expected = \"aa\"\n"
        "replace = \"bb\"\n"
        "[[patch]]\n"
        "feature = \"user-byte\"\n"
        "target = \"disc_user\"\n"
        "offset = 6154\n"
        "expected = \"cc\"\n"
        "replace = \"dd\"\n"
        "[[patch]]\n"
        "feature = \"dynamic-main\"\n"
        "target = \"main_exe\"\n"
        "address = 2147488000\n"
        "expected = \"0000\"\n"
        "replace_from = { option = \"count\", encoding = \"u16le\" }\n"
        "[[patch]]\n"
        "feature = \"dynamic-main\"\n"
        "target = \"main_exe\"\n"
        "address = 2147488002\n"
        "expected = \"0100\"\n"
        "replace_from = { option = \"count\", encoding = \"u16le\", addend = 1 }\n"
        "[[patch]]\n"
        "feature = \"sparse-main\"\n"
        "target = \"main_exe\"\n"
        "address = 2147488256\n"
        "expected = \"02000132\"\n"
        "fields = [{ offset = 0, option = \"frames\", encoding = \"u8\" }]\n"
        "when_integer = { option = \"frames\", op = \"gt\", value = 0 }\n"
        "[[patch]]\n"
        "feature = \"sparse-main\"\n"
        "target = \"main_exe\"\n"
        "address = 2147488256\n"
        "expected = \"02000132\"\n"
        "fields = [{ offset = 0, replace = \"01\" }, "
        "{ offset = 2, replace = \"00\" }]\n"
        "when_integer = { option = \"frames\", op = \"eq\", value = 0 }\n"
        "[[patch]]\n"
        "feature = \"sparse-flag\"\n"
        "target = \"main_exe\"\n"
        "address = 2147488256\n"
        "expected = \"02000132\"\n"
        "fields = [{ offset = 1, replace = \"42\" }]\n"
        "[[patch]]\n"
        "feature = \"sparse-disc\"\n"
        "target = \"disc_user\"\n"
        "offset = 6164\n"
        "expected = \"11223344\"\n"
        "fields = [{ offset = 0, replace = \"aa\" }, "
        "{ offset = 2, replace = \"bb\" }]\n"
        "[[overlay]]\n"
        "feature = \"asset-overlay\"\n"
        "target = \"disc_raw\"\n"
        "offset = 11408\n"
        "file = \"assets/overlay.bin\"\n"
        "sha256 = \"" + sha256_hex(overlay) + "\"\n"
        "expected_sha256 = \"" +
            sha256_hex(std::vector<uint8_t>(overlay.size(), 0)) + "\"\n"
        "[[plugin]]\n"
        "feature = \"vblank-plugin\"\n"
        "id = \"runtime.test-vblank\"\n");
    write_text(root / "state.toml",
        "format_version = 2\n"
        "[[package]]\n"
        "id = \"runtime.test\"\n"
        "version = \"1.0.0\"\n"
        "[[feature]]\n"
        "package_id = \"runtime.test\"\n"
        "id = \"main-code\"\n"
        "enabled = true\n"
        "[[feature]]\n"
        "package_id = \"runtime.test\"\n"
        "id = \"disc-byte\"\n"
        "enabled = true\n"
        "[[feature]]\n"
        "package_id = \"runtime.test\"\n"
        "id = \"asset-overlay\"\n"
        "enabled = true\n"
        "[[feature]]\n"
        "package_id = \"runtime.test\"\n"
        "id = \"user-byte\"\n"
        "enabled = true\n"
        "[[feature]]\n"
        "package_id = \"runtime.test\"\n"
        "id = \"dynamic-main\"\n"
        "enabled = true\n"
        "[feature.values]\n"
        "count = 42\n"
        "[[feature]]\n"
        "package_id = \"runtime.test\"\n"
        "id = \"sparse-main\"\n"
        "enabled = true\n"
        "[feature.values]\n"
        "frames = 0\n"
        "[[feature]]\n"
        "package_id = \"runtime.test\"\n"
        "id = \"sparse-flag\"\n"
        "enabled = true\n"
        "[[feature]]\n"
        "package_id = \"runtime.test\"\n"
        "id = \"sparse-disc\"\n"
        "enabled = true\n"
        "[[feature]]\n"
        "package_id = \"runtime.test\"\n"
        "id = \"vblank-plugin\"\n"
        "enabled = true\n");

    std::string error;
    PSXRecompV4::mod_clear_plugins_for_tests();
    check(PSXRecompV4::mod_register_activation_plugin(
              "runtime.test-vblank", test_activation_plugin),
          "runtime test activation hook must register");
    check(PSXRecompV4::mod_register_vblank_plugin(
              "runtime.test-vblank", test_vblank_plugin),
          "runtime test plugin must register");
    check(PSXRecompV4::mod_runtime_initialize(
              root, "SLUS-RUNTIME", 0x80002000, {}, &error),
          error.c_str());
    check(PSXRecompV4::mod_runtime_commit(cue_path, &error),
          "CUE and its data-track BIN must have the same mod target identity");
    mod_runtime_activate_plugins();
    check(activation_calls == 1,
          "resolved trusted plugin must activate before runtime startup");
    mod_runtime_on_vblank();
    check(plugin_calls == 1,
          "resolved trusted plugin must run on guest VBlank");

    ram[0x1000] = 1; ram[0x1001] = 2; ram[0x1002] = 3; ram[0x1003] = 4;
    ram[0x1100] = 0; ram[0x1101] = 0;
    ram[0x1102] = 1; ram[0x1103] = 0;
    ram[0x1200] = 2; ram[0x1201] = 0;
    ram[0x1202] = 1; ram[0x1203] = 0x32;
    mod_runtime_on_dispatch(0x80001000);
    check(ram[0x1000] == 1, "patch must wait for the configured entry point");
    mod_runtime_on_dispatch(0x80002000);
    check(ram[0x1000] == 0xa1 && ram[0x1003] == 0xa4,
          "main-EXE patch must apply before entry execution");
    check(ram[0x1100] == 42 && ram[0x1101] == 0 &&
              ram[0x1102] == 43 && ram[0x1103] == 0,
          "dynamic main-EXE patches must encode all sites before entry");
    check(ram[0x1200] == 1 && ram[0x1201] == 0x42 &&
              ram[0x1202] == 0 && ram[0x1203] == 0x32,
          "adjacent sparse fields must compose while preserving guard-only "
          "bytes");

    /* A full-machine savestate replaces main RAM after the entry-point plan
     * has already applied. Loading a stock checkpoint must not silently turn
     * the currently enabled main-EXE features back off. */
    ram[0x1000] = 1; ram[0x1001] = 2; ram[0x1002] = 3; ram[0x1003] = 4;
    ram[0x1100] = 0; ram[0x1101] = 0;
    ram[0x1102] = 1; ram[0x1103] = 0;
    ram[0x1200] = 2; ram[0x1201] = 0;
    ram[0x1202] = 1; ram[0x1203] = 0x32;
    mod_runtime_on_savestate_loaded();
    check(ram[0x1000] == 0xa1 && ram[0x1003] == 0xa4 &&
              ram[0x1100] == 42 && ram[0x1102] == 43 &&
              ram[0x1200] == 1 && ram[0x1201] == 0x42 &&
              ram[0x1202] == 0 && ram[0x1203] == 0x32,
          "savestate restore must reapply the complete enabled main plan");

    std::array<uint8_t, 2352> sector{};
    sector[10] = 0xaa;
    mod_runtime_patch_disc_sector(2, 1, sector.data(), (uint32_t)sector.size());
    check(sector[10] == 0xaa, "disc overlay must stay off during reference reads");
    mod_runtime_enable_disc_patches();
    mod_runtime_patch_disc_sector(2, 1, sector.data(), (uint32_t)sector.size());
    check(sector[10] == 0xbb, "raw disc overlay must patch matching sectors");

    std::array<uint8_t, 2352> overlay_sector{};
    mod_runtime_patch_disc_sector(
        4, 1, overlay_sector.data(), (uint32_t)overlay_sector.size());
    check(overlay_sector[1999] == 0 &&
              overlay_sector[2000] == overlay[0] &&
              overlay_sector[2351] == overlay[351],
          "file overlay must patch the tail of its first sector");
    overlay_sector.fill(0);
    mod_runtime_patch_disc_sector(
        5, 1, overlay_sector.data(), (uint32_t)overlay_sector.size());
    check(overlay_sector.front() == overlay[352] &&
              overlay_sector.back() == overlay[2703],
          "file overlay must patch complete middle sectors");
    overlay_sector.fill(0);
    mod_runtime_patch_disc_sector(
        6, 1, overlay_sector.data(), (uint32_t)overlay_sector.size());
    check(overlay_sector[0] == overlay[2704] &&
              overlay_sector[295] == overlay[2999] &&
              overlay_sector[296] == 0,
          "file overlay must patch the head of its final sector");

    std::array<uint8_t, 2352> mode2_sector{};
    mode2_sector[15] = 2;
    mode2_sector[18] = 0;
    mode2_sector[24 + 10] = 0xcc;
    mode2_sector[24 + 20] = 0x11;
    mode2_sector[24 + 21] = 0x22;
    mode2_sector[24 + 22] = 0x33;
    mode2_sector[24 + 23] = 0x44;
    mod_runtime_patch_disc_sector(
        3, 1, mode2_sector.data(), (uint32_t)mode2_sector.size());
    check(mode2_sector[24 + 10] == 0xdd,
          "disc_user operations must apply to raw Mode2 Form1 user data");
    check(mode2_sector[24 + 20] == 0xaa &&
              mode2_sector[24 + 21] == 0x22 &&
              mode2_sector[24 + 22] == 0xbb &&
              mode2_sector[24 + 23] == 0x44,
          "sparse disc writes must validate a complete guard and modify only "
          "owned fields");
    std::array<uint8_t, 2352> audio_sector{};
    audio_sector[24 + 10] = 0xcc;
    mod_runtime_patch_disc_sector(
        3, 1, audio_sector.data(), (uint32_t)audio_sector.size());
    check(audio_sector[24 + 10] == 0xcc,
          "disc_user operations must not modify CDDA/non-data sectors");

    check(PSXRecompV4::mod_runtime_initialize(
              root, "SLUS-RUNTIME", 0x80002000, {}, &error),
          error.c_str());
    check(PSXRecompV4::mod_runtime_commit(stock_path, &error), error.c_str());
    ram[0x1000] = 1; ram[0x1001] = 2; ram[0x1002] = 3; ram[0x1003] = 4;
    ram[0x1100] = 0; ram[0x1101] = 0;
    ram[0x1102] = 2; ram[0x1103] = 0; /* second dynamic guard is wrong */
    mod_runtime_on_dispatch(0x80002000);
    check(ram[0x1000] == 1 && ram[0x1003] == 4 &&
              ram[0x1100] == 0 && ram[0x1101] == 0,
          "one failed generated guard must leave the complete main plan untouched");

    check(PSXRecompV4::mod_runtime_initialize(
              root, "SLUS-RUNTIME", 0x80002000, {}, &error),
          error.c_str());
    check(PSXRecompV4::mod_runtime_commit(stock_path, &error), error.c_str());
    ram[0x1000] = 1; ram[0x1001] = 2; ram[0x1002] = 3; ram[0x1003] = 4;
    ram[0x1100] = 0; ram[0x1101] = 0;
    ram[0x1102] = 1; ram[0x1103] = 0;
    ram[0x1200] = 2; ram[0x1201] = 0;
    ram[0x1202] = 1; ram[0x1203] = 0x33; /* guard-only byte is wrong */
    mod_runtime_on_dispatch(0x80002000);
    check(ram[0x1000] == 1 && ram[0x1003] == 4 &&
              ram[0x1200] == 2 && ram[0x1201] == 0 &&
              ram[0x1202] == 1 && ram[0x1203] == 0x33,
          "a failed sparse guard-only byte must leave the complete main plan "
          "untouched");

    /* Loading can be requested while the boot executable is still running,
     * before the configured game entry point has dispatched. The restored
     * checkpoint itself supplies the bytes used to validate and apply the
     * plan in that case. */
    check(PSXRecompV4::mod_runtime_initialize(
              root, "SLUS-RUNTIME", 0x80002000, {}, &error),
          error.c_str());
    check(PSXRecompV4::mod_runtime_commit(stock_path, &error), error.c_str());
    ram[0x1000] = 1; ram[0x1001] = 2; ram[0x1002] = 3; ram[0x1003] = 4;
    ram[0x1100] = 0; ram[0x1101] = 0;
    ram[0x1102] = 1; ram[0x1103] = 0;
    ram[0x1200] = 2; ram[0x1201] = 0;
    ram[0x1202] = 1; ram[0x1203] = 0x32;
    mod_runtime_on_savestate_loaded();
    check(ram[0x1000] == 0xa1 && ram[0x1003] == 0xa4 &&
              ram[0x1100] == 42 && ram[0x1102] == 43 &&
              ram[0x1200] == 1 && ram[0x1201] == 0x42 &&
              ram[0x1202] == 0 && ram[0x1203] == 0x32,
          "pre-entry savestate restore must validate and apply the main plan");

    mod_runtime_enable_disc_patches();
    std::array<uint8_t, 2352> bad_sparse_disc{};
    bad_sparse_disc[15] = 2;
    bad_sparse_disc[18] = 0;
    bad_sparse_disc[24 + 10] = 0xcc;
    bad_sparse_disc[24 + 20] = 0x11;
    bad_sparse_disc[24 + 21] = 0x99; /* guard-only byte is wrong */
    bad_sparse_disc[24 + 22] = 0x33;
    bad_sparse_disc[24 + 23] = 0x44;
    mod_runtime_patch_disc_sector(
        3, 1, bad_sparse_disc.data(),
        (uint32_t)bad_sparse_disc.size());
    check(bad_sparse_disc[24 + 10] == 0xcc &&
              bad_sparse_disc[24 + 20] == 0x11 &&
              bad_sparse_disc[24 + 22] == 0x33,
           "a failed sparse disc guard must leave every write in the sector "
           "untouched");

    const std::vector<uint8_t> indexed_payload = {0xde, 0xad, 0xbe, 0xef};
    const fs::path indexed_disc_path = root / "indexed-stock.bin";
    write_bytes(indexed_disc_path, stock);
    std::string indexed_disc_hash;
    check(PSXRecompV4::mod_runtime_compute_disc_sha256(
              indexed_disc_path, indexed_disc_hash, &error),
          error.c_str());
    check(indexed_disc_hash != mounted_disc_hash,
          "track geometry and content outside the data BIN must affect identity");
    const fs::path indexed_root = root / "indexed-success";
    write_indexed_fixture(indexed_root, indexed_disc_hash, indexed_payload);
    check(PSXRecompV4::mod_runtime_register_indexed_file_handler(
              "runtime-indexed", test_indexed_file_handler),
          "dummy indexed-file handler must register");
    indexed_handler_fail = false;
    check(PSXRecompV4::mod_runtime_initialize(
              indexed_root, "SLUS-INDEXED-RUNTIME", 0x80002000, {}, &error),
          error.c_str());
    check(PSXRecompV4::mod_runtime_commit(indexed_disc_path, &error), error.c_str());
    check(indexed_handler_calls == 1 &&
              indexed_handler_disc_path ==
                  fs::absolute(indexed_disc_path).string() &&
              indexed_handler_base_sector_count > 2 &&
              indexed_handler_files.size() == 1 &&
              indexed_handler_files[0].index == 7 &&
              indexed_handler_files[0].payload == indexed_payload,
          "commit must pass the resolved payload and base sector count to the handler");
    PS1::ISOReader* transferred_reader =
        PSXRecompV4::mod_runtime_take_verified_disc(indexed_disc_path);
    check(transferred_reader == indexed_handler_reader &&
              PSXRecompV4::mod_runtime_requires_verified_disc() &&
              PSXRecompV4::mod_runtime_take_verified_disc(indexed_disc_path) == nullptr,
          "indexed plans must transfer the exact authenticated reader once");
    auto* unrelated_reader = new PS1::ISOReader();
    check(!PSXRecompV4::mod_runtime_return_verified_disc(unrelated_reader),
          "verified-reader ownership must reject unrelated handles");
    delete unrelated_reader;
    check(PSXRecompV4::mod_runtime_return_verified_disc(transferred_reader) &&
              PSXRecompV4::mod_runtime_take_verified_disc(indexed_disc_path) ==
                  transferred_reader &&
              PSXRecompV4::mod_runtime_return_verified_disc(transferred_reader),
          "verified readers must survive CD-ROM close and reopen cycles");
    const uint32_t indexed_base_sector_count =
        indexed_handler_base_sector_count;

    std::array<uint8_t, 2352> virtual_sector{};
    check(mod_runtime_read_virtual_raw_sector(
              2, virtual_sector.data(), (uint32_t)virtual_sector.size()) &&
              virtual_sector[24] == 0xa5,
          "committed indexed plan must publish an existing-LBA override");
    virtual_sector.fill(0);
    check(mod_runtime_read_virtual_raw_sector(
              indexed_base_sector_count, virtual_sector.data(),
              (uint32_t)virtual_sector.size()) &&
              virtual_sector[24] == 0xb6 &&
              mod_runtime_effective_sector_count(indexed_base_sector_count) ==
                  indexed_base_sector_count + 1,
          "committed indexed plan must publish appended sectors and extend the disc");

    std::vector<uint8_t> changed_stock = stock;
    changed_stock[0] = 1;
    write_bytes(indexed_disc_path, changed_stock);
    const int calls_before_rehash = indexed_handler_calls;
    check(!PSXRecompV4::mod_runtime_commit(indexed_disc_path, &error) &&
              error.find("package does not target this game/image") !=
                  std::string::npos &&
              indexed_handler_calls == calls_before_rehash,
          "commit must rehash a disc changed in place before invoking handlers");
    check(!mod_runtime_read_virtual_raw_sector(
              indexed_base_sector_count, virtual_sector.data(),
              (uint32_t)virtual_sector.size()) &&
              mod_runtime_effective_sector_count(indexed_base_sector_count) ==
                  indexed_base_sector_count,
          "disc identity changes must quarantine the prior virtual-disc plan");
    write_bytes(indexed_disc_path, stock);
    check(PSXRecompV4::mod_runtime_commit(indexed_disc_path, &error), error.c_str());

    indexed_handler_invalid_extension = true;
    check(!PSXRecompV4::mod_runtime_commit(indexed_disc_path, &error) &&
              error.find("invalid contiguous extension") != std::string::npos,
          "indexed handlers must cover every advertised appended sector");
    indexed_handler_invalid_extension = false;

    indexed_handler_fail = true;
    check(!PSXRecompV4::mod_runtime_commit(indexed_disc_path, &error) &&
              error.find("deliberate indexed handler failure") != std::string::npos,
          "handler failure must reject the replacement plan");
    virtual_sector.fill(0);
    check(mod_runtime_read_virtual_raw_sector(
              indexed_base_sector_count, virtual_sector.data(),
              (uint32_t)virtual_sector.size()) &&
              virtual_sector[24] == 0xb6 &&
              !mod_runtime_read_virtual_raw_sector(
                  indexed_base_sector_count + 1, virtual_sector.data(),
                  (uint32_t)virtual_sector.size()) &&
              mod_runtime_effective_sector_count(indexed_base_sector_count) ==
                  indexed_base_sector_count + 1,
          "handler failure must retain the prior plan without publishing partial output");

    check(PSXRecompV4::mod_runtime_clear_for_netplay(&error), error.c_str());
    check(!mod_runtime_read_virtual_raw_sector(
              2, virtual_sector.data(), (uint32_t)virtual_sector.size()) &&
              !mod_runtime_read_virtual_raw_sector(
                  indexed_base_sector_count, virtual_sector.data(),
                  (uint32_t)virtual_sector.size()) &&
              mod_runtime_effective_sector_count(indexed_base_sector_count) ==
                  indexed_base_sector_count,
          "netplay clearing must remove virtual sectors and the extended count");

    const fs::path audio_tail_root = root / "indexed-audio-tail";
    write_indexed_fixture(
        audio_tail_root, mounted_disc_hash, indexed_payload);
    indexed_handler_fail = false;
    const int calls_before_audio_tail = indexed_handler_calls;
    check(PSXRecompV4::mod_runtime_initialize(
              audio_tail_root, "SLUS-INDEXED-RUNTIME", 0x80002000, {}, &error),
          error.c_str());
    check(!PSXRecompV4::mod_runtime_commit(cue_path, &error) &&
              error.find("final data track") != std::string::npos &&
              indexed_handler_calls == calls_before_audio_tail,
          "indexed virtual extensions must reject a final audio track before building");

    const auto check_indexed_conflict = [&](const std::string& name,
                                             const std::string& manifest,
                                             const std::string& state) {
        const fs::path conflict_root = root / name;
        write_indexed_fixture(
              conflict_root, indexed_disc_hash, indexed_payload, manifest, state);
        const int calls_before = indexed_handler_calls;
        check(PSXRecompV4::mod_runtime_initialize(
                  conflict_root, "SLUS-INDEXED-RUNTIME", 0x80002000, {}, &error),
              error.c_str());
        check(!PSXRecompV4::mod_runtime_commit(indexed_disc_path, &error) &&
                  error.find("indexed-file plans cannot be combined") !=
                      std::string::npos &&
                  indexed_handler_calls == calls_before,
              ("indexed files must reject " + name + " before invoking the handler").c_str());
    };

    const std::string conflict_feature_state =
        "[[feature]]\n"
        "package_id = \"runtime.indexed\"\n"
        "id = \"conflict\"\n"
        "enabled = true\n";
    check_indexed_conflict(
        "disc-write",
        "[[patch]]\n"
        "feature = \"conflict\"\n"
        "target = \"disc_raw\"\n"
        "offset = 0\n"
        "expected = \"00\"\n"
        "replace = \"01\"\n",
        conflict_feature_state);

    const fs::path overlay_root = root / "disc-overlay";
    write_bytes(overlay_root /
                    "packages/runtime.indexed/1.0.0/assets/overlay.bin",
                {0x77});
    check_indexed_conflict(
        "disc-overlay",
        "[[overlay]]\n"
        "feature = \"conflict\"\n"
        "target = \"disc_raw\"\n"
        "offset = 0\n"
        "file = \"assets/overlay.bin\"\n"
        "sha256 = \"" + sha256_hex({0x77}) + "\"\n",
        conflict_feature_state);

    const fs::path derived_root = root / "derived-disc";
    write_indexed_fixture(
        derived_root, indexed_disc_hash, indexed_payload, {},
        "[[package]]\n"
        "id = \"runtime.derived\"\n"
        "version = \"1.0.0\"\n"
        "enabled = true\n");
    const fs::path derived_package =
        derived_root / "packages/runtime.derived/1.0.0";
    write_bytes(derived_package / "assets/change.xdelta3", {0x01});
    write_text(derived_package / "manifest.toml",
        "format_version = 1\n"
        "id = \"runtime.derived\"\n"
        "version = \"1.0.0\"\n"
        "name = \"Runtime Derived\"\n"
        "[[target]]\n"
        "game_id = \"SLUS-INDEXED-RUNTIME\"\n"
        "disc_sha256 = \"" + indexed_disc_hash + "\"\n"
        "[[derived_disc]]\n"
        "kind = \"vcdiff\"\n"
        "patch = \"assets/change.xdelta3\"\n"
        "patch_sha256 = \"" + std::string(64, '2') + "\"\n"
        "output_size = 1\n"
        "output_sha256 = \"" + std::string(64, '3') + "\"\n");
    const int calls_before_derived = indexed_handler_calls;
    check(PSXRecompV4::mod_runtime_initialize(
              derived_root, "SLUS-INDEXED-RUNTIME", 0x80002000, {}, &error),
          error.c_str());
    check(!PSXRecompV4::mod_runtime_commit(indexed_disc_path, &error) &&
              error.find("indexed-file plans cannot be combined") !=
                  std::string::npos &&
              indexed_handler_calls == calls_before_derived,
          "indexed files must reject derived discs before invoking the handler");

    fs::remove_all(root, ec);
    if (failures) return 1;
    std::cout << "mod runtime tests passed\n";
    return 0;
}
