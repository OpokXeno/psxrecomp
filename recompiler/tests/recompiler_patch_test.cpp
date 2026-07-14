#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "code_generator.h"
#include "config_loader.h"
#include "control_flow.h"
#include "fmt/format.h"
#include "recompiler_patch.h"

namespace fs = std::filesystem;
using PSXRecompV4::RecompilerPatch;

namespace {

int failures = 0;

void check(bool condition, const std::string& name) {
    if (condition) {
        fmt::print("PASS  {}\n", name);
    } else {
        fmt::print(stderr, "FAIL  {}\n", name);
        ++failures;
    }
}

template <typename Fn>
void check_throws(Fn&& fn, const std::string& needle,
                  const std::string& name) {
    try {
        fn();
        check(false, name + " (did not throw)");
    } catch (const std::exception& e) {
        check(std::string(e.what()).find(needle) != std::string::npos,
              name + " (" + e.what() + ")");
    }
}

void append_word(std::vector<uint8_t>& bytes, uint32_t word) {
    bytes.push_back(static_cast<uint8_t>(word));
    bytes.push_back(static_cast<uint8_t>(word >> 8));
    bytes.push_back(static_cast<uint8_t>(word >> 16));
    bytes.push_back(static_cast<uint8_t>(word >> 24));
}

std::string generate_first_instruction(uint32_t first_word,
                                       std::vector<RecompilerPatch> patches,
                                       bool overlay_mode) {
    constexpr uint32_t base = 0x80010000u;
    PSXRecomp::PS1Executable exe{};
    exe.header.load_address = base;
    exe.header.initial_pc = base;
    exe.header.file_size = 20;
    append_word(exe.code_data, first_word);
    append_word(exe.code_data, 0x00000000u); // branch/nonbranch delay-slot nop
    append_word(exe.code_data, 0x24030001u); // addiu v1, zero, 1
    append_word(exe.code_data, 0x03E00008u); // jr ra
    append_word(exe.code_data, 0x00000000u); // return delay-slot nop

    PSXRecompV4::apply_recompiler_patches_to_executable(
        exe, patches, overlay_mode);

    PSXRecomp::Function function{};
    function.start_addr = base;
    function.end_addr = base + 20;
    function.size = 20;
    function.name = "patch_test";

    PSXRecomp::ControlFlowAnalyzer analyzer(exe);
    const auto cfg = analyzer.analyze_function(function);
    PSXRecomp::CodeGenConfig config;
    config.overlay_mode = overlay_mode;
    PSXRecomp::CodeGenerator generator(exe, config);
    return generator.generate_function(function, cfg).full_code;
}

std::string base_config() {
    return R"toml([game]
name = "Patch Test"
id = "TEST-00000"
exe = "TEST.EXE"
load_address = "0x80010000"
entry_pc = "0x80010000"
text_size = "0x1000"
stack_base = "0x801FFFF0"

[recompiler]
seeds = "seeds.txt"
out_dir = "generated"
)toml";
}

fs::path write_config(const fs::path& root, const std::string& suffix,
                      const std::string& patch_tables) {
    const fs::path path = root / ("game-" + suffix + ".toml");
    std::ofstream file(path, std::ios::binary);
    file << base_config() << patch_tables;
    file.close();
    return path;
}

