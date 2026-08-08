#ifndef PSXRECOMP_GTE_ATTRIBUTION_H
#define PSXRECOMP_GTE_ATTRIBUTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef GTE_ATTRIBUTION_CONTEXT_CAPACITY
#define GTE_ATTRIBUTION_CONTEXT_CAPACITY 4096u
#endif

#ifndef GTE_ATTRIBUTION_SITE_CAPACITY
#define GTE_ATTRIBUTION_SITE_CAPACITY 16384u
#endif

#ifndef GTE_ATTRIBUTION_COUNTER_MAX
#define GTE_ATTRIBUTION_COUNTER_MAX UINT64_MAX
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum GteAttributionExecutionTier {
    GTE_ATTRIBUTION_TIER_UNKNOWN = 0,
    GTE_ATTRIBUTION_TIER_STATIC = 1,
    GTE_ATTRIBUTION_TIER_COLD = 2,
    GTE_ATTRIBUTION_TIER_WARM = 3,
    GTE_ATTRIBUTION_TIER_COUNT = 4,
} GteAttributionExecutionTier;

typedef enum GteAttributionResult {
    GTE_ATTRIBUTION_OK = 0,
    GTE_ATTRIBUTION_INVALID_ARGUMENT,
    GTE_ATTRIBUTION_INVALID_TRANSITION,
    GTE_ATTRIBUTION_INSUFFICIENT_CAPACITY,
    GTE_ATTRIBUTION_BLOCKED,
} GteAttributionResult;

extern int g_gte_attribution_enabled;

typedef enum GteAttributionOverflowReason {
    GTE_ATTRIBUTION_OVERFLOW_NONE = 0,
    GTE_ATTRIBUTION_OVERFLOW_CONTEXT_CAPACITY,
    GTE_ATTRIBUTION_OVERFLOW_SITE_CAPACITY,
    GTE_ATTRIBUTION_OVERFLOW_COUNTER,
} GteAttributionOverflowReason;

typedef struct GteAttributionVisualStateId {
    uint64_t scene_epoch;
    uint64_t state_sequence;
} GteAttributionVisualStateId;

typedef struct GteAttributionProducerContext {
    GteAttributionVisualStateId visual_state_id;
    uint32_t producer_id;
    GteAttributionExecutionTier tier;
} GteAttributionProducerContext;

/* caller is the architectural guest return address ($ra), not a synthesized
 * call instruction. command is the complete GTE command word passed to
 * gte_execute. The known flags distinguish zero from missing attribution and
 * are part of the site key. */
typedef struct GteAttributionSite {
    uint32_t guest_pc;
    uint32_t caller;
    bool guest_pc_known;
    bool caller_known;
    uint32_t command;
    bool command_known;
} GteAttributionSite;

typedef struct GteAttributionContextKey {
    GteAttributionVisualStateId visual_state_id;
    uint32_t producer_id;
    GteAttributionExecutionTier tier;
    bool inside_producer;
} GteAttributionContextKey;

typedef struct GteAttributionContextCounter {
    GteAttributionContextKey context;
    uint64_t count;
} GteAttributionContextCounter;

typedef struct GteAttributionSiteCounter {
    GteAttributionContextKey context;
    GteAttributionSite site;
    uint64_t count;
} GteAttributionSiteCounter;

typedef struct GteAttributionSnapshot {
    uint64_t total_count;
    uint64_t inside_producer_count;
    uint64_t outside_producer_count;
    uint64_t tier_counts[GTE_ATTRIBUTION_TIER_COUNT];
    size_t context_count;
    size_t site_count;
    GteAttributionContextKey current_context;
    GteAttributionOverflowReason overflow_reason;
    bool blocked;
} GteAttributionSnapshot;

/* The execution tier remains in effect outside producer brackets, allowing
 * unattributed work in static/cold/warm execution to remain distinguishable. */
GteAttributionResult gte_attribution_set_execution_tier(
    GteAttributionExecutionTier tier);
GteAttributionResult gte_attribution_producer_begin(
    const GteAttributionProducerContext *context);
GteAttributionResult gte_attribution_producer_end(void);

/* Called once for every authoritative gte_execute. Public for runtimes that
 * have a more exact site source than CPUState. */
GteAttributionResult gte_attribution_record_execute(
    const GteAttributionSite *site);
GteAttributionResult gte_attribution_record_execute_in_tier(
    const GteAttributionSite *site, GteAttributionExecutionTier tier);
void gte_attribution_set_enabled(bool enabled);

/* Clears counters, overflow state, and the current producer/tier. */
void gte_attribution_reset(void);

/* Produces one internally consistent, deterministically ordered publication.
 * The caller supplies storage so the API does not allocate. On insufficient
 * storage, out_snapshot still reports the required context/site counts and no
 * counter entries are written. */
GteAttributionResult gte_attribution_snapshot(
    GteAttributionSnapshot *out_snapshot,
    GteAttributionContextCounter *context_counters,
    size_t context_capacity,
    GteAttributionSiteCounter *site_counters,
    size_t site_capacity);

uint64_t gte_attribution_total_count(void);
size_t gte_attribution_context_capacity(void);
size_t gte_attribution_site_capacity(void);

#ifdef __cplusplus
}
#endif

#endif
