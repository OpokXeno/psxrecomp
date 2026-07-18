#include "function_analysis.h"
#include "ps1_exe_parser.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kLoad = 0x80010000u;
int failures = 0;

#define CHECK(cond, message) do {                                              \
    if (!(cond)) {                                                             \
        std::fprintf(stderr, "FAIL: %s\n", (message));                        \
        failures++;                                                            \
    }                                                                          \
} while (0)

void put32(std::vector<uint8_t>& data, size_t offset, uint32_t value) {
    data[offset + 0] = static_cast<uint8_t>(value);
    data[offset + 1] = static_cast<uint8_t>(value >> 8);
    data[offset + 2] = static_cast<uint8_t>(value >> 16);
    data[offset + 3] = static_cast<uint8_t>(value >> 24);
}

uint32_t jal(uint32_t target) {
    return 0x0C000000u | ((target >> 2) & 0x03FFFFFFu);
}

std::vector<uint8_t> make_exe_buffer(uint32_t image_size,
                                     uint32_t entry = kLoad) {
    std::vector<uint8_t> data(2048u + image_size, 0);
    std::memcpy(data.data(), "PS-X EXE", 8);
    put32(data, 0x10, entry);
    put32(data, 0x18, kLoad);
    put32(data, 0x1C, image_size);
    return data;
}

PSXRecomp::PS1Executable parse(std::vector<uint8_t> data) {
    std::string error;
    auto exe = PSXRecomp::PS1ExeParser::parse_buffer(data, error);
    CHECK(exe.has_value(), error.c_str());
    return exe.value();
}

std::set<uint32_t> starts(const PSXRecomp::FunctionAnalysisResult& result) {
    std::set<uint32_t> out;
    for (const auto& function : result.functions) out.insert(function.start_addr);
    return out;
}

} // namespace