void parser_tests(const fs::path& root) {
    const auto valid = write_config(root, "valid", R"toml(
[[recompiler.patch]]
id = "gameplay-rate"
address = "0x80012340"
expected = "0x24020002"
replacement = "0x24020001"
note = "Test-only fixture"
)toml");
    const auto config = PSXRecompV4::load_game_config(valid);
    check(config.recompiler_patches.size() == 1,
          "parser accepts one guarded patch");
    check(config.recompiler_patches[0].id == "gameplay-rate" &&
          config.recompiler_patches[0].address == 0x80012340u &&
          config.recompiler_patches[0].expected == 0x24020002u &&
          config.recompiler_patches[0].replacement == 0x24020001u,
          "parser preserves patch fields");

    const auto duplicate_address = write_config(root, "duplicate-address", R"toml(
[[recompiler.patch]]
id = "first"
address = "0x80012340"
expected = "0x24020002"
replacement = "0x24020001"

[[recompiler.patch]]
id = "alias"
address = "0xA0012340"
expected = "0x24020002"
replacement = "0x24020001"
)toml");
    check_throws(
        [&] { (void)PSXRecompV4::load_game_config(duplicate_address); },
        "physical address", "parser rejects duplicate aliased addresses");

    const auto duplicate_id = write_config(root, "duplicate-id", R"toml(
[[recompiler.patch]]
id = "same-id"
address = "0x80012340"
expected = "0x24020002"
replacement = "0x24020001"

[[recompiler.patch]]
id = "same-id"
address = "0x80012344"
expected = "0x24030002"
replacement = "0x24030001"
)toml");
    check_throws([&] { (void)PSXRecompV4::load_game_config(duplicate_id); },
                 "duplicate [[recompiler.patch]] id",
                 "parser rejects duplicate IDs");

    const auto unaligned = write_config(root, "unaligned", R"toml(
[[recompiler.patch]]
id = "unaligned"
address = "0x80012342"
expected = "0x24020002"
replacement = "0x24020001"
)toml");
    check_throws([&] { (void)PSXRecompV4::load_game_config(unaligned); },
                 "instruction-aligned", "parser rejects unaligned sites");
}

void codegen_tests() {
    constexpr uint32_t original = 0x24020002u;    // addiu v0, zero, 2
    constexpr uint32_t replacement = 0x24020001u; // addiu v0, zero, 1
    const RecompilerPatch physical_alias{
        "gameplay-rate", 0x00010000u, original, replacement, ""};

    const std::string applied =
        generate_first_instruction(original, {physical_alias}, false);
    check(applied.find("0x24020001") != std::string::npos &&
          applied.find("cpu->gpr[2] = 1;") != std::string::npos,
          "codegen applies exact patch through physical alias");

    check_throws(
        [&] { (void)generate_first_instruction(0x24020003u,
                                               {physical_alias}, false); },
        "wrong game revision or stale patch",
        "main codegen fails on stale opcode guard");

    const std::string overlay =
        generate_first_instruction(0x24020003u, {physical_alias}, true);
    check(overlay.find("0x24020003") != std::string::npos &&
          overlay.find("0x24020001") == std::string::npos,
          "overlay nonmatching variant remains unchanged");

    constexpr uint32_t beq_v0_zero = 0x10400002u;
    constexpr uint32_t bne_v0_zero = 0x14400002u;
    const RecompilerPatch branch_patch{
        "feature-branch", 0x80010000u, beq_v0_zero, bne_v0_zero, ""};
    const std::string branch =
        generate_first_instruction(beq_v0_zero, {branch_patch}, false);
    check(branch.find("cpu->gpr[2] != cpu->gpr[0]") != std::string::npos,
          "patch is applied before control-flow analysis");

    std::vector<RecompilerPatch> merged{physical_alias};
    PSXRecompV4::merge_recompiler_patches(merged, {physical_alias});
    check(merged.size() == 1,
          "config merge deduplicates an identical patch");

    const RecompilerPatch conflicting_id{
        "gameplay-rate", 0x00010004u, original, replacement, ""};
    check_throws(
        [&] {
            PSXRecompV4::merge_recompiler_patches(merged, {conflicting_id});
        },
        "conflicting recompiler patches",
        "config merge rejects cross-config ID conflicts");
}

} // namespace

int main() {
    const fs::path root = fs::temp_directory_path() /
        fmt::format("psxrecomp-patch-test-{}", reinterpret_cast<uintptr_t>(&failures));
    fs::remove_all(root);
    fs::create_directories(root);

    try {
        parser_tests(root);
        codegen_tests();
    } catch (const std::exception& e) {
        fmt::print(stderr, "FAIL  unexpected exception: {}\n", e.what());
        ++failures;
    }

    std::error_code ec;
    fs::remove_all(root, ec);
    fmt::print("\nGuarded recompiler patches: {}\n",
               failures == 0 ? "all tests passed" : "failures detected");
    return failures == 0 ? 0 : 1;
}
