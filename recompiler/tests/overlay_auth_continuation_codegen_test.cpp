#include <cstdint>
#include <cstdlib>
#include <array>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "code_generator.h"
#include "control_flow.h"
#include "fmt/format.h"

namespace {

constexpr uint32_t kBase = 0x80010000u;
constexpr uint32_t kContinuation = kBase + 8u;
constexpr uint32_t kTarget = 0x80020000u;
constexpr uint32_t kJumpTarget = 0x80030000u;
constexpr uint32_t kJal = 0x0C000000u | ((kTarget >> 2u) & 0x03FFFFFFu);
constexpr uint32_t kJalrT9 = 0x0320F809u;
constexpr uint32_t kJump = 0x08000000u | ((kJumpTarget >> 2u) & 0x03FFFFFFu);
constexpr uint32_t kRuntimeVariantObservationStart = 0x800765B0u;
constexpr uint32_t kRuntimeVariantObservationEnd = 0x80076A2Cu;
constexpr uint32_t kRuntimeVariantUnselectedPc = 0x80076864u;
constexpr uint32_t kRuntimeVariantUnselectedInstruction = 0x24840001u;
constexpr uint32_t kRuntimeVariantCutoverPc = 0x800765DCu;
constexpr uint32_t kRuntimeVariantCutoverInstruction = 0xAFA00028u;
constexpr uint32_t kRuntimeVariantCutoverContinuation = 0x80076A28u;

struct SourceObservationSite {
    uint32_t address;
    uint32_t instruction;
};

constexpr std::array<SourceObservationSite, 14> kRuntimeVariantObservationSites = {{
    {0x80076858u, 0x48026000u},
    {0x8007685Cu, 0x48036800u},
    {0x80076860u, 0x48047000u},
    {0x800769C8u, 0x0C000000u},
    {0x800769D4u, 0x8C840000u},
    {0x800769D8u, 0x8C850000u},
    {0x800769E4u, 0x8C860000u},
    {0x800769E8u, 0x8C870000u},
    {0x800769F8u, 0x8C880000u},
    {0x80076A0Cu, 0x8C890000u},
    {0x80076A10u, 0x8C8A0000u},
    {0x800769ECu, 0x00621007u},
    {0x80076A08u, 0xAC840000u},
    {0x80076A24u, 0xAC850000u},
}};

int failures = 0;

void check(bool condition, const char* message) {
    if (condition) {
        fmt::print("PASS  {}\n", message);
    } else {
        fmt::print(stderr, "FAIL  {}\n", message);
        ++failures;
    }
}

size_t count_occurrences(const std::string& text, const std::string& needle) {
    size_t count = 0u;
    size_t offset = 0u;
    while ((offset = text.find(needle, offset)) != std::string::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

void set_cps(bool enabled) {
#ifdef _WIN32
    _putenv_s("PSX_CPS", enabled ? "1" : "0");
#else
    setenv("PSX_CPS", enabled ? "1" : "0", 1);
#endif
}

void write_word(PSXRecomp::PS1Executable& executable,
                uint32_t address,
                uint32_t word) {
    const size_t offset = static_cast<size_t>(address - executable.header.load_address);
    executable.code_data[offset] = static_cast<uint8_t>(word);
    executable.code_data[offset + 1u] = static_cast<uint8_t>(word >> 8u);
    executable.code_data[offset + 2u] = static_cast<uint8_t>(word >> 16u);
    executable.code_data[offset + 3u] = static_cast<uint8_t>(word >> 24u);
}

PSXRecomp::PS1Executable make_executable() {
    PSXRecomp::PS1Executable executable{};
    executable.header.load_address = kBase;
    executable.header.initial_pc = kBase;
    executable.code_data.assign(0x80u, 0u);
    executable.header.file_size = static_cast<uint32_t>(executable.code_data.size());
    return executable;
}

PSXRecomp::PS1Executable make_runtime_variant_observation_executable() {
    PSXRecomp::PS1Executable executable{};
    executable.header.load_address = kRuntimeVariantObservationStart;
    executable.header.initial_pc = kRuntimeVariantObservationStart;
    executable.code_data.assign(kRuntimeVariantObservationEnd - kRuntimeVariantObservationStart + 8u,
                                0u);
    executable.header.file_size = static_cast<uint32_t>(executable.code_data.size());
    const uint32_t branch_offset =
        (kRuntimeVariantCutoverContinuation - (kRuntimeVariantObservationStart + 4u)) / 4u;
    write_word(executable, kRuntimeVariantObservationStart,
               0x10400000u | (branch_offset & 0xFFFFu));
    write_word(executable, kRuntimeVariantCutoverPc, kRuntimeVariantCutoverInstruction);
    write_word(executable, kRuntimeVariantUnselectedPc, kRuntimeVariantUnselectedInstruction);
    for (const SourceObservationSite& site : kRuntimeVariantObservationSites) {
        write_word(executable, site.address, site.instruction);
    }
    write_word(executable, kRuntimeVariantObservationEnd, 0x03E00008u);
    write_word(executable, kRuntimeVariantObservationEnd + 4u, 0u);
    return executable;
}

PSXRecomp::Function make_function(uint32_t start,
                                  uint32_t end,
                                  const char* name) {
    PSXRecomp::Function function{};
    function.start_addr = start;
    function.end_addr = end;
    function.size = end - start;
    function.name = name;
    return function;
}

void write_direct_jal_function(PSXRecomp::PS1Executable& executable) {
    write_word(executable, kBase, kJal);
    write_word(executable, kBase + 4u, 0u);
    write_word(executable, kContinuation, 0x24020001u);
    write_word(executable, kBase + 12u, 0x03E00008u);
    write_word(executable, kBase + 16u, 0u);
}

void write_local_jalr_function(PSXRecomp::PS1Executable& executable,
                               uint32_t start) {
    write_word(executable, start, kJalrT9);
    write_word(executable, start + 4u, 0u);
    write_word(executable, start + 8u, kJump);
    write_word(executable, start + 12u, 0u);
}

std::string return_hook(uint32_t continuation) {
    return fmt::format(
        "psx_xg_render_auth(cpu, PSX_XG_RENDER_AUTH_HOOK_CONTINUATION, "
        "0x{:08X}u, 0u, 0u);",
        continuation);
}

std::string entry_hook(uint32_t entry) {
    return fmt::format(
        "psx_xg_render_auth(cpu, PSX_XG_RENDER_AUTH_HOOK_PRODUCER_ENTRY, "
        "0x{:08X}u, 0u, 0u);",
        entry);
}

std::string source_observation_hook(const char* hook, uint32_t pc,
                                    uint32_t instruction) {
    return fmt::format("psx_xg_render_auth(cpu, {}, 0x{:08X}u, 0x{:08X}u, 0u);",
                       hook, pc, instruction);
}

std::string generate_function(const PSXRecomp::PS1Executable& executable,
                              const PSXRecomp::Function& function,
                              bool cps,
                              const std::set<uint32_t>& known_functions = {},
                              bool exact_lifecycle = false) {
    set_cps(cps);
    PSXRecomp::CodeGenConfig config{};
    config.overlay_mode = true;
    for (const SourceObservationSite& site : kRuntimeVariantObservationSites) {
        config.source_observation_sites.push_back({
            site.address, site.instruction,
            site.address == 0x800769C8u
                ? PSXRecomp::CodeGenConfig::SourceObservationOperation::Call
                : PSXRecomp::CodeGenConfig::SourceObservationOperation::Bucket,
            0u, PSXRecomp::CodeGenConfig::SourceObservationAuxiliary::None,
        });
    }
    config.native_cutover_sites.push_back({
        kRuntimeVariantCutoverPc, kRuntimeVariantCutoverInstruction,
        PSXRecomp::CodeGenConfig::NativeCutoverTransfer::Local,
        kRuntimeVariantCutoverContinuation,
    });
    if (exact_lifecycle) {
        config.render_lifecycle_sites.push_back({
            kBase, *executable.read_word(kBase),
            PSXRecomp::CodeGenConfig::RenderLifecycleRole::Entry, 0u,
        });
        config.render_lifecycle_sites.push_back({
            kBase + 12u, *executable.read_word(kBase + 12u),
            PSXRecomp::CodeGenConfig::RenderLifecycleRole::Capture,
            *executable.read_word(kBase + 16u),
        });
        config.render_lifecycle_sites.push_back({
            kBase + 20u, *executable.read_word(kBase + 20u),
            PSXRecomp::CodeGenConfig::RenderLifecycleRole::Return, 0u,
        });
    }
    PSXRecomp::ControlFlowAnalyzer analyzer(executable);
    const PSXRecomp::ControlFlowGraph cfg = analyzer.analyze_function(function);
    PSXRecomp::CodeGenerator generator(executable, config);
    generator.set_known_functions(known_functions);
    return generator.generate_function(function, cfg).full_code;
}

void test_manifest_lifecycle_suppresses_colliding_generic_return() {
    PSXRecomp::PS1Executable executable = make_executable();
    write_word(executable, kBase, 0x24010001u);
    write_word(executable, kBase + 4u, kJal);
    write_word(executable, kBase + 8u, 0u);
    write_word(executable, kBase + 12u, kJal);
    write_word(executable, kBase + 16u, 0x34040001u);
    write_word(executable, kBase + 20u, 0x24030001u);
    write_word(executable, kBase + 24u, 0x03E00008u);
    write_word(executable, kBase + 28u, 0u);
    const PSXRecomp::Function function =
        make_function(kBase, kBase + 32u, "exact_lifecycle");
    const std::string output = generate_function(
        executable, function, true, {kTarget}, true);
    const std::string entry =
        "PSX_XG_RENDER_AUTH_HOOK_ENTRY, 0x80010000u, 0x24010001u, 0x00000000u";
    const std::string capture =
        "PSX_XG_RENDER_AUTH_HOOK_CAPTURE, 0x8001000Cu, 0x0C008000u, 0x34040001u";
    const std::string exact_return =
        "PSX_XG_RENDER_AUTH_HOOK_RETURN, 0x80010014u, 0x24030001u, 0x00000000u";
    const std::string spurious =
        "PSX_XG_RENDER_AUTH_HOOK_CONTINUATION, 0x8001000Cu";

    check(count_occurrences(output, entry) == 1u,
          "manifest lifecycle emits one exact ENTRY");
    check(count_occurrences(output, capture) == 1u,
          "manifest lifecycle emits one exact CAPTURE");
    check(count_occurrences(output, exact_return) == 1u,
          "manifest lifecycle emits one exact RETURN");
    check(output.find(spurious) == std::string::npos,
          "CAPTURE collision suppresses the generic continuation RETURN");
}

bool appears_in_order(const std::string& text,
                      const std::string& first,
                      const std::string& second,
                      const std::string& third) {
    const size_t first_pos = text.find(first);
    const size_t second_pos = first_pos == std::string::npos
        ? std::string::npos : text.find(second, first_pos + first.size());
    const size_t third_pos = second_pos == std::string::npos
        ? std::string::npos : text.find(third, second_pos + second.size());
    return first_pos != std::string::npos && second_pos != std::string::npos &&
           third_pos != std::string::npos && first_pos < second_pos &&
           second_pos < third_pos;
}

void test_non_cps_return_follows_proven_call_contract() {
    PSXRecomp::PS1Executable executable = make_executable();
    write_direct_jal_function(executable);
    const PSXRecomp::Function function =
        make_function(kBase, kBase + 20u, "non_cps_known");
    const std::string output = generate_function(executable, function, false, {kTarget});
    const std::string expected_return = return_hook(kContinuation);
    const std::string expected_entry = entry_hook(kBase);

    check(count_occurrences(output, expected_entry) == 1u,
          "non-CPS direct JAL function emits one generic producer-entry signal");
    check(count_occurrences(output,
                            "PSX_XG_RENDER_AUTH_HOOK_INTERNAL_OBSERVATION") == 1u,
          "non-CPS known direct JAL emits one internal-observation signal");
    check(appears_in_order(output, "PSX_XG_RENDER_AUTH_HOOK_INTERNAL_OBSERVATION",
                            "/* delay slot (always executes) */",
                            "psx_call_contract"),
          "direct-JAL internal observation remains before the architectural delay slot");
    check(count_occurrences(output, expected_return) == 1u,
          "non-CPS known direct JAL emits one exact call_pc+8 continuation signal");
    check(appears_in_order(output, "psx_call_contract", expected_return,
                           "goto block_80010008"),
          "non-CPS known RETURN follows call contract and precedes local continuation");
}

void test_non_cps_external_and_split_returns_follow_guards() {
    PSXRecomp::PS1Executable executable = make_executable();
    write_direct_jal_function(executable);
    const PSXRecomp::Function local =
        make_function(kBase, kBase + 20u, "non_cps_external");
    const std::string external = generate_function(executable, local, false);
    const std::string expected_return = return_hook(kContinuation);

    check(appears_in_order(external, "if (g_psx_call_bail) return;",
                           expected_return, "goto block_80010008"),
          "non-CPS external RETURN follows bail guard and precedes local continuation");

    const PSXRecomp::Function split =
        make_function(kBase, kContinuation, "non_cps_split");
    const std::string split_output =
        generate_function(executable, split, false, {kTarget, kContinuation});
    check(appears_in_order(split_output, "psx_call_contract", expected_return,
                           "func_80010008(cpu); return;"),
          "non-CPS split RETURN follows call contract and precedes split transfer");
}

void test_cps_local_return_is_in_exact_continuation_case() {
    PSXRecomp::PS1Executable executable = make_executable();
    write_direct_jal_function(executable);
    const PSXRecomp::Function function =
        make_function(kBase, kBase + 20u, "cps_local");
    const std::string output = generate_function(executable, function, true, {kTarget});
    const std::string expected_return = return_hook(kContinuation);

    check(count_occurrences(output, expected_return) == 1u,
          "CPS local direct JAL emits one exact call_pc+8 RETURN");
    check(appears_in_order(output, "case 0x80010008u:", expected_return,
                           "goto block_80010008"),
          "CPS local RETURN executes only in its exact _cont case");
    check(output.find("psx_native_bad_entry") != std::string::npos,
          "CPS local continuation switch preserves fail-closed bad-entry routing");
}

void test_control_flow_cutovers_precede_link_and_delay_slot() {
    PSXRecomp::PS1Executable executable = make_executable();
    write_direct_jal_function(executable);
    const PSXRecomp::Function function =
        make_function(kBase, kBase + 20u, "control_flow_cutover");
    PSXRecomp::ControlFlowAnalyzer analyzer(executable);

    set_cps(true);
    PSXRecomp::CodeGenConfig local_config{};
    local_config.overlay_mode = true;
    local_config.native_cutover_sites.push_back({
        kBase, kJal,
        PSXRecomp::CodeGenConfig::NativeCutoverTransfer::Local,
        kContinuation,
    });
    PSXRecomp::CodeGenerator local_generator(executable, local_config);
    local_generator.set_known_functions({kTarget});
    const std::string local_output = local_generator.generate_function(
        function, analyzer.analyze_function(function)).full_code;
    const std::string local_hook = fmt::format(
        "if (psx_xg_render_native_ft4_bypass(cpu, 0x{:08X}u, "
        "0x{:08X}u)) {{ cpu->pc = 0u; goto block_{:08X}; }}",
        kBase, kJal, kContinuation);
    check(appears_in_order(local_output, local_hook,
                           "jal link before delay slot",
                           "/* delay slot (always executes) */"),
          "local control-flow cutover bypasses both link and delay slot when consumed");

    PSXRecomp::CodeGenConfig observe_config{};
    observe_config.overlay_mode = true;
    observe_config.native_cutover_sites.push_back({
        kBase, kJal,
        PSXRecomp::CodeGenConfig::NativeCutoverTransfer::Observe,
        0u,
    });
    PSXRecomp::CodeGenerator observe_generator(executable, observe_config);
    observe_generator.set_known_functions({kTarget});
    const std::string observe_output = observe_generator.generate_function(
        function, analyzer.analyze_function(function)).full_code;
    const std::string observe_hook = fmt::format(
        "(void)psx_xg_render_native_ft4_bypass(cpu, 0x{:08X}u, "
        "0x{:08X}u);", kBase, kJal);
    check(appears_in_order(observe_output, observe_hook,
                           "jal link before delay slot",
                           "/* delay slot (always executes) */"),
          "observe control-flow cutover records pre-delay state and preserves the call");

    PSXRecomp::CodeGenConfig resident_config{};
    resident_config.native_cutover_sites.push_back({
        kBase, kJal,
        PSXRecomp::CodeGenConfig::NativeCutoverTransfer::Observe,
        0u,
    });
    PSXRecomp::CodeGenerator resident_generator(executable, resident_config);
    resident_generator.set_known_functions({kTarget});
    const std::string resident_output = resident_generator.generate_function(
        function, analyzer.analyze_function(function)).full_code;
    const std::string resident_hook = fmt::format(
        "(void)psx_xg_render_auth_native_ft4_bypass(cpu, 0x{:08X}u, "
        "0x{:08X}u);", kBase, kJal);
    check(appears_in_order(resident_output, resident_hook,
                           "jal link before delay slot",
                           "/* delay slot (always executes) */"),
          "resident control-flow cutover uses the direct runtime callback");
}

void test_cps_split_prologue_emits_return_on_arrival() {
    PSXRecomp::PS1Executable executable = make_executable();
    write_direct_jal_function(executable);
    const PSXRecomp::Function caller =
        make_function(kBase, kContinuation, "cps_split_caller");
    const PSXRecomp::Function continuation =
        make_function(kContinuation, kBase + 20u, "cps_split_continuation");
    PSXRecomp::ControlFlowAnalyzer analyzer(executable);
    std::map<uint32_t, PSXRecomp::ControlFlowGraph> cfgs;
    cfgs.emplace(caller.start_addr, analyzer.analyze_function(caller));
    cfgs.emplace(continuation.start_addr, analyzer.analyze_function(continuation));

    set_cps(true);
    PSXRecomp::CodeGenConfig config{};
    config.overlay_mode = true;
    PSXRecomp::CodeGenerator generator(executable, config);
    generator.set_known_functions({kTarget, kContinuation});
    const std::vector<PSXRecomp::GeneratedFunction> generated =
        generator.generate_all_functions({caller, continuation}, cfgs);
    const std::string expected_return = return_hook(kContinuation);
    std::string caller_output;
    std::string continuation_output;
    for (const PSXRecomp::GeneratedFunction& function : generated) {
        if (function.function_name == caller.name) {
            caller_output = function.full_code;
        } else if (function.function_name == continuation.name) {
            continuation_output = function.full_code;
        }
    }

    check(count_occurrences(caller_output,
                            "PSX_XG_RENDER_AUTH_HOOK_INTERNAL_OBSERVATION") == 1u &&
               caller_output.find("PSX_XG_RENDER_AUTH_HOOK_CONTINUATION") == std::string::npos,
          "CPS split caller observes without signaling before continuation arrival");
    check(appears_in_order(continuation_output, "case 0x80010008u:",
                           expected_return, "break;"),
          "CPS split prologue emits RETURN only when its direct-JAL _cont arrives");
    check(continuation_output.find("psx_native_bad_entry") != std::string::npos,
          "CPS split prologue preserves fail-closed bad-entry routing");
}

void test_cps_alias_body_emits_return_in_exact_case() {
    PSXRecomp::PS1Executable executable = make_executable();
    write_direct_jal_function(executable);
    PSXRecomp::Function alias =
        make_function(kBase, kBase + 20u, "cps_alias");
    alias.alias_walk_lo = kBase;
    alias.alias_group_entries = {kBase};
    PSXRecomp::ControlFlowAnalyzer analyzer(executable);
    const PSXRecomp::ControlFlowGraph cfg = analyzer.analyze_function(alias);

    set_cps(true);
    PSXRecomp::CodeGenConfig config{};
    config.overlay_mode = true;
    PSXRecomp::CodeGenerator generator(executable, config);
    generator.set_known_functions({kTarget});
    const std::vector<PSXRecomp::GeneratedFunction> generated =
        generator.generate_alias_group({&alias}, cfg, "");
    const std::string output = generated.empty() ? std::string() : generated[0].full_code;
    const std::string expected_return = return_hook(kContinuation);

    check(appears_in_order(output, "case 0x80010008u:", expected_return,
                           "goto block_80010008"),
          "CPS alias body emits RETURN only in the direct-JAL _cont case");
    check(count_occurrences(output,
                            "PSX_XG_RENDER_AUTH_HOOK_INTERNAL_OBSERVATION") == 1u &&
               count_occurrences(output, expected_return) == 1u,
          "CPS alias direct JAL has exactly paired observation and continuation signals");
    check(output.find("psx_native_bad_entry") != std::string::npos,
          "CPS alias body preserves fail-closed bad-entry routing");
}

void test_jr_ra_and_jalr_emit_no_lifecycle_events() {
    PSXRecomp::PS1Executable return_executable = make_executable();
    write_word(return_executable, kBase, 0x03E00008u);
    write_word(return_executable, kBase + 4u, 0u);
    const PSXRecomp::Function return_function =
        make_function(kBase, kBase + 8u, "generic_return");
    const std::string cps_return_output =
        generate_function(return_executable, return_function, true);
    const std::string non_cps_return_output =
        generate_function(return_executable, return_function, false);

    check(cps_return_output.find("PSX_XG_RENDER_AUTH_HOOK_INTERNAL_OBSERVATION") == std::string::npos &&
               cps_return_output.find("PSX_XG_RENDER_AUTH_HOOK_CONTINUATION") == std::string::npos &&
               cps_return_output.find("PSX_XG_RENDER_AUTH_HOOK_PRODUCER_EXIT") == std::string::npos &&
               non_cps_return_output.find("PSX_XG_RENDER_AUTH_HOOK_INTERNAL_OBSERVATION") == std::string::npos &&
               non_cps_return_output.find("PSX_XG_RENDER_AUTH_HOOK_CONTINUATION") == std::string::npos &&
               non_cps_return_output.find("PSX_XG_RENDER_AUTH_HOOK_PRODUCER_EXIT") == std::string::npos,
          "generic jr-ra emits no observation continuation or producer-exit signal");

    PSXRecomp::PS1Executable jalr_executable = make_executable();
    write_local_jalr_function(jalr_executable, kBase);
    const PSXRecomp::Function jalr_function =
        make_function(kBase, kBase + 16u, "generic_jalr");
    const std::string cps_jalr_output =
        generate_function(jalr_executable, jalr_function, true);
    const std::string non_cps_jalr_output =
        generate_function(jalr_executable, jalr_function, false);

    check(cps_jalr_output.find("PSX_XG_RENDER_AUTH_HOOK_INTERNAL_OBSERVATION") == std::string::npos &&
               cps_jalr_output.find("PSX_XG_RENDER_AUTH_HOOK_CONTINUATION") == std::string::npos &&
               cps_jalr_output.find("PSX_XG_RENDER_AUTH_HOOK_PRODUCER_EXIT") == std::string::npos &&
               non_cps_jalr_output.find("PSX_XG_RENDER_AUTH_HOOK_INTERNAL_OBSERVATION") == std::string::npos &&
               non_cps_jalr_output.find("PSX_XG_RENDER_AUTH_HOOK_CONTINUATION") == std::string::npos &&
               non_cps_jalr_output.find("PSX_XG_RENDER_AUTH_HOOK_PRODUCER_EXIT") == std::string::npos,
          "JALR emits no lifecycle events in either mode");
}

void test_cps_direct_jal_state_does_not_leak_between_functions() {
    constexpr uint32_t jalr_start = kBase + 0x20u;
    PSXRecomp::PS1Executable executable = make_executable();
    write_direct_jal_function(executable);
    write_local_jalr_function(executable, jalr_start);
    const PSXRecomp::Function direct =
        make_function(kBase, kBase + 20u, "state_direct");
    const PSXRecomp::Function jalr =
        make_function(jalr_start, jalr_start + 16u, "state_jalr");
    PSXRecomp::ControlFlowAnalyzer analyzer(executable);

    set_cps(true);
    PSXRecomp::CodeGenConfig config{};
    config.overlay_mode = true;
    PSXRecomp::CodeGenerator generator(executable, config);
    generator.set_known_functions({kTarget});
    (void)generator.generate_function(direct, analyzer.analyze_function(direct));
    const std::string jalr_output =
        generator.generate_function(jalr, analyzer.analyze_function(jalr)).full_code;

    check(jalr_output.find("PSX_XG_RENDER_AUTH_HOOK_CONTINUATION") == std::string::npos,
          "direct-JAL continuation state is cleared before the next function");
}

void test_runtime_variant_source_observation_hooks_are_exact_and_paired() {
    PSXRecomp::PS1Executable executable = make_runtime_variant_observation_executable();
    const PSXRecomp::Function function = make_function(
        kRuntimeVariantObservationStart, kRuntimeVariantObservationEnd + 8u, "runtime_variant_observation");
    const std::string output = generate_function(executable, function, true);
    const std::string non_cps_output = generate_function(executable, function, false);
    const std::string bypass =
        "bool _xg_native_bypass_800769C8 = "
        "psx_xg_render_native_ft4_bypass(cpu, 0x800769C8u, 0x0C000000u);";
    const std::string cps_bypass =
        "if (_xg_native_bypass_800769C8) { cpu->pc = 0x800769D0u; return; }";
    const std::string non_cps_bypass = "if (!_xg_native_bypass_800769C8) {";
    const std::string cutover =
        "if (psx_xg_render_native_ft4_bypass(cpu, 0x800765DCu, "
        "0xAFA00028u)) { cpu->pc = 0u; goto block_80076A28; }";
    const std::string unselected_pre = source_observation_hook(
        "PSX_XG_RENDER_AUTH_HOOK_SOURCE_PRE", kRuntimeVariantUnselectedPc,
        kRuntimeVariantUnselectedInstruction);
    const std::string unselected_commit = source_observation_hook(
        "PSX_XG_RENDER_AUTH_HOOK_SOURCE_COMMIT", kRuntimeVariantUnselectedPc,
        kRuntimeVariantUnselectedInstruction);

    check(count_occurrences(output, "PSX_XG_RENDER_AUTH_HOOK_SOURCE_PRE") ==
              kRuntimeVariantObservationSites.size(),
          "each runtime descriptor site emits one source-observation pre hook");
    check(count_occurrences(output, "PSX_XG_RENDER_AUTH_HOOK_SOURCE_COMMIT") ==
              kRuntimeVariantObservationSites.size(),
          "each runtime descriptor site emits one source-observation commit hook");
    for (const SourceObservationSite& site : kRuntimeVariantObservationSites) {
        const std::string pre = source_observation_hook(
            "PSX_XG_RENDER_AUTH_HOOK_SOURCE_PRE", site.address, site.instruction);
        const std::string commit = source_observation_hook(
            "PSX_XG_RENDER_AUTH_HOOK_SOURCE_COMMIT", site.address, site.instruction);
        check(count_occurrences(output, pre) == 1u &&
                  count_occurrences(output, commit) == 1u &&
                  output.find(pre) < output.find(commit),
              "a runtime descriptor site emits its exact pre/commit metadata pair");
    }
    check(output.find(unselected_pre) == std::string::npos &&
              output.find(unselected_commit) == std::string::npos,
          "an unselected runtime instruction remains free of source-observation hooks");
    check(output.find("cpu->gpr[4] = cpu->gpr[4] + 1;") != std::string::npos,
          "an unselected runtime instruction retains its vanilla translation");
    check(count_occurrences(output, "psx_xg_render_native_ft4_bypass") == 2u &&
              output.find(bypass) != std::string::npos,
          "the runtime descriptor emits exact early and call-boundary bypass queries");
    check(output.find(cutover) != std::string::npos,
          "the authenticated runtime cutover skips to the next actor");
    check(appears_in_order(output, bypass, "/* delay slot (always executes) */",
                            cps_bypass),
          "CPS evaluates the bypass before the delay slot and transfers at the call boundary");
    check(count_occurrences(non_cps_output,
                            "psx_xg_render_native_ft4_bypass") == 2u &&
              appears_in_order(non_cps_output, bypass,
                               "/* delay slot (always executes) */",
                               non_cps_bypass),
          "non-CPS preserves the delay slot and guards only the original callee");
}

void inspect_authenticated_continuation_snippet() {
    PSXRecomp::PS1Executable executable = make_executable();
    write_direct_jal_function(executable);
    const PSXRecomp::Function function =
        make_function(kBase, kBase + 20u, "inspect_direct_jal");
    const std::string output = generate_function(executable, function, true, {kTarget});
    const std::string needles[] = {
        entry_hook(kBase),
        "PSX_XG_RENDER_AUTH_HOOK_INTERNAL_OBSERVATION",
        return_hook(kContinuation),
    };

    for (const std::string& needle : needles) {
        const size_t offset = output.find(needle);
        const size_t start = output.rfind('\n', offset) + 1u;
        const size_t end = output.find('\n', offset);
        fmt::print("{}\n", output.substr(start, end - start));
    }
}

}

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--inspect") {
        inspect_authenticated_continuation_snippet();
        return 0;
    }
    test_non_cps_return_follows_proven_call_contract();
    test_non_cps_external_and_split_returns_follow_guards();
    test_cps_local_return_is_in_exact_continuation_case();
    test_control_flow_cutovers_precede_link_and_delay_slot();
    test_cps_split_prologue_emits_return_on_arrival();
    test_cps_alias_body_emits_return_in_exact_case();
    test_jr_ra_and_jalr_emit_no_lifecycle_events();
    test_cps_direct_jal_state_does_not_leak_between_functions();
    test_runtime_variant_source_observation_hooks_are_exact_and_paired();
    test_manifest_lifecycle_suppresses_colliding_generic_return();
    return failures == 0 ? 0 : 1;
}
