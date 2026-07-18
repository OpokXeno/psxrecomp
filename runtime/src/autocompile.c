/* autocompile.c — see autocompile.h. Windows-first (the project's dev
 * platform); on other hosts the spawn is a graceful no-op and the manual
 * compile_overlays.py flow still works. */
#include "autocompile.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>   /* getenv — declared here; glibc/windows.h leak it, macOS SDK does not */
#include <string.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

extern void overlay_loader_rescan(void);
extern int overlay_loader_load_published(const char *dll_path);

static char s_cmd[4096];   /* large: the runtime-constructed bundled tcc cmd has
                            * many absolute paths (python+script+recompiler+tcc+...) */
static char s_cwd[512];
/* Canonical loader cache dir + captures file (see autocompile_set_cache_paths).
 * Injected into the compile child's environment so its WRITE location is exactly
 * the loader's READ location, regardless of what the configured command says. */
static char s_cache_dir[512];
static char s_captures[512];

enum { AC_IDLE = 0, AC_RUNNING = 1, AC_DONE = 2 };
#ifdef _WIN32
static volatile LONG s_state     = AC_IDLE;
static volatile LONG s_exit_code = -1;
static int ac_state_load(void) {
    return (int)InterlockedCompareExchange(&s_state, AC_IDLE, AC_IDLE);
}
static void ac_state_store(int value) {
    InterlockedExchange(&s_state, (LONG)value);
}
#else
static int s_state     = AC_IDLE;
static int s_exit_code = -1;
static int ac_state_load(void) { return s_state; }
static void ac_state_store(int value) { s_state = value; }
#endif
static uint32_t     s_runs      = 0;
static uint32_t     s_fails     = 0;
static uint32_t     s_rescans   = 0;
/* Per-shard build accounting, parsed from compile_overlays.py's machine-readable
 * "PSX_SHARD_RESULT ok=N failed=M skipped=K" line. Before this, a compile run
 * that built zero shards because a header change broke EVERY shard compile
 * looked identical to a clean run: the script still exited 0 and the runtime
 * quietly ran everything interpreted. These counters make that visible over the
 * TCP debug server (autocompile_status). s_shard_fail_total accumulates across
 * runs so a single failing run is never overwritten by a later clean one. */
static uint32_t     s_shard_ok         = 0;   /* last run */
static uint32_t     s_shard_fail       = 0;   /* last run */
static uint32_t     s_shard_skipped    = 0;   /* last run */
static uint32_t     s_shard_fail_total = 0;   /* accumulated across all runs */
static int          s_shard_result_seen = 0;  /* did we parse a result line? */

/* Child-output tail ring. Watcher thread writes, TCP reads — guarded by a
 * critical section on Windows. Holds the TAIL (newest bytes win). */
#define AC_OUT_CAP 8192
static char s_out[AC_OUT_CAP];
static int  s_out_len = 0;

#ifdef _WIN32
static CRITICAL_SECTION s_out_lock;
static int              s_out_lock_init = 0;
static HANDLE           s_proc = NULL;

#define AC_PUBLISH_CAP 128
static char s_publish_paths[AC_PUBLISH_CAP][768];
static unsigned s_publish_head = 0, s_publish_count = 0;
static unsigned s_publish_seen_run = 0, s_publish_drops_run = 0;
static char s_child_line[1024];
static int  s_child_line_len = 0;

static void publish_line_locked(void) {
    static const char marker[] = "PSX_SHARD_PUBLISHED ";
    if (strncmp(s_child_line, marker, sizeof(marker) - 1) != 0) return;
    const char *path = s_child_line + sizeof(marker) - 1;
    if (!path[0]) return;
    s_publish_seen_run++;
    if (s_publish_count >= AC_PUBLISH_CAP) {
        s_publish_drops_run++;
        return;
    }
    unsigned slot = (s_publish_head + s_publish_count) % AC_PUBLISH_CAP;
    snprintf(s_publish_paths[slot], sizeof(s_publish_paths[slot]), "%s", path);
    s_publish_count++;
}

static void publish_parse_locked(const char *buf, int n) {
    for (int i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\r') continue;
        if (c == '\n') {
            s_child_line[s_child_line_len] = '\0';
            publish_line_locked();
            s_child_line_len = 0;
        } else if (s_child_line_len < (int)sizeof(s_child_line) - 1) {
            s_child_line[s_child_line_len++] = c;
        }
    }
}

