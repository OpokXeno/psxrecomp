#include "cpu_state.h"
#include "gte_attribution.h"

#include <cstdint>
#include <cstdio>

extern "C" int gpu_ws_present_native_43(void) { return 0; }
extern "C" void psx_ws_note_gte_project(int) {}
extern "C" {
uint64_t s_frame_count = 0;
}

namespace {

int failures = 0;

#define CHECK(condition, message)                                               \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::fprintf(stderr, "FAIL: %s\n", message);                       \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

const GteAttributionSiteCounter *find_site(
    const GteAttributionSiteCounter *sites, size_t count, bool inside,
    bool pc_known, uint32_t pc, bool caller_known, uint32_t caller) {
    for (size_t i = 0; i < count; ++i) {
        if (sites[i].context.inside_producer == inside &&
            sites[i].site.guest_pc_known == pc_known &&
            sites[i].site.guest_pc == pc &&
            sites[i].site.caller_known == caller_known &&
            sites[i].site.caller == caller)
            return &sites[i];
    }
    return nullptr;
}

const GteAttributionSiteCounter *find_command(
    const GteAttributionSiteCounter *sites, size_t count, uint32_t pc,
    uint32_t command) {
    for (size_t i = 0; i < count; ++i) {
        if (sites[i].site.guest_pc_known && sites[i].site.guest_pc == pc &&
            sites[i].site.command_known && sites[i].site.command == command)
            return &sites[i];
    }
    return nullptr;
}

void execute_at(CPUState *cpu, uint32_t pc, uint32_t caller) {
    cpu->gpr[31] = caller;
    gte_execute_at(cpu, 0x06u, pc);
}

void execute_at_tier(CPUState *cpu, uint32_t pc, uint32_t caller,
                     GteAttributionExecutionTier tier) {
    cpu->gpr[31] = caller;
    gte_execute_at_tier(cpu, 0x06u, pc, tier);
}

void test_inside_outside_and_unknown(void) {
    CPUState cpu{};
    GteAttributionContextCounter
        contexts[GTE_ATTRIBUTION_CONTEXT_CAPACITY]{};
    GteAttributionSiteCounter sites[GTE_ATTRIBUTION_SITE_CAPACITY]{};
    GteAttributionSnapshot snapshot{};

    gte_attribution_reset();
    CHECK(gte_attribution_set_execution_tier(GTE_ATTRIBUTION_TIER_STATIC) ==
              GTE_ATTRIBUTION_OK,
          "static tier accepted outside producer");
    execute_at(&cpu, 0x80001000u, 0x80002008u);

    const GteAttributionProducerContext producer = {
        {7u, 11u}, 42u, GTE_ATTRIBUTION_TIER_COLD};
    CHECK(gte_attribution_producer_begin(&producer) == GTE_ATTRIBUTION_OK,
          "producer begin succeeds");
    execute_at_tier(&cpu, 0x80003000u, 0x80004008u,
                    GTE_ATTRIBUTION_TIER_COLD);
    CHECK(gte_attribution_producer_end() == GTE_ATTRIBUTION_OK,
          "producer end succeeds");

    CHECK(gte_attribution_set_execution_tier(GTE_ATTRIBUTION_TIER_STATIC) ==
              GTE_ATTRIBUTION_OK,
          "outside tier can change after producer end");
    cpu.pc = 0x80007000u;
    cpu.gpr[31] = 0;
    gte_execute(&cpu, 0x06u);

    CHECK(gte_attribution_snapshot(
              &snapshot, contexts, GTE_ATTRIBUTION_CONTEXT_CAPACITY,
              sites, GTE_ATTRIBUTION_SITE_CAPACITY) == GTE_ATTRIBUTION_OK,
          "snapshot succeeds");
    CHECK(snapshot.total_count == 3u, "all gte_execute calls counted");
    CHECK(snapshot.inside_producer_count == 1u,
          "inside producer count is distinct");
    CHECK(snapshot.outside_producer_count == 2u,
          "outside producer count is distinct");
    CHECK(snapshot.tier_counts[GTE_ATTRIBUTION_TIER_UNKNOWN] == 1u,
          "direct execution remains explicitly unclassified");
    CHECK(snapshot.tier_counts[GTE_ATTRIBUTION_TIER_STATIC] == 1u,
          "static execution counted");
    CHECK(snapshot.tier_counts[GTE_ATTRIBUTION_TIER_COLD] == 1u,
          "cold producer execution counted");
    CHECK(snapshot.context_count == 3u,
          "inside, outside, and direct contexts retained");
    CHECK(snapshot.site_count == 3u, "three context/site keys retained");
    CHECK(!snapshot.blocked, "normal snapshot is complete");

    const GteAttributionSiteCounter *inside = find_site(
        sites, snapshot.site_count, true, true, 0x80003000u, true,
        0x80004008u);
    CHECK(inside != nullptr, "inside producer site found");
    if (inside) {
        CHECK(inside->context.visual_state_id.scene_epoch == 7u &&
                  inside->context.visual_state_id.state_sequence == 11u,
              "visual state retained");
        CHECK(inside->context.producer_id == 42u,
              "producer id retained");
        CHECK(inside->context.tier == GTE_ATTRIBUTION_TIER_COLD,
              "producer execution tier retained");
    }
    CHECK(find_site(sites, snapshot.site_count, false, true, 0x80001000u,
                    true, 0x80002008u) != nullptr,
          "outside producer site found");
    CHECK(find_site(sites, snapshot.site_count, false, false, 0u, false,
                     0u) != nullptr,
           "direct execution ignores CPUState PC and marks the site unknown");
    CHECK(find_command(sites, snapshot.site_count, 0x80001000u, 0x06u) !=
              nullptr,
          "complete GTE command word is retained in the site key");
}

void test_commands_are_distinct_sites(void) {
    CPUState cpu{};
    GteAttributionContextCounter
        contexts[GTE_ATTRIBUTION_CONTEXT_CAPACITY]{};
    GteAttributionSiteCounter sites[GTE_ATTRIBUTION_SITE_CAPACITY]{};
    GteAttributionSnapshot snapshot{};

    gte_attribution_reset();
    cpu.gpr[31] = 0x80002008u;
    gte_execute_at(&cpu, 0x06u, 0x80001000u);
    gte_execute_at(&cpu, 0x30u, 0x80001000u);
    CHECK(gte_attribution_snapshot(
              &snapshot, contexts, GTE_ATTRIBUTION_CONTEXT_CAPACITY,
              sites, GTE_ATTRIBUTION_SITE_CAPACITY) == GTE_ATTRIBUTION_OK,
          "command-key snapshot succeeds");
    CHECK(snapshot.site_count == 2u,
          "different commands at one PC remain distinct sites");
    CHECK(find_command(sites, snapshot.site_count, 0x80001000u, 0x06u) &&
              find_command(sites, snapshot.site_count, 0x80001000u, 0x30u),
          "both command keys are published");
}

void test_context_overflow_is_fail_closed(void) {
    CPUState cpu{};
    GteAttributionContextCounter
        contexts[GTE_ATTRIBUTION_CONTEXT_CAPACITY]{};
    GteAttributionSiteCounter sites[GTE_ATTRIBUTION_SITE_CAPACITY]{};
    GteAttributionSnapshot snapshot{};
    const GteAttributionExecutionTier tiers[] = {
        GTE_ATTRIBUTION_TIER_UNKNOWN,
        GTE_ATTRIBUTION_TIER_STATIC,
        GTE_ATTRIBUTION_TIER_COLD,
        GTE_ATTRIBUTION_TIER_WARM,
    };

    gte_attribution_reset();
    for (GteAttributionExecutionTier tier : tiers) {
        CHECK(gte_attribution_set_execution_tier(tier) ==
                  GTE_ATTRIBUTION_OK,
              "execution tier accepted before context overflow");
        execute_at_tier(&cpu, 0x80008000u, 0x80009008u, tier);
    }

    CHECK(gte_attribution_snapshot(
              &snapshot, contexts, GTE_ATTRIBUTION_CONTEXT_CAPACITY,
              sites, GTE_ATTRIBUTION_SITE_CAPACITY) == GTE_ATTRIBUTION_OK,
          "context overflow snapshot remains readable");
    CHECK(snapshot.blocked, "context overflow latches blocked state");
    CHECK(snapshot.overflow_reason ==
              GTE_ATTRIBUTION_OVERFLOW_CONTEXT_CAPACITY,
          "context capacity overflow reason surfaced");
    CHECK(snapshot.total_count == GTE_ATTRIBUTION_CONTEXT_CAPACITY,
          "context overflow event is not partially counted");
}

void test_explicit_execution_tiers(void) {
    CPUState cpu{};
    GteAttributionContextCounter
        contexts[GTE_ATTRIBUTION_CONTEXT_CAPACITY]{};
    GteAttributionSiteCounter sites[GTE_ATTRIBUTION_SITE_CAPACITY]{};
    GteAttributionSnapshot snapshot{};

    gte_attribution_reset();
    execute_at(&cpu, 0x80070000u, 0x80071008u);
    execute_at_tier(&cpu, 0x80070004u, 0x80071008u,
                    GTE_ATTRIBUTION_TIER_COLD);
    execute_at_tier(&cpu, 0x80070008u, 0x80071008u,
                    GTE_ATTRIBUTION_TIER_WARM);
    CHECK(gte_attribution_snapshot(
              &snapshot, contexts, GTE_ATTRIBUTION_CONTEXT_CAPACITY,
              sites, GTE_ATTRIBUTION_SITE_CAPACITY) == GTE_ATTRIBUTION_OK,
          "tier snapshot succeeds");
    CHECK(snapshot.tier_counts[GTE_ATTRIBUTION_TIER_UNKNOWN] == 0u,
          "production PC-aware paths avoid unknown attribution");
    CHECK(snapshot.tier_counts[GTE_ATTRIBUTION_TIER_STATIC] == 1u &&
              snapshot.tier_counts[GTE_ATTRIBUTION_TIER_COLD] == 1u &&
              snapshot.tier_counts[GTE_ATTRIBUTION_TIER_WARM] == 1u,
          "static, cold, and warm calls retain their execution tiers");
}

void test_capacity_overflow_is_fail_closed(void) {
    CPUState cpu{};
    GteAttributionContextCounter
        contexts[GTE_ATTRIBUTION_CONTEXT_CAPACITY]{};
    GteAttributionSiteCounter sites[GTE_ATTRIBUTION_SITE_CAPACITY]{};
    GteAttributionSnapshot snapshot{};

    gte_attribution_reset();
    for (uint32_t i = 0; i < GTE_ATTRIBUTION_SITE_CAPACITY + 1u; ++i)
        execute_at(&cpu, 0x80010000u + i * 4u, 0x80020008u);

    CHECK(gte_attribution_snapshot(
              &snapshot, contexts, GTE_ATTRIBUTION_CONTEXT_CAPACITY,
              sites, GTE_ATTRIBUTION_SITE_CAPACITY) == GTE_ATTRIBUTION_OK,
          "overflow snapshot remains readable");
    CHECK(snapshot.blocked, "capacity overflow latches blocked state");
    CHECK(snapshot.overflow_reason == GTE_ATTRIBUTION_OVERFLOW_SITE_CAPACITY,
          "site capacity overflow reason surfaced");
    CHECK(snapshot.total_count == GTE_ATTRIBUTION_SITE_CAPACITY,
          "overflowing event is not partially counted");

    execute_at(&cpu, 0x80030000u, 0x80040008u);
    CHECK(gte_attribution_total_count() == GTE_ATTRIBUTION_SITE_CAPACITY,
          "blocked attribution rejects subsequent execution");
}

void test_counter_overflow_and_reset(void) {
    CPUState cpu{};
    GteAttributionContextCounter
        contexts[GTE_ATTRIBUTION_CONTEXT_CAPACITY]{};
    GteAttributionSiteCounter sites[GTE_ATTRIBUTION_SITE_CAPACITY]{};
    GteAttributionSnapshot snapshot{};

    gte_attribution_reset();
    for (uint64_t i = 0; i < GTE_ATTRIBUTION_COUNTER_MAX + 1u; ++i)
        execute_at(&cpu, 0x80050000u, 0x80060008u);

    CHECK(gte_attribution_snapshot(
              &snapshot, contexts, GTE_ATTRIBUTION_CONTEXT_CAPACITY,
              sites, GTE_ATTRIBUTION_SITE_CAPACITY) == GTE_ATTRIBUTION_OK,
          "counter overflow snapshot remains readable");
    CHECK(snapshot.blocked, "counter overflow latches blocked state");
    CHECK(snapshot.overflow_reason == GTE_ATTRIBUTION_OVERFLOW_COUNTER,
          "counter overflow reason surfaced");
    CHECK(snapshot.total_count == GTE_ATTRIBUTION_COUNTER_MAX,
          "counter never wraps");

    gte_attribution_reset();
    CHECK(gte_attribution_snapshot(
              &snapshot, contexts, GTE_ATTRIBUTION_CONTEXT_CAPACITY,
              sites, GTE_ATTRIBUTION_SITE_CAPACITY) == GTE_ATTRIBUTION_OK,
          "snapshot succeeds after reset");
    CHECK(snapshot.total_count == 0u && snapshot.context_count == 0u &&
              snapshot.site_count == 0u && !snapshot.blocked,
          "reset starts a fresh publication window");
}

} // namespace

int main(void) {
    test_inside_outside_and_unknown();
    test_commands_are_distinct_sites();
    test_explicit_execution_tiers();
    test_context_overflow_is_fail_closed();
    test_capacity_overflow_is_fail_closed();
    test_counter_overflow_and_reset();
    if (failures != 0) return 1;
    std::puts("PASS: bounded production GTE attribution");
    return 0;
}
