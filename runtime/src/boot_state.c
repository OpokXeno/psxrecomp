#include "boot_state.h"
#include "game_identity.h"
#include "overlay_api.h"   /* PSX_OVERLAY_CODEGEN_HASH / _ABI_TAG / _CODEGEN_VER */
#include "dirty_ram_interp.h"
#include "gpu.h"           /* gpu_get_vram — CPU-auth mirror under dual-raster   */
#include "gpu_render.h"    /* gr_vram_transfer_in / gr_vram_transfer_out          */
#include "gpu_vram_dirty.h"
#include "cpu_state.h"     /* gte_canonicalize_cpu_state after CPU wire restore   */
#include "interrupts.h"
#include "memory.h"
#include "psx_cycles.h"
#include "psx_icache.h"    /* g_psx_icache_tv — fetch-cost tags in BS_SEC_ICACHE */
#include "pst_wire.h"
#include "ram_provenance.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#if defined(_WIN32)
#  include <windows.h>
#else
#  include <time.h>
#endif

static double boot_state_mono_ms(void) {
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
#endif
}

/* Compress payloads at/above this size (RAM/VRAM/SPU/dirty dominate I/O). */
#define BOOT_STATE_ZLIB_MIN 256u

#define SPAD_SIZE  (1024u)
#define VRAM_W     1024
#define VRAM_H     512
#define VRAM_SIZE  ((uint32_t)(VRAM_W * VRAM_H * 2))  /* 1 MB, 16bpp */

/* ---- core accessors (existing runtime modules) ---- */
extern uint32_t  i_stat;
extern uint32_t  i_mask;
extern uint64_t  psx_cycle_count;
extern void timers_get_snapshot(uint16_t counter[3], uint32_t mode[3],
                                uint16_t target[3], int32_t irq_line[3],
                                uint32_t frac[3]);
extern void timers_set_snapshot(const uint16_t counter[3], const uint32_t mode[3],
                                const uint16_t target[3], const int32_t irq_line[3],
                                const uint32_t frac[3]);

/* ---- per-subsystem complete-state accessors (defined in each module) ---- */
extern uint32_t gpu_snapshot_bytes(void);
extern void     gpu_snapshot_write(uint8_t* p);
extern int      gpu_snapshot_read(const uint8_t* p, uint32_t len);
extern uint32_t spu_snapshot_bytes(void);
extern void     spu_snapshot_write(uint8_t* p);
extern int      spu_snapshot_read(const uint8_t* p, uint32_t len);
extern uint32_t spu_get_ram_bytes(void);
extern void     spu_ram_copy_out(uint8_t* out, uint32_t len);
extern int      spu_ram_copy_in(const uint8_t* in, uint32_t len);
extern uint32_t cdrom_snapshot_bytes(void);
extern void     cdrom_snapshot_write(uint8_t* p);
extern int      cdrom_snapshot_read(const uint8_t* p, uint32_t len);
extern uint32_t dma_snapshot_bytes(void);
extern void     dma_snapshot_write(uint8_t* p);
extern int      dma_snapshot_read(const uint8_t* p, uint32_t len);
extern uint32_t sio_snapshot_bytes(void);
extern void     sio_snapshot_write(uint8_t* p);
extern int      sio_snapshot_read(const uint8_t* p, uint32_t len);
extern uint32_t mdec_snapshot_bytes(void);
extern void     mdec_snapshot_write(uint8_t* p);
extern int      mdec_snapshot_read(const uint8_t* p, uint32_t len);

/* CPU regs wire: 32+3+32+32+32 LE u32 = 131 * 4 = 524 bytes (no padding). */
#define CPU_REGS_WIRE_BYTES (524u)
/* Timer wire: 3*u16 + 3*u32 + 3*u16 + 3*i32 + 3*u32 = 48 bytes (no pad holes). */
#define TIMER_REGS_WIRE_BYTES (48u)
#define BOOT_STATE_MAX_BYTES (128u * 1024u * 1024u)

static uint32_t active_ram_profile(void)
{
    return memory_developer_ram_enabled()
        ? BOOT_STATE_RAM_PROFILE_DEVELOPER
        : BOOT_STATE_RAM_PROFILE_RETAIL;
}

/* ---- deferred capture state (armed before first boot, fired at handoff) ---- */
static char     s_capture_path[512];
static uint32_t s_capture_checksum;
static uint32_t s_capture_entry_pc;

/* §96: persistent VRAM mirror for raw ring snaps — patch dirty scanlines only. */
static uint16_t s_vram_mirror[VRAM_W * VRAM_H];
static int      s_vram_mirror_valid;
static uint32_t s_last_vram_dirty_rows;
static int      s_last_vram_incremental;
static BootStateNativeCheckpointHooks s_native_checkpoint_hooks;
static int (*s_save_service_hook)(void);
static int s_save_service_busy;
static int (*s_save_consumer_perf_hook)(char *, int);
static struct {
    uint64_t attempts, deferred, started, succeeded, failed, bytes;
    uint64_t last_begin_cycle, last_end_cycle, last_deferred_cycle;
    uint32_t last_failed_section;
    double total_ms, max_ms, last_ms;
    uint64_t async_completed;
    double encode_total_ms, encode_max_ms;
    int active;
    double service_ms, clone_ms, worst_service_ms, worst_clone_ms;
} s_save_perf;

static int save_perf_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *env = getenv("PSX_SNAPSHOT_PERF");
        enabled = env && env[0] == '1';
    }
    return enabled;
}

void boot_state_set_save_consumer_perf_hook(int (*hook)(char *, int)) {
    s_save_consumer_perf_hook = hook;
}

int boot_state_save_perf_json(char *out, int capacity) {
    char consumer[1024] = "null";
    if (!out || capacity <= 0) return 0;
    if (s_save_consumer_perf_hook) {
        const int n = s_save_consumer_perf_hook(consumer, sizeof(consumer));
        if (n <= 0 || n >= (int)sizeof(consumer)) return 0;
    }
    return snprintf(out, (size_t)capacity,
        "{\"enabled\":%s,\"scope\":\"buffer-saves\","
        "\"timing_scope\":\"guest-capture; async encoding reported separately\","
        "\"attempts\":%llu,\"deferred\":%llu,\"started\":%llu,"
        "\"succeeded\":%llu,\"failed\":%llu,\"bytes\":%llu,"
        "\"last_begin_cycle\":%llu,\"last_end_cycle\":%llu,"
        "\"last_deferred_cycle\":%llu,\"last_failed_section\":%u,"
        "\"total_ms\":%.6f,\"max_ms\":%.6f,\"last_ms\":%.6f,"
        "\"async_completed\":%llu,\"encode_total_ms\":%.6f,\"encode_max_ms\":%.6f,"
        "\"last_service_ms\":%.6f,\"last_clone_ms\":%.6f,"
        "\"worst_capture_service_ms\":%.6f,\"worst_capture_clone_ms\":%.6f,\"consumer\":%s}",
        save_perf_enabled() ? "true" : "false",
        (unsigned long long)s_save_perf.attempts,
        (unsigned long long)s_save_perf.deferred,
        (unsigned long long)s_save_perf.started,
        (unsigned long long)s_save_perf.succeeded,
        (unsigned long long)s_save_perf.failed,
        (unsigned long long)s_save_perf.bytes,
        (unsigned long long)s_save_perf.last_begin_cycle,
        (unsigned long long)s_save_perf.last_end_cycle,
        (unsigned long long)s_save_perf.last_deferred_cycle,
        s_save_perf.last_failed_section,
        s_save_perf.total_ms, s_save_perf.max_ms, s_save_perf.last_ms,
        (unsigned long long)s_save_perf.async_completed,
        s_save_perf.encode_total_ms, s_save_perf.encode_max_ms,
        s_save_perf.service_ms, s_save_perf.clone_ms,
        s_save_perf.worst_service_ms, s_save_perf.worst_clone_ms, consumer);
}

static int snapshot_ready(void) {
    return !s_native_checkpoint_hooks.snapshot_ready ||
        s_native_checkpoint_hooks.snapshot_ready();
}
#if defined(PSX_BOOT_STATE_TEST_FAULT_INJECTION)
static int s_test_fail_after_device_apply;
#endif

void boot_state_set_native_checkpoint_hooks(
        const BootStateNativeCheckpointHooks *hooks)
{
    s_native_checkpoint_hooks = hooks != NULL
        ? *hooks : (BootStateNativeCheckpointHooks){0};
}

void boot_state_set_save_service_hook(int (*hook)(void))
{
    s_save_service_hook = hook;
}

static int save_service(void)
{
    int ok;
    if (s_save_service_busy) return 0;
    if (s_save_service_hook == NULL) return 1;
    s_save_service_busy = 1;
    const double begin_ms = s_save_perf.active ? boot_state_mono_ms() : 0.0;
    ok = s_save_service_hook();
    if (s_save_perf.active)
        s_save_perf.service_ms += boot_state_mono_ms() - begin_ms;
    s_save_service_busy = 0;
    return ok;
}

#if defined(PSX_BOOT_STATE_TEST_FAULT_INJECTION)
void boot_state_test_fail_after_device_apply_once(void)
{
    s_test_fail_after_device_apply = 1;
}
#endif

uint32_t boot_state_last_vram_dirty_rows(void)
{
    return s_last_vram_dirty_rows;
}

int boot_state_last_vram_incremental(void)
{
    return s_last_vram_incremental;
}

