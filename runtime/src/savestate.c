/* savestate.c — user save states (Shift+F1-F12 save, F1-F12 load). See savestate.h.
 *
 * Wraps boot_state.c's full-machine serializer. Requests are staged by the SDL
 * key handler / debug server and executed by savestate_poll at a block-leader
 * boundary (in_exception == 0), where cpu->pc is a valid resume PC. A load
 * restores the full machine then unwinds to the scheduler and re-dispatches. */

#include "savestate.h"
#include "boot_state.h"
#include "psx_cycles.h"
#include "psx_netplay.h"
#include "psx_scheduler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#endif

static char     s_dir[512];
static uint32_t s_bios_checksum;
static uint32_t s_entry_pc;
static int      s_configured   = 0;
static int      s_save_pending = -1;   /* slot, or -1 */
static int      s_load_pending = -1;
static int      s_load_completed = 0;
static uint64_t s_load_cooldown_until_frame = 0;
static int      s_load_cooldown_notice = 0;

extern int psx_hle_scheduler_enabled(void);
extern uint64_t s_frame_count;

#define SAVESTATE_LOAD_COOLDOWN_FRAMES 60u

static void ensure_dir(const char* dir) {
    if (!dir || !dir[0]) return;
#ifdef _WIN32
    (void)_mkdir(dir);
#else
    (void)mkdir(dir, 0755);
#endif
}

void savestate_configure(const char* dir, uint32_t bios_checksum, uint32_t entry_pc) {
    if (dir && dir[0]) {
        strncpy(s_dir, dir, sizeof(s_dir) - 1);
        s_dir[sizeof(s_dir) - 1] = '\0';
        ensure_dir(s_dir);
    } else {
        s_dir[0] = '\0';
    }
    s_bios_checksum = bios_checksum;
    s_entry_pc      = entry_pc;
    s_configured    = 1;
}

const char* savestate_dir(void) {
    return s_dir;
}

void savestate_get_integrity(uint32_t* bios_checksum, uint32_t* entry_pc) {
    if (bios_checksum) *bios_checksum = s_bios_checksum;
    if (entry_pc) *entry_pc = s_entry_pc;
}

int savestate_slot_path(int slot, char* out, size_t cap) {
    if (!s_configured || !out || cap == 0) return 0;
    if (slot < 0 || slot >= SAVESTATE_SLOTS) return 0;
    /* Keyed by entry_pc so slots from different games in a shared dir never
     * collide; boot_state_load also rejects a mismatched entry_pc internally. */
    snprintf(out, cap, "%s%sstate_%08X_slot%02d.pst",
             s_dir, (s_dir[0] ? "/" : ""), (unsigned)s_entry_pc, slot);
    return 1;
}

int savestate_slot_exists(int slot) {
    char path[600];
    FILE* f;
    long sz;
    if (!savestate_slot_path(slot, path, sizeof(path))) return 0;
    f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    sz = ftell(f);
    fclose(f);
    return sz > 0;
}

int savestate_read_slot(int slot, uint8_t** data_out, size_t* size_out) {
    char path[600];
    FILE* f;
    long sz;
    uint8_t* buf;
    if (!data_out || !size_out) return 0;
    *data_out = NULL;
    *size_out = 0;
    if (!savestate_slot_path(slot, path, sizeof(path))) return 0;
    f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    sz = ftell(f);
    if (sz <= 0 || (size_t)sz > 8u * 1024u * 1024u) { fclose(f); return 0; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }
    buf = (uint8_t*)malloc((size_t)sz);
    if (!buf) { fclose(f); return 0; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return 0;
    }
    fclose(f);
    *data_out = buf;
    *size_out = (size_t)sz;
    return 1;
}

int savestate_write_slot(int slot, const void* data, size_t size) {
    char path[600];
    FILE* f;
    if (!data || size == 0) return 0;
    if (!savestate_slot_path(slot, path, sizeof(path))) return 0;
    ensure_dir(s_dir);
    f = fopen(path, "wb");
    if (!f) return 0;
    if (fwrite(data, 1, size, f) != size) {
        fclose(f);
        remove(path);
        return 0;
    }
    if (fflush(f) != 0 || fclose(f) != 0) {
        remove(path);
        return 0;
    }
    return 1;
}

static int netplay_savestate_blocked(void) {
    if (!psx_netplay_active()) return 0;
    fprintf(stderr, "savestate: disabled during netplay\n");
    return 1;
}

