#include "code_generator.h"
#include "control_flow.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace {

void append_word(std::vector<uint8_t> &bytes, uint32_t word) {
    bytes.push_back(static_cast<uint8_t>(word));
    bytes.push_back(static_cast<uint8_t>(word >> 8));
    bytes.push_back(static_cast<uint8_t>(word >> 16));
    bytes.push_back(static_cast<uint8_t>(word >> 24));
}

std::string generate(bool overlay_mode) {
    constexpr uint32_t base = 0x80010000u;
    PSXRecomp::PS1Executable executable{};
    executable.header.load_address = base;
    executable.header.initial_pc = base;
    executable.header.file_size = 20u;
    append_word(executable.code_data, 0x4A000006u); // NCLIP
    append_word(executable.code_data, 0x00000000u);
    append_word(executable.code_data, 0x24030001u);
    append_word(executable.code_data, 0x03E00008u);
    append_word(executable.code_data, 0x00000000u);

    PSXRecomp::Function function{};
    function.start_addr = base;
    function.end_addr = base + 20u;
    function.size = 20u;
    function.name = "gte_exact_pc";

    PSXRecomp::ControlFlowAnalyzer analyzer(executable);
    const PSXRecomp::ControlFlowGraph cfg =
        analyzer.analyze_function(function);
    PSXRecomp::CodeGenConfig config{};
    config.overlay_mode = overlay_mode;
    PSXRecomp::CodeGenerator generator(executable, config);
    return generator.generate_function(function, cfg).full_code;
}

bool has_exact_call_only(const std::string &code) {
    return code.find(
               "gte_execute_at(cpu, 0x0000006, 0x80010000u);") !=
               std::string::npos &&
           code.find("gte_execute(cpu") == std::string::npos;
}

} // namespace

int main(void) {
    if (!has_exact_call_only(generate(false))) {
        std::fputs("FAIL: static GTE command lacks exact guest PC\n", stderr);
        return 1;
    }
    if (!has_exact_call_only(generate(true))) {
        std::fputs("FAIL: warm GTE command lacks exact guest PC\n", stderr);
        return 1;
    }
    std::puts("PASS: static and warm GTE commands carry exact guest PCs");
    return 0;
}
