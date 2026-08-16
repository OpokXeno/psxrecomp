#ifndef PSX_INPUT_REPLAY_H
#define PSX_INPUT_REPLAY_H

#include <SDL.h>

#include <array>
#include <cstdint>
#include <string>

#include "host_input_snapshot.h"
#include "psx_netplay.h"

namespace input_replay {

enum class StopReason {
    None,
    CheckpointReached,
    CheckpointNotReached,
    TraceComplete,
};

struct Counters {
    uint64_t vblank_latches = 0;
    uint64_t guest_vblank_callbacks = 0;
    uint64_t trace_state_latches = 0;
    uint64_t provider_updates = 0;
    uint64_t capture_samples = 0;
    uint64_t mapping_reads = 0;
    uint64_t sio_applies = 0;
    uint64_t p1_samples = 0;
    uint64_t p2_samples = 0;
};

struct Snapshot {
    uint32_t field_context, requested_module, active_module, module_pointer;
    uint16_t raw_field_id, masked_field_id, game_progress;
    bool valid_field;
};

struct SioReceipt {
    uint64_t polls;
    uint8_t slot, id, ack, buttons_low, buttons_high;
    bool analog;
};

struct LoaderState {
    int cd_has_disc, cd_reading, cd_sector_available, cd_pending_pending;
    uint8_t cd_pending_cmd, cd_queued_cmd;
    int overlay_active, overlay_registered, overlay_regions_checked, overlay_file_found;
};

struct MediaState {
    bool fmv_active, xa_streaming;
    uint32_t mdec_decode_count;
};

enum class PadMode : uint8_t { Hybrid = 0, Analog = 1, Digital = 2 };

struct HostPadSnapshot {
    bool connected = false;
    PadMode mode = PadMode::Digital;
    std::array<uint8_t, SDL_CONTROLLER_BUTTON_MAX> buttons{};
    std::array<int16_t, SDL_CONTROLLER_AXIS_MAX> axes{};
};

struct HostInputSnapshot { std::array<HostPadSnapshot, 2> pads{}; };

bool load(const char* path, std::string* error);
bool active();
bool attach(SDL_GameController* players[2], std::string* error);
void detach(SDL_GameController* players[2]);
bool latch_vblank();
bool current_pad_config(int slot, bool* connected, PadMode* mode);
SDL_GameController* controller(int slot);
void note_guest_vblank();
void note_capture(int slot);
void note_mapping();
void note_sio();
void note_snapshot(const Snapshot& snapshot);
void note_sio_receipt(const SioReceipt& receipt);
void note_loader_state(const LoaderState& state);
void note_media_state(const MediaState& state);
bool checkpoint(uint32_t* address, uint16_t* expected);
void observe_checkpoint(uint16_t value);
bool finished();
StopReason stop_reason();
Counters counters();
bool write_evidence(const char* path, uint16_t field_id, const char* backend);
int status_json(char* out, int cap);

bool record_begin(const char* path, uint16_t stop_field, uint64_t max_vblanks,
                  std::string* error);
bool record_begin_until_close(const char* path, uint64_t max_vblanks,
                              std::string* error);
bool recording();
void record_note_guest_vblank();
bool record_snapshot(const HostInputSnapshot& snapshot, std::string* error);
bool record_snapshot(const host_input::HostInputSnapshot& snapshot,
                     const std::array<PsxNetPad, 2>& pads, std::string* error);
void record_note_scene(const Snapshot& snapshot, const LoaderState& loader);
bool record_complete();
bool record_close(std::string* error);
bool write_record_evidence(const char* path, std::string* error);
void record_abort();

}

#endif