void boot_state_vram_mirror_reset(void)
{
    s_vram_mirror_valid = 0;
    s_last_vram_dirty_rows = VRAM_H;
    s_last_vram_incremental = 0;
}

static uint16_t *capture_pending_vram_upload(GpuPendingVramUpload *upload)
{
    uint16_t *pixels;
    const size_t pixel_count =
        gpu_pending_vram_upload_capture(upload, NULL, 0u);

    if (pixel_count == 0u)
        return NULL;
    pixels = (uint16_t *)malloc(pixel_count * sizeof(*pixels));
    if (pixels == NULL)
        return NULL;
    if (gpu_pending_vram_upload_capture(upload, pixels, pixel_count) !=
            pixel_count || upload->pixel_count != pixel_count) {
        free(pixels);
        return NULL;
    }
    return pixels;
}

static int restore_pending_vram_upload(
        const GpuPendingVramUpload *upload,
        const uint16_t *pixels,
        uint16_t *snapshot_vram)
{
    if (pixels == NULL)
        return upload->pixel_count == 0u;
    if (!gpu_pending_vram_upload_apply(
            upload, pixels, snapshot_vram, VRAM_W * VRAM_H))
        return 0;
    return gpu_pending_vram_upload_apply(
        upload, pixels, gpu_get_vram_ptr(), VRAM_W * VRAM_H);
}

/* Build s_vram_mirror from live CPU VRAM using dirty rows when possible.
 * Always emits a full 1 MiB BS_SEC_VRAM (loads stay independent).
 * Only used while gpu_vram_dirty_tracking() (rollback netplay). */
static int sync_vram_mirror_for_save(void)
{
    const uint16_t *live = gpu_get_vram();
    uint32_t dirty_n;
    uint32_t y;

    if (!gpu_vram_dirty_tracking()) {
        /* Should not be called offline — full refresh fallback. */
        if (live)
            memcpy(s_vram_mirror, live, VRAM_SIZE);
        else
            gr_vram_transfer_out(0, 0, VRAM_W, VRAM_H, s_vram_mirror);
        s_vram_mirror_valid = 1;
        s_last_vram_dirty_rows = VRAM_H;
        s_last_vram_incremental = 0;
        return 1;
    }

    dirty_n = gpu_vram_dirty_row_count();
    s_last_vram_dirty_rows = dirty_n;

    if (!live) {
        gr_vram_transfer_out(0, 0, VRAM_W, VRAM_H, s_vram_mirror);
        s_vram_mirror_valid = 1;
        s_last_vram_dirty_rows = VRAM_H;
        s_last_vram_incremental = 0;
        gpu_vram_dirty_clear();
        return 1;
    }

    if (!s_vram_mirror_valid || dirty_n >= VRAM_H) {
        memcpy(s_vram_mirror, live, VRAM_SIZE);
        s_vram_mirror_valid = 1;
        s_last_vram_incremental = 0;
    } else if (dirty_n == 0u) {
        /* Mirror already matches live. */
        s_last_vram_incremental = 1;
    } else {
        const uint64_t *mask = gpu_vram_dirty_mask();
        for (y = 0; y < VRAM_H; y++) {
            if (mask[y >> 6] & ((uint64_t)1u << (y & 63u))) {
                memcpy(s_vram_mirror + (size_t)y * VRAM_W,
                       live + (size_t)y * VRAM_W,
                       (size_t)VRAM_W * sizeof(uint16_t));
            }
        }
        s_last_vram_incremental = 1;
    }

    if (gpu_vram_dirty_verify_enabled()) {
        uint16_t *full = (uint16_t *)malloc(VRAM_SIZE);
        if (full) {
            GpuPendingVramUpload upload = {0};
            uint16_t *pending = capture_pending_vram_upload(&upload);

            if (upload.pixel_count != 0u && pending == NULL) {
                free(full);
                gpu_vram_dirty_clear();
                return 0;
            }
            gr_vram_transfer_out(0, 0, VRAM_W, VRAM_H, full);
            if (!restore_pending_vram_upload(&upload, pending, full)) {
                free(pending);
                free(full);
                gpu_vram_dirty_clear();
                return 0;
            }
            free(pending);
            if (memcmp(full, s_vram_mirror, VRAM_SIZE) != 0) {
                fprintf(stderr,
                        "psxrecomp: VRAM dirty VERIFY FAIL dirty_rows=%u "
                        "incr=%d — forcing full mirror\n",
                        (unsigned)dirty_n, s_last_vram_incremental);
                fflush(stderr);
                memcpy(s_vram_mirror, full, VRAM_SIZE);
                s_last_vram_incremental = 0;
                s_last_vram_dirty_rows = VRAM_H;
            }
            free(full);
        }
    }

    gpu_vram_dirty_clear();
    return 1;
}

/* File or growable memory sink — both save paths share one serializer. */
typedef struct BsOut {
    FILE*    f;       /* non-NULL => write to file */
    uint8_t* data;    /* memory sink (owned by caller / save_buffer) */
    size_t   len;
    size_t   cap;
    int      no_zlib; /* 1 => always raw sections (netplay snap ring) */
    int      no_service; /* Pure worker-side encoding; never pump guest/GL hooks. */
    uint32_t section; /* attempted wire section, for failed-save diagnostics */
    BootStateRawCapture *capture;
} BsOut;

struct BootStateRawCapture {
    uint8_t *data;
    uint8_t *staging;
    size_t staging_capacity;
    size_t len, provenance_offset;
    uint32_t provenance_capacity;
    RamProvenanceSnapshot *provenance;
    int profile, ok;
    double encode_ms;
};

static int bs_reserve(BsOut *o, size_t n) {
    if (n > SIZE_MAX - o->len) return 0;
    if (o->len + n > o->cap) {
        size_t nc = o->cap ? o->cap : (256u * 1024u);
        uint8_t* nd;
        while (nc < o->len + n) {
            if (nc > (SIZE_MAX / 2u)) return 0;
            nc *= 2u;
        }
        nd = (uint8_t*)realloc(o->data, nc);
        if (!nd) return 0;
        o->data = nd;
        o->cap = nc;
    }
    return 1;
}

static int bs_write(BsOut* o, const void* p, size_t n) {
    if (!n) return 1;
    if (!o->no_service && !save_service()) return 0;
    if (o->f)
        return fwrite(p, 1, n, o->f) == n && (o->no_service || save_service());
    if (!bs_reserve(o, n)) return 0;
    /* The guest stays stopped and p remains stable across service calls.
     * Keep the original single memcpy when no host service is installed. */
    while (n != 0u) {
        const size_t chunk = !o->no_service && s_save_service_hook != NULL && n > 256u * 1024u
            ? 256u * 1024u : n;
        memcpy(o->data + o->len, p, chunk);
        o->len += chunk;
        p = (const uint8_t *)p + chunk;
        n -= chunk;
        if (!o->no_service && !save_service()) return 0;
    }
    return 1;
}

static int write_header_le(BsOut* o, const BootStateHeader* h) {
    uint8_t buf[BOOT_STATE_HEADER_WIRE_BYTES];
    PstW w;
    pst_w_init(&w, buf, sizeof buf);
    if (!pst_w_u32(&w, h->magic) ||
        !pst_w_u32(&w, h->version) ||
        !pst_w_u32(&w, h->bios_checksum) ||
        !pst_w_u32(&w, h->entry_pc) ||
        !pst_w_u32(&w, h->codegen_hash) ||
        !pst_w_i32(&w, h->abi_tag) ||
        !pst_w_u32(&w, h->codegen_ver) ||
        !pst_w_u32(&w, h->section_count) ||
        !pst_w_u32(&w, h->ram_size) ||
        !pst_w_u32(&w, h->ram_profile) ||
        !pst_w_bytes(&w, h->game_sha256, sizeof(h->game_sha256)) ||
        !pst_w_bytes(&w, h->manifest_sha256, sizeof(h->manifest_sha256)) ||
        w.written != BOOT_STATE_HEADER_WIRE_BYTES)
        return 0;
    return bs_write(o, buf, sizeof buf);
}

static int write_section_raw(BsOut* o, uint32_t tag, uint32_t flags,
                             const void* data, uint64_t len) {
    uint8_t hdr[16];
    PstW w;
    pst_w_init(&w, hdr, sizeof hdr);
    if (!pst_w_u32(&w, tag) || !pst_w_u32(&w, flags) || !pst_w_u64(&w, len))
        return 0;
    if (!bs_write(o, hdr, sizeof hdr)) return 0;
    if (len && !bs_write(o, data, (size_t)len)) return 0;
    return 1;
}

/* Prefer zlib for large blobs (smaller disk + faster load on slow storage).
 * Falls back to raw if compressBound/compress fails.
 * o->no_zlib skips compress entirely (in-memory netplay ring). */
static int write_section(BsOut* o, uint32_t tag, const void* data, uint64_t len) {
    o->section = tag;
    if (!data && len) return 0;
    if (!o->no_zlib && len >= BOOT_STATE_ZLIB_MIN && len <= 0xffffffffu) {
        uLong bound = compressBound((uLong)len);
        uint8_t* packed = (uint8_t*)malloc(4u + (size_t)bound);
        if (packed) {
            PstW lw;
            uLong dest_len = bound;
            pst_w_init(&lw, packed, 4);
            if (pst_w_u32(&lw, (uint32_t)len) &&
                compress2(packed + 4, &dest_len, (const Bytef*)data, (uLong)len,
                          Z_BEST_SPEED) == Z_OK) {
                uint64_t payload = 4u + (uint64_t)dest_len;
                if (payload < len) {
                    int ok = write_section_raw(o, tag, BOOT_STATE_SEC_ZLIB,
                                               packed, payload);
                    free(packed);
                    return ok;
                }
            }
            free(packed);
        }
    }
    return write_section_raw(o, tag, 0u, data, len);
}

