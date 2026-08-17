#include "input_replay.h"
#include "guest_render_bridge.h"
#include "guest_render_native_stream.h"
#include "gpu_gl_renderer.h"
#include "native_render_mode_control.h"
#include "native_render_baseline.h"
#ifdef PSX_INPUT_REPLAY_XG_BASELINE
#include "game_identity.h"
#include "xg_native_render_baseline.h"
#endif
#ifdef PSX_INPUT_REPLAY_XG_AUTH_PROOF
#include "xg_render_auth.h"
#include "xg_render_auth_runtime.h"
#include "xg_render_manifest_generated.h"
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <sys/stat.h>
#include <vector>

namespace input_replay {
namespace {
bool destination_exists(const char* path) {
#ifdef _WIN32
    struct _stat destination {};
    return _stat(path, &destination) == 0 || errno != ENOENT;
#else
    struct stat destination {};
    return lstat(path, &destination) == 0 || errno != ENOENT;
#endif
}

const char* retired_failure_name(uint32_t reason) {
    switch ((GlRendererRetiredFailureReason)reason) {
    case GL_RETIRED_FAILURE_MISSING_ANCHOR: return "missing_anchor";
    case GL_RETIRED_FAILURE_SCENE_MISMATCH: return "scene_mismatch";
    case GL_RETIRED_FAILURE_POSITION_MODE_MISMATCH:
        return "position_mode_mismatch";
    case GL_RETIRED_FAILURE_MATERIAL_POSITION_MISMATCH:
        return "material_position_mismatch";
    case GL_RETIRED_FAILURE_ANCHOR_OVERFLOW: return "anchor_overflow";
    case GL_RETIRED_FAILURE_HISTORY_MISS: return "history_miss";
    case GL_RETIRED_FAILURE_CAPACITY: return "capacity";
    case GL_RETIRED_FAILURE_PHASE: return "phase";
    case GL_RETIRED_FAILURE_MIDPOINT_ZERO_AREA: return "midpoint_zero_area";
    case GL_RETIRED_FAILURE_MIDPOINT_EXTENT_COLLAPSE:
        return "midpoint_extent_collapse";
    case GL_RETIRED_FAILURE_MIDPOINT_WINDING_FLIP:
        return "midpoint_winding_flip";
    case GL_RETIRED_FAILURE_FRONT_ORDER_DISPLACEMENT:
        return "front_order_displacement";
    case GL_RETIRED_FAILURE_MIDPOINT_VERTEX_CONFLICT:
        return "midpoint_vertex_conflict";
    case GL_RETIRED_FAILURE_MIDPOINT_FIXED_ZERO_AREA:
        return "midpoint_fixed_zero_area";
    case GL_RETIRED_FAILURE_MIDPOINT_FIXED_WINDING_FLIP:
        return "midpoint_fixed_winding_flip";
    default: return "unknown";
    }
}

struct Pad {
    std::vector<std::string> buttons;
    std::array<int16_t, SDL_CONTROLLER_AXIS_MAX> axes{};
    bool connected = false;
    PadMode mode = PadMode::Digital;
};
struct State { Pad pads[2]; };
struct RecordedRun { State state; uint64_t repeat = 0; };
struct Action {
    State state;
    uint16_t expected_buttons;
    uint64_t min_polls;
    uint64_t max_vblanks;
    uint64_t repeat_cycles;
    uint32_t until_request;
    bool after_lifecycle;
    bool until_change;
};
struct Transition {
    uint64_t vblank;
    Snapshot snapshot;
    LoaderState loader;
    MediaState media;
};
struct SemanticTransition {
    uint64_t vblank;
    Snapshot snapshot;
    LoaderState loader;
};
struct Replay {
    uint64_t budget = 0;
    uint32_t checkpoint_address = 0;
    uint16_t checkpoint_expected = 0;
    std::vector<State> states;
    std::vector<Action> actions;
    std::array<int, 2> devices{{-1, -1}};
    std::array<SDL_GameController*, 2> controllers{{nullptr, nullptr}};
    uint64_t index = 0;
    uint64_t action_index = 0;
    uint64_t action_polls = 0;
    uint64_t action_vblanks = 0;
    uint8_t lifecycle_stage = 0;
    uint64_t lifecycle_neutral_polls = 0;
    uint64_t completed_cycles = 0;
    Snapshot action_baseline{};
    bool action_baseline_set = false;
    Snapshot checkpoint_snapshot{};
    Counters counters{};
    Snapshot snapshot{};
    SioReceipt receipt{};
    LoaderState loader{};
    MediaState media{};
    uint64_t media_samples = 0;
    uint64_t fmv_active_samples = 0;
    uint64_t xa_streaming_samples = 0;
    uint64_t fmv_first_vblank = 0;
    uint64_t fmv_last_vblank = 0;
    uint64_t xa_first_vblank = 0;
    uint64_t xa_last_vblank = 0;
    uint32_t first_mdec_decode_count = 0;
    uint32_t max_mdec_decode_count = 0;
    bool fmv_seen = false;
    bool xa_seen = false;
    std::vector<Transition> transitions;
    std::vector<SemanticTransition> semantic_transitions;
    bool semantic_overflow = false;
    uint64_t last_receipt_polls = 0;
    uint64_t neutral_count = 0, start_count = 0, cross_count = 0, other_count = 0;
    uint64_t neutral_first = 0, neutral_last = 0, start_first = 0, start_last = 0;
    uint64_t cross_first = 0, cross_last = 0, other_first = 0, other_last = 0;
    uint64_t checkpoint_vblank = 0;
    StopReason reason = StopReason::None;
    uint8_t latch_failure = 0;
    bool state_config = false;
    bool checkpoint_configured = false;
    bool record_on_close = false;
    bool checkpoint_seen = false;
    bool baseline_request = false;
    bool baseline_sample_attempted = false;
#ifdef PSX_INPUT_REPLAY_XG_AUTH_PROOF
    PsxXgRenderAuthInstrumentation auth_instrumentation_start{};
    bool auth_instrumentation_started = false;
    bool producer_family_requested = false;
    bool producer_family_armed = false;
#endif
#ifdef PSX_INPUT_REPLAY_XG_BASELINE
    NativeRenderBaselineConfig baseline_config{};
    bool baseline_armed = false;
#endif
    bool loaded = false;
};
Replay replay;
constexpr uint64_t kProducerFamilyMinimumObservations = 1000u;

const char* render_mode_name(GuestRenderRenderMode mode) {
    switch (mode) {
    case GUEST_RENDER_RENDER_SHADOW: return "shadow";
    case GUEST_RENDER_RENDER_NATIVE: return "native";
    case GUEST_RENDER_RENDER_ORIGINAL:
    default: return "original";
    }
}

const char* timing_mode_name(GuestRenderTimingMode mode) {
    return mode == GUEST_RENDER_TIMING_NATIVE_59_94
        ? "native_59_94" : "original";
}

const char* render_fallback_name(GuestRenderFallbackReason reason) {
    switch (reason) {
    case GUEST_RENDER_FALLBACK_NONE: return "none";
    case GUEST_RENDER_FALLBACK_FORCED_ORIGINAL: return "forced_original";
    case GUEST_RENDER_FALLBACK_INVALID_ARGUMENT: return "invalid_argument";
    case GUEST_RENDER_FALLBACK_SCENE_RESET: return "scene_reset";
    case GUEST_RENDER_FALLBACK_NESTED_PRODUCER: return "nested_producer";
    case GUEST_RENDER_FALLBACK_ACTIVE_PRODUCER: return "active_producer";
    case GUEST_RENDER_FALLBACK_WRONG_STATE: return "wrong_state";
    case GUEST_RENDER_FALLBACK_STALE_HANDLE: return "stale_handle";
    case GUEST_RENDER_FALLBACK_INVALID_PROVENANCE: return "invalid_provenance";
    case GUEST_RENDER_FALLBACK_SLOT_CAPACITY: return "slot_capacity";
    case GUEST_RENDER_FALLBACK_COUNTER_EXHAUSTED: return "counter_exhausted";
    case GUEST_RENDER_FALLBACK_WRONG_THREAD: return "wrong_thread";
    case GUEST_RENDER_FALLBACK_INVALID_PACKET_ADDRESS:
        return "invalid_packet_address";
    case GUEST_RENDER_FALLBACK_DUPLICATE_PACKET_ADDRESS:
        return "duplicate_packet_address";
    case GUEST_RENDER_FALLBACK_DUPLICATE_PRIMITIVE_INDEX:
        return "duplicate_primitive_index";
    case GUEST_RENDER_FALLBACK_BINDING_CAPACITY: return "binding_capacity";
    case GUEST_RENDER_FALLBACK_PRESENTATION_GATE: return "presentation_gate";
    case GUEST_RENDER_FALLBACK_BACKEND_FAILURE: return "backend_failure";
    default: return "unknown";
    }
}

const char* presentation_gate_reason_name(
    NativeRenderPresentationGateReason reason) {
    switch (reason) {
    case NATIVE_RENDER_PRESENTATION_GATE_NONE: return "none";
    case NATIVE_RENDER_PRESENTATION_GATE_REQUESTED_ORIGINAL:
        return "requested_original";
    case NATIVE_RENDER_PRESENTATION_GATE_OPENGL_REQUIRED:
        return "opengl_required";
    case NATIVE_RENDER_PRESENTATION_GATE_HISTORY_NOT_EMPTY:
        return "history_not_empty";
    default: return "unknown";
    }
}

struct Recorder {
    std::string path;
    std::vector<RecordedRun> runs;
    uint16_t stop_field = 0;
    uint64_t max_vblanks = 0;
    uint64_t guest_vblanks = 0;
    uint64_t snapshot_vblank = 0;
    uint32_t stable_field_vblanks = 0;
    bool finishing = false;
    bool finish_on_close = false;
    bool active = false;
    bool complete = false;
};
Recorder recorder;

#ifdef PSX_INPUT_REPLAY_XG_AUTH_PROOF
struct AuthProofTuple {
    uint32_t producer_entry = 0;
    uint32_t capture_site = 0;
    uint32_t static_callee = 0;
    uint32_t return_site = 0;
};

struct AuthProofTrace {
    uint64_t entry_sequence = 0;
    uint64_t capture_sequence = 0;
    uint64_t return_sequence = 0;
    uint64_t scene_epoch = 0;
    uint64_t state_sequence = 0;
};

bool exact_auth_tuple(const AuthProofTuple& value) {
    const XgRenderManifestValidation& validation = xg_render_manifest_validation;

    return value.producer_entry == validation.producer_entry &&
           value.capture_site == validation.caller_site &&
           value.static_callee == validation.static_callee &&
           value.return_site == validation.return_site;
}

void write_auth_tuple(std::ostream& output, const AuthProofTuple& value) {
    output << "{\"producer_entry\":" << value.producer_entry
           << ",\"capture_site\":" << value.capture_site
           << ",\"static_callee\":" << value.static_callee
           << ",\"return_site\":" << value.return_site << "}";
}

void write_ft4_payload_mismatch(
    std::ostream& output, const PsxXgRenderFt4PayloadMismatch& mismatch) {
    output << "{\"field_bits\":" << mismatch.field_bits
           << ",\"packet_address\":" << mismatch.packet_address
           << ",\"descriptor_address\":" << mismatch.descriptor_address
           << ",\"expected_material_word\":"
           << mismatch.expected_material_word
           << ",\"actual_material_word\":" << mismatch.actual_material_word
           << ",\"expected_uv\":[";
    for (uint32_t vertex = 0u; vertex < 4u; ++vertex)
        output << (vertex == 0u ? "" : ",") << mismatch.expected_uv[vertex];
    output << "],\"actual_uv\":[";
    for (uint32_t vertex = 0u; vertex < 4u; ++vertex)
        output << (vertex == 0u ? "" : ",") << mismatch.actual_uv[vertex];
    output << "],\"expected_tpage\":" << mismatch.expected_tpage
           << ",\"actual_tpage\":" << mismatch.actual_tpage
           << ",\"expected_clut\":" << mismatch.expected_clut
           << ",\"actual_clut\":" << mismatch.actual_clut << "}";
}

PsxXgRenderAuthInstrumentation auth_instrumentation_delta(
        const PsxXgRenderAuthInstrumentation& current,
        const PsxXgRenderAuthInstrumentation& start) {
    const uint64_t start_sequence = std::max({
        start.last_progress_sequence,
        start.last_reset_sequence,
        start.last_publish_sequence,
    });
    const auto relative_sequence = [start_sequence](uint64_t sequence) {
        return sequence > start_sequence ? sequence - start_sequence : 0u;
    };

    return {
        current.revision,
        current.cold_hook_ingress_count - start.cold_hook_ingress_count,
        current.activation_physical_count - start.activation_physical_count,
        current.activation_exact_count - start.activation_exact_count,
        current.entry_physical_count - start.entry_physical_count,
        current.entry_exact_count - start.entry_exact_count,
        current.capture_physical_count - start.capture_physical_count,
        current.capture_exact_count - start.capture_exact_count,
        current.return_physical_count - start.return_physical_count,
        current.return_exact_count - start.return_exact_count,
        relative_sequence(current.last_progress_sequence),
        relative_sequence(current.last_reset_sequence),
        relative_sequence(current.last_publish_sequence),
        current.scene_boundary_count - start.scene_boundary_count,
        current.disarm_count - start.disarm_count,
        current.completed_proof_publication_count -
            start.completed_proof_publication_count,
        current.native_ir_flush_attempt_count -
            start.native_ir_flush_attempt_count,
        current.native_ir_flush_failure_count -
            start.native_ir_flush_failure_count,
        current.first_native_ir_flush_failure_index,
        current.first_native_ir_flush_failure_reason,
        current.first_native_ir_flush_failure_packet,
        current.first_native_ir_flush_failure_status,
    };
}

bool auth_trace_contains_sequence(const XgRenderAuthTraceSnapshot& trace,
                                  uint64_t sequence) {
    if (sequence == 0u) return false;
    for (size_t index = 0u; index < trace.count; ++index) {
        if (trace.events[index].sequence == sequence) return true;
    }
    return false;
}

void write_auth_proof(std::ostream& output, uint16_t evidence_field_id) {
    XgRenderAuthSnapshot runtime_snapshot{};
    XgRenderAuthTraceSnapshot runtime_trace{};
    PsxXgRenderAuthProvenance provenance{};
    PsxXgRenderAuthCompletedProofReceipt completed_proof{};
    PsxXgRenderAuthInstrumentation instrumentation{};
    XgRenderAuth* auth = nullptr;
    psx_xg_render_auth_completed_proof_snapshot(&completed_proof);
    psx_xg_render_auth_instrumentation_snapshot(&instrumentation);
    if (replay.auth_instrumentation_started)
        instrumentation = auth_instrumentation_delta(
            instrumentation, replay.auth_instrumentation_start);
    psx_xg_render_auth_provenance_snapshot(&provenance);
    const bool runtime_available =
        xg_render_auth_process_owner(&auth) == XG_RENDER_AUTH_OK && auth != nullptr &&
        xg_render_auth_snapshot(auth, &runtime_snapshot) == XG_RENDER_AUTH_OK &&
        xg_render_auth_trace_snapshot(auth, &runtime_trace) == XG_RENDER_AUTH_OK;
    const bool static_accepted = provenance.manifest_bound &&
        provenance.range_bound;
    const AuthProofTuple runtime_tuple = {
        completed_proof.tuple.producer_entry,
        completed_proof.tuple.capture_site,
        completed_proof.tuple.static_callee,
        completed_proof.tuple.return_site,
    };
    const AuthProofTrace runtime_proof_trace = {
        completed_proof.entry_event_sequence,
        completed_proof.capture_event_sequence,
        completed_proof.return_event_sequence,
        completed_proof.state_id.scene_epoch,
        completed_proof.state_id.state_sequence,
    };
    const bool cold_proof =
        completed_proof.tier == XG_RENDER_AUTH_TIER_COLD_INTERPRETER;
    const bool warm_proof =
        completed_proof.tier == XG_RENDER_AUTH_TIER_WARM_NATIVE;
    const bool field_binding_valid = replay.checkpoint_expected == 5u &&
        replay.checkpoint_seen && replay.checkpoint_vblank <=
            replay.counters.vblank_latches && replay.checkpoint_snapshot.valid_field &&
        replay.checkpoint_snapshot.masked_field_id == 5u;
    const bool runtime_accepted = completed_proof.available &&
        instrumentation.completed_proof_publication_count > 0u &&
        !completed_proof.blocked &&
        completed_proof.producer_record_id ==
            xg_render_manifest_validation.producer_record_id &&
        completed_proof.site_record_id ==
            xg_render_manifest_validation.site_record_id &&
        exact_auth_tuple(runtime_tuple) &&
        runtime_proof_trace.scene_epoch != 0u &&
        runtime_proof_trace.entry_sequence != 0u &&
        runtime_proof_trace.capture_sequence != 0u &&
        runtime_proof_trace.return_sequence != 0u &&
        runtime_proof_trace.entry_sequence + 1u ==
            runtime_proof_trace.capture_sequence &&
        runtime_proof_trace.capture_sequence + 1u ==
            runtime_proof_trace.return_sequence &&
        (cold_proof ||
         (warm_proof && completed_proof.candidate_matched &&
          completed_proof.candidate_dispatched)) &&
        field_binding_valid;
    const bool cold_runtime = completed_proof.available && cold_proof &&
        field_binding_valid;
    const bool warm_runtime = completed_proof.available && warm_proof &&
        field_binding_valid;
    const bool retained_blocked = completed_proof.available &&
        completed_proof.blocked;
    const bool candidate_matched = runtime_accepted
        ? completed_proof.candidate_matched : provenance.candidate_matched;
    const bool candidate_dispatched = runtime_accepted
        ? completed_proof.candidate_dispatched : provenance.candidate_dispatched;
    const XgRenderAuthReason reject_reason_value = retained_blocked
        ? completed_proof.blocker_reason : XG_RENDER_AUTH_REJECT_NONE;
    const PsxXgRenderAuthRejectionReceipt rejection = retained_blocked
        ? completed_proof.blocker_rejection
        : PsxXgRenderAuthRejectionReceipt{};
    const bool observed = static_accepted && runtime_accepted && field_binding_valid;
    const char* reject_reason = xg_render_auth_reason_name(reject_reason_value);
    const char* rejection_source = psx_xg_render_auth_rejection_source_name(
        rejection.source);
    const char* rejection_hook = rejection.has_hook
        ? psx_xg_render_auth_hook_name(rejection.hook) : "none";
    size_t rejected_event_count = 0u;
    bool reset_since_trace_start = false;
    if (runtime_available) {
        for (size_t index = 0u; index < runtime_trace.count; ++index) {
            const XgRenderAuthTraceEvent& event = runtime_trace.events[index];

            if (event.event_mode != XG_RENDER_AUTH_EVENT_ACCEPTED_HOOK)
                ++rejected_event_count;
            if (event.state_id.scene_epoch !=
                    runtime_snapshot.logical_identity.state_id.scene_epoch ||
                event.state_id.state_sequence !=
                    runtime_snapshot.logical_identity.state_id.state_sequence)
                reset_since_trace_start = true;
        }
    }
    const size_t trace_event_count = runtime_available ? runtime_trace.count : 0u;
    const bool trace_overflowed = runtime_available && completed_proof.available &&
        (!auth_trace_contains_sequence(runtime_trace,
                                       completed_proof.entry_event_sequence) ||
         !auth_trace_contains_sequence(runtime_trace,
                                       completed_proof.capture_event_sequence) ||
         !auth_trace_contains_sequence(runtime_trace,
                                       completed_proof.return_event_sequence));

    output << ",\"auth_proof\":{\"schema\":\"xenogears.native-render-auth-proof/v4\""
           << ",\"status\":\"" << (observed ? "OBSERVED" : "BLOCKED") << "\""
           << ",\"privacy\":{\"metadata_only\":true,\"raw_instruction_words\":false"
           << ",\"raw_delay_slot_words\":false,\"identities_or_digests\":false"
           << ",\"private_paths\":false,\"disc_cards_cache_hashes\":false"
           << ",\"input_states\":false,\"packets\":false,\"child_runtime_json\":false}"
            << ",\"static\":{\"accepted\":" << (static_accepted ? "true" : "false")
            << ",\"provenance\":{\"source\":\"manifest-overlay\""
            << ",\"image\":\"field-image\""
            << ",\"producer_entry\":"
            << xg_render_manifest_validation.producer_entry
            << ",\"range_start\":"
            << xg_render_manifest_validation.field_range_start
            << ",\"range_size\":"
            << xg_render_manifest_validation.field_range_size
            << ",\"manifest_bound\":"
            << (provenance.manifest_bound ? "true" : "false")
            << ",\"range_bound\":"
            << (provenance.range_bound ? "true" : "false")
            << ",\"candidate\":{\"matched\":"
            << (candidate_matched ? "true" : "false")
            << ",\"dispatched\":"
            << (candidate_dispatched ? "true" : "false")
            << "}}},\"runtime\":{\"accepted\":"
            << (runtime_accepted ? "true" : "false")
            << ",\"tier\":\""
            << (cold_runtime ? "cold" : warm_runtime ? "warm" : "none") << "\""
            << ",\"reject_reason\":\"" << reject_reason << "\""
            << ",\"scene_aborted\":" << (retained_blocked ? "true" : "false")
            << ",\"ir_usable\":" << (runtime_accepted ? "true" : "false")
            << ",\"native_permitted\":" << (runtime_accepted ? "true" : "false")
            << ",\"diagnostic\":{\"available\":"
            << (runtime_available ? "true" : "false")
            << ",\"producer_begin_count\":"
            << (completed_proof.available ? 1u : 0u)
            << ",\"hook_count\":"
            << (completed_proof.available ? XG_RENDER_AUTH_HOOK_STAGE_COUNT : 0u)
            << ",\"trace_event_count\":" << trace_event_count
            << ",\"trace_overflowed\":" << (trace_overflowed ? "true" : "false")
            << ",\"accepted_entry\":" << (completed_proof.available ? "true" : "false")
            << ",\"accepted_capture\":" << (completed_proof.available ? "true" : "false")
            << ",\"accepted_return\":" << (completed_proof.available ? "true" : "false")
            << ",\"rejected_event_count\":" << rejected_event_count
            << ",\"reset_since_trace_start\":"
            << (reset_since_trace_start ? "true" : "false")
            << ",\"scene_aborted\":"
            << (retained_blocked ? "true" : "false")
            << ",\"reject_reason\":\"" << reject_reason << "\""
            << ",\"rejection_source\":\"" << rejection_source << "\""
             << ",\"rejection_hook\":\"" << rejection_hook << "\""
             << ",\"rejection_guest_pc\":" << rejection.guest_pc
             << ",\"instrumentation\":{\"revision\":" << instrumentation.revision
             << ",\"cold_hook_ingress_count\":" << instrumentation.cold_hook_ingress_count
             << ",\"activation_physical_count\":" << instrumentation.activation_physical_count
             << ",\"activation_exact_count\":" << instrumentation.activation_exact_count
             << ",\"entry_physical_count\":" << instrumentation.entry_physical_count
             << ",\"entry_exact_count\":" << instrumentation.entry_exact_count
             << ",\"capture_physical_count\":" << instrumentation.capture_physical_count
             << ",\"capture_exact_count\":" << instrumentation.capture_exact_count
             << ",\"return_physical_count\":" << instrumentation.return_physical_count
             << ",\"return_exact_count\":" << instrumentation.return_exact_count
             << ",\"last_progress_sequence\":" << instrumentation.last_progress_sequence
             << ",\"last_reset_sequence\":" << instrumentation.last_reset_sequence
             << ",\"last_publish_sequence\":" << instrumentation.last_publish_sequence
              << ",\"scene_boundary_count\":" << instrumentation.scene_boundary_count
              << ",\"disarm_count\":" << instrumentation.disarm_count
              << ",\"completed_proof_publication_count\":" << instrumentation.completed_proof_publication_count
              << ",\"native_ir_flush_attempt_count\":" << instrumentation.native_ir_flush_attempt_count
              << ",\"native_ir_flush_failure_count\":" << instrumentation.native_ir_flush_failure_count
              << ",\"first_native_ir_flush_failure_index\":" << instrumentation.first_native_ir_flush_failure_index
              << ",\"first_native_ir_flush_failure_reason\":" << instrumentation.first_native_ir_flush_failure_reason
              << ",\"first_native_ir_flush_failure_packet\":" << instrumentation.first_native_ir_flush_failure_packet
               << ",\"first_native_ir_flush_failure_status\":" << instrumentation.first_native_ir_flush_failure_status
               << "}}";
    if (runtime_accepted) {
        output << ",\"tuple\":";
        write_auth_tuple(output, runtime_tuple);
        output << ",\"trace\":{\"entry_sequence\":"
               << runtime_proof_trace.entry_sequence
               << ",\"capture_sequence\":"
               << runtime_proof_trace.capture_sequence
               << ",\"return_sequence\":"
               << runtime_proof_trace.return_sequence
               << ",\"scene_epoch\":" << runtime_proof_trace.scene_epoch
               << ",\"state_sequence\":" << runtime_proof_trace.state_sequence
               << "}";
    }
    output << "},\"field_binding\":{\"checkpoint_field_id\":"
           << replay.checkpoint_expected
           << ",\"checkpoint_seen\":"
           << (replay.checkpoint_seen ? "true" : "false")
           << ",\"checkpoint_seen_vblank\":" << replay.checkpoint_vblank
           << ",\"evidence_vblank\":" << replay.counters.vblank_latches
           << ",\"context_valid\":"
           << (replay.checkpoint_snapshot.valid_field ? "true" : "false")
           << ",\"context_field_id\":" << replay.checkpoint_snapshot.masked_field_id
           << "}}";
}
#endif

constexpr uint32_t kStableFieldVblanks = 4;
constexpr uint64_t kMaxReplayVblanks = 1000000u;
constexpr uint64_t kTask5ObservationVblanks = 600u;
constexpr size_t kMaxSemanticTransitions = 64u;

#ifdef PSX_INPUT_REPLAY_XG_BASELINE
extern "C" uint8_t *memory_get_ram_ptr(void);
#endif

std::string trim(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c); });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) { return std::isspace(c); }).base();
    return first < last ? std::string(first, last) : std::string();
}
bool integer(const std::string& raw, uint64_t* out) {
    if (raw.empty() || raw.front() == '-') return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(raw.c_str(), &end, 0);
    if (errno == ERANGE || !end || *trim(end).c_str()) return false;
    *out = value;
    return true;
}
bool signed_integer(const std::string& raw, int64_t* out) {
    char* end = nullptr;
    const long long value = std::strtoll(raw.c_str(), &end, 0);
    if (!end || *trim(end).c_str()) return false;
    *out = value;
    return true;
}
bool buttons(const std::string& raw, std::vector<std::string>* out) {
    const std::string value = trim(raw);
    if (value.size() < 2 || value.front() != '[' || value.back() != ']') return false;
    std::stringstream values(value.substr(1, value.size() - 2));
    std::string item;
    while (std::getline(values, item, ',')) {
        item = trim(item);
        if (item.size() < 2 || item.front() != '"' || item.back() != '"') return false;
        const std::string button = item.substr(1, item.size() - 2);
        if (SDL_GameControllerGetButtonFromString(button.c_str()) == SDL_CONTROLLER_BUTTON_INVALID) return false;
        out->push_back(button);
    }
    return true;
}
bool assign(State* state, const std::string& key, const std::string& value) {
    if (key.size() < 4 || key[0] != 'p' || (key[1] != '1' && key[1] != '2') || key[2] != '_') return false;
    Pad& pad = state->pads[key[1] - '1'];
    const std::string field = key.substr(3);
    if (field == "buttons") return buttons(value, &pad.buttons);
    if (field == "connected") {
        if (value == "true") { pad.connected = true; return true; }
        if (value == "false") { pad.connected = false; return true; }
        return false;
    }
    if (field == "mode") {
        if (value == "\"hybrid\"") { pad.mode = PadMode::Hybrid; return true; }
        if (value == "\"analog\"") { pad.mode = PadMode::Analog; return true; }
        if (value == "\"digital\"") { pad.mode = PadMode::Digital; return true; }
        return false;
    }
    const std::array<std::string, SDL_CONTROLLER_AXIS_MAX> names{{"left_x", "left_y", "right_x", "right_y", "trigger_left", "trigger_right"}};
    for (size_t index = 0; index < names.size(); ++index) {
        if (field != names[index]) continue;
        int64_t parsed = 0;
        if (!signed_integer(value, &parsed) || parsed < -32768 || parsed > 32767) return false;
        pad.axes[index] = static_cast<int16_t>(parsed);
        return true;
    }
    return false;
}
uint16_t v2_state_field_bit(const std::string& key) {
    if (key.size() < 4 || key[0] != 'p' || (key[1] != '1' && key[1] != '2') || key[2] != '_') return 0;
    const std::string field = key.substr(3);
    if (field == "connected") return 1u;
    if (field == "mode") return 2u;
    if (field == "buttons") return 4u;
    if (field == "left_x") return 8u;
    if (field == "left_y") return 16u;
    if (field == "right_x") return 32u;
    if (field == "right_y") return 64u;
    if (field == "trigger_left") return 128u;
    if (field == "trigger_right") return 256u;
    return 0;
}
bool action_buttons(const State& state, uint16_t* out) {
    const std::vector<std::string>& buttons = state.pads[0].buttons;
    if (buttons.empty()) { *out = 0xFFFFu; return true; }
    if (buttons.size() != 1u) return false;
    if (buttons[0] == "start") { *out = 0xFFF7u; return true; }
    if (buttons[0] == "a") { *out = 0xBFFFu; return true; }
    if (buttons[0] == "dpdown") { *out = 0xFFBFu; return true; }
    if (buttons[0] == "dpup") { *out = 0xFFEFu; return true; }
    return false;
}
bool update_pad(SDL_GameController* controller, const Pad& pad) {
    SDL_Joystick* joystick = SDL_GameControllerGetJoystick(controller);
    if (!joystick) return false;
    for (int button = 0; button < SDL_CONTROLLER_BUTTON_MAX; ++button)
        SDL_JoystickSetVirtualButton(joystick, button, SDL_RELEASED);
    for (const std::string& name : pad.buttons) {
        const SDL_GameControllerButton button = SDL_GameControllerGetButtonFromString(name.c_str());
        if (SDL_JoystickSetVirtualButton(joystick, button, SDL_PRESSED) != 0) return false;
    }
    const std::array<SDL_GameControllerAxis, SDL_CONTROLLER_AXIS_MAX> axes{{SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY, SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY, SDL_CONTROLLER_AXIS_TRIGGERLEFT, SDL_CONTROLLER_AXIS_TRIGGERRIGHT}};
    for (size_t index = 0; index < axes.size(); ++index) {
        int16_t value = pad.axes[index];
        if (index == SDL_CONTROLLER_AXIS_TRIGGERLEFT ||
            index == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
            const int32_t trigger = value > 0 ? value : 0;
            value = trigger == INT16_MAX
                ? INT16_MAX
                : static_cast<int16_t>(trigger * 2 - 32768);
        }
        if (SDL_JoystickSetVirtualAxis(joystick, axes[index], value) != 0)
            return false;
    }
    return true;
}
bool same_snapshot(const Snapshot& left, const Snapshot& right) {
    return left.field_context == right.field_context &&
           left.requested_module == right.requested_module &&
           left.active_module == right.active_module &&
           left.module_pointer == right.module_pointer &&
           left.raw_field_id == right.raw_field_id &&
           left.masked_field_id == right.masked_field_id &&
           left.game_progress == right.game_progress &&
           left.valid_field == right.valid_field;
}
bool same_loader(const LoaderState& left, const LoaderState& right) {
    return left.cd_has_disc == right.cd_has_disc && left.cd_reading == right.cd_reading &&
           left.cd_sector_available == right.cd_sector_available &&
           left.cd_pending_pending == right.cd_pending_pending &&
           left.cd_pending_cmd == right.cd_pending_cmd && left.cd_queued_cmd == right.cd_queued_cmd &&
           left.overlay_active == right.overlay_active &&
           left.overlay_registered == right.overlay_registered &&
           left.overlay_regions_checked == right.overlay_regions_checked &&
           left.overlay_file_found == right.overlay_file_found;
}
bool same_media(const MediaState& left, const MediaState& right) {
    return left.fmv_active == right.fmv_active && left.xa_streaming == right.xa_streaming;
}
bool same_pad(const Pad& left, const Pad& right) {
    return left.buttons == right.buttons && left.axes == right.axes &&
           left.connected == right.connected && left.mode == right.mode;
}
bool same_state(const State& left, const State& right) {
    return same_pad(left.pads[0], right.pads[0]) && same_pad(left.pads[1], right.pads[1]);
}
bool neutral_state(const State& state) {
    for (const Pad& pad : state.pads) {
        if (!pad.buttons.empty()) return false;
        for (const int16_t axis : pad.axes) if (axis != 0) return false;
    }
    return true;
}
State record_state(const HostInputSnapshot& snapshot) {
    State state{};
    for (size_t slot = 0; slot < 2; ++slot) {
        Pad& pad = state.pads[slot];
        const HostPadSnapshot& host = snapshot.pads[slot];
        pad.connected = host.connected;
        pad.mode = host.mode;
        for (int button = 0; button < SDL_CONTROLLER_BUTTON_MAX; ++button) {
            if (!host.buttons[button]) continue;
            const char* name = SDL_GameControllerGetStringForButton(
                static_cast<SDL_GameControllerButton>(button));
            if (name) pad.buttons.emplace_back(name);
        }
        for (size_t axis = 0; axis < pad.axes.size(); ++axis)
            pad.axes[axis] = host.axes[axis];
    }
    return state;
}
HostInputSnapshot record_projection(const host_input::HostInputSnapshot& host,
                                    const std::array<PsxNetPad, 2>& pads) {
    HostInputSnapshot snapshot{};
    const std::array<std::pair<unsigned, SDL_GameControllerButton>, 14> buttons{{
        {0u, SDL_CONTROLLER_BUTTON_BACK}, {3u, SDL_CONTROLLER_BUTTON_START},
        {4u, SDL_CONTROLLER_BUTTON_DPAD_UP}, {5u, SDL_CONTROLLER_BUTTON_DPAD_RIGHT},
        {6u, SDL_CONTROLLER_BUTTON_DPAD_DOWN}, {7u, SDL_CONTROLLER_BUTTON_DPAD_LEFT},
        {10u, SDL_CONTROLLER_BUTTON_LEFTSHOULDER}, {11u, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},
        {12u, SDL_CONTROLLER_BUTTON_Y}, {13u, SDL_CONTROLLER_BUTTON_B},
        {14u, SDL_CONTROLLER_BUTTON_A}, {15u, SDL_CONTROLLER_BUTTON_X},
        {1u, SDL_CONTROLLER_BUTTON_LEFTSTICK}, {2u, SDL_CONTROLLER_BUTTON_RIGHTSTICK},
    }};
    for (size_t slot = 0; slot < pads.size(); ++slot) {
        const PsxNetPad& source = pads[slot];
        HostPadSnapshot& pad = snapshot.pads[slot];
        pad.connected = source.connected != 0;
        pad.mode = source.analog ? PadMode::Analog : PadMode::Digital;
        for (const auto& button : buttons)
            if ((source.buttons & (1u << button.first)) == 0u)
                pad.buttons[button.second] = SDL_PRESSED;
        if ((source.buttons & (1u << 8u)) == 0u)
            pad.axes[SDL_CONTROLLER_AXIS_TRIGGERLEFT] = 32767;
        if ((source.buttons & (1u << 9u)) == 0u)
            pad.axes[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] = 32767;
        const auto axis = [](uint8_t value) {
            return value == 0x80u ? static_cast<int16_t>(0) :
                static_cast<int16_t>(static_cast<int>(value) * 256 - 32768);
        };
        const auto& controllers = host.controllers();
        if (slot < controllers.size()) {
            for (int axis_index = SDL_CONTROLLER_AXIS_LEFTX;
                 axis_index <= SDL_CONTROLLER_AXIS_RIGHTY; ++axis_index)
                pad.axes[axis_index] = controllers[slot].axes[axis_index];
        } else {
            pad.axes[SDL_CONTROLLER_AXIS_LEFTX] = axis(source.lx);
            pad.axes[SDL_CONTROLLER_AXIS_LEFTY] = axis(source.ly);
            pad.axes[SDL_CONTROLLER_AXIS_RIGHTX] = axis(source.rx);
            pad.axes[SDL_CONTROLLER_AXIS_RIGHTY] = axis(source.ry);
        }
    }
    return snapshot;
}
void append_recorded_state(const State& state) {
    if (!recorder.runs.empty() && same_state(recorder.runs.back().state, state)) {
        recorder.runs.back().repeat++;
        return;
    }
    recorder.runs.push_back({state, 1u});
}
const char* mode_name(PadMode mode) {
    switch (mode) {
    case PadMode::Hybrid: return "hybrid";
    case PadMode::Analog: return "analog";
    case PadMode::Digital: return "digital";
    }
    return "digital";
}
bool write_recording() {
    const std::string temporary = recorder.path + ".partial";
    std::remove(temporary.c_str());
    std::ofstream output(temporary, std::ios::binary);
    if (!output) return false;
    uint64_t budget = 0;
    for (const RecordedRun& run : recorder.runs) budget += run.repeat;
    output << "schema = \"xenogears.native-render-replay/v3\"\n"
           << "complete = true\n"
           << "vblank_budget = " << budget << "\n";
    if (recorder.finish_on_close) {
        output << "record_on_close = true\n";
    } else {
        output << "record_stop_field = " << recorder.stop_field << "\n"
               << "record_stable_vblanks = " << kStableFieldVblanks << "\n\n"
               << "[checkpoint]\nkind = \"u16\"\naddress = \"0x8006F94E\"\nequals = "
               << recorder.stop_field << "\n";
    }
    const std::array<const char*, SDL_CONTROLLER_AXIS_MAX> axes{{"left_x", "left_y", "right_x", "right_y", "trigger_left", "trigger_right"}};
    for (const RecordedRun& run : recorder.runs) {
        output << "\n[[vblank]]\nrepeat = " << run.repeat << "\n";
        for (size_t slot = 0; slot < 2; ++slot) {
            const Pad& pad = run.state.pads[slot];
            const char prefix = static_cast<char>('1' + slot);
            output << "p" << prefix << "_connected = " << (pad.connected ? "true" : "false") << "\n"
                   << "p" << prefix << "_mode = \"" << mode_name(pad.mode) << "\"\n"
                   << "p" << prefix << "_buttons = [";
            for (size_t button = 0; button < pad.buttons.size(); ++button)
                output << (button ? ", " : "") << "\"" << pad.buttons[button] << "\"";
            output << "]\n";
            for (size_t axis = 0; axis < axes.size(); ++axis)
                output << "p" << prefix << "_" << axes[axis] << " = " << pad.axes[axis] << "\n";
        }
    }
    output.close();
    if (!output.good()) { std::remove(temporary.c_str()); return false; }
    return std::rename(temporary.c_str(), recorder.path.c_str()) == 0;
}
void write_incomplete_sidecar();
bool publish_recording(std::string* error) {
    if (recorder.runs.empty()) {
        write_incomplete_sidecar();
        recorder.active = false;
        if (error) *error = "input record contains no guest VBlanks";
        return false;
    }
    State neutral = recorder.runs.back().state;
    for (Pad& pad : neutral.pads) {
        pad.buttons.clear();
        pad.axes.fill(0);
    }
    append_recorded_state(neutral);
    if (!write_recording()) {
        write_incomplete_sidecar();
        recorder.active = false;
        if (error) *error = "cannot atomically publish input record";
        return false;
    }
    std::remove((recorder.path + ".incomplete").c_str());
    recorder.complete = true;
    recorder.active = false;
    return true;
}
void write_incomplete_sidecar() {
    if (recorder.path.empty()) return;
    std::ofstream output(recorder.path + ".incomplete", std::ios::binary);
    if (output) output << "schema = \"xenogears.native-render-replay/v2\"\ncomplete = false\n";
}
bool same_semantic(const SemanticTransition& transition, const Snapshot& snapshot,
                   const LoaderState& loader) {
    return same_snapshot(transition.snapshot, snapshot) &&
           transition.loader.overlay_active == loader.overlay_active &&
           transition.loader.overlay_registered == loader.overlay_registered &&
           transition.loader.overlay_file_found == loader.overlay_file_found;
}
}

