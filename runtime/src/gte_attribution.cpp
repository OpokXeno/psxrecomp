#include "gte_attribution.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>
#include <tuple>

extern "C" {
int g_gte_attribution_enabled = 0;
}

namespace {

static_assert(GTE_ATTRIBUTION_CONTEXT_CAPACITY > 0,
              "GTE attribution needs at least one context slot");
static_assert(GTE_ATTRIBUTION_SITE_CAPACITY > 0,
              "GTE attribution needs at least one site slot");
static_assert(GTE_ATTRIBUTION_COUNTER_MAX > 0,
              "GTE attribution counters need a nonzero limit");

struct ContextSlot {
    bool occupied;
    GteAttributionContextCounter counter;
};

struct SiteSlot {
    bool occupied;
    GteAttributionSiteCounter counter;
};

std::mutex s_mutex;
std::array<ContextSlot, GTE_ATTRIBUTION_CONTEXT_CAPACITY> s_context_slots{};
std::array<SiteSlot, GTE_ATTRIBUTION_SITE_CAPACITY> s_site_slots{};
GteAttributionSnapshot s_summary{};
GteAttributionExecutionTier s_current_tier = GTE_ATTRIBUTION_TIER_UNKNOWN;
GteAttributionProducerContext s_current_producer{};
bool s_producer_active = false;

uint64_t hash_mix(uint64_t hash, uint64_t value) {
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31;
    return hash ^ (value + UINT64_C(0x9e3779b97f4a7c15) + (hash << 6) +
                   (hash >> 2));
}

uint64_t hash_context(const GteAttributionContextKey &context) {
    uint64_t hash = UINT64_C(0x6a09e667f3bcc909);
    hash = hash_mix(hash, context.visual_state_id.scene_epoch);
    hash = hash_mix(hash, context.visual_state_id.state_sequence);
    hash = hash_mix(hash, context.producer_id);
    hash = hash_mix(hash, static_cast<uint64_t>(context.tier));
    return hash_mix(hash, context.inside_producer ? 1u : 0u);
}

uint64_t hash_site(const GteAttributionContextKey &context,
                   const GteAttributionSite &site) {
    uint64_t hash = hash_context(context);
    hash = hash_mix(hash, site.guest_pc);
    hash = hash_mix(hash, site.caller);
    hash = hash_mix(hash, site.guest_pc_known ? 1u : 0u);
    hash = hash_mix(hash, site.caller_known ? 1u : 0u);
    hash = hash_mix(hash, site.command);
    return hash_mix(hash, site.command_known ? 1u : 0u);
}

bool context_equal(const GteAttributionContextKey &left,
                   const GteAttributionContextKey &right) {
    return left.visual_state_id.scene_epoch ==
               right.visual_state_id.scene_epoch &&
           left.visual_state_id.state_sequence ==
               right.visual_state_id.state_sequence &&
           left.producer_id == right.producer_id &&
           left.tier == right.tier &&
           left.inside_producer == right.inside_producer;
}

bool site_equal(const GteAttributionSite &left,
                const GteAttributionSite &right) {
    return left.guest_pc == right.guest_pc &&
           left.caller == right.caller &&
           left.guest_pc_known == right.guest_pc_known &&
           left.caller_known == right.caller_known &&
           left.command == right.command &&
           left.command_known == right.command_known;
}

GteAttributionContextKey current_context(void) {
    GteAttributionContextKey context{};
    context.tier = s_current_tier;
    context.inside_producer = s_producer_active;
    if (s_producer_active) {
        context.visual_state_id = s_current_producer.visual_state_id;
        context.producer_id = s_current_producer.producer_id;
    }
    return context;
}

ContextSlot *find_context_slot(const GteAttributionContextKey &context,
                               bool *found) {
    const size_t start = static_cast<size_t>(
        hash_context(context) % GTE_ATTRIBUTION_CONTEXT_CAPACITY);
    for (size_t probe = 0; probe < GTE_ATTRIBUTION_CONTEXT_CAPACITY; ++probe) {
        ContextSlot &slot = s_context_slots[
            (start + probe) % GTE_ATTRIBUTION_CONTEXT_CAPACITY];
        if (!slot.occupied) {
            *found = false;
            return &slot;
        }
        if (context_equal(slot.counter.context, context)) {
            *found = true;
            return &slot;
        }
    }
    *found = false;
    return nullptr;
}

SiteSlot *find_site_slot(const GteAttributionContextKey &context,
                         const GteAttributionSite &site, bool *found) {
    const size_t start = static_cast<size_t>(
        hash_site(context, site) % GTE_ATTRIBUTION_SITE_CAPACITY);
    for (size_t probe = 0; probe < GTE_ATTRIBUTION_SITE_CAPACITY; ++probe) {
        SiteSlot &slot =
            s_site_slots[(start + probe) % GTE_ATTRIBUTION_SITE_CAPACITY];
        if (!slot.occupied) {
            *found = false;
            return &slot;
        }
        if (context_equal(slot.counter.context, context) &&
            site_equal(slot.counter.site, site)) {
            *found = true;
            return &slot;
        }
    }
    *found = false;
    return nullptr;
}

void block_on_overflow(GteAttributionOverflowReason reason) {
    s_summary.blocked = true;
    s_summary.overflow_reason = reason;
}

bool counter_can_increment(uint64_t value) {
    return value < static_cast<uint64_t>(GTE_ATTRIBUTION_COUNTER_MAX);
}

bool context_counter_less(const GteAttributionContextCounter &left,
                          const GteAttributionContextCounter &right) {
    return std::tie(left.context.inside_producer, left.context.tier,
                    left.context.visual_state_id.scene_epoch,
                    left.context.visual_state_id.state_sequence,
                    left.context.producer_id) <
           std::tie(right.context.inside_producer, right.context.tier,
                    right.context.visual_state_id.scene_epoch,
                    right.context.visual_state_id.state_sequence,
                    right.context.producer_id);
}

bool site_counter_less(const GteAttributionSiteCounter &left,
                       const GteAttributionSiteCounter &right) {
    return std::tie(left.context.inside_producer, left.context.tier,
                    left.context.visual_state_id.scene_epoch,
                    left.context.visual_state_id.state_sequence,
                     left.context.producer_id, left.site.guest_pc_known,
                     left.site.guest_pc, left.site.caller_known,
                     left.site.caller, left.site.command_known,
                     left.site.command) <
           std::tie(right.context.inside_producer, right.context.tier,
                    right.context.visual_state_id.scene_epoch,
                    right.context.visual_state_id.state_sequence,
                     right.context.producer_id, right.site.guest_pc_known,
                     right.site.guest_pc, right.site.caller_known,
                     right.site.caller, right.site.command_known,
                     right.site.command);
}

GteAttributionResult record_execute(
    const GteAttributionSite *site, bool tier_is_explicit,
    GteAttributionExecutionTier tier) {
    if (!site ||
        (tier_is_explicit &&
         (tier < GTE_ATTRIBUTION_TIER_UNKNOWN ||
          tier >= GTE_ATTRIBUTION_TIER_COUNT)))
        return GTE_ATTRIBUTION_INVALID_ARGUMENT;
    if (!g_gte_attribution_enabled) return GTE_ATTRIBUTION_OK;

    GteAttributionSite normalized_site = *site;
    if (!normalized_site.guest_pc_known) normalized_site.guest_pc = 0;
    if (!normalized_site.caller_known) normalized_site.caller = 0;
    if (!normalized_site.command_known) normalized_site.command = 0;

    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_summary.blocked) return GTE_ATTRIBUTION_BLOCKED;
    if (tier_is_explicit) s_current_tier = tier;