static int boot_state_parse_header(const uint8_t *, size_t, BootStateHeader *);

int boot_state_compress_buffer(const uint8_t *data, size_t size,
                              uint8_t **out_data, size_t *out_size) {
    BootStateHeader header;
    BsOut out = {0};
    size_t offset = BOOT_STATE_HEADER_WIRE_BYTES;
    if (!out_data || !out_size) return 0;
    *out_data = NULL; *out_size = 0u;
    if (!boot_state_parse_header(data, size, &header) ||
        header.magic != BOOT_STATE_MAGIC || header.version != BOOT_STATE_VERSION ||
        header.section_count > (size - BOOT_STATE_HEADER_WIRE_BYTES) / 16u)
        return 0;
    /* Only immutable captured bytes are touched. The normal serializer's
     * service hook belongs to the guest/GL owner, not the rewind encoder. */
    out.no_service = 1;
    if (!bs_write(&out, data, BOOT_STATE_HEADER_WIRE_BYTES)) goto fail;
    for (uint32_t i = 0u; i < header.section_count; ++i) {
        uint32_t tag, flags;
        uint64_t length;
        PstR section;
        if (size - offset < 16u) goto fail;
        pst_r_init(&section, data + offset, 16u);
        if (!pst_r_u32(&section, &tag) || !pst_r_u32(&section, &flags) ||
            !pst_r_u64(&section, &length) || (flags & ~BOOT_STATE_SEC_ZLIB)) goto fail;
        offset += 16u;
        if (length > size - offset) goto fail;
        if (flags) {
            if (!write_section_raw(&out, tag, flags, data + offset, length)) goto fail;
        } else if (!write_section(&out, tag, data + offset, length)) goto fail;
        offset += (size_t)length;
    }
    if (offset != size) goto fail;
    /* Do not retain the serializer's geometric reserve in every rewind slot. */
    uint8_t *tight = realloc(out.data, out.len);
    *out_data = tight ? tight : out.data;
    *out_size = out.len;
    return 1;
fail:
    free(out.data);
    return 0;
}

static int write_module_section(BsOut* o, uint32_t tag,
                                uint32_t (*bytes)(void),
                                void (*write)(uint8_t*)) {
    o->section = tag;
    uint32_t n = bytes();
    uint8_t* buf = (uint8_t*)malloc(n ? n : 1);
    if (!buf) return 0;
    write(buf);
    int ok = write_section(o, tag, buf, n);
    free(buf);
    return ok;
}

static int write_native_checkpoint_section(BsOut *o)
{
    o->section = BS_SEC_NATIVE_RENDER;
    uint32_t size = 0u;
    uint8_t *checkpoint = NULL;
    int ok;

    if (s_native_checkpoint_hooks.snapshot_size != NULL)
        size = s_native_checkpoint_hooks.snapshot_size();
    if (size != 0u) {
        if (s_native_checkpoint_hooks.snapshot_write == NULL)
            return 0;
        checkpoint = (uint8_t *)malloc(size);
        if (checkpoint == NULL)
            return 0;
        if (!s_native_checkpoint_hooks.snapshot_write(checkpoint, size)) {
            free(checkpoint);
            return 0;
        }
    }
    ok = write_section(o, BS_SEC_NATIVE_RENDER, checkpoint, size);
    free(checkpoint);
    return ok;
}

static int write_ram_provenance_section(BsOut *o)
{
    o->section = BS_SEC_RAM_PROVENANCE;
    const int direct = !o->f && o->no_zlib;
    uint32_t size = direct ? ram_provenance_snapshot_capacity() :
        ram_provenance_snapshot_bytes();
    uint8_t *snapshot;
    int ok;

    if (size == 0u)
        return 0;
    if (o->capture) {
        BootStateRawCapture *capture = o->capture;
        if ((size_t)size > SIZE_MAX - 16u || !save_service() ||
            !bs_reserve(o, 16u + (size_t)size))
            return 0;
        const double begin_ms = s_save_perf.active ? boot_state_mono_ms() : 0.0;
        capture->provenance = ram_provenance_snapshot_clone(capture->provenance);
        if (s_save_perf.active)
            s_save_perf.clone_ms = boot_state_mono_ms() - begin_ms;
        if (!capture->provenance) return 0;
        capture->provenance_offset = o->len;
        capture->provenance_capacity = size;
        /* Reserve the original section position. Only the worker writes this
         * hole, validates the owned copy and compacts the following sections. */
        o->len += 16u + size;
        return save_service();
    }
    if (direct) {
        /* Raw snapshots already own a contiguous wire buffer. Encode directly
         * instead of allocating and copying another provenance-sized payload. */
        const size_t section_start = o->len;
        PstW wire;
        if ((size_t)size > SIZE_MAX - 16u || !save_service() ||
            !bs_reserve(o, 16u + (size_t)size) ||
            !ram_provenance_snapshot_capture(o->data + o->len + 16u, size, &size))
            return 0;
        pst_w_init(&wire, o->data + section_start, 16u);
        if (!pst_w_u32(&wire, BS_SEC_RAM_PROVENANCE) ||
            !pst_w_u32(&wire, 0u) || !pst_w_u64(&wire, size))
            return 0;
        o->len += 16u + size;
        return save_service();
    }
    snapshot = (uint8_t *)malloc(size);
    if (snapshot == NULL)
        return 0;
    ok = ram_provenance_snapshot_write(snapshot, size) &&
         write_section(o, BS_SEC_RAM_PROVENANCE, snapshot, size);
    free(snapshot);
    return ok;
}

static int write_cpu_section(BsOut* o, const CPUState* cpu) {
    uint8_t buf[CPU_REGS_WIRE_BYTES];
    PstW w;
    pst_w_init(&w, buf, sizeof buf);
    for (int i = 0; i < 32; i++)
        if (!pst_w_u32(&w, cpu->gpr[i])) return 0;
    if (!pst_w_u32(&w, cpu->pc) || !pst_w_u32(&w, cpu->hi) || !pst_w_u32(&w, cpu->lo))
        return 0;
    for (int i = 0; i < 32; i++)
        if (!pst_w_u32(&w, cpu->cop0[i])) return 0;
    for (int i = 0; i < 32; i++)
        if (!pst_w_u32(&w, cpu->gte_data[i])) return 0;
    for (int i = 0; i < 32; i++)
        if (!pst_w_u32(&w, cpu->gte_ctrl[i])) return 0;
    if (w.written != CPU_REGS_WIRE_BYTES) return 0;
    return write_section(o, BS_SEC_CPU, buf, CPU_REGS_WIRE_BYTES);
}

static int write_timer_section(BsOut* o) {
    uint16_t counter[3], target[3];
    uint32_t mode[3], frac[3];
    int32_t irq_line[3];
    uint8_t buf[TIMER_REGS_WIRE_BYTES];
    PstW w;
    timers_get_snapshot(counter, mode, target, irq_line, frac);
    pst_w_init(&w, buf, sizeof buf);
    for (int i = 0; i < 3; i++)
        if (!pst_w_u16(&w, counter[i])) return 0;
    for (int i = 0; i < 3; i++)
        if (!pst_w_u32(&w, mode[i])) return 0;
    for (int i = 0; i < 3; i++)
        if (!pst_w_u16(&w, target[i])) return 0;
    for (int i = 0; i < 3; i++)
        if (!pst_w_i32(&w, irq_line[i])) return 0;
    for (int i = 0; i < 3; i++)
        if (!pst_w_u32(&w, frac[i])) return 0;
    if (w.written != TIMER_REGS_WIRE_BYTES) return 0;
    return write_section(o, BS_SEC_TIMER, buf, TIMER_REGS_WIRE_BYTES);
}

/* ============================ SAVE ============================ */

/* Classic full VRAM section (offline / zlib / tracking off). */
static int write_vram_section_full(BsOut *o)
{
    o->section = BS_SEC_VRAM;
    uint16_t *vbuf = (uint16_t *)malloc(VRAM_SIZE);
    GpuPendingVramUpload upload = {0};
    uint16_t *pending;
    int ok;
    if (!vbuf)
        return 0;
    pending = capture_pending_vram_upload(&upload);
    if (upload.pixel_count != 0u && pending == NULL) {
        free(vbuf);
        return 0;
    }
    if (!save_service()) {
        free(pending);
        free(vbuf);
        return 0;
    }
    gr_vram_transfer_out(0, 0, VRAM_W, VRAM_H, vbuf);
    if (!restore_pending_vram_upload(&upload, pending, vbuf)) {
        free(pending);
        free(vbuf);
        return 0;
    }
    free(pending);
    s_last_vram_dirty_rows = VRAM_H;
    s_last_vram_incremental = 0;
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    ok = write_section(o, BS_SEC_VRAM, vbuf, VRAM_SIZE);
#else
    {
        uint8_t *wire = (uint8_t *)malloc(VRAM_SIZE);
        if (!wire) {
            free(vbuf);
            return 0;
        }
        {
            PstW w;
            pst_w_init(&w, wire, VRAM_SIZE);
            ok = pst_w_pod(&w, vbuf, VRAM_SIZE, 2) &&
                 write_section(o, BS_SEC_VRAM, wire, VRAM_SIZE);
        }
        free(wire);
    }
#endif
    free(vbuf);
    return ok;
}

