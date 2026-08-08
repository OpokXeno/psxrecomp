#include <cstdint>
#include <string>
#include <vector>

#include "code_generator.h"
#include "control_flow.h"
#include "fmt/format.h"

namespace {

constexpr uint32_t kProducerEntry = 0x80075B44u;
constexpr uint32_t kCaptureSite = 0x800781BCu;
constexpr uint32_t kStaticCallee = 0x8004B54Cu;
constexpr uint32_t kReturnSite = 0x800781C4u;
constexpr uint32_t kJalInstruction = 0x0C012D53u;

int failures = 0;

size_t count_occurrences(const std::string &text, const std::string &needle) {
    size_t count = 0u;
    size_t offset = 0u;

    while ((offset = text.find(needle, offset)) != std::string::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

void check(bool condition, const char *message) {
    if (condition) {
        fmt::print("PASS  {}\n", message);
    } else {
        fmt::print(stderr, "FAIL  {}\n", message);
        ++failures;
    }
}

void write_word(PSXRecomp::PS1Executable &executable,
                uint32_t address,
                uint32_t word) {
    const size_t offset = static_cast<size_t>(address - executable.header.load_address);
    executable.code_data[offset] = static_cast<uint8_t>(word);
    executable.code_data[offset + 1u] = static_cast<uint8_t>(word >> 8u);
    executable.code_data[offset + 2u] = static_cast<uint8_t>(word >> 16u);
    executable.code_data[offset + 3u] = static_cast<uint8_t>(word >> 24u);
}

PSXRecomp::PS1Executable make_executable(uint32_t call_instruction,
                                         uint32_t delay_slot_instruction) {
    PSXRecomp::PS1Executable executable{};

    executable.header.load_address = kProducerEntry;
    executable.header.initial_pc = kProducerEntry;
    executable.code_data.assign((kReturnSite + 12u) - kProducerEntry, 0u);
    executable.header.file_size = static_cast<uint32_t>(executable.code_data.size());
    write_word(executable, kProducerEntry, 0x24010007u);
    write_word(executable, kCaptureSite, call_instruction);
    write_word(executable, kCaptureSite + 4u, delay_slot_instruction);
    write_word(executable, kReturnSite, 0x24030001u);
    write_word(executable, kReturnSite + 4u, 0x03E00008u);
    return executable;
}

std::string generate(uint32_t call_instruction,
                     uint32_t delay_slot_instruction,
                     bool overlay_mode) {
    PSXRecomp::PS1Executable executable = make_executable(call_instruction,
                                                           delay_slot_instruction);
    PSXRecomp::Function function{};
    PSXRecomp::CodeGenConfig config{};

    function.start_addr = kProducerEntry;
    function.end_addr = kReturnSite + 12u;
    function.size = function.end_addr - function.start_addr;
    function.name = "static_provenance_fixture";
    config.overlay_mode = overlay_mode;

    PSXRecomp::ControlFlowAnalyzer analyzer(executable);
    const PSXRecomp::ControlFlowGraph cfg = analyzer.analyze_function(function);
    PSXRecomp::CodeGenerator generator(executable, config);
    return generator.generate_function(function, cfg).full_code;
}

void test_hooks_emit_only_for_authenticated_static_tuple() {
    const std::string output = generate(kJalInstruction, 0u, false);

    check(count_occurrences(output,
                            "psx_xg_render_static_auth_entry(0x80075B44u)") == 1u,
          "static producer entry emits its provenance hook");
    check(count_occurrences(output,
                            "psx_xg_render_static_auth_capture(0x800781BCu, "
                            "0x8004B54Cu, 0x800781C4u, 0x0C012D53u, 0x00000000u)") == 1u,
          "static capture emits the exact caller callee return and instruction tuple");
    check(count_occurrences(output,
                              "psx_xg_render_static_auth_return(0x800781C4u, cpu->gpr[31])") == 1u,
          "static return carries the live return address");
    check(output.find("cpu->gpr[1] = 7;") != std::string::npos,
          "unrelated instruction generation is preserved");
}

void test_overlay_and_invalid_variants_emit_no_static_hooks() {
    const std::string overlay = generate(kJalInstruction, 0u, true);
    const std::string invalid_call = generate(0u, 0u, false);
    const std::string invalid_delay = generate(kJalInstruction, 0x08000000u, false);

    check(overlay.find("psx_xg_render_static_auth_") == std::string::npos,
          "overlay output receives no static-only provenance calls");
    check(invalid_call.find("psx_xg_render_static_auth_") == std::string::npos,
          "invalid call opcode emits no static provenance hooks");
    check(invalid_delay.find("psx_xg_render_static_auth_") == std::string::npos,
          "control-transfer delay slot emits no static provenance hooks");
}

}

int main() {
    test_hooks_emit_only_for_authenticated_static_tuple();
    test_overlay_and_invalid_variants_emit_no_static_hooks();
    return failures == 0 ? 0 : 1;
}