static int request_save_inner(int slot) {
    if (!s_configured) { fprintf(stderr, "savestate: not configured\n"); return 0; }
    if (slot < 0 || slot >= SAVESTATE_SLOTS) return 0;
    s_save_pending = slot;
    return 1;
}

static int request_load_inner(int slot) {
    if (!s_configured) { fprintf(stderr, "savestate: not configured\n"); return 0; }
    if (slot < 0 || slot >= SAVESTATE_SLOTS) return 0;
    if (s_frame_count < s_load_cooldown_until_frame) {
        if (!s_load_cooldown_notice) {
            fprintf(stderr, "savestate: load ignored during restore cooldown\n");
            s_load_cooldown_notice = 1;
        }
        return 1;
    }
    if (!psx_hle_scheduler_enabled()) {
        /* LLE (host-fiber) mode: the restore longjmp target lives on the
         * scheduler fiber; cross-fiber unwind is unsafe. HLE is the default. */
        fprintf(stderr, "savestate: load requires the HLE scheduler (default); "
                        "PSX_HLE_SCHEDULER=0 run cannot load states.\n");
        return 0;
    }
    s_load_pending = slot;
    return 1;
}

int savestate_request_save(int slot) {
    if (netplay_savestate_blocked()) return 0;
    return request_save_inner(slot);
}

int savestate_request_load(int slot) {
    if (netplay_savestate_blocked()) return 0;
    return request_load_inner(slot);
}

int savestate_request_save_protocol(int slot) {
    if (netplay_savestate_blocked()) return 0;
    return request_save_inner(slot);
}

int savestate_request_load_protocol(int slot) {
    if (netplay_savestate_blocked()) return 0;
    return request_load_inner(slot);
}

int savestate_pending(void) {
    return (s_save_pending >= 0 || s_load_pending >= 0) ? 1 : 0;
}

int savestate_take_load_completed(void) {
    int v = s_load_completed;
    s_load_completed = 0;
    return v;
}