bool load(const char* path, std::string* error) {
    replay = Replay{};
    native_render_baseline_reset();
#ifdef PSX_INPUT_REPLAY_XG_AUTH_PROOF
    const char* producer_family = std::getenv("PSX_NATIVE_RENDER_PRODUCER_FAMILY");
    replay.producer_family_requested = producer_family != nullptr &&
        std::string(producer_family) == "1";
    psx_xg_render_auth_producer_family_enable(false);
    replay.producer_family_armed = false;
#endif
    std::ifstream input(path);
    if (!input) { *error = "cannot open input replay"; return false; }
    State current{};
    State current_action{};
    bool state_open = false;
    bool action_open = false;
    uint64_t repeat = 1;
    uint64_t action_min_polls = 0;
    uint64_t action_max_vblanks = 0;
    uint64_t action_repeat_cycles = 0;
    uint64_t action_until_request = 0;
    bool action_after_lifecycle = false, action_until_change = false;
    bool schema_seen = false;
    bool v2 = false;
    bool v3 = false;
    bool baseline_seen = false;
    bool record_on_close_seen = false;
    bool v2_complete = false;
    bool v2_stop_field = false;
    uint16_t v2_stop_value = 0;
    bool v2_stable_vblanks = false;
    bool checkpoint_kind = false;
    std::array<uint16_t, 2> v2_state_fields{};
    std::string line;
    while (std::getline(input, line)) {
        line = trim(line.substr(0, line.find('#')));
        if (line.empty()) continue;
        if (line == "[checkpoint]") continue;
        if (line == "[[vblank]]") {
            if (state_open) {
                if (v2 && (v2_state_fields[0] != 511u || v2_state_fields[1] != 511u)) {
                    *error = "incomplete input replay state";
                    return false;
                }
                if (!replay.budget || replay.states.size() > replay.budget ||
                    repeat > replay.budget - replay.states.size()) {
                    *error = "replay duration exceeds bound";
                    return false;
                }
                replay.states.insert(replay.states.end(), static_cast<size_t>(repeat), current);
            }
            current = State{};
            v2_state_fields.fill(0);
            repeat = 1;
            state_open = true;
            continue;
        }
        if (line == "[[action]]") {
            if (v2) { *error = "invalid input replay action"; return false; }
            if (action_open) {
                uint16_t expected = 0;
                if (!action_min_polls || !action_buttons(current_action, &expected)) { *error = "invalid replay action"; return false; }
                replay.actions.push_back({current_action, expected, action_min_polls, action_max_vblanks, action_repeat_cycles, (uint32_t)action_until_request, action_after_lifecycle, action_until_change});
            }
            current_action = State{}; action_min_polls = action_max_vblanks = action_repeat_cycles = action_until_request = 0; action_after_lifecycle = action_until_change = false; action_open = true; continue;
        }
        const size_t equals = line.find('=');
        if (equals == std::string::npos) { *error = "malformed input replay"; return false; }
        const std::string key = trim(line.substr(0, equals));
        const std::string value = trim(line.substr(equals + 1));
        if (key == "schema") {
            if (schema_seen || (value != "\"xenogears.native-render-replay/v1\"" &&
                                value != "\"xenogears.native-render-replay/v2\"" &&
                                value != "\"xenogears.native-render-replay/v3\"")) {
                *error = "unsupported input replay schema";
                return false;
            }
            schema_seen = true;
            v2 = value != "\"xenogears.native-render-replay/v1\"";
            v3 = value == "\"xenogears.native-render-replay/v3\"";
            replay.state_config = v2;
            continue;
        }
        if (key == "complete") {
            if (!v2 || value != "true") { *error = "incomplete input replay"; return false; }
            v2_complete = true;
            continue;
        }
        if (key == "baseline") {
            if (!v3 || baseline_seen || value != "true") { *error = "invalid baseline request"; return false; }
            baseline_seen = true;
            replay.baseline_request = true;
            continue;
        }
        if (key == "record_on_close") {
            if (!v3 || record_on_close_seen || value != "true") {
                *error = "invalid close-record completion";
                return false;
            }
            record_on_close_seen = true;
            continue;
        }
        if (key == "kind" && value == "\"u16\"") { checkpoint_kind = true; continue; }
        uint64_t parsed = 0;
        if (key == "vblank_budget") { if (!integer(value, &replay.budget) || !replay.budget || replay.budget > kMaxReplayVblanks) { *error = "invalid vblank budget"; return false; } continue; }
        if (key == "record_stop_field" && v2) { if (!integer(value, &parsed) || !parsed || parsed > UINT16_MAX) { *error = "invalid record stop field"; return false; } v2_stop_value = static_cast<uint16_t>(parsed); v2_stop_field = true; continue; }
        if (key == "record_stable_vblanks" && v2) { if (!integer(value, &parsed) || parsed != kStableFieldVblanks) { *error = "invalid record stability"; return false; } v2_stable_vblanks = true; continue; }
        if (key == "address") { if (!integer(value.substr(1, value.size() - 2), &parsed) || parsed > UINT32_MAX) { *error = "invalid checkpoint address"; return false; } replay.checkpoint_address = static_cast<uint32_t>(parsed); continue; }
        if (key == "equals") { if (!integer(value, &parsed) || parsed > UINT16_MAX) { *error = "invalid checkpoint value"; return false; } replay.checkpoint_expected = static_cast<uint16_t>(parsed); continue; }
        if (key == "repeat" && state_open) { if (!integer(value, &repeat) || !repeat) { *error = "invalid replay repeat"; return false; } continue; }
        if (key == "min_polls" && action_open) { if (!integer(value, &action_min_polls) || !action_min_polls) { *error = "invalid action poll threshold"; return false; } continue; }
        if (key == "max_vblanks" && action_open) { if (!integer(value, &action_max_vblanks) || !action_max_vblanks) { *error = "invalid action VBlank bound"; return false; } continue; }
        if (key == "repeat_cycles" && action_open) { if (!integer(value, &action_repeat_cycles) || !action_repeat_cycles || action_repeat_cycles > 8u) { *error = "invalid repeat cycle bound"; return false; } continue; }
        if (key == "until_request" && action_open) { if (!integer(value, &action_until_request) || !action_until_request || action_until_request > UINT32_MAX) { *error = "invalid action request condition"; return false; } continue; }
        if (key == "after_lifecycle" && action_open) { if (value != "true") { *error = "invalid lifecycle condition"; return false; } action_after_lifecycle = true; continue; }
        if (key == "until_change" && action_open) { if (value != "true") { *error = "invalid action change condition"; return false; } action_until_change = true; continue; }
        if (action_open && assign(&current_action, key, value)) continue;
        if (!state_open || !assign(&current, key, value)) { *error = "invalid replay input"; return false; }
        if (v2) {
            const uint16_t field = v2_state_field_bit(key);
            if (!field) { *error = "invalid input replay state"; return false; }
            v2_state_fields[key[1] - '1'] |= field;
        }
    }
    if (state_open) {
        if (v2 && (v2_state_fields[0] != 511u || v2_state_fields[1] != 511u)) {
            *error = "incomplete input replay state";
            return false;
        }
        if (!replay.budget || replay.states.size() > replay.budget ||
            repeat > replay.budget - replay.states.size()) {
            *error = "replay duration exceeds bound";
            return false;
        }
        replay.states.insert(replay.states.end(), static_cast<size_t>(repeat), current);
    }
    if (action_open) {
        uint16_t expected = 0;
        if (!action_min_polls || !action_buttons(current_action, &expected)) { *error = "invalid replay action"; return false; }
        replay.actions.push_back({current_action, expected, action_min_polls, action_max_vblanks, action_repeat_cycles, (uint32_t)action_until_request, action_after_lifecycle, action_until_change});
    }
    const bool checkpoint_record = v2 && !record_on_close_seen &&
        v2_complete && v2_stop_field && v2_stable_vblanks && checkpoint_kind &&
        v2_stop_value == replay.checkpoint_expected && replay.checkpoint_address != 0u;
    const bool close_record = v3 && record_on_close_seen && v2_complete &&
        !v2_stop_field && !v2_stable_vblanks && !checkpoint_kind &&
        replay.checkpoint_address == 0u && replay.checkpoint_expected == 0u &&
        !baseline_seen && replay.actions.empty() &&
        replay.states.size() == replay.budget;
    if (!schema_seen ||
        (v2 && !(checkpoint_record || close_record)) ||
        (!v2 && replay.checkpoint_address == 0u) ||
        (v2 && (replay.states.empty() || !neutral_state(replay.states.back()))) ||
        !replay.budget || (!replay.states.empty() && !replay.actions.empty()) ||
        (replay.states.empty() && replay.actions.empty()) ||
        replay.states.size() > replay.budget) {
        *error = "incomplete input replay";
        return false;
    }
    replay.checkpoint_configured = !v2 || checkpoint_record;
    replay.record_on_close = close_record;
    const char* baseline_env = std::getenv("PSX_INPUT_REPLAY_BASELINE");
    if (!replay.baseline_request && baseline_env && baseline_env[0] == '1')
        replay.baseline_request = true;
    if (replay.baseline_request &&
        (!v3 || !v2_stop_field || v2_stop_value != 5u ||
         replay.checkpoint_expected != 5u)) {
        *error = "baseline request requires a complete v3 Field 5 trace";
        return false;
    }
#ifndef PSX_INPUT_REPLAY_XG_BASELINE
    if (replay.baseline_request) {
        *error = "baseline adapter is unavailable";
        return false;
    }
#else
    if (replay.baseline_request) {
        const XgNativeRenderBaselineResult configured =
            xg_native_render_baseline_configure(
                psx_game_identity_runtime(), static_cast<uint32_t>(replay.budget),
                &replay.baseline_config);
        (void)configured;
    }
#endif

    replay.loaded = true;
    return true;
}
bool active() { return replay.loaded; }
bool record_begin_impl(const char* path, uint16_t stop_field,
                       uint64_t max_vblanks, bool finish_on_close,
                       std::string* error) {
    if (replay.loaded || recorder.active || !path || !*path || !max_vblanks ||
        (!finish_on_close && !stop_field)) {
        if (error) *error = "invalid input record request";
        return false;
    }
    if (destination_exists(path)) {
        if (error) *error = "input record destination must not exist";
        return false;
    }
    recorder = Recorder{};
    recorder.path = path;
    recorder.stop_field = stop_field;
    recorder.max_vblanks = max_vblanks;
    recorder.finish_on_close = finish_on_close;
    recorder.active = true;
    return true;
}
bool record_begin(const char* path, uint16_t stop_field, uint64_t max_vblanks,
                  std::string* error) {
    return record_begin_impl(path, stop_field, max_vblanks, false, error);
}
bool record_begin_until_close(const char* path, uint64_t max_vblanks,
                              std::string* error) {
    return record_begin_impl(path, 0u, max_vblanks, true, error);
}
bool recording() { return recorder.active; }
void record_note_guest_vblank() {
    if (recorder.active) recorder.guest_vblanks++;
}
bool record_snapshot(const HostInputSnapshot& snapshot, std::string* error) {
    if (!recorder.active || recorder.snapshot_vblank == recorder.guest_vblanks) {
        if (error) *error = "input record requires one snapshot per guest VBlank";
        return false;
    }
    recorder.snapshot_vblank = recorder.guest_vblanks;
    if (recorder.guest_vblanks > recorder.max_vblanks) {
        write_incomplete_sidecar();
        recorder.active = false;
        if (error) *error = "input record VBlank bound reached";
        return false;
    }
    append_recorded_state(record_state(snapshot));
    if (!recorder.finishing) return true;
    return publish_recording(error);
}
bool record_snapshot(const host_input::HostInputSnapshot& snapshot,
                     const std::array<PsxNetPad, 2>& pads, std::string* error) {
    const auto& keyboard = snapshot.keyboard();
    const auto& controllers = snapshot.controllers();
    if (keyboard.size() != SDL_NUM_SCANCODES || controllers.size() > 1024u) {
        if (error) *error = "invalid host input snapshot";
        return false;
    }
    return record_snapshot(record_projection(snapshot, pads), error);
}
void record_note_scene(const Snapshot& snapshot, const LoaderState& loader) {
    if (!recorder.active || recorder.finish_on_close) return;
    const bool stable = snapshot.valid_field && snapshot.field_context != 0u &&
                        snapshot.requested_module == 0u && snapshot.active_module == UINT32_MAX &&
                        snapshot.masked_field_id == recorder.stop_field && loader.cd_has_disc != 0 &&
                        loader.cd_reading == 0 && loader.cd_pending_pending == 0;
    recorder.stable_field_vblanks = stable ? recorder.stable_field_vblanks + 1u : 0u;
    if (recorder.stable_field_vblanks >= kStableFieldVblanks) recorder.finishing = true;
}
bool record_complete() { return recorder.complete; }
bool record_close(std::string* error) {
    if (!recorder.active) return true;
    if (!recorder.finish_on_close) {
        record_abort();
        return true;
    }
    return publish_recording(error);
}
bool write_record_evidence(const char* path, std::string* error) {
#ifndef PSX_INPUT_REPLAY_XG_AUTH_PROOF
    (void)path;
    if (error) *error = "native render recording evidence is unavailable";
    return false;
#else
    if (!recorder.complete || !path || !*path) {
        if (error) *error = "input record is not complete";
        return false;
    }
    if (destination_exists(path)) {
        if (error) *error = "record evidence destination must not exist";
        return false;
    }
    PsxXgRenderModelFt4ShadowSnapshot model{};
    PsxXgRenderModelFt3ShadowSnapshot model_ft3{};
    PsxXgRenderSpriteFt4ShadowSnapshot sprite{};
    PsxXgRenderFieldPolylineSnapshot field_polyline{};
    PsxXgRenderModeSnapshot mode{};
    psx_xg_render_auth_model_ft4_shadow_snapshot(&model);
    psx_xg_render_auth_model_ft3_shadow_snapshot(&model_ft3);
    psx_xg_render_auth_sprite_ft4_shadow_snapshot(&sprite);
    psx_xg_render_auth_field_polyline_snapshot(&field_polyline);
    psx_xg_render_auth_mode_snapshot(&mode);
    const bool model_pass = model.invocation_count > 0u &&
        model.primitive_count > 0u &&
        model.match_count == model.primitive_count &&
        model.mismatch_count == 0u && !model.pending && !model.blocked;
    const bool sprite_pass = sprite.projection_count > 0u &&
        sprite.match_count == sprite.projection_count &&
        sprite.mismatch_count == 0u && !sprite.context_active &&
        !sprite.pending && !sprite.blocked;
    const bool mode_pass =
        mode.modes.requested_render_mode == GUEST_RENDER_RENDER_SHADOW &&
        mode.modes.effective_render_mode == GUEST_RENDER_RENDER_SHADOW;
    const std::string temporary = std::string(path) + ".partial";
    std::remove(temporary.c_str());
    std::ofstream output(temporary, std::ios::binary);
    if (!output) {
        if (error) *error = "cannot create record evidence";
        return false;
    }
    output << "{\"schema\":\"xenogears.native-render-session-evidence/v2\""
           << ",\"status\":\"" << (model_pass && sprite_pass && mode_pass ? "PASS" : "FAIL") << "\""
           << ",\"input\":{\"complete\":true,\"vblanks\":" << recorder.guest_vblanks;
    if (recorder.finish_on_close)
        output << ",\"completion\":\"record_on_close\"}";
    else
        output << ",\"checkpoint_field\":" << recorder.stop_field << "}";
    output
           << ",\"render\":{\"requested\":\""
           << render_mode_name(mode.modes.requested_render_mode)
           << "\",\"effective\":\""
           << render_mode_name(mode.modes.effective_render_mode) << "\"}"
           << ",\"model_ft4\":{\"status\":\"" << (model_pass ? "PASS" : "FAIL")
           << "\",\"dispatch_begins\":" << model.dispatch_begin_count
           << ",\"dispatch_caller_rejects\":"
           << model.dispatch_caller_reject_count
           << ",\"dispatch_mode_rejects\":"
           << model.dispatch_mode_reject_count
           << ",\"average_seams\":" << model.average_seam_count
           << ",\"farthest_seams\":" << model.farthest_seam_count
           << ",\"seams_without_context\":"
           << model.seam_without_context_count
           << ",\"invocations\":" << model.invocation_count
           << ",\"primitives\":" << model.primitive_count
           << ",\"matches\":" << model.match_count
           << ",\"mismatches\":" << model.mismatch_count
           << ",\"payload_mismatches\":" << model.payload_mismatch_count
           << ",\"geometry_mismatches\":" << model.geometry_mismatch_count
           << ",\"tag_mismatches\":" << model.tag_mismatch_count
           << ",\"ot_mismatches\":" << model.ot_mismatch_count
           << ",\"cursor_mismatches\":" << model.cursor_mismatch_count
           << ",\"counter_mismatches\":" << model.counter_mismatch_count
           << ",\"template_captures\":" << model.template_capture_count
           << ",\"template_hits\":" << model.template_hit_count
           << ",\"template_misses\":" << model.template_miss_count
           << ",\"last_primitive_count\":" << model.last_primitive_count
           << ",\"first_mismatch_primitive\":" << model.first_mismatch_primitive
           << ",\"first_mismatch_packet\":" << model.first_mismatch_packet
           << ",\"last_model_address\":" << model.last_model_address
           << ",\"last_topology_base\":" << model.last_topology_base
           << ",\"last_material_base\":" << model.last_material_base
           << ",\"last_attribute_address\":" << model.last_attribute_address
           << ",\"last_material_word\":" << model.last_material_word
           << ",\"last_group_count\":" << model.last_group_count
           << ",\"last_target_count\":" << model.last_target_count
           << ",\"last_dispatch_caller\":" << model.last_dispatch_caller
           << ",\"last_dispatch_mode\":" << model.last_dispatch_mode
           << ",\"last_seam_pc\":" << model.last_seam_pc
           << ",\"prepare_failure_detail\":" << model.prepare_failure_detail
           << ",\"first_payload_mismatch\":";
    write_ft4_payload_mismatch(output, model.first_payload_mismatch);
    output << ",\"blocker\":" << model.blocker
           << ",\"pending\":" << (model.pending ? "true" : "false")
           << ",\"blocked\":" << (model.blocked ? "true" : "false") << "}"
           << ",\"model_ft3\":{\"invocations\":"
           << model_ft3.invocation_count
           << ",\"native_cutovers\":" << model_ft3.native_cutover_count
           << ",\"native_primitives\":" << model_ft3.native_primitive_count
           << ",\"primitives\":" << model_ft3.primitive_count
           << ",\"matches\":" << model_ft3.match_count
           << ",\"mismatches\":" << model_ft3.mismatch_count
           << ",\"payload_mismatches\":"
           << model_ft3.payload_mismatch_count
           << ",\"geometry_mismatches\":"
           << model_ft3.geometry_mismatch_count
           << ",\"tag_mismatches\":" << model_ft3.tag_mismatch_count
           << ",\"ot_mismatches\":" << model_ft3.ot_mismatch_count
           << ",\"cursor_mismatches\":" << model_ft3.cursor_mismatch_count
           << ",\"counter_mismatches\":" << model_ft3.counter_mismatch_count
           << ",\"template_captures\":" << model_ft3.template_capture_count
           << ",\"template_hits\":" << model_ft3.template_hit_count
           << ",\"template_misses\":" << model_ft3.template_miss_count
           << ",\"raw_color_differences\":"
           << model_ft3.raw_color_difference_count
           << ",\"first_mismatch_packet\":"
           << model_ft3.first_mismatch_packet
           << ",\"last_group_count\":" << model_ft3.last_group_count
           << ",\"last_target_count\":" << model_ft3.last_target_count
           << ",\"prepare_failure_detail\":"
           << model_ft3.prepare_failure_detail
           << ",\"first_payload_mismatch\":";
    write_ft4_payload_mismatch(output, model_ft3.first_payload_mismatch);
    output
           << ",\"blocker\":" << model_ft3.blocker
           << ",\"pending\":" << (model_ft3.pending ? "true" : "false")
           << ",\"blocked\":" << (model_ft3.blocked ? "true" : "false")
           << "}"
           << ",\"sprite_ft4\":{\"status\":\"" << (sprite_pass ? "PASS" : "FAIL")
           << "\",\"native_cutovers\":" << sprite.native_cutover_count
           << ",\"native_primitives\":" << sprite.native_primitive_count
           << ",\"field_builder_begins\":"
           << sprite.field_builder_begin_count
           << ",\"field_builder_native_cutovers\":"
           << sprite.field_builder_native_cutover_count
           << ",\"field_builder_native_primitives\":"
           << sprite.field_builder_native_primitive_count
           << ",\"field_builder_template_captures\":"
           << sprite.field_builder_template_capture_count
           << ",\"field_builder_template_updates\":"
           << sprite.field_builder_template_update_count
           << ",\"field_builder_template_invalidations\":"
           << sprite.field_builder_template_invalidation_count
           << ",\"field_builder_template_count\":"
           << sprite.field_builder_template_count
           << ",\"field_builder_dma_replay_primitives\":"
           << sprite.field_builder_dma_replay_primitive_count
           << ",\"field_builder_primitives\":"
           << sprite.field_builder_primitive_count
           << ",\"field_builder_matches\":"
           << sprite.field_builder_match_count
           << ",\"field_builder_mismatches\":"
           << sprite.field_builder_mismatch_count
           << ",\"field_builder_active_scenes\":"
           << sprite.field_builder_active_scene_count
           << ",\"callers\":" << sprite.caller_count
           << ",\"empty_callers\":" << sprite.empty_caller_count
           << ",\"projections\":" << sprite.projection_count
           << ",\"matches\":" << sprite.match_count
           << ",\"mismatches\":" << sprite.mismatch_count
           << ",\"geometry_mismatches\":" << sprite.geometry_mismatch_count
           << ",\"payload_mismatches\":" << sprite.payload_mismatch_count
           << ",\"last_caller\":" << sprite.last_caller
           << ",\"last_field_builder_caller\":"
           << sprite.last_field_builder_caller
           << ",\"field_builder_caller_candidates\":["
           << sprite.field_builder_caller_candidates[0] << ','
           << sprite.field_builder_caller_candidates[1] << ','
           << sprite.field_builder_caller_candidates[2] << ','
           << sprite.field_builder_caller_candidates[3] << ']'
           << ",\"field_builder_caller_counts\":["
           << sprite.field_builder_caller_counts[0] << ','
           << sprite.field_builder_caller_counts[1] << ','
           << sprite.field_builder_caller_counts[2] << ','
           << sprite.field_builder_caller_counts[3] << ']'
           << ",\"field_builder_first_mismatch_packet\":"
           << sprite.field_builder_first_mismatch_packet
           << ",\"field_builder_first_mismatch_descriptor\":"
           << sprite.field_builder_first_mismatch_descriptor
           << ",\"field_builder_first_mismatch_caller\":"
           << sprite.field_builder_first_mismatch_caller
           << ",\"field_builder_first_mismatch_bits\":"
           << sprite.field_builder_first_mismatch_bits
           << ",\"field_builder_expected_xy\":["
           << sprite.field_builder_expected_xy[0] << ','
           << sprite.field_builder_expected_xy[1] << ','
           << sprite.field_builder_expected_xy[2] << ','
           << sprite.field_builder_expected_xy[3] << ']'
           << ",\"field_builder_actual_xy\":["
           << sprite.field_builder_actual_xy[0] << ','
           << sprite.field_builder_actual_xy[1] << ','
           << sprite.field_builder_actual_xy[2] << ','
           << sprite.field_builder_actual_xy[3] << ']'
           << ",\"field_builder_expected_uv\":["
           << sprite.field_builder_expected_uv[0] << ','
           << sprite.field_builder_expected_uv[1] << ','
           << sprite.field_builder_expected_uv[2] << ','
           << sprite.field_builder_expected_uv[3] << ']'
           << ",\"field_builder_actual_uv\":["
           << sprite.field_builder_actual_uv[0] << ','
           << sprite.field_builder_actual_uv[1] << ','
           << sprite.field_builder_actual_uv[2] << ','
           << sprite.field_builder_actual_uv[3] << ']'
           << ",\"field_builder_expected_tpage\":"
           << sprite.field_builder_expected_tpage
           << ",\"field_builder_actual_tpage\":"
           << sprite.field_builder_actual_tpage
           << ",\"field_builder_expected_clut\":"
           << sprite.field_builder_expected_clut
           << ",\"field_builder_actual_clut\":"
           << sprite.field_builder_actual_clut
           << ",\"field_builder_actual_command\":"
           << sprite.field_builder_actual_command
           << ",\"field_builder_blocker\":"
           << sprite.field_builder_blocker
           << ",\"field_builder_min_packet\":"
           << sprite.field_builder_min_packet
           << ",\"field_builder_max_packet\":"
           << sprite.field_builder_max_packet
           << ",\"last_sprite_address\":" << sprite.last_sprite_address
           << ",\"last_data_address\":" << sprite.last_data_address
           << ",\"last_descriptor_address\":" << sprite.last_descriptor_address
           << ",\"last_primitive_count\":" << sprite.last_primitive_count
           << ",\"first_mismatch_packet\":" << sprite.first_mismatch_packet
           << ",\"first_mismatch_descriptor\":" << sprite.first_mismatch_descriptor
           << ",\"first_payload_mismatch\":";
    write_ft4_payload_mismatch(output, sprite.first_payload_mismatch);
    output << ",\"blocker\":" << sprite.blocker
           << ",\"blocker_detail\":" << sprite.blocker_detail
           << ",\"context_active\":" << (sprite.context_active ? "true" : "false")
           << ",\"pending\":" << (sprite.pending ? "true" : "false")
           << ",\"blocked\":" << (sprite.blocked ? "true" : "false")
           << ",\"field_builder_pending\":"
           << (sprite.field_builder_pending ? "true" : "false")
           << ",\"field_builder_blocked\":"
           << (sprite.field_builder_blocked ? "true" : "false")
           << "},\"field_polyline\":{\"begins\":"
           << field_polyline.begin_count
           << ",\"invocations\":" << field_polyline.invocation_count
           << ",\"native_cutovers\":" << field_polyline.native_cutover_count
           << ",\"native_primitives\":" << field_polyline.native_primitive_count
           << ",\"primitives\":" << field_polyline.primitive_count
           << ",\"matches\":" << field_polyline.match_count
           << ",\"mismatches\":" << field_polyline.mismatch_count
           << ",\"first_mismatch_packet\":"
           << field_polyline.first_mismatch_packet
           << ",\"blocker\":" << field_polyline.blocker
           << ",\"pending\":" << (field_polyline.pending ? "true" : "false")
           << ",\"blocked\":" << (field_polyline.blocked ? "true" : "false")
           << "}}\n";
    output.close();
    if (!output.good() || std::rename(temporary.c_str(), path) != 0) {
        std::remove(temporary.c_str());
        if (error) *error = "cannot atomically publish record evidence";
        return false;
    }
    return true;
#endif
}
void record_abort() {
    if (!recorder.active) return;
    write_incomplete_sidecar();
    recorder.active = false;
}
bool attach(SDL_GameController* players[2], std::string* error) {
    const bool p2_needed = replay.state_config && std::any_of(
        replay.states.begin(), replay.states.end(), [](const State& state) {
            return state.pads[1].connected;
        });
    for (int slot = 0; slot < (p2_needed ? 2 : 1); ++slot) {
        replay.devices[slot] = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
        if (replay.devices[slot] < 0 || !(players[slot] = SDL_GameControllerOpen(replay.devices[slot]))) { *error = SDL_GetError(); detach(players); return false; }
        replay.controllers[slot] = players[slot];
    }
    return true;
}
void detach(SDL_GameController* players[2]) {
    for (int slot = 0; slot < 2; ++slot) { if (players[slot]) SDL_GameControllerClose(players[slot]); players[slot] = nullptr; replay.controllers[slot] = nullptr; if (replay.devices[slot] >= 0) SDL_JoystickDetachVirtual(replay.devices[slot]); replay.devices[slot] = -1; }
}
bool latch_vblank() {
    if (replay.index >= replay.budget || (!replay.actions.empty() && replay.action_index >= replay.actions.size()) ||
        (replay.actions.empty() && replay.index >= replay.states.size())) {
        if (replay.record_on_close && replay.actions.empty() &&
            replay.index == replay.budget && replay.index == replay.states.size()) {
            replay.reason = StopReason::TraceComplete;
            replay.latch_failure = 0u;
        } else {
            replay.reason = StopReason::CheckpointNotReached;
            replay.latch_failure = 1u;
        }
        return false;
    }
    const Pad neutral{};
    const Action* action = replay.actions.empty() ? nullptr : &replay.actions[replay.action_index];
    const bool lifecycle_ready = replay.lifecycle_stage == 4u && replay.lifecycle_neutral_polls >= 12u;
    const State* state = replay.actions.empty() ? &replay.states[replay.index] : nullptr;
    const Pad& pad = replay.actions.empty() ? state->pads[0] :
        ((action->after_lifecycle && !lifecycle_ready) ||
         (replay.media.fmv_active && !action->until_request && !action->after_lifecycle)
             ? neutral : replay.actions[replay.action_index].state.pads[0]);
    if (action && action->after_lifecycle && lifecycle_ready && !replay.action_baseline_set) {
        replay.action_baseline = replay.snapshot;
        replay.action_baseline_set = true;
    }
    if (!replay.controllers[0] || !update_pad(replay.controllers[0], pad)) {
        replay.reason = StopReason::CheckpointNotReached;
        replay.latch_failure = 2u;
        return false;
    }
    if (state && replay.controllers[1]) {
        const Pad neutral{};
        if (!update_pad(replay.controllers[1], state->pads[1].connected ? state->pads[1] : neutral)) {
            replay.reason = StopReason::CheckpointNotReached;
            replay.latch_failure = 3u;
            return false;
        }
    }
    replay.counters.provider_updates++;
    SDL_GameControllerUpdate();
    replay.index++; replay.counters.vblank_latches++; replay.counters.trace_state_latches++;
    if (action && action->max_vblanks && (!action->after_lifecycle || lifecycle_ready) &&
        ++replay.action_vblanks >= action->max_vblanks) {
        replay.action_index++;
        replay.action_polls = replay.action_vblanks = 0;
    }
    return true;
}
bool current_pad_config(int slot, bool* connected, PadMode* mode) {
    if (!replay.loaded || !replay.state_config || slot < 0 || slot > 1 ||
        replay.index == 0 || replay.index > replay.states.size()) return false;
    const Pad& pad = replay.states[replay.index - 1u].pads[slot];
    if (connected) *connected = pad.connected;
    if (mode) *mode = pad.mode;
    return true;
}
SDL_GameController* controller(int slot) {
    return slot >= 0 && slot < 2 ? replay.controllers[slot] : nullptr;
}
void note_guest_vblank() {
    replay.counters.guest_vblank_callbacks++;
}
void note_capture(int slot) { replay.counters.capture_samples++; if (slot == 0) replay.counters.p1_samples++; else if (slot == 1) replay.counters.p2_samples++; }
void note_mapping() { replay.counters.mapping_reads++; }
void note_sio() { replay.counters.sio_applies++; }
void note_snapshot(const Snapshot& snapshot) {
    replay.snapshot = snapshot;
#ifdef PSX_INPUT_REPLAY_XG_AUTH_PROOF
    if (!replay.auth_instrumentation_started && snapshot.valid_field &&
        snapshot.field_context != 0u && snapshot.masked_field_id == 5u) {
        psx_xg_render_auth_cold_enable(true);
        psx_xg_render_auth_instrumentation_snapshot(
            &replay.auth_instrumentation_start);
        replay.auth_instrumentation_started = true;
    }
    if (replay.producer_family_requested && !replay.producer_family_armed &&
        snapshot.valid_field && snapshot.field_context != 0u &&
        snapshot.masked_field_id == 5u) {
        psx_xg_render_auth_producer_family_enable(true);
        replay.producer_family_armed = true;
    }
#endif
#ifdef PSX_INPUT_REPLAY_XG_BASELINE
    if (replay.baseline_request && !replay.baseline_armed &&
        snapshot.valid_field && snapshot.field_context != 0u &&
        snapshot.masked_field_id == 5u) {
        if (native_render_baseline_arm(&replay.baseline_config)) {
            replay.baseline_armed = true;
            native_render_baseline_set_auto_finalize_vblanks(
                kTask5ObservationVblanks);
            replay.baseline_sample_attempted = true;
            const XgNativeRenderBaselineResult sample =
                xg_native_render_baseline_sample(memory_get_ram_ptr(),
                                                 2u * 1024u * 1024u);
            if (sample.success)
                native_render_baseline_note_camera_actor_digest(sample.digest);
        }
    }
#endif
    if (replay.lifecycle_stage == 0u && snapshot.requested_module == 1u) replay.lifecycle_stage = 1u;
    else if (replay.lifecycle_stage == 1u && snapshot.active_module == 1u) replay.lifecycle_stage = 2u;
    else if (replay.lifecycle_stage == 2u && snapshot.requested_module == 0u && snapshot.active_module == UINT32_MAX) replay.lifecycle_stage = 3u;
    else if (replay.lifecycle_stage == 3u && snapshot.valid_field && snapshot.masked_field_id == 490u) replay.lifecycle_stage = 4u;
    if (!replay.actions.empty() && replay.action_index + 1u < replay.actions.size() &&
        replay.actions[replay.action_index].until_request != 0u &&
        replay.action_polls >= replay.actions[replay.action_index].min_polls &&
        snapshot.requested_module == replay.actions[replay.action_index].until_request) {
        replay.action_index++;
        replay.action_polls = replay.action_vblanks = 0;
    }
    if (!replay.actions.empty() && replay.action_index + 1u < replay.actions.size() &&
        replay.actions[replay.action_index].until_change && replay.action_baseline_set &&
        !same_snapshot(replay.action_baseline, snapshot)) {
        const Action& action = replay.actions[replay.action_index];
        if (action.repeat_cycles && replay.completed_cycles + 1u < action.repeat_cycles) {
            replay.completed_cycles++;
            replay.lifecycle_stage = snapshot.requested_module == 1u ? 1u : 0u;
            replay.lifecycle_neutral_polls = 0;
            replay.action_polls = replay.action_vblanks = 0;
            replay.action_baseline_set = false;
        } else {
            replay.completed_cycles++;
            replay.action_index++;
            replay.action_polls = replay.action_vblanks = 0;
        }
    }
}
void note_loader_state(const LoaderState& state) {
    replay.loader = state;
    if (replay.semantic_transitions.empty() ||
        !same_semantic(replay.semantic_transitions.back(), replay.snapshot, replay.loader)) {
        if (replay.semantic_transitions.size() == kMaxSemanticTransitions) {
            replay.semantic_overflow = true;
        } else {
            replay.semantic_transitions.push_back({replay.counters.vblank_latches,
                                                   replay.snapshot, replay.loader});
        }
    }
    if (!replay.snapshot.valid_field) return;
    if (replay.transitions.size() == 16u ||
        (!replay.transitions.empty() &&
         same_snapshot(replay.transitions.back().snapshot, replay.snapshot) &&
         same_loader(replay.transitions.back().loader, replay.loader) &&
         same_media(replay.transitions.back().media, replay.media))) return;
    replay.transitions.push_back({replay.counters.vblank_latches, replay.snapshot, replay.loader, replay.media});
}
void note_media_state(const MediaState& state) {
    const uint64_t vblank = replay.counters.vblank_latches;
    if (replay.media_samples == 0u)
        replay.first_mdec_decode_count = state.mdec_decode_count;
    ++replay.media_samples;
    if (state.mdec_decode_count > replay.max_mdec_decode_count)
        replay.max_mdec_decode_count = state.mdec_decode_count;
    if (state.fmv_active) {
        if (!replay.fmv_seen) {
            replay.fmv_seen = true;
            replay.fmv_first_vblank = vblank;
        }
        replay.fmv_last_vblank = vblank;
        ++replay.fmv_active_samples;
    }
    if (state.xa_streaming) {
        if (!replay.xa_seen) {
            replay.xa_seen = true;
            replay.xa_first_vblank = vblank;
        }
        replay.xa_last_vblank = vblank;
        ++replay.xa_streaming_samples;
    }
    replay.media = state;
}
void note_sio_receipt(const SioReceipt& receipt) {
    replay.receipt = receipt;
    if (receipt.polls == replay.last_receipt_polls) return;
    replay.last_receipt_polls = receipt.polls;
    const uint16_t buttons = static_cast<uint16_t>(receipt.buttons_low | (receipt.buttons_high << 8));
    if (replay.lifecycle_stage == 4u && buttons == 0xFFFFu && receipt.slot == 0u && receipt.id == 0x41u && receipt.ack == 0x5Au && !receipt.analog)
        replay.lifecycle_neutral_polls++;
    const uint64_t vblank = replay.counters.vblank_latches;
    if ((buttons & 0x4000u) == 0u) {
        if (!replay.cross_count++) replay.cross_first = vblank;
        replay.cross_last = vblank;
    } else if (buttons == 0xFFFFu) {
        if (!replay.neutral_count++) replay.neutral_first = vblank;
        replay.neutral_last = vblank;
    } else if (buttons == 0xFFF7u) {
        if (!replay.start_count++) replay.start_first = vblank;
        replay.start_last = vblank;
    } else {
        if (!replay.other_count++) replay.other_first = vblank;
        replay.other_last = vblank;
    }
    if (!replay.actions.empty() && replay.action_index + 1u < replay.actions.size() &&
        (!replay.media.fmv_active || replay.actions[replay.action_index].expected_buttons == 0xFFFFu ||
         replay.actions[replay.action_index].until_request || replay.actions[replay.action_index].after_lifecycle) &&
        receipt.slot == 0u && receipt.id == 0x41u && receipt.ack == 0x5Au && !receipt.analog &&
        buttons == replay.actions[replay.action_index].expected_buttons) {
        replay.action_polls++;
        if (!replay.actions[replay.action_index].until_request && !replay.actions[replay.action_index].until_change &&
            replay.action_polls >= replay.actions[replay.action_index].min_polls) {
            replay.action_index++;
            replay.action_polls = replay.action_vblanks = 0;
        }
    }
}
bool checkpoint(uint32_t* address, uint16_t* expected) {
    if (!active() || !replay.checkpoint_configured) return false;
    *address = replay.checkpoint_address;
    *expected = replay.checkpoint_expected;
    return true;
}
void observe_checkpoint(uint16_t value) {
    if (!replay.checkpoint_configured) return;
    if (!replay.state_config) {
        if (value == replay.checkpoint_expected) replay.reason = StopReason::CheckpointReached;
        return;
    }
    if (value == replay.checkpoint_expected && !replay.checkpoint_seen) {
        replay.checkpoint_seen = true;
        replay.checkpoint_vblank = replay.index;
        replay.checkpoint_snapshot = replay.snapshot;
    }
    if (replay.checkpoint_seen && replay.index >= replay.states.size()) replay.reason = StopReason::CheckpointReached;
}
bool finished() { return replay.reason != StopReason::None; }
StopReason stop_reason() { return replay.reason; }
Counters counters() { return replay.counters; }
bool write_evidence(const char* path, uint16_t field_id, const char* backend) {
    std::ofstream output(path); if (!output) return false;
    const uint16_t reported_checkpoint_field = replay.checkpoint_configured
        ? (replay.checkpoint_seen ? replay.checkpoint_expected : field_id) : 0u;
    const Counters c = counters();
    NativeRenderBaselineSnapshot baseline{};
    native_render_baseline_finalize();
    native_render_baseline_snapshot(&baseline);
    struct ProducerFamilyEvidence {
        uint64_t geometry_count = 0;
        uint64_t candidate_count = 0;
        uint64_t match_count = 0;
        uint64_t mismatch_count = 0;
        uint32_t last_ot_bucket = 0;
        uint32_t last_runtime_result = 0;
        uint32_t last_compare_result = 0;
        uint32_t first_mismatch_word = 0;
        uint32_t first_mismatch_byte = 0;
        uint32_t blocker = 0;
        uint32_t source_event_count = 0;
        uint32_t source_blocker = 0;
        uint32_t source_context_bits = 0;
        uint32_t collector_phase = 0;
        uint32_t collector_blocker = 0;
        uint32_t collector_access_count = 0;
        uint32_t collector_site_count = 0;
        uint32_t geometry_queued_count = 0;
        uint64_t geometry_completed_count = 0;
        bool enabled = false;
        bool blocked = false;
        bool source_blocked = false;
        bool source_overflowed = false;
        bool geometry_pending = false;
        bool geometry_blocked = false;
        bool geometry_overflowed = false;
    } producer_family;
    bool producer_family_requested = false;
    GuestRenderModes render_modes{};
    NativeRenderPresentationSnapshot render_presentation{};
    GuestRenderFallbackReason render_scene_fallback =
        GUEST_RENDER_FALLBACK_NONE;
    GuestRenderFallbackReason render_last_fallback =
        GUEST_RENDER_FALLBACK_NONE;
    uint64_t render_transaction_count = 0u;
    uint64_t render_substitution_count = 0u;
    uint64_t render_cumulative_fallback_count = 0u;
    uint64_t render_scene_fallback_count_baseline = 0u;
    uint64_t render_scene_fallback_count_delta = 0u;
    bool render_fallback_count_overflowed = false;
    bool render_bridge_snapshot_valid = false;
    GuestRenderNativeStreamSnapshot native_stream{};
    GlRendererNativeMidpointDiagnostics native_midpoint{};
    std::vector<GlRendererRetiredFailureEvent> retired_failure_events;
    std::array<uint64_t,
               GL_RETIRED_FAILURE_MIDPOINT_FIXED_WINDING_FLIP + 1u>
        retired_failure_reason_counts{};
    const uint64_t retired_failure_total =
        gl_renderer_retired_failure_event_total();
    const uint64_t retired_failure_overflow =
        gl_renderer_retired_failure_event_overflow();
    uint64_t native_rate_midpoint_presents = 0u;
    uint64_t native_rate_current_presents = 0u;
    uint64_t native_peak_midpoint_presents = 0u;
    uint64_t native_peak_current_presents = 0u;
    uint32_t native_rate_first_ms = 0u;
    uint32_t native_rate_last_ms = 0u;
    uint32_t native_peak_window_ms = 0u;
    (void)guest_render_native_stream_snapshot(&native_stream);
    gl_renderer_native_midpoint_diag(&native_midpoint);
    retired_failure_events.resize((size_t)retired_failure_total);
    retired_failure_events.resize(gl_renderer_retired_failure_events(
        retired_failure_events.data(), retired_failure_events.size()));
    for (const GlRendererRetiredFailureEvent& event : retired_failure_events)
        if (event.reason < retired_failure_reason_counts.size())
            ++retired_failure_reason_counts[event.reason];
    {
        const uint64_t total = gl_renderer_pres_total();
        const uint64_t first = total > 4096u ? total - 4096u : 0u;
        std::vector<GlPresEvent> native_events;
        GlPresEvent last{};

        if (total != 0u && gl_renderer_pres_get(total - 1u, &last)) {
            native_rate_last_ms = last.t_ms;
            for (uint64_t sequence = first; sequence < total; ++sequence) {
                GlPresEvent event{};

                if (!gl_renderer_pres_get(sequence, &event))
                    continue;
                if (event.path != GL_PRES_NATIVE_MIDPOINT &&
                    event.path != GL_PRES_NATIVE_CURRENT)
                    continue;
                native_events.push_back(event);
                if (native_rate_last_ms - event.t_ms > 10000u)
                    continue;
                if (native_rate_first_ms == 0u)
                    native_rate_first_ms = event.t_ms;
                if (event.path == GL_PRES_NATIVE_MIDPOINT)
                    native_rate_midpoint_presents++;
                else
                    native_rate_current_presents++;
            }
            size_t window_begin = 0u;
            uint64_t window_midpoints = 0u;
            uint64_t window_currents = 0u;
            for (size_t window_end = 0u; window_end < native_events.size();
                 ++window_end) {
                if (native_events[window_end].path == GL_PRES_NATIVE_MIDPOINT)
                    window_midpoints++;
                else
                    window_currents++;
                while (window_begin < window_end &&
                       native_events[window_end].t_ms -
                               native_events[window_begin].t_ms > 10000u) {
                    if (native_events[window_begin].path ==
                        GL_PRES_NATIVE_MIDPOINT)
                        window_midpoints--;
                    else
                        window_currents--;
                    window_begin++;
                }
                const uint32_t window_ms = native_events[window_end].t_ms -
                    native_events[window_begin].t_ms;
                if (window_ms >= 9000u &&
                    window_midpoints > native_peak_midpoint_presents) {
                    native_peak_midpoint_presents = window_midpoints;
                    native_peak_current_presents = window_currents;
                    native_peak_window_ms = window_ms;
                }
            }
        }
    }
#ifdef PSX_INPUT_REPLAY_XG_AUTH_PROOF
    PsxXgRenderProducerFamilySnapshot runtime_producer_family{};
    PsxXgRenderSourceSnapshot runtime_source{};
    FieldCharacterShadowSummary runtime_source_collector{};
    PsxXgRenderFt4GeometrySnapshot runtime_geometry{};
    PsxXgRenderZoomTemplateContractSnapshot zoom_template_contract{};
    PsxXgRenderOverlayFt4Snapshot overlay_ft4_2c{};
    PsxXgRenderProjectedLifecycleSnapshot projected_lifecycle{};
    PsxXgRenderModelFt4ShadowSnapshot model_shadow{};
    PsxXgRenderModelFt3ShadowSnapshot model_ft3_shadow{};
    PsxXgRenderSpriteFt4ShadowSnapshot sprite_shadow{};
    PsxXgRenderFieldPolylineSnapshot field_polyline{};
    PsxXgRenderWorldTerrainWaterShadowSnapshot terrain_water_shadow{};
    PsxXgRenderWorldEntityShadowsShadowSnapshot entity_shadows_shadow{};
    PsxXgRenderWorldDecorationsShadowSnapshot decorations_shadow{};
    PsxXgRenderWorldCloudsShadowSnapshot clouds_shadow{};
    PsxXgRenderWorldHorizonShadowSnapshot horizon_shadow{};
    PsxXgRenderWorldEffectsShadowSnapshot effects_shadow{};
    PsxXgRenderWorldMinimapShadowSnapshot minimap_shadow{};
    PsxXgRenderWorldNativeSnapshot models_native{};
    PsxXgRenderWorldNativeSnapshot actor_sprites_native{};
    PsxXgRenderWorldNativeSnapshot sky_native{};
    PsxXgRenderWorldExecutionSnapshot world_execution{};
    PsxXgRenderUiOtSnapshot ui_ot{};
    producer_family_requested = replay.producer_family_requested;
    psx_xg_render_auth_producer_family_snapshot(&runtime_producer_family);
    psx_xg_render_auth_source_snapshot(&runtime_source);
    psx_xg_render_auth_source_collector_snapshot(&runtime_source_collector);
    psx_xg_render_auth_ft4_geometry_snapshot(&runtime_geometry);
    psx_xg_render_auth_zoom_template_contract_snapshot(
        &zoom_template_contract);
    psx_xg_render_auth_overlay_ft4_snapshot(&overlay_ft4_2c);
    psx_xg_render_auth_projected_lifecycle_snapshot(&projected_lifecycle);
    psx_xg_render_auth_model_ft4_shadow_snapshot(&model_shadow);
    psx_xg_render_auth_model_ft3_shadow_snapshot(&model_ft3_shadow);
    psx_xg_render_auth_sprite_ft4_shadow_snapshot(&sprite_shadow);
    psx_xg_render_auth_field_polyline_snapshot(&field_polyline);
    psx_xg_render_auth_world_terrain_water_shadow_snapshot(
        &terrain_water_shadow);
    psx_xg_render_auth_world_entity_shadows_shadow_snapshot(
        &entity_shadows_shadow);
    psx_xg_render_auth_world_decorations_shadow_snapshot(
        &decorations_shadow);
    psx_xg_render_auth_world_clouds_shadow_snapshot(&clouds_shadow);
    psx_xg_render_auth_world_horizon_shadow_snapshot(&horizon_shadow);
    psx_xg_render_auth_world_effects_shadow_snapshot(&effects_shadow);
    psx_xg_render_auth_world_minimap_shadow_snapshot(&minimap_shadow);
    psx_xg_render_auth_world_models_native_snapshot(&models_native);
    psx_xg_render_auth_world_actor_sprites_native_snapshot(
        &actor_sprites_native);
    psx_xg_render_auth_world_sky_native_snapshot(&sky_native);
    psx_xg_render_auth_world_execution_snapshot(&world_execution);
    psx_xg_render_auth_ui_ot_snapshot(&ui_ot);
    PsxXgRenderModeSnapshot render_mode{};
    psx_xg_render_auth_mode_snapshot(&render_mode);
    render_modes = render_mode.modes;
    render_presentation = render_mode.presentation;
    render_transaction_count = render_mode.transaction_count;
    render_substitution_count = render_mode.substitution_count;
    GuestRenderBridgeSnapshot render_bridge{};
    if (guest_render_bridge_snapshot(&render_bridge) == GUEST_RENDER_OK) {
        render_bridge_snapshot_valid = true;
        if (render_modes.requested_timing_mode !=
                render_bridge.modes.requested_timing_mode ||
            render_modes.requested_render_mode !=
                render_bridge.modes.requested_render_mode) {
            render_modes = render_bridge.modes;
        }
        render_scene_fallback = render_bridge.fallback_reason;
        render_last_fallback = render_bridge.last_fallback_reason;
        render_cumulative_fallback_count = render_bridge.fallback_count;
        render_scene_fallback_count_baseline =
            render_bridge.scene_fallback_count_baseline;
        render_scene_fallback_count_delta =
            render_bridge.scene_fallback_count_delta;
        render_fallback_count_overflowed =
            render_bridge.fallback_count_overflowed;
    }
    producer_family.geometry_count = runtime_producer_family.geometry_count;
    producer_family.candidate_count = runtime_producer_family.candidate_count;
    producer_family.match_count = runtime_producer_family.match_count;
    producer_family.mismatch_count = runtime_producer_family.mismatch_count;
    producer_family.last_ot_bucket = runtime_producer_family.last_ot_bucket;
    producer_family.last_runtime_result =
        runtime_producer_family.last_runtime_result;
    producer_family.last_compare_result =
        runtime_producer_family.last_compare_result;
    producer_family.first_mismatch_word =
        runtime_producer_family.first_mismatch_word;
    producer_family.first_mismatch_byte =
        runtime_producer_family.first_mismatch_byte;
    producer_family.blocker = runtime_producer_family.blocker;
    producer_family.enabled = runtime_producer_family.enabled;
    producer_family.blocked = runtime_producer_family.blocked;
    producer_family.source_event_count = runtime_source.count;
    producer_family.source_blocker = runtime_source.blocker;
    producer_family.source_context_bits = runtime_source.context_bits;
    producer_family.collector_phase = runtime_source_collector.phase;
    producer_family.collector_blocker = runtime_source_collector.blocker;
    producer_family.collector_access_count =
        static_cast<uint32_t>(runtime_source_collector.access_count);
    producer_family.collector_site_count =
        static_cast<uint32_t>(runtime_source_collector.site_count);
    producer_family.source_blocked = runtime_source.blocked;
    producer_family.source_overflowed = runtime_source.overflowed;
    producer_family.geometry_completed_count = runtime_geometry.completed_count;
    producer_family.geometry_queued_count = runtime_geometry.queued_count;
    producer_family.geometry_pending = runtime_geometry.pending;
    producer_family.geometry_blocked = runtime_geometry.blocked;
    producer_family.geometry_overflowed = runtime_geometry.overflowed;
#endif
    const bool producer_family_complete = producer_family.enabled &&
        !producer_family.blocked &&
        producer_family.geometry_count == producer_family.candidate_count &&
        producer_family.candidate_count >= kProducerFamilyMinimumObservations &&
        producer_family.mismatch_count == 0u;
    const bool producer_family_pass = !producer_family_requested ||
        (producer_family_complete &&
         (render_modes.requested_render_mode == GUEST_RENDER_RENDER_NATIVE
              ? producer_family.match_count == 0u
              : producer_family.match_count == producer_family.candidate_count));
    const bool native_packet_stream_complete =
        native_stream.total_native_lists > 0u &&
        native_stream.total_native_packets > 0u &&
        native_stream.total_native_packets ==
            native_stream.total_native_bound_packets +
            native_stream.total_native_state_packets +
            native_stream.total_native_unbound_packets &&
        native_stream.total_native_unsupported_packets == 0u &&
        native_stream.total_consumed <= native_stream.total_staged &&
        native_stream.total_superseded <=
            native_stream.total_staged - native_stream.total_consumed &&
        native_stream.staged_count ==
            native_stream.total_staged - native_stream.total_consumed -
                native_stream.total_superseded &&
        native_stream.total_not_found == 0u &&
        native_stream.total_parser_replay_commands == 0u &&
        native_stream.total_ui_ot_adapter_calls == 0u &&
        native_stream.total_guest_gp0_commands == 0u &&
        native_stream.total_shared_vram_presents == 0u &&
        native_stream.total_shared_fmv_frames == 0u &&
        (native_stream.total_independent_vram_presents > 0u ||
         (native_stream.total_independent_fmv_frames > 0u &&
          native_stream.total_independent_fmv_pixels > 0u &&
          native_stream.last_independent_fmv_width > 0u &&
          native_stream.last_independent_fmv_height > 0u));
    const bool native_stream_complete =
        native_stream.enabled &&
        native_stream.total_original_draws == 0u &&
        native_stream.stage_failure_count == 0u &&
        native_packet_stream_complete;
    const bool cleanly_closed_non_original_mode =
        render_bridge_snapshot_valid &&
        render_modes.effective_render_mode == GUEST_RENDER_RENDER_ORIGINAL &&
        render_scene_fallback == GUEST_RENDER_FALLBACK_NONE &&
        render_scene_fallback_count_delta == 0u &&
        (render_modes.requested_render_mode == GUEST_RENDER_RENDER_SHADOW ||
         (render_modes.requested_render_mode == GUEST_RENDER_RENDER_NATIVE &&
          native_stream_complete));
    /* Scene teardown resets the operational bridge to Original. The receipt
     * describes the completed scene, but only promotes that reset state after
     * the fail-closed Native telemetry proves that no Original draw occurred. */
    if (cleanly_closed_non_original_mode)
        render_modes.effective_render_mode = render_modes.requested_render_mode;
    const bool render_mode_identity_pass =
        render_modes.requested_render_mode ==
            render_modes.effective_render_mode;
    const bool render_mode_pass = !render_fallback_count_overflowed &&
        render_scene_fallback_count_delta == 0u &&
        render_scene_fallback == GUEST_RENDER_FALLBACK_NONE &&
        render_modes.requested_timing_mode ==
            render_modes.effective_timing_mode &&
        render_mode_identity_pass &&
        (render_modes.requested_render_mode ==
             GUEST_RENDER_RENDER_NATIVE
              ? native_stream_complete
             : (!native_stream.enabled &&
                native_stream.total_consumed == 0u &&
                render_substitution_count == 0u)) &&
        (render_modes.requested_render_mode ==
                 GUEST_RENDER_RENDER_ORIGINAL ||
         (render_presentation.quiesced &&
           !render_presentation.interpolation_effective &&
           !render_presentation.smooth_effective &&
           render_presentation.history_count == 0u));
    const bool completion_pass = replay.checkpoint_configured
        ? stop_reason() == StopReason::CheckpointReached
        : stop_reason() == StopReason::TraceComplete;
    const bool pass = completion_pass &&
                        std::string(backend) == "opengl" &&
                        (!replay.baseline_request || baseline.complete) &&
                        producer_family_pass && render_mode_pass;
    const auto digest = [](uint64_t value) {
        std::ostringstream encoded;
        encoded << std::hex << std::nouppercase << std::setfill('0')
                << std::setw(16) << value;
        return encoded.str();
    };
    const auto write_opcode_histogram = [&output](
            const char *name, const uint64_t *counts) {
        output << ",\"" << name << "\":[";
        for (size_t opcode = 0u;
             opcode < GUEST_RENDER_NATIVE_STREAM_OPCODE_COUNT; ++opcode) {
            if (opcode != 0u) output << ',';
            output << counts[opcode];
        }
        output << ']';
    };
    const auto write_opcode_attribution = [&output](
            const char *name, const uint32_t *values) {
        output << ",\"" << name << "\":[";
        for (size_t opcode = 0u;
             opcode < GUEST_RENDER_NATIVE_STREAM_OPCODE_COUNT; ++opcode) {
            if (opcode != 0u) output << ',';
            output << values[opcode];
        }
        output << ']';
    };
    output << "{\"status\":\"" << (pass ? "PASS" : "FAIL") << "\",\"timing_mode\":\""
           << timing_mode_name(render_modes.requested_timing_mode)
           << "\",\"render_mode\":\""
           << render_mode_name(render_modes.requested_render_mode);
    if (replay.checkpoint_configured)
        output << "\",\"checkpoint\":{\"field_id\":"
               << reported_checkpoint_field << "}";
    else
        output << "\",\"completion\":\"trace_complete\",\"checkpoint\":null";
    output << ",\"backend\":\"" << backend
           << "\",\"native_render\":{\"requested_timing_mode\":\""
           << timing_mode_name(render_modes.requested_timing_mode)
           << "\",\"effective_timing_mode\":\""
           << timing_mode_name(render_modes.effective_timing_mode)
           << "\",\"requested_render_mode\":\""
           << render_mode_name(render_modes.requested_render_mode)
           << "\",\"effective_render_mode\":\""
           << render_mode_name(render_modes.effective_render_mode)
           << "\",\"transaction_count\":" << render_transaction_count
           << ",\"substitution_count\":" << render_substitution_count
           << ",\"stream\":{\"enabled\":"
           << (native_stream.enabled ? "true" : "false")
           << ",\"staged_count\":" << native_stream.staged_count
            << ",\"total_staged\":" << native_stream.total_staged
            << ",\"total_consumed\":" << native_stream.total_consumed
            << ",\"total_not_found\":" << native_stream.total_not_found
            << ",\"total_original_draws\":"
            << native_stream.total_original_draws
            << ",\"first_original_draw_opcode\":"
            << static_cast<unsigned>(native_stream.first_original_draw_opcode)
            << ",\"last_original_draw_opcode\":"
            << static_cast<unsigned>(native_stream.last_original_draw_opcode)
             << ",\"total_parser_replay_commands\":"
             << native_stream.total_parser_replay_commands
             << ",\"total_parser_replay_draws\":"
             << native_stream.total_parser_replay_draws
             << ",\"total_native_line_segments\":"
             << native_stream.total_native_line_segments
             << ",\"total_shared_fmv_frames\":"
             << native_stream.total_shared_fmv_frames
             << ",\"total_shared_fmv_pixels\":"
             << native_stream.total_shared_fmv_pixels
             << ",\"last_shared_fmv_width\":"
             << native_stream.last_shared_fmv_width
             << ",\"last_shared_fmv_height\":"
             << native_stream.last_shared_fmv_height
             << ",\"last_shared_fmv_depth24\":"
             << (native_stream.last_shared_fmv_depth24 ? "true" : "false")
             << ",\"total_independent_fmv_frames\":"
             << native_stream.total_independent_fmv_frames
             << ",\"total_independent_fmv_pixels\":"
             << native_stream.total_independent_fmv_pixels
             << ",\"last_independent_fmv_width\":"
             << native_stream.last_independent_fmv_width
             << ",\"last_independent_fmv_height\":"
             << native_stream.last_independent_fmv_height
             << ",\"last_independent_fmv_depth24\":"
             << (native_stream.last_independent_fmv_depth24 ? "true" : "false")
             << ",\"total_ui_ot_adapter_calls\":"
             << native_stream.total_ui_ot_adapter_calls
             << ",\"total_guest_gp0_commands\":"
             << native_stream.total_guest_gp0_commands
              << ",\"total_shared_vram_presents\":"
              << native_stream.total_shared_vram_presents
              << ",\"total_native_lists\":"
              << native_stream.total_native_lists
              << ",\"total_native_packets\":"
              << native_stream.total_native_packets
              << ",\"total_native_bound_packets\":"
              << native_stream.total_native_bound_packets
              << ",\"total_native_state_packets\":"
              << native_stream.total_native_state_packets
              << ",\"total_native_unbound_packets\":"
              << native_stream.total_native_unbound_packets
              << ",\"total_native_producer_bound_draws\":"
              << native_stream.total_native_producer_bound_draws
              << ",\"total_native_packet_derived_draws\":"
              << native_stream.total_native_packet_derived_draws
              << ",\"total_native_unsupported_packets\":"
              << native_stream.total_native_unsupported_packets
               << ",\"first_native_unsupported_opcode\":"
               << static_cast<unsigned>(native_stream.first_native_unsupported_opcode)
               << ",\"last_native_unsupported_opcode\":"
               << static_cast<unsigned>(native_stream.last_native_unsupported_opcode)
               << ",\"first_native_unbound_opcode\":"
               << static_cast<unsigned>(native_stream.first_native_unbound_opcode)
               << ",\"last_native_unbound_opcode\":"
               << static_cast<unsigned>(native_stream.last_native_unbound_opcode)
               << ",\"first_native_unbound_source\":"
               << native_stream.first_native_unbound_source
               << ",\"first_native_unsupported_source\":"
               << native_stream.first_native_unsupported_source
               << ",\"first_native_unbound_pc\":"
               << native_stream.first_native_unbound_pc
               << ",\"first_native_unbound_function\":"
               << native_stream.first_native_unbound_function
               << ",\"first_native_unsupported_pc\":"
               << native_stream.first_native_unsupported_pc
               << ",\"first_native_unsupported_function\":"
               << native_stream.first_native_unsupported_function
               << ",\"first_native_unbound_return_address\":"
               << native_stream.first_native_unbound_return_address
               << ",\"first_native_unsupported_return_address\":"
               << native_stream.first_native_unsupported_return_address
               << ",\"total_independent_vram_presents\":"
                << native_stream.total_independent_vram_presents
                << ",\"native_claim\":\""
               << (native_stream_complete
                       ? (native_stream.total_native_packet_derived_draws != 0u
                               ? "packet-faithful" : "independent")
                        : "hybrid")
                << "\""
                << ",\"native_coverage_contract\":"
                   "\"eligible-3d-producer\"";
    write_opcode_histogram("native_opcode_counts",
                           native_stream.native_opcode_counts);
    write_opcode_histogram("native_state_opcode_counts",
                           native_stream.native_state_opcode_counts);
    write_opcode_histogram("native_unbound_opcode_counts",
                           native_stream.native_unbound_opcode_counts);
    write_opcode_histogram(
        "native_producer_bound_opcode_counts",
        native_stream.native_producer_bound_opcode_counts);
    write_opcode_histogram(
        "native_packet_derived_opcode_counts",
        native_stream.native_packet_derived_opcode_counts);
    write_opcode_histogram("native_unsupported_opcode_counts",
                           native_stream.native_unsupported_opcode_counts);
    write_opcode_attribution("native_unbound_source_by_opcode",
                             native_stream.native_unbound_source_by_opcode);
    write_opcode_attribution("native_unbound_pc_by_opcode",
                             native_stream.native_unbound_pc_by_opcode);
    write_opcode_attribution("native_unsupported_pc_by_opcode",
                             native_stream.native_unsupported_pc_by_opcode);
    write_opcode_attribution("native_unbound_return_address_by_opcode",
                             native_stream.native_unbound_return_address_by_opcode);
    write_opcode_attribution("native_unsupported_return_address_by_opcode",
                               native_stream.native_unsupported_return_address_by_opcode);
    output << ",\"last_native_state\":{\"sequence\":"
           << native_stream.last_native_state.sequence
           << ",\"command_word\":"
           << native_stream.last_native_state.command_word
           << ",\"source_word_address\":"
           << native_stream.last_native_state.source_word_address
           << ",\"draw_mode\":"
           << native_stream.last_native_state.draw_mode
           << ",\"draw_area_left\":"
           << native_stream.last_native_state.draw_area_left
           << ",\"draw_area_top\":"
           << native_stream.last_native_state.draw_area_top
           << ",\"draw_area_right\":"
           << native_stream.last_native_state.draw_area_right
           << ",\"draw_area_bottom\":"
           << native_stream.last_native_state.draw_area_bottom
           << ",\"draw_offset_x\":"
           << native_stream.last_native_state.draw_offset_x
           << ",\"draw_offset_y\":"
           << native_stream.last_native_state.draw_offset_y
           << ",\"texture_window_mask_x\":"
           << static_cast<unsigned>(native_stream.last_native_state.texture_window_mask_x)
           << ",\"texture_window_mask_y\":"
           << static_cast<unsigned>(native_stream.last_native_state.texture_window_mask_y)
           << ",\"texture_window_offset_x\":"
           << static_cast<unsigned>(native_stream.last_native_state.texture_window_offset_x)
           << ",\"texture_window_offset_y\":"
           << static_cast<unsigned>(native_stream.last_native_state.texture_window_offset_y)
           << ",\"dither\":"
           << static_cast<unsigned>(native_stream.last_native_state.dither)
           << ",\"draw_to_display\":"
           << static_cast<unsigned>(native_stream.last_native_state.draw_to_display)
           << ",\"texture_disable\":"
           << static_cast<unsigned>(native_stream.last_native_state.texture_disable)
           << ",\"mask_set\":"
           << static_cast<unsigned>(native_stream.last_native_state.mask_set)
           << ",\"mask_check\":"
           << static_cast<unsigned>(native_stream.last_native_state.mask_check)
           << '}';
    output << ",\"native_unbound_source_hotspots\":[";
    {
        bool first_hotspot = true;
        for (size_t index = 0u;
             index < GUEST_RENDER_NATIVE_STREAM_HOTSPOT_CAPACITY; ++index) {
            const GuestRenderNativeSourceHotspot& hotspot =
                native_stream.native_unbound_source_hotspots[index];
            if (hotspot.count == 0u) continue;
            output << (first_hotspot ? "" : ",")
                   << "{\"opcode\":" << static_cast<unsigned>(hotspot.opcode)
                   << ",\"source_region_start\":" << hotspot.source_region_start
                    << ",\"source_region_size\":"
                    << GUEST_RENDER_NATIVE_STREAM_HOTSPOT_REGION_SIZE
                    << ",\"representative_source\":"
                    << hotspot.representative_source_address
                    << ",\"packet_pc\":"
                    << hotspot.representative_packet_pc
                    << ",\"packet_function\":"
                    << hotspot.representative_packet_function
                    << ",\"packet_return_address\":"
                    << hotspot.representative_packet_return_address
                    << ",\"first_frame\":"
                    << hotspot.first_frame
                    << ",\"last_frame\":"
                    << hotspot.last_frame
                    << ",\"writer_pc\":"
                   << hotspot.representative_writer_pc
                   << ",\"writer_function\":"
                   << hotspot.representative_writer_function
                   << ",\"writer_return_address\":"
                   << hotspot.representative_writer_return_address
                   << ",\"next_word_writer_pc\":"
                   << hotspot.representative_next_word_writer_pc
                   << ",\"next_word_writer_function\":"
                   << hotspot.representative_next_word_writer_function
                   << ",\"next_word_writer_return_address\":"
                   << hotspot.representative_next_word_writer_return_address
                   << ",\"payload_writers\":[";
            for (size_t writer_index = 0u;
                 writer_index < GUEST_RENDER_NATIVE_STREAM_PAYLOAD_WRITER_COUNT;
                 ++writer_index) {
                const GuestRenderNativeSourceWriter& writer =
                    hotspot.representative_payload_writers[writer_index];
                output << (writer_index ? "," : "")
                       << "{\"pc\":" << writer.pc
                       << ",\"function\":" << writer.function
                       << ",\"return_address\":" << writer.return_address
                       << "}";
            }
            output << ']'
                   << ",\"count\":" << hotspot.count
                   << ",\"error\":" << hotspot.error << "}";
            first_hotspot = false;
        }
    }
    output << ']';
    output
             << ",\"total_visual_states\":"
            << native_stream.total_visual_states
           << ",\"total_superseded\":"
           << native_stream.total_superseded
           << ",\"stage_failure_count\":"
           << native_stream.stage_failure_count
           << ",\"first_stage_failure_command_id\":"
           << native_stream.first_stage_failure_command_id
           << ",\"first_stage_failure_visual_id\":{\"scene_epoch\":"
           << native_stream.first_stage_failure_visual_id.scene_epoch
           << ",\"state_sequence\":"
           << native_stream.first_stage_failure_visual_id.state_sequence << "}"
           << ",\"first_stage_failure_status\":"
           << static_cast<unsigned>(native_stream.first_stage_failure_status)
           << ",\"last_command_id\":" << native_stream.last_command_id
           << ",\"last_status\":"
           << static_cast<unsigned>(native_stream.last_status)
           << ",\"last_stage_status\":"
            << static_cast<unsigned>(native_stream.last_stage_status)
             << ",\"last_consume_status\":"
             << static_cast<unsigned>(native_stream.last_consume_status) << "}"
#ifdef PSX_INPUT_REPLAY_XG_AUTH_PROOF
             << ",\"ui_ot\":{\"prepare_count\":" << ui_ot.prepare_count
            << ",\"completed_count\":" << ui_ot.completed_count
            << ",\"node_count\":" << ui_ot.node_count
            << ",\"candidate_count\":" << ui_ot.candidate_count
            << ",\"prebound_count\":" << ui_ot.prebound_count
            << ",\"staged_count\":" << ui_ot.staged_count
            << ",\"blocked_count\":" << ui_ot.blocked_count
            << ",\"last_start_address\":" << ui_ot.last_start_address
            << ",\"last_node_count\":" << ui_ot.last_node_count
            << ",\"last_candidate_count\":" << ui_ot.last_candidate_count
            << ",\"last_prebound_count\":" << ui_ot.last_prebound_count
            << ",\"last_staged_count\":" << ui_ot.last_staged_count
            << ",\"last_ot_digest\":" << ui_ot.last_ot_digest
            << ",\"last_packet_digest\":" << ui_ot.last_packet_digest
            << ",\"last_semantic_digest\":" << ui_ot.last_semantic_digest
            << ",\"last_environment_digest\":"
            << ui_ot.last_environment_digest
             << ",\"last_vram_serial\":" << ui_ot.last_vram_serial
             << ",\"pending\":" << (ui_ot.pending ? "true" : "false")
             << ",\"blocked\":" << (ui_ot.blocked ? "true" : "false") << "}"
#endif
             << ",\"cumulative_fallback_count\":"
           << render_cumulative_fallback_count
           << ",\"scene_fallback_count_baseline\":"
           << render_scene_fallback_count_baseline
           << ",\"scene_fallback_count_delta\":"
           << render_scene_fallback_count_delta
           << ",\"scene_fallback_reason\":\""
           << render_fallback_name(render_scene_fallback)
           << "\",\"last_fallback_reason\":\""
           << render_fallback_name(render_last_fallback)
           << "\",\"fallback_count_overflowed\":"
           << (render_fallback_count_overflowed ? "true" : "false")
           << ",\"presentation_history\":{\"interpolation_requested\":"
           << (render_presentation.interpolation_requested ? "true" : "false")
           << ",\"interpolation_effective\":"
           << (render_presentation.interpolation_effective ? "true" : "false")
           << ",\"smooth_requested\":"
           << (render_presentation.smooth_requested ? "true" : "false")
           << ",\"smooth_effective\":"
           << (render_presentation.smooth_effective ? "true" : "false")
           << ",\"history_count\":" << render_presentation.history_count
           << ",\"quiesced\":"
           << (render_presentation.quiesced ? "true" : "false")
           << ",\"gate_reason\":\""
           << presentation_gate_reason_name(render_presentation.reason)
           << "\"}},\"baseline\":{\"requested\":" << (replay.baseline_request ? "true" : "false")
           << ",\"schema_version\":" << baseline.schema_version
           << ",\"enabled\":" << (baseline.enabled ? "true" : "false")
           << ",\"complete\":" << (baseline.complete ? "true" : "false")
           << ",\"overflow\":" << (baseline.overflow ? "true" : "false")
           << ",\"invalid_ot\":" << (baseline.invalid_ot ? "true" : "false")
           << ",\"cyclic_ot\":" << (baseline.cyclic_ot ? "true" : "false")
           << ",\"reason\":" << static_cast<unsigned>(baseline.incomplete_reason)
           << ",\"field_completeness_mask\":" << baseline.field_completeness_mask
           << ",\"required_field_mask\":" << baseline.required_field_mask
           << ",\"visual_scene_epoch\":" << baseline.visual_state_id.scene_epoch
           << ",\"visual_state_sequence\":" << baseline.visual_state_id.state_sequence
           << ",\"requested_render_mode\":" << static_cast<unsigned>(baseline.requested_render_mode)
           << ",\"effective_render_mode\":" << static_cast<unsigned>(baseline.effective_render_mode)
           << ",\"fallback_reason\":" << static_cast<unsigned>(baseline.fallback_reason)
           << ",\"fallback_count\":" << baseline.fallback_count
           << ",\"producer_count\":" << baseline.producer_count
           << ",\"producer_binding_count\":" << baseline.producer_binding_count
           << ",\"interpreter_calls\":" << baseline.interpreter_calls
           << ",\"native_calls\":" << baseline.native_calls
           << ",\"gte_total_count\":" << baseline.gte_total_count
           << ",\"gte_inside_producer_count\":" << baseline.gte_inside_producer_count
           << ",\"gte_outside_producer_count\":" << baseline.gte_outside_producer_count
           << ",\"gte_tier_counts\":[" << baseline.gte_tier_counts[0]
           << "," << baseline.gte_tier_counts[1]
           << "," << baseline.gte_tier_counts[2]
           << "," << baseline.gte_tier_counts[3] << "]"
           << ",\"gte_overflow_reason\":" << static_cast<unsigned>(baseline.gte_overflow_reason)
           << ",\"gte_blocked\":" << (baseline.gte_blocked ? "true" : "false")
           << ",\"ot_lists\":" << baseline.ot_lists
           << ",\"ot_nodes\":" << baseline.ot_nodes
           << ",\"ot_words\":" << baseline.ot_words
           << ",\"ot_digest\":\"" << digest(baseline.ot_digest) << "\""
           << ",\"topology_digest\":\"" << digest(baseline.topology_digest) << "\""
           << ",\"material_samples\":" << baseline.material_samples
           << ",\"material_digest\":\"" << digest(baseline.material_digest) << "\""
           << ",\"gp0_writes\":" << baseline.gp0_writes
           << ",\"gp1_writes\":" << baseline.gp1_writes
           << ",\"vram_mutations\":" << baseline.vram_mutations
           << ",\"global_vram_mutation_serial\":" << baseline.global_vram_mutation_serial
           << ",\"global_vram_serial_overflowed\":"
           << (baseline.global_vram_serial_overflowed ? "true" : "false")
           << ",\"vram_digest\":\"" << digest(baseline.vram_digest) << "\""
           << ",\"gpu_digest\":\"" << digest(baseline.gpu_digest) << "\""
           << ",\"display_samples\":" << baseline.display_samples
           << ",\"display15_digest\":\"" << digest(baseline.display15_digest) << "\""
           << ",\"display_digest\":\"" << digest(baseline.display_digest) << "\""
           << ",\"host_framebuffer_samples\":" << baseline.host_framebuffer_samples
           << ",\"host_framebuffer_digest\":\"" << digest(baseline.host_framebuffer_digest) << "\""
           << ",\"vblank_delta\":" << baseline.vblank_delta
           << ",\"guest_cycle_delta\":" << baseline.guest_cycle_delta
           << ",\"cycles_per_vblank\":" << baseline.cycles_per_vblank
           << ",\"cycle_digest\":\"" << digest(baseline.cycle_digest) << "\""
           << ",\"audio_frames\":" << baseline.audio_frames
           << ",\"audio_events\":" << baseline.audio_events
           << ",\"audio_digest\":\"" << digest(baseline.audio_digest) << "\""
           << ",\"game_digest\":\"" << digest(baseline.game_digest) << "\""
           << ",\"camera_actor_digest\":\"" << digest(baseline.camera_actor_digest) << "\""
           << ",\"normalized_digest\":\"" << digest(baseline.normalized_digest)
             << "\"},\"native_midpoint\":{\"target_fps\":"
             << native_midpoint.target_fps
             << ",\"phase_count\":" << native_midpoint.phase_count
             << ",\"midpoint_presents\":" << native_midpoint.midpoint_presents
             << ",\"current_presents\":" << native_midpoint.current_presents
             << ",\"eligibility_no_previous\":"
             << native_midpoint.eligibility_no_previous_frames
             << ",\"rate_window_ms\":"
             << (native_rate_first_ms != 0u
                    ? native_rate_last_ms - native_rate_first_ms : 0u)
             << ",\"rate_midpoint_presents\":"
             << native_rate_midpoint_presents
             << ",\"rate_current_presents\":"
             << native_rate_current_presents
             << ",\"peak_window_ms\":" << native_peak_window_ms
             << ",\"peak_midpoint_presents\":"
             << native_peak_midpoint_presents
              << ",\"peak_current_presents\":"
              << native_peak_current_presents
              << ",\"retired_candidates\":"
              << native_midpoint.retired_candidate_count
              << ",\"retired_inserted\":"
              << native_midpoint.retired_inserted_count
              << ",\"retired_history_misses\":"
              << native_midpoint.retired_history_miss_count
              << ",\"retired_capacity_misses\":"
              << native_midpoint.retired_capacity_miss_count
              << ",\"retired_phase_failures\":"
              << native_midpoint.retired_phase_failure_count
              << ",\"retired_producer_history_recoveries\":"
              << native_midpoint.retired_producer_history_recovery_count
              << ",\"retired_world_model_candidates\":"
              << native_midpoint.retired_world_model_candidate_count
              << ",\"retired_world_model_inserted\":"
              << native_midpoint.retired_world_model_inserted_count
              << ",\"retired_world_model_history_misses\":"
              << native_midpoint.retired_world_model_history_miss_count
              << ",\"retired_world_model_history_recoveries\":"
              << native_midpoint.retired_world_model_history_recovery_count
              << ",\"retired_world_model_producer_context_recoveries\":"
              << native_midpoint
                     .retired_world_model_producer_context_recovery_count
              << ",\"retired_world_model_class_context_recoveries\":"
              << native_midpoint
                     .retired_world_model_class_context_recovery_count
              << ",\"retired_terrain_unmatched\":"
              << native_midpoint.retired_terrain_unmatched_count
              << ",\"retired_terrain_eligible\":"
              << native_midpoint.retired_terrain_eligible_count
              << ",\"retired_terrain_missing_current_geometry\":"
              << native_midpoint
                     .retired_terrain_missing_current_geometry_count
              << ",\"retired_terrain_missing_anchors\":"
              << native_midpoint.retired_terrain_missing_anchor_count
              << ",\"retired_terrain_scene_mismatches\":"
              << native_midpoint.retired_terrain_scene_mismatch_count
              << ",\"retired_terrain_position_mode_mismatches\":"
              << native_midpoint
                     .retired_terrain_position_mode_mismatch_count
              << ",\"retired_terrain_material_position_mismatches\":"
              << native_midpoint
                     .retired_terrain_material_position_mismatch_count
              << ",\"retired_terrain_anchor_overflows\":"
              << native_midpoint.retired_terrain_anchor_overflow_count
              << ",\"retired_terrain_candidates\":"
              << native_midpoint.retired_terrain_candidate_count
              << ",\"retired_terrain_inserted\":"
              << native_midpoint.retired_terrain_inserted_count
              << ",\"retired_terrain_history_misses\":"
              << native_midpoint.retired_terrain_history_miss_count
              << ",\"retired_terrain_history_recoveries\":"
              << native_midpoint.retired_terrain_history_recovery_count
              << ",\"first_retired_terrain_missing_primitive\":"
              << native_midpoint.first_retired_terrain_missing_primitive
              << ",\"first_retired_terrain_missing_group\":"
              << native_midpoint.first_retired_terrain_missing_group
              << ",\"first_retired_terrain_missing_vertex\":"
              << native_midpoint.first_retired_terrain_missing_vertex
              << ",\"last_retired_phase_failure_producer\":"
              << native_midpoint.last_retired_phase_failure_producer
              << ",\"last_retired_phase_failure_primitive\":"
              << native_midpoint.last_retired_phase_failure_primitive
              << ",\"last_retired_history_miss_producer\":"
              << native_midpoint.last_retired_history_miss_producer
              << ",\"last_retired_history_miss_primitive\":"
              << native_midpoint.last_retired_history_miss_primitive
              << ",\"workload_total_matched\":"
             << native_midpoint.workload_total_matched
             << ",\"workload_total_snapped\":"
             << native_midpoint.workload_total_snapped
              << ",\"workload_total_ambiguous\":"
              << native_midpoint.workload_total_ambiguous
              << ",\"workload_total_moved\":"
              << native_midpoint.workload_total_moved
              << ",\"workload_total_unkeyed\":"
              << native_midpoint.workload_total_unkeyed
              << ",\"workload_total_source_geometry_matches\":"
              << native_midpoint.workload_total_source_geometry_matches
              << ",\"workload_total_matched_vertices\":"
              << native_midpoint.workload_total_matched_vertices
              << ",\"workload_total_position_changed_vertices\":"
              << native_midpoint.workload_total_position_changed_vertices
              << ",\"workload_total_position_delta_fixed\":"
              << native_midpoint.workload_total_position_delta_fixed
              << ",\"workload_max_semantic_position_delta_fixed\":"
              << native_midpoint.workload_max_semantic_position_delta_fixed
              << ",\"workload_max_semantic_identity_scene\":"
              << native_midpoint.workload_max_semantic_identity_scene
              << ",\"workload_max_semantic_identity_producer\":"
              << native_midpoint.workload_max_semantic_identity_producer
              << ",\"workload_max_semantic_identity_primitive\":"
              << native_midpoint.workload_max_semantic_identity_primitive
              << ",\"workload_max_semantic_identity_valid\":"
              << (native_midpoint.workload_max_semantic_identity_valid
                      ? "true" : "false")
              << ",\"workload_total_unkeyed_moved_matches\":"
              << native_midpoint.workload_total_unkeyed_moved_matches
              << ",\"workload_total_unkeyed_motion_over_32px\":"
              << native_midpoint.workload_total_unkeyed_motion_over_32px
              << ",\"workload_total_unkeyed_motion_over_64px\":"
              << native_midpoint.workload_total_unkeyed_motion_over_64px
              << ",\"workload_total_unkeyed_motion_over_128px\":"
              << native_midpoint.workload_total_unkeyed_motion_over_128px
              << ",\"workload_total_unkeyed_motion_over_192px\":"
              << native_midpoint.workload_total_unkeyed_motion_over_192px
              << ",\"workload_total_unkeyed_motion_over_240px\":"
              << native_midpoint.workload_total_unkeyed_motion_over_240px
              << ",\"workload_max_keyed_semantic_position_delta_fixed\":"
              << native_midpoint.workload_max_keyed_semantic_position_delta_fixed
              << ",\"workload_max_keyed_semantic_identity_scene\":"
              << native_midpoint.workload_max_keyed_semantic_identity_scene
              << ",\"workload_max_keyed_semantic_identity_producer\":"
              << native_midpoint.workload_max_keyed_semantic_identity_producer
              << ",\"workload_max_keyed_semantic_identity_primitive\":"
              << native_midpoint.workload_max_keyed_semantic_identity_primitive
              << ",\"workload_total_keyed_moved_matches\":"
              << native_midpoint.workload_total_keyed_moved_matches
              << ",\"workload_total_keyed_motion_over_32px\":"
              << native_midpoint.workload_total_keyed_motion_over_32px
              << ",\"workload_total_keyed_motion_over_64px\":"
              << native_midpoint.workload_total_keyed_motion_over_64px
              << ",\"workload_total_keyed_motion_over_128px\":"
              << native_midpoint.workload_total_keyed_motion_over_128px
              << ",\"workload_total_keyed_motion_over_192px\":"
              << native_midpoint.workload_total_keyed_motion_over_192px
              << ",\"workload_total_keyed_motion_over_240px\":"
              << native_midpoint.workload_total_keyed_motion_over_240px
              << ",\"presented_midpoint_position_changed_vertices\":"
              << native_midpoint.presented_midpoint_position_changed_vertices
              << ",\"presented_midpoint_position_delta_fixed\":"
              << native_midpoint.presented_midpoint_position_delta_fixed
              << ",\"workload_total_midpoint_collapsed_vertices\":"
             << native_midpoint.workload_total_midpoint_collapsed_vertices
              << ",\"workload_total_midpoint_formula_failures\":"
             << native_midpoint.workload_total_midpoint_formula_failures
              << ",\"workload_total_projective_phase_vertices\":"
             << native_midpoint.workload_total_projective_phase_vertices
              << ",\"temporal_candidates\":"
              << native_midpoint.temporal_candidate_count
              << ",\"temporal_candidates_recorded\":"
              << native_midpoint.temporal_candidate_recorded_count
              << ",\"temporal_candidates_visible\":"
              << native_midpoint.temporal_candidate_visible_count
              << ",\"temporal_candidate_record_failures\":"
              << native_midpoint.temporal_candidate_record_failure_count
              << ",\"temporal_candidate_duplicates\":"
              << native_midpoint.temporal_candidate_duplicate_count
              << ",\"temporal_candidate_identity_collisions\":"
              << native_midpoint.temporal_candidate_identity_collision_count
              << ",\"temporal_candidate_peak_workload\":"
              << native_midpoint.temporal_candidate_peak_workload_count
              << ",\"temporal_candidate_first_failure\":{\"status\":"
              << native_midpoint.temporal_candidate_first_failure_status
              << ",\"workload_count\":"
              << native_midpoint.temporal_candidate_first_failure_workload_count
              << ",\"producer\":"
              << native_midpoint.temporal_candidate_first_failure_producer
              << ",\"primitive\":"
              << native_midpoint.temporal_candidate_first_failure_primitive
              << "}"
              << ",\"cancelled_frames\":"
             << native_midpoint.cancelled_frames
              << ",\"cancel_reasons\":{\"generic\":"
              << native_midpoint.cancel_reason_counts[
                     GL_NATIVE_MIDPOINT_CANCEL_GENERIC]
              << ",\"workload_record\":"
              << native_midpoint.cancel_reason_counts[
                     GL_NATIVE_MIDPOINT_CANCEL_WORKLOAD_RECORD]
              << "}"
              << ",\"last_cancel_status\":"
              << native_midpoint.last_cancel_status
              << ",\"last_cancel_workload_current\":"
              << native_midpoint.last_cancel_workload_current
             << ",\"gl_error_count\":" << native_midpoint.gl_error_count
             << ",\"reset_total\":" << native_midpoint.reset_count
             << ",\"reset_with_previous\":"
             << native_midpoint.reset_with_previous_count
             << ",\"reset_with_pending\":"
             << native_midpoint.reset_with_pending_count
             << ",\"last_reset_reason\":" << native_midpoint.last_reset_reason
             << ",\"reset_reasons\":{\"suspension_change\":"
             << native_midpoint.reset_reason_counts[
                    GL_NATIVE_MIDPOINT_RESET_SUSPENSION_CHANGE]
             << ",\"pending_canonical_mismatch\":"
             << native_midpoint.reset_reason_counts[
                    GL_NATIVE_MIDPOINT_RESET_PENDING_CANONICAL_MISMATCH]
             << ",\"pending_view_mismatch\":"
             << native_midpoint.reset_reason_counts[
                    GL_NATIVE_MIDPOINT_RESET_PENDING_VIEW_MISMATCH]
             << ",\"frontend_transaction\":"
             << native_midpoint.reset_reason_counts[
                    GL_NATIVE_MIDPOINT_RESET_FRONTEND_TRANSACTION]
             << ",\"frontend_non_native_wide\":"
             << native_midpoint.reset_reason_counts[
                    GL_NATIVE_MIDPOINT_RESET_FRONTEND_NON_NATIVE_WIDE]
             << ",\"frontend_non_native_stream\":"
             << native_midpoint.reset_reason_counts[
                    GL_NATIVE_MIDPOINT_RESET_FRONTEND_NON_NATIVE_STREAM]
             << ",\"frontend_cpu_present\":"
             << native_midpoint.reset_reason_counts[
                    GL_NATIVE_MIDPOINT_RESET_FRONTEND_CPU_PRESENT]
             << "},\"reset_with_previous_reasons\":{\"suspension_change\":"
             << native_midpoint.reset_with_previous_reason_counts[
                    GL_NATIVE_MIDPOINT_RESET_SUSPENSION_CHANGE]
             << ",\"pending_canonical_mismatch\":"
             << native_midpoint.reset_with_previous_reason_counts[
                    GL_NATIVE_MIDPOINT_RESET_PENDING_CANONICAL_MISMATCH]
             << ",\"pending_view_mismatch\":"
             << native_midpoint.reset_with_previous_reason_counts[
                    GL_NATIVE_MIDPOINT_RESET_PENDING_VIEW_MISMATCH]
             << ",\"frontend_transaction\":"
             << native_midpoint.reset_with_previous_reason_counts[
                    GL_NATIVE_MIDPOINT_RESET_FRONTEND_TRANSACTION]
             << ",\"frontend_non_native_wide\":"
             << native_midpoint.reset_with_previous_reason_counts[
                    GL_NATIVE_MIDPOINT_RESET_FRONTEND_NON_NATIVE_WIDE]
             << ",\"frontend_non_native_stream\":"
             << native_midpoint.reset_with_previous_reason_counts[
                    GL_NATIVE_MIDPOINT_RESET_FRONTEND_NON_NATIVE_STREAM]
             << ",\"frontend_cpu_present\":"
             << native_midpoint.reset_with_previous_reason_counts[
                    GL_NATIVE_MIDPOINT_RESET_FRONTEND_CPU_PRESENT]
             << "},\"pending_mismatch_counts\":{\"slot\":"
             << native_midpoint.pending_mismatch_slot_count
             << ",\"x\":" << native_midpoint.pending_mismatch_x_count
             << ",\"y\":" << native_midpoint.pending_mismatch_y_count
             << ",\"width\":" << native_midpoint.pending_mismatch_width_count
             << ",\"height\":" << native_midpoint.pending_mismatch_height_count
             << ",\"accepted_vertical_lag\":"
             << native_midpoint.pending_vertical_lag_count
             << "},\"last_pending_rect\":{\"slot\":"
             << native_midpoint.last_pending_slot
             << ",\"x\":" << native_midpoint.last_pending_x
             << ",\"y\":" << native_midpoint.last_pending_y
             << ",\"width\":" << native_midpoint.last_pending_width
             << ",\"height\":" << native_midpoint.last_pending_height
              << "},\"last_present_rect\":{\"slot\":"
             << native_midpoint.last_present_slot
             << ",\"x\":" << native_midpoint.last_present_x
             << ",\"y\":" << native_midpoint.last_present_y
             << ",\"width\":" << native_midpoint.last_present_width
              << ",\"height\":" << native_midpoint.last_present_height
              << "}},\"retired_failures\":{\"total\":"
              << retired_failure_total
              << ",\"stored\":" << retired_failure_events.size()
              << ",\"telemetry_overflow\":" << retired_failure_overflow
              << ",\"reason_counts\":{\"missing_anchor\":"
              << retired_failure_reason_counts[GL_RETIRED_FAILURE_MISSING_ANCHOR]
              << ",\"scene_mismatch\":"
              << retired_failure_reason_counts[GL_RETIRED_FAILURE_SCENE_MISMATCH]
              << ",\"position_mode_mismatch\":"
              << retired_failure_reason_counts[
                     GL_RETIRED_FAILURE_POSITION_MODE_MISMATCH]
              << ",\"material_position_mismatch\":"
              << retired_failure_reason_counts[
                     GL_RETIRED_FAILURE_MATERIAL_POSITION_MISMATCH]
              << ",\"anchor_overflow\":"
              << retired_failure_reason_counts[GL_RETIRED_FAILURE_ANCHOR_OVERFLOW]
              << ",\"history_miss\":"
              << retired_failure_reason_counts[GL_RETIRED_FAILURE_HISTORY_MISS]
              << ",\"capacity\":"
              << retired_failure_reason_counts[GL_RETIRED_FAILURE_CAPACITY]
              << ",\"phase\":"
              << retired_failure_reason_counts[GL_RETIRED_FAILURE_PHASE]
              << ",\"midpoint_zero_area\":"
              << retired_failure_reason_counts[
                     GL_RETIRED_FAILURE_MIDPOINT_ZERO_AREA]
              << ",\"midpoint_extent_collapse\":"
              << retired_failure_reason_counts[
                     GL_RETIRED_FAILURE_MIDPOINT_EXTENT_COLLAPSE]
              << ",\"midpoint_winding_flip\":"
              << retired_failure_reason_counts[
                     GL_RETIRED_FAILURE_MIDPOINT_WINDING_FLIP]
              << ",\"front_order_displacement\":"
              << retired_failure_reason_counts[
                     GL_RETIRED_FAILURE_FRONT_ORDER_DISPLACEMENT]
              << ",\"midpoint_vertex_conflict\":"
              << retired_failure_reason_counts[
                     GL_RETIRED_FAILURE_MIDPOINT_VERTEX_CONFLICT]
              << ",\"midpoint_fixed_zero_area\":"
              << retired_failure_reason_counts[
                     GL_RETIRED_FAILURE_MIDPOINT_FIXED_ZERO_AREA]
              << ",\"midpoint_fixed_winding_flip\":"
              << retired_failure_reason_counts[
                     GL_RETIRED_FAILURE_MIDPOINT_FIXED_WINDING_FLIP]
              << "},\"events\":[";
    for (size_t index = 0u; index < retired_failure_events.size(); ++index) {
        const GlRendererRetiredFailureEvent& event =
            retired_failure_events[index];
        output << (index != 0u ? "," : "")
               << "{\"frame\":" << event.frame
               << ",\"reason\":\"" << retired_failure_name(event.reason)
               << "\",\"reason_id\":" << event.reason
               << ",\"scene\":" << event.scene_id
               << ",\"producer\":" << event.producer_id
               << ",\"primitive\":" << event.primitive_id
               << ",\"group\":" << event.group_id
               << ",\"vertex\":" << event.vertex_id
               << ",\"previous_order\":" << event.previous_order
               << ",\"auxiliary\":" << event.auxiliary
               << ",\"value_a\":" << event.value_a
               << ",\"value_b\":" << event.value_b << "}";
    }
    output << "]},\"context\":{\"valid\":" << (replay.snapshot.valid_field ? "true" : "false") << ",\"id\":" << replay.snapshot.masked_field_id << ",\"raw_id\":" << replay.snapshot.raw_field_id << ",\"progress\":" << replay.snapshot.game_progress << ",\"requested_module\":" << replay.snapshot.requested_module << ",\"active_module\":" << replay.snapshot.active_module << "},\"media\":{\"fmv_active\":" << (replay.media.fmv_active ? "true" : "false") << ",\"xa_streaming\":" << (replay.media.xa_streaming ? "true" : "false") << ",\"mdec_decode_count\":" << replay.media.mdec_decode_count << "},\"media_observation\":{\"samples\":" << replay.media_samples << ",\"fmv_seen\":" << (replay.fmv_seen ? "true" : "false") << ",\"fmv_active_samples\":" << replay.fmv_active_samples << ",\"fmv_first_vblank\":" << replay.fmv_first_vblank << ",\"fmv_last_vblank\":" << replay.fmv_last_vblank << ",\"xa_seen\":" << (replay.xa_seen ? "true" : "false") << ",\"xa_streaming_samples\":" << replay.xa_streaming_samples << ",\"xa_first_vblank\":" << replay.xa_first_vblank << ",\"xa_last_vblank\":" << replay.xa_last_vblank << ",\"first_mdec_decode_count\":" << replay.first_mdec_decode_count << ",\"max_mdec_decode_count\":" << replay.max_mdec_decode_count << "},\"loader\":{\"active\":" << replay.loader.overlay_active << ",\"registered\":" << replay.loader.overlay_registered << ",\"regions_checked\":" << replay.loader.overlay_regions_checked << ",\"file_found\":" << replay.loader.overlay_file_found << "},\"cd\":{\"has_disc\":" << replay.loader.cd_has_disc << ",\"reading\":" << replay.loader.cd_reading << ",\"sector_available\":" << replay.loader.cd_sector_available << ",\"pending_pending\":" << replay.loader.cd_pending_pending << ",\"pending_cmd\":" << (unsigned)replay.loader.cd_pending_cmd << ",\"queued_cmd\":" << (unsigned)replay.loader.cd_queued_cmd << "},\"semantic_overflow\":" << (replay.semantic_overflow ? "true" : "false") << ",\"semantic_transitions\":[";
    for (size_t index = 0; index < replay.semantic_transitions.size(); ++index) {
        const SemanticTransition& transition = replay.semantic_transitions[index];
        output << (index ? "," : "") << "{\"vblank\":" << transition.vblank
               << ",\"requested_module\":" << transition.snapshot.requested_module
               << ",\"active_module\":" << transition.snapshot.active_module
               << ",\"field_id\":" << transition.snapshot.masked_field_id
               << ",\"game_progress\":" << transition.snapshot.game_progress
               << ",\"context_valid\":" << (transition.snapshot.valid_field ? "true" : "false")
               << ",\"loader_registered\":" << transition.loader.overlay_registered
               << ",\"loader_file_found\":" << transition.loader.overlay_file_found << "}";
    }
    output << "],\"replay\":{\"action_index\":" << replay.action_index
             << ",\"action_polls\":" << replay.action_polls
             << ",\"action_vblanks\":" << replay.action_vblanks
             << ",\"checkpoint_seen_vblank\":" << replay.checkpoint_vblank
             << ",\"trace_state_count\":" << replay.states.size()
             << ",\"trace_index\":" << replay.index
             << ",\"stop_reason\":" << static_cast<unsigned>(replay.reason)
             << ",\"latch_failure\":" << static_cast<unsigned>(replay.latch_failure)
             << ",\"lifecycle_stage\":" << static_cast<unsigned>(replay.lifecycle_stage)
           << ",\"lifecycle_neutral_polls\":" << replay.lifecycle_neutral_polls
           << ",\"completed_cycles\":" << replay.completed_cycles
           << "},\"transitions\":[";
    for (size_t index = 0; index < replay.transitions.size(); ++index) {
        const Transition& transition = replay.transitions[index];
        output << (index ? "," : "") << "{\"vblank\":" << transition.vblank
               << ",\"requested_module\":" << transition.snapshot.requested_module
               << ",\"active_module\":" << transition.snapshot.active_module
               << ",\"field_id\":" << transition.snapshot.masked_field_id
               << ",\"game_progress\":" << transition.snapshot.game_progress
               << ",\"cd_pending_cmd\":" << (unsigned)transition.loader.cd_pending_cmd
               << ",\"cd_reading\":" << transition.loader.cd_reading
               << ",\"cd_sector_available\":" << transition.loader.cd_sector_available
               << ",\"loader_registered\":" << transition.loader.overlay_registered
               << ",\"loader_file_found\":" << transition.loader.overlay_file_found
               << ",\"fmv_active\":" << (transition.media.fmv_active ? "true" : "false")
               << ",\"xa_streaming\":" << (transition.media.xa_streaming ? "true" : "false")
               << ",\"mdec_decode_count\":" << transition.media.mdec_decode_count << "}";
    }
    output << "],\"sio\":{\"polls\":" << replay.receipt.polls << ",\"slot\":" << (unsigned)replay.receipt.slot << ",\"id\":" << (unsigned)replay.receipt.id << ",\"ack\":" << (unsigned)replay.receipt.ack << ",\"buttons_low\":" << (unsigned)replay.receipt.buttons_low << ",\"buttons_high\":" << (unsigned)replay.receipt.buttons_high << ",\"analog\":" << (replay.receipt.analog ? "true" : "false") << ",\"neutral_count\":" << replay.neutral_count << ",\"start_count\":" << replay.start_count << ",\"cross_count\":" << replay.cross_count << ",\"cross_first\":" << replay.cross_first << ",\"cross_last\":" << replay.cross_last << ",\"other_count\":" << replay.other_count << "},\"guest_sequence\":{\"vblank_latches\":" << c.vblank_latches << "},\"counters\":{\"vblank_latches\":" << c.vblank_latches << ",\"guest_vblank_callbacks\":" << c.guest_vblank_callbacks << ",\"trace_state_latches\":" << c.trace_state_latches << ",\"provider_updates\":" << c.provider_updates << ",\"capture_samples\":" << c.capture_samples << ",\"mapping_reads\":" << c.mapping_reads << ",\"sio_applies\":" << c.sio_applies << "},\"prohibited_apis\":{\"net_pad\":false,\"direct_sio\":false,\"debug_input_override\":false,\"ram_writes\":false}";
#ifdef PSX_INPUT_REPLAY_XG_AUTH_PROOF
    output << ",\"shadow\":{\"zoom_template_contract\":{\"generation\":"
           << zoom_template_contract.generation
           << ",\"producer_store_pc\":"
           << zoom_template_contract.producer_store_pc
           << ",\"template_count\":"
           << zoom_template_contract.template_count
           << ",\"buffer_count\":"
           << zoom_template_contract.buffer_count
           << ",\"opcode\":"
           << static_cast<unsigned>(zoom_template_contract.opcode)
           << ",\"authenticated\":"
           << (zoom_template_contract.authenticated ? "true" : "false")
           << ",\"initializer_begins\":"
           << zoom_template_contract.initializer_begin_count
           << ",\"initializer_commits\":"
           << zoom_template_contract.initializer_commit_count
           << ",\"initializer_2e\":"
           << zoom_template_contract.initializer_2e_count
           << ",\"rgb_updates\":"
           << zoom_template_contract.rgb_update_count
           << ",\"invocations\":"
           << zoom_template_contract.invocation_count
           << ",\"cutover_attempts\":"
           << zoom_template_contract.cutover_attempt_count
           << ",\"native_invocations\":"
           << zoom_template_contract.native_invocation_count
           << ",\"native_primitives\":"
           << zoom_template_contract.native_primitive_count
           << ",\"replay_invocations\":"
           << zoom_template_contract.replay_invocation_count
           << ",\"replay_primitives\":"
           << zoom_template_contract.replay_primitive_count
           << ",\"rejections\":"
           << zoom_template_contract.rejection_count
           << ",\"last_rejection_blocker\":"
           << zoom_template_contract.last_rejection_blocker
           << "},\"overlay_ft4_2c\":{\"producer_entries\":"
           << overlay_ft4_2c.producer_entry_count
           << ",\"producer_returns\":" << overlay_ft4_2c.producer_return_count
           << ",\"caller_calls\":" << overlay_ft4_2c.caller_call_count
           << ",\"caller_finishes\":" << overlay_ft4_2c.caller_finish_count
           << ",\"rectangle_helpers\":" << overlay_ft4_2c.rectangle_helper_count
           << ",\"static_quads\":" << overlay_ft4_2c.static_quad_count
           << ",\"dynamic_uv_templates\":"
           << overlay_ft4_2c.dynamic_uv_template_count
           << ",\"rejected_sites\":" << overlay_ft4_2c.rejected_site_count
           << ",\"direct_templates\":" << overlay_ft4_2c.direct_template_count
           << ",\"direct_add_prims\":" << overlay_ft4_2c.direct_add_prim_count
           << ",\"direct_native\":" << overlay_ft4_2c.direct_native_count
           << ",\"direct_stage_failures\":"
           << overlay_ft4_2c.direct_stage_failure_count
           << ",\"rectangle_templates\":"
           << overlay_ft4_2c.rectangle_template_count
           << ",\"rectangle_add_prims\":"
           << overlay_ft4_2c.rectangle_add_prim_count
           << ",\"rectangle_native\":" << overlay_ft4_2c.rectangle_native_count
           << ",\"rectangle_stage_failures\":"
           << overlay_ft4_2c.rectangle_stage_failure_count
           << ",\"projected_materials\":"
           << overlay_ft4_2c.projected_material_count
           << ",\"projected_geometry\":"
           << overlay_ft4_2c.projected_geometry_count
           << ",\"projected_add_prims\":"
           << overlay_ft4_2c.projected_add_prim_count
           << ",\"projected_native\":" << overlay_ft4_2c.projected_native_count
            << ",\"projected_stage_failures\":"
            << overlay_ft4_2c.projected_stage_failure_count
            << ",\"projected_missing_materials\":"
            << overlay_ft4_2c.projected_missing_material_count
            << ",\"projected_missing_outer_overflow\":"
            << overlay_ft4_2c.projected_missing_outer_overflow
            << ",\"projected_missing_outers\":[";
    for (uint32_t index = 0u;
         index < overlay_ft4_2c.projected_missing_outer_count; ++index) {
        if (index != 0u) output << ',';
        output << "{\"return\":"
               << overlay_ft4_2c.projected_missing_outer_returns[index]
               << ",\"count\":"
               << overlay_ft4_2c.projected_missing_outer_counts[index] << '}';
    }
    output
            << "]"
            << ",\"projected_2e_materials\":"
           << overlay_ft4_2c.projected_2e_material_count
           << ",\"projected_2e_geometry\":"
           << overlay_ft4_2c.projected_2e_geometry_count
           << ",\"projected_2e_add_prims\":"
           << overlay_ft4_2c.projected_2e_add_prim_count
           << ",\"projected_2e_native\":"
           << overlay_ft4_2c.projected_2e_native_count
           << ",\"projected_2e_stage_failures\":"
           << overlay_ft4_2c.projected_2e_stage_failure_count
           << ",\"field_source_templates\":"
           << overlay_ft4_2c.field_source_template_count
           << ",\"field_base_templates\":"
           << overlay_ft4_2c.field_base_template_count
           << ",\"field_offset_templates\":"
           << overlay_ft4_2c.field_offset_template_count
           << ",\"field_materials\":"
           << overlay_ft4_2c.field_material_count
           << ",\"field_add_prims\":"
           << overlay_ft4_2c.field_add_prim_count
           << ",\"field_native\":"
           << overlay_ft4_2c.field_native_count
           << ",\"field_stage_failures\":"
           << overlay_ft4_2c.field_stage_failure_count
           << ",\"last_pc\":" << overlay_ft4_2c.last_pc
           << ",\"last_packet\":" << overlay_ft4_2c.last_packet
           << ",\"last_ot\":" << overlay_ft4_2c.last_ot
           << ",\"substitution_blocker\":"
           << overlay_ft4_2c.substitution_blocker
           << "},\"projected\":{\"initializer_begins\":"
           << projected_lifecycle.initializer_begin_count
           << ",\"initializer_registrations\":"
           << projected_lifecycle.initializer_registration_count
           << ",\"cutover_attempts\":"
           << projected_lifecycle.cutover_attempt_count
           << ",\"cutover_successes\":"
           << projected_lifecycle.cutover_success_count
           << ",\"native_primitives\":"
           << projected_lifecycle.primitive_count
           << ",\"source_misses\":"
           << projected_lifecycle.source_miss_count
           << ",\"source_blocked\":"
           << projected_lifecycle.source_blocked_count
           << ",\"pending_resets\":"
           << projected_lifecycle.pending_reset_count
           << ",\"disable_resets\":"
           << projected_lifecycle.disable_reset_count
           << ",\"code_write_resets\":"
           << projected_lifecycle.code_write_reset_count
           << ",\"loader_resets\":"
           << projected_lifecycle.loader_reset_count
           << "},\"model_ft4\":{\"dispatch_begins\":"
           << model_shadow.dispatch_begin_count
           << ",\"dispatch_caller_rejects\":"
           << model_shadow.dispatch_caller_reject_count
           << ",\"dispatch_mode_rejects\":"
           << model_shadow.dispatch_mode_reject_count
           << ",\"average_seams\":" << model_shadow.average_seam_count
           << ",\"farthest_seams\":" << model_shadow.farthest_seam_count
           << ",\"seams_without_context\":"
           << model_shadow.seam_without_context_count
           << ",\"invocations\":" << model_shadow.invocation_count
           << ",\"native_cutovers\":" << model_shadow.native_cutover_count
           << ",\"native_primitives\":" << model_shadow.native_primitive_count
           << ",\"primitives\":" << model_shadow.primitive_count
           << ",\"matches\":" << model_shadow.match_count
           << ",\"mismatches\":" << model_shadow.mismatch_count
           << ",\"payload_mismatches\":" << model_shadow.payload_mismatch_count
           << ",\"geometry_mismatches\":" << model_shadow.geometry_mismatch_count
           << ",\"tag_mismatches\":" << model_shadow.tag_mismatch_count
           << ",\"ot_mismatches\":" << model_shadow.ot_mismatch_count
           << ",\"cursor_mismatches\":" << model_shadow.cursor_mismatch_count
           << ",\"counter_mismatches\":" << model_shadow.counter_mismatch_count
           << ",\"template_captures\":" << model_shadow.template_capture_count
           << ",\"template_hits\":" << model_shadow.template_hit_count
           << ",\"template_misses\":" << model_shadow.template_miss_count
           << ",\"guest_pass_observations\":"
           << model_shadow.guest_pass_observation_count
           << ",\"guest_pass_projection_disagreements\":"
           << model_shadow.guest_pass_projection_disagreement_count
           << ",\"replay_attempts\":" << model_shadow.replay_attempt_count
           << ",\"replay_resolved\":" << model_shadow.replay_resolved_count
           << ",\"replay_lookup_misses\":"
           << model_shadow.replay_lookup_miss_count
           << ",\"replay_record_rejects\":"
           << model_shadow.replay_record_reject_count
           << ",\"replay_container_rejects\":"
           << model_shadow.replay_container_reject_count
           << ",\"replay_lifecycle_rejects\":"
           << model_shadow.replay_lifecycle_reject_count
           << ",\"replay_translate_rejects\":"
           << model_shadow.replay_translate_reject_count
           << ",\"publish_invocations\":"
           << model_shadow.publish_invocation_count
           << ",\"publish_sources\":" << model_shadow.publish_source_count
           << ",\"validation_rejected_sources\":"
           << model_shadow.validation_rejected_source_count
           << ",\"framing_rejected_invocations\":"
           << model_shadow.framing_rejected_invocation_count
           << ",\"first_mismatch_primitive\":" << model_shadow.first_mismatch_primitive
           << ",\"first_mismatch_packet\":" << model_shadow.first_mismatch_packet
           << ",\"last_model_address\":" << model_shadow.last_model_address
           << ",\"last_topology_base\":" << model_shadow.last_topology_base
           << ",\"last_material_base\":" << model_shadow.last_material_base
           << ",\"last_attribute_address\":" << model_shadow.last_attribute_address
           << ",\"last_material_word\":" << model_shadow.last_material_word
           << ",\"last_group_count\":" << model_shadow.last_group_count
           << ",\"last_target_count\":" << model_shadow.last_target_count
           << ",\"last_dispatch_caller\":" << model_shadow.last_dispatch_caller
           << ",\"last_dispatch_mode\":" << model_shadow.last_dispatch_mode
           << ",\"last_seam_pc\":" << model_shadow.last_seam_pc
           << ",\"prepare_failure_detail\":" << model_shadow.prepare_failure_detail
           << ",\"first_payload_mismatch\":";
    write_ft4_payload_mismatch(
        output, model_shadow.first_payload_mismatch);
    output << ",\"blocker\":" << model_shadow.blocker
           << ",\"pending\":" << (model_shadow.pending ? "true" : "false")
           << ",\"blocked\":" << (model_shadow.blocked ? "true" : "false")
           << "},\"model_ft3\":{\"invocations\":"
           << model_ft3_shadow.invocation_count
           << ",\"native_cutovers\":"
           << model_ft3_shadow.native_cutover_count
           << ",\"native_primitives\":"
           << model_ft3_shadow.native_primitive_count
           << ",\"primitives\":" << model_ft3_shadow.primitive_count
           << ",\"matches\":" << model_ft3_shadow.match_count
           << ",\"mismatches\":" << model_ft3_shadow.mismatch_count
           << ",\"payload_mismatches\":"
           << model_ft3_shadow.payload_mismatch_count
           << ",\"geometry_mismatches\":"
           << model_ft3_shadow.geometry_mismatch_count
           << ",\"tag_mismatches\":"
           << model_ft3_shadow.tag_mismatch_count
           << ",\"ot_mismatches\":" << model_ft3_shadow.ot_mismatch_count
           << ",\"cursor_mismatches\":"
           << model_ft3_shadow.cursor_mismatch_count
           << ",\"counter_mismatches\":"
           << model_ft3_shadow.counter_mismatch_count
           << ",\"template_captures\":"
           << model_ft3_shadow.template_capture_count
           << ",\"template_hits\":" << model_ft3_shadow.template_hit_count
           << ",\"template_misses\":" << model_ft3_shadow.template_miss_count
           << ",\"raw_color_differences\":"
           << model_ft3_shadow.raw_color_difference_count
           << ",\"guest_pass_observations\":"
           << model_ft3_shadow.guest_pass_observation_count
           << ",\"guest_pass_projection_disagreements\":"
           << model_ft3_shadow.guest_pass_projection_disagreement_count
           << ",\"replay_attempts\":"
           << model_ft3_shadow.replay_attempt_count
           << ",\"replay_resolved\":"
           << model_ft3_shadow.replay_resolved_count
           << ",\"replay_lookup_misses\":"
           << model_ft3_shadow.replay_lookup_miss_count
           << ",\"replay_lookup_invalid\":"
           << model_ft3_shadow.replay_lookup_invalid_count
           << ",\"replay_lookup_absent\":"
           << model_ft3_shadow.replay_lookup_absent_count
           << ",\"replay_record_rejects\":"
           << model_ft3_shadow.replay_record_reject_count
           << ",\"replay_container_rejects\":"
           << model_ft3_shadow.replay_container_reject_count
           << ",\"replay_lifecycle_rejects\":"
           << model_ft3_shadow.replay_lifecycle_reject_count
           << ",\"replay_translate_rejects\":"
           << model_ft3_shadow.replay_translate_reject_count
           << ",\"publish_invocations\":"
           << model_ft3_shadow.publish_invocation_count
           << ",\"publish_sources\":"
           << model_ft3_shadow.publish_source_count
           << ",\"validation_rejected_sources\":"
           << model_ft3_shadow.validation_rejected_source_count
           << ",\"framing_rejected_invocations\":"
           << model_ft3_shadow.framing_rejected_invocation_count
           << ",\"first_mismatch_packet\":"
           << model_ft3_shadow.first_mismatch_packet
           << ",\"last_replay_lookup_miss_source\":"
           << model_ft3_shadow.last_replay_lookup_miss_source
           << ",\"source_count\":" << model_ft3_shadow.source_count
           << ",\"last_group_count\":" << model_ft3_shadow.last_group_count
           << ",\"last_target_count\":" << model_ft3_shadow.last_target_count
           << ",\"prepare_failure_detail\":"
           << model_ft3_shadow.prepare_failure_detail
           << ",\"first_payload_mismatch\":";
    write_ft4_payload_mismatch(
        output, model_ft3_shadow.first_payload_mismatch);
    output
           << ",\"blocker\":" << model_ft3_shadow.blocker
           << ",\"pending\":"
           << (model_ft3_shadow.pending ? "true" : "false")
           << ",\"blocked\":"
           << (model_ft3_shadow.blocked ? "true" : "false")
           << "},\"sprite_ft4\":{\"callers\":" << sprite_shadow.caller_count
           << ",\"native_cutovers\":"
           << actor_sprites_native.native_cutover_count +
                  sprite_shadow.native_cutover_count
           << ",\"native_primitives\":"
           << actor_sprites_native.native_primitive_count +
                  sprite_shadow.native_primitive_count
           << ",\"direct_native_cutovers\":"
           << sprite_shadow.native_cutover_count
           << ",\"direct_native_primitives\":"
           << sprite_shadow.native_primitive_count
           << ",\"resident_publish_sources\":"
           << sprite_shadow.resident_publish_source_count
           << ",\"resident_replay_attempts\":"
           << sprite_shadow.resident_replay_attempt_count
           << ",\"resident_replay_resolved\":"
           << sprite_shadow.resident_replay_resolved_count
           << ",\"resident_replay_lookup_misses\":"
           << sprite_shadow.resident_replay_lookup_miss_count
           << ",\"resident_replay_record_rejects\":"
           << sprite_shadow.resident_replay_record_reject_count
           << ",\"resident_replay_container_rejects\":"
           << sprite_shadow.resident_replay_container_reject_count
           << ",\"resident_replay_lifecycle_rejects\":"
           << sprite_shadow.resident_replay_lifecycle_reject_count
           << ",\"resident_replay_translate_rejects\":"
           << sprite_shadow.resident_replay_translate_reject_count
           << ",\"field_builder_begins\":"
           << sprite_shadow.field_builder_begin_count
           << ",\"field_builder_native_cutovers\":"
           << sprite_shadow.field_builder_native_cutover_count
           << ",\"field_builder_native_primitives\":"
           << sprite_shadow.field_builder_native_primitive_count
           << ",\"field_builder_template_captures\":"
           << sprite_shadow.field_builder_template_capture_count
           << ",\"field_builder_template_updates\":"
           << sprite_shadow.field_builder_template_update_count
           << ",\"field_builder_template_invalidations\":"
           << sprite_shadow.field_builder_template_invalidation_count
           << ",\"field_builder_template_count\":"
           << sprite_shadow.field_builder_template_count
           << ",\"field_builder_dma_replay_primitives\":"
           << sprite_shadow.field_builder_dma_replay_primitive_count
           << ",\"field_builder_primitives\":"
           << sprite_shadow.field_builder_primitive_count
           << ",\"field_builder_matches\":"
           << sprite_shadow.field_builder_match_count
           << ",\"field_builder_mismatches\":"
           << sprite_shadow.field_builder_mismatch_count
           << ",\"field_builder_active_scenes\":"
           << sprite_shadow.field_builder_active_scene_count
           << ",\"empty_callers\":" << sprite_shadow.empty_caller_count
           << ",\"projections\":" << sprite_shadow.projection_count
           << ",\"matches\":" << sprite_shadow.match_count
           << ",\"mismatches\":" << sprite_shadow.mismatch_count
           << ",\"geometry_mismatches\":" << sprite_shadow.geometry_mismatch_count
           << ",\"payload_mismatches\":" << sprite_shadow.payload_mismatch_count
           << ",\"last_caller\":" << sprite_shadow.last_caller
           << ",\"last_field_builder_caller\":"
           << sprite_shadow.last_field_builder_caller
           << ",\"field_builder_caller_candidates\":["
           << sprite_shadow.field_builder_caller_candidates[0] << ','
           << sprite_shadow.field_builder_caller_candidates[1] << ','
           << sprite_shadow.field_builder_caller_candidates[2] << ','
           << sprite_shadow.field_builder_caller_candidates[3] << ']'
           << ",\"field_builder_caller_counts\":["
           << sprite_shadow.field_builder_caller_counts[0] << ','
           << sprite_shadow.field_builder_caller_counts[1] << ','
           << sprite_shadow.field_builder_caller_counts[2] << ','
           << sprite_shadow.field_builder_caller_counts[3] << ']'
           << ",\"field_builder_first_mismatch_packet\":"
           << sprite_shadow.field_builder_first_mismatch_packet
           << ",\"field_builder_first_mismatch_descriptor\":"
           << sprite_shadow.field_builder_first_mismatch_descriptor
           << ",\"field_builder_first_mismatch_caller\":"
           << sprite_shadow.field_builder_first_mismatch_caller
           << ",\"field_builder_first_mismatch_bits\":"
           << sprite_shadow.field_builder_first_mismatch_bits
           << ",\"field_builder_expected_xy\":["
           << sprite_shadow.field_builder_expected_xy[0] << ','
           << sprite_shadow.field_builder_expected_xy[1] << ','
           << sprite_shadow.field_builder_expected_xy[2] << ','
           << sprite_shadow.field_builder_expected_xy[3] << ']'
           << ",\"field_builder_actual_xy\":["
           << sprite_shadow.field_builder_actual_xy[0] << ','
           << sprite_shadow.field_builder_actual_xy[1] << ','
           << sprite_shadow.field_builder_actual_xy[2] << ','
           << sprite_shadow.field_builder_actual_xy[3] << ']'
           << ",\"field_builder_expected_uv\":["
           << sprite_shadow.field_builder_expected_uv[0] << ','
           << sprite_shadow.field_builder_expected_uv[1] << ','
           << sprite_shadow.field_builder_expected_uv[2] << ','
           << sprite_shadow.field_builder_expected_uv[3] << ']'
           << ",\"field_builder_actual_uv\":["
           << sprite_shadow.field_builder_actual_uv[0] << ','
           << sprite_shadow.field_builder_actual_uv[1] << ','
           << sprite_shadow.field_builder_actual_uv[2] << ','
           << sprite_shadow.field_builder_actual_uv[3] << ']'
           << ",\"field_builder_expected_tpage\":"
           << sprite_shadow.field_builder_expected_tpage
           << ",\"field_builder_actual_tpage\":"
           << sprite_shadow.field_builder_actual_tpage
           << ",\"field_builder_expected_clut\":"
           << sprite_shadow.field_builder_expected_clut
           << ",\"field_builder_actual_clut\":"
           << sprite_shadow.field_builder_actual_clut
           << ",\"field_builder_actual_command\":"
           << sprite_shadow.field_builder_actual_command
           << ",\"field_builder_blocker\":"
           << sprite_shadow.field_builder_blocker
           << ",\"field_builder_min_packet\":"
           << sprite_shadow.field_builder_min_packet
           << ",\"field_builder_max_packet\":"
           << sprite_shadow.field_builder_max_packet
           << ",\"last_sprite_address\":" << sprite_shadow.last_sprite_address
           << ",\"last_data_address\":" << sprite_shadow.last_data_address
           << ",\"last_descriptor_address\":" << sprite_shadow.last_descriptor_address
           << ",\"last_primitive_count\":" << sprite_shadow.last_primitive_count
           << ",\"first_mismatch_packet\":" << sprite_shadow.first_mismatch_packet
           << ",\"first_mismatch_descriptor\":" << sprite_shadow.first_mismatch_descriptor
           << ",\"first_payload_mismatch\":";
    write_ft4_payload_mismatch(
        output, sprite_shadow.first_payload_mismatch);
    output << ",\"blocker\":" << sprite_shadow.blocker
           << ",\"blocker_detail\":" << sprite_shadow.blocker_detail
           << ",\"context_active\":" << (sprite_shadow.context_active ? "true" : "false")
           << ",\"pending\":" << (sprite_shadow.pending ? "true" : "false")
           << ",\"blocked\":" << (sprite_shadow.blocked ? "true" : "false")
           << ",\"field_builder_pending\":"
           << (sprite_shadow.field_builder_pending ? "true" : "false")
           << ",\"field_builder_blocked\":"
           << (sprite_shadow.field_builder_blocked ? "true" : "false")
           << "},\"field_polyline\":{\"begins\":"
           << field_polyline.begin_count
           << ",\"invocations\":" << field_polyline.invocation_count
           << ",\"native_cutovers\":" << field_polyline.native_cutover_count
           << ",\"native_primitives\":" << field_polyline.native_primitive_count
           << ",\"primitives\":" << field_polyline.primitive_count
           << ",\"matches\":" << field_polyline.match_count
           << ",\"mismatches\":" << field_polyline.mismatch_count
           << ",\"first_mismatch_packet\":"
           << field_polyline.first_mismatch_packet
           << ",\"blocker\":" << field_polyline.blocker
           << ",\"pending\":" << (field_polyline.pending ? "true" : "false")
           << ",\"blocked\":" << (field_polyline.blocked ? "true" : "false")
           << "},\"world_horizon\":{\"native_cutovers\":"
           << horizon_shadow.native_cutover_count
           << ",\"native_primitives\":"
           << horizon_shadow.native_primitive_count
           << ",\"begins\":"
           << horizon_shadow.begin_count
           << ",\"completions\":" << horizon_shadow.completion_count
           << ",\"accepted\":" << horizon_shadow.accepted_invocation_count
           << ",\"primitives\":" << horizon_shadow.primitive_count
           << ",\"matches\":" << horizon_shadow.match_count
           << ",\"mismatches\":" << horizon_shadow.mismatch_count
           << ",\"source_capture_failures\":"
           << horizon_shadow.source_capture_failure_count
           << ",\"source_reads\":" << horizon_shadow.source_read_count
           << ",\"source_read_bytes\":" << horizon_shadow.source_read_bytes
           << ",\"payload_mismatches\":"
           << horizon_shadow.payload_mismatch_count
           << ",\"geometry_mismatches\":"
           << horizon_shadow.geometry_mismatch_count
           << ",\"tag_mismatches\":" << horizon_shadow.tag_mismatch_count
           << ",\"ot_mismatches\":" << horizon_shadow.ot_mismatch_count
           << ",\"texture_window_mismatches\":"
           << horizon_shadow.texture_window_mismatch_count
           << ",\"last_ot_bucket\":" << horizon_shadow.last_ot_bucket
           << ",\"first_mismatch_packet\":"
           << horizon_shadow.first_mismatch_packet
           << ",\"first_geometry_mismatch_invocation\":"
           << horizon_shadow.first_geometry_mismatch_invocation
           << ",\"first_geometry_mismatch_quad\":"
           << horizon_shadow.first_geometry_mismatch_quad
           << ",\"first_geometry_mismatch_vertex\":"
           << horizon_shadow.first_geometry_mismatch_vertex
           << ",\"first_geometry_expected_xy\":"
           << horizon_shadow.first_geometry_expected_xy
           << ",\"first_geometry_actual_xy\":"
           << horizon_shadow.first_geometry_actual_xy
           << ",\"first_payload_mismatch\":";
    write_ft4_payload_mismatch(
        output, horizon_shadow.first_payload_mismatch);
    output << ",\"blocker\":" << horizon_shadow.blocker
           << ",\"pending\":" << (horizon_shadow.pending ? "true" : "false")
           << ",\"blocked\":" << (horizon_shadow.blocked ? "true" : "false")
             << "},\"world_effects\":{\"native_cutovers\":"
             << effects_shadow.native_cutover_count
             << ",\"native_primitives\":"
             << effects_shadow.native_primitive_count
             << ",\"begins\":"
             << effects_shadow.begin_count
            << ",\"completions\":" << effects_shadow.completion_count
            << ",\"active_sources\":" << effects_shadow.active_source_count
            << ",\"primitives\":" << effects_shadow.primitive_count
            << ",\"candidates\":" << effects_shadow.candidate_count
            << ",\"matches\":" << effects_shadow.match_count
            << ",\"mismatches\":" << effects_shadow.mismatch_count
            << ",\"invocation_matches\":"
            << effects_shadow.invocation_match_count
            << ",\"invocation_mismatches\":"
            << effects_shadow.invocation_mismatch_count
            << ",\"source_capture_failures\":"
            << effects_shadow.source_capture_failure_count
            << ",\"source_reads\":" << effects_shadow.source_read_count
            << ",\"source_read_bytes\":" << effects_shadow.source_read_bytes
            << ",\"count_mismatches\":" << effects_shadow.count_mismatch_count
            << ",\"geometry_mismatches\":"
            << effects_shadow.geometry_mismatch_count
            << ",\"payload_mismatches\":"
            << effects_shadow.payload_mismatch_count
            << ",\"tag_mismatches\":" << effects_shadow.tag_mismatch_count
            << ",\"ot_mismatches\":" << effects_shadow.ot_mismatch_count
            << ",\"last_primitive_count\":"
            << effects_shadow.last_primitive_count
            << ",\"last_candidate_count\":"
            << effects_shadow.last_candidate_count
            << ",\"first_mismatch_packet\":"
            << effects_shadow.first_mismatch_packet
            << ",\"first_mismatch_source\":"
            << effects_shadow.first_mismatch_source
            << ",\"first_payload_mismatch\":";
    write_ft4_payload_mismatch(
        output, effects_shadow.first_payload_mismatch);
    output << ",\"blocker\":" << effects_shadow.blocker
            << ",\"pending\":" << (effects_shadow.pending ? "true" : "false")
             << ",\"blocked\":" << (effects_shadow.blocked ? "true" : "false")
             << "},\"world_terrain_water\":{\"native_cutovers\":"
             << terrain_water_shadow.native_cutover_count
             << ",\"native_primitives\":"
             << terrain_water_shadow.native_primitive_count
             << ",\"begins\":"
             << terrain_water_shadow.begin_count
             << ",\"completions\":" << terrain_water_shadow.completion_count
             << ",\"candidates\":" << terrain_water_shadow.candidate_count
             << ",\"primitives\":"
             << terrain_water_shadow.original_primitive_count
             << ",\"invocation_matches\":"
             << terrain_water_shadow.invocation_match_count
             << ",\"invocation_mismatches\":"
             << terrain_water_shadow.invocation_mismatch_count
             << ",\"packet_mismatches\":"
             << terrain_water_shadow.packet_mismatch_count
             << ",\"geometry_mismatches\":"
             << terrain_water_shadow.geometry_mismatch_count
             << ",\"payload_mismatches\":"
             << terrain_water_shadow.payload_mismatch_count
             << ",\"tag_mismatches\":"
             << terrain_water_shadow.tag_mismatch_count
             << ",\"ot_mismatches\":"
             << terrain_water_shadow.touched_ot_mismatch_count +
                    terrain_water_shadow.untouched_ot_mismatch_count
             << ",\"count_mismatches\":"
             << terrain_water_shadow.count_mismatch_count
             << ",\"capture_result\":"
             << terrain_water_shadow.last_capture_result
             << ",\"build_result\":" << terrain_water_shadow.last_build_result
             << ",\"caller_return\":" << terrain_water_shadow.last_caller_return
             << ",\"position_x\":" << terrain_water_shadow.last_position_x
             << ",\"position_z\":" << terrain_water_shadow.last_position_z
             << ",\"projection_distance\":"
             << terrain_water_shadow.last_projection_distance
             << ",\"mesh_duplicate_vertices\":"
             << terrain_water_shadow.mesh_duplicate_vertices
             << ",\"mesh_cross_tile_duplicate_vertices\":"
             << terrain_water_shadow.mesh_cross_tile_duplicate_vertices
             << ",\"mesh_canonical_raster_conflicts\":"
             << terrain_water_shadow.mesh_canonical_raster_conflicts
             << ",\"mesh_native_raster_conflicts\":"
             << terrain_water_shadow.mesh_native_raster_conflicts
             << ",\"mesh_cross_tile_native_raster_conflicts\":"
             << terrain_water_shadow.mesh_cross_tile_native_raster_conflicts
             << ",\"build_shared_duplicate_vertices\":"
             << terrain_water_shadow.build_diagnostics.shared_duplicate_vertices
             << ",\"build_shared_raster_conflicts\":"
             << terrain_water_shadow.build_diagnostics.shared_raster_conflicts
             << ",\"blocker_detail\":" << terrain_water_shadow.blocker_detail
             << ",\"blocker\":" << terrain_water_shadow.blocker
             << ",\"pending\":"
             << (terrain_water_shadow.pending ? "true" : "false")
             << ",\"blocked\":"
             << (terrain_water_shadow.blocked ? "true" : "false")
             << "},\"world_entity_shadows\":{\"native_cutovers\":"
             << entity_shadows_shadow.native_cutover_count
             << ",\"native_primitives\":"
             << entity_shadows_shadow.native_primitive_count
             << ",\"begins\":"
             << entity_shadows_shadow.begin_count
             << ",\"completions\":" << entity_shadows_shadow.completion_count
             << ",\"candidates\":" << entity_shadows_shadow.candidate_count
             << ",\"primitives\":"
             << entity_shadows_shadow.accepted_packet_count
             << ",\"invocation_matches\":"
             << entity_shadows_shadow.invocation_match_count
             << ",\"invocation_mismatches\":"
             << entity_shadows_shadow.invocation_mismatch_count
             << ",\"packet_mismatches\":"
             << entity_shadows_shadow.packet_mismatch_count
             << ",\"geometry_mismatches\":"
             << entity_shadows_shadow.geometry_mismatch_count
             << ",\"payload_mismatches\":"
             << entity_shadows_shadow.payload_mismatch_count
             << ",\"tag_mismatches\":"
             << entity_shadows_shadow.tag_mismatch_count
             << ",\"ot_mismatches\":"
             << entity_shadows_shadow.ot_mismatch_count
             << ",\"cursor_mismatches\":"
             << entity_shadows_shadow.cursor_mismatch_count
             << ",\"capture_result\":"
             << entity_shadows_shadow.last_source_capture_result
             << ",\"build_result\":"
             << entity_shadows_shadow.last_native_build_result
             << ",\"expected_finish_sp\":"
             << entity_shadows_shadow.expected_finish_stack_pointer
             << ",\"actual_finish_sp\":"
             << entity_shadows_shadow.actual_finish_stack_pointer
             << ",\"expected_saved_return\":"
             << entity_shadows_shadow.expected_saved_return
             << ",\"actual_saved_return\":"
             << entity_shadows_shadow.actual_saved_return
             << ",\"last_mismatch_bits\":"
             << entity_shadows_shadow.last_mismatch_bits
             << ",\"first_mismatch_address\":"
             << entity_shadows_shadow.first_mismatch_address
             << ",\"first_mismatch_source\":"
             << entity_shadows_shadow.first_mismatch_source_index
             << ",\"first_expected_word\":"
             << entity_shadows_shadow.first_expected_word
             << ",\"first_actual_word\":"
             << entity_shadows_shadow.first_actual_word
             << ",\"transform_observations\":"
             << entity_shadows_shadow.transform_observation_count
             << ",\"transform_mismatches\":"
             << entity_shadows_shadow.transform_mismatch_count
             << ",\"diagnostic_type\":"
             << entity_shadows_shadow.diagnostic_type
             << ",\"pending_x\":"
             << entity_shadows_shadow.diagnostic_pending_x
             << ",\"pending_z\":"
             << entity_shadows_shadow.diagnostic_pending_z
             << ",\"terrain_chunk\":"
             << entity_shadows_shadow.diagnostic_terrain_chunk
             << ",\"terrain_cell\":"
             << entity_shadows_shadow.diagnostic_terrain_cell
             << ",\"terrain_heights\":["
             << static_cast<unsigned>(entity_shadows_shadow.diagnostic_heights[0]) << ','
             << static_cast<unsigned>(entity_shadows_shadow.diagnostic_heights[1]) << ','
             << static_cast<unsigned>(entity_shadows_shadow.diagnostic_heights[2]) << ','
             << static_cast<unsigned>(entity_shadows_shadow.diagnostic_heights[3]) << ','
             << static_cast<unsigned>(entity_shadows_shadow.diagnostic_heights[4]) << ']'
             << ",\"source_terrain_normal\":["
             << entity_shadows_shadow.expected_local_transform.rotation[1][0] << ','
             << entity_shadows_shadow.expected_local_transform.rotation[1][1] << ','
             << entity_shadows_shadow.expected_local_transform.rotation[1][2] << ']'
             << ",\"expected_local_rotation\":["
             << entity_shadows_shadow.expected_local_transform.rotation[0][0] << ','
             << entity_shadows_shadow.expected_local_transform.rotation[0][1] << ','
             << entity_shadows_shadow.expected_local_transform.rotation[0][2] << ','
             << entity_shadows_shadow.expected_local_transform.rotation[1][0] << ','
             << entity_shadows_shadow.expected_local_transform.rotation[1][1] << ','
             << entity_shadows_shadow.expected_local_transform.rotation[1][2] << ','
             << entity_shadows_shadow.expected_local_transform.rotation[2][0] << ','
             << entity_shadows_shadow.expected_local_transform.rotation[2][1] << ','
             << entity_shadows_shadow.expected_local_transform.rotation[2][2] << ']'
             << ",\"actual_local_rotation\":["
             << entity_shadows_shadow.actual_local_transform.rotation[0][0] << ','
             << entity_shadows_shadow.actual_local_transform.rotation[0][1] << ','
             << entity_shadows_shadow.actual_local_transform.rotation[0][2] << ','
             << entity_shadows_shadow.actual_local_transform.rotation[1][0] << ','
             << entity_shadows_shadow.actual_local_transform.rotation[1][1] << ','
             << entity_shadows_shadow.actual_local_transform.rotation[1][2] << ','
             << entity_shadows_shadow.actual_local_transform.rotation[2][0] << ','
             << entity_shadows_shadow.actual_local_transform.rotation[2][1] << ','
             << entity_shadows_shadow.actual_local_transform.rotation[2][2] << ']'
             << ",\"expected_position\":["
             << entity_shadows_shadow.expected_position[0] << ','
             << entity_shadows_shadow.expected_position[1] << ','
             << entity_shadows_shadow.expected_position[2] << ']'
             << ",\"actual_position\":["
             << entity_shadows_shadow.actual_position[0] << ','
             << entity_shadows_shadow.actual_position[1] << ','
             << entity_shadows_shadow.actual_position[2] << ']'
             << ",\"expected_transform_rotation\":["
             << entity_shadows_shadow.expected_transform.rotation[0][0] << ','
             << entity_shadows_shadow.expected_transform.rotation[0][1] << ','
             << entity_shadows_shadow.expected_transform.rotation[0][2] << ','
             << entity_shadows_shadow.expected_transform.rotation[1][0] << ','
             << entity_shadows_shadow.expected_transform.rotation[1][1] << ','
             << entity_shadows_shadow.expected_transform.rotation[1][2] << ','
             << entity_shadows_shadow.expected_transform.rotation[2][0] << ','
             << entity_shadows_shadow.expected_transform.rotation[2][1] << ','
             << entity_shadows_shadow.expected_transform.rotation[2][2] << ']'
             << ",\"actual_transform_rotation\":["
             << entity_shadows_shadow.actual_transform.rotation[0][0] << ','
             << entity_shadows_shadow.actual_transform.rotation[0][1] << ','
             << entity_shadows_shadow.actual_transform.rotation[0][2] << ','
             << entity_shadows_shadow.actual_transform.rotation[1][0] << ','
             << entity_shadows_shadow.actual_transform.rotation[1][1] << ','
             << entity_shadows_shadow.actual_transform.rotation[1][2] << ','
             << entity_shadows_shadow.actual_transform.rotation[2][0] << ','
             << entity_shadows_shadow.actual_transform.rotation[2][1] << ','
             << entity_shadows_shadow.actual_transform.rotation[2][2] << ']'
             << ",\"expected_transform_translation\":["
             << entity_shadows_shadow.expected_transform.translation[0] << ','
             << entity_shadows_shadow.expected_transform.translation[1] << ','
             << entity_shadows_shadow.expected_transform.translation[2] << ']'
             << ",\"actual_transform_translation\":["
             << entity_shadows_shadow.actual_transform.translation[0] << ','
             << entity_shadows_shadow.actual_transform.translation[1] << ','
             << entity_shadows_shadow.actual_transform.translation[2] << ']'
             << ",\"blocker\":" << entity_shadows_shadow.blocker
             << ",\"pending\":"
             << (entity_shadows_shadow.pending ? "true" : "false")
             << ",\"blocked\":"
             << (entity_shadows_shadow.blocked ? "true" : "false")
             << "},\"world_decorations\":{\"native_cutovers\":"
             << decorations_shadow.native_cutover_count
             << ",\"native_primitives\":"
             << decorations_shadow.native_primitive_count
             << ",\"outer_begins\":"
             << decorations_shadow.outer_begin_count
             << ",\"outer_finishes\":"
             << decorations_shadow.outer_finish_count
             << ",\"helper_begins\":"
             << decorations_shadow.helper_begin_count
             << ",\"helper_finishes\":"
             << decorations_shadow.helper_finish_count
             << ",\"candidates\":" << decorations_shadow.candidate_count
             << ",\"primitives\":" << decorations_shadow.primitive_count
             << ",\"matches\":" << decorations_shadow.match_count
             << ",\"mismatches\":" << decorations_shadow.mismatch_count
             << ",\"helper_matches\":"
             << decorations_shadow.helper_match_count
             << ",\"helper_mismatches\":"
             << decorations_shadow.helper_mismatch_count
             << ",\"payload_mismatches\":"
             << decorations_shadow.payload_mismatch_count
             << ",\"geometry_mismatches\":"
             << decorations_shadow.geometry_mismatch_count
             << ",\"tag_mismatches\":"
             << decorations_shadow.tag_mismatch_count
             << ",\"ot_mismatches\":"
             << decorations_shadow.ot_mismatch_count
             << ",\"first_mismatch_kind\":"
             << decorations_shadow.first_mismatch_kind
             << ",\"first_mismatch_word\":"
             << decorations_shadow.first_mismatch_word
             << ",\"first_expected_word\":"
             << decorations_shadow.first_expected_word
             << ",\"first_actual_word\":"
             << decorations_shadow.first_actual_word
             << ",\"blocker\":" << decorations_shadow.blocker
             << ",\"pending\":"
             << (decorations_shadow.helper_active ? "true" : "false")
             << ",\"blocked\":"
             << (decorations_shadow.blocked ? "true" : "false")
             << "},\"world_clouds\":{\"native_cutovers\":"
             << clouds_shadow.native_cutover_count
             << ",\"native_primitives\":"
             << clouds_shadow.native_primitive_count
             << ",\"begins\":" << clouds_shadow.begin_count
             << ",\"completions\":" << clouds_shadow.completion_count
             << ",\"candidates\":" << clouds_shadow.candidate_count
             << ",\"primitives\":" << clouds_shadow.primitive_count
             << ",\"invocation_matches\":"
             << clouds_shadow.invocation_match_count
             << ",\"invocation_mismatches\":"
             << clouds_shadow.invocation_mismatch_count
             << ",\"packet_mismatches\":"
             << clouds_shadow.packet_mismatch_count
             << ",\"geometry_mismatches\":"
             << clouds_shadow.geometry_mismatch_count
             << ",\"payload_mismatches\":"
             << clouds_shadow.payload_mismatch_count
             << ",\"tag_mismatches\":" << clouds_shadow.tag_mismatch_count
             << ",\"ot_mismatches\":" << clouds_shadow.ot_mismatch_count
             << ",\"position_mismatches\":"
             << clouds_shadow.position_mismatch_count
             << ",\"unexpected_packet_writes\":"
             << clouds_shadow.unexpected_packet_write_count
             << ",\"cursor_mismatches\":"
             << clouds_shadow.cursor_mismatch_count
             << ",\"scratch_emitted_mismatches\":"
             << clouds_shadow.scratch_emitted_mismatch_count
             << ",\"scratch_attempt_mismatches\":"
             << clouds_shadow.scratch_attempt_mismatch_count
             << ",\"expected_cursor\":" << clouds_shadow.expected_final_cursor
             << ",\"actual_cursor\":" << clouds_shadow.actual_final_cursor
             << ",\"clouds_entered\":"
             << clouds_shadow.last_build_stats.clouds_entered
             << ",\"clouds_world_culled\":"
             << clouds_shadow.last_build_stats.clouds_world_culled
             << ",\"clouds_anchor_culled\":"
             << clouds_shadow.last_build_stats.clouds_anchor_culled
             << ",\"quad_attempts\":"
             << clouds_shadow.last_build_stats.quad_attempt_count
             << ",\"quad_projection_culled\":"
             << clouds_shadow.last_build_stats.quad_projection_culled
             << ",\"quad_screen_culled\":"
             << clouds_shadow.last_build_stats.quad_screen_culled
             << ",\"first_world_source\":"
             << clouds_shadow.last_build_stats.first_world_accepted_source
             << ",\"first_world_x\":"
             << clouds_shadow.last_build_stats.first_world_relative_x
             << ",\"first_world_z\":"
             << clouds_shadow.last_build_stats.first_world_relative_z
             << ",\"first_anchor_flags\":"
             << clouds_shadow.last_build_stats.first_anchor_flags
             << ",\"first_anchor_tx\":"
             << clouds_shadow.last_build_stats.first_anchor_translation_x
             << ",\"first_anchor_ty\":"
             << clouds_shadow.last_build_stats.first_anchor_translation_y
             << ",\"first_anchor_tz\":"
             << clouds_shadow.last_build_stats.first_anchor_translation_z
             << ",\"anchor_observations\":"
             << clouds_shadow.anchor_observation_count
             << ",\"anchor_mismatches\":"
             << clouds_shadow.anchor_mismatch_count
             << ",\"diagnostic_anchor_source\":"
             << clouds_shadow.diagnostic_anchor_source
             << ",\"expected_anchor_flags\":"
             << clouds_shadow.expected_anchor_flags
             << ",\"actual_anchor_flags\":"
             << clouds_shadow.actual_anchor_flags
             << ",\"expected_anchor_translation\":["
             << clouds_shadow.expected_anchor_translation[0] << ','
             << clouds_shadow.expected_anchor_translation[1] << ','
             << clouds_shadow.expected_anchor_translation[2] << ']'
             << ",\"actual_anchor_translation\":["
             << clouds_shadow.actual_anchor_translation[0] << ','
              << clouds_shadow.actual_anchor_translation[1] << ','
              << clouds_shadow.actual_anchor_translation[2] << ']'
              << ",\"first_packet_index\":"
              << clouds_shadow.first_packet_mismatch.packet_index
              << ",\"first_packet_address\":"
              << clouds_shadow.first_packet_mismatch.packet_address
              << ",\"first_packet_source\":"
              << clouds_shadow.first_packet_mismatch.source_index
              << ",\"first_packet_lod\":"
              << clouds_shadow.first_packet_mismatch.lod
              << ",\"first_packet_lod_quad\":"
              << clouds_shadow.first_packet_mismatch.lod_quad_index
              << ",\"first_packet_word_mask\":"
              << clouds_shadow.first_packet_mismatch.word_mismatch_mask
              << ",\"first_packet_expected_material\":"
              << clouds_shadow.first_packet_mismatch.expected_words[1]
              << ",\"first_packet_actual_material\":"
              << clouds_shadow.first_packet_mismatch.actual_words[1]
              << ",\"first_packet_expected_uv\":["
              << (clouds_shadow.first_packet_mismatch.expected_words[3] & 0xffffu) << ','
              << (clouds_shadow.first_packet_mismatch.expected_words[5] & 0xffffu) << ','
              << (clouds_shadow.first_packet_mismatch.expected_words[7] & 0xffffu) << ','
              << (clouds_shadow.first_packet_mismatch.expected_words[9] & 0xffffu) << ']'
              << ",\"first_packet_actual_uv\":["
              << (clouds_shadow.first_packet_mismatch.actual_words[3] & 0xffffu) << ','
              << (clouds_shadow.first_packet_mismatch.actual_words[5] & 0xffffu) << ','
              << (clouds_shadow.first_packet_mismatch.actual_words[7] & 0xffffu) << ','
              << (clouds_shadow.first_packet_mismatch.actual_words[9] & 0xffffu) << ']'
              << ",\"first_packet_expected_clut\":"
              << (clouds_shadow.first_packet_mismatch.expected_words[3] >> 16u)
              << ",\"first_packet_actual_clut\":"
              << (clouds_shadow.first_packet_mismatch.actual_words[3] >> 16u)
              << ",\"first_packet_expected_tpage\":"
              << (clouds_shadow.first_packet_mismatch.expected_words[5] >> 16u)
              << ",\"first_packet_actual_tpage\":"
              << (clouds_shadow.first_packet_mismatch.actual_words[5] >> 16u)
              << ",\"first_ot_bucket\":"
             << clouds_shadow.first_ot_mismatch.bucket
             << ",\"first_ot_expected\":"
             << clouds_shadow.first_ot_mismatch.expected_word
             << ",\"first_ot_actual\":"
             << clouds_shadow.first_ot_mismatch.actual_word
             << ",\"blocker\":" << clouds_shadow.blocker
             << ",\"pending\":" << (clouds_shadow.pending ? "true" : "false")
             << ",\"blocked\":" << (clouds_shadow.blocked ? "true" : "false")
              << "},\"world_minimap\":{\"native_cutovers\":"
              << minimap_shadow.native_cutover_count
              << ",\"native_primitives\":"
              << minimap_shadow.native_primitive_count
              << ",\"begins\":" << minimap_shadow.begin_count
             << ",\"completions\":" << minimap_shadow.completion_count
             << ",\"primitives\":" << minimap_shadow.primitive_count
             << ",\"invocation_matches\":"
             << minimap_shadow.invocation_match_count
             << ",\"invocation_mismatches\":"
             << minimap_shadow.invocation_mismatch_count
             << ",\"payload_mismatches\":"
             << minimap_shadow.payload_mismatch_count
             << ",\"tag_mismatches\":" << minimap_shadow.tag_mismatch_count
             << ",\"ot_mismatches\":" << minimap_shadow.ot_mismatch_count
             << ",\"scratch_mismatches\":"
             << minimap_shadow.scratch_mismatch_count
             << ",\"blocker\":" << minimap_shadow.blocker
              << ",\"pending\":" << (minimap_shadow.pending ? "true" : "false")
              << ",\"blocked\":" << (minimap_shadow.blocked ? "true" : "false")
              << "},\"world_models\":{\"native_cutovers\":"
              << models_native.native_cutover_count
              << ",\"native_primitives\":"
              << models_native.native_primitive_count
              << ",\"native_failures\":"
              << models_native.native_failure_count
              << ",\"first_failure_stage\":"
              << models_native.first_failure_stage
              << ",\"first_failure_detail\":"
              << models_native.first_failure_detail
              << ",\"first_anchor_count\":"
              << models_native.first_anchor_count
              << ",\"last_failure_stage\":"
              << models_native.last_failure_stage
              << ",\"last_failure_detail\":"
              << models_native.last_failure_detail
              << ",\"last_anchor_count\":"
              << models_native.last_anchor_count
              << ",\"packet_copy_begin_count\":"
              << models_native.packet_copy_begin_count
              << ",\"packet_copy_finish_count\":"
              << models_native.packet_copy_finish_count
              << ",\"packet_copy_template_count\":"
              << models_native.packet_copy_template_count
              << ",\"packet_copy_failure_detail\":"
              << models_native.packet_copy_failure_detail
              << ",\"packet_copy_last_destination\":"
              << models_native.packet_copy_last_destination
              << ",\"packet_copy_last_source\":"
              << models_native.packet_copy_last_source
              << ",\"packet_copy_last_size\":"
              << models_native.packet_copy_last_size
              << ",\"packet_copy_range_count\":"
              << models_native.packet_copy_range_count
              << ",\"first_missing_model_address\":"
              << models_native.first_missing_model_address
              << ",\"first_missing_packet_base\":"
              << models_native.first_missing_packet_base
              << ",\"first_missing_packet_address\":"
              << models_native.first_missing_packet_address
              << ",\"first_missing_copy_range_kind\":"
              << models_native.first_missing_copy_range_kind
              << ",\"first_missing_copy_range_index\":"
              << models_native.first_missing_copy_range_index
              << "},\"world_actor_sprites\":{\"native_cutovers\":"
              << actor_sprites_native.native_cutover_count
              << ",\"native_primitives\":"
              << actor_sprites_native.native_primitive_count
              << "},\"world_sky\":{\"native_cutovers\":"
              << sky_native.native_cutover_count
              << ",\"native_primitives\":"
              << sky_native.native_primitive_count
              << "},\"world_execution\":{\"full_dispatchers\":"
           << world_execution.full_dispatcher_count
           << ",\"observed_mask\":" << world_execution.observed_family_mask
           << ",\"instruction_mismatch_mask\":"
           << world_execution.instruction_mismatch_mask
           << ",\"overflowed\":"
           << (world_execution.overflowed ? "true" : "false")
           << ",\"families\":[";
    for (uint32_t index = 0u;
         index < PSX_XG_RENDER_WORLD_FAMILY_COUNT; ++index)
        output << (index ? "," : "")
               << world_execution.family_entry_count[index];
    output << "]}}";
#endif
#ifdef PSX_INPUT_REPLAY_XG_AUTH_PROOF
    if (producer_family_requested) {
        output << ",\"producer_family\":{\"schema\":\"xenogears.field-character-candidate/v1\""
               << ",\"family\":\"poly-ft4-semitrans\",\"opcode\":46,\"length_words\":9"
               << ",\"enabled\":" << (producer_family.enabled ? "true" : "false")
               << ",\"blocked\":" << (producer_family.blocked ? "true" : "false")
               << ",\"geometry_count\":" << producer_family.geometry_count
               << ",\"candidate_count\":" << producer_family.candidate_count
               << ",\"match_count\":" << producer_family.match_count
               << ",\"mismatch_count\":" << producer_family.mismatch_count
               << ",\"last_ot_bucket\":" << producer_family.last_ot_bucket
               << ",\"last_runtime_result\":"
               << producer_family.last_runtime_result
               << ",\"last_compare_result\":"
               << producer_family.last_compare_result
               << ",\"first_mismatch_word\":"
               << producer_family.first_mismatch_word
               << ",\"first_mismatch_byte\":"
               << producer_family.first_mismatch_byte
               << ",\"blocker\":" << producer_family.blocker
               << ",\"diagnostic\":{\"source_event_count\":"
               << producer_family.source_event_count
               << ",\"source_blocker\":" << producer_family.source_blocker
               << ",\"source_context_bits\":"
               << producer_family.source_context_bits
               << ",\"collector_phase\":" << producer_family.collector_phase
               << ",\"collector_blocker\":"
               << producer_family.collector_blocker
               << ",\"collector_access_count\":"
               << producer_family.collector_access_count
               << ",\"collector_site_count\":"
               << producer_family.collector_site_count
               << ",\"source_blocked\":"
               << (producer_family.source_blocked ? "true" : "false")
               << ",\"source_overflowed\":"
               << (producer_family.source_overflowed ? "true" : "false")
               << ",\"geometry_completed_count\":"
               << producer_family.geometry_completed_count
               << ",\"geometry_queued_count\":"
               << producer_family.geometry_queued_count
               << ",\"geometry_pending\":"
               << (producer_family.geometry_pending ? "true" : "false")
               << ",\"geometry_blocked\":"
               << (producer_family.geometry_blocked ? "true" : "false")
               << ",\"geometry_overflowed\":"
               << (producer_family.geometry_overflowed ? "true" : "false")
               << "}"
               << ",\"privacy\":{\"metadata_only\":true,\"packet_words\":false"
               << ",\"guest_paths\":false}}";
    }
#endif
#ifdef PSX_INPUT_REPLAY_XG_AUTH_PROOF
    write_auth_proof(output, field_id);
#endif
    output << "}\n";
    return output.good();
}
int status_json(char* out, int cap) {
    const Counters c = counters();
    return std::snprintf(out, static_cast<size_t>(cap),
        "{\"active\":%s,\"guest_vblank_callbacks\":%llu,\"vblank_latches\":%llu}",
        active() ? "true" : "false",
        static_cast<unsigned long long>(c.guest_vblank_callbacks),
        static_cast<unsigned long long>(c.vblank_latches));
}
}

extern "C" int input_replay_status_json(char* out, int cap) {
    return input_replay::status_json(out, cap);
}