int main() {
    auto image = make_exe_buffer(0x2000);
    const size_t text = 2048;

    // Root: direct call to a framed target, direct call to a frameless leaf,
    // unresolved jalr, direct call beyond the eventual bound, then return.
    put32(image, text + 0x00, jal(kLoad + 0x100));
    put32(image, text + 0x04, 0x00000000u);
    put32(image, text + 0x08, jal(kLoad + 0x300));
    put32(image, text + 0x0C, 0x00000000u);
    put32(image, text + 0x10, 0x0320F809u); // jalr $ra,$t9 (unresolved)
    put32(image, text + 0x14, 0x00000000u);
    put32(image, text + 0x18, jal(kLoad + 0x1100));
    put32(image, text + 0x1C, 0x00000000u);
    put32(image, text + 0x20, 0x03E00008u);
    put32(image, text + 0x24, 0x00000000u);

    // Callable direct target.
    put32(image, text + 0x100, 0x27BDFFF0u);
    put32(image, text + 0x104, 0x03E00008u);
    put32(image, text + 0x108, 0x27BD0010u);

    // Valid-looking callback not referenced by a direct call.
    put32(image, text + 0x200, 0x27BDFFF0u);
    put32(image, text + 0x204, 0x03E00008u);
    put32(image, text + 0x208, 0x27BD0010u);

    // A direct JAL is sufficient callable-boundary evidence even when the
    // target is a tiny frameless leaf.
    put32(image, text + 0x300, 0x24020001u); // addiu $v0,$zero,1
    put32(image, text + 0x304, 0x03E00008u);
    put32(image, text + 0x308, 0x00000000u);

    // Callable target outside the verified static bound.
    put32(image, text + 0x1100, 0x27BDFFF0u);
    put32(image, text + 0x1104, 0x03E00008u);
    put32(image, text + 0x1108, 0x27BD0010u);

    auto exe = parse(image);
    std::string error;
    CHECK(PSXRecomp::apply_static_analysis_bound(exe, 0x1000, error),
          "valid page-aligned analysis bound is accepted");
    CHECK(exe.code_size() == 0x1000 && exe.code_data.size() == 0x1000,
          "accepted bound truncates header and analysis payload together");

    PSXRecomp::FunctionAnalyzer analyzer(exe);
    const auto reachable = starts(analyzer.analyze_exact_entries({kLoad}));
    CHECK(reachable == std::set<uint32_t>({kLoad, kLoad + 0x100,
                                           kLoad + 0x300}),
          "reachable analysis follows framed and frameless direct JAL targets");
    CHECK(!reachable.count(kLoad + 0x200),
          "unseen indirect callback fails closed");
    CHECK(!reachable.count(kLoad + 0x1100),
          "direct target beyond verified bound fails closed");

    // A synthetic adjacent-producer envelope is not one linked image. The
    // caller supplies the cross-boundary targets that passed its independent
    // callable-CFG proof; other valid-looking targets stay with the interpreter.
    PSXRecomp::FunctionAnalyzer composite_analyzer(exe);
    const std::vector<std::pair<uint32_t, uint32_t>> producer_ranges = {
        {kLoad, kLoad + 0x80}, {kLoad + 0x80, kLoad + 0x1000}};
    const auto composite = starts(composite_analyzer.analyze_exact_entries(
        {kLoad}, producer_ranges, {kLoad + 0x100}));
    CHECK(composite.count(kLoad) && composite.count(kLoad + 0x100),
          "approved cross-producer call target is discovered");
    CHECK(!composite.count(kLoad + 0x300),
          "unapproved cross-producer JAL target fails closed");

    // A constant-register JR out of the current function is a statically
    // proven tail call.  Resolve the common lui/addiu/jr sequence without an
    // execution-derived seed.
    auto indirect_image = make_exe_buffer(0x1000, kLoad + 0x500);
    put32(indirect_image, text + 0x400, 0x24020001u);
    put32(indirect_image, text + 0x404, 0x03E00008u);
    put32(indirect_image, text + 0x408, 0x00000000u);
    put32(indirect_image, text + 0x500, 0x3C088001u); // lui $t0,0x8001
    put32(indirect_image, text + 0x504, 0x25080400u); // addiu $t0,$t0,0x400
    put32(indirect_image, text + 0x508, 0x01000008u); // jr $t0
    put32(indirect_image, text + 0x50C, 0x00000000u);
    auto indirect_exe = parse(indirect_image);
    PSXRecomp::FunctionAnalyzer indirect_analyzer(indirect_exe);
    const auto indirect = starts(
        indirect_analyzer.analyze_exact_entries({kLoad + 0x500}));
    CHECK(indirect.count(kLoad + 0x400) && indirect.count(kLoad + 0x500),
          "constant-register JR discovers its frameless tail-call target");

    // A packed BIOS thunk is also a backward-scan boundary.  The frameless
    // leaf after its delay slot must not be swallowed into the thunk run.
    auto thunk_image = make_exe_buffer(0x1000);
    put32(thunk_image, text + 0x600, 0x240A00B0u);
    put32(thunk_image, text + 0x604, 0x01400008u);
    put32(thunk_image, text + 0x608, 0x24090008u);
    put32(thunk_image, text + 0x60C, 0x00000000u);
    put32(thunk_image, text + 0x610, 0x24020001u);
    put32(thunk_image, text + 0x614, 0x03E00008u);
    put32(thunk_image, text + 0x618, 0x00000000u);
    auto thunk_exe = parse(thunk_image);
    PSXRecomp::FunctionAnalyzer thunk_analyzer(thunk_exe);
    const auto thunk_starts = starts(thunk_analyzer.analyze());
    CHECK(thunk_starts.count(kLoad + 0x600) &&
          thunk_starts.count(kLoad + 0x610),
          "packed BIOS thunk bounds the following frameless leaf");

    PSXRecomp::FunctionAnalyzer seeded_analyzer(exe);
    const auto seeded = starts(
        seeded_analyzer.analyze_exact_entries({kLoad, kLoad + 0x200}));
    CHECK(seeded.count(kLoad + 0x200),
          "evidence-backed explicit callback seed is compiled");

    auto bad = parse(image);
    const auto original_size = bad.code_data.size();
    CHECK(!PSXRecomp::apply_static_analysis_bound(bad, 0x1800, error),
          "non-page-aligned truncation is rejected");
    CHECK(bad.code_size() == 0x2000 && bad.code_data.size() == original_size,
          "invalid bound leaves parsed image unchanged");
    CHECK(!PSXRecomp::apply_static_analysis_bound(bad, 0x3000, error),
          "oversized noncanonical bound is rejected");
    CHECK(!PSXRecomp::apply_static_analysis_bound(bad, 0, error),
          "zero bound is rejected");

    auto excludes_entry = parse(image);
    excludes_entry.header.initial_pc = kLoad + 0x1000;
    CHECK(!PSXRecomp::apply_static_analysis_bound(excludes_entry, 0x1000, error),
          "bound excluding entry point is rejected");

    auto rounded = parse(make_exe_buffer(0x1800));
    CHECK(PSXRecomp::apply_static_analysis_bound(rounded, 0x2000, error),
          "canonical generated final-page reservation remains compatible");
    CHECK(rounded.code_size() == 0x1800,
          "page reservation does not widen static analysis payload");

    if (failures) {
        std::fprintf(stderr, "FAILED (%d)\n", failures);
        return 1;
    }
    std::puts("ALL PASS");
    return 0;
}