void savestate_poll(CPUState* cpu, uint32_t resume_pc) {
    if (s_save_pending < 0 && s_load_pending < 0) return;   /* hot path: nothing staged */

    if (s_save_pending >= 0) {
        int slot = s_save_pending;
        s_save_pending = -1;
        char path[600];
        if (resume_pc == 0) {
            /* 0 is never a legitimate resume address (see psx_is_dispatchable) —
             * writing it would produce a savestate that hangs forever on load
             * instead of failing loudly. All resume_pc call sites are expected
             * to resolve a real block-leader/interpreter-safe PC; refuse rather
             * than persist a bad one. */
            fprintf(stderr,
                "savestate: SAVE REFUSED slot %d (resume_pc=0x00000000 — no "
                "valid resume point at this poll)\n", slot);
        } else if (savestate_slot_path(slot, path, sizeof(path))) {
            /* Save the exact resume PC (cpu->pc is 0 mid-block; resume_pc is the
             * block leader the interrupt path would resume at). */
            CPUState snap = *cpu;
            snap.pc = resume_pc;
            int ok = boot_state_save(&snap, s_bios_checksum, s_entry_pc, path);
            fprintf(stderr, "savestate: %s slot %d @ pc=0x%08X -> %s\n",
                    ok ? "SAVED" : "SAVE FAILED", slot, (unsigned)resume_pc, path);
        }
    }

    if (s_load_pending >= 0) {
        int slot = s_load_pending;
        s_load_pending = -1;
        char path[600];
        if (!savestate_slot_path(slot, path, sizeof(path))) return;
        if (boot_state_load(path, s_bios_checksum, s_entry_pc, cpu)) {
            if (cpu->pc == 0) {
                /* Never a valid resume address (psx_is_dispatchable rejects it
                 * too) — this state was written by the resume_pc==0 save bug.
                 * Resuming anyway hangs forever (frame counter frozen, no
                 * fail-fast) instead of failing loudly, which reads as a mystery
                 * "crash" to a player. Refuse and say why. */
                fprintf(stderr,
                    "savestate: LOAD REFUSED slot %d (saved resume pc=0x00000000 — "
                    "this state was captured with no valid resume point and "
                    "cannot be resumed; re-save from live gameplay)\n", slot);
                return;
            }
            psx_cycles_resync_after_restore();
            /* Re-seed interrupt-delivery resume bookkeeping to the fresh
             * load target — it lives outside CPUState/RAM so boot_state_load
             * never touches it, and stale pre-load values (from whatever
             * context F1 was pressed in) crash the very first interrupt
             * after resume almost immediately. See
             * psx_interrupt_resume_bookkeeping_reset in interrupts.c and
             * ISSUES.md #10. Also clear the dirty-RAM interpreter's own
             * stale safe-resume latch for the same reason. */
            {
                extern void psx_interrupt_resume_bookkeeping_reset(uint32_t resume_pc);
                extern uint32_t g_dirty_safe_resume_pc;
                extern uint32_t g_async_rfe_resume_pc;
                psx_interrupt_resume_bookkeeping_reset(cpu->pc);
                g_dirty_safe_resume_pc = 0;
                /* g_async_rfe_resume_pc (dirty_ram_interp.c, "Tomba 2 frame-1997
                 * fix") is explicitly documented there as PERSISTENT — unlike
                 * g_dirty_safe_resume_pc it survives across the gap between an
                 * interrupt and the game's later ReturnFromException, by design,
                 * for games whose handler drives RFE asynchronously. That same
                 * persistence makes it exactly the ISSUES.md #10 bug shape: it
                 * lives outside CPUState/RAM, so a load leaves it holding
                 * whatever it was in the pre-load LIVE session — not even
                 * something the save captured. If the live session happened to
                 * have a pending async RFE at the moment F1 was pressed, the
                 * resumed guest's next sentinel-gated dispatch redirects to that
                 * stale, no-longer-meaningful address instead of resolving
                 * normally, and keeps re-landing there every time the sentinel
                 * fires — an infinite (or very long) loop with completely normal
                 * per-frame timing, which is why it presented as a "freeze that
                 * gets worse" rather than a crash: frame pacing/presentation
                 * never stalls, only guest logical progress does. Zero means "no
                 * async RFE pending", the same state a cold boot starts in. */
                g_async_rfe_resume_pc = 0;
            }
            /* MULT/DIV and GTE completion-stall deadlines (cpu->muldiv_ts_done /
             * cpu->gte_ts_done — absolute guest-cycle numbers, psx_cycles.c) are
             * fields on CPUState but write_cpu_section() never serializes them
             * (BS_SEC_CPU only covers gpr/pc/hi/lo/cop0/gte_data/gte_ctrl), so
             * boot_state_load leaves them holding whatever they were in the
             * pre-load live session — almost always far ABOVE the freshly
             * restored (lower) psx_cycle_count. The first MFLO/MFHI or GTE op
             * after resume sees "deadline > now" and stalls for the entire
             * stale gap via psx_advance_cycles(), which psx_devices_service_to_now()
             * then has to replay event-by-event: a freeze that gets worse the
             * longer the game ran before this particular load (measured:
             * resume-to-first-check grew ~180ms per 10s of prior runtime).
             * Zero means "nothing pending", the same state a cold boot starts
             * in. */
            cpu->muldiv_ts_done = 0;
            cpu->gte_ts_done    = 0;
            s_load_cooldown_until_frame =
                s_frame_count + SAVESTATE_LOAD_COOLDOWN_FRAMES;
            s_load_cooldown_notice = 0;
            fprintf(stderr, "savestate: LOADED slot %d -> resuming pc=0x%08X\n",
                    slot, (unsigned)cpu->pc);
            /* Netplay post-load barrier observes this before the longjmp. */
            s_load_completed = 1;
            /* Restage FBO/present latch so the restored frame is visible
             * immediately (avoids disabled-display blank latch + stale smooth). */
            psx_frontend_on_savestate_loaded();
            /* Bounded grace window (ISSUES.md #10): if the resumed guest walks
             * into an undiscovered dispatch target within the next few thousand
             * dispatch checks, survive it loudly instead of crashing the whole
             * process. See psx_post_load_grace_arm in traps.c. */
            {
                extern void psx_post_load_grace_arm(void);
                psx_post_load_grace_arm();
            }
            /* Unwind to the scheduler and re-dispatch the restored PC. Never
             * returns; abandons the suspended CPS frames on the current stack. */
            psx_scheduler_resume_at(cpu->pc);
        } else {
            fprintf(stderr, "savestate: LOAD FAILED slot %d (missing/mismatched) %s\n",
                    slot, path);
        }
    }
}