    const GteAttributionContextKey context = current_context();
    bool context_found = false;
    bool site_found = false;
    ContextSlot *context_slot = find_context_slot(context, &context_found);
    SiteSlot *site_slot = find_site_slot(context, normalized_site, &site_found);
    if (!context_slot) {
        block_on_overflow(GTE_ATTRIBUTION_OVERFLOW_CONTEXT_CAPACITY);
        return GTE_ATTRIBUTION_BLOCKED;
    }
    if (!site_slot) {
        block_on_overflow(GTE_ATTRIBUTION_OVERFLOW_SITE_CAPACITY);
        return GTE_ATTRIBUTION_BLOCKED;
    }

    const uint64_t direction_count = context.inside_producer
        ? s_summary.inside_producer_count
        : s_summary.outside_producer_count;
    if (!counter_can_increment(s_summary.total_count) ||
        !counter_can_increment(direction_count) ||
        !counter_can_increment(s_summary.tier_counts[context.tier]) ||
        (context_found && !counter_can_increment(context_slot->counter.count)) ||
        (site_found && !counter_can_increment(site_slot->counter.count))) {
        block_on_overflow(GTE_ATTRIBUTION_OVERFLOW_COUNTER);
        return GTE_ATTRIBUTION_BLOCKED;
    }

    if (!context_found) {
        context_slot->occupied = true;
        context_slot->counter.context = context;
        context_slot->counter.count = 0;
        ++s_summary.context_count;
    }
    if (!site_found) {
        site_slot->occupied = true;
        site_slot->counter.context = context;
        site_slot->counter.site = normalized_site;
        site_slot->counter.count = 0;
        ++s_summary.site_count;
    }