static int publish_pop(char *out, int cap) {
    int found = 0;
    EnterCriticalSection(&s_out_lock);
    if (s_publish_count) {
        snprintf(out, (size_t)cap, "%s", s_publish_paths[s_publish_head]);
        s_publish_head = (s_publish_head + 1u) % AC_PUBLISH_CAP;
        s_publish_count--;
        found = 1;
    }
    LeaveCriticalSection(&s_out_lock);
    return found;
}

static int publish_pending(void) {
    int pending;
    EnterCriticalSection(&s_out_lock);
    pending = s_publish_count != 0;
    LeaveCriticalSection(&s_out_lock);
    return pending;
}

static void out_append(const char *buf, int n) {
    EnterCriticalSection(&s_out_lock);
    publish_parse_locked(buf, n);
    if (n >= AC_OUT_CAP) {
        memcpy(s_out, buf + (n - AC_OUT_CAP), AC_OUT_CAP);
        s_out_len = AC_OUT_CAP;
    } else {
        if (s_out_len + n > AC_OUT_CAP) {
            int keep = AC_OUT_CAP - n;
            memmove(s_out, s_out + (s_out_len - keep), keep);
            s_out_len = keep;
        }
        memcpy(s_out + s_out_len, buf, n);
        s_out_len += n;
    }
    LeaveCriticalSection(&s_out_lock);
}

typedef struct { HANDLE read_pipe; HANDLE proc; } WatchCtx;

static DWORD WINAPI watch_thread(LPVOID arg) {
    WatchCtx *ctx = (WatchCtx *)arg;
    char buf[1024];
    DWORD got;
    while (ReadFile(ctx->read_pipe, buf, sizeof(buf), &got, NULL) && got > 0)
        out_append(buf, (int)got);
    CloseHandle(ctx->read_pipe);
    WaitForSingleObject(ctx->proc, INFINITE);
    DWORD code = (DWORD)-1;
    GetExitCodeProcess(ctx->proc, &code);
    CloseHandle(ctx->proc);
    HeapFree(GetProcessHeap(), 0, ctx);
    InterlockedExchange(&s_exit_code, (LONG)code);
    ac_state_store(AC_DONE);    /* emu thread applies via autocompile_poll_main */
    return 0;
}
#endif /* _WIN32 */

void autocompile_configure(const char *cmd, const char *cwd) {
    snprintf(s_cmd, sizeof(s_cmd), "%s", cmd ? cmd : "");
    snprintf(s_cwd, sizeof(s_cwd), "%s", cwd ? cwd : "");
#ifdef _WIN32
    if (!s_out_lock_init) {
        InitializeCriticalSection(&s_out_lock);
        s_out_lock_init = 1;
    }
#endif
}

void autocompile_set_cache_paths(const char *cache_dir, const char *captures) {
    snprintf(s_cache_dir, sizeof(s_cache_dir), "%s", cache_dir ? cache_dir : "");
    snprintf(s_captures,  sizeof(s_captures),  "%s", captures  ? captures  : "");
}

int autocompile_configured(void) { return s_cmd[0] != '\0'; }
int autocompile_busy(void)       { return ac_state_load() != AC_IDLE; }

/* Probe PATH for a real C compiler (gcc/cc/clang). A configured command STRING
 * (autocompile_configured) is not enough: the shipped game.toml always carries
 * overlay_autocompile_cmd, so it can't tell a dev box from a toolchain-less
 * player. This opens each candidate exe in each PATH dir — the actual "can we
 * compile a shard" signal. Memoized (PATH doesn't change mid-run). */
int autocompile_toolchain_available(void) {
    static int s_cached = -1;
    if (s_cached >= 0) return s_cached;
    const char *path = getenv("PATH");
    int found = 0;
    if (path && *path) {
#ifdef _WIN32
        const char sep = ';';
        static const char *exes[] = { "gcc.exe", "cc.exe", "clang.exe" };
#else
        const char sep = ':';
        static const char *exes[] = { "gcc", "cc", "clang" };
#endif
        const char *p = path;
        while (*p && !found) {
            const char *e = strchr(p, sep);
            size_t dlen = e ? (size_t)(e - p) : strlen(p);
            if (dlen > 0 && dlen < 480) {
                for (size_t k = 0; k < sizeof(exes) / sizeof(exes[0]); k++) {
                    char cand[512];
                    snprintf(cand, sizeof(cand), "%.*s/%s", (int)dlen, p, exes[k]);
                    FILE *f = fopen(cand, "rb");
                    if (f) { fclose(f); found = 1; break; }
                }
            }
            if (!e) break;
            p = e + 1;
        }
    }
    s_cached = found;
    return found;
}