static int boot_state_save_to(BsOut* o, const CPUState* cpu,
                              uint32_t bios_checksum, uint32_t entry_pc) {
    BootStateHeader h;
    int ok;
    memset(&h, 0, sizeof h);
    h.magic         = BOOT_STATE_MAGIC;
    h.version       = BOOT_STATE_VERSION;
    h.bios_checksum = bios_checksum;
    h.entry_pc      = entry_pc;
    h.codegen_hash  = (uint32_t)PSX_OVERLAY_CODEGEN_HASH;
    h.abi_tag       = (int32_t)PSX_OVERLAY_ABI_TAG;
    h.codegen_ver   = (uint32_t)PSX_OVERLAY_CODEGEN_VER;
    h.ram_size      = memory_get_ram_size();
    h.ram_profile   = active_ram_profile();
    if ((h.ram_profile == BOOT_STATE_RAM_PROFILE_RETAIL &&
         h.ram_size != PSX_MAIN_RAM_RETAIL_SIZE) ||
        (h.ram_profile == BOOT_STATE_RAM_PROFILE_DEVELOPER &&
         h.ram_size != PSX_MAIN_RAM_DEVELOPER_SIZE))
        return 0;
    const PsxGameIdentity *identity = psx_game_identity_runtime();
    if (!identity) return 0;
    memcpy(h.game_sha256, identity->game_sha256, sizeof(h.game_sha256));
    memcpy(h.manifest_sha256, identity->manifest_sha256, sizeof(h.manifest_sha256));
    h.section_count = 18;

    ok = write_header_le(o, &h);

    if (ok) ok = write_cpu_section(o, cpu);
    if (ok) ok = write_section(o, BS_SEC_RAM,  memory_get_ram_ptr(), h.ram_size);
    if (ok) ok = write_ram_provenance_section(o);
    if (ok) ok = write_section(o, BS_SEC_SPAD, memory_get_scratchpad_ptr(), SPAD_SIZE);
    if (ok) {
        /* 12B: i_stat, i_mask, cycles_since_vblank. Zeroing csv on warm load
         * rebased every tip to phase 0 and forked MotK wait-loop resim
         * (IRQ at CD54 vs CDA0). Selfcheck already restored csv out-of-band. */
        uint8_t irq[12];
        PstW w;
        pst_w_init(&w, irq, sizeof irq);
        ok = pst_w_u32(&w, i_stat) && pst_w_u32(&w, i_mask) &&
             pst_w_u32(&w, interrupts_get_cycles_since_vblank()) &&
             write_section(o, BS_SEC_IRQ, irq, 12);
    }
    if (ok) ok = write_timer_section(o);
    if (ok) {
        uint8_t cyc[8];
        PstW w;
        pst_w_init(&w, cyc, sizeof cyc);
        /* Publish deferred load-charge batch before snapshotting the clock. */
        psx_cyc_batch_flush();
        ok = pst_w_u64(&w, psx_cycle_count) &&
             write_section(o, BS_SEC_CLOCK, cyc, 8);
    }
    if (ok) ok = write_module_section(o, BS_SEC_GPU, gpu_snapshot_bytes, gpu_snapshot_write);
    if (ok) {
        o->section = BS_SEC_VRAM;
        /* §96 incremental mirror only while RB dirty-tracking is on.
         * Offline / delay-sync / zlib disk: classic full transfer_out. */
        if (o->no_zlib && gpu_vram_dirty_tracking()) {
            ok = sync_vram_mirror_for_save();
            if (ok) {
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
                ok = write_section(o, BS_SEC_VRAM, s_vram_mirror, VRAM_SIZE);
#else
                {
                    uint8_t* wire = (uint8_t*)malloc(VRAM_SIZE);
                    if (!wire) ok = 0;
                    else {
                        PstW w;
                        pst_w_init(&w, wire, VRAM_SIZE);
                        ok = pst_w_pod(&w, s_vram_mirror, VRAM_SIZE, 2) &&
                             write_section(o, BS_SEC_VRAM, wire, VRAM_SIZE);
                        free(wire);
                    }
                }
#endif
            }
        } else {
            ok = write_vram_section_full(o);
        }
    }
    if (ok) ok = write_native_checkpoint_section(o);
    if (ok) ok = write_module_section(o, BS_SEC_SPU, spu_snapshot_bytes, spu_snapshot_write);
    if (ok) {
        o->section = BS_SEC_SPURAM;
        const uint32_t bytes = spu_get_ram_bytes();
        uint8_t* copy = (uint8_t*)malloc(bytes);
        if (!copy) ok = 0;
        else {
            spu_ram_copy_out(copy, bytes);
            ok = write_section(o, BS_SEC_SPURAM, copy, bytes);
            free(copy);
        }
    }
    if (ok) ok = write_module_section(o, BS_SEC_CDROM, cdrom_snapshot_bytes, cdrom_snapshot_write);
    if (ok) ok = write_module_section(o, BS_SEC_DMA,   dma_snapshot_bytes,   dma_snapshot_write);
    if (ok) ok = write_module_section(o, BS_SEC_SIO,   sio_snapshot_bytes,   sio_snapshot_write);
    if (ok) ok = write_module_section(o, BS_SEC_MDEC,  mdec_snapshot_bytes,  mdec_snapshot_write);
    if (ok) {
        /* I-cache tags: warm loads must replay with the fetch-cost state the
         * live timeline had, or miss cycles differ per peer/retry and IRQ
         * delivery forks a few wait-loop iterations (MotK abort@940). */
        o->section = BS_SEC_ICACHE;
        uint8_t ib[1024u * 4u];
        PstW w;
        pst_w_init(&w, ib, sizeof ib);
        ok = 1;
        for (uint32_t i = 0; ok && i < 1024u; i++)
            ok = pst_w_u32(&w, g_psx_icache_tv[i]);
        if (ok) ok = write_section(o, BS_SEC_ICACHE, ib, sizeof ib);
    }
    if (ok) {
        uint32_t wc = dirty_ram_get_bitmap_word_count();
        o->section = BS_SEC_DIRTY;
        uint64_t nbytes = (uint64_t)wc * 4u;
        uint8_t* db = (uint8_t*)malloc(nbytes ? (size_t)nbytes : 1);
        if (!db) ok = 0;
        else {
            PstW w;
            pst_w_init(&w, db, (size_t)nbytes);
            ok = 1;
            for (uint32_t i = 0; ok && i < wc; i++)
                ok = pst_w_u32(&w, dirty_ram_get_bitmap_word(i));
            if (ok) ok = write_section(o, BS_SEC_DIRTY, db, nbytes);
            free(db);
        }
    }
    return ok;
}

int boot_state_save(const CPUState* cpu, uint32_t bios_checksum,
                     uint32_t entry_pc, const char* path) {
    BsOut o;
    if (s_save_service_busy || !snapshot_ready()) return 0;
    FILE* f = fopen(path, "wb");
    int ok;
    if (!f) return 0;
    memset(&o, 0, sizeof o);
    o.f = f;
    ok = boot_state_save_to(&o, cpu, bios_checksum, entry_pc);
    fclose(f);
    if (!ok)
        remove(path);
    return ok;
}

static int boot_state_save_buffer_ex(const CPUState* cpu, uint32_t bios_checksum,
                                     uint32_t entry_pc, uint8_t** out_data,
                                     size_t* out_len, int no_zlib,
                                     BootStateRawCapture *capture) {
    BsOut o;
    if (s_save_service_busy || !out_data || !out_len) return 0;
    *out_data = NULL;
    *out_len = 0;
    const int profile = save_perf_enabled();
    if (profile) s_save_perf.attempts++;
    if (!snapshot_ready()) {
        if (profile) {
            s_save_perf.deferred++;
            s_save_perf.last_deferred_cycle = psx_cycle_count;
        }
        return 0;
    }
    const double begin_ms = profile ? boot_state_mono_ms() : 0.0;
    if (profile) {
        s_save_perf.started++;
        s_save_perf.last_begin_cycle = psx_cycle_count;
        s_save_perf.active = 1;
        s_save_perf.service_ms = s_save_perf.clone_ms = 0.0;
    }
    memset(&o, 0, sizeof o);
    o.no_zlib = no_zlib ? 1 : 0;
    o.capture = capture;
    if (capture) capture->profile = profile;
    /* Include provenance before copying RAM, avoiding a grow/copy of the RAM
     * prefix on every raw save. Further growth still handles Native payloads. */
    o.cap = no_zlib ? (size_t)memory_get_ram_size() + (3u * 1024u * 1024u)
                    : (2u * 1024u * 1024u);
    if (no_zlib) {
        const uint32_t provenance_capacity = ram_provenance_snapshot_capacity();
        if (provenance_capacity <= SIZE_MAX - o.cap)
            o.cap += provenance_capacity;
    }
    if (capture) {
        if (capture->staging_capacity < o.cap) {
            uint8_t *grown = realloc(capture->staging, o.cap);
            if (!grown) return 0;
            capture->staging = grown;
            capture->staging_capacity = o.cap;
        }
        o.data = capture->staging;
        o.cap = capture->staging_capacity;
    } else {
        o.data = (uint8_t*)malloc(o.cap);
    }
    const int ok = o.data && boot_state_save_to(&o, cpu, bios_checksum, entry_pc);
    if (capture) {
        capture->staging = o.data;
        capture->staging_capacity = o.cap;
    }
    if (profile) {
        s_save_perf.active = 0;
        s_save_perf.last_ms = boot_state_mono_ms() - begin_ms;
        s_save_perf.total_ms += s_save_perf.last_ms;
        if (s_save_perf.last_ms > s_save_perf.max_ms) {
            s_save_perf.max_ms = s_save_perf.last_ms;
            s_save_perf.worst_service_ms = s_save_perf.service_ms;
            s_save_perf.worst_clone_ms = s_save_perf.clone_ms;
        }
        s_save_perf.last_end_cycle = psx_cycle_count;
        if (ok && !capture) {
            s_save_perf.succeeded++;
            s_save_perf.bytes += o.len;
        } else if (!ok) {
            s_save_perf.failed++;
            s_save_perf.last_failed_section = o.section;
        }
    }
    if (!ok) {
        if (!capture) free(o.data);
        return 0;
    }
    *out_data = o.data;
    *out_len = o.len;
    return 1;
}