    ++s_summary.total_count;
    if (context.inside_producer)
        ++s_summary.inside_producer_count;
    else
        ++s_summary.outside_producer_count;
    ++s_summary.tier_counts[context.tier];
    ++context_slot->counter.count;
    ++site_slot->counter.count;
    return GTE_ATTRIBUTION_OK;
}

} // namespace

extern "C" GteAttributionResult gte_attribution_set_execution_tier(
    GteAttributionExecutionTier tier) {
    if (tier < GTE_ATTRIBUTION_TIER_UNKNOWN ||
        tier >= GTE_ATTRIBUTION_TIER_COUNT)
        return GTE_ATTRIBUTION_INVALID_ARGUMENT;

    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_summary.blocked) return GTE_ATTRIBUTION_BLOCKED;
    if (s_producer_active) return GTE_ATTRIBUTION_INVALID_TRANSITION;
    s_current_tier = tier;
    return GTE_ATTRIBUTION_OK;
}

extern "C" GteAttributionResult gte_attribution_producer_begin(
    const GteAttributionProducerContext *context) {
    if (!context || context->tier <= GTE_ATTRIBUTION_TIER_UNKNOWN ||
        context->tier >= GTE_ATTRIBUTION_TIER_COUNT)
        return GTE_ATTRIBUTION_INVALID_ARGUMENT;

    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_summary.blocked) return GTE_ATTRIBUTION_BLOCKED;
    if (s_producer_active) return GTE_ATTRIBUTION_INVALID_TRANSITION;
    s_current_producer = *context;
    s_current_tier = context->tier;
    s_producer_active = true;
    return GTE_ATTRIBUTION_OK;
}

extern "C" GteAttributionResult gte_attribution_producer_end(void) {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_producer_active) return GTE_ATTRIBUTION_INVALID_TRANSITION;
    const bool blocked = s_summary.blocked;
    s_current_producer = {};
    s_producer_active = false;
    return blocked ? GTE_ATTRIBUTION_BLOCKED : GTE_ATTRIBUTION_OK;
}

extern "C" GteAttributionResult gte_attribution_record_execute(
    const GteAttributionSite *site) {
    return record_execute(site, false, GTE_ATTRIBUTION_TIER_UNKNOWN);
}

extern "C" GteAttributionResult gte_attribution_record_execute_in_tier(
    const GteAttributionSite *site, GteAttributionExecutionTier tier) {
    return record_execute(site, true, tier);
}

extern "C" void gte_attribution_set_enabled(bool enabled) {
    std::lock_guard<std::mutex> lock(s_mutex);
    g_gte_attribution_enabled = enabled ? 1 : 0;
}

extern "C" void gte_attribution_reset(void) {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_context_slots = {};
    s_site_slots = {};
    s_summary = {};
    s_current_tier = GTE_ATTRIBUTION_TIER_UNKNOWN;
    s_current_producer = {};
    s_producer_active = false;
    g_gte_attribution_enabled = 1;
}

extern "C" GteAttributionResult gte_attribution_snapshot(
    GteAttributionSnapshot *out_snapshot,
    GteAttributionContextCounter *context_counters,
    size_t context_capacity,
    GteAttributionSiteCounter *site_counters,
    size_t site_capacity) {
    if (!out_snapshot) return GTE_ATTRIBUTION_INVALID_ARGUMENT;

    size_t copied_contexts = 0;
    size_t copied_sites = 0;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        *out_snapshot = s_summary;
        out_snapshot->current_context = current_context();
        if (context_capacity < s_summary.context_count ||
            site_capacity < s_summary.site_count ||
            (s_summary.context_count != 0 && !context_counters) ||
            (s_summary.site_count != 0 && !site_counters))
            return GTE_ATTRIBUTION_INSUFFICIENT_CAPACITY;

        for (const ContextSlot &slot : s_context_slots) {
            if (slot.occupied)
                context_counters[copied_contexts++] = slot.counter;
        }
        for (const SiteSlot &slot : s_site_slots) {
            if (slot.occupied) site_counters[copied_sites++] = slot.counter;
        }
    }

    if (copied_contexts > 1)
        std::sort(context_counters, context_counters + copied_contexts,
                  context_counter_less);
    if (copied_sites > 1)
        std::sort(site_counters, site_counters + copied_sites,
                  site_counter_less);
    return GTE_ATTRIBUTION_OK;
}

extern "C" uint64_t gte_attribution_total_count(void) {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_summary.total_count;
}

extern "C" size_t gte_attribution_context_capacity(void) {
    return GTE_ATTRIBUTION_CONTEXT_CAPACITY;
}

extern "C" size_t gte_attribution_site_capacity(void) {
    return GTE_ATTRIBUTION_SITE_CAPACITY;
}