int autocompile_request(void) {
    /* DONE owns an unconsumed cache rescan/result. Only the emulation-thread
     * poll may return it to IDLE; never overwrite it with another child. */
    if (!autocompile_configured() || ac_state_load() != AC_IDLE) return 0;
#ifdef _WIN32
    EnterCriticalSection(&s_out_lock);
    s_publish_head = s_publish_count = 0;
    s_publish_seen_run = s_publish_drops_run = 0;
    s_child_line_len = 0;
    LeaveCriticalSection(&s_out_lock);
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return 0;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    /* Pin the WRITE cache + READ captures to the loader's canonical locations.
     * Set on the PARENT env so the child (CreateProcessA lpEnvironment=NULL =>
     * inherit) sees them; compile_overlays.py / coverage_vault.py prefer these
     * over any CLI --out-dir/--captures, so the cache can never drift from where
     * the loader reads. This is THE fix for the read/write-location divergence. */
    if (s_cache_dir[0]) SetEnvironmentVariableA("PSX_OVERLAY_CACHE_DIR", s_cache_dir);
    if (s_captures[0])  SetEnvironmentVariableA("PSX_OVERLAY_CAPTURES",  s_captures);
    SetEnvironmentVariableA("PSX_OVERLAY_LIVE_AUTOCOMPILE", "1");

    /* cmd.exe /C resolves the command via PATH and supports the relative
     * paths in the configured line (cwd = project root). */
    char full[4200];
    snprintf(full, sizeof(full), "cmd.exe /C %s", s_cmd);

    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError  = wr;
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));
    /* Compilation is opportunistic: it must never outrank the interpreter that
     * keeps the current frame alive. cmd -> python -> gcc inherit below-normal
     * priority, while the live compile script also defaults to one worker. */
    BOOL ok = CreateProcessA(NULL, full, NULL, NULL, TRUE,
                             CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                             NULL, s_cwd[0] ? s_cwd : NULL, &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        s_fails++;
        return 0;
    }
    CloseHandle(pi.hThread);

    WatchCtx *ctx = (WatchCtx *)HeapAlloc(GetProcessHeap(), 0, sizeof(*ctx));
    if (!ctx) {
        CloseHandle(rd);
        CloseHandle(pi.hProcess);
        s_fails++;
        return 0;
    }
    ctx->read_pipe = rd;
    ctx->proc      = pi.hProcess;
    s_proc = pi.hProcess;
    ac_state_store(AC_RUNNING);
    s_runs++;
    HANDLE th = CreateThread(NULL, 0, watch_thread, ctx, 0, NULL);
    if (th) {
        CloseHandle(th);
    } else {
        /* Without the watcher nobody drains stdout, observes completion, or
         * closes the process handle. Cancel the just-created child fail-closed. */
        TerminateProcess(pi.hProcess, ERROR_NOT_ENOUGH_MEMORY);
        CloseHandle(rd);
        CloseHandle(pi.hProcess);
        HeapFree(GetProcessHeap(), 0, ctx);
        s_proc = NULL;
        ac_state_store(AC_IDLE);
        s_fails++;
        return 0;
    }
    return 1;
#else
    return 0;  /* non-Windows hosts: manual compile flow only */
#endif
}

/* Scan the child-output tail ring for the LAST "PSX_SHARD_RESULT ok=N failed=M
 * skipped=K" line and record the counts. Returns 1 if a result line was found.
 * Called from autocompile_poll_main (emu thread) after the child exits. */
static int parse_shard_result(void) {
#ifdef _WIN32
    char buf[AC_OUT_CAP + 1];
    int n = 0;
    if (s_out_lock_init) {
        EnterCriticalSection(&s_out_lock);
        n = s_out_len;
        memcpy(buf, s_out, (size_t)n);
        LeaveCriticalSection(&s_out_lock);
    }
    buf[n] = '\0';
    /* Find the LAST occurrence (a run may print more than once over its life). */
    const char *marker = "PSX_SHARD_RESULT";
    const char *hit = NULL, *p = buf;
    for (;;) {
        const char *q = strstr(p, marker);
        if (!q) break;
        hit = q;
        p = q + 1;
    }
    if (!hit) return 0;
    unsigned ok = 0, failed = 0, skipped = 0;
    /* Tolerant of field order: scan for each key independently within the line. */
    const char *ln_end = strchr(hit, '\n');
    size_t span = ln_end ? (size_t)(ln_end - hit) : strlen(hit);
    char line[256];
    size_t cp = span < sizeof(line) - 1 ? span : sizeof(line) - 1;
    memcpy(line, hit, cp);
    line[cp] = '\0';
    const char *f;
    if ((f = strstr(line, "ok=")))      sscanf(f, "ok=%u", &ok);
    if ((f = strstr(line, "failed=")))  sscanf(f, "failed=%u", &failed);
    if ((f = strstr(line, "skipped="))) sscanf(f, "skipped=%u", &skipped);
    s_shard_ok      = ok;
    s_shard_fail    = failed;
    s_shard_skipped = skipped;
    if (failed) s_shard_fail_total += failed;
    return 1;
#else
    return 0;
#endif
}