int boot_state_save_buffer(const CPUState* cpu, uint32_t bios_checksum,
                           uint32_t entry_pc, uint8_t** out_data,
                           size_t* out_len) {
    return boot_state_save_buffer_ex(cpu, bios_checksum, entry_pc, out_data,
                                     out_len, 0, NULL);
}

int boot_state_save_buffer_raw(const CPUState* cpu, uint32_t bios_checksum,
                               uint32_t entry_pc, uint8_t** out_data,
                               size_t* out_len) {
    return boot_state_save_buffer_ex(cpu, bios_checksum, entry_pc, out_data,
                                     out_len, 1, NULL);
}

BootStateRawCapture *boot_state_prepare_raw(void) {
    BootStateRawCapture *capture=calloc(1,sizeof(*capture));
    if(!capture)return NULL;
    capture->provenance=ram_provenance_snapshot_prepare();
    if(!capture->provenance){free(capture);return NULL;}
    return capture;
}

BootStateRawCapture *boot_state_capture_raw(const CPUState *cpu,
                                           uint32_t bios_checksum,
                                           uint32_t entry_pc,
                                           BootStateRawCapture *reuse) {
    BootStateRawCapture *capture = reuse ? reuse :
        (BootStateRawCapture *)calloc(1u, sizeof(*capture));
    if (!capture) return NULL;
    capture->ok = 0;
    capture->encode_ms = 0.0;
    if (!boot_state_save_buffer_ex(cpu, bios_checksum, entry_pc,
            &capture->data, &capture->len, 1, capture)) {
        boot_state_free_raw(capture);
        return NULL;
    }
    return capture;
}

void boot_state_encode_raw(BootStateRawCapture *capture) {
    const double begin_ms = capture->profile ? boot_state_mono_ms() : 0.0;
    uint32_t size = 0u;
    uint8_t *section = capture->data + capture->provenance_offset;
    PstW wire;
    capture->ok = ram_provenance_snapshot_encode(capture->provenance,
        section + 16u, capture->provenance_capacity, &size);
    if (capture->ok) {
        const size_t tail = capture->provenance_offset + 16u +
            capture->provenance_capacity;
        pst_w_init(&wire, section, 16u);
        capture->ok = pst_w_u32(&wire, BS_SEC_RAM_PROVENANCE) &&
            pst_w_u32(&wire, 0u) && pst_w_u64(&wire, size);
        if (size != capture->provenance_capacity)
            memmove(section + 16u + size, capture->data + tail,
                    capture->len - tail);
        capture->len -= capture->provenance_capacity - size;
    }
    if (capture->ok) {
        uint8_t *packed = NULL;
        size_t packed_size = 0u;
        if (boot_state_compress_buffer(capture->data, capture->len, &packed, &packed_size)) {
            /* Keep the large raw workspace in the encoder, rather than
             * feeding a new ~25 MiB allocation through malloc each capture. */
            if (capture->data != capture->staging) free(capture->data);
            capture->data = packed;
            capture->len = packed_size;
        }
        /* A compression allocation failure still leaves a complete raw state.
         * Capturing/decompressing it uses the same checked section format. */
    }
    if (capture->profile)
        capture->encode_ms = boot_state_mono_ms() - begin_ms;
}

int boot_state_finish_raw(BootStateRawCapture *capture,
                          uint8_t **out_data, size_t *out_len,
                          BootStateRawCapture **out_reuse) {
    const int ok = capture->ok;
    *out_data = NULL;
    *out_len = 0u;
    if (capture->profile) {
        s_save_perf.async_completed++;
        s_save_perf.encode_total_ms += capture->encode_ms;
        if (capture->encode_ms > s_save_perf.encode_max_ms)
            s_save_perf.encode_max_ms = capture->encode_ms;
        if (ok) {
            s_save_perf.succeeded++;
            s_save_perf.bytes += capture->len;
        } else {
            s_save_perf.failed++;
            s_save_perf.last_failed_section = BS_SEC_RAM_PROVENANCE;
        }
    }
    if (ok) {
        *out_data = capture->data;
        *out_len = capture->len;
        if (capture->data == capture->staging) {
            /* Raw fallback transfers ownership to the ring. */
            capture->staging = NULL;
            capture->staging_capacity = 0u;
        }
    } else {
        if (capture->data != capture->staging) free(capture->data);
    }
    capture->data = NULL;
    capture->len = 0u;
    *out_reuse = capture;
    return ok;
}

void boot_state_free_raw(BootStateRawCapture *capture) {
    if (!capture) return;
    if (capture->data != capture->staging) free(capture->data);
    free(capture->staging);
    ram_provenance_snapshot_free(capture->provenance);
    free(capture);
}

/* ============================ LOAD ============================ */

static int apply_section(uint32_t tag, const uint8_t* p, uint32_t len,
                         CPUState* cpu, uint32_t entry_pc) {
    switch (tag) {
    case BS_SEC_CPU: {
        PstR r;
        if (len != CPU_REGS_WIRE_BYTES) return 0;
        pst_r_init(&r, p, len);
        for (int i = 0; i < 32; i++)
            if (!pst_r_u32(&r, &cpu->gpr[i])) return 0;
        if (!pst_r_u32(&r, &cpu->pc) || !pst_r_u32(&r, &cpu->hi) ||
            !pst_r_u32(&r, &cpu->lo))
            return 0;
        (void)entry_pc;
        for (int i = 0; i < 32; i++)
            if (!pst_r_u32(&r, &cpu->cop0[i])) return 0;
        for (int i = 0; i < 32; i++)
            if (!pst_r_u32(&r, &cpu->gte_data[i])) return 0;
        for (int i = 0; i < 32; i++)
            if (!pst_r_u32(&r, &cpu->gte_ctrl[i])) return 0;
        /* Architectural normalize + drop host-only projection provenance that
         * belonged to the pre-load timeline (not part of the wire format). */
        gte_canonicalize_cpu_state(cpu);
        return 1;
    }
    case BS_SEC_RAM:
        if (len != memory_get_ram_size()) return 0;
        memcpy(memory_get_ram_ptr(), p, len);
        {
            extern void psx_kernel_bless_note_range(uint32_t phys, uint32_t l);
            psx_kernel_bless_note_range(0, len);
        }
        return 1;
    case BS_SEC_SPAD:
        if (len != SPAD_SIZE) return 0;
        memcpy(memory_get_scratchpad_ptr(), p, SPAD_SIZE);
        return 1;
    case BS_SEC_IRQ: {
        PstR r;
        uint32_t st, mk, csv;
        if (len != 8 && len != 12) return 0;
        pst_r_init(&r, p, len);
        if (!pst_r_u32(&r, &st) || !pst_r_u32(&r, &mk)) return 0;
        i_stat = st;
        i_mask = mk;
        if (len == 12) {
            if (!pst_r_u32(&r, &csv)) return 0;
            interrupts_set_cycles_since_vblank(csv);
        } else {
            /* Legacy UI/disk snaps: no phase — rebase like pre-csv saves. */
            interrupts_set_cycles_since_vblank(0);
        }
        return 1;
    }
    case BS_SEC_TIMER: {
        uint16_t counter[3], target[3];
        uint32_t mode[3], frac[3];
        int32_t irq_line[3];
        PstR r;
        if (len != TIMER_REGS_WIRE_BYTES) return 0;
        pst_r_init(&r, p, len);
        for (int i = 0; i < 3; i++)
            if (!pst_r_u16(&r, &counter[i])) return 0;
        for (int i = 0; i < 3; i++)
            if (!pst_r_u32(&r, &mode[i])) return 0;
        for (int i = 0; i < 3; i++)
            if (!pst_r_u16(&r, &target[i])) return 0;
        for (int i = 0; i < 3; i++)
            if (!pst_r_i32(&r, &irq_line[i])) return 0;
        for (int i = 0; i < 3; i++)
            if (!pst_r_u32(&r, &frac[i])) return 0;
        timers_set_snapshot(counter, mode, target, irq_line, frac);
        return 1;
    }
    case BS_SEC_CLOCK: {
        PstR r;
        uint64_t cyc;
        if (len != 8) return 0;
        pst_r_init(&r, p, len);
        if (!pst_r_u64(&r, &cyc)) return 0;
        psx_cycle_count = cyc;
        return 1;
    }
    case BS_SEC_GPU:
        return gpu_snapshot_read(p, len);
    case BS_SEC_VRAM: {
        if (len != VRAM_SIZE) return 0;
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
        /* Wire == host layout: upload straight from the section buffer. */
        gr_vram_transfer_in(0, 0, VRAM_W, VRAM_H, (const uint16_t*)p);
        if (gpu_vram_dirty_tracking()) {
            memcpy(s_vram_mirror, p, VRAM_SIZE);
            s_vram_mirror_valid = 1;
            gpu_vram_dirty_clear();
        } else {
            s_vram_mirror_valid = 0;
        }
        return 1;
#else
        {
            uint16_t* vbuf;
            PstR r;
            vbuf = (uint16_t*)malloc(VRAM_SIZE);
            if (!vbuf) return 0;
            pst_r_init(&r, p, len);
            if (!pst_r_pod(&r, vbuf, VRAM_SIZE, 2)) {
                free(vbuf);
                return 0;
            }
            gr_vram_transfer_in(0, 0, VRAM_W, VRAM_H, vbuf);
            if (gpu_vram_dirty_tracking()) {
                memcpy(s_vram_mirror, vbuf, VRAM_SIZE);
                s_vram_mirror_valid = 1;
                gpu_vram_dirty_clear();
            } else {
                s_vram_mirror_valid = 0;
            }
            free(vbuf);
            return 1;
        }
#endif
    }
    case BS_SEC_SPU:
        return spu_snapshot_read(p, len);
    case BS_SEC_SPURAM:
        if (len != spu_get_ram_bytes()) return 0;
        return spu_ram_copy_in(p, len);
    case BS_SEC_CDROM:
        return cdrom_snapshot_read(p, len);
    case BS_SEC_DMA:
        return dma_snapshot_read(p, len);
    case BS_SEC_SIO:
        return sio_snapshot_read(p, len);
    case BS_SEC_MDEC:
        return mdec_snapshot_read(p, len);
    case BS_SEC_DIRTY: {
        uint32_t wc, expected_wc;
        uint32_t* words;
        PstR r;
        if (len % 4u) return 0;
        wc = len / 4u;
        expected_wc = dirty_ram_get_bitmap_word_count();
        if (wc != expected_wc) return 0;
        words = (uint32_t*)malloc(len ? len : 1);
        if (!words) return 0;
        pst_r_init(&r, p, len);
        for (uint32_t i = 0; i < wc; i++) {
            if (!pst_r_u32(&r, &words[i])) {
                free(words);
                return 0;
            }
        }
        dirty_ram_set_bitmap_words(words, wc);
        free(words);
        return 1;
    }
    case BS_SEC_ICACHE: {
        PstR r;
        if (len != 1024u * 4u) return 0;
        pst_r_init(&r, p, len);
        for (uint32_t i = 0; i < 1024u; i++)
            if (!pst_r_u32(&r, &g_psx_icache_tv[i])) return 0;
        return 1;
    }
    default:
        return 0;
    }
}

static int boot_state_parse_header(const uint8_t* file, size_t file_len,
                                   BootStateHeader* h_out) {
    PstR hr;
    if (!file || !h_out || file_len < BOOT_STATE_HEADER_WIRE_BYTES ||
        file_len > BOOT_STATE_MAX_BYTES) {
        return 0;
    }
    pst_r_init(&hr, file, BOOT_STATE_HEADER_WIRE_BYTES);
    memset(h_out, 0, sizeof(*h_out));
    if (!pst_r_u32(&hr, &h_out->magic) ||
        !pst_r_u32(&hr, &h_out->version) ||
        !pst_r_u32(&hr, &h_out->bios_checksum) ||
        !pst_r_u32(&hr, &h_out->entry_pc) ||
        !pst_r_u32(&hr, &h_out->codegen_hash) ||
        !pst_r_i32(&hr, &h_out->abi_tag) ||
        !pst_r_u32(&hr, &h_out->codegen_ver) ||
        !pst_r_u32(&hr, &h_out->section_count) ||
        !pst_r_u32(&hr, &h_out->ram_size) ||
        !pst_r_u32(&hr, &h_out->ram_profile) ||
        !pst_r_bytes(&hr, h_out->game_sha256, sizeof(h_out->game_sha256)) ||
        !pst_r_bytes(&hr, h_out->manifest_sha256, sizeof(h_out->manifest_sha256))) {
        return 0;
    }
    return 1;
}

static void boot_state_append_reason(char* reason, size_t reason_cap,
                                     const char* part) {
    size_t n;
    if (!reason || reason_cap == 0 || !part || !part[0]) return;
    n = strlen(reason);
    if (n + 1 >= reason_cap) return;
    if (n > 0) {
        reason[n++] = ',';
        reason[n] = '\0';
        if (n + 1 >= reason_cap) return;
    }
    snprintf(reason + n, reason_cap - n, "%s", part);
}

int boot_state_check_buffer(const uint8_t* file, size_t file_len,
                            uint32_t bios_checksum, uint32_t entry_pc,
                            char* reason, size_t reason_cap) {
    BootStateHeader h;
    char part[96];

    const PsxGameIdentity *identity;

    if (reason && reason_cap)
        reason[0] = '\0';

    if (!file || file_len < BOOT_STATE_HEADER_WIRE_BYTES) {
        boot_state_append_reason(reason, reason_cap, "missing_or_truncated");
        return 0;
    }
    if (file_len > BOOT_STATE_MAX_BYTES) {
        boot_state_append_reason(reason, reason_cap, "too_large");
        return 0;
    }
    if (!boot_state_parse_header(file, file_len, &h)) {
        boot_state_append_reason(reason, reason_cap, "header_parse");
        return 0;
    }

    if (h.magic != BOOT_STATE_MAGIC) {
        snprintf(part, sizeof(part), "magic=%08X(want %08X)",
                 (unsigned)h.magic, (unsigned)BOOT_STATE_MAGIC);
        boot_state_append_reason(reason, reason_cap, part);
    }
    if (h.version < BOOT_STATE_VERSION_MIN_READ ||
        h.version > BOOT_STATE_VERSION) {
        snprintf(part, sizeof(part), "version=%u(want %u..%u)",
                 (unsigned)h.version, (unsigned)BOOT_STATE_VERSION_MIN_READ,
                 (unsigned)BOOT_STATE_VERSION);
        boot_state_append_reason(reason, reason_cap, part);
    }
    if (h.bios_checksum != bios_checksum) {
        snprintf(part, sizeof(part), "bios=%08X(want %08X)",
                 (unsigned)h.bios_checksum, (unsigned)bios_checksum);
        boot_state_append_reason(reason, reason_cap, part);
    }
    if (h.entry_pc != entry_pc) {
        snprintf(part, sizeof(part), "entry=%08X(want %08X)",
                 (unsigned)h.entry_pc, (unsigned)entry_pc);
        boot_state_append_reason(reason, reason_cap, part);
    }
    if (h.codegen_hash != (uint32_t)PSX_OVERLAY_CODEGEN_HASH) {
        snprintf(part, sizeof(part), "codegen_hash=%08X(want %08X)",
                 (unsigned)h.codegen_hash,
                 (unsigned)PSX_OVERLAY_CODEGEN_HASH);
        boot_state_append_reason(reason, reason_cap, part);
    }
    if (h.abi_tag != (int32_t)PSX_OVERLAY_ABI_TAG) {
        snprintf(part, sizeof(part), "abi_tag=%d(want %d)",
                 (int)h.abi_tag, (int)PSX_OVERLAY_ABI_TAG);
        boot_state_append_reason(reason, reason_cap, part);
    }
    if (h.codegen_ver != (uint32_t)PSX_OVERLAY_CODEGEN_VER) {
        snprintf(part, sizeof(part), "codegen_ver=%u(want %u)",
                 (unsigned)h.codegen_ver, (unsigned)PSX_OVERLAY_CODEGEN_VER);
        boot_state_append_reason(reason, reason_cap, part);
    }
    if (h.ram_size != memory_get_ram_size()) {
        snprintf(part, sizeof(part), "ram_size=%u(want %u)",
                 (unsigned)h.ram_size, (unsigned)memory_get_ram_size());
        boot_state_append_reason(reason, reason_cap, part);
    }
    if (h.ram_profile != active_ram_profile()) {
        snprintf(part, sizeof(part), "ram_profile=%u(want %u)",
                 (unsigned)h.ram_profile, (unsigned)active_ram_profile());
        boot_state_append_reason(reason, reason_cap, part);
    }
    identity = psx_game_identity_runtime();
    if (!identity) {
        boot_state_append_reason(reason, reason_cap, "game_identity_missing");
    } else {
        if (memcmp(h.game_sha256, identity->game_sha256,
                   sizeof(h.game_sha256)) != 0)
            boot_state_append_reason(reason, reason_cap, "game_sha256");
        if (memcmp(h.manifest_sha256, identity->manifest_sha256,
                   sizeof(h.manifest_sha256)) != 0)
            boot_state_append_reason(reason, reason_cap, "manifest_sha256");
    }

    if (reason && reason_cap && reason[0])
        return 0;
    return 1;
}

typedef struct BootStateParsedSection {
    uint32_t tag;
    uint32_t len;
    const uint8_t *data;
    uint8_t *owned;
} BootStateParsedSection;

static void boot_state_free_parsed_sections(BootStateParsedSection *sections,
                                            uint32_t count)
{
    for (uint32_t index = 0u; index < count; ++index)
        free(sections[index].owned);
}

typedef struct BootStateModuleSnapshot {
    uint8_t *data;
    uint32_t size;
} BootStateModuleSnapshot;