void autocompile_poll_main(void) {
    /* Drain at most one atomic publication per frame. The producer only sends
     * paths; candidate registration remains single-threaded here. */
#ifdef _WIN32
    char published[768];
    if (publish_pop(published, (int)sizeof(published))) {
        (void)overlay_loader_load_published(published);
        s_rescans++;
    }
    if (ac_state_load() == AC_RUNNING) return;
#endif
    if (ac_state_load() != AC_DONE) return;
#ifdef _WIN32
    if (publish_pending()) return;
#endif
    ac_state_store(AC_IDLE);
    /* New producers report each DLL directly; a full rescan is only the
     * compatibility/overflow fallback. The loader is
     * idempotent — rescanning after a failed compile is harmless. */
#ifdef _WIN32
    if (s_publish_seen_run == 0 || s_publish_drops_run != 0) {
        overlay_loader_rescan();
        s_rescans++;
    }
#else
    overlay_loader_rescan();
    s_rescans++;
#endif
    s_shard_result_seen = parse_shard_result();
    /* A run "failed" if the process exited non-zero OR it reported shard
     * failures. compile_overlays.py exits 2 when any shard that should have
     * built failed, so these usually agree; the parsed count is the detail. */
    if (s_exit_code != 0 || (s_shard_result_seen && s_shard_fail > 0))
        s_fails++;
}

/* Minimal JSON string escaper for the output tail. */
static int json_escape_into(char *dst, int cap, const char *src, int n) {
    int o = 0;
    for (int i = 0; i < n && o < cap - 8; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') { dst[o++] = '\\'; dst[o++] = (char)c; }
        else if (c == '\n') { dst[o++] = '\\'; dst[o++] = 'n'; }
        else if (c == '\r') { /* drop */ }
        else if (c == '\t') { dst[o++] = '\\'; dst[o++] = 't'; }
        else if (c < 0x20)  { o += snprintf(dst + o, cap - o, "\\u%04x", c); }
        /* Bytes >= 0x80 are cp1252 console output from the child compile
         * (e.g. 0x97 em-dash) — raw they make the JSON invalid UTF-8 and
         * break strict clients. Escape as \u00XX (Latin-1 view). */
        else if (c >= 0x80) { o += snprintf(dst + o, cap - o, "\\u%04x", c); }
        else dst[o++] = (char)c;
    }
    dst[o] = '\0';
    return o;
}

int autocompile_status_json(char *out, int cap) {
    static const char *names[] = { "idle", "running", "done" };
    char tail[2048];
    int  tn = 0;
    tail[0] = '\0';
#ifdef _WIN32
    if (s_out_lock_init) {
        EnterCriticalSection(&s_out_lock);
        int take = s_out_len < 900 ? s_out_len : 900;   /* newest tail */
        tn = json_escape_into(tail, sizeof(tail),
                              s_out + (s_out_len - take), take);
        LeaveCriticalSection(&s_out_lock);
    }
#endif
    (void)tn;
    return snprintf(out, cap,
        "{\"configured\":%d,\"state\":\"%s\",\"runs\":%u,\"fails\":%u,"
        "\"rescans\":%u,\"last_exit\":%d,"
        "\"shard_ok\":%u,\"shard_fail\":%u,\"shard_skipped\":%u,"
        "\"shard_fail_total\":%u,\"shard_result_seen\":%d,"
        "\"output_tail\":\"%s\"}",
        autocompile_configured(), names[ac_state_load() & 3], s_runs, s_fails,
        s_rescans, s_exit_code,
        s_shard_ok, s_shard_fail, s_shard_skipped,
        s_shard_fail_total, s_shard_result_seen, tail);
}