typedef struct BootStateRollback {
    uint8_t scratchpad[SPAD_SIZE];
    uint32_t irq_stat;
    uint32_t irq_mask;
    uint32_t cycles_since_vblank;
    uint16_t timer_counter[3];
    uint32_t timer_mode[3];
    uint16_t timer_target[3];
    int32_t timer_irq_line[3];
    uint32_t timer_frac[3];
    uint64_t cycle_count;
    BootStateModuleSnapshot gpu;
    BootStateModuleSnapshot spu;
    BootStateModuleSnapshot cdrom;
    BootStateModuleSnapshot dma;
    BootStateModuleSnapshot sio;
    BootStateModuleSnapshot mdec;
    uint16_t *vram;
    uint8_t *spu_ram;
    uint32_t spu_ram_size;
    uint32_t *dirty_words;
    uint32_t dirty_word_count;
    uint32_t icache[1024];
    uint16_t *vram_mirror;
    uint64_t vram_dirty_mask[GPU_VRAM_DIRTY_H / 64u];
    int vram_mirror_valid;
    uint32_t last_vram_dirty_rows;
    int last_vram_incremental;
} BootStateRollback;

static int boot_state_capture_module(
        BootStateModuleSnapshot *snapshot,
        uint32_t (*bytes)(void), void (*write)(uint8_t *))
{
    snapshot->size = bytes();
    snapshot->data = (uint8_t *)malloc(snapshot->size ? snapshot->size : 1u);
    if (snapshot->data == NULL)
        return 0;
    write(snapshot->data);
    return 1;
}

static void boot_state_free_rollback(BootStateRollback *rollback)
{
    free(rollback->gpu.data);
    free(rollback->spu.data);
    free(rollback->cdrom.data);
    free(rollback->dma.data);
    free(rollback->sio.data);
    free(rollback->mdec.data);
    free(rollback->vram);
    free(rollback->spu_ram);
    free(rollback->dirty_words);
    free(rollback->vram_mirror);
    memset(rollback, 0, sizeof(*rollback));
}

static int boot_state_capture_rollback(BootStateRollback *rollback)
{
    const uint16_t *live_vram;

    memset(rollback, 0, sizeof(*rollback));
    memcpy(rollback->scratchpad, memory_get_scratchpad_ptr(), SPAD_SIZE);
    rollback->irq_stat = i_stat;
    rollback->irq_mask = i_mask;
    rollback->cycles_since_vblank = interrupts_get_cycles_since_vblank();
    timers_get_snapshot(
        rollback->timer_counter, rollback->timer_mode,
        rollback->timer_target, rollback->timer_irq_line,
        rollback->timer_frac);
    rollback->cycle_count = psx_cycle_count;
    if (!boot_state_capture_module(
            &rollback->gpu, gpu_snapshot_bytes, gpu_snapshot_write) ||
        !boot_state_capture_module(
            &rollback->spu, spu_snapshot_bytes, spu_snapshot_write) ||
        !boot_state_capture_module(
            &rollback->cdrom, cdrom_snapshot_bytes, cdrom_snapshot_write) ||
        !boot_state_capture_module(
            &rollback->dma, dma_snapshot_bytes, dma_snapshot_write) ||
        !boot_state_capture_module(
            &rollback->sio, sio_snapshot_bytes, sio_snapshot_write) ||
        !boot_state_capture_module(
            &rollback->mdec, mdec_snapshot_bytes, mdec_snapshot_write))
        goto fail;

    rollback->vram = (uint16_t *)malloc(VRAM_SIZE);
    rollback->spu_ram_size = spu_get_ram_bytes();
    rollback->spu_ram = (uint8_t *)malloc(
        rollback->spu_ram_size ? rollback->spu_ram_size : 1u);
    rollback->dirty_word_count = dirty_ram_get_bitmap_word_count();
    rollback->dirty_words = (uint32_t *)malloc(
        rollback->dirty_word_count
            ? rollback->dirty_word_count * sizeof(*rollback->dirty_words)
            : 1u);
    rollback->vram_mirror = (uint16_t *)malloc(VRAM_SIZE);
    if (rollback->vram == NULL || rollback->spu_ram == NULL ||
        rollback->dirty_words == NULL || rollback->vram_mirror == NULL)
        goto fail;

    live_vram = gpu_get_vram();
    if (live_vram != NULL)
        memcpy(rollback->vram, live_vram, VRAM_SIZE);
    else
        gr_vram_transfer_out(0, 0, VRAM_W, VRAM_H, rollback->vram);
    spu_ram_copy_out(rollback->spu_ram, rollback->spu_ram_size);
    for (uint32_t index = 0u; index < rollback->dirty_word_count; ++index)
        rollback->dirty_words[index] = dirty_ram_get_bitmap_word(index);
    memcpy(rollback->icache, g_psx_icache_tv, sizeof(rollback->icache));
    memcpy(rollback->vram_mirror, s_vram_mirror, VRAM_SIZE);
    memcpy(rollback->vram_dirty_mask, gpu_vram_dirty_mask(),
           sizeof(rollback->vram_dirty_mask));
    rollback->vram_mirror_valid = s_vram_mirror_valid;
    rollback->last_vram_dirty_rows = s_last_vram_dirty_rows;
    rollback->last_vram_incremental = s_last_vram_incremental;
    return 1;

fail:
    boot_state_free_rollback(rollback);
    return 0;
}

static int boot_state_restore_rollback(const BootStateRollback *rollback)
{
    int ok = 1;

    memcpy(memory_get_scratchpad_ptr(), rollback->scratchpad, SPAD_SIZE);
    i_stat = rollback->irq_stat;
    i_mask = rollback->irq_mask;
    interrupts_set_cycles_since_vblank(rollback->cycles_since_vblank);
    timers_set_snapshot(
        rollback->timer_counter, rollback->timer_mode,
        rollback->timer_target, rollback->timer_irq_line,
        rollback->timer_frac);
    psx_cycle_count = rollback->cycle_count;
    ok &= gpu_snapshot_read(rollback->gpu.data, rollback->gpu.size);
    gr_vram_transfer_in(0, 0, VRAM_W, VRAM_H, rollback->vram);
    memcpy(gpu_get_vram_ptr(), rollback->vram, VRAM_SIZE);
    ok &= spu_snapshot_read(rollback->spu.data, rollback->spu.size);
    ok &= spu_ram_copy_in(rollback->spu_ram, rollback->spu_ram_size);
    ok &= cdrom_snapshot_read(rollback->cdrom.data, rollback->cdrom.size);
    ok &= dma_snapshot_read(rollback->dma.data, rollback->dma.size);
    ok &= sio_snapshot_read(rollback->sio.data, rollback->sio.size);
    ok &= mdec_snapshot_read(rollback->mdec.data, rollback->mdec.size);
    dirty_ram_set_bitmap_words(
        rollback->dirty_words, rollback->dirty_word_count);
    memcpy(g_psx_icache_tv, rollback->icache, sizeof(rollback->icache));
    memcpy(s_vram_mirror, rollback->vram_mirror, VRAM_SIZE);
    s_vram_mirror_valid = rollback->vram_mirror_valid;
    s_last_vram_dirty_rows = rollback->last_vram_dirty_rows;
    s_last_vram_incremental = rollback->last_vram_incremental;
    if (gpu_vram_dirty_tracking()) {
        gpu_vram_dirty_clear();
        for (uint32_t row = 0u; row < GPU_VRAM_DIRTY_H; ++row) {
            if (rollback->vram_dirty_mask[row >> 6u] &
                (UINT64_C(1) << (row & 63u)))
                gpu_vram_dirty_mark_row_impl(row);
        }
    }
    return ok;
}

int boot_state_load_buffer(const uint8_t* file, size_t file_len,
                           uint32_t bios_checksum, uint32_t entry_pc,
                           CPUState* cpu) {
    if (s_save_service_busy) return 0;
    const uint8_t* cur;
    const uint8_t* end;
    BootStateHeader h;
    BootStateParsedSection sections[32] = {{0}};
    BootStateParsedSection *ram_section = NULL;
    BootStateParsedSection *provenance_section = NULL;
    BootStateParsedSection *native_section = NULL;
    RamProvenanceSnapshot *provenance_snapshot = NULL;
    BootStateRollback rollback;
    void *prepared_native = NULL;
    CPUState staged_cpu;
    char reject[256];
    const uint32_t required =
        (1u<<BS_SEC_CPU)|(1u<<BS_SEC_RAM)|(1u<<BS_SEC_SPAD)|(1u<<BS_SEC_IRQ)|
        (1u<<BS_SEC_TIMER)|(1u<<BS_SEC_CLOCK)|(1u<<BS_SEC_GPU)|(1u<<BS_SEC_VRAM)|
        (1u<<BS_SEC_SPU)|(1u<<BS_SEC_SPURAM)|(1u<<BS_SEC_CDROM)|(1u<<BS_SEC_DMA)|
        (1u<<BS_SEC_SIO)|(1u<<BS_SEC_MDEC)|(1u<<BS_SEC_DIRTY)|(1u<<BS_SEC_ICACHE)|
        (1u<<BS_SEC_NATIVE_RENDER)|(1u<<BS_SEC_RAM_PROVENANCE);
    uint32_t seen = 0u;
    int ok = 1;
    const double t0 = boot_state_mono_ms();
    double inflate_ms = 0.0;
    double apply_ram_ms = 0.0;
    double apply_vram_ms = 0.0;
    double apply_spuram_ms = 0.0;
    double apply_other_ms = 0.0;

    reject[0] = '\0';
    if (cpu == NULL ||
        !boot_state_check_buffer(file, file_len, bios_checksum, entry_pc,
                                 reject, sizeof(reject))) {
        fprintf(stderr, "boot_state: reject — %s\n",
                reject[0] ? reject : "unknown");
        return 0;
    }
    if (!boot_state_parse_header(file, file_len, &h) ||
        h.section_count > (uint32_t)(sizeof(sections) / sizeof(sections[0])))
        return 0;

    cur = file + BOOT_STATE_HEADER_WIRE_BYTES;
    end = file + file_len;

    for (uint32_t index = 0u; ok && index < h.section_count; ++index) {
        BootStateParsedSection *section = &sections[index];
        PstR sh;
        uint32_t pad = 0u;
        uint64_t len = 0u;
        const uint8_t *payload;

        if ((size_t)(end - cur) < 16u) { ok = 0; break; }
        pst_r_init(&sh, cur, 16u);
        if (!pst_r_u32(&sh, &section->tag) || !pst_r_u32(&sh, &pad) ||
            !pst_r_u64(&sh, &len)) {
            ok = 0;
            break;
        }
        cur += 16u;
        if (section->tag == 0u || section->tag >= 32u ||
            (required & (1u << section->tag)) == 0u ||
            (seen & (1u << section->tag)) != 0u ||
            len > BOOT_STATE_MAX_BYTES || (uint64_t)(end - cur) < len) {
            ok = 0;
            break;
        }
        payload = cur;
        cur += (size_t)len;

        if (pad == BOOT_STATE_SEC_ZLIB) {
            PstR lr;
            uint32_t raw_len = 0u;
            uLong dest_len;
            double t_inf;

            if (len < 4u) { ok = 0; break; }
            pst_r_init(&lr, payload, 4u);
            if (!pst_r_u32(&lr, &raw_len) || raw_len == 0u ||
                raw_len > BOOT_STATE_MAX_BYTES) {
                ok = 0;
                break;
            }
            section->owned = (uint8_t *)malloc(raw_len);
            if (section->owned == NULL) { ok = 0; break; }
            dest_len = (uLong)raw_len;
            t_inf = boot_state_mono_ms();
            if (uncompress(section->owned, &dest_len, payload + 4u,
                           (uLong)(len - 4u)) != Z_OK ||
                dest_len != (uLong)raw_len) {
                ok = 0;
                break;
            }
            inflate_ms += boot_state_mono_ms() - t_inf;
            section->data = section->owned;
            section->len = raw_len;
        } else if (pad != 0u || len > UINT32_MAX) {
            ok = 0;
            break;
        } else {
            section->data = payload;
            section->len = (uint32_t)len;
        }
        seen |= 1u << section->tag;
    }

    if (!ok || cur != end || (seen & required) != required) {
        boot_state_free_parsed_sections(sections, h.section_count);
        return 0;
    }
    for (uint32_t index = 0u; index < h.section_count; ++index) {
        if (sections[index].tag == BS_SEC_RAM)
            ram_section = &sections[index];
        else if (sections[index].tag == BS_SEC_RAM_PROVENANCE)
            provenance_section = &sections[index];
        else if (sections[index].tag == BS_SEC_NATIVE_RENDER)
            native_section = &sections[index];
    }
    if (ram_section == NULL || ram_section->len != memory_get_ram_size() ||
        provenance_section == NULL || native_section == NULL ||
        !ram_provenance_snapshot_decode(
            provenance_section->data, provenance_section->len,
            ram_section->len, &provenance_snapshot)) {
        boot_state_free_parsed_sections(sections, h.section_count);
        return 0;
    }
    if ((s_native_checkpoint_hooks.restore_prepare == NULL) !=
            (s_native_checkpoint_hooks.restore_commit == NULL) ||
        (s_native_checkpoint_hooks.restore_prepare == NULL) !=
            (s_native_checkpoint_hooks.restore_cancel == NULL) ||
        (s_native_checkpoint_hooks.restore_prepare == NULL &&
         native_section->len != 0u) ||
        (s_native_checkpoint_hooks.restore_prepare != NULL &&
         (!s_native_checkpoint_hooks.restore_prepare(
              native_section->data, native_section->len, &prepared_native) ||
          prepared_native == NULL))) {
        ram_provenance_snapshot_free(provenance_snapshot);
        boot_state_free_parsed_sections(sections, h.section_count);
        return 0;
    }
    if (!boot_state_capture_rollback(&rollback)) {
        if (prepared_native != NULL)
            s_native_checkpoint_hooks.restore_cancel(prepared_native);
        ram_provenance_snapshot_free(provenance_snapshot);
        boot_state_free_parsed_sections(sections, h.section_count);
        return 0;
    }

    staged_cpu = *cpu;
    for (uint32_t index = 0u; ok && index < h.section_count; ++index) {
        const BootStateParsedSection *section = &sections[index];
        double t_sec;

        if (section->tag == BS_SEC_RAM ||
            section->tag == BS_SEC_RAM_PROVENANCE ||
            section->tag == BS_SEC_NATIVE_RENDER)
            continue;
        t_sec = boot_state_mono_ms();
        ok = apply_section(section->tag, section->data, section->len,
                           &staged_cpu, entry_pc);
#if defined(PSX_BOOT_STATE_TEST_FAULT_INJECTION)
        if (ok && section->tag == BS_SEC_GPU &&
            s_test_fail_after_device_apply) {
            s_test_fail_after_device_apply = 0;
            ok = 0;
        }
#endif
        {
            const double dt = boot_state_mono_ms() - t_sec;
            if (section->tag == BS_SEC_VRAM) apply_vram_ms += dt;
            else if (section->tag == BS_SEC_SPURAM) apply_spuram_ms += dt;
            else apply_other_ms += dt;
        }
    }

    if (!ok) {
        (void)boot_state_restore_rollback(&rollback);
        boot_state_free_rollback(&rollback);
        if (prepared_native != NULL)
            s_native_checkpoint_hooks.restore_cancel(prepared_native);
        ram_provenance_snapshot_free(provenance_snapshot);
        boot_state_free_parsed_sections(sections, h.section_count);
        return 0;
    }

    if (prepared_native != NULL)
        s_native_checkpoint_hooks.restore_commit(prepared_native);
    gpu_note_vram_restore();

    {
        const double t_ram = boot_state_mono_ms();
        memcpy(memory_get_ram_ptr(), ram_section->data, ram_section->len);
        ram_provenance_snapshot_commit(provenance_snapshot);
        provenance_snapshot = NULL;
        *cpu = staged_cpu;
        apply_ram_ms += boot_state_mono_ms() - t_ram;
    }
    {
        extern void psx_kernel_bless_note_range(uint32_t phys, uint32_t l);
        psx_kernel_bless_note_range(0u, ram_section->len);
    }
    overlay_watch_invalidate_after_ram_restore();
    boot_state_free_rollback(&rollback);
    boot_state_free_parsed_sections(sections, h.section_count);

    {
        const double total_ms = boot_state_mono_ms() - t0;
        fprintf(stderr,
                "savestate: load_timing read=0.0 inflate=%.1f "
                "apply_ram=%.1f apply_vram=%.1f apply_spuram=%.1f "
                "apply_other=%.1f total=%.1f ms (file=%zu)\n",
                inflate_ms,
                apply_ram_ms, apply_vram_ms, apply_spuram_ms,
                apply_other_ms, total_ms, file_len);
    }
    return 1;
}

int boot_state_load(const char* path, uint32_t bios_checksum,
                    uint32_t entry_pc, CPUState* cpu) {
    FILE* f = fopen(path, "rb");
    long sz;
    uint8_t* file = NULL;
    size_t file_len = 0;
    int ok;
    const double t0 = boot_state_mono_ms();
    double t_after_read;

    if (!f) {
        fprintf(stderr, "boot_state: reject — missing %s\n",
                path ? path : "(null)");
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    sz = ftell(f);
    if (sz < (long)BOOT_STATE_HEADER_WIRE_BYTES ||
        (uint64_t)sz > (uint64_t)BOOT_STATE_MAX_BYTES) {
        fprintf(stderr, "boot_state: reject — bad size %ld for %s\n",
                sz, path ? path : "(null)");
        fclose(f);
        return 0;
    }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }
    file_len = (size_t)sz;
    file = (uint8_t*)malloc(file_len);
    if (!file) { fclose(f); return 0; }
    if (fread(file, 1, file_len, f) != file_len) {
        free(file);
        fclose(f);
        return 0;
    }
    fclose(f);
    t_after_read = boot_state_mono_ms();
    (void)t0;
    (void)t_after_read;

    ok = boot_state_load_buffer(file, file_len, bios_checksum, entry_pc, cpu);
    free(file);
    return ok;
}

void boot_state_set_capture(const char* path, uint32_t bios_checksum,
                            uint32_t entry_pc) {
    strncpy(s_capture_path, path, sizeof(s_capture_path) - 1);
    s_capture_path[sizeof(s_capture_path) - 1] = '\0';
    s_capture_checksum = bios_checksum;
    s_capture_entry_pc = entry_pc;
}

void boot_state_trigger_capture(const CPUState* cpu) {
    if (!s_capture_path[0]) return;
    boot_state_save(cpu, s_capture_checksum, s_capture_entry_pc, s_capture_path);
    s_capture_path[0] = '\0';
}
