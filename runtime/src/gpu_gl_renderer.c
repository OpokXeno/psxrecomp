/* gpu_gl_renderer.c — hardware OpenGL renderer backend.
 *
 * ARCHITECTURE (v2 — GPU-authoritative VRAM)
 * -------------------------------------------
 * The FBO color texture (`s_hr_tex`, RGBA8, 1024*S x 512*S where S is the
 * internal-resolution scale from [video] supersampling) is the single
 * authoritative copy of VRAM. EVERY mutation goes through the GPU:
 *
 *   - polys / rects / lines  -> rasterized into the hr FBO
 *   - GP0(02h) fills         -> scissored glClear (color + stencil)
 *   - VRAM->VRAM copies      -> hr FBO -> scratch texture blit, then a
 *                               masked quad draw back into the hr FBO
 *   - CPU->VRAM transfers    -> written to the CPU mirror immediately and
 *                               accumulated into a pending-upload rect; the
 *                               rect is flushed (staging texture + quad into
 *                               the hr FBO, plus a direct R16UI subimage)
 *                               before the next GPU op, preserving op order
 *
 * PS1 MASK BIT (bit15). The FBO alpha channel carries bit15 exactly
 * (1.0 = set), and a stencil buffer mirrors it (stencil bit0 == bit15):
 *   - "set mask"  -> fragment alpha + stencil write value
 *   - "check mask"-> stencil test (pass iff stored == 0). The stencil write
 *     value is coupled to the test reference in GL, so when checking we use
 *     GL_INVERT on pass to write a 1 (stored is known 0 when the test
 *     passes) and GL_KEEP to write a 0.
 *   - textured prims (and copies/uploads, whose pixels carry per-texel STP
 *     bits) are drawn in TWO passes split by the STP bit via discard, so
 *     each pass writes a single known stencil value. The same split already
 *     existed for semi-transparent texture blending.
 *   - blending uses glBlendFuncSeparate so the alpha (mask) channel is
 *     always REPLACED by the source fragment's mask bit, never blended.
 *
 * TEXTURE SAMPLING / RENDER-TO-TEXTURE. Textured prims sample a native-res
 * R16UI mirror (`s_raw_tex`) holding raw 1555 VRAM values (CLUT decode +
 * texture window + optional bilinear in the fragment shader). GPU draws
 * mark a native-coords dirty union; before any textured draw whose texture
 * page or CLUT intersects the union, a PACK pass re-encodes the dirty
 * region of the hr FBO into the raw mirror (point-sampled at native
 * coords). CPU->VRAM uploads update the raw mirror directly. So content
 * rendered by the GPU is immediately valid as a texture source.
 *
 * CPU READBACKS (VRAM->CPU transfers, GPUREAD, screenshots, 24-bit FMV
 * display) flush uploads + pack, then glReadPixels the raw mirror straight
 * into the CPU VRAM array (raw 1555, no conversion loop).
 *
 * PRESENT is deterministic: 15-bit frames always blit the display region
 * from the hr FBO into a 4:3 letterboxed rect (single path — no more
 * frame-to-frame alternation between FBO and CPU presents). 24-bit (FMV)
 * frames sync to CPU and use the quad-present path, also letterboxed.
 * PSX_GL_FORCE_CPU_PRESENT=1 (read by main.cpp) forces the CPU path as a
 * diagnostic.
 *
 * Known divergences from the software rasterizer (accepted, documented):
 *   - GL triangle/line coverage rules differ from the PS1 DDA by ±1px on
 *     edges; lines use GL_LINES (width S) instead of Bresenham.
 *   - Gouraud interpolation uses GL noperspective interpolation rather than the
 *     PS1 fixed-point DDA. Dither and 15-bit quantization after interpolation
 *     are exact and shared by Original GP0 and semantic draws.
 *   - VRAM-wrapping draws are clamped, except GP0(02h) fills which split
 *     into wrapped segments. Wrapping copies/draws are unused by real SDKs.
 *
 * Init is all-or-nothing: if any shader/FBO fails, gl_renderer_init_context
 * returns 0 and the runtime falls back to the pure software renderer — no
 * half-GL hybrid. */

#include "gpu.h"
#include "gpu_render.h"
#include "gpu_semantic_workload.h"
#include "gpu_sw_renderer.h"
#include "gpu_gl_renderer.h"
#include "latency_ring.h"
#include "debug_overlay.h"
#include "wayland_presentation.h"

#include "psx_sdl.h"
#include <SDL_opengl.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
/* GL 1.4+ enums absent from MinGW's GL 1.1 headers. */
#define PSXGL_FRAGMENT_SHADER       0x8B30
#define PSXGL_VERTEX_SHADER         0x8B31
#define PSXGL_COMPILE_STATUS        0x8B81
#define PSXGL_LINK_STATUS           0x8B82
#define PSXGL_TEXTURE0              0x84C0
#define PSXGL_ARRAY_BUFFER          0x8892
#define PSXGL_PIXEL_PACK_BUFFER     0x88EB
#define PSXGL_STREAM_DRAW           0x88E0
#define PSXGL_STREAM_READ           0x88E1
#define PSXGL_MAP_READ_BIT          0x0001
#define PSXGL_FRAMEBUFFER           0x8D40
#define PSXGL_READ_FRAMEBUFFER      0x8CA8
#define PSXGL_DRAW_FRAMEBUFFER      0x8CA9
#define PSXGL_COLOR_ATTACHMENT0     0x8CE0
#define PSXGL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define PSXGL_FRAMEBUFFER_COMPLETE  0x8CD5
#define PSXGL_RENDERBUFFER          0x8D41
#define PSXGL_DEPTH24_STENCIL8      0x88F0
#define PSXGL_R16UI                 0x8234
#define PSXGL_RED_INTEGER           0x8D94
#define PSXGL_FUNC_ADD              0x8006
#define PSXGL_FUNC_REVERSE_SUBTRACT 0x800B
#define PSXGL_CONSTANT_ALPHA        0x8003
#define PSXGL_UNPACK_ROW_LENGTH     0x0CF2
#define PSXGL_SRC1_ALPHA            0x8589
#define PSXGL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#define PSXGL_TIMEOUT_IGNORED       0xFFFFFFFFFFFFFFFFull

#ifndef APIENTRY
#define APIENTRY
#endif

#define VRAM_W 1024
#define VRAM_H 512
/* User-facing cap. x8 = 8192x4096 — inside every
 * modern driver's GL_MAX_TEXTURE_SIZE. The live-rebuild fallback in
 * gl_maybe_apply_scale still restores the previous scale if refused. */
#define GL_MAX_INTERNAL_SCALE 8

/* ---- Loaded modern-GL entry points ------------------------------------- */
typedef GLuint (APIENTRY *PFN_glCreateShader)(GLenum);
typedef void   (APIENTRY *PFN_glShaderSource)(GLuint, GLsizei, const char *const *, const GLint *);
typedef void   (APIENTRY *PFN_glCompileShader)(GLuint);
typedef void   (APIENTRY *PFN_glGetShaderiv)(GLuint, GLenum, GLint *);
typedef void   (APIENTRY *PFN_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei *, char *);
typedef void   (APIENTRY *PFN_glDeleteShader)(GLuint);
typedef GLuint (APIENTRY *PFN_glCreateProgram)(void);
typedef void   (APIENTRY *PFN_glAttachShader)(GLuint, GLuint);
typedef void   (APIENTRY *PFN_glLinkProgram)(GLuint);
typedef void   (APIENTRY *PFN_glGetProgramiv)(GLuint, GLenum, GLint *);
typedef void   (APIENTRY *PFN_glGetProgramInfoLog)(GLuint, GLsizei, GLsizei *, char *);
typedef void   (APIENTRY *PFN_glUseProgram)(GLuint);
typedef void   (APIENTRY *PFN_glDeleteProgram)(GLuint);
typedef void   (APIENTRY *PFN_glDeleteBuffers)(GLsizei, const GLuint *);
typedef void   (APIENTRY *PFN_glDeleteVertexArrays)(GLsizei, const GLuint *);
typedef GLint  (APIENTRY *PFN_glGetUniformLocation)(GLuint, const char *);
typedef void   (APIENTRY *PFN_glUniform1i)(GLint, GLint);
typedef void   (APIENTRY *PFN_glUniform1f)(GLint, GLfloat);
typedef void   (APIENTRY *PFN_glUniform2i)(GLint, GLint, GLint);
typedef void   (APIENTRY *PFN_glUniform4i)(GLint, GLint, GLint, GLint, GLint);
typedef void   (APIENTRY *PFN_glUniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void   (APIENTRY *PFN_glBlendColor)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void   (APIENTRY *PFN_glBlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum);
typedef void   (APIENTRY *PFN_glBlendEquationSeparate)(GLenum, GLenum);
typedef void   (APIENTRY *PFN_glGenVertexArrays)(GLsizei, GLuint *);
typedef void   (APIENTRY *PFN_glBindVertexArray)(GLuint);
typedef void   (APIENTRY *PFN_glActiveTexture)(GLenum);
typedef void   (APIENTRY *PFN_glGenBuffers)(GLsizei, GLuint *);
typedef void   (APIENTRY *PFN_glBindBuffer)(GLenum, GLuint);
typedef void   (APIENTRY *PFN_glBufferData)(GLenum, ptrdiff_t, const void *, GLenum);
typedef void  *(APIENTRY *PFN_glMapBufferRange)(GLenum, ptrdiff_t, ptrdiff_t, GLbitfield);
typedef GLboolean (APIENTRY *PFN_glUnmapBuffer)(GLenum);
typedef void   (APIENTRY *PFN_glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
typedef void   (APIENTRY *PFN_glEnableVertexAttribArray)(GLuint);
typedef void   (APIENTRY *PFN_glBindFragDataLocationIndexed)(GLuint, GLuint, GLuint, const char *);
typedef GLsync (APIENTRY *PFN_glFenceSync)(GLenum, GLbitfield);
typedef GLenum (APIENTRY *PFN_glClientWaitSync)(GLsync, GLbitfield, GLuint64);
typedef void   (APIENTRY *PFN_glWaitSync)(GLsync, GLbitfield, GLuint64);
typedef void   (APIENTRY *PFN_glDeleteSync)(GLsync);
typedef void   (APIENTRY *PFN_glGenFramebuffers)(GLsizei, GLuint *);
typedef void   (APIENTRY *PFN_glDeleteFramebuffers)(GLsizei, const GLuint *);
typedef void   (APIENTRY *PFN_glBindFramebuffer)(GLenum, GLuint);
typedef void   (APIENTRY *PFN_glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (APIENTRY *PFN_glCheckFramebufferStatus)(GLenum);
typedef void   (APIENTRY *PFN_glBlitFramebuffer)(GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLbitfield,GLenum);
typedef void   (APIENTRY *PFN_glGenRenderbuffers)(GLsizei, GLuint *);
typedef void   (APIENTRY *PFN_glDeleteRenderbuffers)(GLsizei, const GLuint *);
typedef void   (APIENTRY *PFN_glBindRenderbuffer)(GLenum, GLuint);
typedef void   (APIENTRY *PFN_glRenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
typedef void   (APIENTRY *PFN_glFramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
/* GPU timer queries (ARB_timer_query / core GL 3.3) — frame_perf instrumentation. */
typedef void   (APIENTRY *PFN_glGenQueries)(GLsizei, GLuint *);
typedef void   (APIENTRY *PFN_glDeleteQueries)(GLsizei, const GLuint *);
typedef void   (APIENTRY *PFN_glBeginQuery)(GLenum, GLuint);
typedef void   (APIENTRY *PFN_glEndQuery)(GLenum);
typedef void   (APIENTRY *PFN_glGetQueryObjectui64v)(GLuint, GLenum, GLuint64 *);
typedef void   (APIENTRY *PFN_glGetQueryObjectiv)(GLuint, GLenum, GLint *);
typedef void   (APIENTRY *PFN_glQueryCounter)(GLuint, GLenum);
#ifndef GL_TIME_ELAPSED
#define GL_TIME_ELAPSED            0x88BF
#endif
#ifndef GL_TIMESTAMP
#define GL_TIMESTAMP               0x8E28
#endif
#ifndef GL_QUERY_RESULT
#define GL_QUERY_RESULT            0x8866
#endif
#ifndef GL_QUERY_RESULT_AVAILABLE
#define GL_QUERY_RESULT_AVAILABLE  0x8867
#endif

static PFN_glCreateShader      p_glCreateShader;
static PFN_glShaderSource      p_glShaderSource;
static PFN_glCompileShader     p_glCompileShader;
static PFN_glGetShaderiv       p_glGetShaderiv;
static PFN_glGetShaderInfoLog  p_glGetShaderInfoLog;
static PFN_glDeleteShader      p_glDeleteShader;
static PFN_glCreateProgram     p_glCreateProgram;
static PFN_glAttachShader      p_glAttachShader;
static PFN_glLinkProgram       p_glLinkProgram;
static PFN_glGetProgramiv      p_glGetProgramiv;
static PFN_glGetProgramInfoLog p_glGetProgramInfoLog;
static PFN_glUseProgram        p_glUseProgram;
static PFN_glDeleteProgram     p_glDeleteProgram;
static PFN_glDeleteBuffers     p_glDeleteBuffers;
static PFN_glDeleteVertexArrays p_glDeleteVertexArrays;
static PFN_glGetUniformLocation p_glGetUniformLocation;
static PFN_glUniform1i         p_glUniform1i;
static PFN_glUniform1f         p_glUniform1f;
static PFN_glUniform2i         p_glUniform2i;
static PFN_glUniform4i         p_glUniform4i;
static PFN_glUniform4f         p_glUniform4f;
static PFN_glBlendColor        p_glBlendColor;
static PFN_glBlendFuncSeparate p_glBlendFuncSeparate;
static PFN_glBlendEquationSeparate p_glBlendEquationSeparate;
static PFN_glGenVertexArrays   p_glGenVertexArrays;
static PFN_glBindVertexArray   p_glBindVertexArray;
static PFN_glActiveTexture     p_glActiveTexture;
static PFN_glGenBuffers        p_glGenBuffers;
static PFN_glBindBuffer        p_glBindBuffer;
static PFN_glBufferData        p_glBufferData;
static PFN_glMapBufferRange    p_glMapBufferRange;
static PFN_glUnmapBuffer       p_glUnmapBuffer;
static PFN_glVertexAttribPointer p_glVertexAttribPointer;
static PFN_glEnableVertexAttribArray p_glEnableVertexAttribArray;
static PFN_glBindFragDataLocationIndexed p_glBindFragDataLocationIndexed;
static PFN_glFenceSync p_glFenceSync;
static PFN_glClientWaitSync p_glClientWaitSync;
static PFN_glWaitSync p_glWaitSync;
static PFN_glDeleteSync p_glDeleteSync;
static PFN_glGenFramebuffers   p_glGenFramebuffers;
static PFN_glDeleteFramebuffers p_glDeleteFramebuffers;
static PFN_glBindFramebuffer   p_glBindFramebuffer;
static PFN_glFramebufferTexture2D p_glFramebufferTexture2D;
static PFN_glCheckFramebufferStatus p_glCheckFramebufferStatus;
static PFN_glBlitFramebuffer   p_glBlitFramebuffer;
static PFN_glGenQueries          p_glGenQueries;
static PFN_glDeleteQueries       p_glDeleteQueries;
static PFN_glBeginQuery          p_glBeginQuery;
static PFN_glEndQuery            p_glEndQuery;
static PFN_glGetQueryObjectui64v p_glGetQueryObjectui64v;
static PFN_glGetQueryObjectiv    p_glGetQueryObjectiv;
static PFN_glQueryCounter        p_glQueryCounter;
static void gl_perf_init(void);   /* frame_perf — defined below, called from init_gpu_raster */
static void gl_perf_mirror_begin(void); /* frame_perf: GPU-time bracket around ONE native-wide */
static void gl_perf_mirror_end(void);   /* mirror pass (timestamp pair; splits scene canonical/mirror) */
/* Native-wide mirror ABLATION (perf attribution, debug cmd gl_ws_ablate):
 * 0 = normal, 1 = skip the whole mirror pass, 2 = full mirror state churn but no
 * draw calls, 3 = mirror draws land in the hr FBO (no per-pass FBO rebind; wide
 * margins go stale + hr gets garbage — perf probe only). */
static int s_ws_ablate = 0;
static void flush_tex_batch(void); /* textured-prim batch — defined below, flushed from coherency points */
static void flush_flat_batch(void); /* flat/gouraud GEO batch (MotK starfield 0x68 dots) */
static PFN_glGenRenderbuffers  p_glGenRenderbuffers;
static PFN_glDeleteRenderbuffers p_glDeleteRenderbuffers;
static PFN_glBindRenderbuffer  p_glBindRenderbuffer;
static PFN_glRenderbufferStorage p_glRenderbufferStorage;
static PFN_glFramebufferRenderbuffer p_glFramebufferRenderbuffer;

static int load_modern_gl(void) {
    int ok = 1;
#define LOAD(p, n) do { p = (void *)SDL_GL_GetProcAddress(n); if (!p) ok = 0; } while (0)
    LOAD(p_glCreateShader, "glCreateShader");   LOAD(p_glShaderSource, "glShaderSource");
    LOAD(p_glCompileShader, "glCompileShader"); LOAD(p_glGetShaderiv, "glGetShaderiv");
    LOAD(p_glGetShaderInfoLog, "glGetShaderInfoLog"); LOAD(p_glDeleteShader, "glDeleteShader");
    LOAD(p_glCreateProgram, "glCreateProgram"); LOAD(p_glAttachShader, "glAttachShader");
    LOAD(p_glLinkProgram, "glLinkProgram");     LOAD(p_glGetProgramiv, "glGetProgramiv");
    LOAD(p_glGetProgramInfoLog, "glGetProgramInfoLog"); LOAD(p_glUseProgram, "glUseProgram");
    LOAD(p_glDeleteProgram, "glDeleteProgram"); LOAD(p_glDeleteBuffers, "glDeleteBuffers");
    LOAD(p_glDeleteVertexArrays, "glDeleteVertexArrays");
    LOAD(p_glGetUniformLocation, "glGetUniformLocation"); LOAD(p_glUniform1i, "glUniform1i");
    LOAD(p_glUniform1f, "glUniform1f");
    LOAD(p_glUniform2i, "glUniform2i"); LOAD(p_glUniform4i, "glUniform4i");
    LOAD(p_glUniform4f, "glUniform4f");
    LOAD(p_glBlendColor, "glBlendColor");
    LOAD(p_glBlendFuncSeparate, "glBlendFuncSeparate");
    LOAD(p_glBlendEquationSeparate, "glBlendEquationSeparate");
    LOAD(p_glGenVertexArrays, "glGenVertexArrays"); LOAD(p_glBindVertexArray, "glBindVertexArray");
    LOAD(p_glActiveTexture, "glActiveTexture");  LOAD(p_glGenBuffers, "glGenBuffers");
    LOAD(p_glBindBuffer, "glBindBuffer");        LOAD(p_glBufferData, "glBufferData");
    LOAD(p_glMapBufferRange, "glMapBufferRange");
    LOAD(p_glUnmapBuffer, "glUnmapBuffer");
    LOAD(p_glVertexAttribPointer, "glVertexAttribPointer");
    LOAD(p_glEnableVertexAttribArray, "glEnableVertexAttribArray");
    LOAD(p_glBindFragDataLocationIndexed, "glBindFragDataLocationIndexed");
    LOAD(p_glFenceSync, "glFenceSync");
    LOAD(p_glClientWaitSync, "glClientWaitSync");
    LOAD(p_glWaitSync, "glWaitSync");
    LOAD(p_glDeleteSync, "glDeleteSync");
    LOAD(p_glGenFramebuffers, "glGenFramebuffers"); LOAD(p_glBindFramebuffer, "glBindFramebuffer");
    LOAD(p_glDeleteFramebuffers, "glDeleteFramebuffers");
    LOAD(p_glFramebufferTexture2D, "glFramebufferTexture2D");
    LOAD(p_glCheckFramebufferStatus, "glCheckFramebufferStatus");
    LOAD(p_glBlitFramebuffer, "glBlitFramebuffer");
    LOAD(p_glGenRenderbuffers, "glGenRenderbuffers");
    LOAD(p_glDeleteRenderbuffers, "glDeleteRenderbuffers");
    LOAD(p_glBindRenderbuffer, "glBindRenderbuffer");
    LOAD(p_glRenderbufferStorage, "glRenderbufferStorage");
    LOAD(p_glFramebufferRenderbuffer, "glFramebufferRenderbuffer");
    /* GPU timer queries — optional (frame_perf). Don't fail the renderer if
     * absent; gl_perf just stays disabled. */
    p_glGenQueries          = (void *)SDL_GL_GetProcAddress("glGenQueries");
    p_glDeleteQueries       = (void *)SDL_GL_GetProcAddress("glDeleteQueries");
    p_glBeginQuery          = (void *)SDL_GL_GetProcAddress("glBeginQuery");
    p_glEndQuery            = (void *)SDL_GL_GetProcAddress("glEndQuery");
    p_glGetQueryObjectui64v = (void *)SDL_GL_GetProcAddress("glGetQueryObjectui64v");
    p_glGetQueryObjectiv    = (void *)SDL_GL_GetProcAddress("glGetQueryObjectiv");
    p_glQueryCounter        = (void *)SDL_GL_GetProcAddress("glQueryCounter");
#undef LOAD
    return ok;
}

/* ---- state ------------------------------------------------------------- */
static SDL_Window   *s_win = NULL;
static SDL_GLContext s_ctx = NULL;
static uint16_t     *s_vram = NULL;       /* CPU VRAM array (gpu.c's storage) */
static int           s_swap_interval = 1; /* SDL_GL swap interval (vsync mode) */

static int           s_scale = 1;          /* internal-res scale (hr FBO) */
/* Netplay: software VRAM is authoritative while GL mirrors every canonical
 * draw at the requested presentation scale. */
static int           s_cpu_auth_dual = 0;
static int           s_req_scale = 1;      /* requested before context init */
static int           s_scale_apply_pending = 0; /* set by glb_set_scale; consumed at next present */

static GLuint        s_present_tex = 0;    /* CPU-readout present path (24bpp) */
static int           s_present_w = 0, s_present_h = 0;
static GLuint        s_native_present_tex = 0; /* independent Native FMV surface */
static int           s_native_present_w = 0, s_native_present_h = 0;
static GLuint        s_present_prog = 0, s_present_vao = 0;
static GLint         s_present_uTex = -1, s_present_uUvRect = -1;
static GLint         s_present_uLut = -1, s_present_uLutOn = -1;
/* Present-time screen LUT (CRT/composite/trinitron color grade): the baked
 * gpu.c table as a 256x128 RGB8 texture, re-uploaded only when the
 * generation counter bumps. Applied on the 15-bit game present paths
 * (present_target_quad callers); the 24-bit FMV/CPU path keeps it off,
 * mirroring the documented CPU-path semantics. */
static GLuint        s_lut_tex = 0;
static int           s_lut_gen_seen = -1;
static int           s_lut_on = 0;
static GLuint        s_interp_prog = 0, s_interp_tex[3];
static GLsync        s_interp_fence[3];
static GLsync        s_interp_draw_fence = NULL;
static GLint         s_interp_uPrev = -1, s_interp_uCurr = -1;
static GLint         s_interp_uAlpha = -1, s_interp_uUvRect = -1;
static GLint         s_interp_uBlendMode = -1;
static int           s_interp_enabled = 0, s_interp_valid = 0;
static int           s_interp_suspended = 0;
static int           s_interp_blend_mode = 0;
static int           s_interp_prev = 0, s_interp_cur = 0;
static int           s_interp_w = 0, s_interp_h = 0, s_interp_linear = 0;
static int           s_interp_force_4_3 = 0, s_interp_source_path = -1;
static uint64_t      s_interp_start = 0, s_interp_duration = 1;
static uint64_t      s_interp_last_capture = 0, s_interp_swaps = 0;
static uint64_t      s_interp_captures = 0;
static int           s_interp_diag = 0;
static double        s_interp_host_hz = 0.0;
static double        s_interp_target_hz = 0.0;
static SDL_GLContext s_interp_ctx = NULL;
static SDL_Thread   *s_interp_thread = NULL;
static SDL_mutex    *s_interp_mutex = NULL;
static SDL_atomic_t  s_interp_thread_run;
static GLuint        s_interp_thread_vao = 0;
static void interp_reset_history(void);
static int interp_thread_main(void *opaque);
static int interp_present(void);
static void interp_draw_quad(float alpha, int lx, int ly, int lw, int lh);

static int           s_raster_ok = 0;      /* full GPU pipeline available */

/* Authoritative VRAM: hr color texture + stencil (mask bit) FBO. */
static GLuint        s_hr_tex = 0, s_hr_fbo = 0, s_hr_rb = 0;
/* Host-only midpoint companion. It is never sampled as guest VRAM: each
 * native source frame starts as an exact hr color+stencil clone, then receives
 * only midpoint semantic draws and mirrored nonsemantic mutations. */
static GLuint        s_midpoint_tex = 0, s_midpoint_fbo = 0, s_midpoint_rb = 0;
#define NATIVE_INTERPOLATION_MAX_PHASES GPU_SEMANTIC_INTERPOLATION_MAX_PHASES
static GLuint s_extra_phase_tex[NATIVE_INTERPOLATION_MAX_PHASES - 1u];
static GLuint s_extra_phase_fbo[NATIVE_INTERPOLATION_MAX_PHASES - 1u];
static GLuint s_extra_phase_rb[NATIVE_INTERPOLATION_MAX_PHASES - 1u];
static unsigned int s_native_interpolation_denominator = 2u;
static unsigned int s_native_interpolation_phase_count = 1u;
static uint64_t s_native_present_deadline;

static void apply_swap_interval(void) {
    int interval = s_native_interpolation_denominator > 2u
        ? 0 : s_swap_interval;

    if (!s_ctx) return;
    if (SDL_GL_SetSwapInterval(interval) != 0 && interval < 0) {
        SDL_GL_SetSwapInterval(1);
        s_swap_interval = 1;
    }
}
static uint8_t       *s_canonical_digest_pixels = NULL;
static size_t         s_canonical_digest_capacity = 0u;
/* Native raw-1555 sampling mirror + readback source. */
static GLuint        s_raw_tex = 0, s_raw_fbo = 0;
/* CPU->VRAM upload staging (native RGBA8). */
static GLuint        s_up_tex = 0;
/* copy_rect staging (hr-sized RGBA8). */
static GLuint        s_scratch_tex = 0, s_scratch_fbo = 0;

/* Programs. */
static GLuint s_geo_prog = 0, s_geo_vao = 0, s_geo_vbo = 0;
static GLuint s_tex_prog = 0, s_tex_vao = 0, s_tex_vbo = 0;
/* Textured vertex: pos(2) uv(2) col(4) tpage(2) clut(2) depth(1) raw(1),
 * limits(4), semi(1), perspective q(1). q == 0 keeps affine interpolation. */
#define TEXV 20
static GLuint s_blit_prog = 0, s_blit_vao = 0, s_blit_vbo = 0;
static GLuint s_pack_prog = 0, s_stencil_prog = 0, s_empty_vao = 0;

/* One-shot PGXP overrides for the next legacy triangle. Native semantic draws
 * carry their precise positions directly and do not use this side channel. */
static int s_pc_valid;
static float s_pc_x[3], s_pc_y[3];
static int s_pq_valid;
static float s_pq[3];

/* TEX program uniforms. */
static GLint s_uVram = -1, s_uTpage = -1, s_uClut = -1, s_uDepth = -1;
static GLint s_uRaw = -1, s_uSemipass = -1, s_uSemimode = -1;
static GLint s_uTwin = -1, s_uMaskset = -1, s_uFilter = -1;
static GLint s_geo_uDither = -1, s_geo_uScale = -1;
static GLint s_tex_uDither = -1, s_tex_uScale = -1;
static GLint s_uLimits = -1;
/* Native-wide x-projection uniforms (per program). u_xoff = x translation in
 * native px (0 canonical), u_xhalf = x clip half-extent in native px (512
 * canonical). When wide is off these stay 0 / 512 so the canonical pass is
 * bit-identical to the pre-native-wide projection. */
static GLint s_geo_uXoff = -1, s_geo_uXhalf = -1;
static GLint s_tex_uXoff = -1, s_tex_uXhalf = -1;
/* Native-wide 2D-backdrop x-stretch uniforms. The far 2D backdrop layer is a
 * ~4:3-width set of static prims scrolled by the draw offset; native-wide widens
 * 3D via the GTE but never these, so they leave a void in the 16:9 margins. The
 * wide mirror scales them about screen centre (u_xscale, u_xcenter) to fill it.
 * Applied only to "backdrop-phase" prims (drawn before the first clearly-wide
 * prim each frame). 1.0 / 0.0 => no-op, so the canonical pass stays identical. */
static GLint s_geo_uXscale = -1, s_geo_uXcenter = -1;
static GLint s_tex_uXscale = -1, s_tex_uXcenter = -1;
/* Runtime controls (ws_backdrop_stretch debug command). */
int g_ws_bd_stretch_on   = 1;   /* feature on (gated by native-wide + per-prim gate) */
int g_ws_bd_stretch_pct  = 0;   /* 0 = auto (g_wide_w/native_w); else pct/100 */
int g_ws_bd_phase_thresh = 24;  /* "narrow" margin (px): a prim overhanging the 4:3
                                 * region by more than this is treated as already-
                                 * widened GTE geometry and NOT stretched */
int g_ws_bd_phase_mode   = 1;   /* (retained for the debug command; unused since the
                                 * gate is now per-prim !tagged && narrow) */
/* Per-prim 2D-backdrop gate (replaces the old draw-order phase): a prim is
 * stretched iff native-wide + feature on + NOT sprite-tagged (foreground chars/
 * HUD are tagged) + NARROW (the GTE far-parallax/3D extend into the margins).
 * s_bd_gate is what wide_set_bd_scale reads; s_tb_gate is the open textured
 * batch's gate (batch flushes when a prim's gate differs, so a batch is uniform). */
static int s_bd_gate = 0;
static int s_tb_gate = 0;
/* ws_backdrop_stretch diagnostics: per-frame snapshot reported by the command. */
int g_bdg_applied = 0, g_bdg_prims = 0, g_bdg_clearx = -999999;
int g_bdg_cur = 0, g_bdg_base = 0, g_bdg_w = 0, g_bdg_off = 0;
static int s_bdg_applied = 0, s_bdg_prims = 0, s_bdg_clearx = -999999;
/* per-prim draw-order trace (ws_backdrop_trace): x-extent + textured flag, in
 * draw order, for the last frame -- so we can SEE where the background sits. */
typedef struct { short x0, x1, y0, y1; unsigned char tex; } PrimRec;
#define PTRACE_CAP 200
static PrimRec s_ptrace[PTRACE_CAP]; static int s_ptrace_n = 0;
PrimRec g_ptrace[PTRACE_CAP]; int g_ptrace_n = 0;   /* snapshot (extern) */
/* BLIT program uniforms. */
static GLint s_uBlitSrc = -1, s_uBlitPass = -1, s_uBlitMaskset = -1;
static GLint s_uBlitSrcDiv = -1, s_uBlitSrcOff = -1;
static GLint s_uBlitTargetSize = -1;
/* PACK program uniforms. */
static GLint s_uPackHr = -1, s_uPackScale = -1;
static GLint s_uStencilSrc = -1;

static uint32_t     *s_conv = NULL;        /* RGBA8 staging for uploads      */
static int           s_gpu_dirty = 0;      /* CPU VRAM array may be stale    */

/* Dirty-rect unions, native VRAM coords, inclusive bounds. */
typedef struct { int x0, y0, x1, y1, set; } DirtyRect;
static DirtyRect s_pack_dirty;             /* hr FBO content not in raw mirror */
static void native_view_mirror_canonical_rects(const DirtyRect *rects,
                                                 int rect_count);
static void native_midpoint_mirror_canonical_rects(const DirtyRect *rects,
                                                    int rect_count);

/* CPU writes not yet in the FBO — an EXACT rect list, NOT a single union.
 *
 * THE FLICKER CLASS BUG (MMX6 GL black-frame flicker, ISSUES.md #7): the old
 * single-union s_up_pending merged DISJOINT uploads (e.g. a sprite column at
 * x>=320 and a tile row at y>=480) into one bounding box that covered the
 * framebuffers in between; flush_cpu_upload then painted that whole box from
 * the CPU VRAM array — which is STALE under GL (the FBO is authoritative,
 * ensure_cpu only syncs on demand) — stomping freshly-rendered framebuffer
 * content with stale (typically black) pixels for 1-2 presents until the game
 * redrew each buffer. Fix: track the exact uploaded rects and flush each one;
 * only pixels the CPU actually wrote are ever painted. Merging is allowed only
 * when it adds NO uncovered pixels (containment / same-band extension). On
 * overflow the pending set is flushed and the new rect starts a fresh list —
 * order-preserving and still batched for the common poke patterns.
 * (Pre-context, s_raster_ok == 0, the CPU array is fully authoritative — the
 * software rasterizer mirrors every draw — so union-merging is harmless and
 * used as the overflow strategy there.) */
#define UP_RECTS_MAX 16
static DirtyRect s_up_rects[UP_RECTS_MAX];
static int       s_up_nrects = 0;
static uint64_t  s_rt_up_diag[6];

static int runtime_upload_diag_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("PSX_RUNTIME_PERF_DIAG");
        enabled = e && e[0] && e[0] != '0';
    }
    return enabled;
}

void gl_renderer_runtime_diag(uint64_t out[6]) {
    for (int i = 0; i < 6; i++) out[i] = s_rt_up_diag[i];
}

/* Draw state mirrored from the vtable set_* calls. */
static int s_off_x = 0, s_off_y = 0;
static int s_area_x1 = 0, s_area_y1 = 0, s_area_x2 = VRAM_W - 1, s_area_y2 = VRAM_H - 1;
static int s_semi_en = 0, s_semi_mode = 0;
static int s_mod_r = 128, s_mod_g = 128, s_mod_b = 128, s_mod_raw = 0;
static int s_dither = 0;
static int s_mask_set = 0, s_mask_check = 0;
static int s_tw_mask_x = 0, s_tw_mask_y = 0, s_tw_off_x = 0, s_tw_off_y = 0;
static int s_tex_filter = 0;
/* Opaque textured draws carry the exact mask bit in FBO alpha. Keeping the
 * duplicate stencil copy current is deferred until mask checking is requested. */
static int s_stencil_valid = 1;

/* ---- native-wide compositor (mirrors gpu_sw_renderer.c g_wide_*) ----------
 * Canonical VRAM (the hr FBO) stays 100% faithful. Native-wide lives in
 * SEPARATE wide FBOs keyed by framebuffer base_x; each framebuffer-targeting
 * primitive is mirrored into the active wide surface at local x = vram_x -
 * base_x + OFFSET. Present reads the displayed buffer's wide surface. Textures
 * always sample canonical VRAM (s_raw_tex). 4:3 / non-opted games never call
 * wide_configure, so g_wide_cur stays 0 and the wide pass never runs. */
#define WIDE_MAX_SURF 4
static GLuint s_wide_tex[WIDE_MAX_SURF];     /* color tex per surface (0 = free) */
static GLuint s_wide_fbo[WIDE_MAX_SURF];     /* FBO per surface */
static GLuint s_wide_rb[WIDE_MAX_SURF];      /* depth-stencil RB per surface (mask
                                              * mirror, same as hr). PERF-CRITICAL:
                                              * a stencil-less wide FBO made every
                                              * stencil-enabled mirror draw cost
                                              * ~0.6ms of driver work (Tomba2 16:9
                                              * collapsed to 12fps); with the RB
                                              * attached the pass costs ~2us. */
static int    s_wide_base[WIDE_MAX_SURF];    /* base_x per surface (-1 = free) */
static int    g_wide_w        = 0;           /* wide width (native px); 0 = disabled */
static int    g_wide_off      = 0;           /* centering OFFSET (native px) */
static GLuint g_wide_cur      = 0;           /* active mirror FBO (0 = no mirror) */
static int    g_wide_cur_base = 0;           /* base_x of g_wide_cur */
/* Set by gpu_flat_rect for the full-screen-overlay case so the generic
 * gpu_geometry wide mirror is skipped and the flat path emits its own
 * full-wide-width pass instead (mirrors sw_draw_flat_rect). */
static int    s_wide_suppress = 0;

/* Producer-driven Native view. Unlike the legacy wide mirror, these surfaces
 * receive only Native semantic draws and may use source-derived positions. */
#define NATIVE_VIEW_MAX_SURF 4
static GLuint s_native_view_tex[NATIVE_VIEW_MAX_SURF];
static GLuint s_native_view_fbo[NATIVE_VIEW_MAX_SURF];
static GLuint s_native_view_rb[NATIVE_VIEW_MAX_SURF];
static GLuint s_native_midpoint_tex[NATIVE_VIEW_MAX_SURF];
static GLuint s_native_midpoint_fbo[NATIVE_VIEW_MAX_SURF];
static GLuint s_native_midpoint_rb[NATIVE_VIEW_MAX_SURF];
static GLuint s_native_extra_phase_tex[NATIVE_VIEW_MAX_SURF]
                                      [NATIVE_INTERPOLATION_MAX_PHASES - 1u];
static GLuint s_native_extra_phase_fbo[NATIVE_VIEW_MAX_SURF]
                                      [NATIVE_INTERPOLATION_MAX_PHASES - 1u];
static GLuint s_native_extra_phase_rb[NATIVE_VIEW_MAX_SURF]
                                     [NATIVE_INTERPOLATION_MAX_PHASES - 1u];
static int s_native_view_base[NATIVE_VIEW_MAX_SURF] = { -1, -1, -1, -1 };
static int s_native_view_seeded[NATIVE_VIEW_MAX_SURF];
static int s_native_midpoint_seeded[NATIVE_VIEW_MAX_SURF];
static int s_native_extra_phase_seeded[NATIVE_VIEW_MAX_SURF]
                                      [NATIVE_INTERPOLATION_MAX_PHASES - 1u];
static uint64_t s_canonical_geometry_hash[NATIVE_INTERPOLATION_MAX_PHASES + 1u];
static uint32_t s_canonical_geometry_count[NATIVE_INTERPOLATION_MAX_PHASES + 1u];
static uint64_t s_native_view_geometry_hash[NATIVE_INTERPOLATION_MAX_PHASES + 1u];
static uint32_t s_native_view_geometry_count[NATIVE_INTERPOLATION_MAX_PHASES + 1u];
static uint64_t s_pending_canonical_geometry_hash
    [NATIVE_INTERPOLATION_MAX_PHASES + 1u];
static uint32_t s_pending_canonical_geometry_count
    [NATIVE_INTERPOLATION_MAX_PHASES + 1u];
static uint64_t s_pending_native_view_geometry_hash
    [NATIVE_INTERPOLATION_MAX_PHASES + 1u];
static uint32_t s_pending_native_view_geometry_count
    [NATIVE_INTERPOLATION_MAX_PHASES + 1u];
static int s_native_view_enabled;
static int s_native_view_width;
static int s_native_view_offset;
static int s_native_view_canonical_width = 320;
static int s_native_view_canonical_height = 240;
static int s_native_view_pass;
static GLuint s_native_view_pass_fbo;
static int s_native_view_pass_base;
static int s_native_view_expand_x;
static int s_native_view_scale_2d;
static int s_native_view_preserve_2d_translation_x;
static GlRendererNativeMidpointDiagnostics s_native_midpoint_diag;
static int s_native_midpoint_frame_blocked;
static int s_native_midpoint_duplicate_seen;
static int s_native_midpoint_canonical_enabled;
static int s_native_midpoint_current_pending;
static unsigned int s_native_midpoint_pending_phase;
static int s_native_midpoint_pending_slot = -1;
static int s_native_midpoint_pending_x;
static int s_native_midpoint_pending_y;
static int s_native_midpoint_pending_scanout_y;
static int s_native_midpoint_pending_w;
static int s_native_midpoint_pending_h;
static int s_native_midpoint_promoted_y;
static int s_native_midpoint_promoted_scanout_y;
static int s_native_midpoint_promoted_valid;
static GLuint s_midpoint_pass_fbo;
static int s_midpoint_copy_pass;
static int s_native_view_copy_self;
static GLuint s_native_view_copy_source_fbo;
#define NATIVE_CURRENT_VARIANT NATIVE_INTERPOLATION_MAX_PHASES
#define NATIVE_VIEW_WAVE_COLUMNS 20
#define NATIVE_VIEW_WAVE_ROWS 17
#define NATIVE_VIEW_WAVE_PACKET_COUNT \
    (NATIVE_VIEW_WAVE_COLUMNS * NATIVE_VIEW_WAVE_ROWS)
#define NATIVE_VIEW_WAVE_VARIANTS (NATIVE_INTERPOLATION_MAX_PHASES + 1u)
typedef struct NativeViewWaveRow {
    int boundaries[NATIVE_VIEW_WAVE_COLUMNS + 1];
    int left_source_top;
    int left_source_bottom;
    int right_source_top;
    int right_source_bottom;
    int left_top;
    int left_bottom;
    int right_top;
    int right_bottom;
} NativeViewWaveRow;
typedef struct NativeViewWaveTile {
    int left;
    int right;
    int top;
    int bottom;
    int texture_x;
    int texture_y;
    int u;
    int v;
    int draw_top;
    int framebuffer_height;
} NativeViewWaveTile;
typedef struct NativeViewWaveState {
    NativeViewWaveTile tiles[NATIVE_VIEW_WAVE_VARIANTS]
                            [NATIVE_VIEW_WAVE_PACKET_COUNT];
    NativeViewWaveRow rows[NATIVE_VIEW_WAVE_VARIANTS]
                            [NATIVE_VIEW_WAVE_ROWS];
    uint8_t tile_seen[NATIVE_VIEW_WAVE_PACKET_COUNT];
    int packet_count;
    int present_row_count[NATIVE_VIEW_WAVE_VARIANTS];
    int vertical_anchor_source[NATIVE_VIEW_WAVE_VARIANTS][2];
    int packed_vertical_offset[NATIVE_VIEW_WAVE_VARIANTS][2][2];
    int base_x;
    int slot;
    int recording;
    int ready;
} NativeViewWaveState;
static NativeViewWaveState s_native_view_wave;
static GlRendererNativeWaveDiagnostics s_native_view_wave_diag;
static int s_native_view_wave_authenticated;
static int s_native_view_wave_authenticated_base_x;
static int s_native_view_wave_authenticated_slot = -1;
#define NATIVE_HOST_QUEUE_CAP GPU_SEMANTIC_WORKLOAD_CAPACITY
typedef struct NativeHostQueuedSemantic {
    GpuRenderSemantic current;
    GpuRenderSemantic midpoint;
    GpuRenderSemantic extra_phases[NATIVE_INTERPOLATION_MAX_PHASES - 1u];
    int base_x;
    int slot;
    int clear_y;
    int clear_h;
    uint16_t clear_color;
    int clear_margins;
    int midpoint_valid;
    int phase_only;
    int temporal_order_valid;
    uint8_t phase_visibility_mask;
    uint32_t phase_order[NATIVE_INTERPOLATION_MAX_PHASES];
} NativeHostQueuedSemantic;
static NativeHostQueuedSemantic s_native_host_queue[NATIVE_HOST_QUEUE_CAP];
static size_t s_native_host_render_order[NATIVE_HOST_QUEUE_CAP];
static size_t s_native_host_queue_count;
static size_t s_native_host_queue_last_present_count;
static int s_native_host_queue_flushing;
static int s_native_host_queue_midpoint_rendered;
typedef struct NativeHostSemanticHistory {
    GpuRenderSemantic semantic;
    int base_x;
    int slot;
    uint32_t generation;
} NativeHostSemanticHistory;
static NativeHostSemanticHistory
    s_native_host_semantic_history[2][NATIVE_HOST_QUEUE_CAP];
typedef struct NativeHostSemanticContextHistory {
    GpuRenderInterpolationIdentity identity;
    int base_x;
    int slot;
} NativeHostSemanticContextHistory;
static NativeHostSemanticContextHistory
    s_native_host_semantic_context_history[2][NATIVE_HOST_QUEUE_CAP];
static size_t s_native_host_semantic_context_history_count[2];
static uint32_t s_native_host_semantic_history_generation[2];
static unsigned int s_native_host_semantic_history_index;
static int s_native_host_semantic_history_valid;
#define NATIVE_HOST_DIAG_PRIMITIVE_CAP (NATIVE_HOST_QUEUE_CAP * 8u)
static GlRendererSemanticProducerItemDiagnostics
    s_native_host_diag_primitives[NATIVE_HOST_DIAG_PRIMITIVE_CAP];
static uint64_t s_native_host_diag_primitive_total;
#define RETIRED_FAILURE_EVENT_CAP 131072u
#define RETIRED_ISSUE_SCRATCH_CAP \
    (GPU_SEMANTIC_WORKLOAD_CAPACITY * GPU_RENDER_SEMANTIC_TRIANGLE_CAPACITY * 3u)
static GlRendererRetiredFailureEvent
    s_retired_failure_events[RETIRED_FAILURE_EVENT_CAP];
static GpuSemanticWorkloadRetiredIssue
    s_retired_issue_scratch[RETIRED_ISSUE_SCRATCH_CAP];
static uint64_t s_retired_failure_event_count;
static uint64_t s_retired_failure_event_overflow;
#define PRODUCER_DIAG_VERTEX_CAP 32768u
typedef struct ProducerDiagVertex {
    uint64_t scene_id;
    uint32_t group_id;
    uint32_t vertex_id;
    int64_t x;
    int64_t y;
    int used;
} ProducerDiagVertex;
static ProducerDiagVertex s_producer_diag_vertices[PRODUCER_DIAG_VERTEX_CAP];
static size_t s_producer_diag_previous_order[NATIVE_HOST_QUEUE_CAP];
static void wide_free_all(void);
static void native_view_free_all(void);
static int native_view_surface_slot(int base_x, int create);
static void native_view_wave_reset(void);
static void native_view_wave_record(
    const GpuRenderSemantic *semantic,
    const GpuRenderSemantic *phase_semantics, int base_x, int slot);
static int native_view_wave_apply_copy(
    int slot, int base_x, int src_x, int src_y,
    int dst_x, int dst_y, int w, int h);
static GpuRenderTransactionStatus native_host_queue_flush(void);
static GpuRenderTransactionStatus native_host_queue_prepare_present(
    int use_midpoint);
static GpuRenderTransactionStatus native_host_pending_flush(void);
static GpuRenderTransactionStatus native_host_pending_flush_reason(
    unsigned int reason);
static GpuRenderTransactionStatus native_host_queue_push(
    const GpuRenderSemantic *semantic,
    const GpuRenderSemantic *phase_semantics, int base_x, int slot);
static GpuRenderTransactionStatus native_host_queue_push_margin_clear(
    int slot, int y, int h, uint16_t color, int midpoint_valid);
static void native_midpoint_seed_slot(int slot);
static void native_midpoint_seed_canonical(void);

static GLuint native_phase_tex(unsigned int phase) {
    return phase == 0u ? s_midpoint_tex : s_extra_phase_tex[phase - 1u];
}

static GLuint native_phase_fbo(unsigned int phase) {
    return phase == 0u ? s_midpoint_fbo : s_extra_phase_fbo[phase - 1u];
}

static GLuint native_view_phase_tex(int slot, unsigned int phase) {
    return phase == 0u ? s_native_midpoint_tex[slot]
                       : s_native_extra_phase_tex[slot][phase - 1u];
}

static GLuint native_view_phase_fbo(int slot, unsigned int phase) {
    return phase == 0u ? s_native_midpoint_fbo[slot]
                       : s_native_extra_phase_fbo[slot][phase - 1u];
}

static int *native_view_phase_seeded(int slot, unsigned int phase) {
    return phase == 0u ? &s_native_midpoint_seeded[slot]
                       : &s_native_extra_phase_seeded[slot][phase - 1u];
}

typedef struct NativeDrawState {
    int off_x, off_y;
    int area_x1, area_y1, area_x2, area_y2;
    int semi_en, semi_mode;
    int mod_r, mod_g, mod_b, mod_raw;
    int dither;
    int mask_set, mask_check;
    int tw_mask_x, tw_mask_y, tw_off_x, tw_off_y;
    int tex_filter;
} NativeDrawState;

static void native_draw_state_save(NativeDrawState *state) {
    state->off_x = s_off_x;
    state->off_y = s_off_y;
    state->area_x1 = s_area_x1;
    state->area_y1 = s_area_y1;
    state->area_x2 = s_area_x2;
    state->area_y2 = s_area_y2;
    state->semi_en = s_semi_en;
    state->semi_mode = s_semi_mode;
    state->mod_r = s_mod_r;
    state->mod_g = s_mod_g;
    state->mod_b = s_mod_b;
    state->mod_raw = s_mod_raw;
    state->dither = s_dither;
    state->mask_set = s_mask_set;
    state->mask_check = s_mask_check;
    state->tw_mask_x = s_tw_mask_x;
    state->tw_mask_y = s_tw_mask_y;
    state->tw_off_x = s_tw_off_x;
    state->tw_off_y = s_tw_off_y;
    state->tex_filter = s_tex_filter;
}

static void native_draw_state_restore(const NativeDrawState *state) {
    s_off_x = state->off_x;
    s_off_y = state->off_y;
    s_area_x1 = state->area_x1;
    s_area_y1 = state->area_y1;
    s_area_x2 = state->area_x2;
    s_area_y2 = state->area_y2;
    s_semi_en = state->semi_en;
    s_semi_mode = state->semi_mode;
    s_mod_r = state->mod_r;
    s_mod_g = state->mod_g;
    s_mod_b = state->mod_b;
    s_mod_raw = state->mod_raw;
    s_dither = state->dither;
    s_mask_set = state->mask_set;
    s_mask_check = state->mask_check;
    s_tw_mask_x = state->tw_mask_x;
    s_tw_mask_y = state->tw_mask_y;
    s_tw_off_x = state->tw_off_x;
    s_tw_off_y = state->tw_off_y;
    s_tex_filter = state->tex_filter;
    sw_set_draw_area(state->area_x1, state->area_y1,
                     state->area_x2, state->area_y2);
    sw_set_draw_offset(state->off_x, state->off_y);
    sw_set_semi_transparency(state->semi_en, state->semi_mode);
    sw_set_mask_bits(state->mask_set, state->mask_check);
    sw_set_texture_window((uint32_t)state->tw_mask_x |
                          ((uint32_t)state->tw_mask_y << 5u) |
                          ((uint32_t)state->tw_off_x << 10u) |
                          ((uint32_t)state->tw_off_y << 15u));
    sw_set_color_modulation(state->mod_r, state->mod_g,
                            state->mod_b, state->mod_raw);
    sw_set_texture_filter(state->tex_filter);
}

/* X-translation (native px) from canonical VRAM space into the active wide
 * surface: local_x = vram_x - base_x + OFFSET. Same as SW wide_dx(). */
static inline int wide_dx(void) { return g_wide_off - g_wide_cur_base; }

/* ---- dirty-rect helpers ------------------------------------------------- */
static void rect_clear(DirtyRect *r) { r->set = 0; }
static void rect_add(DirtyRect *r, int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > VRAM_W - 1) x1 = VRAM_W - 1;
    if (y1 > VRAM_H - 1) y1 = VRAM_H - 1;
    if (x0 > x1 || y0 > y1) return;
    if (!r->set) { r->x0 = x0; r->y0 = y0; r->x1 = x1; r->y1 = y1; r->set = 1; return; }
    if (x0 < r->x0) r->x0 = x0;
    if (y0 < r->y0) r->y0 = y0;
    if (x1 > r->x1) r->x1 = x1;
    if (y1 > r->y1) r->y1 = y1;
}
static int rect_intersects(const DirtyRect *r, int x0, int y0, int x1, int y1) {
    if (!r->set) return 0;
    return !(x1 < r->x0 || x0 > r->x1 || y1 < r->y0 || y0 > r->y1);
}

/* ---- exact pending-upload rect list (see s_up_rects comment) ------------- */
static void flush_cpu_upload(void);   /* fwd: overflow flushes then re-adds */

/* Add an uploaded rect. Merges ONLY when the merge adds no uncovered pixels:
 * containment either way, or an extension within the same row-band / column-
 * band (equal y-range with touching/overlapping x-ranges, or equal x-range
 * with touching/overlapping y-ranges — the row-scan / column-scan poke
 * patterns). Never unions disjoint rects post-init (that was the flicker bug). */
static void up_add(int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > VRAM_W - 1) x1 = VRAM_W - 1;
    if (y1 > VRAM_H - 1) y1 = VRAM_H - 1;
    if (x0 > x1 || y0 > y1) return;
    for (int i = 0; i < s_up_nrects; i++) {
        DirtyRect *r = &s_up_rects[i];
        if (x0 >= r->x0 && x1 <= r->x1 && y0 >= r->y0 && y1 <= r->y1)
            return;                                   /* contained */
        if (x0 <= r->x0 && x1 >= r->x1 && y0 <= r->y0 && y1 >= r->y1) {
            r->x0 = x0; r->y0 = y0; r->x1 = x1; r->y1 = y1;  /* contains */
            return;
        }
        if (y0 == r->y0 && y1 == r->y1 &&
            x0 <= r->x1 + 1 && x1 >= r->x0 - 1) {     /* same row-band extension */
            if (x0 < r->x0) r->x0 = x0;
            if (x1 > r->x1) r->x1 = x1;
            return;
        }
        if (x0 == r->x0 && x1 == r->x1 &&
            y0 <= r->y1 + 1 && y1 >= r->y0 - 1) {     /* same column-band extension */
            if (y0 < r->y0) r->y0 = y0;
            if (y1 > r->y1) r->y1 = y1;
            return;
        }
    }
    if (s_up_nrects >= UP_RECTS_MAX) {
        if (s_raster_ok) {
            flush_cpu_upload();       /* order-preserving: land the old ones */
        } else {
            /* Pre-context: the CPU array is fully authoritative (software
             * rasterizer mirrors every op), so a union is harmless. */
            DirtyRect *r = &s_up_rects[0];
            for (int i = 1; i < s_up_nrects; i++) {
                if (s_up_rects[i].x0 < r->x0) r->x0 = s_up_rects[i].x0;
                if (s_up_rects[i].y0 < r->y0) r->y0 = s_up_rects[i].y0;
                if (s_up_rects[i].x1 > r->x1) r->x1 = s_up_rects[i].x1;
                if (s_up_rects[i].y1 > r->y1) r->y1 = s_up_rects[i].y1;
            }
            if (x0 < r->x0) r->x0 = x0;
            if (y0 < r->y0) r->y0 = y0;
            if (x1 > r->x1) r->x1 = x1;
            if (y1 > r->y1) r->y1 = y1;
            s_up_nrects = 1;
            return;
        }
    }
    DirtyRect *r = &s_up_rects[s_up_nrects++];
    r->x0 = x0; r->y0 = y0; r->x1 = x1; r->y1 = y1; r->set = 1;
}

/* Add a GP0(A0) transfer's exact touched region. The software reference wraps
 * per pixel (px = (x+col) & 1023, py = (y+row) & 511), so a wrapping transfer
 * touches up to four exact rects — NOT all of VRAM (the old "wrapped: take
 * all" full-VRAM union painted stale CPU content over the framebuffers). */
static void up_add_transfer(int x, int y, int w, int h) {
    x &= VRAM_W - 1; y &= VRAM_H - 1;
    if (w > VRAM_W) w = VRAM_W;
    if (h > VRAM_H) h = VRAM_H;
    if (w <= 0 || h <= 0) return;
    int w1 = w, w2 = 0, h1 = h, h2 = 0;
    if (x + w > VRAM_W) { w1 = VRAM_W - x; w2 = w - w1; }
    if (y + h > VRAM_H) { h1 = VRAM_H - y; h2 = h - h1; }
    up_add(x, y, x + w1 - 1, y + h1 - 1);
    if (w2)       up_add(0, y, w2 - 1, y + h1 - 1);
    if (h2)       up_add(x, 0, x + w1 - 1, h2 - 1);
    if (w2 && h2) up_add(0, 0, w2 - 1, h2 - 1);
}

/* ---- coherency event ring (always-on, debug server "gl_coh_ring") -------- */
/* Every coherency-relevant operation — upload flushes, fills, copies, draw
 * bboxes, packs, full readbacks, presents, and probe perturbations — is
 * recorded with its rect and frame number. Per CLAUDE.md ring-buffer rule:
 * capture is continuous, observers query a window after the fact. Trigger
 * attribution convention: an op that flushes internally (fill/copy/draw/
 * present/peek) records its own event AFTER the FLUSH event it caused, so
 * the event following a FLUSH names the trigger. 16 B * 64 Ki = 1 MB. */
extern uint64_t s_frame_count;  /* defined in debug_server.c */

#define GL_COH_RING_CAP (1u << 16)
static GlCohEvent s_coh_ring[GL_COH_RING_CAP];
static uint64_t   s_coh_seq = 0;

/* 16x16 native-pixel tiles changed since their last on-screen present. This
 * lets a double-buffered 30 Hz game avoid swapping the unchanged front buffer
 * on the intervening 60 Hz vblank without guessing from game identity. */
#define PRES_TILE 16
#define PRES_ROWS (VRAM_H / PRES_TILE)
static uint64_t s_present_dirty[PRES_ROWS];
static int s_last_present_path = -1;
static int s_last_dx, s_last_dy, s_last_dw, s_last_dh;
/* After savestate restore: keep swapping for a few presents even when the
 * display rect is byte-identical to the last swap. Double/triple-buffered
 * windows otherwise can leave a stale back buffer on screen while vblanks
 * (and FPS) keep advancing — especially on a 2nd+ load of the same slot. */
static int s_force_present_remaining = 0;

/* Rollback/rewind hold-last. A drawable capture already contains the composed
 * window image; a native capture is an uncomposed guest display band. */
#define HOLD_NONE     0
#define HOLD_DRAWABLE 1
#define HOLD_NATIVE   2
static int s_hold_kind = HOLD_NONE;
static GLuint s_hold_tex = 0;
static GLuint s_hold_fbo = 0;
static int s_hold_tw = 0, s_hold_th = 0;
static int s_hold_force_4_3 = 0;
static int s_hold_linear = 0;

/* Post-load freeze probe counters. */
static uint64_t s_probe_skip = 0;
static uint64_t s_probe_swap = 0;
static uint64_t s_probe_dirty_marks = 0;

/* A transaction renders into canonical VRAM while retaining one bounded
 * rollback checkpoint. READY keeps that checkpoint through private staging and
 * the one final default-framebuffer blit; non-canonical presents fail closed. */
typedef struct GlTransactionCheckpoint {
    GpuRenderTransactionId id;
    uint64_t vram_serial;
    int committed;
    GpuRenderPresent present;
    GLuint staging_tex, staging_fbo;
    int staging_w, staging_h;
    int aspect_num, aspect_den;
    GlPresEvent staged_present_event;
    uint16_t vram[VRAM_W * VRAM_H];

    int off_x, off_y;
    int area_x1, area_y1, area_x2, area_y2;
    int semi_en, semi_mode;
    int mod_r, mod_g, mod_b, mod_raw, dither;
    int mask_set, mask_check;
    int tw_mask_x, tw_mask_y, tw_off_x, tw_off_y;
    int tex_filter;
    int stencil_valid;

    int gpu_dirty;
    DirtyRect pack_dirty;
    DirtyRect up_rects[UP_RECTS_MAX];
    int up_nrects;
    int depth24_skip_up;
    DirtyRect d24_skip_fb;

    uint64_t present_dirty[PRES_ROWS];
    int last_present_path;
    int last_dx, last_dy, last_dw, last_dh;
    int force_present_remaining;
    uint64_t probe_skip, probe_swap, probe_dirty_marks;

    uint64_t coh_seq;
    uint64_t rt_up_diag[6];
    uint64_t scene_prims, scene_prims_tex;
    uint64_t batch_total, batch_reason[7];
    int bd_gate, tb_gate, wide_suppress;
    int bdg_applied, bdg_prims, bdg_clearx;
    PrimRec ptrace[PTRACE_CAP];
    int ptrace_n;
    int tb_semi, tb_mask, tb_filter, tb_dither, tb_twin[4];
    int fb_semi, fb_mask, fb_dither;
    double cw_flush_ms, cw_wide_ms;
    int cw_batches, cw_wide_sets, cw_wide_cfgs, cw_wide_clears;
    int cw_fbo_creates, cw_flush_depth;
    GpuRenderDeferredCandidateToken deferred_candidate_token;
} GlTransactionCheckpoint;

typedef struct GlDeferredCandidate {
    GpuRenderDeferredCandidateToken token;
    GpuRenderTransactionId visual_id;
    GLuint texture;
    GLuint framebuffer;
    int width;
    int height;
    int scale;
} GlDeferredCandidate;

static GlTransactionCheckpoint *s_transaction = NULL;
static GlDeferredCandidate s_deferred_candidate;
static GpuRenderDeferredCandidateToken s_deferred_candidate_next_token = 1u;
static int s_transaction_force_original = 0;
static GLuint s_transaction_deferred_staging_tex = 0;
static GLuint s_transaction_deferred_staging_fbo = 0;
static int glb_transaction_context_ready(void);
static void glb_transaction_discard_checkpoint(void);
static void glb_transaction_cleanup_deferred_staging(void);
static void glb_deferred_candidate_discard_owned(void);
static int glb_transaction_prepare_original_present(void);
static void glb_transaction_original_presented(void);
static int glb_transaction_abort_pending(int force_original);
static int glb_transaction_reject_other_present(void);

#ifdef PSX_GL_TRANSACTION_TESTING
static GlRendererTransactionTestDiag s_transaction_test_diag;
static int s_transaction_test_fault;

void gl_renderer_transaction_test_reset(void) {
    memset(&s_transaction_test_diag, 0, sizeof(s_transaction_test_diag));
    s_transaction_test_fault = GL_TRANSACTION_FAULT_NONE;
}

void gl_renderer_transaction_test_inject_fault(int phase) {
    s_transaction_test_fault = phase;
}

void gl_renderer_transaction_test_diag(GlRendererTransactionTestDiag *out) {
    if (!out) return;
    *out = s_transaction_test_diag;
    out->pending_commit = s_transaction && s_transaction->committed;
    out->deferred_candidate_active =
        s_deferred_candidate.token != GPU_RENDER_DEFERRED_CANDIDATE_NONE;
}
#endif

static void present_dirty_rect(int x0, int y0, int x1, int y1, int set) {
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 >= VRAM_W) x1 = VRAM_W - 1; if (y1 >= VRAM_H) y1 = VRAM_H - 1;
    if (x0 > x1 || y0 > y1) return;
    int tx0 = x0 / PRES_TILE, tx1 = x1 / PRES_TILE;
    uint64_t mask = (~0ull << tx0) & (~0ull >> (63 - tx1));
    for (int ty = y0 / PRES_TILE; ty <= y1 / PRES_TILE; ty++) {
        if (set) s_present_dirty[ty] |= mask; else s_present_dirty[ty] &= ~mask;
    }
    if (set) s_probe_dirty_marks++;
}

static int present_dirty_test(int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 >= VRAM_W) x1 = VRAM_W - 1; if (y1 >= VRAM_H) y1 = VRAM_H - 1;
    if (x0 > x1 || y0 > y1) return 0;
    int tx0 = x0 / PRES_TILE, tx1 = x1 / PRES_TILE;
    uint64_t mask = (~0ull << tx0) & (~0ull >> (63 - tx1));
    for (int ty = y0 / PRES_TILE; ty <= y1 / PRES_TILE; ty++)
        if (s_present_dirty[ty] & mask) return 1;
    return 0;
}

static void present_force_consumed(void) {
    if (s_force_present_remaining > 0)
        s_force_present_remaining--;
}

static void hold_ensure_tex(int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (!s_hold_tex) {
        glGenTextures(1, &s_hold_tex);
        p_glGenFramebuffers(1, &s_hold_fbo);
    }
    if (s_hold_tw == w && s_hold_th == h) return;
    glBindTexture(GL_TEXTURE_2D, s_hold_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_hold_fbo);
    p_glFramebufferTexture2D(PSXGL_FRAMEBUFFER, PSXGL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, s_hold_tex, 0);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    s_hold_tw = w;
    s_hold_th = h;
}

/* Copy a complete composed target. For FBO 0 this snapshots the backbuffer
 * before SwapWindow; transaction staging can be captured after its swap. */
static void hold_capture_drawable_target(GLuint source_fbo, int w, int h) {
    if (!s_ctx || w < 1 || h < 1) return;
    hold_ensure_tex(w, h);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, source_fbo);
    glReadBuffer(source_fbo ? GL_COLOR_ATTACHMENT0 : GL_BACK);
    glBindTexture(GL_TEXTURE_2D, s_hold_tex);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
    glBindTexture(GL_TEXTURE_2D, 0);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    s_hold_kind = HOLD_DRAWABLE;
    s_hold_force_4_3 = 0;
    s_hold_linear = 0;
}

static void hold_capture_drawable(void) {
    int ww = 0, wh = 0;
    if (!s_ctx || !s_win) return;
    SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    hold_capture_drawable_target(0, ww, wh);
}

/* Interpolation may own SwapWindow, so retain the immutable guest display band
 * instead of attempting to read a concurrently-mutating drawable. */
static void hold_capture_native_fbo(GLuint source_fbo, int x, int y,
                                    int w, int h, int force_4_3, int linear) {
    const int scale = s_scale > 0 ? s_scale : 1;
    if (!s_ctx || !source_fbo || w < 1 || h < 1) return;
    hold_ensure_tex(w * scale, h * scale);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, source_fbo);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, s_hold_fbo);
    glDisable(GL_SCISSOR_TEST);
    p_glBlitFramebuffer(x * scale, y * scale,
                        (x + w) * scale, (y + h) * scale,
                        0, 0, w * scale, h * scale,
                        GL_COLOR_BUFFER_BIT, GL_NEAREST);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
    s_hold_kind = HOLD_NATIVE;
    s_hold_force_4_3 = force_4_3 ? 1 : 0;
    s_hold_linear = linear ? 1 : 0;
}

static void hold_invalidate(void) {
    s_hold_kind = HOLD_NONE;
}

static void coh_record(int kind, int x0, int y0, int x1, int y1) {
    GlCohEvent *e = &s_coh_ring[s_coh_seq % GL_COH_RING_CAP];
    e->frame = (uint32_t)s_frame_count;
    e->kind  = (uint8_t)kind;
    e->x0 = (int16_t)x0; e->y0 = (int16_t)y0;
    e->x1 = (int16_t)x1; e->y1 = (int16_t)y1;
    s_coh_seq++;
    if (kind == GL_COH_FLUSH || kind == GL_COH_FILL ||
        kind == GL_COH_COPY || kind == GL_COH_DRAW)
        present_dirty_rect(x0, y0, x1, y1, 1);
}

uint64_t gl_renderer_coh_total(void) { return s_coh_seq; }
int gl_renderer_coh_get(uint64_t seq, GlCohEvent *out) {
    if (seq >= s_coh_seq) return 0;
    if (s_coh_seq - seq > GL_COH_RING_CAP) return 0;  /* evicted */
    *out = s_coh_ring[seq % GL_COH_RING_CAP];
    return 1;
}

/* ---- present ring (always-on, debug server "present_ring") --------------- */
/* Records every SwapWindow (vram/wide/cpu/blank/interpolated) and its
 * source + letterbox rects. PSX_GL_PRESENT_PROBE=1 additionally drains
 * glGetError and samples one backbuffer pixel before the swap; that synchronous
 * diagnostic is intentionally opt-in. Observers query a window after the fact. */
#define GL_PRES_RING_CAP 4096
static GlPresEvent s_pres_ring[GL_PRES_RING_CAP];
static uint64_t    s_pres_seq = 0;

#define GL_PRESENT_HASH_SLOT_COUNT 32u
#define GL_PRESENT_HASH_FNV_OFFSET UINT64_C(1469598103934665603)
#define GL_PRESENT_HASH_FNV_PRIME UINT64_C(1099511628211)
#define PSXGL_ALREADY_SIGNALED 0x911A
#define PSXGL_CONDITION_SATISFIED 0x911C
typedef struct GlPresentHashSlot {
    GLuint pbo;
    GLsync fence;
    uint64_t sequence;
    size_t capacity;
    size_t bytes;
    uint8_t kind;
} GlPresentHashSlot;
static GlPresentHashSlot s_present_hash_slots[GL_PRESENT_HASH_SLOT_COUNT];
static unsigned int s_present_hash_next;
static int s_present_hash_enabled = -1;
static uint64_t s_present_hash_requested;
static uint64_t s_present_hash_completed;
static uint64_t s_present_hash_dropped;
static uint64_t s_present_source_hash_requested;
static uint64_t s_present_source_hash_completed;
static uint64_t s_present_source_hash_dropped;
static uint64_t s_present_phase_surface_hash_requested;
static uint64_t s_present_phase_surface_hash_completed;
static uint64_t s_present_phase_surface_hash_dropped;
static uint64_t s_present_phase_vram_hash_requested;
static uint64_t s_present_phase_vram_hash_completed;
static uint64_t s_present_phase_vram_hash_dropped;

enum {
    GL_PRESENT_HASH_FRAMEBUFFER = 0,
    GL_PRESENT_HASH_SOURCE,
    GL_PRESENT_HASH_PHASE_SURFACE,
    GL_PRESENT_HASH_PHASE_VRAM,
};

static int pres_probe_pixels_enabled(void) {
    static int probe_pixels = -1;

    if (probe_pixels < 0) {
        const char *cfg = getenv("PSX_GL_PRESENT_PROBE");
        probe_pixels = (cfg && cfg[0] == '1') ? 1 : 0;
    }
    return probe_pixels;
}

static int pres_hash_enabled(void) {
    if (s_present_hash_enabled < 0) {
        const char *cfg = getenv("PSX_GL_PRESENT_HASH");
        s_present_hash_enabled = (cfg && cfg[0] == '1') ? 1 : 0;
    }
    return s_present_hash_enabled;
}

static uint64_t pres_hash_bytes(const uint8_t *bytes, size_t size) {
    uint64_t hash = GL_PRESENT_HASH_FNV_OFFSET;
    size_t index = 0u;

    while (index + sizeof(uint64_t) <= size) {
        uint64_t word;
        memcpy(&word, bytes + index, sizeof(word));
        hash ^= word;
        hash *= GL_PRESENT_HASH_FNV_PRIME;
        hash = (hash << 27u) | (hash >> 37u);
        index += sizeof(word);
    }
    while (index < size) {
        hash = (hash ^ bytes[index]) * GL_PRESENT_HASH_FNV_PRIME;
        ++index;
    }
    return hash;
}

static void pres_set_hash(uint64_t sequence, uint64_t hash) {
    if (sequence < s_pres_seq && s_pres_seq - sequence <= GL_PRES_RING_CAP) {
        GlPresEvent *event = &s_pres_ring[sequence % GL_PRES_RING_CAP];
        event->framebuffer_hash = hash;
        event->framebuffer_hash_valid = 1u;
    }
}

static void pres_set_source_hash(uint64_t sequence, uint64_t hash) {
    if (sequence < s_pres_seq && s_pres_seq - sequence <= GL_PRES_RING_CAP) {
        GlPresEvent *event = &s_pres_ring[sequence % GL_PRES_RING_CAP];
        event->source_hash = hash;
        event->source_hash_valid = 1u;
    }
}

static void pres_set_phase_surface_hash(uint64_t sequence, uint64_t hash) {
    if (sequence < s_pres_seq && s_pres_seq - sequence <= GL_PRES_RING_CAP) {
        GlPresEvent *event = &s_pres_ring[sequence % GL_PRES_RING_CAP];
        event->phase_surface_hash = hash;
        event->phase_surface_hash_valid = 1u;
    }
}

static void pres_set_phase_vram_hash(uint64_t sequence, uint64_t hash) {
    if (sequence < s_pres_seq && s_pres_seq - sequence <= GL_PRES_RING_CAP) {
        GlPresEvent *event = &s_pres_ring[sequence % GL_PRES_RING_CAP];
        event->phase_vram_hash = hash;
        event->phase_vram_hash_valid = 1u;
    }
}

static void pres_hash_collect(void) {
    for (unsigned int index = 0u;
         index < GL_PRESENT_HASH_SLOT_COUNT; ++index) {
        GlPresentHashSlot *slot = &s_present_hash_slots[index];
        GLenum status;
        uint8_t *bytes;

        if (!slot->fence) continue;
        status = p_glClientWaitSync(slot->fence, 0u, 0u);
        if (status != PSXGL_ALREADY_SIGNALED &&
            status != PSXGL_CONDITION_SATISFIED)
            continue;
        p_glDeleteSync(slot->fence);
        slot->fence = NULL;
        p_glBindBuffer(PSXGL_PIXEL_PACK_BUFFER, slot->pbo);
        bytes = (uint8_t *)p_glMapBufferRange(
            PSXGL_PIXEL_PACK_BUFFER, 0, (ptrdiff_t)slot->bytes,
            PSXGL_MAP_READ_BIT);
        if (bytes) {
            const uint64_t hash = pres_hash_bytes(bytes, slot->bytes);
            if (slot->kind == GL_PRESENT_HASH_SOURCE)
                pres_set_source_hash(slot->sequence, hash);
            else if (slot->kind == GL_PRESENT_HASH_PHASE_SURFACE)
                pres_set_phase_surface_hash(slot->sequence, hash);
            else if (slot->kind == GL_PRESENT_HASH_PHASE_VRAM)
                pres_set_phase_vram_hash(slot->sequence, hash);
            else
                pres_set_hash(slot->sequence, hash);
            (void)p_glUnmapBuffer(PSXGL_PIXEL_PACK_BUFFER);
            if (slot->kind == GL_PRESENT_HASH_SOURCE)
                s_present_source_hash_completed++;
            else if (slot->kind == GL_PRESENT_HASH_PHASE_SURFACE)
                s_present_phase_surface_hash_completed++;
            else if (slot->kind == GL_PRESENT_HASH_PHASE_VRAM)
                s_present_phase_vram_hash_completed++;
            else
                s_present_hash_completed++;
        } else {
            if (slot->kind == GL_PRESENT_HASH_SOURCE)
                s_present_source_hash_dropped++;
            else if (slot->kind == GL_PRESENT_HASH_PHASE_SURFACE)
                s_present_phase_surface_hash_dropped++;
            else if (slot->kind == GL_PRESENT_HASH_PHASE_VRAM)
                s_present_phase_vram_hash_dropped++;
            else
                s_present_hash_dropped++;
        }
        p_glBindBuffer(PSXGL_PIXEL_PACK_BUFFER, 0);
        slot->bytes = 0u;
    }
}

static void pres_hash_issue_readback(uint64_t sequence, GLuint source_fbo,
                                     int x, int y, int width, int height,
                                     unsigned int kind) {
    GlPresentHashSlot *slot;
    size_t bytes;

    if (!pres_hash_enabled() || width <= 0 || height <= 0) return;
    pres_hash_collect();
    slot = &s_present_hash_slots[s_present_hash_next];
    s_present_hash_next =
        (s_present_hash_next + 1u) % GL_PRESENT_HASH_SLOT_COUNT;
    if (slot->fence) {
        if (kind == GL_PRESENT_HASH_SOURCE)
            s_present_source_hash_dropped++;
        else if (kind == GL_PRESENT_HASH_PHASE_SURFACE)
            s_present_phase_surface_hash_dropped++;
        else if (kind == GL_PRESENT_HASH_PHASE_VRAM)
            s_present_phase_vram_hash_dropped++;
        else
            s_present_hash_dropped++;
        return;
    }
    bytes = (size_t)width * (size_t)height * 4u;
    if (!slot->pbo) p_glGenBuffers(1, &slot->pbo);
    if (!slot->pbo) {
        if (kind == GL_PRESENT_HASH_SOURCE)
            s_present_source_hash_dropped++;
        else if (kind == GL_PRESENT_HASH_PHASE_SURFACE)
            s_present_phase_surface_hash_dropped++;
        else if (kind == GL_PRESENT_HASH_PHASE_VRAM)
            s_present_phase_vram_hash_dropped++;
        else
            s_present_hash_dropped++;
        return;
    }
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, source_fbo);
    glReadBuffer(source_fbo ? PSXGL_COLOR_ATTACHMENT0 : GL_BACK);
    p_glBindBuffer(PSXGL_PIXEL_PACK_BUFFER, slot->pbo);
    if (slot->capacity != bytes) {
        p_glBufferData(
            PSXGL_PIXEL_PACK_BUFFER, (ptrdiff_t)bytes, NULL,
            PSXGL_STREAM_READ);
        slot->capacity = bytes;
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    p_glBindBuffer(PSXGL_PIXEL_PACK_BUFFER, 0);
    slot->fence = p_glFenceSync(PSXGL_SYNC_GPU_COMMANDS_COMPLETE, 0u);
    if (!slot->fence) {
        if (kind == GL_PRESENT_HASH_SOURCE)
            s_present_source_hash_dropped++;
        else if (kind == GL_PRESENT_HASH_PHASE_SURFACE)
            s_present_phase_surface_hash_dropped++;
        else if (kind == GL_PRESENT_HASH_PHASE_VRAM)
            s_present_phase_vram_hash_dropped++;
        else
            s_present_hash_dropped++;
        return;
    }
    slot->sequence = sequence;
    slot->bytes = bytes;
    slot->kind = (uint8_t)kind;
    if (kind == GL_PRESENT_HASH_SOURCE)
        s_present_source_hash_requested++;
    else if (kind == GL_PRESENT_HASH_PHASE_SURFACE)
        s_present_phase_surface_hash_requested++;
    else if (kind == GL_PRESENT_HASH_PHASE_VRAM)
        s_present_phase_vram_hash_requested++;
    else
        s_present_hash_requested++;
}

static void pres_hash_issue(uint64_t sequence, int width, int height) {
    pres_hash_issue_readback(
        sequence, 0, 0, 0, width, height, GL_PRESENT_HASH_FRAMEBUFFER);
}

static void pres_source_hash_issue(uint64_t sequence, GLuint source_fbo,
                                   int x, int y, int width, int height) {
    if (!source_fbo) return;
    pres_hash_issue_readback(
        sequence, source_fbo, x, y, width, height, GL_PRESENT_HASH_SOURCE);
}

static void pres_phase_surface_hash_issue(
        uint64_t sequence, GLuint source_fbo,
        int x, int y, int width, int height) {
    if (!source_fbo) return;
    pres_hash_issue_readback(
        sequence, source_fbo, x, y, width, height,
        GL_PRESENT_HASH_PHASE_SURFACE);
}

static void pres_phase_vram_hash_issue(
        uint64_t sequence, GLuint source_fbo, int width) {
    if (!source_fbo || width <= 0) return;
    pres_hash_issue_readback(
        sequence, source_fbo, 0, 0, width * s_scale, VRAM_H * s_scale,
        GL_PRESENT_HASH_PHASE_VRAM);
}

static void pres_wayland_feedback(
        const PsxWaylandPresentationEvent *feedback, void *opaque) {
    GlPresEvent *event;
    (void)opaque;

    if (!feedback || feedback->swap_sequence >= s_pres_seq ||
        s_pres_seq - feedback->swap_sequence > GL_PRES_RING_CAP)
        return;
    event = &s_pres_ring[feedback->swap_sequence % GL_PRES_RING_CAP];
    event->presentation_feedback = feedback->presented ? 1u : 2u;
    event->presentation_time_ns = feedback->presentation_time_ns;
    event->refresh_sequence = feedback->refresh_sequence;
    event->refresh_ns = feedback->refresh_ns;
    event->presentation_flags = feedback->flags;
}

static uint64_t pres_record(int path, int dx, int dy, int w, int h,
                            int lx, int ly, int lw, int lh) {
    /* The ring metadata stays always-on, but pixel probing must not: each
     * glReadPixels synchronously drains queued GPU work. Two probes per frame
     * were enough to make Tomba 2 miss its frame budget. */
    int probe_pixels = pres_probe_pixels_enabled();
    const uint64_t sequence = s_pres_seq;
    GlPresEvent *e = &s_pres_ring[sequence % GL_PRES_RING_CAP];
    memset(e, 0, sizeof(*e));
    e->frame = (uint32_t)s_frame_count;
    e->t_ms  = (uint32_t)SDL_GetTicks();
    e->path  = (uint8_t)path;
    e->swap_completed = 0;
    e->phase_numerator = 0;
    e->phase_denominator = 0;
    e->glerr = probe_pixels ? (uint16_t)glGetError() : 0;
    e->dx = (int16_t)dx; e->dy = (int16_t)dy;
    e->w  = (int16_t)w;  e->h  = (int16_t)h;
    e->lx = (int16_t)lx; e->ly = (int16_t)ly;
    e->lw = (int16_t)lw; e->lh = (int16_t)lh;
    /* Backbuffer sample at the letterbox centre (GL bottom-origin; the rects
     * we pass in are already bottom-origin GL window coords). */
    uint8_t px[3] = { 0, 0, 0 };
    if (probe_pixels && lw > 0 && lh > 0) {
        glReadBuffer(GL_BACK);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(lx + lw / 2, ly + lh / 2, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, px);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
    }
    e->px_r = px[0]; e->px_g = px[1]; e->px_b = px[2];
    /* Blit-source sample: the hr FBO pixel at the display-rect centre. Splits
     * "FBO content was black" from "the blit malfunctioned". */
    uint8_t sp[3] = { 0, 0, 0 };
    e->src_valid = 0;
    if (probe_pixels && (path == GL_PRES_VRAM) && w > 0 && h > 0 && s_hr_fbo) {
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_hr_fbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels((dx + w / 2) * s_scale, (dy + h / 2) * s_scale,
                     1, 1, GL_RGB, GL_UNSIGNED_BYTE, sp);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
        e->src_valid = 1;
    }
    e->src_r = sp[0]; e->src_g = sp[1]; e->src_b = sp[2];
    s_pres_seq++;
    return sequence;
}

static void pres_mark_swap_completed(uint64_t sequence) {
    if (sequence < s_pres_seq && s_pres_seq - sequence <= GL_PRES_RING_CAP)
        s_pres_ring[sequence % GL_PRES_RING_CAP].swap_completed = 1;
}

static void pres_set_phase(uint64_t sequence, unsigned int numerator,
                           unsigned int denominator) {
    if (sequence < s_pres_seq && s_pres_seq - sequence <= GL_PRES_RING_CAP) {
        GlPresEvent *event = &s_pres_ring[sequence % GL_PRES_RING_CAP];
        event->phase_numerator = (uint8_t)numerator;
        event->phase_denominator = (uint8_t)denominator;
    }
}

static void pres_set_scanout(uint64_t sequence, int x, int y, int w, int h) {
    if (sequence < s_pres_seq && s_pres_seq - sequence <= GL_PRES_RING_CAP) {
        GlPresEvent *event = &s_pres_ring[sequence % GL_PRES_RING_CAP];
        event->scanout_dx = (int16_t)x;
        event->scanout_dy = (int16_t)y;
        event->scanout_w = (int16_t)w;
        event->scanout_h = (int16_t)h;
    }
}

static void pres_set_geometry_hash(uint64_t sequence, uint64_t hash,
                                   int valid) {
    if (valid && sequence < s_pres_seq &&
        s_pres_seq - sequence <= GL_PRES_RING_CAP) {
        GlPresEvent *event = &s_pres_ring[sequence % GL_PRES_RING_CAP];
        event->geometry_hash = hash;
        event->geometry_hash_valid = 1u;
    }
}

/* Transactional presentation gathers any opt-in pixel probes from staging,
 * but does not make the ring entry visible until after SwapWindow succeeds. */
static void pres_prepare_staged(GlPresEvent *event, GLuint staging_fbo,
                                int dx, int dy, int w, int h,
                                int lx, int ly, int lw, int lh) {
    int probe_pixels = pres_probe_pixels_enabled();
    uint8_t px[3] = { 0, 0, 0 };
    uint8_t sp[3] = { 0, 0, 0 };

    memset(event, 0, sizeof(*event));
    event->path = GL_PRES_VRAM;
    event->dx = (int16_t)dx; event->dy = (int16_t)dy;
    event->w = (int16_t)w; event->h = (int16_t)h;
    event->lx = (int16_t)lx; event->ly = (int16_t)ly;
    event->lw = (int16_t)lw; event->lh = (int16_t)lh;
    if (probe_pixels && lw > 0 && lh > 0) {
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, staging_fbo);
        glReadBuffer(PSXGL_COLOR_ATTACHMENT0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(lx + lw / 2, ly + lh / 2, 1, 1,
                     GL_RGB, GL_UNSIGNED_BYTE, px);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
    }
    event->px_r = px[0]; event->px_g = px[1]; event->px_b = px[2];
    if (probe_pixels && w > 0 && h > 0 && s_hr_fbo) {
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_hr_fbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels((dx + w / 2) * s_scale, (dy + h / 2) * s_scale,
                     1, 1, GL_RGB, GL_UNSIGNED_BYTE, sp);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        event->src_valid = 1;
    }
    event->src_r = sp[0]; event->src_g = sp[1]; event->src_b = sp[2];
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, staging_fbo);
}

static void pres_publish_staged(GlPresEvent *event) {
    event->frame = (uint32_t)s_frame_count;
    event->t_ms = (uint32_t)SDL_GetTicks();
    event->glerr = 0;
    event->swap_completed = 1;
    s_pres_ring[s_pres_seq % GL_PRES_RING_CAP] = *event;
    s_pres_seq++;
}

uint64_t gl_renderer_pres_total(void) { return s_pres_seq; }
int gl_renderer_pres_get(uint64_t seq, GlPresEvent *out) {
    if (seq >= s_pres_seq) return 0;
    if (s_pres_seq - seq > GL_PRES_RING_CAP) return 0;  /* evicted */
    *out = s_pres_ring[seq % GL_PRES_RING_CAP];
    return 1;
}

void gl_renderer_presentation_diagnostics(
        GlRendererPresentationDiagnostics *out_diagnostics) {
    PsxWaylandPresentationDiagnostics wayland = {0};

    if (!out_diagnostics) return;
    psx_wayland_presentation_diagnostics(&wayland);
    memset(out_diagnostics, 0, sizeof(*out_diagnostics));
    out_diagnostics->hash_requested = s_present_hash_requested;
    out_diagnostics->hash_completed = s_present_hash_completed;
    out_diagnostics->hash_dropped = s_present_hash_dropped;
    out_diagnostics->source_hash_requested = s_present_source_hash_requested;
    out_diagnostics->source_hash_completed = s_present_source_hash_completed;
    out_diagnostics->source_hash_dropped = s_present_source_hash_dropped;
    out_diagnostics->phase_surface_hash_requested =
        s_present_phase_surface_hash_requested;
    out_diagnostics->phase_surface_hash_completed =
        s_present_phase_surface_hash_completed;
    out_diagnostics->phase_surface_hash_dropped =
        s_present_phase_surface_hash_dropped;
    out_diagnostics->phase_vram_hash_requested =
        s_present_phase_vram_hash_requested;
    out_diagnostics->phase_vram_hash_completed =
        s_present_phase_vram_hash_completed;
    out_diagnostics->phase_vram_hash_dropped =
        s_present_phase_vram_hash_dropped;
    out_diagnostics->feedback_requested = wayland.requested;
    out_diagnostics->feedback_presented = wayland.presented;
    out_diagnostics->feedback_discarded = wayland.discarded;
    out_diagnostics->feedback_pending = wayland.pending;
    out_diagnostics->presentation_clock_id = wayland.clock_id;
    out_diagnostics->hash_enabled = pres_hash_enabled();
    out_diagnostics->wayland_window = wayland.wayland_window;
    out_diagnostics->presentation_protocol_available =
        wayland.protocol_available;
}

/* ---- shaders ------------------------------------------------------------ */
static const char *PRESENT_VS =
    "#version 330\n"
    "out vec2 v_uv;\n"
    "uniform vec4 u_uv_rect;\n"
    "void main(){ vec2 p = vec2((gl_VertexID<<1)&2, gl_VertexID&2);\n"
    "  v_uv = vec2(mix(u_uv_rect.x,u_uv_rect.z,p.x),\n"
    "              mix(u_uv_rect.y,u_uv_rect.w,1.0-p.y));\n"
    "  gl_Position = vec4(p*2.0-1.0,0.0,1.0); }\n";
static const char *PRESENT_FS =
    "#version 330\n"
    "in vec2 v_uv; uniform sampler2D u_tex; uniform sampler2D u_screenlut;\n"
    "uniform int u_screenlut_on; out vec4 frag;\n"
    "void main(){\n"
    "    frag = texture(u_tex, v_uv);\n"
    "    if (u_screenlut_on == 1) {\n"
    /* Quantize back to the exact source 5-bit channels (both the <<3 and
     * the <<3|>>2 expansions recover exactly under >>3), index the baked
     * BGR555 screen LUT (256x128 RGB8 upload of the CPU scanout table). */
    "        ivec3 q = ivec3(clamp(frag.rgb, 0.0, 1.0) * 255.0 + 0.5) >> 3;\n"
    "        int idx = (q.b << 10) | (q.g << 5) | q.r;\n"
    "        frag.rgb = texelFetch(u_screenlut, ivec2(idx & 255, idx >> 8), 0).rgb;\n"
    "    }\n"
    "}\n";
static const char *INTERP_FS =
    "#version 330\n"
    "in vec2 v_uv; uniform sampler2D u_prev; uniform sampler2D u_curr;\n"
    "uniform float u_alpha; uniform int u_blend_mode; out vec4 frag;\n"
    "void main(){\n"
    "  vec4 prev=texture(u_prev,v_uv), curr=texture(u_curr,v_uv);\n"
    "  float alpha=u_alpha;\n"
    "  if(u_blend_mode==1){\n"
    "    vec3 d=abs(prev.rgb-curr.rgb);\n"
    "    float change=max(max(d.r,d.g),d.b);\n"
    "    float safe_blend=1.0-smoothstep(0.08,0.20,change);\n"
    "    alpha=mix(step(0.5,u_alpha),u_alpha,safe_blend);\n"
    "  }\n"
    "  frag=mix(prev,curr,alpha);\n"
    "}\n";

/* Geometry: position in VRAM pixels (draw offset already applied by gpu.c),
 * color rgb in 0..1, color a = mask bit (0/1). The clip transform is in
 * native VRAM space; the viewport at S* the size scales rasterization.
 *
 * ALL drawn prims shift positions by u_shift = half an HR pixel (0.5/S in
 * native units): GL samples coverage/attributes at pixel CENTERS, the PS1
 * DDA at INTEGER coords. The shift aligns GL's sample grid with the PS1
 * grid — without it, any texture mapping with slope != 1 (scaled sprites,
 * squished menu fonts) samples one texel off per row/column (striped
 * glyphs, seam lines). Half an HR pixel (not half a native pixel!) keeps
 * rect coverage exactly [x*S, (x+w)*S) at every scale AND makes the
 * top-left subpixel of each S*S block sample the exact PS1 value (which is
 * what the PACK pass reads back). */
static const char *GEO_VS =
    "#version 330\n"
    "layout(location=0) in vec2 a_pos;\n"
    "layout(location=1) in vec4 a_col;\n"
    "uniform float u_shift;\n"
    "uniform float u_xoff;   /* native-wide x translation (px); 0 canonical */\n"
    "uniform float u_xhalf;  /* x clip half-extent (px); 512 canonical */\n"
    "uniform float u_xscale; /* native-wide 2D-backdrop x-stretch; 1 canonical */\n"
    "uniform float u_xcenter;/* stretch centre in VRAM px; 0 canonical */\n"
    "noperspective out vec4 v_col;\n"
    "void main(){ v_col = a_col;\n"
    "  float xb = a_pos.x;\n"
    "  if (u_xscale < 0.0) {\n"
    "    float s = -u_xscale; float h = u_xhalf / s;\n"
    "    float l = u_xcenter - h, r = u_xcenter + h;\n"
    "    if (xb < l) xb = l + (xb-l)*s; else if (xb > r) xb = r + (xb-r)*s;\n"
    "  } else xb = (xb - u_xcenter)*u_xscale + u_xcenter;\n"
    "  gl_Position = vec4((xb+u_shift+u_xoff)/u_xhalf - 1.0, (a_pos.y+u_shift)/256.0 - 1.0, 0.0, 1.0); }\n";

/* One literal PS1 color endpoint for both geometry programs. Inputs are in the
 * GPU's pre-quantization integer domain: RGB888 for untextured shading, and
 * (texel5 * modulation8) >> 4 for modulated textures. gl_FragCoord is the final
 * VRAM destination, so the matrix phase includes GP0(E5) draw offset exactly. */
#define PSX_DITHER_QUANTIZE_GLSL \
    "const int PSX_DITHER[16] = int[16](\n" \
    "  -4, 0,-3, 1,  2,-2, 3,-1, -3, 1,-4, 0,  3,-1, 2,-2);\n" \
    "ivec3 psx_quantize(ivec3 color, int dither_on){\n" \
    "  ivec2 p = ivec2(gl_FragCoord.xy) / u_scale;\n" \
    "  int d = dither_on != 0 ? PSX_DITHER[((p.y & 3) << 2) | (p.x & 3)] : 0;\n" \
    "  return clamp((color + ivec3(d)) >> 3, ivec3(0), ivec3(31));\n" \
    "}\n" \
    "vec3 psx_expand5(ivec3 color5){ return vec3(color5 << 3) / 255.0; }\n"

static const char *GEO_FS =
    "#version 330\n"
    "noperspective in vec4 v_col; out vec4 frag;\n"
    "uniform int u_dither; uniform int u_scale;\n"
    PSX_DITHER_QUANTIZE_GLSL
    "void main(){\n"
    "  ivec3 color8 = ivec3(clamp(v_col.rgb * 255.0 + 0.5, 0.0, 255.0));\n"
    "  frag = vec4(psx_expand5(psx_quantize(color8, u_dither)), v_col.a);\n"
    "}\n";

/* Textured prims: sample raw 1555 VRAM (integer), CLUT decode per depth,
 * texture window, optional bilinear, texel-0 discard, STP-split discard,
 * PS1 *2-around-0x80 modulation. Output alpha = bit15 of the written pixel.
 * Texel coords use floor() to match the software rasterizer's truncation
 * (rounding shifted sampling +1 texel half the time: smeared text). */
/* Textured program. Per-prim texture state (texpage, clut, depth, raw, uv
 * limits) is carried in FLAT vertex attributes — constant across a prim's
 * vertices — instead of uniforms, so consecutive textured prims with the same
 * blend/mask/texture-window state batch into one draw (see flush_tex_batch).
 * The remaining uniforms (u_twin/u_maskset/u_filter/u_semipass) are the batch
 * keys + per-pass state. */
static const char *TEX_VS =
    "#version 330\n"
    "layout(location=0) in vec2 a_pos;\n"
    "layout(location=1) in vec2 a_uv;\n"
    "layout(location=2) in vec4 a_col;\n"
    "layout(location=3) in vec2 a_tpage;\n"
    "layout(location=4) in vec2 a_clut;\n"
    "layout(location=5) in float a_depth;\n"
    "layout(location=6) in float a_raw;\n"
    "layout(location=7) in vec4 a_limits;\n"
    "layout(location=8) in float a_semi;\n"
    "layout(location=9) in float a_q;\n"
    "uniform float u_shift;\n"
    "uniform float u_xoff;   /* native-wide x translation (px); 0 canonical */\n"
    "uniform float u_xhalf;  /* x clip half-extent (px); 512 canonical */\n"
    "uniform float u_xscale; /* native-wide 2D-backdrop x-stretch; 1 canonical */\n"
    "uniform float u_xcenter;/* stretch centre in VRAM px; 0 canonical */\n"
    "noperspective out vec2 v_uv; smooth out vec2 v_uv_p;\n"
    "noperspective out vec4 v_col; flat out int v_persp;\n"
    "flat out ivec2 v_tpage; flat out ivec2 v_clut; flat out int v_depth;\n"
    "flat out int v_raw; flat out ivec4 v_limits; flat out int v_semi;\n"
    "void main(){ v_uv = a_uv; v_uv_p = a_uv; v_col = a_col;\n"
    "  v_persp = a_q > 0.0 ? 1 : 0;\n"
    "  v_tpage = ivec2(a_tpage + 0.5); v_clut = ivec2(a_clut + 0.5);\n"
    "  v_depth = int(a_depth + 0.5); v_raw = int(a_raw + 0.5);\n"
    "  v_semi = int(a_semi + 0.5);\n"
    "  v_limits = ivec4(floor(a_limits + 0.5));\n"
    "  /* u_shift: align GL's center-sample grid with the PS1 integer grid (see\n"
    "   * GEO_VS) so interpolated uv at a fragment equals the PS1 DDA value. */\n"
    "  float xb = a_pos.x;\n"
    "  if (u_xscale < 0.0) {\n"
    "    float s = -u_xscale; float h = u_xhalf / s;\n"
    "    float l = u_xcenter - h, r = u_xcenter + h;\n"
    "    if (xb < l) xb = l + (xb-l)*s; else if (xb > r) xb = r + (xb-r)*s;\n"
    "  } else xb = (xb - u_xcenter)*u_xscale + u_xcenter;\n"
    "  float w = a_q > 0.0 ? 1.0/a_q : 1.0;\n"
    "  vec2 ndc = vec2((xb+u_shift+u_xoff)/u_xhalf - 1.0,\n"
    "                  (a_pos.y+u_shift)/256.0 - 1.0);\n"
    "  gl_Position = vec4(ndc*w, 0.0, w); }\n";
static const char *TEX_FS =
    "#version 330\n"
    "noperspective in vec2 v_uv; smooth in vec2 v_uv_p;\n"
    "noperspective in vec4 v_col; flat in int v_persp;\n"
    "out vec4 frag; out vec4 blend_factor;\n"
    "flat in ivec2 v_tpage;   /* texture page base, VRAM px */\n"
    "flat in ivec2 v_clut;    /* CLUT base, VRAM px */\n"
    "flat in int v_depth;     /* 0=4bit 1=8bit 2=15bit */\n"
    "flat in int v_raw;       /* 1 = no color modulation */\n"
    "flat in ivec4 v_limits;  /* prim uv sampling bounds (inclusive, post-wrap) */\n"
    "flat in int v_semi;      /* GP0 command has semi-transparency enabled */\n"
    "uniform usampler2D u_vram;\n"
    "uniform int u_semipass;  /* 0=all texels, 1=STP=0 only, 2=STP=1 only */\n"
    "uniform int u_semimode;  /* PS1 blend mode; drives dual-source factors */\n"
    "uniform ivec4 u_twin;    /* texture window: mask_x, mask_y, off_x, off_y */\n"
    "uniform int u_maskset;   /* GP0(E6h) set-mask: OR bit15 into output */\n"
    "uniform int u_filter;    /* 1 = bilinear */\n"
    "uniform int u_dither;   /* effective per-primitive GP0(E1) dither */\n"
    "uniform int u_scale;    /* HR samples per native VRAM pixel */\n"
    PSX_DITHER_QUANTIZE_GLSL
    "int vram_at(int x, int y){\n"
    "  return int(texelFetch(u_vram, ivec2(x & 1023, y & 511), 0).r);\n"
    "}\n"
    "int fetch_texel(int u, int v){\n"
    "  u &= 255; v &= 255;\n"
    "  if ((u_twin.x | u_twin.y) != 0) {\n"
    "    u = (u & ~(u_twin.x * 8)) | ((u_twin.z & u_twin.x) * 8);\n"
    "    v = (v & ~(u_twin.y * 8)) | ((u_twin.w & u_twin.y) * 8);\n"
    "  } else {\n"
    "    u = clamp(u, v_limits.x, v_limits.z);\n"
    "    v = clamp(v, v_limits.y, v_limits.w);\n"
    "  }\n"
    "  if (v_depth == 0) {\n"
    "    int px = vram_at(v_tpage.x + (u >> 2), v_tpage.y + v);\n"
    "    return vram_at(v_clut.x + ((px >> ((u & 3) * 4)) & 0xF), v_clut.y);\n"
    "  } else if (v_depth == 1) {\n"
    "    int px = vram_at(v_tpage.x + (u >> 1), v_tpage.y + v);\n"
    "    return vram_at(v_clut.x + ((px >> ((u & 1) * 8)) & 0xFF), v_clut.y);\n"
    "  }\n"
    "  return vram_at(v_tpage.x + u, v_tpage.y + v);\n"
    "}\n"
    "ivec3 col5i(int raw){\n"
    "  return ivec3(raw & 31, (raw >> 5) & 31, (raw >> 10) & 31);\n"
    "}\n"
    "vec3 col5(int raw){\n"
    "  return vec3(col5i(raw)) / 31.0;\n"
    "}\n"
    "void main(){\n"
    "  vec2 uv = v_persp != 0 ? v_uv_p : v_uv;\n"
    "  int stp; vec3 rgb = vec3(0.0); ivec3 nearest5 = ivec3(0);\n"
    "  if (u_filter == 0) {\n"
    "    int raw = fetch_texel(int(floor(uv.x)), int(floor(uv.y)));\n"
    "    if (raw == 0) discard;\n"
    "    nearest5 = col5i(raw);\n"
    "    stp = (raw >> 15) & 1;\n"
    "  } else {\n"
    "    /* Bilinear, Beetle-PSX formulation: the NEAREST texel is the base\n"
    "     * (cutout + STP authority), the neighbours lie toward the sub-texel\n"
    "     * offset and clamp to u_limits, and each texel's weight is gated by\n"
    "     * its opacity with the colour renormalised — so prim edges and\n"
    "     * cutout borders keep their colour instead of dissolving into the\n"
    "     * transparent (black) neighbour and discarding whole edge columns. */\n"
    "    int iu = int(floor(uv.x)), iv = int(floor(uv.y));\n"
    "    float fx = uv.x - float(iu) - 0.5, fy = uv.y - float(iv) - 0.5;\n"
    "    int sx = fx < 0.0 ? -1 : 1, sy = fy < 0.0 ? -1 : 1;\n"
    "    fx = abs(fx); fy = abs(fy);\n"
    "    int c00 = fetch_texel(iu, iv);\n"
    "    int c10 = fetch_texel(iu + sx, iv);\n"
    "    int c01 = fetch_texel(iu, iv + sy);\n"
    "    int c11 = fetch_texel(iu + sx, iv + sy);\n"
    "    float w00 = (c00 == 0 ? 0.0 : 1.0) * (1.0 - fx) * (1.0 - fy);\n"
    "    float w10 = (c10 == 0 ? 0.0 : 1.0) * fx * (1.0 - fy);\n"
    "    float w01 = (c01 == 0 ? 0.0 : 1.0) * (1.0 - fx) * fy;\n"
    "    float w11 = (c11 == 0 ? 0.0 : 1.0) * fx * fy;\n"
    "    float opac = w00 + w10 + w01 + w11;\n"
    "    if (opac < 0.5) discard;\n"
    "    rgb = (col5(c00)*w00 + col5(c10)*w10 + col5(c01)*w01 + col5(c11)*w11) / opac;\n"
    "    float stpf = (float((c00 >> 15) & 1) * w00 + float((c10 >> 15) & 1) * w10\n"
    "                + float((c01 >> 15) & 1) * w01 + float((c11 >> 15) & 1) * w11) / opac;\n"
    "    stp = stpf >= 0.5 ? 1 : 0;\n"
    "  }\n"
    "  if (u_semipass == 1 && stp == 1) discard;\n"
    "  if (u_semipass == 2 && stp == 0) discard;\n"
    "  ivec3 color5;\n"
    "  if (u_filter == 0) {\n"
    "    if (v_raw != 0) color5 = nearest5;\n"
    "    else {\n"
    "      ivec3 color8 = ivec3(clamp(v_col.rgb * 255.0 + 0.5, 0.0, 255.0));\n"
    "      color5 = psx_quantize((nearest5 * color8) >> 4, u_dither);\n"
    "    }\n"
    "  } else {\n"
    "    if (v_raw == 0) rgb = clamp(rgb * v_col.rgb * 2.0, 0.0, 1.0);\n"
    "    ivec3 color8 = ivec3(clamp(rgb * 255.0 + 0.5, 0.0, 255.0));\n"
    "    color5 = psx_quantize(color8, v_raw == 0 ? u_dither : 0);\n"
    "  }\n"
    "  rgb = psx_expand5(color5);\n"
    "  float dst_factor = 0.0;\n"
    "  if (u_semimode == 4 && v_semi != 0 && stp != 0) {\n"
    "    dst_factor = v_semi == 1 ? 0.5 : 1.0;\n"
    "    if (v_semi == 1) rgb *= 0.5; else if (v_semi == 4) rgb *= 0.25;\n"
    "  }\n"
    "  frag = vec4(rgb, (stp == 1 || u_maskset == 1) ? 1.0 : 0.0);\n"
    "  blend_factor = vec4(0.0, 0.0, 0.0, dst_factor);\n"
    "}\n";

#undef PSX_DITHER_QUANTIZE_GLSL

/* Quad blit: used for CPU->VRAM upload flushes and VRAM->VRAM copies.
 * Samples an RGBA8 source (alpha = bit15), splits by STP for the stencil
 * write, optionally ORs set-mask into the output alpha. */
static const char *BLIT_VS =
    "#version 330\n"
    "layout(location=0) in vec2 a_pos;   /* native VRAM px */\n"
    "uniform float u_shift;\n"
    "uniform ivec2 u_target_size;\n"
    "void main(){\n"
    "  vec2 half_size = vec2(u_target_size) * 0.5;\n"
    "  gl_Position = vec4((a_pos+u_shift)/half_size - 1.0, 0.0, 1.0); }\n";
static const char *BLIT_FS =
    "#version 330\n"
    "out vec4 frag;\n"
    "uniform sampler2D u_src;\n"
    "uniform int u_stp_pass;  /* 0=all, 1=bit15=0 only, 2=bit15=1 only */\n"
    "uniform int u_maskset;\n"
    "uniform int u_src_div;   /* fragcoord -> src texel divisor (S for native-res\n"
    "                            sources, 1 for hr-res sources) */\n"
    "uniform ivec2 u_src_off; /* added after the divide, in src texel units */\n"
    "void main(){\n"
    "  /* Exact integer source fetch — no normalized-uv edge precision. */\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "  vec4 c = texelFetch(u_src, p / u_src_div + u_src_off, 0);\n"
    "  bool stp = c.a >= 0.5;\n"
    "  if (u_stp_pass == 1 && stp) discard;\n"
    "  if (u_stp_pass == 2 && !stp) discard;\n"
    "  frag = vec4(c.rgb, (stp || u_maskset != 0) ? 1.0 : 0.0);\n"
    "}\n";

/* Pack: re-encode the hr FBO into the native R16UI raw mirror. Runs over a
 * native-res viewport with scissor = dirty rect; each native pixel takes the
 * top-left sample of its S*S block (the sample at the exact native coord). */
static const char *PACK_VS =
    "#version 330\n"
    "void main(){ vec2 p = vec2((gl_VertexID<<1)&2, gl_VertexID&2);\n"
    "  gl_Position = vec4(p*2.0-1.0, 0.0, 1.0); }\n";
static const char *PACK_FS =
    "#version 330\n"
    "uniform sampler2D u_hr;\n"
    "uniform int u_scale;\n"
    "out uint o_pix;\n"
    "void main(){\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "  vec4 c = texelFetch(u_hr, p * u_scale, 0);\n"
    "  uint r = uint(c.r * 255.0 + 0.5) >> 3;\n"
    "  uint g = uint(c.g * 255.0 + 0.5) >> 3;\n"
    "  uint b = uint(c.b * 255.0 + 0.5) >> 3;\n"
    "  o_pix = r | (g << 5) | (b << 10) | (c.a >= 0.5 ? 0x8000u : 0u);\n"
    "}\n";

/* Rebuild stencil bit 0 from a copied RGBA target's alpha. Color writes are
 * disabled while this shader runs; only alpha>=0.5 fragments replace stencil. */
static const char *STENCIL_FS =
    "#version 330\n"
    "uniform sampler2D u_src; out vec4 frag;\n"
    "void main(){ vec4 c=texelFetch(u_src,ivec2(gl_FragCoord.xy),0);\n"
    "  if(c.a<0.5) discard; frag=vec4(0.0); }\n";

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint s = p_glCreateShader(type);
    p_glShaderSource(s, 1, &src, NULL);
    p_glCompileShader(s);
    GLint ok = 0; p_glGetShaderiv(s, PSXGL_COMPILE_STATUS, &ok);
    if (!ok) { char log[1024]; log[0]=0; p_glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stdout, "psxrecomp: GL shader compile failed: %s\n", log);
        p_glDeleteShader(s); return 0; }
    return s;
}
static GLuint build_program_ex(const char *vs, const char *fs, int dual_source) {
    GLuint v = compile_shader(PSXGL_VERTEX_SHADER, vs), f = compile_shader(PSXGL_FRAGMENT_SHADER, fs);
    if (!v || !f) return 0;
    GLuint p = p_glCreateProgram();
    p_glAttachShader(p, v); p_glAttachShader(p, f);
    if (dual_source) {
        p_glBindFragDataLocationIndexed(p, 0, 0, "frag");
        p_glBindFragDataLocationIndexed(p, 0, 1, "blend_factor");
    }
    p_glLinkProgram(p);
    p_glDeleteShader(v); p_glDeleteShader(f);
    GLint ok = 0; p_glGetProgramiv(p, PSXGL_LINK_STATUS, &ok);
    if (!ok) { char log[1024]; log[0]=0; p_glGetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stdout, "psxrecomp: GL program link failed: %s\n", log); return 0; }
    return p;
}
static GLuint build_program(const char *vs, const char *fs) {
    return build_program_ex(vs, fs, 0);
}

/* ---- pixel conversion (PS1 1555: bit15=mask, B[14:10] G[9:5] R[4:0]) ---- */
static inline uint32_t conv_1555_to_rgba8(uint16_t p) {
    uint32_t r = (p & 0x1F) << 3, g = ((p >> 5) & 0x1F) << 3, b = ((p >> 10) & 0x1F) << 3;
    uint32_t a = (p >> 15) & 1 ? 0xFF : 0;
    return r | (g << 8) | (b << 16) | (a << 24);   /* RGBA8 little-endian */
}

/* ---- mask-bit stencil --------------------------------------------------- *
 * Stencil bit0 mirrors bit15 of every pixel.
 *   check off: test ALWAYS, op REPLACE with ref = write value.
 *   check on:  test EQUAL 0 (pass iff dest unmasked). GL couples the REPLACE
 *              value to the test reference, so write a 1 via INVERT (the
 *              stored value is known to be 0 when the test passed) and a 0
 *              via KEEP. */
static void mask_stencil(int write_val) {
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0x01);
    if (s_mask_check) {
        glStencilFunc(GL_EQUAL, 0, 0x01);
        glStencilOp(GL_KEEP, GL_KEEP, write_val ? GL_INVERT : GL_KEEP);
    } else {
        glStencilFunc(GL_ALWAYS, write_val ? 1 : 0, 0x01);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    }
}
/* Like mask_stencil but never checks (uploads: gpu.c already applied mask). */
static void plain_stencil(int write_val) {
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0x01);
    glStencilFunc(GL_ALWAYS, write_val ? 1 : 0, 0x01);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
}

/* PS1 semi-transparency as fixed-function blending, RGB only — the alpha
 * channel (mask bit) is always replaced by the source fragment's alpha.
 *   0: B/2 + F/2   1: B + F   2: B - F   3: B + F/4 */
static void apply_psx_blend(int mode) {
    glEnable(GL_BLEND);
    p_glBlendEquationSeparate((mode & 3) == 2 ? PSXGL_FUNC_REVERSE_SUBTRACT
                                              : PSXGL_FUNC_ADD,
                              PSXGL_FUNC_ADD);
    switch (mode & 3) {
    case 0:
        p_glBlendColor(0.5f, 0.5f, 0.5f, 0.5f);
        p_glBlendFuncSeparate(PSXGL_CONSTANT_ALPHA, PSXGL_CONSTANT_ALPHA, GL_ONE, GL_ZERO);
        break;
    case 1:
    case 2:
        p_glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ZERO);
        break;
    case 3:
        p_glBlendColor(0.25f, 0.25f, 0.25f, 0.25f);
        p_glBlendFuncSeparate(PSXGL_CONSTANT_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
        break;
    }
}

/* ---- hr FBO render-state bracket ---------------------------------------- */
static void hr_begin(int clip_to_draw_area) {
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER,
                        s_midpoint_pass_fbo ? s_midpoint_pass_fbo :
                        (s_native_view_pass ? s_native_view_pass_fbo : s_hr_fbo));
    glViewport(0, 0,
               (s_native_view_pass ? s_native_view_width : VRAM_W) * s_scale,
               VRAM_H * s_scale);
    glEnable(GL_SCISSOR_TEST);
    if (clip_to_draw_area) {
        int sx = s_area_x1;
        int sw = s_area_x2 - s_area_x1 + 1;
        int sh = s_area_y2 - s_area_y1 + 1;
        if (s_native_view_pass) {
            if (s_native_view_scale_2d ==
                    GPU_RENDER_SCREEN_SPACE_2D_STRETCH) {
                const int local_left = s_area_x1 - s_native_view_pass_base;
                const int local_right = s_area_x2 -
                                        s_native_view_pass_base + 1;
                const int64_t left =
                    (int64_t)local_left * s_native_view_width;
                const int64_t right =
                    (int64_t)local_right * s_native_view_width;

                sx = left >= 0
                    ? (int)(left / s_native_view_canonical_width)
                    : -(int)((-left + s_native_view_canonical_width - 1) /
                             s_native_view_canonical_width);
                {
                    const int scaled_right = right >= 0
                        ? (int)((right + s_native_view_canonical_width - 1) /
                               s_native_view_canonical_width)
                        : -(int)((-right) / s_native_view_canonical_width);
                    sw = scaled_right - sx;
                }
                if (sx < 0) {
                    sw += sx;
                    sx = 0;
                }
                if (sx + sw > s_native_view_width)
                    sw = s_native_view_width - sx;
            } else if (s_native_view_scale_2d ==
                           GPU_RENDER_SCREEN_SPACE_2D_PRESERVE_SIZE) {
                const int base_right = s_native_view_pass_base +
                                       s_native_view_canonical_width - 1;

                if (s_area_x1 <= s_native_view_pass_base &&
                    s_area_x2 >= base_right) {
                    sx = 0;
                    sw = s_native_view_width;
                } else {
                    sx = s_area_x1 +
                         s_native_view_preserve_2d_translation_x;
                    sw = s_area_x2 - s_area_x1 + 1;
                    if (sx < 0) {
                        sw += sx;
                        sx = 0;
                    }
                    if (sx + sw > s_native_view_width)
                        sw = s_native_view_width - sx;
                }
            } else if (s_native_view_expand_x) {
                sx = 0;
                sw = s_native_view_width;
            } else {
                const int base_right = s_native_view_pass_base +
                                       s_native_view_canonical_width - 1;

                if (s_area_x1 <= s_native_view_pass_base &&
                    s_area_x2 >= base_right) {
                    sx = 0;
                    sw = s_native_view_width;
                } else {
                    const int right = s_area_x2 - s_native_view_pass_base +
                                      s_native_view_offset;

                    sx = s_area_x1 - s_native_view_pass_base +
                         s_native_view_offset;
                    if (sx < 0) sx = 0;
                    sw = right - sx + 1;
                    if (sx + sw > s_native_view_width)
                        sw = s_native_view_width - sx;
                }
            }
        }
        if (sw < 0) sw = 0; if (sh < 0) sh = 0;
        glScissor(sx * s_scale, s_area_y1 * s_scale,
                  sw * s_scale, sh * s_scale);
    }
}
static void hr_end(void) {
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    p_glBindVertexArray(0);
    p_glUseProgram(0);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
}

static int native_midpoint_active(void) {
    return s_native_midpoint_diag.frame_open &&
           s_native_midpoint_diag.frame_valid &&
           !s_native_midpoint_current_pending &&
           s_native_midpoint_canonical_enabled && s_midpoint_fbo != 0;
}

static int native_midpoint_gl_ok(uint32_t operation) {
    GLenum error;
    int ok = 1;

    while ((error = glGetError()) != GL_NO_ERROR) {
        ok = 0;
        s_native_midpoint_diag.gl_error_count++;
        s_native_midpoint_diag.last_gl_error = (uint32_t)error;
        s_native_midpoint_diag.last_gl_operation = operation;
    }
    return ok;
}

/* Keep the host-only midpoint target exact for mutations that have no
 * interpolable geometry. Copying final canonical pixels also preserves the
 * mask-bit stencil companion without exposing this target to guest VRAM. */
static void native_midpoint_mirror_canonical_rects(const DirtyRect *rects,
                                                    int rect_count) {
    if (!native_midpoint_active()) return;
    if (!rects || rect_count <= 0 || !s_hr_fbo) {
        gl_renderer_native_midpoint_cancel();
        return;
    }
    for (int index = 0; index < rect_count; ++index) {
        const DirtyRect *rect = &rects[index];
        const int width = rect->x1 - rect->x0 + 1;
        const int height = rect->y1 - rect->y0 + 1;

        if (!rect->set || rect->x0 < 0 || rect->y0 < 0 ||
            rect->x1 >= VRAM_W || rect->y1 >= VRAM_H ||
            width <= 0 || height <= 0) {
            gl_renderer_native_midpoint_cancel();
            return;
        }
        for (unsigned int phase = 0u;
             phase < s_native_interpolation_phase_count; ++phase) {
            p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_hr_fbo);
            p_glBindFramebuffer(
                PSXGL_DRAW_FRAMEBUFFER, native_phase_fbo(phase));
            glDisable(GL_SCISSOR_TEST);
            p_glBlitFramebuffer(rect->x0 * s_scale, rect->y0 * s_scale,
                                (rect->x1 + 1) * s_scale,
                                (rect->y1 + 1) * s_scale,
                                rect->x0 * s_scale, rect->y0 * s_scale,
                                (rect->x1 + 1) * s_scale,
                                (rect->y1 + 1) * s_scale,
                                GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT,
                                GL_NEAREST);
        }
    }
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
    if (!native_midpoint_gl_ok(GL_NATIVE_MIDPOINT_GL_MIRROR_RECTS))
        gl_renderer_native_midpoint_cancel();
}

static void native_midpoint_mirror_wrapped_rect(int x, int y, int w, int h) {
    DirtyRect rects[4];
    int count = 0;
    int first_width;
    int first_height;

    if (!native_midpoint_active() || w <= 0 || h <= 0) return;
    x &= VRAM_W - 1;
    y &= VRAM_H - 1;
    if (w > VRAM_W) w = VRAM_W;
    if (h > VRAM_H) h = VRAM_H;
    first_width = w < VRAM_W - x ? w : VRAM_W - x;
    first_height = h < VRAM_H - y ? h : VRAM_H - y;
    rects[count++] = (DirtyRect){ x, y, x + first_width - 1,
                                  y + first_height - 1, 1 };
    if (first_width < w)
        rects[count++] = (DirtyRect){ 0, y, w - first_width - 1,
                                      y + first_height - 1, 1 };
    if (first_height < h)
        rects[count++] = (DirtyRect){ x, 0, x + first_width - 1,
                                      h - first_height - 1, 1 };
    if (first_width < w && first_height < h)
        rects[count++] = (DirtyRect){ 0, 0, w - first_width - 1,
                                      h - first_height - 1, 1 };
    native_midpoint_mirror_canonical_rects(rects, count);
    if (s_native_midpoint_diag.frame_open) {
        s_native_midpoint_diag.nonsemantic_fills++;
    }
}

/* ---- coherency: CPU -> GPU upload flush --------------------------------- */
/* CPU-side VRAM writes (GP0 A0 transfers, DMA, single pixel pokes) land in
 * the CPU array immediately and accumulate s_up_rects. Flushing before the
 * next GPU op (or readback/present) preserves PS1 command order. */
static void flush_cpu_upload(void) {
    if (s_native_host_queue_flushing) return;
    if (!s_raster_ok || s_up_nrects == 0) return;
    if (native_host_pending_flush_reason(1u) !=
        GPU_RENDER_TRANSACTION_OK) return;
    const int diag = runtime_upload_diag_enabled();
    if (diag) { s_rt_up_diag[0]++; s_rt_up_diag[1] += (uint64_t)s_up_nrects; }
    flush_flat_batch();  /* queued flat GEO before upload mutates VRAM */
    flush_tex_batch();   /* queued textured draws before this upload writes VRAM */
    /* Snapshot + clear first (re-entrancy safe; up_add overflow calls back in). */
    DirtyRect rects[UP_RECTS_MAX];
    int nrects = s_up_nrects;
    memcpy(rects, s_up_rects, (size_t)nrects * sizeof(DirtyRect));
    s_up_nrects = 0;

    /* Stage every rect's CPU data first (texture uploads outside the FBO
     * bracket), then draw all the quads in one bracket. Only the exact
     * uploaded rects are painted — never the union bounding box (stale-CPU
     * flicker class bug, see s_up_rects). */
    for (int i = 0; i < nrects; i++) {
        int x = rects[i].x0, y = rects[i].y0;
        int w = rects[i].x1 - rects[i].x0 + 1;
        int h = rects[i].y1 - rects[i].y0 + 1;
        if (diag) s_rt_up_diag[2] += (uint64_t)w * (uint64_t)h;
        coh_record(GL_COH_FLUSH, x, y, x + w - 1, y + h - 1);

        /* RGBA8 staging for the hr quad draw. */
        uint64_t t0 = diag ? SDL_GetPerformanceCounter() : 0;
        for (int row = 0; row < h; row++) {
            const uint16_t *src = s_vram + (size_t)(y + row) * VRAM_W + x;
            uint32_t *dst = s_conv + (size_t)row * w;
            for (int col = 0; col < w; col++) dst[col] = conv_1555_to_rgba8(src[col]);
        }
        if (diag) s_rt_up_diag[3] += SDL_GetPerformanceCounter() - t0;
        t0 = diag ? SDL_GetPerformanceCounter() : 0;
        glBindTexture(GL_TEXTURE_2D, s_up_tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, s_conv);

        /* Raw mirror takes the CPU data directly — current for this rect, so no
         * pack is needed for uploaded content. */
        glBindTexture(GL_TEXTURE_2D, s_raw_tex);
        glPixelStorei(PSXGL_UNPACK_ROW_LENGTH, VRAM_W);
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h,
                        PSXGL_RED_INTEGER, GL_UNSIGNED_SHORT,
                        s_vram + (size_t)y * VRAM_W + x);
        glPixelStorei(PSXGL_UNPACK_ROW_LENGTH, 0);
        if (diag) s_rt_up_diag[4] += SDL_GetPerformanceCounter() - t0;
    }

    /* Quads into the hr FBO; two passes split by bit15 so the stencil mirror
     * stays exact. gpu.c applied mask set/check per pixel already — no check
     * here, the data is final. up_tex is VRAM-aligned: src texel = frag/S. */
    uint64_t draw_t0 = diag ? SDL_GetPerformanceCounter() : 0;
    hr_begin(0);
    glDisable(GL_BLEND);
    p_glUseProgram(s_blit_prog);
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_up_tex);
    p_glUniform1i(s_uBlitSrc, 0);
    p_glUniform1i(s_uBlitMaskset, 0);
    p_glUniform1i(s_uBlitSrcDiv, s_scale);
    p_glUniform2i(s_uBlitSrcOff, 0, 0);
    p_glUniform2i(s_uBlitTargetSize, VRAM_W, VRAM_H);
    p_glBindVertexArray(s_blit_vao);
    p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_blit_vbo);
    for (int i = 0; i < nrects; i++) {
        int x = rects[i].x0, y = rects[i].y0;
        int w = rects[i].x1 - rects[i].x0 + 1;
        int h = rects[i].y1 - rects[i].y0 + 1;
        glScissor(x * s_scale, y * s_scale, w * s_scale, h * s_scale);
        float fx0 = (float)x, fy0 = (float)y, fx1 = (float)(x + w), fy1 = (float)(y + h);
        float verts[6 * 2] = {
            fx0, fy0,  fx1, fy0,  fx0, fy1,
            fx1, fy0,  fx0, fy1,  fx1, fy1,
        };
        p_glBufferData(PSXGL_ARRAY_BUFFER, sizeof verts, verts, PSXGL_STREAM_DRAW);
        plain_stencil(0); p_glUniform1i(s_uBlitPass, 1); glDrawArrays(GL_TRIANGLES, 0, 6);
        plain_stencil(1); p_glUniform1i(s_uBlitPass, 2); glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    hr_end();
    native_midpoint_mirror_canonical_rects(rects, nrects);
    if (s_native_midpoint_diag.frame_open) {
        s_native_midpoint_diag.nonsemantic_uploads++;
    }
    native_view_mirror_canonical_rects(rects, nrects);
    if (diag) s_rt_up_diag[5] += SDL_GetPerformanceCounter() - draw_t0;
}

/* Recreate one target's stencil mask from its authoritative alpha channel.
 * Sampling an attached render target is undefined, so copy color to the shared
 * scratch texture first. Wide targets never exceed the 1024-pixel VRAM width. */
static void rebuild_target_stencil(GLuint target_fbo, int target_w, int target_h) {
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, target_fbo);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, s_scratch_fbo);
    p_glBlitFramebuffer(0, 0, target_w, target_h, 0, 0, target_w, target_h,
                        GL_COLOR_BUFFER_BIT, GL_NEAREST);

    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, target_fbo);
    glViewport(0, 0, target_w, target_h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0x01);
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    glStencilFunc(GL_ALWAYS, 1, 0x01);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    p_glUseProgram(s_stencil_prog);
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_scratch_tex);
    p_glUniform1i(s_uStencilSrc, 0);
    p_glBindVertexArray(s_empty_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_STENCIL_TEST);
    p_glBindVertexArray(0);
    p_glUseProgram(0);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
}

static void rebuild_mask_stencils(void) {
    if (s_stencil_valid || !s_raster_ok) return;
    int hw = VRAM_W * s_scale, hh = VRAM_H * s_scale;
    rebuild_target_stencil(s_hr_fbo, hw, hh);
    for (unsigned int phase = 0u;
         phase < s_native_interpolation_phase_count; ++phase)
        if (native_phase_fbo(phase))
            rebuild_target_stencil(native_phase_fbo(phase), hw, hh);
    for (int i = 0; i < WIDE_MAX_SURF; i++) {
        if (s_wide_fbo[i])
            rebuild_target_stencil(s_wide_fbo[i], g_wide_w * s_scale, hh);
    }
    for (int i = 0; i < NATIVE_VIEW_MAX_SURF; ++i) {
        if (s_native_view_fbo[i])
            rebuild_target_stencil(s_native_view_fbo[i],
                                   s_native_view_width * s_scale, hh);
        for (unsigned int phase = 0u;
             phase < s_native_interpolation_phase_count; ++phase)
            if (native_view_phase_fbo(i, phase))
                rebuild_target_stencil(native_view_phase_fbo(i, phase),
                                       s_native_view_width * s_scale, hh);
    }
    s_stencil_valid = 1;
}

/* ---- coherency: hr FBO -> raw mirror (pack) ------------------------------ */
static void pack_flush(void) {
    if (!s_raster_ok || !s_pack_dirty.set) return;
    if (!s_native_host_queue_flushing &&
        native_host_pending_flush_reason(2u) !=
            GPU_RENDER_TRANSACTION_OK)
        return;
    int x = s_pack_dirty.x0, y = s_pack_dirty.y0;
    int w = s_pack_dirty.x1 - s_pack_dirty.x0 + 1;
    int h = s_pack_dirty.y1 - s_pack_dirty.y0 + 1;
    rect_clear(&s_pack_dirty);
    coh_record(GL_COH_PACK, x, y, x + w - 1, y + h - 1);

    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_raw_fbo);
    glViewport(0, 0, VRAM_W, VRAM_H);
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y, w, h);
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    p_glUseProgram(s_pack_prog);
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_hr_tex);
    p_glUniform1i(s_uPackHr, 0);
    p_glUniform1i(s_uPackScale, s_scale);
    p_glBindVertexArray(s_empty_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    p_glBindVertexArray(0);
    p_glUseProgram(0);
    glDisable(GL_SCISSOR_TEST);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
}

/* Make sure the raw mirror is current for a textured draw that samples the
 * given texture page / CLUT. */
static void flush_pack_if_sampling(int tpage_x, int tpage_y, int depth,
                                   int clut_x, int clut_y) {
    if (s_native_host_queue_flushing) return;
    if (!s_pack_dirty.set) return;
    int page_w = depth == 0 ? 64 : depth == 1 ? 128 : 256;  /* VRAM columns */
    if (rect_intersects(&s_pack_dirty, tpage_x, tpage_y,
                        tpage_x + page_w - 1, tpage_y + 255)) {
        if (native_host_pending_flush_reason(3u) !=
            GPU_RENDER_TRANSACTION_OK) return;
        flush_flat_batch();
        flush_tex_batch();   /* queued draws are part of s_pack_dirty — realise them before packing */
        pack_flush(); return;
    }
    if (depth <= 1) {
        int n = depth == 0 ? 16 : 256;
        if (rect_intersects(&s_pack_dirty, clut_x, clut_y, clut_x + n - 1, clut_y)) {
            if (native_host_pending_flush_reason(3u) !=
                GPU_RENDER_TRANSACTION_OK) return;
            flush_flat_batch();
            flush_tex_batch();
            pack_flush();
        }
    }
}

/* ---- coherency: GPU -> CPU readback -------------------------------------- */
static void ensure_cpu(void) {
    if (!s_raster_ok || !s_gpu_dirty) return;
    if (s_cpu_auth_dual) {
        s_gpu_dirty = 0;
        return;
    }
    flush_flat_batch();
    flush_tex_batch();   /* realise queued textured draws before reading the FBO back */
    flush_cpu_upload();
    pack_flush();
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_raw_fbo);
    glReadPixels(0, 0, VRAM_W, VRAM_H, PSXGL_RED_INTEGER, GL_UNSIGNED_SHORT, s_vram);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    s_gpu_dirty = 0;
    coh_record(GL_COH_ENSURE, 0, 0, VRAM_W - 1, VRAM_H - 1);
}

/* ---- GPU primitives ------------------------------------------------------ */

static uint64_t s_scene_prims = 0;     /* frame_perf: scene primitives submitted (pre double-draw) */
static uint64_t s_scene_prims_tex = 0; /* frame_perf: of which textured (vs flat geometry)         */
static void flush_tex_batch(void);     /* fwd: drained at the backdrop-phase boundary below */
static void mark_prim_dirty(const int *xs, const int *ys, int n, int textured) {
    s_scene_prims++;
    int x0 = xs[0], x1 = xs[0], y0 = ys[0], y1 = ys[0];
    for (int i = 1; i < n; i++) {
        if (xs[i] < x0) x0 = xs[i]; if (xs[i] > x1) x1 = xs[i];
        if (ys[i] < y0) y0 = ys[i]; if (ys[i] > y1) y1 = ys[i];
    }
    if (s_native_view_pass || s_midpoint_pass_fbo) return;
    s_bdg_prims++;   /* dbg: prims seen this frame (gate is now per-prim, see bd_prim_gate) */
    if (s_ptrace_n < PTRACE_CAP) {
        PrimRec *p = &s_ptrace[s_ptrace_n++];
        p->x0 = (short)x0; p->x1 = (short)x1; p->y0 = (short)y0; p->y1 = (short)y1;
        p->tex = (unsigned char)textured;
    }
    if (x0 < s_area_x1) x0 = s_area_x1;
    if (y0 < s_area_y1) y0 = s_area_y1;
    if (x1 > s_area_x2) x1 = s_area_x2;
    if (y1 > s_area_y2) y1 = s_area_y2;
    rect_add(&s_pack_dirty, x0, y0, x1, y1);
    if (!s_cpu_auth_dual) s_gpu_dirty = 1;
    coh_record(GL_COH_DRAW, x0, y0, x1, y1);
}

/* ---- native-wide mirror pass plumbing ----------------------------------- *
 * Re-issue an already-set-up draw (program/VAO/VBO/blend/stencil bound) into
 * the active wide surface. The geometry positions are identical on the host
 * side; the x translation (wide_dx) and the wider clip are applied entirely in
 * the vertex shader via u_xoff / u_xhalf, so the SAME glDrawArrays produces the
 * shifted copy. wide_target_begin binds the wide FBO + viewport + scissor and
 * sets the projection uniforms; wide_target_end restores u_xoff=0/u_xhalf=512
 * on the active program so the next canonical pass is bit-identical. The
 * caller stays inside hr_begin/hr_end; hr_end unbinds and the next hr_begin
 * resets the viewport, so no viewport restore is needed mid-function.
 *
 * The scissor X is the FULL wide surface (NOT the draw area): the SW reference
 * (rt_wide()) deliberately lets the shifted geometry fill the revealed 16:9
 * margins that lie OUTSIDE the game's 4:3 draw-area x-range; scissoring X to
 * the (translated) draw area would crop exactly the margin content native-wide
 * exists to reveal. The scissor Y stays clamped to the DRAW AREA, exactly like
 * rt_wide() (t.cy1/cy2 = g_clip_y1/y2): native-wide only widens X. A full-height
 * Y scissor let draws that canonically clip at a vertical double-buffer band
 * boundary (MMX6: draw area alternates y=0/y=240, both bands in ONE wide
 * surface) bleed into the OTHER band's rows — presented one frame later as
 * top/bottom edge flicker (16:9 GL only). */
static void wide_target_begin(int dx, GLint uXoff, GLint uXhalf) {
    if (s_ws_ablate != 3)   /* ablate 3: no FBO rebind (draws land in hr — perf probe) */
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER, g_wide_cur);
    glViewport(0, 0, g_wide_w * s_scale, VRAM_H * s_scale);
    glEnable(GL_SCISSOR_TEST);
    {
        int sy = s_area_y1, sh = s_area_y2 - s_area_y1 + 1;
        if (sy < 0) { sh += sy; sy = 0; }
        if (sy + sh > VRAM_H) sh = VRAM_H - sy;
        if (sh < 0) sh = 0;
        glScissor(0, sy * s_scale, g_wide_w * s_scale, sh * s_scale);
    }
    p_glUniform1f(uXoff, (float)dx);
    p_glUniform1f(uXhalf, (float)g_wide_w / 2.0f);
}
static void wide_target_end(GLint uXoff, GLint uXhalf) {
    p_glUniform1f(uXoff, 0.0f);
    p_glUniform1f(uXhalf, 512.0f);
    if (s_ws_ablate != 3)
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_hr_fbo);
}

static void native_view_projection_uniforms(GLint uXoff, GLint uXhalf) {
    p_glUniform1f(uXoff, 0.0f);
    p_glUniform1f(uXhalf, s_native_view_pass
        ? (float)s_native_view_width / 2.0f : 512.0f);
}

extern int psx_ws_prim_is_tagged(void);   /* gpu.c: is the current GP0 prim sprite-tagged? */
extern int psx_ws_prim_in_backdrop(void); /* gpu.c: is its source addr in the flower-field struct? */
extern int gpu_ws_nw_flat_backdrop_enabled(void); /* gpu.c: per-title flat backdrop opt-in */

/* Per-prim gate: stretch this prim iff native-wide + feature on AND the prim's
 * source address is inside the flower-field backdrop data structure (precise —
 * excludes the 3D rock/foreground, which is untagged AND has narrow prims so the
 * earlier tag/narrow heuristic tore it). mode!=0 falls back to the old
 * tag+narrow heuristic for A/B. */
static int bd_prim_gate(const int *xs, int n, int textured) {
    if (g_wide_w <= 0 || !g_ws_bd_stretch_on) return 0;
    /* Some games draw their authored 4:3 sky/water as flat-colour polygons.
     * Stretch those only in the native-wide mirror: the canonical framebuffer
     * remains byte-for-byte 4:3, while the flat backdrop reaches the reveal
     * margins. Opt-in because flat foreground geometry is title-dependent. */
    if (!textured && gpu_ws_nw_flat_backdrop_enabled()) return 1;
    if (g_ws_bd_phase_mode != 0) return psx_ws_prim_in_backdrop();  /* default: precise address gate */
    /* mode 0: legacy tag+narrow heuristic (kept for comparison) */
    int native_w = g_wide_w - 2 * g_wide_off;
    if (native_w <= 0) return 0;
    if (psx_ws_prim_is_tagged()) return 0;
    int base = g_wide_cur_base, lo = xs[0], hi = xs[0];
    for (int i = 1; i < n; i++) { if (xs[i] < lo) lo = xs[i]; if (xs[i] > hi) hi = xs[i]; }
    if (lo < base - g_ws_bd_phase_thresh) return 0;             /* into left margin -> GTE-wide */
    if (hi > base + native_w + g_ws_bd_phase_thresh) return 0;  /* into right margin -> GTE-wide */
    return 1;
}

/* ---- native-wide FAST path (skip redundant center mirror) ----------------- *
 * The wide surface's CENTRE columns [g_wide_off, g_wide_off+native_w) are, by
 * construction, identical to the canonical 4:3 framebuffer. So instead of
 * re-rasterizing every primitive into the wide surface (the "mirror" pass — the
 * dominant native-wide GPU cost, ~2x scene fill), we copy the canonical centre
 * into the wide surface once at present (blit_wide_center_from_canonical), and
 * the per-prim mirror only needs to produce the reveal MARGINS. Any prim/batch
 * whose x-range is fully inside the 4:3 frame contributes nothing to the margins,
 * so its mirror is skipped entirely. Correctness does not depend on the skip
 * being precise: the centre is authoritatively overwritten by the blit, so the
 * ONLY requirement is that a margin-reaching prim is NOT skipped — hence the
 * conservative strict-inside test. 4:3 never runs any of this (g_wide_cur == 0).
 * Toggle via gl_wide_fast for A/B; default ON. */
static int s_wide_fast = 1;
void gl_renderer_set_wide_fast(int on) { s_wide_fast = on ? 1 : 0; }
int  gl_renderer_get_wide_fast(void) { return s_wide_fast; }
static void wide_blit_center(GLuint wide_fbo, int base_x, int disp_y, int disp_h); /* def below */
/* True if [lo,hi] (canonical draw-x) lies strictly inside the 4:3 frame, so the
 * prim adds nothing to either reveal margin and its mirror can be skipped. */
static int mirror_x_center_only(int lo, int hi) {
    if (!s_wide_fast) return 0;
    int base = g_wide_cur_base, native_w = g_wide_w - 2 * g_wide_off;
    if (native_w <= 0) return 0;
    return (lo >= base) && (hi < base + native_w);
}
static int mirror_geo_center_only(const int *xs, int n) {
    if (!s_wide_fast) return 0;
    int lo = xs[0], hi = xs[0];
    for (int i = 1; i < n; i++) { if (xs[i] < lo) lo = xs[i]; if (xs[i] > hi) hi = xs[i]; }
    return mirror_x_center_only(lo, hi);
}
/* mirror_batch_center_only (textured-batch variant) is defined after s_tb below. */

/* Set / clear the 2D-backdrop x-stretch for a wide-mirror draw, per the current
 * gate (s_bd_gate, set by the caller from bd_prim_gate / the batch gate). */
static void wide_set_bd_scale(GLint uScale, GLint uCenter) {
    extern int g_ws_tex_edge_pct;
    float scale = 1.0f, center = 0.0f;
    if (s_bd_gate && g_ws_bd_stretch_on && g_wide_w > 0) {
        int native_w = g_wide_w - 2 * g_wide_off;
        if (native_w > 0) {
            scale  = g_ws_bd_stretch_pct > 0 ? (float)g_ws_bd_stretch_pct / 100.0f
                                             : (float)g_wide_w / (float)native_w;
            if (s_bd_gate == 2) {
                scale = g_ws_tex_edge_pct > 0
                      ? (float)g_ws_tex_edge_pct / 100.0f : scale;
                scale = -scale; /* shader: expand only beyond canonical edges */
            }
            center = (float)g_wide_cur_base + (float)native_w / 2.0f;
        }
    }
    if (scale != 1.0f) s_bdg_applied++;
    p_glUniform1f(uScale, scale);
    p_glUniform1f(uCenter, center);
}
static void wide_clear_bd_scale(GLint uScale, GLint uCenter) {
    p_glUniform1f(uScale, 1.0f);
    p_glUniform1f(uCenter, 0.0f);
}

/* ---- textured-prim batching -------------------------------------------- *
 * Consecutive textured prims sharing blend/mask/texwindow/filter coalesce into
 * one draw. Per-prim texture state (texpage/clut/depth/raw/uv-limits) rides in
 * the vertex (TEXV flat attributes), so only `semi` (blend) and the global
 * mask/twin/filter are batch keys. flush_tex_batch() draws the queued verts; it
 * is called before any op that reads VRAM, writes it outside the batch, or
 * changes batch state (see callers: flush_cpu_upload, flush_pack_if_sampling,
 * every non-textured glb_ wrapper, and the present path). Drawing reads only the
 * already-coherent texture (per-prim coherency was ensured at append time), so
 * flush_tex_batch never re-enters those helpers. */
#define TEXBATCH_MAXV 8190                 /* multiple of 3; ~2730 tris */
static float s_tb[TEXBATCH_MAXV * TEXV];
static int   s_tb_n = 0;                    /* verts queued */
static int   s_tb_semi = -2;
static int   s_tb_mask = 0, s_tb_filter = 0, s_tb_dither = 0;
static int   s_tb_twin[4] = {0, 0, 0, 0};
static uint64_t s_batch_total = 0, s_batch_reason[7];

void gl_renderer_batch_diag(uint64_t out[8]) {
    out[0] = s_batch_total;
    for (int i = 0; i < 7; i++) out[i + 1] = s_batch_reason[i];
}

/* Draw the queued textured batch with correct PSX mask-bit handling AND correct
 * painter's order. The mask bit lives in both the colour-attachment alpha
 * (frag.a) and the stencil; STP=1 texels must always set it.
 *
 * OPAQUE batch (semi < 0): the STP bit does NOT gate COLOUR (every texel is
 * opaque), so colour is drawn in ONE ordered pass over all texels — frag.a still
 * carries the per-texel mask bit, so the alpha mask is correct — followed by a
 * COLOUR-MASKED pass that only fixes the STENCIL for STP=1 texels. The old code
 * split colour into STP=0 (pass 1) then STP=1 (pass 2) across the WHOLE batch,
 * which let a behind prim's STP=1 texels overwrite a front prim's STP=0 colour
 * (the Tomba character drew behind an AP-block's letters / a save post on GL
 * only). Same draw count, order preserved, mask preserved.
 *
 * SEMI batch (semi >= 0): genuine two-pass — STP=0 texels opaque, STP=1 texels
 * blended per the PSX mode. Cross-prim order is kept by isolating semi prims to
 * one per batch (see gpu_textured_triangle), so this batch holds a single prim
 * whose two passes do not self-overlap. */
static void tex_batch_draw_passes(int nverts, int semi) {
    p_glUniform1i(s_uSemimode, semi < 0 ? 0 : semi);
    if (semi < 0) {
        glDisable(GL_BLEND);
        mask_stencil(s_tb_mask);
        p_glUniform1i(s_uSemipass, 0);                 /* all texels, one ordered colour pass */
        glDrawArrays(GL_TRIANGLES, 0, nverts);
        if (s_mask_check) {
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);  /* stencil-only fixup */
            mask_stencil(1);
            p_glUniform1i(s_uSemipass, 2);             /* STP=1 texels set the mask bit */
            glDrawArrays(GL_TRIANGLES, 0, nverts);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        } else if (!s_tb_mask) {
            /* Alpha is already exact; defer its duplicate stencil encoding
             * until a later GP0(E6h) actually enables destination masking. */
            s_stencil_valid = 0;
        }
    } else if (!s_mask_check && semi == 4) {
        /* Modes 0/1/3 can select opaque-vs-semi behavior per fragment with
         * dual-source factors, so the whole painter-ordered batch is one draw.
         * Mode 2 needs a different blend equation and stays on the conservative
         * isolated path below. */
        glEnable(GL_BLEND);
        p_glBlendEquationSeparate(PSXGL_FUNC_ADD, PSXGL_FUNC_ADD);
        p_glBlendFuncSeparate(GL_ONE, PSXGL_SRC1_ALPHA, GL_ONE, GL_ZERO);
        if (s_tb_mask) mask_stencil(1); else glDisable(GL_STENCIL_TEST);
        p_glUniform1i(s_uSemipass, 0);
        glDrawArrays(GL_TRIANGLES, 0, nverts);
        if (!s_tb_mask) s_stencil_valid = 0;
    } else {
        glDisable(GL_BLEND);                           /* Pass 1: STP=0 texels (opaque) */
        mask_stencil(s_tb_mask);
        p_glUniform1i(s_uSemipass, 1);
        glDrawArrays(GL_TRIANGLES, 0, nverts);
        apply_psx_blend(semi);                         /* Pass 2: STP=1 texels (blended) */
        mask_stencil(1);
        p_glUniform1i(s_uSemipass, 2);
        glDrawArrays(GL_TRIANGLES, 0, nverts);
    }
}

/* ---- frame_perf CPU attribution (native-wide wedge hunt) ----------------- *
 * Per-frame CPU wall time spent inside the GL submission paths (driver CPU
 * cost surfaces INSIDE our gl* calls) + counters for the wide plumbing, so a
 * CPU-bound wide frame (emu_cpu >> scene_gpu) can be attributed without a
 * sampling profiler. Reset at present enter; reported by frame_perf. */
static double cw_ms(void) {
    static double freq = 0.0;
    if (freq == 0.0) {
        uint64_t f = SDL_GetPerformanceFrequency();
        freq = f ? (double)f : 1.0;
    }
    return (double)SDL_GetPerformanceCounter() * 1000.0 / freq;
}
static double s_cw_flush_ms = 0.0;   /* CPU wall inside flush_tex_batch        */
static double s_cw_wide_ms  = 0.0;   /* CPU wall inside glb_wide_* entry points */
static int    s_cw_batches = 0, s_cw_wide_sets = 0, s_cw_wide_cfgs = 0,
              s_cw_wide_clears = 0, s_cw_fbo_creates = 0, s_cw_flush_depth = 0;

/* Textured-batch variant of mirror_x_center_only: scan the queued verts' x
 * (attr 0, stride TEXV). Defined here so s_tb / TEXV are in scope. */
static int mirror_batch_center_only(int nverts) {
    if (!s_wide_fast || nverts <= 0) return 0;
    int lo = (int)s_tb[0], hi = (int)s_tb[0];
    for (int i = 1; i < nverts; i++) {
        int x = (int)s_tb[i * TEXV];
        if (x < lo) lo = x; if (x > hi) hi = x;
    }
    return mirror_x_center_only(lo, hi);
}

/* fwd: depth24_upload_policy is defined below; flush_tex_batch calls it to
 * close a gap where a textured draw — not a VRAM write/transfer — is the
 * first thing to touch the screen after depth24 exits (see the big comment
 * on depth24_upload_policy for the full rationale). */
static void depth24_upload_policy(void);

static void flush_tex_batch(void) {
    if (s_tb_n == 0) return;
    int nverts = s_tb_n, semi = s_tb_semi;
    s_tb_n = 0;                             /* clear first: re-entrancy safe */
    double cw_t0 = cw_ms();
    s_cw_batches++; s_batch_total++; s_cw_flush_depth++;

    depth24_upload_policy();
    hr_begin(1);
    p_glUseProgram(s_tex_prog);
    native_view_projection_uniforms(s_tex_uXoff, s_tex_uXhalf);
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_raw_tex);
    p_glUniform1i(s_uVram, 0);
    p_glUniform4i(s_uTwin, s_tb_twin[0], s_tb_twin[1], s_tb_twin[2], s_tb_twin[3]);
    p_glUniform1i(s_uMaskset, s_tb_mask);
    p_glUniform1i(s_uFilter, s_tb_filter);
    p_glUniform1i(s_tex_uDither, s_tb_dither);
    p_glUniform1i(s_tex_uScale, s_scale);
    p_glBindVertexArray(s_tex_vao);
    p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_tex_vbo);
    p_glBufferData(PSXGL_ARRAY_BUFFER, (ptrdiff_t)(nverts * TEXV * sizeof(float)), s_tb, PSXGL_STREAM_DRAW);

    tex_batch_draw_passes(nverts, semi);

    /* Native-wide mirror — skipped for a batch fully inside the 4:3 frame (its
     * centre content comes from the present-time canonical blit; nothing to add
     * to the margins). A backdrop-stretched batch (s_tb_gate) widens past the
     * frame, so it is never treated as centre-only. */
    if (!s_midpoint_pass_fbo && g_wide_cur && s_ws_ablate != 1 &&
        !(s_tb_gate == 0 && mirror_batch_center_only(nverts))) {   /* native-wide mirror */
        int dx = wide_dx();
        s_bd_gate = s_tb_gate;              /* this batch is uniform-gate (flushed on change) */
        gl_perf_mirror_begin();
        wide_target_begin(dx, s_tex_uXoff, s_tex_uXhalf);
        wide_set_bd_scale(s_tex_uXscale, s_tex_uXcenter);
        if (s_ws_ablate != 2) tex_batch_draw_passes(nverts, semi);
        wide_clear_bd_scale(s_tex_uXscale, s_tex_uXcenter);
        wide_target_end(s_tex_uXoff, s_tex_uXhalf);
        gl_perf_mirror_end();
    }
    hr_end();
    if (--s_cw_flush_depth == 0) s_cw_flush_ms += cw_ms() - cw_t0;
}

/* Flat / gouraud GEO batch — MotK title/char-select starfields issue ~30k/s
 * GP0(68h) 1x1 dots; each was two immediate gpu_triangle draws (BufferData +
 * DrawArrays each). Coalesce opaque/semi-uniform tris into one draw. */
#define FLATBATCH_MAXV 8190                 /* multiple of 3 */
static float s_fb[FLATBATCH_MAXV * 6];
static int   s_fb_n = 0;
static int   s_fb_semi = -2;
static int   s_fb_mask = -1, s_fb_dither = 0;

static int mirror_flat_batch_center_only(int nverts) {
    if (!s_wide_fast || nverts <= 0) return 0;
    int lo = (int)s_fb[0], hi = (int)s_fb[0];
    for (int i = 1; i < nverts; i++) {
        int x = (int)s_fb[i * 6];
        if (x < lo) lo = x; if (x > hi) hi = x;
    }
    return mirror_x_center_only(lo, hi);
}

static void flush_flat_batch(void) {
    if (s_fb_n == 0) return;
    int nverts = s_fb_n, semi = s_fb_semi, mask = s_fb_mask;
    s_fb_n = 0;

    hr_begin(1);
    if (semi >= 0) apply_psx_blend(semi); else glDisable(GL_BLEND);
    mask_stencil(mask);
    p_glUseProgram(s_geo_prog);
    native_view_projection_uniforms(s_geo_uXoff, s_geo_uXhalf);
    p_glUniform1i(s_geo_uDither, s_fb_dither);
    p_glUniform1i(s_geo_uScale, s_scale);
    p_glBindVertexArray(s_geo_vao);
    p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_geo_vbo);
    p_glBufferData(PSXGL_ARRAY_BUFFER, (ptrdiff_t)(nverts * 6 * sizeof(float)),
                   s_fb, PSXGL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, nverts);

    if (!s_midpoint_pass_fbo && g_wide_cur && !s_wide_suppress && s_ws_ablate != 1 &&
        !(!g_ws_bd_stretch_on && mirror_flat_batch_center_only(nverts))) {
        int dx = wide_dx();
        /* Batch may span many prims; use stretch gate off (flat dots/UI). */
        s_bd_gate = 0;
        gl_perf_mirror_begin();
        wide_target_begin(dx, s_geo_uXoff, s_geo_uXhalf);
        wide_set_bd_scale(s_geo_uXscale, s_geo_uXcenter);
        if (s_ws_ablate != 2) glDrawArrays(GL_TRIANGLES, 0, nverts);
        wide_clear_bd_scale(s_geo_uXscale, s_geo_uXcenter);
        wide_target_end(s_geo_uXoff, s_geo_uXhalf);
        gl_perf_mirror_end();
    }
    hr_end();
}

/* Flat / gouraud triangles and lines share the GEO program. mode: GL_TRIANGLES
 * or GL_LINES; verts are (x, y, r, g, b, a) tuples with colors as 1555. */
static void gpu_geometry(GLenum mode, const int *xs, const int *ys,
                         const float *subpixel_x, const float *subpixel_y,
                         const uint32_t *cs, int n, int semi, int dither) {
    flush_tex_batch();   /* flat prim: drain textured draws first (order + program) */
    if (!s_native_host_queue_flushing)
        flush_cpu_upload();  /* also drains flat batch if an upload was pending */
    mark_prim_dirty(xs, ys, n, 0 /* flat */);

    /* Lines stay immediate (rare); tris batch for MotK 0x68 starfields. */
    if (mode != GL_TRIANGLES || n < 3) {
        flush_flat_batch();
        float verts[3 * 6];
        float mask_a = s_mask_set ? 1.0f : 0.0f;
        for (int i = 0; i < n; i++) {
            verts[i*6+0] = subpixel_x ? subpixel_x[i] : (float)xs[i];
            verts[i*6+1] = subpixel_y ? subpixel_y[i] : (float)ys[i];
            verts[i*6+2] = (cs[i] & 0xFF) / 255.0f;
            verts[i*6+3] = ((cs[i] >> 8) & 0xFF) / 255.0f;
            verts[i*6+4] = ((cs[i] >> 16) & 0xFF) / 255.0f;
            verts[i*6+5] = mask_a;
        }
        hr_begin(1);
        if (semi >= 0) apply_psx_blend(semi); else glDisable(GL_BLEND);
        mask_stencil(s_mask_set);
        if (mode == GL_LINES) glLineWidth((float)s_scale);
        p_glUseProgram(s_geo_prog);
        native_view_projection_uniforms(s_geo_uXoff, s_geo_uXhalf);
        p_glUniform1i(s_geo_uDither, dither);
        p_glUniform1i(s_geo_uScale, s_scale);
        p_glBindVertexArray(s_geo_vao);
        p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_geo_vbo);
        p_glBufferData(PSXGL_ARRAY_BUFFER, (ptrdiff_t)(n * 6 * sizeof(float)),
                       verts, PSXGL_STREAM_DRAW);
        glDrawArrays(mode, 0, n);
        if (!s_midpoint_pass_fbo && g_wide_cur && !s_wide_suppress && s_ws_ablate != 1 &&
            !(!g_ws_bd_stretch_on && mirror_geo_center_only(xs, n))) {
            int dx = wide_dx();
            s_bd_gate = bd_prim_gate(xs, n, 0);
            gl_perf_mirror_begin();
            wide_target_begin(dx, s_geo_uXoff, s_geo_uXhalf);
            wide_set_bd_scale(s_geo_uXscale, s_geo_uXcenter);
            if (s_ws_ablate != 2) glDrawArrays(mode, 0, n);
            wide_clear_bd_scale(s_geo_uXscale, s_geo_uXcenter);
            wide_target_end(s_geo_uXoff, s_geo_uXhalf);
            gl_perf_mirror_end();
        }
        hr_end();
        return;
    }

    if (s_fb_n > 0 && (s_fb_semi != semi || s_fb_mask != (int)s_mask_set ||
                       s_fb_dither != dither))
        flush_flat_batch();
    if (s_fb_n + n > FLATBATCH_MAXV)
        flush_flat_batch();
    s_fb_semi = semi;
    s_fb_mask = (int)s_mask_set;
    s_fb_dither = dither;

    float mask_a = s_mask_set ? 1.0f : 0.0f;
    for (int i = 0; i < n; i++) {
        float *v = &s_fb[s_fb_n * 6];
        v[0] = subpixel_x ? subpixel_x[i] : (float)xs[i];
        v[1] = subpixel_y ? subpixel_y[i] : (float)ys[i];
        v[2] = (cs[i] & 0xFF) / 255.0f;
        v[3] = ((cs[i] >> 8) & 0xFF) / 255.0f;
        v[4] = ((cs[i] >> 16) & 0xFF) / 255.0f;
        v[5] = mask_a;
        s_fb_n++;
    }
}

static void gpu_triangle(int x0,int y0,uint32_t c0, int x1,int y1,uint32_t c1,
                         int x2,int y2,uint32_t c2, int semi, int dither) {
    int xs[3] = {x0, x1, x2}, ys[3] = {y0, y1, y2};
    uint32_t cs[3] = {c0, c1, c2};
    gpu_geometry(GL_TRIANGLES, xs, ys, NULL, NULL, cs, 3, semi, dither);
}

static void gpu_triangle_subpixel(const float *x, const float *y,
                                  const uint32_t *colors, int semi,
                                  int dither) {
    int raster_x[3], raster_y[3];

    for (int index = 0; index < 3; ++index) {
        raster_x[index] = (int)x[index];
        raster_y[index] = (int)y[index];
    }
    gpu_geometry(GL_TRIANGLES, raster_x, raster_y, x, y, colors, 3,
                 semi, dither);
}

static void gpu_line(int x0,int y0,uint32_t c0,int x1,int y1,uint32_t c1,int semi,
                     int dither) {
    int xs[2] = {x0, x1}, ys[2] = {y0, y1};
    uint32_t cs[2] = {c0, c1};
    gpu_geometry(GL_LINES, xs, ys, NULL, NULL, cs, 2, semi, dither);
}

static void gpu_line_subpixel(const float *x, const float *y,
                              const uint32_t *colors, int semi, int dither) {
    int raster_x[2], raster_y[2];

    for (int index = 0; index < 2; ++index) {
        raster_x[index] = (int)x[index];
        raster_y[index] = (int)y[index];
    }
    gpu_geometry(GL_LINES, raster_x, raster_y, x, y, colors, 2, semi,
                 dither);
}

/* Shared PS1 uv-sampling model (limits + mirrored-2D compensation) — one
 * implementation for GL/VK/SW, see gpu_uv.h. */
#include "gpu_uv.h"

/* Textured triangle. Always two passes split by the per-texel STP bit so the
 * stencil (mask) write value is constant within each pass; the semi pass is
 * also where PS1 blending applies. lim = uv sampling bounds (see
 * tri_uv_limits); NULL computes them from the vertices. */
static void gpu_textured_triangle(const int *xs, const int *ys,
                                  const int *us, const int *vs,
                                  const float *col, uint16_t texpage,
                                  uint16_t clut_x, uint16_t clut_y, int rawtex,
                                  int semi, int dither, const int *lim,
                                  const float *subpixel_x,
                                  const float *subpixel_y) {
    int lim_buf[4];
    int uv_buf[6];
    if (!lim) {
        /* Poly path: exact sampled bounds from the ORIGINAL uvs, then the
         * center-sampling mirror compensation (rect prims arrive with their
         * own precomputed lim and pre-bumped uvs). */
        int *mu = uv_buf, *mv = uv_buf + 3;
        for (int i = 0; i < 3; i++) { mu[i] = us[i]; mv[i] = vs[i]; }
        if (subpixel_x && subpixel_y) {
            psx_uv_tri_limits_f32(
                subpixel_x, subpixel_y, mu, mv, lim_buf);
            psx_uv_tri_mirror_offset_f32(
                subpixel_x, subpixel_y, mu, mv);
        } else {
            psx_uv_tri_limits(xs, ys, mu, mv, lim_buf);
            psx_uv_tri_mirror_offset(xs, ys, mu, mv);
        }
        us = mu; vs = mv;
        lim = lim_buf;
    }
    s_scene_prims_tex++;
    int base_x = (texpage & 0xF) * 64;
    int base_y = ((texpage >> 4) & 1) * 256;
    int depth  = (texpage >> 7) & 3; if (depth > 2) depth = 2;

    if (!s_native_host_queue_flushing)
        flush_cpu_upload();   /* if a CPU->VRAM upload is pending it flushes the batch first */
    flush_pack_if_sampling(base_x, base_y, depth, clut_x, clut_y);  /* flushes batch iff it must pack */
    mark_prim_dirty(xs, ys, 3, 1 /* textured */);

    /* Append to the textured batch. Flush first if this prim's blend/mask/twin/
     * filter differ from the open batch, or the buffer is full. Per-prim texture
     * state goes in the vertex; only these keys force a new draw. */
    {
        flush_flat_batch();   /* painter order: flat GEO before textured */
        int twx = s_tw_mask_x, twy = s_tw_mask_y, tox = s_tw_off_x, toy = s_tw_off_y;
        int gate = bd_prim_gate(xs, 3, 1); /* backdrop-stretch gate is also a batch key */
        /* With mask checking off, opaque and mode-0 semi primitives use the
         * same dual-source blend state. The per-vertex a_semi flag selects
         * replace vs half-blend without breaking painter order. */
        int batch_semi = (!s_mask_check && semi != 2) ? 4 : semi;
        /* STP draw-ORDER correctness. flush_tex_batch draws a batch in two passes
         * over the WHOLE batch (pass 1 = every prim's STP=0/opaque texels, pass 2
         * = every prim's STP=1/semi texels with the PSX blend). For overlapping
         * prims that share a batch, a BEHIND prim's semi texels (pass 2) then
         * paint OVER a FRONT prim's opaque texels (pass 1) — a painter's-order
         * violation that only exists on GL (Tomba: the character drew behind the
         * AP-block letters / a save post on GL, correct on software). So a
         * semi-transparent prim must NOT coalesce with its neighbours: drain the
         * open batch, draw this prim alone (its own STP=0+STP=1 passes, which do
         * not self-overlap → composited fully before the next prim, exactly like
         * the software renderer), and let opaque prims keep batching. Opaque
         * content (terrain/foliage — the batching perf win) is untouched; the cost
         * is one draw per semi prim, which are sparse. (A future single-pass
         * optimization for modes 0/1/3 via GL_ONE,GL_SRC_ALPHA with a per-fragment
         * destination factor in alpha could re-batch semi prims — see memory
         * tomba_sprite_zorder_bug; mode 2 subtractive still needs isolation.) */
        int isolate = (semi >= 0 && (semi == 2 || s_mask_check));
        int reason = -1;
        if (s_tb_n > 0) {
            if (isolate) reason = 0;
            else if (batch_semi != s_tb_semi) reason = 1;
            else if (s_mask_set != s_tb_mask) reason = 2;
            else if (s_tex_filter != s_tb_filter) reason = 3;
            else if (gate != s_tb_gate) reason = 4;
            else if (twx != s_tb_twin[0] || twy != s_tb_twin[1] ||
                      tox != s_tb_twin[2] || toy != s_tb_twin[3]) reason = 5;
            else if (dither != s_tb_dither) reason = 5;
        }
        if (reason >= 0) {
            s_batch_reason[reason]++;
            flush_tex_batch();
        }
        if (s_tb_n + 3 > TEXBATCH_MAXV) { s_batch_reason[6]++; flush_tex_batch(); }
        if (s_tb_n == 0) {            /* opening a batch: capture its keyed state */
            s_tb_semi = batch_semi; s_tb_mask = s_mask_set;
            s_tb_filter = s_tex_filter; s_tb_dither = dither;
            s_tb_gate = gate;
            s_tb_twin[0] = twx; s_tb_twin[1] = twy; s_tb_twin[2] = tox; s_tb_twin[3] = toy;
        }
        float *vp = &s_tb[s_tb_n * TEXV];
        for (int i = 0; i < 3; i++, vp += TEXV) {
            vp[0] = subpixel_x ? subpixel_x[i] : (float)xs[i];
            vp[1] = subpixel_y ? subpixel_y[i] : (float)ys[i];
            vp[2] = (float)us[i];   vp[3] = (float)vs[i];
            vp[4] = col[i*3+0];     vp[5] = col[i*3+1];     vp[6] = col[i*3+2];   vp[7] = 1.0f;
            vp[8]  = (float)base_x;  vp[9]  = (float)base_y;        /* a_tpage  */
            vp[10] = (float)clut_x;  vp[11] = (float)clut_y;        /* a_clut   */
            vp[12] = (float)depth;   vp[13] = (float)rawtex;        /* a_depth, a_raw */
            vp[14] = (float)lim[0];  vp[15] = (float)lim[1];        /* a_limits */
            vp[16] = (float)lim[2];  vp[17] = (float)lim[3];
            vp[18] = semi >= 0 ? (float)(semi + 1) : 0.0f;          /* a_semi code */
            vp[19] = s_pq_valid ? s_pq[i] : 0.0f;                   /* a_q */
        }
        s_tb_n += 3;
        if (isolate) flush_tex_batch();   /* draw this semi prim alone, in submission order */
    }
}

/* Draw a flat-colored rect (GEO program) DIRECTLY into the active wide surface
 * at wide-space coords [wx, wx+ww) × [y, y+h). Used only by the full-screen-
 * overlay path; positions are already in wide space so u_xoff stays 0. Mirrors
 * raster_flat_rect(&wt, ...) in sw_draw_flat_rect. Caller stays inside
 * hr_begin/hr_end. */
static void wide_flat_rect_direct(int wx, int y, int ww, int h, uint16_t c, int semi) {
    if (ww <= 0 || h <= 0) return;
    float r = ((c & 0x1F) << 3) / 255.0f;
    float g = (((c >> 5) & 0x1F) << 3) / 255.0f;
    float b = (((c >> 10) & 0x1F) << 3) / 255.0f;
    float a = s_mask_set ? 1.0f : 0.0f;
    float fx0 = (float)wx, fy0 = (float)y, fx1 = (float)(wx + ww), fy1 = (float)(y + h);
    float verts[6 * 6] = {
        fx0,fy0,r,g,b,a,  fx1,fy0,r,g,b,a,  fx0,fy1,r,g,b,a,
        fx1,fy0,r,g,b,a,  fx0,fy1,r,g,b,a,  fx1,fy1,r,g,b,a,
    };
    /* Wide target: positions already wide-space so u_xoff = 0; full-width
     * scissor; u_xhalf = g_wide_w/2. */
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, g_wide_cur);
    glViewport(0, 0, g_wide_w * s_scale, VRAM_H * s_scale);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, g_wide_w * s_scale, VRAM_H * s_scale);  /* full surface (rt_wide) */
    if (semi >= 0) apply_psx_blend(semi); else glDisable(GL_BLEND);
    mask_stencil(s_mask_set);
    p_glUseProgram(s_geo_prog);
    p_glUniform1i(s_geo_uDither, 0);
    p_glUniform1i(s_geo_uScale, s_scale);
    p_glUniform1f(s_geo_uXoff, 0.0f);
    p_glUniform1f(s_geo_uXhalf, (float)g_wide_w / 2.0f);
    p_glBindVertexArray(s_geo_vao);
    p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_geo_vbo);
    p_glBufferData(PSXGL_ARRAY_BUFFER, sizeof verts, verts, PSXGL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    p_glUniform1f(s_geo_uXoff, 0.0f);
    p_glUniform1f(s_geo_uXhalf, 512.0f);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_hr_fbo);
}

static void gpu_flat_rect(int x,int y,int w,int h,uint16_t c,int semi) {
    if (w <= 0 || h <= 0) return;
    /* Full-screen 2D overlay (pause gray-filter / load fade): a flat rect
     * spanning the whole 4:3 framebuffer must cover the whole wide surface too,
     * else the revealed 16:9 margins are left undimmed/unfaded. Same detection
     * as sw_draw_flat_rect: native_w = the 4:3 framebuffer width (g_wide_w less
     * the per-side reveal on both sides); the rect's native screen-X span
     * (x - base) must cover [0, native_w). When it does, the two canonical
     * triangles are drawn WITHOUT the per-triangle wide mirror (suppressed) and
     * a single full-width rect is drawn into the wide surface instead; every
     * other rect mirrors 1:1 via the generic gpu_geometry path. Only runs in
     * native-wide (g_wide_cur != 0), so 4:3 is unaffected. */
    int overlay = 0;
    if (!s_midpoint_pass_fbo && g_wide_cur) {
        int native_w = g_wide_w - 2 * g_wide_off;
        int lx = x - g_wide_cur_base, rx = x + w - g_wide_cur_base;
        overlay = (native_w > 0 && lx <= 0 && rx >= native_w);
    }
    /* Flat geometry is batched. Drain preceding geometry before changing the
     * wide-mirror policy, then drain this overlay while its suppression is
     * active. Otherwise a deferred flush sees suppression cleared and blends
     * the wide margins a second time. */
    if (overlay) {
        flush_flat_batch();
        s_wide_suppress = 1;
    }
    uint32_t c24 = ((uint32_t)(c & 0x1f) << 3) |
                   ((uint32_t)(c & 0x03e0) << 6) |
                   ((uint32_t)(c & 0x7c00) << 9);
    gpu_triangle(x,   y,   c24, x+w, y,   c24, x,   y+h, c24, semi, 0);
    gpu_triangle(x+w, y,   c24, x,   y+h, c24, x+w, y+h, c24, semi, 0);
    if (overlay) {
        flush_flat_batch();
        s_wide_suppress = 0;
        /* Re-open the bracket just for the full-width wide pass so
         * blend/scissor/program state is clean. */
        if (s_ws_ablate != 1) {
            hr_begin(0);
            gl_perf_mirror_begin();
            wide_flat_rect_direct(0, y, g_wide_w, h, c, semi);
            gl_perf_mirror_end();
            hr_end();
        }
    }
}

static void gpu_textured_rect(int x,int y,int w,int h,
                              int u0,int v0,int u1,int v1,
                              uint16_t clut_x,uint16_t clut_y,uint16_t tp,int semi) {
    if (w <= 0 || h <= 0) return;
    float mr=s_mod_r/255.0f, mg=s_mod_g/255.0f, mb=s_mod_b/255.0f;
    float col[9]={mr,mg,mb, mr,mg,mb, mr,mg,mb};
    /* gpu.c routes axis-aligned MIRRORED quads (X/Y-flipped 2D sprites,
     * e.g. right-facing MMX entities) through THIS path as scaled rects
     * with u0>u1 / v0>v1 — they never reach the poly path. Exact bounds
     * from the original corners, then the mirror bump (see gpu_uv.h). */
    int lim[4];
    psx_uv_rect_limits(u0, v0, u1, v1, lim);
    psx_uv_rect_mirror_offset(&u0, &v0, &u1, &v1);
    int xs1[3]={x, x+w, x},    ys1[3]={y, y, y+h};
    int us1[3]={u0,u1,u0},     vs1[3]={v0,v0,v1};
    gpu_textured_triangle(xs1,ys1,us1,vs1,col,tp,clut_x,clut_y,
                           s_mod_raw,semi,0,lim,NULL,NULL);
    int xs2[3]={x+w, x, x+w},  ys2[3]={y, y+h, y+h};
    int us2[3]={u1,u0,u1},     vs2[3]={v0,v1,v1};
    gpu_textured_triangle(xs2,ys2,us2,vs2,col,tp,clut_x,clut_y,
                           s_mod_raw,semi,0,lim,NULL,NULL);
}

/* GP0(02h) fill: writes color with bit15=0, ignoring draw area, mask and
 * offset; coordinates wrap. A scissored clear (color + stencil) per wrapped
 * segment is exactly this. */
static void fill_segment(int x, int y, int w, int h, float r, float g, float b) {
    if (w <= 0 || h <= 0) return;
    glScissor(x * s_scale, y * s_scale, w * s_scale, h * s_scale);
    glClearColor(r, g, b, 0.0f);
    glClearStencil(0);
    glStencilMask(0xFF);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    rect_add(&s_pack_dirty, x, y, x + w - 1, y + h - 1);
}

static void gpu_fill(int x,int y,int w,int h,uint16_t c) {
    if (w <= 0 || h <= 0) return;
    if (!s_native_host_queue_flushing &&
        native_host_pending_flush_reason(4u) !=
            GPU_RENDER_TRANSACTION_OK)
        return;
    flush_cpu_upload();
    float r=(c&0x1F)/31.0f, g=((c>>5)&0x1F)/31.0f, b=((c>>10)&0x1F)/31.0f;
    x &= VRAM_W - 1; y &= VRAM_H - 1;
    if (w > VRAM_W) w = VRAM_W;
    if (h > VRAM_H) h = VRAM_H;
    int w1 = w, w2 = 0, h1 = h, h2 = 0;
    if (x + w > VRAM_W) { w1 = VRAM_W - x; w2 = w - w1; }
    if (y + h > VRAM_H) { h1 = VRAM_H - y; h2 = h - h1; }

    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_hr_fbo);
    glViewport(0, 0, VRAM_W * s_scale, VRAM_H * s_scale);
    glEnable(GL_SCISSOR_TEST);
    fill_segment(x, y, w1, h1, r, g, b);
    if (w2)       fill_segment(0, y, w2, h1, r, g, b);
    if (h2)       fill_segment(x, 0, w1, h2, r, g, b);
    if (w2 && h2) fill_segment(0, 0, w2, h2, r, g, b);
    glDisable(GL_SCISSOR_TEST);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    if (!s_cpu_auth_dual) s_gpu_dirty = 1;
    coh_record(GL_COH_FILL, x, y, x + w - 1, y + h - 1);
}

/* VRAM->VRAM copy: blit the source region to the scratch texture (resolves
 * overlap), then draw it back at the destination through the BLIT program so
 * mask set/check and the stencil mirror apply, exactly like sw_copy_rect. */
static void gpu_copy_rect(int sx,int sy,int dx,int dy,int w,int h) {
    const int native_target = s_native_view_pass;
    const int midpoint_target = s_midpoint_pass_fbo != 0;
    const int source_is_target = s_midpoint_copy_pass ||
        (native_target && s_native_view_copy_self);
    const int target_width = native_target ? s_native_view_width : VRAM_W;
    const GLuint source_fbo = s_native_view_copy_source_fbo
        ? s_native_view_copy_source_fbo
        : source_is_target
            ? (midpoint_target ? s_midpoint_pass_fbo : s_native_view_pass_fbo)
            : s_hr_fbo;
    int logical_y;
    int S;

    if (w <= 0 || h <= 0) return;
    if (!s_native_host_queue_flushing &&
        native_host_pending_flush_reason(5u) !=
            GPU_RENDER_TRANSACTION_OK)
        return;
    flush_flat_batch();
    flush_tex_batch();
    flush_cpu_upload();
    sx &= VRAM_W - 1;
    sy &= VRAM_H - 1;
    dx = native_target ? dx : dx & (VRAM_W - 1);
    dy &= VRAM_H - 1;
    if (w > VRAM_W) w = VRAM_W;
    if (h > VRAM_H) h = VRAM_H;
    if (native_target && (dx < 0 || dx >= target_width)) return;
    if (native_target && dx + w > target_width) w = target_width - dx;
    if (w <= 0) return;

    S = s_scale;
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, source_fbo);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, s_scratch_fbo);
    glDisable(GL_SCISSOR_TEST);
    logical_y = 0;
    while (logical_y < h) {
        const int source_y = (sy + logical_y) & (VRAM_H - 1);
        const int copy_h = h - logical_y < VRAM_H - source_y
            ? h - logical_y : VRAM_H - source_y;
        int logical_x = 0;

        while (logical_x < w) {
            const int source_x = (sx + logical_x) & (VRAM_W - 1);
            const int copy_w = w - logical_x < VRAM_W - source_x
                ? w - logical_x : VRAM_W - source_x;

            p_glBlitFramebuffer(
                source_x * S, source_y * S,
                (source_x + copy_w) * S, (source_y + copy_h) * S,
                logical_x * S, logical_y * S,
                (logical_x + copy_w) * S, (logical_y + copy_h) * S,
                GL_COLOR_BUFFER_BIT, GL_NEAREST);
            if (!native_target && !midpoint_target)
                coh_record(GL_COH_COPY_SRC, source_x, source_y,
                           source_x + copy_w - 1, source_y + copy_h - 1);
            logical_x += copy_w;
        }
        logical_y += copy_h;
    }
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);

    hr_begin(0);   /* copies ignore the draw area */
    glScissor(dx*S, dy*S, w*S, h*S);
    glDisable(GL_BLEND);
    p_glUseProgram(s_blit_prog);
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_scratch_tex);
    p_glUniform1i(s_uBlitSrc, 0);
    p_glUniform1i(s_uBlitMaskset, s_mask_set);
    p_glUniform1i(s_uBlitSrcDiv, 1);
    p_glUniform2i(s_uBlitTargetSize,
                  target_width, VRAM_H);
    p_glBindVertexArray(s_blit_vao);
    p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_blit_vbo);
    logical_y = 0;
    while (logical_y < h) {
        const int destination_y = (dy + logical_y) & (VRAM_H - 1);
        const int copy_h = h - logical_y < VRAM_H - destination_y
            ? h - logical_y : VRAM_H - destination_y;
        int logical_x = 0;

        while (logical_x < w) {
            const int destination_x = native_target
                ? dx + logical_x
                : (dx + logical_x) & (VRAM_W - 1);
            const int copy_w = native_target
                ? w - logical_x
                : (w - logical_x < VRAM_W - destination_x
                       ? w - logical_x : VRAM_W - destination_x);
            const float fx0 = (float)destination_x;
            const float fy0 = (float)destination_y;
            const float fx1 = (float)(destination_x + copy_w);
            const float fy1 = (float)(destination_y + copy_h);
            const float verts[6 * 2] = {
                fx0, fy0,  fx1, fy0,  fx0, fy1,
                fx1, fy0,  fx0, fy1,  fx1, fy1,
            };

            glScissor(destination_x * S, destination_y * S,
                      copy_w * S, copy_h * S);
            p_glUniform2i(s_uBlitSrcOff,
                          (logical_x - destination_x) * S,
                          (logical_y - destination_y) * S);
            p_glBufferData(PSXGL_ARRAY_BUFFER, sizeof verts, verts,
                           PSXGL_STREAM_DRAW);
            mask_stencil(s_mask_set);
            p_glUniform1i(s_uBlitPass, 1);
            glDrawArrays(GL_TRIANGLES, 0, 6);
            mask_stencil(1);
            p_glUniform1i(s_uBlitPass, 2);
            glDrawArrays(GL_TRIANGLES, 0, 6);
            if (!native_target && !midpoint_target) {
                rect_add(&s_pack_dirty, destination_x, destination_y,
                         destination_x + copy_w - 1,
                         destination_y + copy_h - 1);
                coh_record(GL_COH_COPY, destination_x, destination_y,
                           destination_x + copy_w - 1,
                           destination_y + copy_h - 1);
            }
            logical_x += copy_w;
        }
        logical_y += copy_h;
    }
    hr_end();

    if (!native_target && !midpoint_target && !s_cpu_auth_dual)
        s_gpu_dirty = 1;
}

/* ---- backend vtable wrappers ------------------------------------------- */
static void glb_init(uint16_t *vram) {
    gl_renderer_native_midpoint_reset_for_reason(
        GL_NATIVE_MIDPOINT_RESET_INITIALIZE);
    s_vram = vram;
    s_dither = 0;
    s_pc_valid = 0;
    s_pq_valid = 0;
    sw_renderer_init(vram);
}

/* Under GL the internal-resolution scale lives in the hr FBO; the CPU-side
 * (software mirror, readbacks, screenshots) stays native, so the reported
 * scale is 1 and the software hi-res mirror stays off. */
static void glb_set_scale(int s) {
    if (s < 1) s = 1;
    if (s > GL_MAX_INTERNAL_SCALE) s = GL_MAX_INTERNAL_SCALE;
    s_req_scale = s;
    /* Applied at the next present (context current, no draw in flight) via
     * gl_maybe_apply_scale — callers may be on threads without GL. */
    s_scale_apply_pending = 1;
    sw_renderer_set_scale(1);
}
static int  glb_scale(void) { return s_scale; }   /* real internal SSAA scale (was a stub 1; the
                                                      native-wide CPU present path + gr_scale() callers
                                                      need the true scale — the FBO-direct present is
                                                      unaffected since it never reads gr_scale()) */
static void glb_set_texture_filter(int b) { s_tex_filter = b ? 1 : 0; sw_set_texture_filter(b); }
static int  glb_texture_filter(void) { return s_tex_filter; }

static void glb_set_semi_transparency(int e, int m) { s_semi_en = e; s_semi_mode = m & 3; sw_set_semi_transparency(e, m); }
static void glb_set_mask_bits(int s, int c) {
    int next_check = c ? 1 : 0;
    if (next_check != s_mask_check) {
        if (!s_native_host_queue_flushing &&
            native_host_pending_flush_reason(6u) !=
                GPU_RENDER_TRANSACTION_OK)
            return;
        flush_flat_batch();
        flush_tex_batch();
        flush_cpu_upload();
        if (next_check)
            rebuild_mask_stencils();
    }
    s_mask_set = s ? 1 : 0;
    s_mask_check = next_check;
    sw_set_mask_bits(s, c);
}
static void glb_set_texture_window(uint32_t r) {
    s_tw_mask_x = (int)(r & 0x1F);
    s_tw_mask_y = (int)((r >> 5) & 0x1F);
    s_tw_off_x  = (int)((r >> 10) & 0x1F);
    s_tw_off_y  = (int)((r >> 15) & 0x1F);
    sw_set_texture_window(r);
}
static void glb_set_color_modulation(int r,int g,int b,int raw) { s_mod_r=r; s_mod_g=g; s_mod_b=b; s_mod_raw=raw; sw_set_color_modulation(r,g,b,raw); }
static void glb_set_precise_triangle(int enabled,
                                     int32_t x0, int32_t y0,
                                     int32_t x1, int32_t y1,
                                     int32_t x2, int32_t y2) {
    s_pc_valid = enabled ? 1 : 0;
    if (s_pc_valid) {
        s_pc_x[0] = (float)x0 / 65536.0f;
        s_pc_y[0] = (float)y0 / 65536.0f;
        s_pc_x[1] = (float)x1 / 65536.0f;
        s_pc_y[1] = (float)y1 / 65536.0f;
        s_pc_x[2] = (float)x2 / 65536.0f;
        s_pc_y[2] = (float)y2 / 65536.0f;
    }
    sw_set_precise_triangle(enabled, x0, y0, x1, y1, x2, y2);
}
static void glb_set_perspective_triangle(int enabled,
                                         float q0, float q1, float q2) {
    s_pq_valid = enabled && q0 > 0.0f && q1 > 0.0f && q2 > 0.0f;
    s_pq[0] = q0;
    s_pq[1] = q1;
    s_pq[2] = q2;
    sw_set_perspective_triangle(enabled, q0, q1, q2);
}
static void glb_precise_consumed(void) {
    s_pc_valid = 0;
    s_pq_valid = 0;
}
static void glb_set_dither(int enabled) {
    int next = enabled ? 1 : 0;
    if (next != s_dither) {
        flush_flat_batch();
        flush_tex_batch();
    }
    s_dither = next;
}
static void glb_set_draw_area(int x1,int y1,int x2,int y2) {
    if (x1 == s_area_x1 && y1 == s_area_y1 &&
        x2 == s_area_x2 && y2 == s_area_y2)
        return;
    flush_flat_batch(); flush_tex_batch();
    s_area_x1=x1; s_area_y1=y1; s_area_x2=x2; s_area_y2=y2;
    sw_set_draw_area(x1,y1,x2,y2);
}
static void glb_get_draw_area(int *x1,int *y1,int *x2,int *y2) { sw_get_draw_area(x1,y1,x2,y2); }
static void glb_set_draw_offset(int x,int y) {
    if (x == s_off_x && y == s_off_y) return;
    flush_flat_batch(); flush_tex_batch();
    s_off_x=x; s_off_y=y;
    sw_set_draw_offset(x,y);
}

static uint32_t glb_rgb555_to_rgb888(uint16_t color) {
    return ((uint32_t)(color & 0x001f) << 3) |
           ((uint32_t)(color & 0x03e0) << 6) |
           ((uint32_t)(color & 0x7c00) << 9);
}

static int glb_cpu_auth_draw(void) {
    return s_cpu_auth_dual && !s_native_view_pass && !s_midpoint_pass_fbo;
}

/* Pre-context draws (s_raster_ok == 0) fall back to the software rasterizer
 * over CPU VRAM; the initial full-VRAM upload at context init folds them in.
 * Post-init, the GPU pipeline is all-or-nothing — no per-prim fallback. */
static void glb_draw_flat_triangle(int x0,int y0,int x1,int y1,int x2,int y2,uint16_t col) {
    if (glb_cpu_auth_draw() || !s_raster_ok)
        sw_draw_flat_triangle(x0,y0,x1,y1,x2,y2,col);
    if (!s_raster_ok) {
        glb_precise_consumed();
        return;
    }
    uint32_t color = glb_rgb555_to_rgb888(col);
    uint32_t colors[3] = { color, color, color };
    if (s_pc_valid)
        gpu_triangle_subpixel(s_pc_x, s_pc_y, colors,
                              s_semi_en?s_semi_mode:-1, 0);
    else
        gpu_triangle(x0,y0,color, x1,y1,color, x2,y2,color,
                     s_semi_en?s_semi_mode:-1, 0);
    glb_precise_consumed();
}
static void glb_draw_gouraud_triangle(int x0,int y0,uint16_t c0,int x1,int y1,uint16_t c1,int x2,int y2,uint16_t c2) {
    if (glb_cpu_auth_draw() || !s_raster_ok)
        sw_draw_gouraud_triangle(x0,y0,c0,x1,y1,c1,x2,y2,c2);
    if (!s_raster_ok) {
        glb_precise_consumed();
        return;
    }
    uint32_t colors[3] = { glb_rgb555_to_rgb888(c0),
                           glb_rgb555_to_rgb888(c1),
                           glb_rgb555_to_rgb888(c2) };
    if (s_pc_valid)
        gpu_triangle_subpixel(s_pc_x, s_pc_y, colors,
                              s_semi_en?s_semi_mode:-1, s_dither);
    else
        gpu_triangle(x0,y0,colors[0], x1,y1,colors[1], x2,y2,colors[2],
                     s_semi_en?s_semi_mode:-1, s_dither);
    glb_precise_consumed();
}
static void glb_draw_flat_triangle_rgb888(int x0,int y0,int x1,int y1,
                                           int x2,int y2,uint32_t color) {
    if (glb_cpu_auth_draw() || !s_raster_ok)
        sw_draw_flat_triangle(x0,y0,x1,y1,x2,y2,
                              (uint16_t)(((color >> 3) & 0x1f) |
                              ((color >> 6) & 0x03e0) |
                              ((color >> 9) & 0x7c00)));
    if (!s_raster_ok) {
        glb_precise_consumed();
        return;
    }
    uint32_t colors[3] = { color, color, color };
    if (s_pc_valid)
        gpu_triangle_subpixel(s_pc_x, s_pc_y, colors,
                              s_semi_en?s_semi_mode:-1, 0);
    else
        gpu_triangle(x0,y0,color, x1,y1,color, x2,y2,color,
                     s_semi_en?s_semi_mode:-1, 0);
    glb_precise_consumed();
}
static void glb_draw_gouraud_triangle_rgb888(int x0,int y0,uint32_t c0,
                                              int x1,int y1,uint32_t c1,
                                              int x2,int y2,uint32_t c2) {
    if (glb_cpu_auth_draw() || !s_raster_ok)
        sw_draw_gouraud_triangle(
            x0,y0,(uint16_t)(((c0 >> 3) & 0x1f) | ((c0 >> 6) & 0x03e0) | ((c0 >> 9) & 0x7c00)),
            x1,y1,(uint16_t)(((c1 >> 3) & 0x1f) | ((c1 >> 6) & 0x03e0) | ((c1 >> 9) & 0x7c00)),
            x2,y2,(uint16_t)(((c2 >> 3) & 0x1f) | ((c2 >> 6) & 0x03e0) | ((c2 >> 9) & 0x7c00)));
    if (!s_raster_ok) {
        glb_precise_consumed();
        return;
    }
    uint32_t colors[3] = { c0, c1, c2 };
    if (s_pc_valid)
        gpu_triangle_subpixel(s_pc_x, s_pc_y, colors,
                              s_semi_en?s_semi_mode:-1, s_dither);
    else
        gpu_triangle(x0,y0,c0, x1,y1,c1, x2,y2,c2,
                     s_semi_en?s_semi_mode:-1, s_dither);
    glb_precise_consumed();
}
static void glb_fill_rect(int x,int y,int w,int h,uint16_t c){
    if (glb_cpu_auth_draw() || !s_raster_ok) sw_fill_rect(x,y,w,h,c);
    if (!s_raster_ok) return;
    gpu_fill(x,y,w,h,c);
}
static void glb_copy_rect(int sx,int sy,int dx,int dy,int w,int h){
    /* Close the same gap as flush_tex_batch: a copy can be the first thing
     * to touch the screen after depth24 exits, and its SOURCE may be a
     * region that was never really uploaded to the FBO (skipped as a
     * framebuffer-sized MDEC transfer during playback — see
     * depth24_upload_policy). Running the transition here guarantees the
     * FBO already holds real content by the time the read below happens,
     * so this can always go through the normal GPU-side copy path (correct
     * masking, command ordering, and no stale-CPU-mirror special case). */
    depth24_upload_policy();
    if (glb_cpu_auth_draw() || !s_raster_ok)
        sw_copy_rect(sx,sy,dx,dy,w,h);
    if (!s_raster_ok) return;
    gpu_copy_rect(sx,sy,dx,dy,w,h);
}
static void glb_draw_textured_triangle(int x0,int y0,int u0,int v0,int x1,int y1,int u1,int v1,int x2,int y2,int u2,int v2,uint16_t cx,uint16_t cy,uint16_t tp){
    if (glb_cpu_auth_draw() || !s_raster_ok)
        sw_draw_textured_triangle(x0,y0,u0,v0,x1,y1,u1,v1,x2,y2,u2,v2,cx,cy,tp);
    if (!s_raster_ok) {
        glb_precise_consumed();
        return;
    }
    int xs[3]={x0,x1,x2}, ys[3]={y0,y1,y2}, us[3]={u0,u1,u2}, vs[3]={v0,v1,v2};
    float mr=s_mod_r/255.0f, mg=s_mod_g/255.0f, mb=s_mod_b/255.0f;
    float col[9]={mr,mg,mb, mr,mg,mb, mr,mg,mb};
    gpu_textured_triangle(xs,ys,us,vs,col,tp,cx,cy,s_mod_raw,
                          s_semi_en?s_semi_mode:-1,
                          s_dither && !s_mod_raw, NULL,
                          s_pc_valid ? s_pc_x : NULL,
                          s_pc_valid ? s_pc_y : NULL);
    glb_precise_consumed();
}
static void glb_draw_shaded_textured_triangle(int x0,int y0,int u0,int v0,uint32_t c0,int x1,int y1,int u1,int v1,uint32_t c1,int x2,int y2,int u2,int v2,uint32_t c2,uint16_t cx,uint16_t cy,uint16_t tp,int raw){
    if (glb_cpu_auth_draw() || !s_raster_ok)
        sw_draw_shaded_textured_triangle(x0,y0,u0,v0,c0,x1,y1,u1,v1,c1,x2,y2,u2,v2,c2,cx,cy,tp,raw);
    if (!s_raster_ok) {
        glb_precise_consumed();
        return;
    }
    int xs[3]={x0,x1,x2}, ys[3]={y0,y1,y2}, us[3]={u0,u1,u2}, vs[3]={v0,v1,v2};
    uint32_t cc[3]={c0,c1,c2}; float col[9];
    for (int i=0;i<3;i++){ col[i*3+0]=(cc[i]&0xFF)/255.0f; col[i*3+1]=((cc[i]>>8)&0xFF)/255.0f; col[i*3+2]=((cc[i]>>16)&0xFF)/255.0f; }
    gpu_textured_triangle(xs,ys,us,vs,col,tp,cx,cy,raw,
                          s_semi_en?s_semi_mode:-1,
                          s_dither && !raw, NULL,
                          s_pc_valid ? s_pc_x : NULL,
                          s_pc_valid ? s_pc_y : NULL);
    glb_precise_consumed();
}
static void glb_draw_flat_rect(int x,int y,int w,int h,uint16_t c){
    if (glb_cpu_auth_draw() || !s_raster_ok) sw_draw_flat_rect(x,y,w,h,c);
    if (!s_raster_ok) return;
    gpu_flat_rect(x,y,w,h,c, s_semi_en?s_semi_mode:-1);
}
static void glb_draw_textured_rect(int x,int y,int w,int h,int u,int v,uint16_t cx,uint16_t cy,uint16_t tp){
    if (glb_cpu_auth_draw() || !s_raster_ok)
        sw_draw_textured_rect(x,y,w,h,u,v,cx,cy,tp);
    if (!s_raster_ok) return;
    gpu_textured_rect(x,y,w,h, u,v, u+w,v+h, cx,cy,tp, s_semi_en?s_semi_mode:-1);
}
static void glb_draw_textured_rect_scaled(int x,int y,int w,int h,int u0,int v0,int u1,int v1,uint16_t cx,uint16_t cy,uint16_t tp){
    if (glb_cpu_auth_draw() || !s_raster_ok)
        sw_draw_textured_rect_scaled(x,y,w,h,u0,v0,u1,v1,cx,cy,tp);
    if (!s_raster_ok) return;
    gpu_textured_rect(x,y,w,h, u0,v0, u1,v1, cx,cy,tp, s_semi_en?s_semi_mode:-1);
}
static void glb_draw_line(int x0,int y0,int x1,int y1,uint16_t c){
    if (glb_cpu_auth_draw() || !s_raster_ok) sw_draw_line(x0,y0,x1,y1,c);
    if (!s_raster_ok) return;
    uint32_t color = glb_rgb555_to_rgb888(c);
    gpu_line(x0,y0,color, x1,y1,color, s_semi_en?s_semi_mode:-1, s_dither);
}
static void glb_draw_shaded_line(int x0,int y0,uint16_t c0,int x1,int y1,uint16_t c1){
    if (glb_cpu_auth_draw() || !s_raster_ok)
        sw_draw_shaded_line(x0,y0,c0,x1,y1,c1);
    if (!s_raster_ok) return;
    gpu_line(x0,y0,glb_rgb555_to_rgb888(c0),
             x1,y1,glb_rgb555_to_rgb888(c1),
             s_semi_en?s_semi_mode:-1, s_dither);
}
static int  glb_render_display(uint32_t *o,int p,int dx,int dy,int dw,int dh){ ensure_cpu(); return sw_render_display(o,p,dx,dy,dw,dh); }
static int  glb_render_display_hires(uint32_t *o,int p,int dx,int dy,int dw,int dh){ ensure_cpu(); return sw_render_display_hires(o,p,dx,dy,dw,dh); }
/* While GP1 depth24 is on, packed RGB888 lives in the CPU mirror and is
 * presented via gl_renderer_present — never as 1555 FBO texels. Queuing those
 * MDEC A0 rects hits UP_RECTS_MAX (16) and force-flushes mid-movie (MotK intro
 * ~50→~30 FPS). Skip ONLY framebuffer-sized transfers (RGB888); still upload
 * smaller texture A0s so post-FMV menus keep VRAM pages coherent.
 * On leave: depth24_clear_skipped_fb repaints the skipped FB union with its
 * real, correctly-converted content — see that function for why restaging
 * the STILL-PACKED 24-bit bytes as 1555 directly (without converting first)
 * is a different, historically-broken idea (MotK title rainbow/static). */
static int s_depth24_skip_up = 0;
static DirtyRect s_d24_skip_fb; /* union of skipped MDEC FB rects (VRAM halfwords) */

static int depth24_is_fb_transfer(int w, int h) {
    if (!gpu_display_is_depth24() || w <= 0 || h <= 0) return 0;
    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    int fb_w = (int)((di.width * 3u + 1u) / 2u); /* RGB W → halfwords */
    int fb_h = (int)di.height;
    if (fb_w < 8) fb_w = 8;
    if (fb_h < 1) fb_h = 1;
    /* MotK: 768×128 class blits; allow slack. Reject small texture pages. */
    if (h >= fb_h - 8 && h <= fb_h + 16 && w >= (fb_w * 3) / 4) return 1;
    if ((int64_t)w * (int64_t)h >= ((int64_t)fb_w * fb_h) / 2) return 1;
    return 0;
}

static void depth24_mark_scanout_band(void) {
    GpuDisplayInfo display;
    int fb_w, fb_h, x0, y0, x1, y1;
    if (!gpu_display_is_depth24()) return;
    gpu_get_display_info(&display);
    fb_w = (int)((display.width * 3u + 1u) / 2u);
    fb_h = (int)display.height;
    if (fb_w < 8) fb_w = 8;
    if (fb_h < 1) fb_h = 1;
    x0 = (int)(display.display_x & (VRAM_W - 1));
    y0 = (int)(display.display_y & (VRAM_H - 1));
    x1 = x0 + fb_w - 1;
    y1 = y0 + fb_h - 1;
    if (x1 >= VRAM_W) x1 = VRAM_W - 1;
    if (y1 >= VRAM_H) y1 = VRAM_H - 1;
    rect_add(&s_d24_skip_fb, x0, y0, x1, y1);
}

/* Repaint the skipped-FB region with its real content instead of black-
 * clearing it and hoping a later draw covers it (that left a black/stale
 * hole visible until something unrelated happened to redraw over it — the
 * original right-half-black bug). The bytes there are still the packed
 * 24-bit RGB888 the movie last wrote (never uploaded to the FBO — that's
 * what "skipped" means), so gpu_depth24_convert_region_to_15bit unpacks
 * them into real RGB555 pixels IN s_vram first; only THEN do we restage
 * through the normal CPU-upload path. This is NOT the same as restaging
 * the still-packed bytes directly (the historical MotK rainbow/static
 * bug, see the comment above s_depth24_skip_up) — the conversion happens
 * first, so flush_cpu_upload's 1555->RGBA8 step operates on already-
 * correct data.
 *
 * ensure_cpu() guards the source: if s_gpu_dirty is set (some GPU-side
 * draw is ahead of the CPU mirror), s_vram could be stale for this region
 * without it — sync first so the repaint can't silently drop pending GPU
 * writes.
 *
 * Painting happens exactly once, synchronously, right here — no sticky
 * "stale region" tracking afterward. The FBO is correct immediately, so
 * nothing needs to keep re-checking or re-uploading around it on a later,
 * unrelated hot path (that re-upload was overwriting legitimately newer
 * GPU-rendered content for as long as it stayed sticky). */
static void depth24_clear_skipped_fb(void) {
    if (!s_raster_ok || !s_d24_skip_fb.set) return;
    flush_flat_batch();
    flush_tex_batch();
    ensure_cpu();
    int x0 = s_d24_skip_fb.x0, y0 = s_d24_skip_fb.y0;
    int x1 = s_d24_skip_fb.x1, y1 = s_d24_skip_fb.y1;
    int rw = x1 - x0 + 1, rh = y1 - y0 + 1;
    int S = s_scale;

    gpu_depth24_convert_region_to_15bit((uint32_t)x0, (uint32_t)y0,
                                        (uint32_t)rw, (uint32_t)rh);
    up_add(x0, y0, x1, y1);
    flush_cpu_upload();

    /* Stencil bookkeeping only — color content is already correct above. */
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_hr_fbo);
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_SCISSOR_TEST);
    glViewport(0, 0, VRAM_W * S, VRAM_H * S);
    glScissor(x0 * S, y0 * S, rw * S, rh * S);
    glClearStencil(0);
    glStencilMask(0xFF);
    glClear(GL_STENCIL_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    rect_add(&s_pack_dirty, x0, y0, x1, y1);
    present_dirty_rect(x0, y0, x1, y1, 1);
    for (int i = 0; i < NATIVE_VIEW_MAX_SURF; ++i)
        s_native_view_seeded[i] = 0;
    rect_clear(&s_d24_skip_fb);
}

/* Runs the depth24 skip-tracking enter/exit transition. Called from every
 * VRAM write/transfer (below) AND from flush_tex_batch/glb_copy_rect above
 * — a copy or textured draw can be the first thing to touch the screen
 * after depth24 exits, and without calling this there too, the repaint
 * above would never run in time for it to read correct data. */
static void depth24_upload_policy(void) {
    int d24 = gpu_display_is_depth24();
    if (d24 && !s_depth24_skip_up) {
        flush_cpu_upload();
        rect_clear(&s_d24_skip_fb);
    } else if (!d24 && s_depth24_skip_up) {
        flush_cpu_upload();
        depth24_clear_skipped_fb();
        gpu_depth24_upload_span_reset();
    }
    s_depth24_skip_up = d24;
}

static void glb_vram_write(int x,int y,uint16_t px){
    depth24_upload_policy();
    sw_vram_write(x,y,px);
    /* Point pokes are never MDEC frames — always stage to FBO. */
    up_add(x & (VRAM_W-1), y & (VRAM_H-1), x & (VRAM_W-1), y & (VRAM_H-1));
}
static uint16_t glb_vram_read(int x,int y){ ensure_cpu(); return sw_vram_read(x,y); }
static void glb_vram_transfer_in(int x,int y,int w,int h,const uint16_t *d){
    depth24_upload_policy();
    sw_vram_transfer_in(x,y,w,h,d);
    if (s_depth24_skip_up && depth24_is_fb_transfer(w, h)) {
        int x0 = x & (VRAM_W - 1), y0 = y & (VRAM_H - 1);
        rect_add(&s_d24_skip_fb, x0, y0, x0 + w - 1, y0 + h - 1);
        coh_record(GL_COH_UPLOAD, x, y, x+w-1, y+h-1);
        return;
    }
    up_add_transfer(x, y, w, h);   /* exact touched rects, incl. per-pixel wrap */
    coh_record(GL_COH_UPLOAD, x, y, x+w-1, y+h-1);
}
static void glb_vram_transfer_out(int x,int y,int w,int h,uint16_t *d){ ensure_cpu(); sw_vram_transfer_out(x,y,w,h,d); }

/* ---- context init / present -------------------------------------------- */
static void upload_present_tex(const uint32_t *pixels, int w, int h, int linear) {
    glBindTexture(GL_TEXTURE_2D, s_present_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    /* Re-assert clamp every upload: a stale REPEAT wrap samples past the
     * right edge into garbage (MotK FMV right-strip flicker). */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (w != s_present_w || h != s_present_h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
        s_present_w = w; s_present_h = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
    }
}

static void upload_native_present_tex(const uint32_t *pixels, int w, int h,
                                      int linear) {
    glBindTexture(GL_TEXTURE_2D, s_native_present_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                   linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                   linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (w != s_native_present_w || h != s_native_present_h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_BGRA,
                     GL_UNSIGNED_BYTE, pixels);
        s_native_present_w = w;
        s_native_present_h = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA,
                        GL_UNSIGNED_BYTE, pixels);
    }
}

/* Display aspect for the present letterbox. Default 4:3 (native). When a wide
 * aspect is configured the 4:3 frame is stretched into it — paired with the
 * GTE X-squash (gte_set_display_aspect) this nets a wider field of view. */
static int s_aspect_num = 4, s_aspect_den = 3;

void gl_renderer_set_display_aspect(int num, int den) {
    if (num <= 0 || den <= 0) { num = 4; den = 3; }
    s_aspect_num = num; s_aspect_den = den;
}

/* Letterbox: largest num:den rect centered in the drawable. */
static void letterbox_rect_aspect(int ww, int wh, int num, int den,
                                  int *x, int *y, int *w, int *h) {
    int dw = ww, dh = (ww * den) / num;
    if (dh > wh) { dh = wh; dw = (wh * num) / den; }
    *x = (ww - dw) / 2;
    *y = (wh - dh) / 2;
    *w = dw; *h = dh;
}
static void letterbox_rect(int ww, int wh, int *x, int *y, int *w, int *h) {
    letterbox_rect_aspect(ww, wh, s_aspect_num, s_aspect_den, x, y, w, h);
}

static GLuint make_tex(GLenum internal, int w, int h, GLenum fmt, GLenum type) {
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)internal, w, h, 0, fmt, type, NULL);
    return t;
}

static int make_fbo(GLuint *out_fbo, GLuint color_tex, GLuint stencil_rb) {
    p_glGenFramebuffers(1, out_fbo);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, *out_fbo);
    p_glFramebufferTexture2D(PSXGL_FRAMEBUFFER, PSXGL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_tex, 0);
    if (stencil_rb)
        p_glFramebufferRenderbuffer(PSXGL_FRAMEBUFFER, PSXGL_DEPTH_STENCIL_ATTACHMENT,
                                    PSXGL_RENDERBUFFER, stencil_rb);
    GLenum st = p_glCheckFramebufferStatus(PSXGL_FRAMEBUFFER);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    if (st != PSXGL_FRAMEBUFFER_COMPLETE) {
        fprintf(stdout, "psxrecomp: GL FBO incomplete (0x%X)\n", st);
        p_glDeleteFramebuffers(1, out_fbo);
        *out_fbo = 0;
        return 0;
    }
    return 1;
}

static void native_phase_free_canonical(unsigned int phase);
static void native_phase_free_view(int slot, unsigned int phase);

static int native_phase_allocate_canonical(unsigned int phase, int width,
                                           int height) {
    GLuint *texture;
    GLuint *framebuffer;
    GLuint *renderbuffer;

    if (phase == 0u || phase >= NATIVE_INTERPOLATION_MAX_PHASES) return 0;
    texture = &s_extra_phase_tex[phase - 1u];
    framebuffer = &s_extra_phase_fbo[phase - 1u];
    renderbuffer = &s_extra_phase_rb[phase - 1u];
    if (*framebuffer && *texture && *renderbuffer) return 1;
    native_phase_free_canonical(phase);
    *texture = make_tex(GL_RGBA8, width, height, GL_RGBA, GL_UNSIGNED_BYTE);
    p_glGenRenderbuffers(1, renderbuffer);
    p_glBindRenderbuffer(PSXGL_RENDERBUFFER, *renderbuffer);
    p_glRenderbufferStorage(
        PSXGL_RENDERBUFFER, PSXGL_DEPTH24_STENCIL8, width, height);
    if (*texture && *renderbuffer &&
        make_fbo(framebuffer, *texture, *renderbuffer))
        return 1;
    native_phase_free_canonical(phase);
    return 0;
}

static void native_phase_free_canonical(unsigned int phase) {
    if (phase == 0u || phase >= NATIVE_INTERPOLATION_MAX_PHASES) return;
    if (s_extra_phase_fbo[phase - 1u])
        p_glDeleteFramebuffers(1, &s_extra_phase_fbo[phase - 1u]);
    if (s_extra_phase_tex[phase - 1u])
        glDeleteTextures(1, &s_extra_phase_tex[phase - 1u]);
    if (s_extra_phase_rb[phase - 1u])
        p_glDeleteRenderbuffers(1, &s_extra_phase_rb[phase - 1u]);
    s_extra_phase_fbo[phase - 1u] = 0;
    s_extra_phase_tex[phase - 1u] = 0;
    s_extra_phase_rb[phase - 1u] = 0;
}

static int native_phase_allocate_view(int slot, unsigned int phase,
                                      int width, int height) {
    GLuint *texture;
    GLuint *framebuffer;
    GLuint *renderbuffer;

    if (slot < 0 || slot >= NATIVE_VIEW_MAX_SURF || phase == 0u ||
        phase >= NATIVE_INTERPOLATION_MAX_PHASES)
        return 0;
    texture = &s_native_extra_phase_tex[slot][phase - 1u];
    framebuffer = &s_native_extra_phase_fbo[slot][phase - 1u];
    renderbuffer = &s_native_extra_phase_rb[slot][phase - 1u];
    if (*framebuffer && *texture && *renderbuffer) return 1;
    native_phase_free_view(slot, phase);
    *texture = make_tex(GL_RGBA8, width, height, GL_RGBA, GL_UNSIGNED_BYTE);
    p_glGenRenderbuffers(1, renderbuffer);
    p_glBindRenderbuffer(PSXGL_RENDERBUFFER, *renderbuffer);
    p_glRenderbufferStorage(
        PSXGL_RENDERBUFFER, PSXGL_DEPTH24_STENCIL8, width, height);
    if (*texture && *renderbuffer &&
        make_fbo(framebuffer, *texture, *renderbuffer))
        return 1;
    native_phase_free_view(slot, phase);
    return 0;
}

static void native_phase_free_view(int slot, unsigned int phase) {
    if (slot < 0 || slot >= NATIVE_VIEW_MAX_SURF || phase == 0u ||
        phase >= NATIVE_INTERPOLATION_MAX_PHASES)
        return;
    if (s_native_extra_phase_fbo[slot][phase - 1u])
        p_glDeleteFramebuffers(
            1, &s_native_extra_phase_fbo[slot][phase - 1u]);
    if (s_native_extra_phase_tex[slot][phase - 1u])
        glDeleteTextures(1, &s_native_extra_phase_tex[slot][phase - 1u]);
    if (s_native_extra_phase_rb[slot][phase - 1u])
        p_glDeleteRenderbuffers(
            1, &s_native_extra_phase_rb[slot][phase - 1u]);
    s_native_extra_phase_fbo[slot][phase - 1u] = 0;
    s_native_extra_phase_tex[slot][phase - 1u] = 0;
    s_native_extra_phase_rb[slot][phase - 1u] = 0;
    s_native_extra_phase_seeded[slot][phase - 1u] = 0;
}

static int init_gpu_raster(void) {
    s_scale = s_req_scale;

    s_geo_prog  = build_program(GEO_VS, GEO_FS);
    s_tex_prog  = build_program_ex(TEX_VS, TEX_FS, 1);
    s_blit_prog = build_program(BLIT_VS, BLIT_FS);
    s_pack_prog = build_program(PACK_VS, PACK_FS);
    s_stencil_prog = build_program(PACK_VS, STENCIL_FS);
    if (!s_geo_prog || !s_tex_prog || !s_blit_prog || !s_pack_prog || !s_stencil_prog) return 0;

    s_conv = (uint32_t *)malloc((size_t)VRAM_W * VRAM_H * sizeof(uint32_t));
    if (!s_conv) return 0;

    int hw = VRAM_W * s_scale, hh = VRAM_H * s_scale;
    s_hr_tex      = make_tex(GL_RGBA8, hw, hh, GL_RGBA, GL_UNSIGNED_BYTE);
    s_midpoint_tex = make_tex(GL_RGBA8, hw, hh, GL_RGBA, GL_UNSIGNED_BYTE);
    for (unsigned int phase = 1u;
         phase < s_native_interpolation_phase_count; ++phase)
        if (!native_phase_allocate_canonical(phase, hw, hh)) return 0;
    s_scratch_tex = make_tex(GL_RGBA8, hw, hh, GL_RGBA, GL_UNSIGNED_BYTE);
    s_up_tex      = make_tex(GL_RGBA8, VRAM_W, VRAM_H, GL_RGBA, GL_UNSIGNED_BYTE);
    s_raw_tex     = make_tex(PSXGL_R16UI, VRAM_W, VRAM_H, PSXGL_RED_INTEGER, GL_UNSIGNED_SHORT);
    /* Force the driver's first texture-upload allocation while the renderer is
     * initializing. NVIDIA otherwise defers it until the first MDEC frame,
     * producing a measured ~33 ms glTexSubImage hitch and an audible underrun. */
    {
        const uint32_t zero_rgba = 0;
        const uint16_t zero_raw = 0;
        glBindTexture(GL_TEXTURE_2D, s_up_tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1,
                        GL_RGBA, GL_UNSIGNED_BYTE, &zero_rgba);
        glBindTexture(GL_TEXTURE_2D, s_raw_tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1,
                        PSXGL_RED_INTEGER, GL_UNSIGNED_SHORT, &zero_raw);
        glFinish();
    }

    p_glGenRenderbuffers(1, &s_hr_rb);
    p_glBindRenderbuffer(PSXGL_RENDERBUFFER, s_hr_rb);
    p_glRenderbufferStorage(PSXGL_RENDERBUFFER, PSXGL_DEPTH24_STENCIL8, hw, hh);
    p_glGenRenderbuffers(1, &s_midpoint_rb);
    p_glBindRenderbuffer(PSXGL_RENDERBUFFER, s_midpoint_rb);
    p_glRenderbufferStorage(PSXGL_RENDERBUFFER, PSXGL_DEPTH24_STENCIL8, hw, hh);
    p_glBindRenderbuffer(PSXGL_RENDERBUFFER, 0);

    if (!make_fbo(&s_hr_fbo, s_hr_tex, s_hr_rb)) return 0;
    if (!make_fbo(&s_midpoint_fbo, s_midpoint_tex, s_midpoint_rb)) return 0;
    if (!make_fbo(&s_raw_fbo, s_raw_tex, 0)) return 0;
    if (!make_fbo(&s_scratch_fbo, s_scratch_tex, 0)) return 0;

    s_uVram  = p_glGetUniformLocation(s_tex_prog, "u_vram");
    s_uTpage = p_glGetUniformLocation(s_tex_prog, "u_tpage");
    s_uClut  = p_glGetUniformLocation(s_tex_prog, "u_clut");
    s_uDepth = p_glGetUniformLocation(s_tex_prog, "u_depth");
    s_uRaw   = p_glGetUniformLocation(s_tex_prog, "u_raw");
    s_uSemipass = p_glGetUniformLocation(s_tex_prog, "u_semipass");
    s_uSemimode = p_glGetUniformLocation(s_tex_prog, "u_semimode");
    s_uTwin     = p_glGetUniformLocation(s_tex_prog, "u_twin");
    s_uMaskset  = p_glGetUniformLocation(s_tex_prog, "u_maskset");
    s_uFilter   = p_glGetUniformLocation(s_tex_prog, "u_filter");
    s_geo_uDither = p_glGetUniformLocation(s_geo_prog, "u_dither");
    s_geo_uScale = p_glGetUniformLocation(s_geo_prog, "u_scale");
    s_tex_uDither = p_glGetUniformLocation(s_tex_prog, "u_dither");
    s_tex_uScale = p_glGetUniformLocation(s_tex_prog, "u_scale");
    s_uLimits   = p_glGetUniformLocation(s_tex_prog, "u_limits");
    s_uBlitSrc     = p_glGetUniformLocation(s_blit_prog, "u_src");
    s_uBlitPass    = p_glGetUniformLocation(s_blit_prog, "u_stp_pass");
    s_uBlitMaskset = p_glGetUniformLocation(s_blit_prog, "u_maskset");
    s_uBlitSrcDiv  = p_glGetUniformLocation(s_blit_prog, "u_src_div");
    s_uBlitSrcOff  = p_glGetUniformLocation(s_blit_prog, "u_src_off");
    s_uBlitTargetSize = p_glGetUniformLocation(s_blit_prog, "u_target_size");
    s_uPackHr    = p_glGetUniformLocation(s_pack_prog, "u_hr");
    s_uPackScale = p_glGetUniformLocation(s_pack_prog, "u_scale");
    s_uStencilSrc = p_glGetUniformLocation(s_stencil_prog, "u_src");
    s_geo_uXoff  = p_glGetUniformLocation(s_geo_prog, "u_xoff");
    s_geo_uXhalf = p_glGetUniformLocation(s_geo_prog, "u_xhalf");
    s_tex_uXoff  = p_glGetUniformLocation(s_tex_prog, "u_xoff");
    s_tex_uXhalf = p_glGetUniformLocation(s_tex_prog, "u_xhalf");
    s_geo_uXscale  = p_glGetUniformLocation(s_geo_prog, "u_xscale");
    s_geo_uXcenter = p_glGetUniformLocation(s_geo_prog, "u_xcenter");
    s_tex_uXscale  = p_glGetUniformLocation(s_tex_prog, "u_xscale");
    s_tex_uXcenter = p_glGetUniformLocation(s_tex_prog, "u_xcenter");
    /* Default the new uniforms to the no-op (1.0 scale, 0 centre) -- GLSL would
     * otherwise zero them, collapsing all x to 0. */
    p_glUseProgram(s_geo_prog);
    p_glUniform1f(s_geo_uXscale, 1.0f); p_glUniform1f(s_geo_uXcenter, 0.0f);
    p_glUniform1i(s_geo_uDither, 0); p_glUniform1i(s_geo_uScale, s_scale);
    p_glUseProgram(s_tex_prog);
    p_glUniform1f(s_tex_uXscale, 1.0f); p_glUniform1f(s_tex_uXcenter, 0.0f);
    p_glUniform1i(s_tex_uDither, 0); p_glUniform1i(s_tex_uScale, s_scale);

    /* Sample-grid alignment shift: half an HR pixel, set once (S is fixed
     * for the lifetime of the pipeline). Backed off by 1/64 native px so
     * primitive edges never land EXACTLY on sample centers — that float tie
     * dropped 1px columns at quad seams (e.g. the 256px texture-page seam in
     * Tomba's title background). The 1/64 bias keeps floor(uv) on the exact
     * PS1 texel for |uv slope| < 64; mirrored (negative-slope) mappings can
     * be off by one texel at exact-integer uv — accepted. */
    {
        float shift = 0.5f / (float)s_scale - 1.0f / 64.0f;
        p_glUseProgram(s_geo_prog);
        p_glUniform1f(p_glGetUniformLocation(s_geo_prog, "u_shift"), shift);
        /* Native-wide projection defaults: x translation 0, clip half-extent
         * 512 — so the GEO_VS x term reduces to (x+u_shift)/512-1, bit-identical
         * to the pre-native-wide projection. The wide passes set these, then
         * restore these defaults. */
        p_glUniform1f(s_geo_uXoff, 0.0f);
        p_glUniform1f(s_geo_uXhalf, 512.0f);
        p_glUseProgram(s_tex_prog);
        p_glUniform1f(p_glGetUniformLocation(s_tex_prog, "u_shift"), shift);
        p_glUniform1f(s_tex_uXoff, 0.0f);
        p_glUniform1f(s_tex_uXhalf, 512.0f);
        p_glUseProgram(s_blit_prog);
        p_glUniform1f(p_glGetUniformLocation(s_blit_prog, "u_shift"), shift);
        p_glUseProgram(0);
    }

    p_glGenVertexArrays(1, &s_geo_vao);
    p_glBindVertexArray(s_geo_vao);
    p_glGenBuffers(1, &s_geo_vbo);
    p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_geo_vbo);
    p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)0);
    p_glEnableVertexAttribArray(0);
    p_glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)(2 * sizeof(float)));
    p_glEnableVertexAttribArray(1);

    p_glGenVertexArrays(1, &s_tex_vao);
    p_glBindVertexArray(s_tex_vao);
    p_glGenBuffers(1, &s_tex_vbo);
    p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_tex_vbo);
    {
        GLsizei st = TEXV * sizeof(float);
        p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, st, (void*)0);                  p_glEnableVertexAttribArray(0); /* pos    */
        p_glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, st, (void*)(2*sizeof(float)));  p_glEnableVertexAttribArray(1); /* uv     */
        p_glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, st, (void*)(4*sizeof(float)));  p_glEnableVertexAttribArray(2); /* col    */
        p_glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, st, (void*)(8*sizeof(float)));  p_glEnableVertexAttribArray(3); /* tpage  */
        p_glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, st, (void*)(10*sizeof(float))); p_glEnableVertexAttribArray(4); /* clut   */
        p_glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, st, (void*)(12*sizeof(float))); p_glEnableVertexAttribArray(5); /* depth  */
        p_glVertexAttribPointer(6, 1, GL_FLOAT, GL_FALSE, st, (void*)(13*sizeof(float))); p_glEnableVertexAttribArray(6); /* raw    */
        p_glVertexAttribPointer(7, 4, GL_FLOAT, GL_FALSE, st, (void*)(14*sizeof(float))); p_glEnableVertexAttribArray(7); /* limits */
        p_glVertexAttribPointer(8, 1, GL_FLOAT, GL_FALSE, st, (void*)(18*sizeof(float))); p_glEnableVertexAttribArray(8); /* semi   */
        p_glVertexAttribPointer(9, 1, GL_FLOAT, GL_FALSE, st, (void*)(19*sizeof(float))); p_glEnableVertexAttribArray(9); /* q      */
    }

    p_glGenVertexArrays(1, &s_blit_vao);
    p_glBindVertexArray(s_blit_vao);
    p_glGenBuffers(1, &s_blit_vbo);
    p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_blit_vbo);
    p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2*sizeof(float), (void*)0);
    p_glEnableVertexAttribArray(0);

    p_glGenVertexArrays(1, &s_empty_vao);
    p_glBindVertexArray(0);

    /* Clear the authoritative surface (color + stencil) and queue a full
     * upload of whatever the CPU VRAM already holds (pre-context software
     * draws, BIOS logo state, ...). */
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_hr_fbo);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 0);
    glClearStencil(0);
    glStencilMask(0xFF);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_midpoint_fbo);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    for (unsigned int phase = 1u;
         phase < s_native_interpolation_phase_count; ++phase) {
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER, native_phase_fbo(phase));
        glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    rect_clear(&s_pack_dirty);
    s_up_nrects = 0;
    up_add(0, 0, VRAM_W - 1, VRAM_H - 1);
    s_gpu_dirty = 0;
    s_stencil_valid = 1;
    for (int i = 0; i < PRES_ROWS; i++) s_present_dirty[i] = ~0ull;
    s_last_present_path = -1;

    /* Native-wide compositor surfaces start unallocated (lazily created when a
     * widescreen game calls wide_configure + wide_set_target). */
    for (int i = 0; i < WIDE_MAX_SURF; i++) {
        s_wide_tex[i] = 0; s_wide_fbo[i] = 0; s_wide_base[i] = -1;
    }
    g_wide_w = 0; g_wide_off = 0; g_wide_cur = 0; g_wide_cur_base = 0;

    s_raster_ok = 1;
    gl_perf_init();   /* frame_perf GPU/CPU phase timing (no-op if queries absent) */
    fprintf(stdout, "psxrecomp: GL GPU pipeline ready (internal scale %dx, "
            "mask-bit stencil, texture window, GPU copy/upload)\n", s_scale);
    return 1;
}

/* Tear down everything init_gpu_raster created (programs, textures, hr
 * renderbuffer, FBOs, VAOs/VBOs, s_conv). The GL context itself and the
 * lazily-created surfaces (s_present_tex, wide compositor surfaces) are
 * untouched. Used by the live internal-scale change path. */
static void destroy_gpu_raster(void) {
    if (s_transaction) (void)glb_transaction_abort_pending(1);
    glb_deferred_candidate_discard_owned();
    glb_transaction_cleanup_deferred_staging();
    wide_free_all();
    native_view_free_all();
    if (s_geo_prog)     { p_glDeleteProgram(s_geo_prog);     s_geo_prog = 0; }
    if (s_tex_prog)     { p_glDeleteProgram(s_tex_prog);     s_tex_prog = 0; }
    if (s_blit_prog)    { p_glDeleteProgram(s_blit_prog);    s_blit_prog = 0; }
    if (s_pack_prog)    { p_glDeleteProgram(s_pack_prog);    s_pack_prog = 0; }
    if (s_stencil_prog) { p_glDeleteProgram(s_stencil_prog); s_stencil_prog = 0; }
    if (s_hr_tex)      { glDeleteTextures(1, &s_hr_tex);      s_hr_tex = 0; }
    if (s_midpoint_tex){ glDeleteTextures(1, &s_midpoint_tex);s_midpoint_tex = 0; }
    for (unsigned int phase = 1u;
         phase < NATIVE_INTERPOLATION_MAX_PHASES; ++phase)
        native_phase_free_canonical(phase);
    if (s_scratch_tex) { glDeleteTextures(1, &s_scratch_tex); s_scratch_tex = 0; }
    if (s_up_tex)      { glDeleteTextures(1, &s_up_tex);      s_up_tex = 0; }
    if (s_raw_tex)     { glDeleteTextures(1, &s_raw_tex);     s_raw_tex = 0; }
    if (s_hr_rb)       { p_glDeleteRenderbuffers(1, &s_hr_rb); s_hr_rb = 0; }
    if (s_midpoint_rb) { p_glDeleteRenderbuffers(1, &s_midpoint_rb); s_midpoint_rb = 0; }
    if (s_hr_fbo)      { p_glDeleteFramebuffers(1, &s_hr_fbo);      s_hr_fbo = 0; }
    if (s_midpoint_fbo){ p_glDeleteFramebuffers(1, &s_midpoint_fbo);s_midpoint_fbo = 0; }
    if (s_raw_fbo)     { p_glDeleteFramebuffers(1, &s_raw_fbo);     s_raw_fbo = 0; }
    if (s_scratch_fbo) { p_glDeleteFramebuffers(1, &s_scratch_fbo); s_scratch_fbo = 0; }
    if (s_geo_vbo)     { p_glDeleteBuffers(1, &s_geo_vbo);   s_geo_vbo = 0; }
    if (s_tex_vbo)     { p_glDeleteBuffers(1, &s_tex_vbo);   s_tex_vbo = 0; }
    if (s_blit_vbo)    { p_glDeleteBuffers(1, &s_blit_vbo);  s_blit_vbo = 0; }
    if (s_geo_vao)     { p_glDeleteVertexArrays(1, &s_geo_vao);   s_geo_vao = 0; }
    if (s_tex_vao)     { p_glDeleteVertexArrays(1, &s_tex_vao);   s_tex_vao = 0; }
    if (s_blit_vao)    { p_glDeleteVertexArrays(1, &s_blit_vao);  s_blit_vao = 0; }
    if (s_empty_vao)   { p_glDeleteVertexArrays(1, &s_empty_vao); s_empty_vao = 0; }
    free(s_conv); s_conv = NULL;
    s_raster_ok = 0;
}

static void native_scale_release_preserved(const GLuint *textures,
                                           const GLuint *framebuffers) {
    for (int i = 0; i < NATIVE_VIEW_MAX_SURF; ++i) {
        if (framebuffers[i]) p_glDeleteFramebuffers(1, &framebuffers[i]);
        if (textures[i]) glDeleteTextures(1, &textures[i]);
    }
}

static int native_scale_restore_views(const GLuint *framebuffers,
                                      const int *bases, int old_width,
                                      int old_height, int *restored_slots) {
    const int new_width = s_native_view_width * s_scale;
    const int new_height = VRAM_H * s_scale;

    for (int i = 0; i < NATIVE_VIEW_MAX_SURF; ++i) restored_slots[i] = -1;
    for (int i = 0; i < NATIVE_VIEW_MAX_SURF; ++i) {
        int slot;

        if (!framebuffers[i]) continue;
        slot = native_view_surface_slot(bases[i], 1);
        if (slot < 0) return 0;
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, framebuffers[i]);
        p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER,
                            s_native_view_fbo[slot]);
        p_glBlitFramebuffer(0, 0, old_width, old_height,
                            0, 0, new_width, new_height,
                            GL_COLOR_BUFFER_BIT, GL_NEAREST);
        rebuild_target_stencil(s_native_view_fbo[slot], new_width, new_height);
        s_native_view_seeded[slot] = 1;
        restored_slots[i] = slot;
    }
    return 1;
}

/* Apply a pending internal-scale change: full raster rebuild at the new
 * scale. Native semantic paths call this after swapping their completed
 * old-scale frame; other paths call it before collecting their present image. */
static void gl_maybe_apply_scale(void) {
    GLuint preserved_tex[NATIVE_VIEW_MAX_SURF] = {0};
    GLuint preserved_fbo[NATIVE_VIEW_MAX_SURF] = {0};
    int preserved_base[NATIVE_VIEW_MAX_SURF] = {0};
    int restored_slots[NATIVE_VIEW_MAX_SURF];
    int preserved_ok = 1;
    int restored_ok = 0;
    int old_native_width = 0;
    int old_native_height = 0;

    if (!s_scale_apply_pending) return;
    if (!s_ctx || !s_raster_ok || s_scale == s_req_scale) {
        s_scale_apply_pending = 0;
        return;
    }
    const int requested = s_req_scale;
    const int previous = s_scale;
    ensure_cpu();
    if (s_native_view_enabled && s_native_view_width > 0) {
        old_native_width = s_native_view_width * s_scale;
        old_native_height = VRAM_H * s_scale;
        for (int i = 0; i < NATIVE_VIEW_MAX_SURF; ++i) {
            if (!s_native_view_seeded[i] || !s_native_view_fbo[i]) continue;
            preserved_tex[i] = make_tex(GL_RGBA8, old_native_width,
                                        old_native_height, GL_RGBA,
                                        GL_UNSIGNED_BYTE);
            if (!preserved_tex[i] ||
                !make_fbo(&preserved_fbo[i], preserved_tex[i], 0)) {
                preserved_ok = 0;
                break;
            }
            preserved_base[i] = s_native_view_base[i];
            p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,
                                s_native_view_fbo[i]);
            p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, preserved_fbo[i]);
            p_glBlitFramebuffer(0, 0, old_native_width, old_native_height,
                                0, 0, old_native_width, old_native_height,
                                GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
        p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
    }
    if (!preserved_ok) {
        native_scale_release_preserved(preserved_tex, preserved_fbo);
        return;
    }
    s_scale_apply_pending = 0;
    gl_renderer_native_midpoint_reset_for_reason(
        GL_NATIVE_MIDPOINT_RESET_SCALE_CHANGE);
    destroy_gpu_raster();
    if (init_gpu_raster()) {
        restored_ok = native_scale_restore_views(
            preserved_fbo, preserved_base, old_native_width,
            old_native_height, restored_slots);
    }
    if (!restored_ok) {
        destroy_gpu_raster();
        s_req_scale = previous;
        if (init_gpu_raster()) {
            restored_ok = native_scale_restore_views(
                preserved_fbo, preserved_base, old_native_width,
                old_native_height, restored_slots);
        }
    }
    if (!restored_ok) {
        destroy_gpu_raster();
        fprintf(stderr, "psxrecomp: GL raster rebuild/restore failed at %dx "
                "and at restore %dx — GL path dead, software mirror "
                "authoritative\n", requested, previous);
    }
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
    native_scale_release_preserved(preserved_tex, preserved_fbo);
}

int gl_renderer_init_context(SDL_Window *win) {
    s_win = win;
    s_present_w = 0;
    s_present_h = 0;
    s_native_present_w = 0;
    s_native_present_h = 0;
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    s_ctx = SDL_GL_CreateContext(win);
    if (!s_ctx) { fprintf(stdout, "psxrecomp: GL context creation failed (%s)\n", SDL_GetError()); return 0; }
    if (SDL_GL_MakeCurrent(win, s_ctx) != 0) { fprintf(stdout, "psxrecomp: MakeCurrent failed (%s)\n", SDL_GetError()); SDL_GL_DeleteContext(s_ctx); s_ctx=NULL; return 0; }
    /* Swap interval: 1=vsync (tear-free, default), 0=immediate (lowest display
     * latency, may tear; our wall-clock pacer still holds 59.94Hz), -1=adaptive.
     * Adaptive falls back to vsync if the driver rejects it. */
    apply_swap_interval();
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
    const char *ver = (const char *)glGetString(GL_VERSION);
    fprintf(stdout, "psxrecomp: OpenGL context created (%s)\n", ver ? ver : "?");

    /* All-or-nothing: any missing entry point / failed shader / bad FBO means
     * the whole GL renderer is unavailable and the runtime stays on the pure
     * software path — no half-GL hybrid (that mixed mode is what produced
     * the alternating-present menu jitter). */
    int ok = load_modern_gl();
    if (ok) {
        glGenTextures(1, &s_present_tex);
        glGenTextures(1, &s_native_present_tex);
        glBindTexture(GL_TEXTURE_2D, s_present_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, s_native_present_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        s_present_prog = build_program(PRESENT_VS, PRESENT_FS);
        s_interp_prog = build_program(PRESENT_VS, INTERP_FS);
        if (s_present_prog && s_interp_prog) {
            p_glGenVertexArrays(1, &s_present_vao);
            s_present_uTex = p_glGetUniformLocation(s_present_prog, "u_tex");
            s_present_uUvRect = p_glGetUniformLocation(s_present_prog, "u_uv_rect");
            s_present_uLut = p_glGetUniformLocation(s_present_prog, "u_screenlut");
            s_present_uLutOn = p_glGetUniformLocation(s_present_prog, "u_screenlut_on");
            p_glUseProgram(s_present_prog);
            p_glUniform1i(s_present_uLut, 1);  /* LUT texture lives on unit 1 */
            p_glUniform1i(s_present_uLutOn, 0);
            p_glUseProgram(0);
            s_interp_uPrev = p_glGetUniformLocation(s_interp_prog, "u_prev");
            s_interp_uCurr = p_glGetUniformLocation(s_interp_prog, "u_curr");
            s_interp_uAlpha = p_glGetUniformLocation(s_interp_prog, "u_alpha");
            s_interp_uUvRect = p_glGetUniformLocation(s_interp_prog, "u_uv_rect");
            s_interp_uBlendMode =
                p_glGetUniformLocation(s_interp_prog, "u_blend_mode");
            glGenTextures(3, s_interp_tex);
            for (int i = 0; i < 3; i++) {
                glBindTexture(GL_TEXTURE_2D, s_interp_tex[i]);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            }
        } else ok = 0;
    }
    if (ok) ok = init_gpu_raster();
    if (!ok) {
        fprintf(stdout, "psxrecomp: GL pipeline init failed — falling back to software renderer\n");
        SDL_GL_DeleteContext(s_ctx); s_ctx = NULL;
        s_raster_ok = 0;
        return 0;
    }
    (void)psx_wayland_presentation_init(
        s_win, pres_wayland_feedback, NULL);
    return 1;
}

/* Set the GL swap interval (vsync mode): 1=vsync, 0=immediate, -1=adaptive.
 * Safe to call before or after context creation; applies live when a context
 * exists. Adaptive falls back to vsync if unsupported. */
void gl_renderer_set_swap_interval(int interval) {
    s_swap_interval = interval;
    apply_swap_interval();
}

int gl_renderer_set_native_interpolation_fps(int target_fps) {
    unsigned int denominator;
    unsigned int phase_count;
    const unsigned int old_phase_count = s_native_interpolation_phase_count;

    if (target_fps == 0) denominator = 2u;
    else if (target_fps == 60) denominator = 2u;
    else if (target_fps == 120) denominator = 4u;
    else if (target_fps == 240) denominator = 8u;
    else return 0;
    phase_count = denominator - 1u;
    if (denominator == s_native_interpolation_denominator) return 1;
    gl_renderer_native_midpoint_reset_for_reason(
        GL_NATIVE_MIDPOINT_RESET_FPS_CHANGE);
    if (s_ctx && s_raster_ok && phase_count > old_phase_count) {
        const int canonical_width = VRAM_W * s_scale;
        const int canonical_height = VRAM_H * s_scale;

        for (unsigned int phase = old_phase_count;
             phase < phase_count; ++phase)
            if (!native_phase_allocate_canonical(
                    phase, canonical_width, canonical_height))
                goto rollback_growth;
        for (int slot = 0; slot < NATIVE_VIEW_MAX_SURF; ++slot) {
            if (!s_native_view_fbo[slot]) continue;
            for (unsigned int phase = old_phase_count;
                 phase < phase_count; ++phase)
                if (!native_phase_allocate_view(
                         slot, phase, s_native_view_width * s_scale,
                         VRAM_H * s_scale))
                    goto rollback_growth;
        }
    }
    if (s_ctx && s_raster_ok && phase_count < old_phase_count) {
        for (unsigned int phase = phase_count;
             phase < old_phase_count; ++phase) {
            native_phase_free_canonical(phase);
            for (int slot = 0; slot < NATIVE_VIEW_MAX_SURF; ++slot)
                native_phase_free_view(slot, phase);
        }
    }
    s_native_interpolation_denominator = denominator;
    s_native_interpolation_phase_count = phase_count;
    apply_swap_interval();
    return 1;

rollback_growth:
    for (unsigned int phase = old_phase_count;
         phase < phase_count; ++phase) {
        native_phase_free_canonical(phase);
        for (int slot = 0; slot < NATIVE_VIEW_MAX_SURF; ++slot)
            native_phase_free_view(slot, phase);
    }
    return 0;
}

int gl_renderer_native_interpolation_fps(void) {
    return (int)s_native_interpolation_denominator * 30;
}

void gl_renderer_shutdown(void) {
    if (s_ctx) SDL_GL_MakeCurrent(s_win, s_ctx);
    psx_wayland_presentation_shutdown();
    pres_hash_collect();
    for (unsigned int index = 0u;
         index < GL_PRESENT_HASH_SLOT_COUNT; ++index) {
        GlPresentHashSlot *slot = &s_present_hash_slots[index];
        if (slot->fence) p_glDeleteSync(slot->fence);
        if (slot->pbo) p_glDeleteBuffers(1, &slot->pbo);
        memset(slot, 0, sizeof(*slot));
    }
    if (s_transaction) (void)glb_transaction_abort_pending(0);
    glb_deferred_candidate_discard_owned();
    glb_transaction_cleanup_deferred_staging();
    if (s_interp_thread) {
        SDL_AtomicSet(&s_interp_thread_run, 0);
        SDL_WaitThread(s_interp_thread, NULL);
        s_interp_thread = NULL;
    }
    if (s_interp_ctx) {
        SDL_GL_DeleteContext(s_interp_ctx);
        s_interp_ctx = NULL;
    }
    if (s_ctx) {
        SDL_GL_MakeCurrent(s_win, s_ctx);
        if (s_interp_draw_fence) {
            p_glDeleteSync(s_interp_draw_fence);
            s_interp_draw_fence = NULL;
        }
        for (int i = 0; i < 3; i++) {
            if (s_interp_fence[i]) {
                p_glDeleteSync(s_interp_fence[i]);
                s_interp_fence[i] = NULL;
            }
        }
        ensure_cpu();
        native_view_free_all();
        SDL_GL_DeleteContext(s_ctx); s_ctx = NULL;
    }
    s_transaction_deferred_staging_fbo = 0;
    s_transaction_deferred_staging_tex = 0;
    s_transaction_force_original = 0;
    if (s_interp_mutex) {
        SDL_DestroyMutex(s_interp_mutex);
        s_interp_mutex = NULL;
    }
    free(s_conv); s_conv = NULL;
    free(s_canonical_digest_pixels);
    s_canonical_digest_pixels = NULL;
    s_canonical_digest_capacity = 0u;
    s_raster_ok = 0;
    /* New context regenerates s_present_tex empty; a stale size makes
     * upload_present_tex take glTexSubImage2D into an unallocated texture
     * (rematch 24-bit FMV → black picture, audio still runs). */
    s_present_w = 0;
    s_present_h = 0;
    s_native_present_w = 0;
    s_native_present_h = 0;
    s_depth24_skip_up = 0;
    rect_clear(&s_d24_skip_fb);
    hold_invalidate();
    s_hold_tex = 0;
    s_hold_fbo = 0;
    s_hold_tw = 0;
    s_hold_th = 0;
}

/* CPU-readout present (24-bit FMV frames and the PSX_GL_FORCE_CPU_PRESENT
 * diagnostic): full-window clear, then a quad into the letterbox rect.
 * force_4_3 pins the rect to native 4:3 regardless of the display aspect —
 * FMVs are authored 4:3 and have no GTE squash to compensate a stretch, so
 * widescreen presents them pillarboxed instead of distorted. */
void gl_renderer_present(const uint32_t *pixels, int src_w, int src_h, int linear,
                          int force_4_3, int content_w) {
    if (s_transaction) {
        (void)glb_transaction_reject_other_present();
        return;
    }
    if (!s_ctx) return;
    if (!glb_transaction_prepare_original_present()) return;
    gl_maybe_apply_scale();
    interp_reset_history();
    int ww = 0, wh = 0; SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, ww, wh);
    glClearColor(0.f,0.f,0.f,1.f); glClear(GL_COLOR_BUFFER_BIT);
    int lx, ly, lw, lh;
    if (force_4_3)
        letterbox_rect_aspect(ww, wh, 4, 3, &lx, &ly, &lw, &lh);
    else
        letterbox_rect(ww, wh, &lx, &ly, &lw, &lh);
    /* Short GP1(07h) bands (MotK FMV is 128 lines) only fill a fraction of
     * NTSC active height on hardware. Stretching them to the full letterbox
     * doubles vertical scale vs horizontal and makes the frame look too wide
     * with the right edge clipped. Letterbox within the present rect instead.
     * Apply whenever the source is short — not only when force_4_3 — so a
     * misclassified FMV frame still keeps correct pixel aspect. */
    /* Genuinely windowed video bands only (<80% of the 240-line field, e.g.
     * MotK's 128-line FMV). A game's native short display mode (216/224)
     * fills the rect as on hardware. */
    if (src_h > 0 && src_h < 192) {
        int content_h = (lh * src_h) / 240;
        if (content_h < 1) content_h = 1;
        ly += (lh - content_h) / 2;
        lh = content_h;
    }
    /* Optional trailing-column crop (depth24 margin): shrink the draw width
     * left-aligned so cleared black remains on the right — never stretch. */
    float uv_x1 = 1.f;
    int crop = (content_w > 0 && content_w < src_w && src_w > 0);
    if (crop) {
        uv_x1 = (float)content_w / (float)src_w;
        lw = (lw * content_w) / src_w;
        if (lw < 1) lw = 1;
    }
    glViewport(lx, ly, lw, lh);
    p_glActiveTexture(PSXGL_TEXTURE0);
    upload_present_tex(pixels, src_w, src_h, linear);
    p_glUseProgram(s_present_prog); p_glUniform1i(s_present_uTex, 0);
    p_glUniform1i(s_present_uLutOn, 0);  /* 24-bit/FMV frames: LUT off (documented semantics) */
    if (crop) {
        p_glUniform4f(s_present_uUvRect, 0.f, 0.f, uv_x1, 1.f);
    } else if (!linear && src_w > 0 && src_h > 0) {
        /* Nearest: half-texel UV inset so UV=1.0 never grazes past the last
         * column into undefined border samples on some drivers. */
        float u0 = 0.5f / (float)src_w, v0 = 0.5f / (float)src_h;
        p_glUniform4f(s_present_uUvRect, u0, v0, 1.f - u0, 1.f - v0);
    } else {
        p_glUniform4f(s_present_uUvRect, 0.f, 0.f, 1.f, 1.f);
    }
    p_glBindVertexArray(s_present_vao); glDrawArrays(GL_TRIANGLES, 0, 3);
    p_glBindVertexArray(0); p_glUseProgram(0);
    uint64_t present_sequence =
        pres_record(GL_PRES_CPU, 0, 0, src_w, src_h, lx, ly, lw, lh);
    /* Pre-swap hook. The renderer's draw left the default framebuffer
     * implicit (no explicit FBO 0 bind on this path), so defensively
     * rebind FBO 0 to both DRAW and READ before the hook — the overlay
     * init/ImGui/window_shot readback expects to operate on the default
     * framebuffer. */
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    hold_capture_drawable();
    psx_debug_overlay_pre_swap();
    latency_ring_mark(LAT_SWAP_BEGIN);
    SDL_GL_SwapWindow(s_win);
    pres_mark_swap_completed(present_sequence);
    s_probe_swap++;
    latency_ring_mark(LAT_SWAP_END);
    present_force_consumed();
    s_last_present_path = GL_PRES_CPU;
    glb_transaction_original_presented();
}

/* Independent Native FMV presentation. This is intentionally not a wrapper
 * around gl_renderer_present(): Native owns a separate upload surface and
 * publishes it directly through the backend. The guest MDEC/DMA path still
 * supplies the pixels; this function only owns the host-side presentation. */
int gl_renderer_present_native_cpu_frame(const uint32_t *pixels, int src_w,
                                         int src_h, int linear, int force_4_3,
                                         int content_w) {
    int ww, wh, lx, ly, lw, lh;
    float uv_x1 = 1.f;
    int crop;

    if (s_transaction || !s_ctx || !s_native_present_tex || !pixels ||
        src_w <= 0 || src_h <= 0)
        return 0;
    gl_maybe_apply_scale();
    SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    if (ww <= 0 || wh <= 0) return 0;
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, ww, wh);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (force_4_3)
        letterbox_rect_aspect(ww, wh, 4, 3, &lx, &ly, &lw, &lh);
    else
        letterbox_rect(ww, wh, &lx, &ly, &lw, &lh);
    if (src_h < 192) {
        int content_h = (lh * src_h) / 240;
        if (content_h < 1) content_h = 1;
        ly += (lh - content_h) / 2;
        lh = content_h;
    }
    crop = content_w > 0 && content_w < src_w;
    if (crop) {
        uv_x1 = (float)content_w / (float)src_w;
        lw = (lw * content_w) / src_w;
        if (lw < 1) lw = 1;
    }
    glViewport(lx, ly, lw, lh);
    p_glActiveTexture(PSXGL_TEXTURE0);
    upload_native_present_tex(pixels, src_w, src_h, linear);
    p_glUseProgram(s_present_prog);
    p_glUniform1i(s_present_uTex, 0);
    p_glUniform1i(s_present_uLutOn, 0);
    if (crop) {
        p_glUniform4f(s_present_uUvRect, 0.f, 0.f, uv_x1, 1.f);
    } else if (!linear) {
        float u0 = 0.5f / (float)src_w;
        float v0 = 0.5f / (float)src_h;
        p_glUniform4f(s_present_uUvRect, u0, v0, 1.f - u0, 1.f - v0);
    } else {
        p_glUniform4f(s_present_uUvRect, 0.f, 0.f, 1.f, 1.f);
    }
    p_glBindVertexArray(s_present_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    p_glBindVertexArray(0);
    p_glUseProgram(0);
    uint64_t present_sequence =
        pres_record(GL_PRES_CPU, 0, 0, src_w, src_h, lx, ly, lw, lh);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    hold_capture_drawable();
    psx_debug_overlay_pre_swap();
    latency_ring_mark(LAT_SWAP_BEGIN);
    SDL_GL_SwapWindow(s_win);
    pres_mark_swap_completed(present_sequence);
    s_probe_swap++;
    latency_ring_mark(LAT_SWAP_END);
    present_force_consumed();
    s_last_present_path = GL_PRES_CPU;
    return 1;
}

void gl_renderer_present_blank(void) {
    gl_renderer_native_midpoint_reset_for_reason(
        GL_NATIVE_MIDPOINT_RESET_BLANK_PRESENT);
    if (s_transaction) {
        (void)glb_transaction_reject_other_present();
        return;
    }
    if (!s_ctx) return;
    if (!glb_transaction_prepare_original_present()) return;
    gl_maybe_apply_scale();
    interp_reset_history();
    int ww = 0, wh = 0; SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, ww, wh); glClearColor(0.f,0.f,0.f,1.f); glClear(GL_COLOR_BUFFER_BIT);
    uint64_t present_sequence =
        pres_record(GL_PRES_BLANK, 0, 0, 0, 0, 0, 0, ww, wh);
    /* Pre-swap hook on the blank path. No draw happens on this path, so
     * the back buffer is whatever the previous frame left (or the clear
     * above) — defensive FBO-0 rebind keeps the hook operating on the
     * default framebuffer regardless of the path's prior state. */
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    hold_capture_drawable();
    psx_debug_overlay_pre_swap();
    latency_ring_mark(LAT_SWAP_BEGIN);
    SDL_GL_SwapWindow(s_win);
    pres_mark_swap_completed(present_sequence);
    s_probe_swap++;
    latency_ring_mark(LAT_SWAP_END);
    present_force_consumed();
    s_last_present_path = GL_PRES_BLANK;
    glb_transaction_original_presented();
}

/* Sync the authoritative FBO down into CPU VRAM (no-op when current).
 * Screenshots / debug server. Not for 24-bit FMV scanout (see flush). */
void gl_renderer_sync_cpu(void) {
    ensure_cpu();
}

void gl_renderer_invalidate_present(void) {
    for (int i = 0; i < PRES_ROWS; i++) s_present_dirty[i] = ~0ull;
    for (int i = 0; i < NATIVE_VIEW_MAX_SURF; ++i)
        s_native_view_seeded[i] = 0;
    s_last_present_path = -1;
    s_force_present_remaining = 8;
    hold_invalidate();
    interp_reset_history();
    gl_renderer_native_midpoint_reset_for_reason(
        GL_NATIVE_MIDPOINT_RESET_INVALIDATE_PRESENT);
}

void gl_renderer_restage_vram_after_savestate(void) {
    if (!s_raster_ok || !s_vram) return;
    s_up_nrects = 0;
    rect_clear(&s_d24_skip_fb);
    s_depth24_skip_up = 0;
    up_add_transfer(0, 0, VRAM_W, VRAM_H);
    flush_cpu_upload();
    if (gpu_display_is_depth24()) {
        s_depth24_skip_up = 1;
        depth24_mark_scanout_band();
    }
    if (s_cpu_auth_dual) s_gpu_dirty = 0;
}

void gl_renderer_set_cpu_auth_dual(int on) {
    s_cpu_auth_dual = on ? 1 : 0;
    if (s_cpu_auth_dual) s_gpu_dirty = 0;
}

int gl_renderer_cpu_auth_dual(void) {
    return s_cpu_auth_dual;
}

void gl_renderer_present_probe_reset(void) {
    if (s_interp_mutex) SDL_LockMutex(s_interp_mutex);
    s_probe_skip = 0;
    s_probe_swap = 0;
    s_probe_dirty_marks = 0;
    if (s_interp_mutex) SDL_UnlockMutex(s_interp_mutex);
}

void gl_renderer_present_probe_take(uint64_t *skip_delta,
                                    uint64_t *swap_delta,
                                    uint64_t *dirty_mark_delta,
                                    int *force_remaining) {
    if (s_interp_mutex) SDL_LockMutex(s_interp_mutex);
    if (skip_delta) {
        *skip_delta = s_probe_skip;
        s_probe_skip = 0;
    }
    if (swap_delta) {
        *swap_delta = s_probe_swap;
        s_probe_swap = 0;
    }
    if (dirty_mark_delta) {
        *dirty_mark_delta = s_probe_dirty_marks;
        s_probe_dirty_marks = 0;
    }
    if (force_remaining) *force_remaining = s_force_present_remaining;
    if (s_interp_mutex) SDL_UnlockMutex(s_interp_mutex);
}

int gl_renderer_present_rect_dirty(int disp_x, int disp_y, int w, int h) {
    if (!s_raster_ok || w <= 0 || h <= 0) return 0;
    return present_dirty_test(disp_x, disp_y,
                              disp_x + w - 1, disp_y + h - 1);
}

void gl_renderer_flush_cpu_uploads(void) {
    if (!s_raster_ok) return;
    flush_flat_batch();
    flush_tex_batch();
    flush_cpu_upload();
}

/* Diagnostic (debug server "gl_fbo_peek"): read a rect of the GPU-side
 * authoritative VRAM (via the pack pass + raw mirror) WITHOUT writing CPU
 * VRAM — lets a probe diff FBO truth against CPU truth. Returns 0 when the
 * GL pipeline is inactive (software backend). */
int gl_renderer_fbo_peek(int x, int y, int w, int h, uint16_t *out) {
    if (!s_raster_ok || !s_ctx) return 0;
    if (x < 0 || y < 0 || w < 1 || h < 1 ||
        x + w > VRAM_W || y + h > VRAM_H) return 0;
    flush_flat_batch();
    flush_tex_batch();
    flush_cpu_upload();
    rect_add(&s_pack_dirty, x, y, x + w - 1, y + h - 1);
    pack_flush();
    coh_record(GL_COH_PEEK, x, y, x + w - 1, y + h - 1);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_raw_fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 2);
    glReadPixels(x, y, w, h, PSXGL_RED_INTEGER, GL_UNSIGNED_SHORT, out);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    return 1;
}

/* Diagnostic (debug server "gl_vram_diff"): full-VRAM comparison of the
 * GPU-side truth (FBO via pack) against the CPU array, WITHOUT writing
 * either. Reports mismatch count + bounding box + a few sample coords.
 * Divergence is expected where the GPU is legitimately ahead (gpu_dirty);
 * at upload-only scenes the two must match exactly. */
int gl_renderer_vram_diff(uint32_t *count, int bbox[4],
                          int samples[8][2], uint16_t samples_px[8][2]) {
    if (!s_raster_ok || !s_ctx) return 0;
    uint16_t *tmp = (uint16_t *)malloc((size_t)VRAM_W * VRAM_H * 2);
    if (!tmp) return 0;
    flush_flat_batch();
    flush_tex_batch();
    flush_cpu_upload();
    /* Force a full pack: the diff must read FBO truth even where the
     * raw-mirror invariant (raw == FBO outside s_pack_dirty) is broken —
     * a broken invariant is exactly what this tool hunts. */
    rect_add(&s_pack_dirty, 0, 0, VRAM_W - 1, VRAM_H - 1);
    pack_flush();
    coh_record(GL_COH_DIFF, 0, 0, VRAM_W - 1, VRAM_H - 1);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_raw_fbo);
    glReadPixels(0, 0, VRAM_W, VRAM_H, PSXGL_RED_INTEGER, GL_UNSIGNED_SHORT, tmp);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    uint32_t n = 0;
    int x0 = VRAM_W, y0 = VRAM_H, x1 = -1, y1 = -1, ns = 0;
    for (int y = 0; y < VRAM_H; y++) {
        for (int x = 0; x < VRAM_W; x++) {
            uint16_t f = tmp[y * VRAM_W + x], c = s_vram[y * VRAM_W + x];
            if (f == c) continue;
            n++;
            if (x < x0) x0 = x; if (x > x1) x1 = x;
            if (y < y0) y0 = y; if (y > y1) y1 = y;
            if (ns < 8 && (n % 977) == 1) {  /* spread samples */
                samples[ns][0] = x; samples[ns][1] = y;
                samples_px[ns][0] = f; samples_px[ns][1] = c;
                ns++;
            }
        }
    }
    free(tmp);
    *count = n;
    bbox[0] = x0; bbox[1] = y0; bbox[2] = x1; bbox[3] = y1;
    return 1 + ns;  /* >=1 means valid; ns = samples filled */
}

/* Diagnostic state for the debug server: coherency flags + dirty rects. */
void gl_renderer_diag(int *gpu_dirty, int pending[5], int pack[5]) {
    if (gpu_dirty) *gpu_dirty = s_gpu_dirty;
    if (pending) {
        /* [0] = pending rect count; [1..4] = union bbox (diagnostic only —
         * the flush itself paints the exact rects, never this union). */
        pending[0] = s_up_nrects;
        pending[1] = pending[2] = pending[3] = pending[4] = 0;
        for (int i = 0; i < s_up_nrects; i++) {
            if (i == 0) {
                pending[1] = s_up_rects[i].x0; pending[2] = s_up_rects[i].y0;
                pending[3] = s_up_rects[i].x1; pending[4] = s_up_rects[i].y1;
            } else {
                if (s_up_rects[i].x0 < pending[1]) pending[1] = s_up_rects[i].x0;
                if (s_up_rects[i].y0 < pending[2]) pending[2] = s_up_rects[i].y0;
                if (s_up_rects[i].x1 > pending[3]) pending[3] = s_up_rects[i].x1;
                if (s_up_rects[i].y1 > pending[4]) pending[4] = s_up_rects[i].y1;
            }
        }
    }
    if (pack) {
        pack[0] = s_pack_dirty.set;
        pack[1] = s_pack_dirty.x0; pack[2] = s_pack_dirty.y0;
        pack[3] = s_pack_dirty.x1; pack[4] = s_pack_dirty.y1;
    }
}

/* ------------------------------------------------------------------------- *
 * Native-wide compositor (GL). Mirrors gpu_sw_renderer.c's wide functions:
 * canonical VRAM (the hr FBO) is untouched; framebuffer draws are also mirrored
 * into per-base_x wide FBOs (see the draw funcs' wide passes). Present reads the
 * displayed buffer's wide FBO via glReadPixels into the CPU present buffer, then
 * the existing CPU present path uploads/letterboxes it (Option B: reuse the CPU
 * present path). Self-gates on the GL pipeline being live; never runs for
 * 4:3 / non-opted games (gpu.c never calls wide_configure for those).
 * ------------------------------------------------------------------------- */

static void wide_free_all(void) {
    for (int i = 0; i < WIDE_MAX_SURF; i++) {
        if (s_wide_fbo[i]) { p_glDeleteFramebuffers(1, &s_wide_fbo[i]); s_wide_fbo[i] = 0; }
        if (s_wide_tex[i]) { glDeleteTextures(1, &s_wide_tex[i]); s_wide_tex[i] = 0; }
        if (s_wide_rb[i])  { p_glDeleteRenderbuffers(1, &s_wide_rb[i]); s_wide_rb[i] = 0; }
        s_wide_base[i] = -1;
    }
    g_wide_cur = 0;
}

/* Lazily allocate (or find) the wide FBO+tex for base_x. Returns the FBO id, or
 * 0 on failure / more distinct buffers than WIDE_MAX_SURF. */
static GLuint wide_fbo_for(int base_x) {
    if (g_wide_w <= 0) return 0;
    for (int i = 0; i < WIDE_MAX_SURF; i++)
        if (s_wide_fbo[i] && s_wide_base[i] == base_x) return s_wide_fbo[i];
    for (int i = 0; i < WIDE_MAX_SURF; i++) {
        if (!s_wide_fbo[i]) {
            int w = g_wide_w * s_scale, h = VRAM_H * s_scale;
            s_cw_fbo_creates++;
            s_wide_tex[i] = make_tex(GL_RGBA8, w, h, GL_RGBA, GL_UNSIGNED_BYTE);
            /* Depth-stencil RB, same as the hr FBO: the stencil carries the
             * PSX mask-bit mirror for the wide surface, and (the hard lesson)
             * a stencil-less FBO turns every stencil-enabled mirror draw into
             * ~0.6ms of driver-side work — the 16:9 GL perf collapse. */
            p_glGenRenderbuffers(1, &s_wide_rb[i]);
            p_glBindRenderbuffer(PSXGL_RENDERBUFFER, s_wide_rb[i]);
            p_glRenderbufferStorage(PSXGL_RENDERBUFFER, PSXGL_DEPTH24_STENCIL8, w, h);
            if (!make_fbo(&s_wide_fbo[i], s_wide_tex[i], s_wide_rb[i])) {
                glDeleteTextures(1, &s_wide_tex[i]); s_wide_tex[i] = 0;
                p_glDeleteRenderbuffers(1, &s_wide_rb[i]); s_wide_rb[i] = 0;
                return 0;
            }
            /* Clear to black so unwritten margins are clean (not stale). */
            p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_wide_fbo[i]);
            glDisable(GL_SCISSOR_TEST);
            glClearColor(0, 0, 0, 0);
            glClearStencil(0);
            glStencilMask(0xFF);
            glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
            s_wide_base[i] = base_x;
            return s_wide_fbo[i];
        }
    }
    return 0;  /* more distinct buffers than WIDE_MAX_SURF — shouldn't happen */
}

/* Enable native-wide with a wide width + centering offset (native px), or
 * disable (wide_w <= 0). Re-allocates if the width changed. Mirrors
 * sw_wide_configure. */
static void glb_wide_configure(int wide_w, int offset) {
    if (!s_raster_ok) return;
    double t0 = cw_ms(); s_cw_wide_cfgs++;
    flush_tex_batch();   /* a queued batch's wide mirror targets the CURRENT surfaces */
    if (wide_w <= 0) { wide_free_all(); g_wide_w = 0; g_wide_off = 0; s_cw_wide_ms += cw_ms() - t0; return; }
    if (wide_w != g_wide_w) wide_free_all();
    g_wide_w = wide_w;
    g_wide_off = offset;
    s_cw_wide_ms += cw_ms() - t0;
}

/* Select the wide surface to mirror into for the back buffer at base_x. */
static void glb_wide_set_target(int base_x) {
    if (!s_raster_ok) { g_wide_cur = 0; return; }
    double t0 = cw_ms(); s_cw_wide_sets++;
    flush_tex_batch();   /* drain into the OLD target before switching */
    g_wide_cur = wide_fbo_for(base_x);
    g_wide_cur_base = base_x;
    s_cw_wide_ms += cw_ms() - t0;
}

/* Stop mirroring (offscreen draws that don't target a framebuffer). */
static void glb_wide_disable_target(void) { flush_tex_batch(); g_wide_cur = 0; }

/* Mirror a framebuffer clear: fill the full wide width over [y, y+h) of the
 * surface for base_x, so the revealed margins are clean. Mirrors sw_wide_clear:
 * a scissored glClear with the 1555 color converted to RGBA8 (alpha = bit15). */
static void glb_wide_clear(int base_x, int y, int h, uint16_t color) {
    if (!s_raster_ok || s_ws_ablate == 1) return;
    double t0 = cw_ms(); s_cw_wide_clears++;
    flush_tex_batch();
    GLuint fbo = wide_fbo_for(base_x);
    if (!fbo) { s_cw_wide_ms += cw_ms() - t0; return; }
    gl_perf_mirror_begin();
    int H = VRAM_H * s_scale;
    int y0 = y * s_scale, y1 = (y + h) * s_scale;
    if (y0 < 0) y0 = 0;
    if (y1 > H) y1 = H;
    if (y1 <= y0) return;
    float r = (color & 0x1F) / 31.0f;
    float g = ((color >> 5) & 0x1F) / 31.0f;
    float b = ((color >> 10) & 0x1F) / 31.0f;
    float a = (color >> 15) & 1 ? 1.0f : 0.0f;
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, fbo);
    glViewport(0, 0, g_wide_w * s_scale, H);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, y0, g_wide_w * s_scale, y1 - y0);
    glClearColor(r, g, b, a);
    glClearStencil((color >> 15) & 1);   /* stencil mirrors bit15, like the color alpha */
    glStencilMask(0xFF);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    gl_perf_mirror_end();
    s_cw_wide_ms += cw_ms() - t0;
}

/* Clear only the two synthetic reveal strips, preserving the centred canonical
 * framebuffer. This is an opt-in transition cleanup driven by gpu.c. */
static void glb_wide_clear_margins(int base_x, int y, int h, uint16_t color, int sides) {
    if (!s_raster_ok || s_ws_ablate == 1 || g_wide_off <= 0) return;
    double t0 = cw_ms(); s_cw_wide_clears++;
    flush_tex_batch();
    GLuint fbo = wide_fbo_for(base_x);
    if (!fbo) { s_cw_wide_ms += cw_ms() - t0; return; }
    gl_perf_mirror_begin();
    int H = VRAM_H * s_scale;
    int W = g_wide_w * s_scale;
    int margin = g_wide_off * s_scale;
    int y0 = y * s_scale, y1 = (y + h) * s_scale;
    if (y0 < 0) y0 = 0;
    if (y1 > H) y1 = H;
    if (y1 <= y0 || margin * 2 >= W) {
        gl_perf_mirror_end();
        s_cw_wide_ms += cw_ms() - t0;
        return;
    }
    float r = (color & 0x1F) / 31.0f;
    float g = ((color >> 5) & 0x1F) / 31.0f;
    float b = ((color >> 10) & 0x1F) / 31.0f;
    float a = (color >> 15) & 1 ? 1.0f : 0.0f;
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, fbo);
    glViewport(0, 0, W, H);
    glEnable(GL_SCISSOR_TEST);
    glClearColor(r, g, b, a);
    glClearStencil((color >> 15) & 1);
    glStencilMask(0xFF);
    if (sides & 1) {
        glScissor(0, y0, margin, y1 - y0);
        glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }
    if (sides & 2) {
        glScissor(W - margin, y0, margin, y1 - y0);
        glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }
    glDisable(GL_SCISSOR_TEST);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    gl_perf_mirror_end();
    s_cw_wide_ms += cw_ms() - t0;
}

/* Present source: read the wide FBO for the displayed buffer (base_x) into the
 * CPU present buffer as ARGB8888, byte-identical to sw_render_wide_display's
 * output so the shared CPU present path consumes it the same way. Output is
 * (g_wide_w*scale) wide × (disp_h*scale) tall. Returns pixel count (>0), or 0
 * if no surface exists for base_x (caller falls back to the canonical present).
 *
 * PIXEL FORMAT: glReadPixels(GL_BGRA, GL_UNSIGNED_BYTE) yields, per pixel, the
 * little-endian uint32 0xAARRGGBB == ARGB8888 — exactly what rgb555_to_argb
 * produces and what upload_present_tex feeds to glTexImage2D(GL_BGRA,...). The
 * SW path forces alpha to 0xFF; we OR it in to match (present ignores alpha, but
 * we keep the two paths bit-identical). GL's read origin is bottom-left, so the
 * block is read bottom-to-top and copied into `out` reversed (out row 0 = the
 * display's top scanline, as the SW path and the PRESENT_VS V-flip expect). */
static int glb_render_wide_display(uint32_t *out, int pitch, int base_x,
                                   int disp_y, int disp_h) {
    if (!s_raster_ok || !s_ctx || g_wide_w <= 0) return 0;
    GLuint fbo = 0;
    for (int i = 0; i < WIDE_MAX_SURF; i++)
        if (s_wide_fbo[i] && s_wide_base[i] == base_x) { fbo = s_wide_fbo[i]; break; }
    if (!fbo) return 0;

    /* Fold any pending CPU->VRAM uploads into the canonical FBO first (uploads
     * are never mirrored to wide, but draws after them are; keep op order) and
     * make sure all wide-FBO draws have completed before the readback. */
    flush_tex_batch();
    flush_cpu_upload();
    wide_blit_center(fbo, base_x, disp_y, disp_h);   /* fast-path: authoritative centre before readback */
    glFinish();

    int W = g_wide_w * s_scale;
    int H = VRAM_H * s_scale;
    int out_h = disp_h * s_scale;
    int ry0 = disp_y * s_scale;
    if (ry0 < 0) ry0 = 0;
    if (ry0 + out_h > H) out_h = H - ry0;
    if (out_h <= 0) return 0;

    uint32_t *tmp = (uint32_t *)malloc((size_t)W * out_h * sizeof(uint32_t));
    if (!tmp) return 0;
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, ry0, W, out_h, GL_BGRA, GL_UNSIGNED_BYTE, tmp);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);

    /* Orientation: the wide FBO stores PS1 y inverted (geo shader maps vram_y=0
     * to NDC y=-1 = FBO BOTTOM), and glReadPixels reads bottom-up — the two
     * inversions CANCEL, so glReadPixels row 0 already = PS1 top scanline. Copy
     * straight (NO flip) so `out` is top-down, matching sw_render_wide_display
     * (which the shared CPU present path + its PRESENT_VS V-flip expect). Force
     * alpha = 0xFF (match SW). [An earlier reversal here made the frame
     * upside-down.] */
    int count = 0;
    for (int row = 0; row < out_h; row++) {
        const uint32_t *src = tmp + (size_t)row * W;
        uint32_t *dst = (uint32_t *)((uint8_t *)out + (size_t)row * pitch);
        for (int col = 0; col < W; col++) { dst[col] = src[col] | 0xFF000000u; count++; }
    }
    free(tmp);
    return count;
}

/* Dump the ENTIRE wide compositor surface for base_x (all double-buffer bands +
 * both reveal margins), g_wide_w x VRAM_H at scale. Debug/inspection tool (TCP
 * wide_full) — the GL analog of sw_wide_dump_full, so native-wide can be
 * inspected without touching the game window. Runs the same authoritative
 * centre blit first so the dump matches what present shows. Top-down, alpha=FF
 * (matches sw_wide_dump_full / render_wide_display orientation). */
static int glb_wide_dump_full(uint32_t *out, int cap_pixels, int *ow, int *oh,
                              int base_x) {
    if (!s_raster_ok || !s_ctx || g_wide_w <= 0) return 0;
    GLuint fbo = 0;
    for (int i = 0; i < WIDE_MAX_SURF; i++)
        if (s_wide_fbo[i] && s_wide_base[i] == base_x) { fbo = s_wide_fbo[i]; break; }
    if (!fbo) return 0;
    flush_tex_batch();
    flush_cpu_upload();
    wide_blit_center(fbo, base_x, 0, VRAM_H);   /* authoritative centre (full height) */
    glFinish();
    int W = g_wide_w * s_scale;
    int H = VRAM_H * s_scale;
    if (cap_pixels > 0 && (long)W * H > cap_pixels) { H = cap_pixels / W; if (H <= 0) return 0; }
    uint32_t *tmp = (uint32_t *)malloc((size_t)W * H * sizeof(uint32_t));
    if (!tmp) return 0;
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, W, H, GL_BGRA, GL_UNSIGNED_BYTE, tmp);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    /* Same orientation reasoning as glb_render_wide_display: glReadPixels row 0 =
     * PS1 top scanline, so copy straight (top-down). */
    int count = 0;
    for (int i = 0; i < W * H; i++) { out[i] = tmp[i] | 0xFF000000u; count++; }
    free(tmp);
    if (ow) *ow = W;
    if (oh) *oh = H;
    return count;
}

/* THE present path for 15-bit frames: blit the display region from the
 * authoritative hr FBO into a letterboxed rect. Deterministic — runs
 * every 15-bit frame regardless of what mix of ops produced it.
 * force_4_3 pins to native 4:3 (15-bit MDEC FMV frames on a wide aspect). */
/* ===================== frame_perf: per-frame GPU/CPU phase timing ============
 * Developer builds use two GL_TIME_ELAPSED queries per frame to bracket (a) the
 * scene draws (all GP0 raster issued between two presents) and (b) the present
 * clear+blit, giving TRUE GPU time per phase independent of CPU/GPU overlap
 * (glFinish would only catch the non-overlapped tail and mislead). CPU wall time
 * (present-to-present total, and the present call) comes from SDL perf counters.
 * Results are read back GLPERF_NBUF frames late (no pipeline stall) into a ring
 * the debug server's frame_perf command aggregates. Release builds compile out
 * the debug server, so they also leave this instrumentation disabled: a native-
 * wide frame can otherwise issue hundreds of unused mirror timestamp queries.
 * One question this answers in a diagnostics build:
 * where does a 16:9 frame go vs 4:3 — scene fill, wide composite, or CPU. */
#define GLPERF_NBUF 4
#define GLPERF_RING 256
typedef struct {
    double   total_ms;        /* present-entry to present-entry (full frame)  */
    double   present_wall_ms; /* CPU wall time inside the present call         */
    double   scene_gpu_ms;    /* GPU: all scene draws this frame               */
    double   present_gpu_ms;  /* GPU: the present clear+blit                   */
    double   mirror_gpu_ms;   /* GPU: of scene_gpu, the native-wide mirror passes */
    double   prims;           /* scene primitives submitted this frame         */
    double   mirror_passes;   /* mirror passes this frame (measured + overflow) */
    double   cw_flush_ms;     /* CPU wall inside flush_tex_batch this frame    */
    double   cw_wide_ms;      /* CPU wall inside glb_wide_* this frame         */
    double   batches;         /* flush_tex_batch draws this frame              */
    double   wide_sets;       /* glb_wide_set_target calls this frame          */
    double   fbo_creates;     /* wide FBO+tex creations this frame             */
    int      wide;            /* 1 = native-wide (16:9) present, 0 = 4:3       */
    uint64_t frame;
} GlPerfSample;

/* Native-wide mirror-pass GPU attribution: the scene TIME_ELAPSED query spans
 * the whole frame (queries of one target cannot nest), so each mirror pass is
 * bracketed with a GL_TIMESTAMP pair instead (glQueryCounter does not conflict
 * with an active TIME_ELAPSED query). Pairs are pooled per buffered frame and
 * summed at readback, splitting scene_gpu into canonical vs mirror cost. */
#define GLPERF_MIRQ 1024               /* measured mirror passes per frame */
static GLuint s_mq_q[GLPERF_NBUF][GLPERF_MIRQ * 2];
static int    s_mq_n[GLPERF_NBUF];     /* pairs recorded this frame        */
static int    s_mq_over[GLPERF_NBUF];  /* passes beyond the pool (counted, untimed) */
static int    s_mq_open = 0;           /* begin issued, end pending        */
static int    s_mq_ok = 0;             /* glQueryCounter available         */

static int      s_pf_on = 0;
static GLuint   s_pf_scene_q[GLPERF_NBUF];
static GLuint   s_pf_present_q[GLPERF_NBUF];
static int      s_pf_b = 0;            /* buffer for the CURRENT frame         */
static int      s_pf_scene_active = 0;
static uint64_t s_pf_count = 0;
static uint64_t s_pf_freq = 1;
static uint64_t s_pf_last_enter = 0;
static uint64_t s_pf_enter = 0;
static double   s_pf_total_pending = 0.0;
static double   s_pf_buf_total[GLPERF_NBUF];
static double   s_pf_buf_pwall[GLPERF_NBUF];
static double   s_pf_buf_prims[GLPERF_NBUF];
static int      s_pf_buf_wide[GLPERF_NBUF];
static uint64_t s_pf_buf_frame[GLPERF_NBUF];
static uint64_t s_pf_prims_last = 0;
static double   s_pf_prims_pending = 0.0;
static double   s_pf_cw_pending[5];            /* flush_ms, wide_ms, batches, wide_sets, fbo_creates */
static double   s_pf_buf_cw[GLPERF_NBUF][5];
static GlPerfSample s_pf_ring[GLPERF_RING];
static uint64_t     s_pf_ring_seq = 0;

static void gl_perf_init(void) {
#ifdef PSX_NO_DEBUG_TOOLS
    return;
#else
    /* Idempotent: init_gpu_raster calls this on every (re)build — the live
     * scale-change path re-enters, and re-GenQuery'ing would leak query
     * objects in the live context. */
    static int s_pf_done = 0;
    if (s_pf_done) return;
    s_pf_done = 1;
    /* Timer queries are intentionally available in diagnostics builds, but a
     * driver may serialize command submission while collecting them.  Keep an
     * escape hatch so frame cadence can be A/B tested without rebuilding or
     * losing the rest of the debug server telemetry. */
    {
        const char *enabled = getenv("PSX_GL_PERF");
        if (enabled && enabled[0] == '0') return;
    }
    if (!p_glGenQueries || !p_glBeginQuery || !p_glEndQuery || !p_glGetQueryObjectui64v) return;
    p_glGenQueries(GLPERF_NBUF, s_pf_scene_q);
    p_glGenQueries(GLPERF_NBUF, s_pf_present_q);
    s_mq_ok = (p_glQueryCounter != NULL);
    if (s_mq_ok)
        for (int i = 0; i < GLPERF_NBUF; i++) {
            p_glGenQueries(GLPERF_MIRQ * 2, s_mq_q[i]);
            s_mq_n[i] = 0; s_mq_over[i] = 0;
        }
    s_mq_open = 0;
    s_pf_freq = SDL_GetPerformanceFrequency();
    if (!s_pf_freq) s_pf_freq = 1;
    s_pf_b = 0; s_pf_scene_active = 0; s_pf_count = 0; s_pf_ring_seq = 0;
    s_pf_last_enter = 0;
    s_pf_on = 1;
#endif
}

/* Bracket ONE native-wide mirror pass (called from the wide-mirror draw sites).
 * Timestamp pairs, not TIME_ELAPSED — see the pool comment above. */
static void gl_perf_mirror_begin(void) {
    if (!s_pf_on || !s_mq_ok) return;
    int b = s_pf_b;
    if (s_mq_n[b] >= GLPERF_MIRQ) { s_mq_over[b]++; return; }
    p_glQueryCounter(s_mq_q[b][s_mq_n[b] * 2], GL_TIMESTAMP);
    s_mq_open = 1;
}
static void gl_perf_mirror_end(void) {
    if (!s_pf_on || !s_mq_ok || !s_mq_open) return;
    int b = s_pf_b;
    p_glQueryCounter(s_mq_q[b][s_mq_n[b] * 2 + 1], GL_TIMESTAMP);
    s_mq_n[b]++;
    s_mq_open = 0;
}

/* Top of present (after flush_cpu_upload, before clear/blit). */
static void gl_perf_present_enter(void) {
    /* Per-frame boundary for the 2D-backdrop stretch — runs from BOTH present
     * paths (4:3 present_vram AND native-wide present_wide_fbo), before the
     * perf-on gate so native-wide frames reset too. Snapshot this frame's
     * backdrop-stretch diagnostics, then reset the per-frame counters. (The gate
     * is per-prim now — no draw-order phase to reset.) Final batch already flushed
     * by the caller. */
    g_bdg_applied = s_bdg_applied; g_bdg_prims = s_bdg_prims; g_bdg_clearx = s_bdg_clearx;
    g_bdg_cur = (g_wide_cur != 0); g_bdg_base = g_wide_cur_base; g_bdg_w = g_wide_w; g_bdg_off = g_wide_off;
    s_bdg_applied = 0; s_bdg_prims = 0; s_bdg_clearx = -999999;
    { extern void psx_ws_dbg_gate_frame_snapshot(void); psx_ws_dbg_gate_frame_snapshot(); }
    if (!s_pf_on) return;
    uint64_t now = SDL_GetPerformanceCounter();
    s_pf_enter = now;
    s_pf_total_pending = s_pf_last_enter
        ? (double)(now - s_pf_last_enter) * 1000.0 / (double)s_pf_freq : 0.0;
    s_pf_last_enter = now;
    s_pf_prims_pending = (double)(s_scene_prims - s_pf_prims_last);   /* prims drawn this frame */
    s_pf_prims_last = s_scene_prims;
    s_pf_cw_pending[0] = s_cw_flush_ms;  s_pf_cw_pending[1] = s_cw_wide_ms;
    s_pf_cw_pending[2] = (double)s_cw_batches;
    s_pf_cw_pending[3] = (double)s_cw_wide_sets;
    s_pf_cw_pending[4] = (double)s_cw_fbo_creates;
    s_cw_flush_ms = 0.0; s_cw_wide_ms = 0.0;
    s_cw_batches = 0; s_cw_wide_sets = 0; s_cw_wide_cfgs = 0;
    s_cw_wide_clears = 0; s_cw_fbo_creates = 0;
    if (s_pf_scene_active) { p_glEndQuery(GL_TIME_ELAPSED); s_pf_scene_active = 0; } /* end frame b's scene draws */
    p_glBeginQuery(GL_TIME_ELAPSED, s_pf_present_q[s_pf_b]);                         /* time frame b's present */
}

/* End of present (after SwapWindow). wide = native-wide path. */
static void gl_perf_present_exit(int wide) {
    if (!s_pf_on) return;
    uint64_t now = SDL_GetPerformanceCounter();
    p_glEndQuery(GL_TIME_ELAPSED);   /* end present_q[b] */
    s_pf_buf_total[s_pf_b] = s_pf_total_pending;
    s_pf_buf_pwall[s_pf_b] = (double)(now - s_pf_enter) * 1000.0 / (double)s_pf_freq;
    s_pf_buf_prims[s_pf_b] = s_pf_prims_pending;
    for (int ci = 0; ci < 5; ci++) s_pf_buf_cw[s_pf_b][ci] = s_pf_cw_pending[ci];
    s_pf_buf_wide[s_pf_b]  = wide;
    s_pf_buf_frame[s_pf_b] = s_pf_count;
    int rd = (s_pf_b + 1) % GLPERF_NBUF;   /* oldest buffer (frame count+1-NBUF), now done */
    if (s_pf_count >= (uint64_t)GLPERF_NBUF) {
        GLuint64 sc = 0, pr = 0;
        p_glGetQueryObjectui64v(s_pf_scene_q[rd],   GL_QUERY_RESULT, &sc);
        p_glGetQueryObjectui64v(s_pf_present_q[rd], GL_QUERY_RESULT, &pr);
        double mir = 0.0;
        for (int i = 0; i < s_mq_n[rd]; i++) {   /* sum that frame's mirror pairs */
            GLuint64 t0 = 0, t1 = 0;
            p_glGetQueryObjectui64v(s_mq_q[rd][i * 2],     GL_QUERY_RESULT, &t0);
            p_glGetQueryObjectui64v(s_mq_q[rd][i * 2 + 1], GL_QUERY_RESULT, &t1);
            if (t1 > t0) mir += (double)(t1 - t0);
        }
        GlPerfSample *s = &s_pf_ring[s_pf_ring_seq % GLPERF_RING];
        s->total_ms        = s_pf_buf_total[rd];
        s->present_wall_ms = s_pf_buf_pwall[rd];
        s->scene_gpu_ms    = (double)sc / 1.0e6;
        s->present_gpu_ms  = (double)pr / 1.0e6;
        s->mirror_gpu_ms   = mir / 1.0e6;
        s->prims           = s_pf_buf_prims[rd];
        s->mirror_passes   = (double)(s_mq_n[rd] + s_mq_over[rd]);
        s->cw_flush_ms     = s_pf_buf_cw[rd][0];
        s->cw_wide_ms      = s_pf_buf_cw[rd][1];
        s->batches         = s_pf_buf_cw[rd][2];
        s->wide_sets       = s_pf_buf_cw[rd][3];
        s->fbo_creates     = s_pf_buf_cw[rd][4];
        s->wide            = s_pf_buf_wide[rd];
        s->frame           = s_pf_buf_frame[rd];
        s_pf_ring_seq++;
    }
    s_mq_n[rd] = 0; s_mq_over[rd] = 0;   /* rd becomes the next frame's buffer */
    s_pf_count++;
    s_pf_b = rd;                                          /* reuse oldest for next frame */
    p_glBeginQuery(GL_TIME_ELAPSED, s_pf_scene_q[s_pf_b]); /* open next frame's scene draws */
    s_pf_scene_active = 1;
}

/* Aggregate the ring for the debug server. wide_filter: -1 all, 0 = 4:3, 1 = wide.
 * out[0]=count, [1]=total_avg, [2]=total_max, [3]=emu_cpu_avg (total-present_wall),
 * [4]=present_wall_avg, [5]=scene_gpu_avg, [6]=scene_gpu_max, [7]=present_gpu_avg,
 * [8]=present_gpu_max. Returns the sample count. */
/* Cumulative textured fraction of scene prims (decides flat vs textured batching
 * priority). out_tex_frac = textured/total since boot; returns total prim count. */
uint64_t gl_renderer_perf_prim_split(double *out_tex_frac) {
    if (out_tex_frac) *out_tex_frac = s_scene_prims ? (double)s_scene_prims_tex / (double)s_scene_prims : 0.0;
    return s_scene_prims;
}

int gl_renderer_perf_aggregate(int wide_filter, double out[18]) {
    for (int i = 0; i < 18; i++) out[i] = 0.0;
    if (!s_pf_on) return 0;
    int navail = (int)(s_pf_ring_seq < (uint64_t)GLPERF_RING ? s_pf_ring_seq : GLPERF_RING);
    uint64_t start = s_pf_ring_seq - (uint64_t)navail;
    int n = 0;
    for (int i = 0; i < navail; i++) {
        const GlPerfSample *s = &s_pf_ring[(start + i) % GLPERF_RING];
        if (wide_filter >= 0 && s->wide != wide_filter) continue;
        double emu = s->total_ms - s->present_wall_ms; if (emu < 0) emu = 0;
        out[1] += s->total_ms;       if (s->total_ms     > out[2]) out[2] = s->total_ms;
        out[3] += emu;
        out[4] += s->present_wall_ms;
        out[5] += s->scene_gpu_ms;   if (s->scene_gpu_ms > out[6]) out[6] = s->scene_gpu_ms;
        out[7] += s->present_gpu_ms; if (s->present_gpu_ms > out[8]) out[8] = s->present_gpu_ms;
        out[9] += s->prims;
        out[10] += s->mirror_gpu_ms; if (s->mirror_gpu_ms > out[11]) out[11] = s->mirror_gpu_ms;
        out[12] += s->mirror_passes;
        out[13] += s->cw_flush_ms;
        out[14] += s->cw_wide_ms;
        out[15] += s->batches;
        out[16] += s->wide_sets;
        out[17] += s->fbo_creates;
        n++;
    }
    if (n) {
        out[1]/=n; out[3]/=n; out[4]/=n; out[5]/=n; out[7]/=n; out[9]/=n; out[10]/=n; out[12]/=n;
        out[13]/=n; out[14]/=n; out[15]/=n; out[16]/=n; out[17]/=n;
    }
    out[0] = (double)n;
    return n;
}

/* Native-wide mirror ablation (perf attribution): see s_ws_ablate. */
void gl_renderer_set_ws_ablate(int mode) { s_ws_ablate = (mode >= 0 && mode <= 3) ? mode : 0; }
int  gl_renderer_get_ws_ablate(void)     { return s_ws_ablate; }

static void interp_reset_history_unlocked(void) {
    s_interp_valid = 0;
    s_interp_w = s_interp_h = 0;
    s_interp_start = s_interp_last_capture = 0;
    s_interp_duration = 1;
    s_interp_source_path = -1;
}

static void interp_reset_history(void) {
    if (s_interp_mutex) SDL_LockMutex(s_interp_mutex);
    interp_reset_history_unlocked();
    if (s_interp_mutex) SDL_UnlockMutex(s_interp_mutex);
}

void gl_renderer_set_interpolation(int enabled, double host_hz,
                                   double target_hz, int blend_mode) {
    int active = (enabled && host_hz >= 90.0) ? 1 : 0;
    double effective_hz = target_hz >= 90.0 ? target_hz : host_hz;
    const char *diag = getenv("PSX_GL_INTERP_DIAG");
    s_interp_diag = diag && diag[0] && diag[0] != '0';
    if (active && !s_interp_ctx && s_ctx) {
        if (!s_interp_mutex) s_interp_mutex = SDL_CreateMutex();
        SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
        s_interp_ctx = SDL_GL_CreateContext(s_win);
        SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
        SDL_GL_MakeCurrent(s_win, s_ctx);
        /* The wall-clock pacer and interpolation scheduler own cadence. Keeping
         * driver vsync on the main context adds a 6-11 ms block whenever an
         * FMV temporarily suspends interpolation and the main context presents. */
        SDL_GL_SetSwapInterval(0);
        if (s_interp_ctx && s_interp_mutex) {
            SDL_AtomicSet(&s_interp_thread_run, 1);
            s_interp_thread = SDL_CreateThread(interp_thread_main,
                                               "psx-gl-interp", NULL);
        }
        if (!s_interp_thread) {
            SDL_AtomicSet(&s_interp_thread_run, 0);
            if (s_interp_ctx) SDL_GL_DeleteContext(s_interp_ctx);
            s_interp_ctx = NULL;
            active = 0;
        }
    }
    if (s_interp_mutex) SDL_LockMutex(s_interp_mutex);
    if (active != s_interp_enabled) interp_reset_history_unlocked();
    s_interp_enabled = active;
    s_interp_blend_mode = blend_mode == 1 ? 1 : 0;
    s_interp_host_hz = host_hz;
    s_interp_target_hz = active ? effective_hz : 0.0;
    if (s_interp_mutex) SDL_UnlockMutex(s_interp_mutex);
    if (active)
        fprintf(stdout, "psxrecomp: GL frame interpolation enabled: %.1f FPS "
                "target on %.1f Hz display\n", effective_hz, host_hz);
}

void gl_renderer_set_interpolation_suspended(int suspended) {
    suspended = suspended ? 1 : 0;
    if (s_interp_mutex) SDL_LockMutex(s_interp_mutex);
    if (suspended != s_interp_suspended) interp_reset_history_unlocked();
    s_interp_suspended = suspended;
    if (s_interp_mutex) SDL_UnlockMutex(s_interp_mutex);
}

void gl_renderer_interpolation_diag(int *enabled, int *suspended,
                                    int *history_frames,
                                    double *host_hz, double *target_hz,
                                    uint64_t *swaps) {
    if (s_interp_mutex) SDL_LockMutex(s_interp_mutex);
    if (enabled) *enabled = s_interp_enabled;
    if (suspended) *suspended = s_interp_suspended;
    if (history_frames) *history_frames = s_interp_valid;
    if (host_hz) *host_hz = s_interp_host_hz;
    if (target_hz) *target_hz = s_interp_target_hz;
    if (swaps) *swaps = s_interp_swaps;
    if (s_interp_mutex) SDL_UnlockMutex(s_interp_mutex);
}

/* Copy a stable display image out of the mutable VRAM/wide render target.
 * Returns true once both previous and current images are available. */
static int interp_capture(GLuint fbo, int x, int y, int w, int h,
                          int linear, int force_4_3, int source_path) {
    if (!s_interp_enabled || s_interp_suspended || !fbo || w <= 0 || h <= 0) return 0;
    SDL_LockMutex(s_interp_mutex);
    /* The presentation context may still have a draw queued which samples one
     * of the shared history textures.  Order this context's next allocation or
     * copy after that draw before recycling a texture.  glWaitSync keeps the
     * dependency on the GPU; unlike glFinish it does not stall the guest CPU. */
    if (s_interp_draw_fence) {
        p_glWaitSync(s_interp_draw_fence, 0, PSXGL_TIMEOUT_IGNORED);
        p_glDeleteSync(s_interp_draw_fence);
        s_interp_draw_fence = NULL;
    }
    int pw = w * s_scale, ph = h * s_scale;
    if (pw != s_interp_w || ph != s_interp_h ||
        source_path != s_interp_source_path || force_4_3 != s_interp_force_4_3) {
        s_interp_valid = 0;
        s_interp_w = pw; s_interp_h = ph;
        s_interp_prev = s_interp_cur = 0;
        for (int i = 0; i < 3; i++) {
            glBindTexture(GL_TEXTURE_2D, s_interp_tex[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, pw, ph, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        }
    }

    int dst = 0;
    if (s_interp_valid == 1) dst = s_interp_cur == 0 ? 1 : 0;
    else if (s_interp_valid >= 2) dst = s_interp_prev;
    if (s_interp_fence[dst]) {
        p_glDeleteSync(s_interp_fence[dst]);
        s_interp_fence[dst] = NULL;
    }

    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, fbo);
    glBindTexture(GL_TEXTURE_2D, s_interp_tex[dst]);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                        x * s_scale, y * s_scale, pw, ph);
    s_interp_fence[dst] = p_glFenceSync(PSXGL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);

    uint64_t now = SDL_GetPerformanceCounter();
    uint64_t freq = SDL_GetPerformanceFrequency();
    if (s_interp_valid > 0 && s_interp_last_capture && now > s_interp_last_capture) {
        uint64_t d = now - s_interp_last_capture;
        uint64_t lo = freq / 240u, hi = freq / 10u;
        if (d < lo) d = lo;
        if (d > hi) d = hi;
        s_interp_duration = d;
    }
    s_interp_last_capture = now;
    s_interp_start = now;
    if (s_interp_valid == 0) {
        s_interp_cur = dst;
        s_interp_valid = 1;
    } else {
        s_interp_prev = s_interp_cur;
        s_interp_cur = dst;
        s_interp_valid = 2;
    }
    s_interp_linear = linear;
    s_interp_force_4_3 = force_4_3;
    s_interp_source_path = source_path;
    s_interp_captures++;
    int ready = s_interp_valid >= 2;
    SDL_UnlockMutex(s_interp_mutex);
    return ready;
}

static void interp_draw_quad(float alpha, int lx, int ly, int lw, int lh) {
    int prev = s_interp_prev, curr = s_interp_cur;
    if (s_interp_fence[prev])
        p_glWaitSync(s_interp_fence[prev], 0, PSXGL_TIMEOUT_IGNORED);
    if (s_interp_fence[curr])
        p_glWaitSync(s_interp_fence[curr], 0, PSXGL_TIMEOUT_IGNORED);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    glViewport(lx, ly, lw, lh);
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_interp_tex[prev]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, s_interp_linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, s_interp_linear ? GL_LINEAR : GL_NEAREST);
    p_glActiveTexture(PSXGL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, s_interp_tex[curr]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, s_interp_linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, s_interp_linear ? GL_LINEAR : GL_NEAREST);
    p_glUseProgram(s_interp_prog);
    p_glUniform1i(s_interp_uPrev, 0);
    p_glUniform1i(s_interp_uCurr, 1);
    p_glUniform1f(s_interp_uAlpha, alpha);
    p_glUniform1i(s_interp_uBlendMode, s_interp_blend_mode);
    p_glUniform4f(s_interp_uUvRect, 0.f, 0.f, 1.f, 1.f);
    p_glBindVertexArray(s_interp_thread_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    p_glBindVertexArray(0);
    p_glUseProgram(0);
    p_glActiveTexture(PSXGL_TEXTURE0);
}

static int interp_present(void) {
    if (!s_ctx || !s_interp_enabled || s_interp_suspended || s_interp_valid < 2) return 0;
    uint64_t now = SDL_GetPerformanceCounter();
    if (now <= s_interp_start || !s_interp_duration) return 0;
    double a = (double)(now - s_interp_start) / (double)s_interp_duration;
    /* Keep swapping at the host cadence after the blend completes.  Holding
     * alpha at one is visually identical to leaving the current image on the
     * front buffer, but avoids an irregular 2/3-swap pattern on 120/144/165 Hz
     * displays while the next 59.94 Hz guest frame is being produced. */
    if (a > 1.0) a = 1.0;
    if (a < 0.0) a = 0.0;

    int ww = 0, wh = 0; SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    int lx, ly, lw, lh;
    if (s_interp_force_4_3)
        letterbox_rect_aspect(ww, wh, 4, 3, &lx, &ly, &lw, &lh);
    else
        letterbox_rect(ww, wh, &lx, &ly, &lw, &lh);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, ww, wh);
    if (lx != 0 || ly != 0 || lw != ww || lh != wh) {
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    interp_draw_quad((float)a, lx, ly, lw, lh);
    if (s_interp_draw_fence) p_glDeleteSync(s_interp_draw_fence);
    s_interp_draw_fence = p_glFenceSync(PSXGL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    uint64_t present_sequence =
        pres_record(GL_PRES_INTERP, 0, 0, s_interp_w, s_interp_h,
                    lx, ly, lw, lh);
    SDL_GL_SwapWindow(s_win);
    pres_mark_swap_completed(present_sequence);
    s_probe_swap++;
    s_interp_swaps++;
    return 1;
}

static int interp_thread_main(void *opaque) {
    (void)opaque;
    if (SDL_GL_MakeCurrent(s_win, s_interp_ctx) != 0) return -1;
    SDL_GL_SetSwapInterval(0); /* host-period scheduler owns cadence */
    p_glGenVertexArrays(1, &s_interp_thread_vao);
    uint64_t freq = SDL_GetPerformanceFrequency();
    uint64_t deadline = SDL_GetPerformanceCounter();
    uint64_t diag_start = deadline, diag_swaps = 0, diag_captures = 0;

    while (SDL_AtomicGet(&s_interp_thread_run)) {
        SDL_LockMutex(s_interp_mutex);
        double hz = s_interp_target_hz >= 90.0 ? s_interp_target_hz : 120.0;
        SDL_UnlockMutex(s_interp_mutex);
        uint64_t period = (uint64_t)((double)freq / hz);
        if (!period) period = 1;
        deadline += period;
        uint64_t now = SDL_GetPerformanceCounter();
        if (now > deadline + period * 4u) deadline = now + period;
        for (;;) {
            now = SDL_GetPerformanceCounter();
            if (now >= deadline) break;
            uint64_t remain = deadline - now;
            uint32_t ms = (uint32_t)((remain * 1000u) / (freq ? freq : 1u));
            if (ms > 1) SDL_Delay(ms - 1);
        }
        while (SDL_GetPerformanceCounter() < deadline) {}

        SDL_LockMutex(s_interp_mutex);
        if (SDL_AtomicGet(&s_interp_thread_run) && s_interp_enabled)
            interp_present();
        if (s_interp_diag && now - diag_start >= freq * 5u) {
            double seconds = (double)(now - diag_start) / (double)freq;
            fprintf(stdout, "psxrecomp: GL interpolation cadence: "
                    "%.2f captures/s, %.2f presents/s\n",
                    (double)(s_interp_captures - diag_captures) / seconds,
                    (double)(s_interp_swaps - diag_swaps) / seconds);
            fflush(stdout);
            diag_start = now;
            diag_captures = s_interp_captures;
            diag_swaps = s_interp_swaps;
        }
        SDL_UnlockMutex(s_interp_mutex);
    }
    p_glBindVertexArray(0);
    SDL_GL_MakeCurrent(s_win, NULL);
    return 0;
}

/* Re-upload the screen LUT texture only when gpu.c's generation bumps.
 * Cheap per-present no-op otherwise. */
static void update_screen_lut(void) {
    const uint8_t *tab = NULL;
    int gen = gpu_screen_lut_snapshot(&tab);
    if (gen == s_lut_gen_seen) return;
    s_lut_gen_seen = gen;
    if (!tab) { s_lut_on = 0; return; }
    if (!s_lut_tex) {
        glGenTextures(1, &s_lut_tex);
        glBindTexture(GL_TEXTURE_2D, s_lut_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, s_lut_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 256, 128, 0,
                 GL_RGB, GL_UNSIGNED_BYTE, tab);
    s_lut_on = 1;
}

static void present_target_quad(GLuint target_fbo,
                                GLuint tex, int tex_w, int tex_h,
                                int x, int y, int w, int h, int linear,
                                int lx, int ly, int lw, int lh) {
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, target_fbo);
    glViewport(lx, ly, lw, lh);
    update_screen_lut();
    if (s_lut_on) {
        p_glActiveTexture(PSXGL_TEXTURE0 + 1);
        glBindTexture(GL_TEXTURE_2D, s_lut_tex);
        p_glActiveTexture(PSXGL_TEXTURE0);
    }
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    p_glUseProgram(s_present_prog);
    p_glUniform1i(s_present_uTex, 0);
    p_glUniform1i(s_present_uLutOn, s_lut_on);
    /* Half-texel inset: with GL_LINEAR, corner-mapped UVs make the outermost
     * dest pixels blend the border texel with VRAM outside the content rect
     * (visible edge stripe with AA on). Center-mapped UVs keep edge samples
     * inside the rect; interior sampling is unchanged. */
    p_glUniform4f(s_present_uUvRect,
                  ((float)x + 0.5f) / (float)tex_w, ((float)y + 0.5f) / (float)tex_h,
                  ((float)(x + w) - 0.5f) / (float)tex_w, ((float)(y + h) - 0.5f) / (float)tex_h);
    p_glBindVertexArray(s_present_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    p_glBindVertexArray(0);
    p_glUseProgram(0);
}

int gl_renderer_present_hold_last(void) {
    int ww = 0, wh = 0;
    int lx, ly, lw, lh;

    if (s_transaction || !s_ctx || !s_win ||
        s_hold_kind == HOLD_NONE || !s_hold_tex)
        return 0;
    SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    if (ww < 1 || wh < 1) return 0;

    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, ww, wh);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (s_hold_kind == HOLD_DRAWABLE) {
        float u0 = 0.5f / (float)s_hold_tw;
        float v0 = 0.5f / (float)s_hold_th;
        float u1 = 1.f - u0;
        float v1 = 1.f - v0;

        lx = 0;
        ly = 0;
        lw = ww;
        lh = wh;
        if ((int64_t)s_hold_tw * wh != (int64_t)s_hold_th * ww) {
            if ((int64_t)ww * s_hold_th >
                (int64_t)wh * s_hold_tw) {
                lw = (int)((int64_t)wh * s_hold_tw / s_hold_th);
                if (lw < 1) lw = 1;
                lx = (ww - lw) / 2;
            } else {
                lh = (int)((int64_t)ww * s_hold_th / s_hold_tw);
                if (lh < 1) lh = 1;
                ly = (wh - lh) / 2;
            }
        }
        glViewport(lx, ly, lw, lh);
        p_glActiveTexture(PSXGL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_hold_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        p_glUseProgram(s_present_prog);
        p_glUniform1i(s_present_uTex, 0);
        p_glUniform1i(s_present_uLutOn, 0);
        /* A drawable copy is already screen-oriented; reverse the shader's
         * normal guest-FBO vertical flip. */
        p_glUniform4f(s_present_uUvRect, u0, v1, u1, v0);
        p_glBindVertexArray(s_present_vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        p_glBindVertexArray(0);
        p_glUseProgram(0);
    } else {
        if (s_hold_force_4_3)
            letterbox_rect_aspect(ww, wh, 4, 3, &lx, &ly, &lw, &lh);
        else
            letterbox_rect(ww, wh, &lx, &ly, &lw, &lh);
        present_target_quad(0, s_hold_tex, s_hold_tw, s_hold_th,
                            0, 0, s_hold_tw, s_hold_th, s_hold_linear,
                            lx, ly, lw, lh);
        hold_capture_drawable();
    }

    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    psx_debug_overlay_pre_swap();
    latency_ring_mark(LAT_SWAP_BEGIN);
    SDL_GL_SwapWindow(s_win);
    latency_ring_mark(LAT_SWAP_END);
    s_probe_swap++;
    return 1;
}

int gl_renderer_present_vram(int disp_x, int disp_y, int w, int h, int linear,
                             int force_4_3) {
    if (s_transaction) {
        (void)glb_transaction_reject_other_present();
        return 0;
    }
    if (!s_ctx || !s_raster_ok) return 0;
    if (!glb_transaction_prepare_original_present()) return 0;
    flush_flat_batch();
    flush_tex_batch();
    flush_cpu_upload();
    if (s_force_present_remaining <= 0 &&
        s_last_present_path == GL_PRES_VRAM &&
        s_last_dx == disp_x && s_last_dy == disp_y &&
        s_last_dw == w && s_last_dh == h &&
        !present_dirty_test(disp_x, disp_y, disp_x + w - 1, disp_y + h - 1)) {
        s_probe_skip++;
        gl_perf_present_enter();
        gl_perf_present_exit(0);
        return 1;
    }
    gl_perf_present_enter();   /* per-frame backdrop-phase reset + dbg snapshot live in here */
    int ww = 0, wh = 0; SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    int lx, ly, lw, lh;
    if (force_4_3)
        letterbox_rect_aspect(ww, wh, 4, 3, &lx, &ly, &lw, &lh);
    else
        letterbox_rect(ww, wh, &lx, &ly, &lw, &lh);
    /* No short-band adjustment on the 15-bit FBO path: a game's native short
     * display mode (e.g. 216/224-line menus) must fill the rect as before.
     * The FMV band fix lives in the depth24/CPU present paths only. */

    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, ww, wh);
    if (lx != 0 || ly != 0 || lw != ww || lh != wh) {
        glClearColor(0.f,0.f,0.f,1.f); glClear(GL_COLOR_BUFFER_BIT);
    }
    int interp_pair = s_transaction_force_original ? 0 :
        interp_capture(s_hr_fbo, disp_x, disp_y, w, h,
                       linear, force_4_3, GL_PRES_VRAM);
    if (interp_pair) {
        hold_capture_native_fbo(s_hr_fbo, disp_x, disp_y, w, h,
                                force_4_3, linear);
        gl_perf_present_exit(0);
        present_dirty_rect(disp_x, disp_y, disp_x + w - 1, disp_y + h - 1, 0);
        present_force_consumed();
        s_last_present_path = GL_PRES_VRAM;
        s_last_dx = disp_x; s_last_dy = disp_y; s_last_dw = w; s_last_dh = h;
        return 1;
    }
    present_target_quad(0, s_hr_tex, VRAM_W, VRAM_H,
                        disp_x, disp_y, w, h, linear, lx, ly, lw, lh);
    uint64_t present_sequence =
        pres_record(GL_PRES_VRAM, disp_x, disp_y, w, h, lx, ly, lw, lh);
    /* Pre-swap hook. present_target_quad already bound FBO 0 (DRAW only)
     * for its own draw at the top of this function; rebind READ too so
     * the hook's readback targets the default framebuffer's back buffer. */
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    hold_capture_drawable();
    psx_debug_overlay_pre_swap();
    latency_ring_mark(LAT_SWAP_BEGIN);
    SDL_GL_SwapWindow(s_win);
    pres_mark_swap_completed(present_sequence);
    s_probe_swap++;
    latency_ring_mark(LAT_SWAP_END);
    gl_perf_present_exit(0);
    present_dirty_rect(disp_x, disp_y, disp_x + w - 1, disp_y + h - 1, 0);
    present_force_consumed();
    s_last_present_path = GL_PRES_VRAM;
    s_last_dx = disp_x; s_last_dy = disp_y; s_last_dw = w; s_last_dh = h;
    coh_record(GL_COH_PRESENT, disp_x, disp_y, disp_x + w - 1, disp_y + h - 1);
    glb_transaction_original_presented();
    return 1;
}

void gl_renderer_native_midpoint_reset_for_reason(
        GlRendererNativeMidpointResetReason reason) {
    if (reason < GL_NATIVE_MIDPOINT_RESET_EXPLICIT ||
        reason >= GL_NATIVE_MIDPOINT_RESET_REASON_COUNT)
        reason = GL_NATIVE_MIDPOINT_RESET_EXPLICIT;
    s_native_midpoint_diag.reset_count++;
    s_native_midpoint_diag.reset_reason_counts[reason]++;
    if (s_native_midpoint_diag.previous_usable) {
        s_native_midpoint_diag.reset_with_previous_count++;
        s_native_midpoint_diag.reset_with_previous_reason_counts[reason]++;
    }
    if (s_native_midpoint_current_pending)
        s_native_midpoint_diag.reset_with_pending_count++;
    s_native_midpoint_diag.last_reset_reason = (uint32_t)reason;
    if (glb_transaction_context_ready())
        (void)native_host_pending_flush();
    s_native_host_queue_count = 0u;
    s_native_host_queue_last_present_count = 0u;
    s_native_host_queue_flushing = 0;
    s_native_host_queue_midpoint_rendered = 0;
    s_native_host_semantic_history_generation[0] = 0u;
    s_native_host_semantic_history_generation[1] = 0u;
    s_native_host_semantic_context_history_count[0] = 0u;
    s_native_host_semantic_context_history_count[1] = 0u;
    s_native_host_semantic_history_index = 0u;
    s_native_host_semantic_history_valid = 0;
    s_native_midpoint_current_pending = 0;
    s_native_midpoint_pending_phase = 0u;
    s_native_midpoint_pending_slot = -1;
    s_native_midpoint_pending_scanout_y = 0;
    s_native_midpoint_promoted_y = 0;
    s_native_midpoint_promoted_scanout_y = 0;
    s_native_midpoint_promoted_valid = 0;
    s_native_present_deadline = 0u;
    gpu_semantic_workload_reset();
    s_native_midpoint_diag.frame_open = 0;
    s_native_midpoint_diag.frame_valid = 0;
    s_native_midpoint_diag.previous_usable = 0;
    s_native_midpoint_frame_blocked = 0;
    s_native_midpoint_duplicate_seen = 0;
    s_native_midpoint_canonical_enabled = 0;
    memset(s_native_midpoint_seeded, 0, sizeof(s_native_midpoint_seeded));
    memset(s_native_extra_phase_seeded, 0,
           sizeof(s_native_extra_phase_seeded));
    memset(s_canonical_geometry_hash, 0, sizeof(s_canonical_geometry_hash));
    memset(s_canonical_geometry_count, 0, sizeof(s_canonical_geometry_count));
    memset(s_native_view_geometry_hash, 0, sizeof(s_native_view_geometry_hash));
    memset(s_native_view_geometry_count, 0, sizeof(s_native_view_geometry_count));
    memset(s_pending_canonical_geometry_hash, 0,
           sizeof(s_pending_canonical_geometry_hash));
    memset(s_pending_canonical_geometry_count, 0,
           sizeof(s_pending_canonical_geometry_count));
    memset(s_pending_native_view_geometry_hash, 0,
           sizeof(s_pending_native_view_geometry_hash));
    memset(s_pending_native_view_geometry_count, 0,
           sizeof(s_pending_native_view_geometry_count));
}

void gl_renderer_native_midpoint_reset(void) {
    gl_renderer_native_midpoint_reset_for_reason(
        GL_NATIVE_MIDPOINT_RESET_EXPLICIT);
}

static void native_midpoint_cancel_with_reason(
        GlRendererNativeMidpointCancelReason reason, uint32_t status,
        const GpuRenderSemantic *semantic) {
    for (size_t index = 0u; index < s_native_host_queue_count; ++index)
        s_native_host_queue[index].midpoint_valid = 0;
    if (!s_native_midpoint_frame_blocked &&
        (s_native_midpoint_diag.frame_open ||
         s_native_midpoint_diag.frame_valid)) {
        GpuSemanticWorkloadDiagnostics workload_diag = {0};

        gpu_semantic_workload_diagnostics(&workload_diag);
        s_native_midpoint_frame_blocked = 1;
        s_native_midpoint_diag.cancelled_frames++;
        if (reason > GL_NATIVE_MIDPOINT_CANCEL_NONE &&
            reason < GL_NATIVE_MIDPOINT_CANCEL_REASON_COUNT)
            s_native_midpoint_diag.cancel_reason_counts[reason]++;
        s_native_midpoint_diag.last_cancel_reason = (uint32_t)reason;
        s_native_midpoint_diag.last_cancel_status = status;
        s_native_midpoint_diag.last_cancel_workload_current =
            workload_diag.current_count;
        s_native_midpoint_diag.last_cancel_identity_scene = semantic != NULL
            ? semantic->interpolation_identity.scene_id : 0u;
        s_native_midpoint_diag.last_cancel_identity_producer = semantic != NULL
            ? semantic->interpolation_identity.producer_id : 0u;
        s_native_midpoint_diag.last_cancel_identity_primitive = semantic != NULL
            ? semantic->interpolation_identity.primitive_id : 0u;
        s_native_midpoint_diag.last_cancel_identity_valid = semantic != NULL &&
            semantic->interpolation_identity.valid;
    }
    /* Never seal a partially rendered source frame: a failed wave or side pass
     * must not become the prior semantic match for the next source frame. */
    gpu_semantic_workload_reset();
    s_native_midpoint_diag.frame_open = 0;
    s_native_midpoint_diag.frame_valid = 0;
    s_native_midpoint_diag.previous_usable = 0;
}

void gl_renderer_native_midpoint_cancel(void) {
    native_midpoint_cancel_with_reason(
        GL_NATIVE_MIDPOINT_CANCEL_GENERIC, 0u, NULL);
}

int gl_renderer_native_midpoint_begin(void) {
    if (s_native_midpoint_diag.suspended || s_native_midpoint_frame_blocked ||
        s_native_midpoint_diag.frame_open || !s_midpoint_fbo ||
        !glb_transaction_context_ready())
        return 0;
    if (s_native_host_queue_midpoint_rendered &&
        native_host_queue_flush() != GPU_RENDER_TRANSACTION_OK)
        return 0;
    flush_flat_batch();
    flush_tex_batch();
    flush_cpu_upload();
    if (gpu_semantic_workload_begin() != GPU_SEMANTIC_WORKLOAD_OK)
        return 0;
    s_native_host_semantic_context_history_count[
        s_native_host_semantic_history_index ^ 1u] = 0u;
    memset(s_canonical_geometry_hash, 0, sizeof(s_canonical_geometry_hash));
    memset(s_canonical_geometry_count, 0, sizeof(s_canonical_geometry_count));
    memset(s_native_view_geometry_hash, 0, sizeof(s_native_view_geometry_hash));
    memset(s_native_view_geometry_count, 0, sizeof(s_native_view_geometry_count));
    s_native_midpoint_diag.frame_open = 1;
    s_native_midpoint_diag.frame_valid = 1;
    s_native_midpoint_diag.previous_usable =
        gpu_semantic_workload_previous_frame_usable() ? 1 : 0;
    s_native_midpoint_diag.begun_frames++;
    s_native_midpoint_canonical_enabled =
        !s_native_view_enabled || gpu_ws_present_native_43();
    if (s_native_midpoint_canonical_enabled &&
        !s_native_midpoint_current_pending)
        native_midpoint_seed_canonical();
    if (!s_native_midpoint_diag.frame_open) return 0;
    for (int slot = 0; slot < NATIVE_VIEW_MAX_SURF; ++slot)
        if (s_native_view_seeded[slot] &&
            !s_native_midpoint_current_pending)
            native_midpoint_seed_slot(slot);
    return s_native_midpoint_diag.frame_open;
}

GpuRenderTransactionStatus gl_renderer_record_interpolation_anchors(
        const GpuRenderInterpolationVertexAnchor *anchors, size_t count) {
    GpuSemanticWorkloadStatus status;

    if (anchors == NULL && count != 0u)
        return GPU_RENDER_TRANSACTION_INVALID_ARGUMENT;
    if (count == 0u || s_native_midpoint_diag.suspended ||
        s_native_midpoint_frame_blocked)
        return GPU_RENDER_TRANSACTION_OK;
    if (!s_native_midpoint_diag.frame_open &&
        !gl_renderer_native_midpoint_begin())
        return GPU_RENDER_TRANSACTION_INVALID_TRANSITION;
    status = gpu_semantic_workload_record_anchors(anchors, count);
    if (status != GPU_SEMANTIC_WORKLOAD_OK) {
        native_midpoint_cancel_with_reason(
            GL_NATIVE_MIDPOINT_CANCEL_WORKLOAD_RECORD, (uint32_t)status, NULL);
        return status == GPU_SEMANTIC_WORKLOAD_CAPACITY_EXCEEDED
            ? GPU_RENDER_TRANSACTION_STATE_REJECTED
            : GPU_RENDER_TRANSACTION_INVALID_ARGUMENT;
    }
    return GPU_RENDER_TRANSACTION_OK;
}

int gl_renderer_native_midpoint_seal(void) {
    if (!s_native_midpoint_diag.frame_open ||
        !s_native_midpoint_diag.frame_valid ||
        !gpu_semantic_workload_current_frame_has_work())
        return 0;
    if (gpu_semantic_workload_seal() != GPU_SEMANTIC_WORKLOAD_OK) {
        gl_renderer_native_midpoint_cancel();
        return 0;
    }
    s_native_midpoint_diag.frame_open = 0;
    s_native_midpoint_diag.previous_usable =
        gpu_semantic_workload_previous_frame_usable() ? 1 : 0;
    s_native_midpoint_diag.sealed_frames++;
    return 1;
}

static void native_midpoint_discard_open_frame(void) {
    if (!s_native_midpoint_diag.frame_open) return;
    if (gpu_semantic_workload_discard_current() != GPU_SEMANTIC_WORKLOAD_OK) {
        gl_renderer_native_midpoint_cancel();
        return;
    }
    s_native_midpoint_diag.frame_open = 0;
    s_native_midpoint_diag.frame_valid = 0;
    s_native_midpoint_diag.previous_usable =
        gpu_semantic_workload_previous_frame_usable() ? 1 : 0;
}

/* Returns non-zero only when the current source image can use its midpoint.
 * Empty VBlanks discard their open workload while retaining the last semantic
 * source. Uploads, fills, copies, and margin clears are mirrored into the
 * midpoint targets when issued; they are current discrete GPU state, not a new
 * primitive identity and must not sever the retrospective A->B link. */
static int native_midpoint_prepare_present(int *out_had_work) {
    const int had_work = s_native_midpoint_diag.frame_open &&
        gpu_semantic_workload_current_frame_has_work();
    GpuSemanticWorkloadDiagnostics workload_diag = {0};

    if (out_had_work) *out_had_work = had_work;
    if (!s_native_midpoint_diag.frame_open) return 0;
    if (!had_work) {
        native_midpoint_discard_open_frame();
        s_native_midpoint_diag.midpoint_duplicate_empty_frames++;
        s_native_midpoint_duplicate_seen = 1;
        return 0;
    }
    if (!s_native_midpoint_diag.frame_valid ||
        !gl_renderer_native_midpoint_seal()) {
        gl_renderer_native_midpoint_cancel();
        return 0;
    }
    gpu_semantic_workload_diagnostics(&workload_diag);
    switch (workload_diag.last_seal_eligibility) {
    case GPU_SEMANTIC_WORKLOAD_ELIGIBILITY_ELIGIBLE:
        s_native_midpoint_diag.eligibility_complete_frames++;
        break;
    case GPU_SEMANTIC_WORKLOAD_ELIGIBILITY_PARTIAL_COUNT_MISMATCH:
        s_native_midpoint_diag.eligibility_partial_count_mismatch_frames++;
        break;
    case GPU_SEMANTIC_WORKLOAD_ELIGIBILITY_PARTIAL_INCOMPLETE_MATCH:
        s_native_midpoint_diag.eligibility_partial_incomplete_match_frames++;
        break;
    case GPU_SEMANTIC_WORKLOAD_ELIGIBILITY_NO_PREVIOUS:
        s_native_midpoint_diag.eligibility_no_previous_frames++;
        break;
    case GPU_SEMANTIC_WORKLOAD_ELIGIBILITY_OVERFLOW:
        s_native_midpoint_diag.eligibility_overflow_frames++;
        break;
    case GPU_SEMANTIC_WORKLOAD_ELIGIBILITY_COUNT_MISMATCH:
        s_native_midpoint_diag.eligibility_count_mismatch_frames++;
        break;
    case GPU_SEMANTIC_WORKLOAD_ELIGIBILITY_INCOMPLETE_MATCH:
        s_native_midpoint_diag.eligibility_incomplete_match_frames++;
        break;
    case GPU_SEMANTIC_WORKLOAD_ELIGIBILITY_STATIC:
        s_native_midpoint_diag.eligibility_static_frames++;
        break;
    case GPU_SEMANTIC_WORKLOAD_ELIGIBILITY_UNKNOWN:
    default:
        break;
    }
    if (workload_diag.matched_count != 0u &&
        workload_diag.moved_count == 0u) {
        s_native_midpoint_diag.midpoint_duplicate_static_frames++;
        s_native_midpoint_duplicate_seen = 1;
        return 0;
    }
    {
        int use_midpoint = 0;

        if (s_native_midpoint_diag.previous_usable) {
            if (s_native_midpoint_duplicate_seen) {
                s_native_midpoint_diag.midpoint_candidates++;
                use_midpoint = 1;
            } else {
                s_native_midpoint_diag
                    .midpoint_eligible_without_duplicate_frames++;
            }
        } else if (s_native_midpoint_duplicate_seen) {
            s_native_midpoint_diag
                .midpoint_ineligible_after_duplicate_frames++;
        } else {
            s_native_midpoint_diag
                .midpoint_ineligible_without_duplicate_frames++;
        }
        s_native_midpoint_duplicate_seen = 0;
        return use_midpoint;
    }
}

static void native_midpoint_begin_after_present(void) {
    if (s_native_midpoint_frame_blocked)
        s_native_midpoint_frame_blocked = 0;
    if (!s_native_midpoint_diag.suspended &&
        !s_native_host_queue_midpoint_rendered)
        (void)gl_renderer_native_midpoint_begin();
}

static int native_midpoint_axis_target(int display_origin, int extent,
                                       int draw_area_origin, int draw_area_end,
                                       int draw_offset, int limit) {
    const int minimum_separation = extent / 2;
    int64_t delta;

    if (extent <= 0 || extent > limit) return display_origin;
    delta = (int64_t)draw_offset - display_origin;
    if (draw_offset >= 0 && draw_offset <= limit - extent &&
        (delta >= minimum_separation || delta <= -minimum_separation) &&
        draw_area_end >= draw_offset &&
        draw_area_origin < draw_offset + extent)
        return draw_offset;
    delta = (int64_t)draw_area_origin - display_origin;
    if (draw_area_origin >= 0 && draw_area_origin <= limit - extent &&
        (delta >= minimum_separation || delta <= -minimum_separation))
        return draw_area_origin;
    return display_origin;
}

/* Native interpolation can see a complete backbuffer before GP1(05h) publishes
 * it. A distinct, valid GP0 draw band is therefore the phase source; small
 * offsets inside the scanout remain ordinary draw transforms. */
static void native_midpoint_current_target(int display_x, int display_y,
                                           int display_w, int display_h,
                                           int *out_x, int *out_y) {
    GpuDrawArea draw = {0};

    gpu_get_draw_area(&draw);
    *out_x = native_midpoint_axis_target(
        display_x, display_w, (int)draw.left, (int)draw.right,
        draw.offset_x, VRAM_W);
    *out_y = native_midpoint_axis_target(
        display_y, display_h, (int)draw.top, (int)draw.bottom,
        draw.offset_y, VRAM_H);
}

void gl_renderer_native_midpoint_set_suspended(int suspended) {
    const int next = suspended ? 1 : 0;
    if (next == s_native_midpoint_diag.suspended) return;
    gl_renderer_native_midpoint_reset_for_reason(
        GL_NATIVE_MIDPOINT_RESET_SUSPENSION_CHANGE);
    s_native_midpoint_diag.suspended = next;
}

void gl_renderer_native_midpoint_diag(
        GlRendererNativeMidpointDiagnostics *out_diagnostics) {
    if (out_diagnostics) {
        GpuSemanticWorkloadDiagnostics workload_diag = {0};

        gpu_semantic_workload_diagnostics(&workload_diag);
        *out_diagnostics = s_native_midpoint_diag;
        out_diagnostics->target_fps =
            (uint32_t)gl_renderer_native_interpolation_fps();
        out_diagnostics->phase_count = s_native_interpolation_phase_count;
        out_diagnostics->current_pending_present =
            s_native_midpoint_current_pending;
        out_diagnostics->workload_epoch = workload_diag.epoch;
        out_diagnostics->workload_recorded = workload_diag.total_recorded;
        out_diagnostics->workload_total_matched =
            workload_diag.total_matched;
        out_diagnostics->workload_total_snapped =
            workload_diag.total_snapped;
        out_diagnostics->workload_total_ambiguous =
            workload_diag.total_ambiguous;
        out_diagnostics->workload_total_moved = workload_diag.total_moved;
        out_diagnostics->workload_total_unkeyed = workload_diag.total_unkeyed;
        out_diagnostics->workload_total_exact_matches =
            workload_diag.total_exact_matches;
        out_diagnostics->workload_total_exact_semitransparent_matches =
            workload_diag.total_exact_semitransparent_matches;
        out_diagnostics->workload_total_source_geometry_matches =
            workload_diag.total_source_geometry_matches;
        out_diagnostics->workload_total_matched_vertices =
            workload_diag.total_matched_vertices;
        out_diagnostics->workload_total_position_changed_vertices =
            workload_diag.total_position_changed_vertices;
        out_diagnostics->workload_total_position_delta_fixed =
            workload_diag.total_position_delta_fixed;
        out_diagnostics->workload_max_semantic_position_delta_fixed =
            workload_diag.max_semantic_position_delta_fixed;
        out_diagnostics->workload_max_semantic_identity_scene =
            workload_diag.max_semantic_identity_scene;
        out_diagnostics->workload_max_semantic_identity_producer =
            workload_diag.max_semantic_identity_producer;
        out_diagnostics->workload_max_semantic_identity_primitive =
            workload_diag.max_semantic_identity_primitive;
        out_diagnostics->workload_max_semantic_identity_valid =
            workload_diag.max_semantic_identity_valid;
        out_diagnostics->workload_total_unkeyed_moved_matches =
            workload_diag.total_unkeyed_moved_matches;
        out_diagnostics->workload_total_unkeyed_motion_over_32px =
            workload_diag.total_unkeyed_motion_over_32px;
        out_diagnostics->workload_total_unkeyed_motion_over_64px =
            workload_diag.total_unkeyed_motion_over_64px;
        out_diagnostics->workload_total_unkeyed_motion_over_128px =
            workload_diag.total_unkeyed_motion_over_128px;
        out_diagnostics->workload_total_unkeyed_motion_over_192px =
            workload_diag.total_unkeyed_motion_over_192px;
        out_diagnostics->workload_total_unkeyed_motion_over_240px =
            workload_diag.total_unkeyed_motion_over_240px;
        out_diagnostics->workload_max_keyed_semantic_position_delta_fixed =
            workload_diag.max_keyed_semantic_position_delta_fixed;
        out_diagnostics->workload_max_keyed_semantic_identity_scene =
            workload_diag.max_keyed_semantic_identity_scene;
        out_diagnostics->workload_max_keyed_semantic_identity_producer =
            workload_diag.max_keyed_semantic_identity_producer;
        out_diagnostics->workload_max_keyed_semantic_identity_primitive =
            workload_diag.max_keyed_semantic_identity_primitive;
        out_diagnostics->workload_total_keyed_moved_matches =
            workload_diag.total_keyed_moved_matches;
        out_diagnostics->workload_total_keyed_motion_over_32px =
            workload_diag.total_keyed_motion_over_32px;
        out_diagnostics->workload_total_keyed_motion_over_64px =
            workload_diag.total_keyed_motion_over_64px;
        out_diagnostics->workload_total_keyed_motion_over_128px =
            workload_diag.total_keyed_motion_over_128px;
        out_diagnostics->workload_total_keyed_motion_over_192px =
            workload_diag.total_keyed_motion_over_192px;
        out_diagnostics->workload_total_keyed_motion_over_240px =
            workload_diag.total_keyed_motion_over_240px;
        out_diagnostics->workload_total_midpoint_distinct_vertices =
            workload_diag.total_midpoint_distinct_vertices;
        out_diagnostics->workload_total_midpoint_collapsed_vertices =
            workload_diag.total_midpoint_collapsed_vertices;
        out_diagnostics->workload_total_midpoint_formula_failures =
            workload_diag.total_midpoint_formula_failures;
        out_diagnostics->workload_total_projective_input_vertices =
            workload_diag.total_projective_input_vertices;
        out_diagnostics->workload_total_projective_valid_input_vertices =
            workload_diag.total_projective_valid_input_vertices;
        out_diagnostics->workload_total_projective_phase_vertices =
            workload_diag.total_projective_phase_vertices;
        out_diagnostics->workload_total_previous_unmatched =
            workload_diag.total_previous_unmatched;
        out_diagnostics->workload_total_previous_unmatched_keyed =
            workload_diag.total_previous_unmatched_keyed;
        out_diagnostics->workload_total_previous_unmatched_projective =
            workload_diag.total_previous_unmatched_projective;
        out_diagnostics->workload_total_retrospective_semitransparent_rejected =
            workload_diag.total_retrospective_semitransparent_rejected;
        out_diagnostics->workload_total_eligible_frames =
            workload_diag.total_eligible_frames;
        out_diagnostics->workload_total_rejected_no_previous_frames =
            workload_diag.total_rejected_no_previous_frames;
        out_diagnostics->workload_total_rejected_overflow_frames =
            workload_diag.total_rejected_overflow_frames;
        out_diagnostics->workload_total_rejected_count_mismatch_frames =
            workload_diag.total_rejected_count_mismatch_frames;
        out_diagnostics->workload_total_rejected_incomplete_match_frames =
            workload_diag.total_rejected_incomplete_match_frames;
        out_diagnostics->workload_total_rejected_static_frames =
            workload_diag.total_rejected_static_frames;
        out_diagnostics->workload_total_partial_count_mismatch_frames =
            workload_diag.total_partial_count_mismatch_frames;
        out_diagnostics->workload_total_partial_incomplete_match_frames =
            workload_diag.total_partial_incomplete_match_frames;
        out_diagnostics->workload_current = workload_diag.current_count;
        out_diagnostics->workload_previous = workload_diag.previous_count;
        out_diagnostics->workload_matched = workload_diag.matched_count;
        out_diagnostics->workload_snapped = workload_diag.snapped_count;
        out_diagnostics->workload_ambiguous = workload_diag.ambiguous_count;
        out_diagnostics->workload_moved = workload_diag.moved_count;
        out_diagnostics->workload_unkeyed = workload_diag.unkeyed_count;
        out_diagnostics->workload_exact_matches =
            workload_diag.exact_match_count;
        out_diagnostics->workload_exact_semitransparent_matches =
            workload_diag.exact_semitransparent_match_count;
        out_diagnostics->workload_source_geometry_matches =
            workload_diag.source_geometry_match_count;
        out_diagnostics->workload_matched_vertices =
            workload_diag.matched_vertex_count;
        out_diagnostics->workload_position_changed_vertices =
            workload_diag.position_changed_vertex_count;
        out_diagnostics->workload_position_delta_fixed =
            workload_diag.position_delta_fixed;
        out_diagnostics->workload_midpoint_distinct_vertices =
            workload_diag.midpoint_distinct_vertex_count;
        out_diagnostics->workload_midpoint_collapsed_vertices =
            workload_diag.midpoint_collapsed_vertex_count;
        out_diagnostics->workload_midpoint_formula_failures =
            workload_diag.midpoint_formula_failure_count;
        out_diagnostics->workload_retrospective_candidates =
            workload_diag.retrospective_candidates;
        out_diagnostics->workload_retrospective_budget_exhausted =
            workload_diag.retrospective_budget_exhausted;
        out_diagnostics->workload_retrospective_semitransparent_rejected =
            workload_diag.retrospective_semitransparent_rejected;
        out_diagnostics->workload_last_previous =
            workload_diag.last_seal_previous_count;
        out_diagnostics->workload_last_current =
            workload_diag.last_seal_current_count;
        out_diagnostics->workload_last_previous_unkeyed =
            workload_diag.last_seal_previous_unkeyed_count;
        out_diagnostics->workload_last_current_unkeyed =
            workload_diag.last_seal_current_unkeyed_count;
        out_diagnostics->workload_last_matched =
            workload_diag.last_seal_matched_count;
        out_diagnostics->workload_last_snapped =
            workload_diag.last_seal_snapped_count;
        out_diagnostics->workload_last_ambiguous =
            workload_diag.last_seal_ambiguous_count;
        out_diagnostics->workload_last_moved =
            workload_diag.last_seal_moved_count;
        out_diagnostics->workload_last_exact_matches =
            workload_diag.last_seal_exact_match_count;
        out_diagnostics->workload_last_exact_semitransparent_matches =
            workload_diag.last_seal_exact_semitransparent_match_count;
        out_diagnostics->workload_last_previous_unmatched =
            workload_diag.last_seal_previous_unmatched_count;
        out_diagnostics->workload_last_previous_unmatched_keyed =
            workload_diag.last_seal_previous_unmatched_keyed_count;
        out_diagnostics->workload_last_previous_unmatched_projective =
            workload_diag.last_seal_previous_unmatched_projective_count;
        out_diagnostics->workload_last_previous_overflowed =
            workload_diag.last_seal_previous_overflowed ? 1 : 0;
        out_diagnostics->workload_last_current_overflowed =
            workload_diag.last_seal_current_overflowed ? 1 : 0;
        out_diagnostics->workload_last_eligibility =
            (uint32_t)workload_diag.last_seal_eligibility;
    }
}

static void native_midpoint_note_completed_present(void) {
    GpuSemanticWorkloadDiagnostics workload_diag = {0};

    gpu_semantic_workload_diagnostics(&workload_diag);
    s_native_midpoint_diag.presented_midpoint_matched_vertices +=
        workload_diag.matched_vertex_count;
    s_native_midpoint_diag.presented_midpoint_position_changed_vertices +=
        workload_diag.position_changed_vertex_count;
    s_native_midpoint_diag.presented_midpoint_distinct_vertices +=
        workload_diag.midpoint_distinct_vertex_count;
    s_native_midpoint_diag.presented_midpoint_collapsed_vertices +=
        workload_diag.midpoint_collapsed_vertex_count;
    s_native_midpoint_diag.presented_midpoint_formula_failures +=
        workload_diag.midpoint_formula_failure_count;
    s_native_midpoint_diag.presented_midpoint_position_delta_fixed +=
        workload_diag.position_delta_fixed;
}

static void native_present_wait_until(uint64_t deadline, uint64_t frequency) {
    for (;;) {
        const uint64_t now = SDL_GetPerformanceCounter();
        uint64_t remaining;
        uint32_t milliseconds;

        if (now >= deadline) break;
        remaining = deadline - now;
        milliseconds = frequency != 0u
            ? (uint32_t)(remaining * 1000u / frequency) : 0u;
        if (milliseconds <= 1u) break;
        SDL_Delay(milliseconds - 1u);
    }
    while (SDL_GetPerformanceCounter() < deadline) {}
}

static void native_present_wait_next(uint64_t period, uint64_t frequency) {
    uint64_t now;

    if (s_native_interpolation_denominator <= 2u) {
        s_native_present_deadline = 0u;
        return;
    }
    if (period == 0u || frequency == 0u) {
        s_native_present_deadline = 0u;
        return;
    }
    now = SDL_GetPerformanceCounter();
    if (s_native_present_deadline == 0u ||
        (now >= s_native_present_deadline &&
         now - s_native_present_deadline >= period * 4u)) {
        s_native_present_deadline = now;
    }
    native_present_wait_until(s_native_present_deadline, frequency);
    s_native_present_deadline += period;
}

static uint64_t native_present_swap_texture(
        GLuint texture, int texture_width, int texture_height,
        GLuint source_fbo, GLuint phase_surface_fbo,
        int source_x, int source_y, int source_width, int source_height,
        int scanout_x, int scanout_y, int scanout_width, int scanout_height,
        int linear, int window_width, int window_height,
        int lx, int ly, int lw, int lh, int path,
        unsigned int phase_numerator, unsigned int phase_denominator,
        uint64_t geometry_hash, int geometry_hash_valid,
        int hash_framebuffer) {
    uint64_t sequence;

    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, window_width, window_height);
    if (lx != 0 || ly != 0 || lw != window_width || lh != window_height) {
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    present_target_quad(
        0, texture, texture_width, texture_height,
        source_x, source_y, source_width, source_height,
        linear, lx, ly, lw, lh);
    sequence = pres_record(
        path, source_x, source_y, source_width, source_height,
        lx, ly, lw, lh);
    pres_set_phase(sequence, phase_numerator, phase_denominator);
    pres_set_scanout(
        sequence, scanout_x, scanout_y, scanout_width, scanout_height);
    pres_set_geometry_hash(sequence, geometry_hash, geometry_hash_valid);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    hold_capture_drawable();
    psx_debug_overlay_pre_swap();
    if (hash_framebuffer) {
        pres_phase_vram_hash_issue(sequence, phase_surface_fbo, texture_width);
        pres_phase_surface_hash_issue(
            sequence, phase_surface_fbo,
            source_x * s_scale, source_y * s_scale,
            source_width * s_scale, source_height * s_scale);
        pres_source_hash_issue(
            sequence, source_fbo, source_x * s_scale, source_y * s_scale,
            source_width * s_scale, source_height * s_scale);
        pres_hash_issue(sequence, window_width, window_height);
    }
    (void)psx_wayland_presentation_request(sequence);
    latency_ring_mark(LAT_SWAP_BEGIN);
    SDL_GL_SwapWindow(s_win);
    pres_mark_swap_completed(sequence);
    s_probe_swap++;
    latency_ring_mark(LAT_SWAP_END);
    if (path == GL_PRES_NATIVE_MIDPOINT) {
        s_native_midpoint_diag.midpoint_presents++;
        if (phase_numerator * 2u == phase_denominator)
            native_midpoint_note_completed_present();
    } else {
        s_native_midpoint_diag.current_presents++;
    }
    return sequence;
}

static int native_midpoint_snapshot_current(int disp_x, int disp_y,
                                            int scanout_y,
                                            int disp_w, int disp_h,
                                            int native_view,
                                            unsigned int pending_phase) {
    const int canonical_width = VRAM_W * s_scale;
    const int native_width = s_native_view_width * s_scale;
    const int height = VRAM_H * s_scale;
    int pending_slot = -1;

    if (!s_hr_fbo || !s_midpoint_fbo) return 0;
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_hr_fbo);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, s_midpoint_fbo);
    glDisable(GL_SCISSOR_TEST);
    p_glBlitFramebuffer(
        0, 0, canonical_width, height,
        0, 0, canonical_width, height,
        GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
    for (int slot = 0; slot < NATIVE_VIEW_MAX_SURF; ++slot) {
        if (!s_native_view_seeded[slot] || !s_native_view_fbo[slot] ||
            !s_native_midpoint_fbo[slot])
            continue;
        if (native_view &&
            s_native_view_base[slot] == (disp_x & (VRAM_W - 1)))
            pending_slot = slot;
        p_glBindFramebuffer(
            PSXGL_READ_FRAMEBUFFER, s_native_view_fbo[slot]);
        p_glBindFramebuffer(
            PSXGL_DRAW_FRAMEBUFFER, s_native_midpoint_fbo[slot]);
        p_glBlitFramebuffer(
            0, 0, native_width, height,
            0, 0, native_width, height,
            GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
        s_native_midpoint_seeded[slot] = 1;
    }
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
    if (!native_midpoint_gl_ok(GL_NATIVE_MIDPOINT_GL_SNAPSHOT_CURRENT))
        return 0;
    if (native_view && pending_slot < 0) return 0;
    memcpy(s_pending_canonical_geometry_hash, s_canonical_geometry_hash,
           sizeof(s_pending_canonical_geometry_hash));
    memcpy(s_pending_canonical_geometry_count, s_canonical_geometry_count,
           sizeof(s_pending_canonical_geometry_count));
    memcpy(s_pending_native_view_geometry_hash, s_native_view_geometry_hash,
           sizeof(s_pending_native_view_geometry_hash));
    memcpy(s_pending_native_view_geometry_count, s_native_view_geometry_count,
           sizeof(s_pending_native_view_geometry_count));
    s_native_midpoint_current_pending = 1;
    s_native_midpoint_pending_phase = pending_phase;
    s_native_midpoint_pending_slot = pending_slot;
    s_native_midpoint_pending_x = disp_x;
    s_native_midpoint_pending_y = disp_y;
    s_native_midpoint_pending_scanout_y = scanout_y;
    s_native_midpoint_pending_w = disp_w;
    s_native_midpoint_pending_h = disp_h;
    return 1;
}

static int native_midpoint_pending_matches(int slot, int disp_x, int disp_y,
                                           int disp_w, int disp_h) {
    const int matches = s_native_midpoint_current_pending &&
        s_native_midpoint_pending_slot == slot &&
        s_native_midpoint_pending_x == disp_x &&
        (s_native_midpoint_pending_scanout_y == disp_y ||
         s_native_midpoint_pending_y == disp_y) &&
        s_native_midpoint_pending_w == disp_w &&
        s_native_midpoint_pending_h == disp_h;

    if (matches && s_native_midpoint_pending_y != disp_y)
        s_native_midpoint_diag.pending_vertical_lag_count++;
    return matches;
}

/* Once a completed draw band has been promoted through midpoint/current, keep
 * presenting it until GP1(05h) catches up. Falling back to the older scanout
 * band in that interval would make time run backwards for one or more VBlanks. */
static int native_midpoint_promoted_source_y(int scanout_y) {
    if (!s_native_midpoint_promoted_valid) return scanout_y;
    if (scanout_y == s_native_midpoint_promoted_y) {
        s_native_midpoint_promoted_valid = 0;
        return scanout_y;
    }
    if (scanout_y == s_native_midpoint_promoted_scanout_y)
        return s_native_midpoint_promoted_y;
    s_native_midpoint_promoted_valid = 0;
    return scanout_y;
}

static void native_midpoint_note_pending_mismatch(int slot, int disp_x,
                                                   int disp_y, int disp_w,
                                                   int disp_h) {
    if (s_native_midpoint_pending_slot != slot)
        s_native_midpoint_diag.pending_mismatch_slot_count++;
    if (s_native_midpoint_pending_x != disp_x)
        s_native_midpoint_diag.pending_mismatch_x_count++;
    if (s_native_midpoint_pending_y != disp_y)
        s_native_midpoint_diag.pending_mismatch_y_count++;
    if (s_native_midpoint_pending_w != disp_w)
        s_native_midpoint_diag.pending_mismatch_width_count++;
    if (s_native_midpoint_pending_h != disp_h)
        s_native_midpoint_diag.pending_mismatch_height_count++;
    s_native_midpoint_diag.last_pending_slot = s_native_midpoint_pending_slot;
    s_native_midpoint_diag.last_pending_x = s_native_midpoint_pending_x;
    s_native_midpoint_diag.last_pending_y = s_native_midpoint_pending_y;
    s_native_midpoint_diag.last_pending_width = s_native_midpoint_pending_w;
    s_native_midpoint_diag.last_pending_height = s_native_midpoint_pending_h;
    s_native_midpoint_diag.last_present_slot = slot;
    s_native_midpoint_diag.last_present_x = disp_x;
    s_native_midpoint_diag.last_present_y = disp_y;
    s_native_midpoint_diag.last_present_width = disp_w;
    s_native_midpoint_diag.last_present_height = disp_h;
}

static void native_midpoint_finish_present(int used_midpoint,
                                           int presented_pending_current,
                                           int had_work, int disp_x,
                                           int disp_y, int scanout_y,
                                           int disp_w, int disp_h,
                                           int native_view,
                                           unsigned int next_pending_phase) {
    const int preserve_current = used_midpoint ||
        (presented_pending_current && had_work);

    if (!used_midpoint && !presented_pending_current) return;
    if (presented_pending_current) {
        if (s_native_midpoint_pending_y != scanout_y &&
            s_native_midpoint_pending_scanout_y == scanout_y) {
            s_native_midpoint_promoted_y = s_native_midpoint_pending_y;
            s_native_midpoint_promoted_scanout_y = scanout_y;
            s_native_midpoint_promoted_valid = 1;
        } else if (s_native_midpoint_pending_y == scanout_y) {
            s_native_midpoint_promoted_valid = 0;
        }
    }
    s_native_midpoint_current_pending = 0;
    s_native_midpoint_pending_phase = 0u;
    memset(s_pending_canonical_geometry_hash, 0,
           sizeof(s_pending_canonical_geometry_hash));
    memset(s_pending_canonical_geometry_count, 0,
           sizeof(s_pending_canonical_geometry_count));
    memset(s_pending_native_view_geometry_hash, 0,
           sizeof(s_pending_native_view_geometry_hash));
    memset(s_pending_native_view_geometry_count, 0,
           sizeof(s_pending_native_view_geometry_count));
    if (native_host_pending_flush() != GPU_RENDER_TRANSACTION_OK) {
        gl_renderer_native_midpoint_cancel();
        return;
    }
    if (s_scale_apply_pending) gl_maybe_apply_scale();
    if (preserve_current) {
        flush_cpu_upload();
        if (!glb_transaction_context_ready()) return;
        if (!native_midpoint_snapshot_current(
                disp_x, disp_y, scanout_y, disp_w, disp_h, native_view,
                next_pending_phase))
            gl_renderer_native_midpoint_cancel();
    }
}

static void native_midpoint_seed_canonical(void) {
    const int width = VRAM_W * s_scale;
    const int height = VRAM_H * s_scale;

    if (!native_midpoint_active() || !s_hr_fbo) {
        gl_renderer_native_midpoint_cancel();
        return;
    }
    for (unsigned int phase = 0u;
         phase < s_native_interpolation_phase_count; ++phase) {
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_hr_fbo);
        p_glBindFramebuffer(
            PSXGL_DRAW_FRAMEBUFFER, native_phase_fbo(phase));
        glDisable(GL_SCISSOR_TEST);
        p_glBlitFramebuffer(0, 0, width, height, 0, 0, width, height,
                            GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT,
                            GL_NEAREST);
    }
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
    if (!native_midpoint_gl_ok(GL_NATIVE_MIDPOINT_GL_SEED_CANONICAL))
        gl_renderer_native_midpoint_cancel();
}

static void native_view_wave_reset(void) {
    memset(&s_native_view_wave, 0, sizeof(s_native_view_wave));
    s_native_view_wave.slot = -1;
    for (unsigned int variant = 0u;
         variant < NATIVE_VIEW_WAVE_VARIANTS; ++variant)
        for (int page = 0; page < 2; ++page)
            s_native_view_wave.vertical_anchor_source[variant][page] = -1;
}

static void native_view_free_all(void) {
    gl_renderer_native_midpoint_reset_for_reason(
        GL_NATIVE_MIDPOINT_RESET_VIEW_FREE);
    native_view_wave_reset();
    s_native_view_wave_authenticated = 0;
    s_native_view_wave_authenticated_slot = -1;
    for (int i = 0; i < NATIVE_VIEW_MAX_SURF; ++i) {
        if (s_native_view_fbo[i])
            p_glDeleteFramebuffers(1, &s_native_view_fbo[i]);
        if (s_native_view_tex[i]) glDeleteTextures(1, &s_native_view_tex[i]);
        if (s_native_view_rb[i])
            p_glDeleteRenderbuffers(1, &s_native_view_rb[i]);
        if (s_native_midpoint_fbo[i])
            p_glDeleteFramebuffers(1, &s_native_midpoint_fbo[i]);
        if (s_native_midpoint_tex[i])
            glDeleteTextures(1, &s_native_midpoint_tex[i]);
        if (s_native_midpoint_rb[i])
            p_glDeleteRenderbuffers(1, &s_native_midpoint_rb[i]);
        for (unsigned int phase = 1u;
             phase < NATIVE_INTERPOLATION_MAX_PHASES; ++phase)
            native_phase_free_view(i, phase);
        s_native_view_fbo[i] = 0;
        s_native_view_tex[i] = 0;
        s_native_view_rb[i] = 0;
        s_native_midpoint_fbo[i] = 0;
        s_native_midpoint_tex[i] = 0;
        s_native_midpoint_rb[i] = 0;
        s_native_view_base[i] = -1;
        s_native_view_seeded[i] = 0;
    }
    s_native_view_pass = 0;
    s_native_view_pass_fbo = 0;
    s_native_view_pass_base = 0;
    s_native_view_expand_x = 0;
    s_native_view_scale_2d = 0;
    s_native_view_preserve_2d_translation_x = 0;
}

static int native_view_surface_slot(int base_x, int create) {
    for (int i = 0; i < NATIVE_VIEW_MAX_SURF; ++i) {
        if (s_native_view_fbo[i] && s_native_view_base[i] == base_x) return i;
    }
    if (!create || !s_native_view_enabled || s_native_view_width <= 0) return -1;
    for (int i = 0; i < NATIVE_VIEW_MAX_SURF; ++i) {
        if (!s_native_view_fbo[i]) {
            const int width = s_native_view_width * s_scale;
            const int height = VRAM_H * s_scale;

            s_native_view_tex[i] = make_tex(
                GL_RGBA8, width, height, GL_RGBA, GL_UNSIGNED_BYTE);
            s_native_midpoint_tex[i] = make_tex(
                GL_RGBA8, width, height, GL_RGBA, GL_UNSIGNED_BYTE);
            p_glGenRenderbuffers(1, &s_native_view_rb[i]);
            p_glBindRenderbuffer(PSXGL_RENDERBUFFER, s_native_view_rb[i]);
            p_glRenderbufferStorage(
                PSXGL_RENDERBUFFER, PSXGL_DEPTH24_STENCIL8, width, height);
            p_glGenRenderbuffers(1, &s_native_midpoint_rb[i]);
            p_glBindRenderbuffer(PSXGL_RENDERBUFFER,
                                 s_native_midpoint_rb[i]);
            p_glRenderbufferStorage(
                PSXGL_RENDERBUFFER, PSXGL_DEPTH24_STENCIL8, width, height);
            if (!make_fbo(&s_native_view_fbo[i], s_native_view_tex[i],
                          s_native_view_rb[i]) ||
                !make_fbo(&s_native_midpoint_fbo[i],
                          s_native_midpoint_tex[i],
                          s_native_midpoint_rb[i])) {
                if (s_native_view_fbo[i])
                    p_glDeleteFramebuffers(1, &s_native_view_fbo[i]);
                if (s_native_midpoint_fbo[i])
                    p_glDeleteFramebuffers(1, &s_native_midpoint_fbo[i]);
                if (s_native_view_tex[i])
                    glDeleteTextures(1, &s_native_view_tex[i]);
                if (s_native_midpoint_tex[i])
                    glDeleteTextures(1, &s_native_midpoint_tex[i]);
                if (s_native_view_rb[i])
                    p_glDeleteRenderbuffers(1, &s_native_view_rb[i]);
                if (s_native_midpoint_rb[i])
                    p_glDeleteRenderbuffers(1, &s_native_midpoint_rb[i]);
                s_native_view_fbo[i] = 0;
                s_native_midpoint_fbo[i] = 0;
                s_native_view_tex[i] = 0;
                s_native_midpoint_tex[i] = 0;
                s_native_view_rb[i] = 0;
                s_native_midpoint_rb[i] = 0;
                return -1;
            }
            for (unsigned int phase = 1u;
                 phase < s_native_interpolation_phase_count; ++phase) {
                if (!native_phase_allocate_view(i, phase, width, height)) {
                    for (unsigned int allocated = 1u;
                         allocated <= phase; ++allocated)
                        native_phase_free_view(i, allocated);
                    if (s_native_view_fbo[i])
                        p_glDeleteFramebuffers(1, &s_native_view_fbo[i]);
                    if (s_native_midpoint_fbo[i])
                        p_glDeleteFramebuffers(1, &s_native_midpoint_fbo[i]);
                    if (s_native_view_tex[i])
                        glDeleteTextures(1, &s_native_view_tex[i]);
                    if (s_native_midpoint_tex[i])
                        glDeleteTextures(1, &s_native_midpoint_tex[i]);
                    if (s_native_view_rb[i])
                        p_glDeleteRenderbuffers(1, &s_native_view_rb[i]);
                    if (s_native_midpoint_rb[i])
                        p_glDeleteRenderbuffers(1, &s_native_midpoint_rb[i]);
                    s_native_view_fbo[i] = 0;
                    s_native_midpoint_fbo[i] = 0;
                    s_native_view_tex[i] = 0;
                    s_native_midpoint_tex[i] = 0;
                    s_native_view_rb[i] = 0;
                    s_native_midpoint_rb[i] = 0;
                    return -1;
                }
            }
            s_native_view_base[i] = base_x;
            return i;
        }
    }
    return -1;
}

int gl_renderer_configure_native_view(int enabled, int aspect_num,
                                      int aspect_den, int canonical_width,
                                      int canonical_height) {
    int width;

    if (!s_raster_ok || !s_ctx) return enabled ? 0 : 1;
    flush_flat_batch();
    flush_tex_batch();
    native_view_free_all();
    s_native_view_enabled = 0;
    s_native_view_width = 0;
    s_native_view_offset = 0;
    s_native_view_canonical_height = 240;
    if (!enabled) return 1;
    if (aspect_num <= 0 || aspect_den <= 0 || canonical_width <= 0 ||
        canonical_height <= 0 ||
        aspect_num * canonical_height <= aspect_den * canonical_width)
        return 0;
    width = (canonical_height * aspect_num + aspect_den / 2) / aspect_den;
    if (width <= canonical_width || width > VRAM_W) return 0;
    s_native_view_width = width;
    s_native_view_offset = (width - canonical_width) / 2;
    s_native_view_canonical_width = canonical_width;
    s_native_view_canonical_height = canonical_height;
    s_native_view_enabled = 1;
    s_native_midpoint_diag.suspended = 0;
    return 1;
}

int gl_renderer_native_view_width(void) {
    return s_native_view_enabled ? s_native_view_width : 0;
}

static int native_view_prepare_surface(int base_x) {
    int slot;
    const int width = s_native_view_width * s_scale;
    const int height = VRAM_H * s_scale;
    int logical_x;

    base_x &= VRAM_W - 1;
    slot = native_view_surface_slot(base_x, 1);
    if (slot < 0) return -1;
    if (s_native_view_seeded[slot]) return slot;
    flush_flat_batch();
    flush_tex_batch();
    flush_cpu_upload();
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_native_view_fbo[slot]);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClearStencil(0);
    glStencilMask(0xff);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_hr_fbo);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, s_native_view_fbo[slot]);
    logical_x = 0;
    while (logical_x < s_native_view_canonical_width) {
        const int source_x = (base_x + logical_x) & (VRAM_W - 1);
        const int copy_w = s_native_view_canonical_width - logical_x <
                VRAM_W - source_x
            ? s_native_view_canonical_width - logical_x
            : VRAM_W - source_x;
        const int destination_x = s_native_view_offset + logical_x;

        p_glBlitFramebuffer(
            source_x * s_scale, 0,
            (source_x + copy_w) * s_scale, height,
            destination_x * s_scale, 0,
            (destination_x + copy_w) * s_scale, height,
            GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
        logical_x += copy_w;
    }
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
    glViewport(0, 0, width, height);
    s_native_view_seeded[slot] = 1;
    if (s_native_midpoint_diag.frame_open) native_midpoint_seed_slot(slot);
    return slot;
}

static void native_midpoint_seed_slot(int slot) {
    const int width = s_native_view_width * s_scale;
    const int height = VRAM_H * s_scale;

    if (slot < 0 || slot >= NATIVE_VIEW_MAX_SURF ||
        !s_native_view_seeded[slot] || !s_native_view_fbo[slot] ||
        !s_native_midpoint_fbo[slot]) {
        gl_renderer_native_midpoint_cancel();
        return;
    }
    for (unsigned int phase = 0u;
         phase < s_native_interpolation_phase_count; ++phase) {
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,
                            s_native_view_fbo[slot]);
        p_glBindFramebuffer(
            PSXGL_DRAW_FRAMEBUFFER, native_view_phase_fbo(slot, phase));
        glDisable(GL_SCISSOR_TEST);
        p_glBlitFramebuffer(0, 0, width, height, 0, 0, width, height,
                            GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT,
                            GL_NEAREST);
        *native_view_phase_seeded(slot, phase) = 1;
    }
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
    if (!native_midpoint_gl_ok(GL_NATIVE_MIDPOINT_GL_SEED_VIEW)) {
        for (unsigned int phase = 0u;
             phase < s_native_interpolation_phase_count; ++phase)
            *native_view_phase_seeded(slot, phase) = 0;
        gl_renderer_native_midpoint_cancel();
    }
}

static void native_view_mirror_canonical_rects(const DirtyRect *rects,
                                                  int rect_count) {
    const int scale = s_scale;

    if (!s_native_view_enabled || !rects || rect_count <= 0) return;
    for (int slot = 0; slot < NATIVE_VIEW_MAX_SURF; ++slot) {
        const int base_x = s_native_view_base[slot];

        if (!s_native_view_fbo[slot] || !s_native_view_seeded[slot]) continue;
        for (int index = 0; index < rect_count; ++index) {
            const int y0 = rects[index].y0;
            const int y1 = rects[index].y1;
            int logical_x = 0;

            while (logical_x < s_native_view_canonical_width) {
                const int source_x =
                    (base_x + logical_x) & (VRAM_W - 1);
                const int span_w = s_native_view_canonical_width - logical_x <
                        VRAM_W - source_x
                    ? s_native_view_canonical_width - logical_x
                    : VRAM_W - source_x;
                const int x0 = rects[index].x0 > source_x
                    ? rects[index].x0 : source_x;
                const int x1 = rects[index].x1 < source_x + span_w - 1
                    ? rects[index].x1 : source_x + span_w - 1;

                if (x0 <= x1 && y0 <= y1) {
                    const int destination_x = s_native_view_offset +
                        logical_x + x0 - source_x;
                    const int phase_targets =
                        s_native_midpoint_diag.frame_open &&
                        !s_native_midpoint_current_pending &&
                        s_native_midpoint_diag.frame_valid
                            ? (int)s_native_interpolation_phase_count : 0;

                    for (int target = -1; target < phase_targets; ++target) {
                        const GLuint target_fbo = target < 0
                            ? s_native_view_fbo[slot]
                            : native_view_phase_fbo(
                                  slot, (unsigned int)target);
                        p_glBindFramebuffer(
                            PSXGL_READ_FRAMEBUFFER, s_hr_fbo);
                        p_glBindFramebuffer(
                            PSXGL_DRAW_FRAMEBUFFER, target_fbo);
                        glDisable(GL_SCISSOR_TEST);
                        p_glBlitFramebuffer(
                            x0 * scale, y0 * scale,
                            (x1 + 1) * scale, (y1 + 1) * scale,
                            destination_x * scale, y0 * scale,
                            (destination_x + x1 - x0 + 1) * scale,
                            (y1 + 1) * scale,
                            GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT,
                            GL_NEAREST);
                    }
                }
                logical_x += span_w;
            }
        }
    }
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
}

static void native_view_fill_local_rect(GLuint target_fbo, int local_x, int y,
                                        int local_w, int h, uint16_t color) {
    float red;
    float green;
    float blue;

    if (!target_fbo || local_w <= 0 || h <= 0)
        return;
    if (local_x < 0) {
        local_w += local_x;
        local_x = 0;
    }
    if (local_x + local_w > s_native_view_width)
        local_w = s_native_view_width - local_x;
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (y + h > VRAM_H) h = VRAM_H - y;
    if (local_w <= 0 || h <= 0) return;
    red = (color & 0x1f) / 31.0f;
    green = ((color >> 5) & 0x1f) / 31.0f;
    blue = ((color >> 10) & 0x1f) / 31.0f;
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, target_fbo);
    glViewport(0, 0, s_native_view_width * s_scale, VRAM_H * s_scale);
    glEnable(GL_SCISSOR_TEST);
    glScissor(local_x * s_scale, y * s_scale,
              local_w * s_scale, h * s_scale);
    glClearColor(red, green, blue, 0.0f);
    glClearStencil(0);
    glStencilMask(0xff);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
}

static void native_view_fill_local_wrapped_y(GLuint target_fbo, int local_x, int y,
                                             int w, int h, uint16_t color) {
    int first_height;

    if (h <= 0) return;
    y &= VRAM_H - 1;
    if (h > VRAM_H) h = VRAM_H;
    first_height = h < VRAM_H - y ? h : VRAM_H - y;
    native_view_fill_local_rect(target_fbo, local_x, y, w, first_height, color);
    if (first_height < h)
        native_view_fill_local_rect(target_fbo, local_x, 0, w,
                                    h - first_height, color);
}

static void native_view_fill_slot_wrapped_y(int slot, int local_x, int y,
                                              int w, int h, uint16_t color) {
    native_view_fill_local_wrapped_y(
        s_native_view_fbo[slot], local_x, y, w, h, color);
    if (s_native_midpoint_diag.frame_open &&
        !s_native_midpoint_current_pending &&
        s_native_midpoint_diag.frame_valid) {
        for (unsigned int phase = 0u;
             phase < s_native_interpolation_phase_count; ++phase)
            native_view_fill_local_wrapped_y(
                native_view_phase_fbo(slot, phase),
                local_x, y, w, h, color);
        if (!native_midpoint_gl_ok(GL_NATIVE_MIDPOINT_GL_FILL_VIEW))
            gl_renderer_native_midpoint_cancel();
    }
}

static void glb_native_view_clear_margins(int base_x, int y, int h,
                                             uint16_t color) {
    int slot;

    if (!s_native_view_enabled || s_native_view_width <= 0 || h <= 0 ||
        !s_ctx || SDL_GL_GetCurrentContext() != s_ctx || s_transaction)
        return;
    slot = native_view_surface_slot(base_x & (VRAM_W - 1), 0);
    if (slot < 0 || !s_native_view_seeded[slot]) return;
    s_native_view_wave_diag.margin_clears++;
    if (y == 0 || y == 256) {
        const int page = y / 256;

        s_native_view_wave_diag.margin_clears_by_page[page]++;
        s_native_view_wave_diag.wave_valid_by_page[page] = 0;
    }
    if (native_host_queue_push_margin_clear(
            slot, y, h, color,
            s_native_midpoint_diag.frame_open &&
                    !s_native_midpoint_current_pending &&
                    s_native_midpoint_diag.frame_valid) !=
        GPU_RENDER_TRANSACTION_OK) {
        gl_renderer_native_midpoint_cancel();
        return;
    }
    if (s_native_midpoint_diag.frame_open) {
        s_native_midpoint_diag.nonsemantic_margin_clears++;
    }
}

static void native_view_ensure_destination_surfaces(int x, int w) {
    int remaining;
    int segment_x;

    if (!s_native_view_enabled || w <= 0) return;
    segment_x = x & (VRAM_W - 1);
    remaining = w > VRAM_W ? VRAM_W : w;
    while (remaining > 0) {
        const int segment_w = remaining < VRAM_W - segment_x
            ? remaining : VRAM_W - segment_x;
        const int segment_end = segment_x + segment_w;
        int base_x = (segment_x / s_native_view_canonical_width) *
                     s_native_view_canonical_width;

        while (base_x < segment_end) {
            (void)native_view_prepare_surface(base_x);
            base_x += s_native_view_canonical_width;
        }
        remaining -= segment_w;
        segment_x = 0;
    }
}

static void glb_native_fill_rect(int x, int y, int w, int h, uint16_t color) {
    const int operation_x = x & (VRAM_W - 1);
    const int operation_w = w > VRAM_W ? VRAM_W : w;

    if (s_cpu_auth_dual || !s_raster_ok)
        sw_fill_rect(x, y, w, h, color);
    if (!s_raster_ok) {
        return;
    }
    gpu_fill(x, y, w, h, color);
    native_midpoint_mirror_wrapped_rect(x, y, w, h);
    if (!s_native_view_enabled || w <= 0) return;
    native_view_ensure_destination_surfaces(operation_x, operation_w);
    for (int slot = 0; slot < NATIVE_VIEW_MAX_SURF; ++slot) {
        const int base_x = s_native_view_base[slot];
        const int base_distance = (base_x - operation_x) & (VRAM_W - 1);
        int operation_logical_x = 0;

        if (!s_native_view_fbo[slot] || !s_native_view_seeded[slot]) continue;
        if (operation_w == VRAM_W ||
                base_distance + s_native_view_canonical_width <= operation_w) {
            native_view_fill_slot_wrapped_y(
                slot, 0, y, s_native_view_width, h, color);
            continue;
        }
        while (operation_logical_x < operation_w) {
            const int destination_x =
                (operation_x + operation_logical_x) & (VRAM_W - 1);
            const int destination_w = operation_w - operation_logical_x <
                    VRAM_W - destination_x
                ? operation_w - operation_logical_x : VRAM_W - destination_x;
            int surface_logical_x = 0;

            while (surface_logical_x < s_native_view_canonical_width) {
                const int surface_x =
                    (base_x + surface_logical_x) & (VRAM_W - 1);
                const int surface_w = s_native_view_canonical_width -
                            surface_logical_x < VRAM_W - surface_x
                    ? s_native_view_canonical_width - surface_logical_x
                    : VRAM_W - surface_x;
                const int overlap_x = destination_x > surface_x
                    ? destination_x : surface_x;
                const int overlap_end = destination_x + destination_w <
                            surface_x + surface_w
                    ? destination_x + destination_w : surface_x + surface_w;

                if (overlap_x < overlap_end)
                    native_view_fill_slot_wrapped_y(
                        slot,
                        s_native_view_offset + surface_logical_x +
                            overlap_x - surface_x,
                        y, overlap_end - overlap_x, h, color);
                surface_logical_x += surface_w;
            }
            operation_logical_x += destination_w;
        }
    }
}

static int native_view_wave_displacement(
        const int boundaries[NATIVE_VIEW_WAVE_COLUMNS + 1], int x) {
    int wrapped = x % s_native_view_canonical_width;
    int index;
    int fraction;
    int first;
    int second;

    if (wrapped < 0) wrapped += s_native_view_canonical_width;
    index = wrapped / 16;
    fraction = wrapped - index * 16;
    first = boundaries[index] - index * 16;
    second = boundaries[index + 1] - (index + 1) * 16;
    return (first * (16 - fraction) + second * fraction + 8) / 16;
}

static void native_view_wave_blit_row(const NativeViewWaveRow *row) {
    const int scale = s_scale;
    const int center_right = s_native_view_offset +
                             s_native_view_canonical_width;

    if (s_native_view_offset > 0) {
        glScissor(0, row->left_top * scale, s_native_view_offset * scale,
                  (row->left_bottom - row->left_top) * scale);
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        for (int source_right = s_native_view_offset;
             source_right > 0; source_right -= 16) {
            const int source_left = source_right > 16
                ? source_right - 16 : 0;
            const int canonical_left = source_left - s_native_view_offset;
            const int canonical_right = source_right - s_native_view_offset;
            int destination_left = source_left +
                native_view_wave_displacement(
                    row->boundaries, canonical_left);
            int destination_right = source_right +
                native_view_wave_displacement(
                    row->boundaries, canonical_right);

            if (source_left == 0) destination_left = 0;
            if (source_right == s_native_view_offset)
                destination_right = s_native_view_offset;
            if (destination_right > destination_left)
                p_glBlitFramebuffer(
                    source_left * scale, row->left_source_top * scale,
                    source_right * scale, row->left_source_bottom * scale,
                    destination_left * scale, row->left_top * scale,
                    destination_right * scale, row->left_bottom * scale,
                    GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }
    }
    if (center_right < s_native_view_width) {
        glScissor(center_right * scale, row->right_top * scale,
                  (s_native_view_width - center_right) * scale,
                  (row->right_bottom - row->right_top) * scale);
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        for (int source_left = center_right;
             source_left < s_native_view_width; source_left += 16) {
            const int source_right = source_left + 16 < s_native_view_width
                ? source_left + 16 : s_native_view_width;
            const int canonical_left = source_left - s_native_view_offset;
            const int canonical_right = source_right - s_native_view_offset;
            int destination_left = source_left +
                native_view_wave_displacement(
                    row->boundaries, canonical_left);
            int destination_right = source_right +
                native_view_wave_displacement(
                    row->boundaries, canonical_right);

            if (source_left == center_right)
                destination_left = center_right;
            if (source_right == s_native_view_width)
                destination_right = s_native_view_width;
            if (destination_right > destination_left)
                p_glBlitFramebuffer(
                    source_left * scale, row->right_source_top * scale,
                    source_right * scale, row->right_source_bottom * scale,
                    destination_left * scale, row->right_top * scale,
                    destination_right * scale, row->right_bottom * scale,
                    GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }
    }
}

static int native_view_wave_apply_variant(
        GLuint target_fbo, unsigned int variant) {
    const int width = s_native_view_width * s_scale;
    const int height = VRAM_H * s_scale;
    const int row_count = variant < NATIVE_VIEW_WAVE_VARIANTS
        ? s_native_view_wave.present_row_count[variant] : 0;

    if (!target_fbo || !s_scratch_fbo ||
        variant >= NATIVE_VIEW_WAVE_VARIANTS ||
        row_count <= 0 || row_count > NATIVE_VIEW_WAVE_ROWS)
        return 0;
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, target_fbo);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, s_scratch_fbo);
    glDisable(GL_SCISSOR_TEST);
    p_glBlitFramebuffer(0, 0, width, height, 0, 0, width, height,
                        GL_COLOR_BUFFER_BIT, GL_NEAREST);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_scratch_fbo);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, target_fbo);
    glEnable(GL_SCISSOR_TEST);
    for (int index = 0; index < row_count; ++index) {
        const NativeViewWaveRow *row =
            &s_native_view_wave.rows[variant][index];

        native_view_wave_blit_row(row);
    }
    glDisable(GL_SCISSOR_TEST);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
    s_stencil_valid = 0;
    return native_midpoint_gl_ok(GL_NATIVE_MIDPOINT_GL_WAVE_COPY);
}

static int native_view_wave_apply_copy(
        int slot, int base_x, int src_x, int src_y,
        int dst_x, int dst_y, int w, int h) {
    int applied = 0;

    if (!s_native_view_wave_authenticated ||
        slot != s_native_view_wave_authenticated_slot ||
        base_x != s_native_view_wave_authenticated_base_x ||
        w != s_native_view_canonical_width ||
        (src_x & (VRAM_W - 1)) != base_x ||
        (dst_x & (VRAM_W - 1)) != base_x || src_y != dst_y + 32 || h != 192 ||
        s_mask_set || s_mask_check)
        return 0;
    s_native_view_wave_diag.matching_copies++;
    s_native_view_wave_diag.last_copy_dst_y = dst_y;
    s_native_view_wave_diag.last_copy_packet_count =
        s_native_view_wave.packet_count;
    s_native_view_wave_diag.last_copy_row_count =
        s_native_view_wave.present_row_count[NATIVE_CURRENT_VARIANT];
    if (s_native_view_wave.recording && s_native_view_wave.ready) {
        s_native_view_wave_diag.ready_copies++;
        applied = native_view_wave_apply_variant(
            s_native_view_fbo[slot], NATIVE_CURRENT_VARIANT);
        if (applied)
            s_native_view_wave_diag.apply_successes++;
        else
            s_native_view_wave_diag.apply_failures++;
        for (unsigned int phase = 0u;
             phase < s_native_interpolation_phase_count; ++phase) {
            if (!native_view_wave_apply_variant(
                    native_view_phase_fbo(slot, phase), phase)) {
                gl_renderer_native_midpoint_cancel();
                break;
            }
        }
    } else {
        s_native_view_wave_diag.partial_copies++;
    }
    if (dst_y == 0 || dst_y == 256) {
        const int page = dst_y / 256;

        if (s_native_view_wave.recording && s_native_view_wave.ready) {
            s_native_view_wave_diag.ready_copies_by_page[page]++;
            s_native_view_wave_diag.wave_valid_by_page[page] = applied != 0;
        } else {
            s_native_view_wave_diag.partial_copies_by_page[page]++;
            s_native_view_wave_diag.wave_valid_by_page[page] = 0;
        }
    }
    s_native_view_wave.recording = 0;
    s_native_view_wave_authenticated = 0;
    return 1;
}

static int native_view_copy_full_width(GLuint target_fbo, int src_y,
                                        int dst_y, int h) {
    const int width = s_native_view_width * s_scale;
    const int scale = s_scale;

    if (!target_fbo || !s_scratch_fbo || h <= 0 ||
        src_y < 0 || dst_y < 0 || src_y + h > VRAM_H || dst_y + h > VRAM_H)
        return 0;
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, target_fbo);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, s_scratch_fbo);
    glDisable(GL_SCISSOR_TEST);
    p_glBlitFramebuffer(
        0, src_y * scale, width, (src_y + h) * scale,
        0, 0, width, h * scale, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_scratch_fbo);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, target_fbo);
    p_glBlitFramebuffer(
        0, 0, width, h * scale,
        0, dst_y * scale, width, (dst_y + h) * scale,
        GL_COLOR_BUFFER_BIT, GL_NEAREST);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
    s_stencil_valid = 0;
    return 1;
}

static void native_view_copy_to_target(GLuint target_fbo, int base_x,
                                         int src_x, int src_y, int dst_x,
                                         int dst_y, int w, int h,
                                         int source_is_target,
                                         GLuint source_fbo) {
    s_native_view_pass = 1;
    s_native_view_pass_fbo = target_fbo;
    s_native_view_pass_base = base_x;
    s_native_view_copy_self = source_is_target;
    s_native_view_copy_source_fbo = source_fbo;
    gpu_copy_rect(src_x, src_y, dst_x, dst_y, w, h);
    s_native_view_copy_source_fbo = 0;
    s_native_view_copy_self = 0;
    s_native_view_pass = 0;
    s_native_view_pass_fbo = 0;
    s_native_view_pass_base = 0;
}

static void glb_native_copy_rect(int src_x, int src_y, int dst_x, int dst_y,
                                  int w, int h) {
    if (s_cpu_auth_dual || !s_raster_ok)
        sw_copy_rect(src_x, src_y, dst_x, dst_y, w, h);
    if (!s_raster_ok) {
        return;
    }
    flush_flat_batch();
    flush_tex_batch();
    flush_cpu_upload();
    if (native_host_pending_flush_reason(5u) !=
        GPU_RENDER_TRANSACTION_OK) {
        gl_renderer_native_midpoint_cancel();
        return;
    }
    if (s_native_view_enabled && w > 0) {
        const int operation_x = dst_x & (VRAM_W - 1);
        const int operation_w = w > VRAM_W ? VRAM_W : w;

        native_view_ensure_destination_surfaces(operation_x, operation_w);
        for (int slot = 0; slot < NATIVE_VIEW_MAX_SURF; ++slot) {
            const int base_x = s_native_view_base[slot];
            int operation_logical_x = 0;
            int wave_applied;

            if (!s_native_view_fbo[slot] || !s_native_view_seeded[slot])
                continue;
            wave_applied = native_view_wave_apply_copy(
                slot, base_x, src_x, src_y, dst_x, dst_y, w, h);
            /* A same-X copy covering the canonical framebuffer is a
             * postprocess step, so replay it across the complete Native view. */
            if (!wave_applied &&
                operation_w == s_native_view_canonical_width &&
                operation_x == base_x &&
                (src_x & (VRAM_W - 1)) == base_x &&
                !s_mask_set && !s_mask_check &&
                native_view_copy_full_width(
                    s_native_view_fbo[slot], src_y, dst_y, h)) {
                if (s_native_midpoint_diag.frame_open &&
                    !s_native_midpoint_current_pending &&
                    s_native_midpoint_diag.frame_valid) {
                    for (unsigned int phase = 0u;
                         phase < s_native_interpolation_phase_count; ++phase)
                        if (!native_view_copy_full_width(
                                native_view_phase_fbo(slot, phase),
                                src_y, dst_y, h)) {
                            gl_renderer_native_midpoint_cancel();
                            break;
                        }
                }
                continue;
            }
            while (operation_logical_x < operation_w) {
                const int destination_x =
                    (operation_x + operation_logical_x) & (VRAM_W - 1);
                const int destination_w = operation_w - operation_logical_x <
                        VRAM_W - destination_x
                    ? operation_w - operation_logical_x
                    : VRAM_W - destination_x;
                int surface_logical_x = 0;

                while (surface_logical_x < s_native_view_canonical_width) {
                    const int surface_x =
                        (base_x + surface_logical_x) & (VRAM_W - 1);
                    const int surface_w = s_native_view_canonical_width -
                                surface_logical_x < VRAM_W - surface_x
                        ? s_native_view_canonical_width - surface_logical_x
                        : VRAM_W - surface_x;
                    const int overlap_x = destination_x > surface_x
                        ? destination_x : surface_x;
                    const int overlap_end = destination_x + destination_w <
                                surface_x + surface_w
                        ? destination_x + destination_w
                        : surface_x + surface_w;

                    if (overlap_x < overlap_end) {
                        const int copy_width = overlap_end - overlap_x;
                        const int copy_src_x =
                            src_x + operation_logical_x + overlap_x -
                                destination_x;
                        const int copy_dst_x =
                            s_native_view_offset + surface_logical_x +
                                overlap_x - surface_x;
                        const int phase_source_distance =
                            (copy_src_x - base_x) & (VRAM_W - 1);
                        const int phase_source_in_view =
                            phase_source_distance + copy_width <=
                                s_native_view_canonical_width;
                        const int phase_src_x = s_native_view_offset +
                            phase_source_distance;
                        native_view_copy_to_target(
                            s_native_view_fbo[slot], base_x,
                            copy_src_x, src_y, copy_dst_x, dst_y,
                            copy_width, h, 0, 0);
                        if (s_native_midpoint_diag.frame_open &&
                            !s_native_midpoint_current_pending &&
                            s_native_midpoint_diag.frame_valid) {
                            for (unsigned int phase = 0u;
                                 phase < s_native_interpolation_phase_count;
                                 ++phase)
                                native_view_copy_to_target(
                                    native_view_phase_fbo(slot, phase), base_x,
                                    phase_source_in_view
                                        ? phase_src_x : copy_src_x,
                                    src_y, copy_dst_x, dst_y,
                                    copy_width, h, phase_source_in_view,
                                    phase_source_in_view
                                        ? 0 : native_phase_fbo(phase));
                            if (!native_midpoint_gl_ok(
                                    GL_NATIVE_MIDPOINT_GL_COPY_VIEW))
                                gl_renderer_native_midpoint_cancel();
                        }
                    }
                    surface_logical_x += surface_w;
                }
                operation_logical_x += destination_w;
            }
        }
        dst_x = operation_x;
        w = operation_w;
    }
    if (native_midpoint_active()) {
        for (unsigned int phase = 0u;
             phase < s_native_interpolation_phase_count; ++phase) {
            s_midpoint_pass_fbo = native_phase_fbo(phase);
            s_midpoint_copy_pass = 1;
            gpu_copy_rect(src_x, src_y, dst_x, dst_y, w, h);
            s_midpoint_copy_pass = 0;
            s_midpoint_pass_fbo = 0;
        }
        if (!native_midpoint_gl_ok(
                GL_NATIVE_MIDPOINT_GL_COPY_CANONICAL))
            gl_renderer_native_midpoint_cancel();
        s_native_midpoint_diag.nonsemantic_copies++;
    }
    gpu_copy_rect(src_x, src_y, dst_x, dst_y, w, h);
}

/* Native presentation wrappers. The frame data still came through the
 * canonical guest MDEC/DMA/VRAM path; these wrappers only select the active
 * OpenGL backend's presentation operation instead of the legacy main.cpp
 * presentation call site. */
static int glb_present_vram(int disp_x, int disp_y, int w, int h, int linear,
                            int force_4_3) {
    return gl_renderer_present_vram(disp_x, disp_y, w, h, linear, force_4_3);
}

static int glb_present_cpu_frame(const uint32_t *pixels, int src_w, int src_h,
                                 int linear, int force_4_3, int content_w) {
    if (!s_ctx || s_transaction) return 0;
    gl_renderer_present(pixels, src_w, src_h, linear, force_4_3, content_w);
    return 1;
}

static int glb_present_native_cpu_frame(const uint32_t *pixels, int src_w,
                                        int src_h, int linear, int force_4_3,
                                        int content_w) {
    return gl_renderer_present_native_cpu_frame(
        pixels, src_w, src_h, linear, force_4_3, content_w);
}

/* Native-wide fast path: authoritatively copy the canonical 4:3 framebuffer
 * into the wide surface's CENTRE columns [g_wide_off, g_wide_off+native_w) for
 * the displayed Y band, so the per-prim mirror could skip every centre-only
 * prim (the dominant native-wide GPU saving). The reveal margins were already
 * produced by the mirror; this leaves them untouched. One FBO->FBO blit,
 * x-translated by the reveal offset. No-op when s_wide_fast is off (then the
 * mirror drew the full surface, as before). Shared by both present paths. */
static void wide_blit_center(GLuint wide_fbo, int base_x, int disp_y, int disp_h) {
    if (!s_wide_fast || g_wide_w <= 0) return;
    int native_w = g_wide_w - 2 * g_wide_off;
    if (native_w <= 0) return;
    int S = s_scale;
    (void)disp_y; (void)disp_h;
    /* Copy the canonical framebuffer column into the wide surface CENTRE over the
     * FULL VRAM height, not just the current display band [disp_y, disp_y+disp_h].
     * The wide surface holds BOTH vertical double-buffer bands (Ape flips
     * display_y 0<->256), and the game's draw area / display band can differ per
     * scene (the cityscape intro exposed rows outside disp_h). Copying the whole
     * column makes the wide CENTRE bit-identical to what the full mirror would
     * have drawn there for every band, so nothing the present reads is ever left
     * black. The margins (x outside the centre) are untouched. */
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_hr_fbo);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, wide_fbo);
    glDisable(GL_SCISSOR_TEST);
    p_glBlitFramebuffer(base_x * S, 0,
                        (base_x + native_w) * S, VRAM_H * S,
                        g_wide_off * S, 0,
                        (g_wide_off + native_w) * S, VRAM_H * S,
                        GL_COLOR_BUFFER_BIT, GL_NEAREST);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
}

/* GPU-direct native-wide present: blit the displayed buffer's wide FBO straight
 * to the window (no glReadPixels / glFinish CPU round-trip). The wide surface is
 * g_wide_w wide × VRAM_H tall (at scale S); present its [0,g_wide_w] × [disp_y,
 * disp_y+disp_h] region into the letterbox, V-flipped like present_vram (FBO y
 * bottom-origin → window top). Returns 0 if there's no wide surface for base_x
 * (caller falls back). disp_x is the displayed buffer base (the wide-surface key). */
int gl_renderer_present_wide_fbo(int disp_x, int disp_y, int disp_h, int linear) {
    if (s_transaction) {
        (void)glb_transaction_reject_other_present();
        return 1;
    }
    if (!s_ctx || !s_raster_ok || g_wide_w <= 0) return 0;
    if (!glb_transaction_prepare_original_present()) return 1;
    gl_maybe_apply_scale();
    GLuint fbo = 0, tex = 0;
    for (int i = 0; i < WIDE_MAX_SURF; i++)
        if (s_wide_fbo[i] && s_wide_base[i] == disp_x) {
            fbo = s_wide_fbo[i]; tex = s_wide_tex[i]; break;
        }
    if (!fbo) return 0;
    flush_flat_batch();
    flush_tex_batch();
    flush_cpu_upload();
    if (s_force_present_remaining <= 0 &&
        s_last_present_path == GL_PRES_WIDE &&
        s_last_dx == disp_x && s_last_dy == disp_y &&
        s_last_dw == g_wide_w && s_last_dh == disp_h &&
        !present_dirty_test(0, disp_y, VRAM_W - 1, disp_y + disp_h - 1)) {
        s_probe_skip++;
        gl_perf_present_enter();
        gl_perf_present_exit(1);
        return 1;
    }
    gl_perf_present_enter();
    int ww = 0, wh = 0; SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    int lx, ly, lw, lh;
    letterbox_rect(ww, wh, &lx, &ly, &lw, &lh);
    wide_blit_center(fbo, disp_x, disp_y, disp_h);   /* fast-path: authoritative centre */

    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, ww, wh);
    if (lx != 0 || ly != 0 || lw != ww || lh != wh) {
        glClearColor(0.f, 0.f, 0.f, 1.f); glClear(GL_COLOR_BUFFER_BIT);
    }
    int interp_pair = s_transaction_force_original ? 0 :
        interp_capture(fbo, 0, disp_y, g_wide_w, disp_h,
                       linear, 0, GL_PRES_WIDE);
    if (interp_pair) {
        hold_capture_native_fbo(fbo, 0, disp_y, g_wide_w, disp_h, 0, linear);
        gl_perf_present_exit(1);
        present_dirty_rect(0, disp_y, VRAM_W - 1, disp_y + disp_h - 1, 0);
        present_force_consumed();
        s_last_present_path = GL_PRES_WIDE;
        s_last_dx = disp_x; s_last_dy = disp_y;
        s_last_dw = g_wide_w; s_last_dh = disp_h;
        return 1;
    }
    present_target_quad(0, tex, g_wide_w, VRAM_H,
                        0, disp_y, g_wide_w, disp_h, linear, lx, ly, lw, lh);
    uint64_t present_sequence = pres_record(
        GL_PRES_WIDE, disp_x, disp_y, g_wide_w, disp_h, lx, ly, lw, lh);
    /* Pre-swap hook on the wide FBO path. present_target_quad bound FBO 0
     * (DRAW) for its own blit earlier; rebind READ here so the readback in
     * the hook targets the default framebuffer's back buffer, not whatever
     * FBO wide_blit_center left bound. */
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    hold_capture_drawable();
    psx_debug_overlay_pre_swap();
    latency_ring_mark(LAT_SWAP_BEGIN);
    SDL_GL_SwapWindow(s_win);
    pres_mark_swap_completed(present_sequence);
    s_probe_swap++;
    latency_ring_mark(LAT_SWAP_END);
    gl_perf_present_exit(1);
    present_dirty_rect(0, disp_y, VRAM_W - 1, disp_y + disp_h - 1, 0);
    present_force_consumed();
    s_last_present_path = GL_PRES_WIDE;
    s_last_dx = disp_x; s_last_dy = disp_y; s_last_dw = g_wide_w; s_last_dh = disp_h;
    coh_record(GL_COH_PRESENT, 0, disp_y, g_wide_w - 1, disp_y + disp_h - 1);
    glb_transaction_original_presented();
    return 1;
}

int gl_renderer_present_native_midpoint(int disp_x, int disp_y, int w, int h,
                                        int linear, int force_4_3) {
    int had_work = 0;
    int use_midpoint;
    int present_pending_current;
    int current_x = disp_x;
    int current_y = disp_y;
    int source_x = disp_x;
    int source_y;
    int ww, wh, lx, ly, lw, lh;
    int last_path = GL_PRES_NATIVE_CURRENT;
    unsigned int next_pending_phase = 0u;
    uint64_t frequency;
    uint64_t period;

    if (s_transaction) {
        (void)glb_transaction_reject_other_present();
        return 0;
    }
    if (!s_ctx || !s_raster_ok || !s_midpoint_tex) return 0;
    if (!glb_transaction_prepare_original_present()) return 0;
    flush_flat_batch();
    flush_tex_batch();
    flush_cpu_upload();
    source_y = native_midpoint_promoted_source_y(disp_y);
    present_pending_current = s_native_midpoint_current_pending;
    if (present_pending_current && !native_midpoint_pending_matches(
            -1, disp_x, disp_y, w, h)) {
        native_midpoint_note_pending_mismatch(-1, disp_x, disp_y, w, h);
        gl_renderer_native_midpoint_reset_for_reason(
            GL_NATIVE_MIDPOINT_RESET_PENDING_CANONICAL_MISMATCH);
        present_pending_current = 0;
    }
    use_midpoint = native_midpoint_prepare_present(&had_work);
    if (use_midpoint && !s_native_midpoint_canonical_enabled) {
        s_native_midpoint_diag.midpoint_candidate_canonical_disabled++;
        use_midpoint = 0;
    }
    if (present_pending_current && use_midpoint) {
        s_native_midpoint_diag.midpoint_candidate_pending_current++;
        use_midpoint = 0;
    }
    if (had_work)
        native_midpoint_current_target(
            disp_x, disp_y, w, h, &current_x, &current_y);
    if (present_pending_current) {
        source_x = s_native_midpoint_pending_x;
        source_y = s_native_midpoint_pending_y;
    } else if (use_midpoint) {
        source_x = current_x;
        source_y = current_y;
    }

    gl_perf_present_enter();
    SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    if (force_4_3)
        letterbox_rect_aspect(ww, wh, 4, 3, &lx, &ly, &lw, &lh);
    else
        letterbox_rect(ww, wh, &lx, &ly, &lw, &lh);
    frequency = SDL_GetPerformanceFrequency();
    period = frequency != 0u
        ? frequency / ((uint64_t)s_native_interpolation_denominator * 30u)
        : 0u;
    if (present_pending_current) {
        unsigned int emitted = 0u;
        const unsigned int subframes =
            s_native_interpolation_denominator / 2u;

        next_pending_phase = s_native_interpolation_phase_count;
        for (unsigned int phase = s_native_midpoint_pending_phase;
             phase < s_native_interpolation_phase_count; ++phase) {
            native_present_wait_next(period, frequency);
            native_present_swap_texture(
                native_phase_tex(phase), VRAM_W, VRAM_H,
                native_phase_fbo(phase), native_phase_fbo(phase),
                source_x, source_y, w, h,
                disp_x, disp_y, w, h, linear, ww, wh, lx, ly, lw, lh,
                GL_PRES_NATIVE_MIDPOINT, phase + 1u,
                s_native_interpolation_denominator,
                s_pending_canonical_geometry_hash[phase],
                s_pending_canonical_geometry_count[phase] != 0u, 1);
            last_path = GL_PRES_NATIVE_MIDPOINT;
            ++emitted;
        }
        while (emitted < subframes) {
            native_present_wait_next(period, frequency);
            native_present_swap_texture(
                s_midpoint_tex, VRAM_W, VRAM_H,
                s_midpoint_fbo, s_midpoint_fbo,
                source_x, source_y, w, h,
                disp_x, disp_y, w, h, linear, ww, wh, lx, ly, lw, lh,
                GL_PRES_NATIVE_CURRENT, 0u, 0u,
                s_pending_canonical_geometry_hash
                    [NATIVE_CURRENT_VARIANT],
                s_pending_canonical_geometry_count
                    [NATIVE_CURRENT_VARIANT] != 0u,
                emitted + 1u == subframes);
            last_path = GL_PRES_NATIVE_CURRENT;
            ++emitted;
        }
    } else if (use_midpoint) {
        const unsigned int first_half =
            s_native_interpolation_denominator / 2u;

        for (unsigned int phase = 0u; phase < first_half; ++phase) {
            native_present_wait_next(period, frequency);
            native_present_swap_texture(
                native_phase_tex(phase), VRAM_W, VRAM_H,
                native_phase_fbo(phase), native_phase_fbo(phase),
                source_x, source_y, w, h,
                disp_x, disp_y, w, h, linear, ww, wh, lx, ly, lw, lh,
                GL_PRES_NATIVE_MIDPOINT, phase + 1u,
                s_native_interpolation_denominator,
                s_canonical_geometry_hash[phase],
                s_canonical_geometry_count[phase] != 0u, 1);
        }
        last_path = GL_PRES_NATIVE_MIDPOINT;
        next_pending_phase = first_half;
    } else {
        const unsigned int subframes = s_native_midpoint_diag.suspended
            ? 1u : s_native_interpolation_denominator / 2u;

        for (unsigned int subframe = 0u; subframe < subframes; ++subframe) {
            native_present_wait_next(period, frequency);
            native_present_swap_texture(
                s_hr_tex, VRAM_W, VRAM_H, s_hr_fbo, s_hr_fbo,
                source_x, source_y, w, h,
                disp_x, disp_y, w, h, linear, ww, wh, lx, ly, lw, lh,
                GL_PRES_NATIVE_CURRENT, 0u, 0u,
                s_canonical_geometry_hash[NATIVE_CURRENT_VARIANT],
                s_canonical_geometry_count[NATIVE_CURRENT_VARIANT] != 0u,
                subframe == 0u);
        }
    }
    gl_perf_present_exit(0);
    present_force_consumed();
    s_last_present_path = last_path;
    s_last_dx = disp_x; s_last_dy = disp_y; s_last_dw = w; s_last_dh = h;
    coh_record(GL_COH_PRESENT, disp_x, disp_y, disp_x + w - 1,
               disp_y + h - 1);
    glb_transaction_original_presented();
    native_midpoint_finish_present(
        use_midpoint, present_pending_current, had_work,
        had_work ? current_x : disp_x, had_work ? current_y : disp_y,
        disp_y, w, h, 0, next_pending_phase);
    /* A live scale transition must happen after the completed old-scale frame;
     * source geometry was already rasterized before this VBlank callback. */
    gl_maybe_apply_scale();
    native_midpoint_begin_after_present();
    return 1;
}

int gl_renderer_present_native_view(int disp_x, int disp_y, int disp_h,
                                    int linear) {
    int slot;
    int source_slot;
    int had_work = 0;
    int use_midpoint;
    int present_pending_current;
    int current_x = disp_x;
    int current_y = disp_y;
    int source_y;
    int ww, wh, lx, ly, lw, lh;
    int last_path = GL_PRES_NATIVE_CURRENT;
    unsigned int next_pending_phase = 0u;
    uint64_t frequency;
    uint64_t period;

    if (s_transaction) {
        (void)glb_transaction_reject_other_present();
        return 1;
    }
    if (!s_ctx || !s_raster_ok || !s_native_view_enabled) return 0;
    if (!glb_transaction_prepare_original_present()) return 1;
    slot = native_view_prepare_surface(disp_x);
    if (slot < 0) {
        gl_renderer_native_midpoint_cancel();
        return 0;
    }
    s_native_view_wave_diag.presents++;
    s_native_view_wave_diag.last_present_y = disp_y;
    if (disp_y == 0 || disp_y == 256) {
        const int page = disp_y / 256;

        s_native_view_wave_diag.presents_by_page[page]++;
        if (s_native_view_wave_diag.wave_valid_by_page[page])
            s_native_view_wave_diag.presents_with_wave_by_page[page]++;
    }
    flush_flat_batch();
    flush_tex_batch();
    flush_cpu_upload();
    source_y = native_midpoint_promoted_source_y(disp_y);
    present_pending_current = s_native_midpoint_current_pending;
    if (present_pending_current && !native_midpoint_pending_matches(
            slot, disp_x, disp_y, s_native_view_width, disp_h)) {
        native_midpoint_note_pending_mismatch(
            slot, disp_x, disp_y, s_native_view_width, disp_h);
        gl_renderer_native_midpoint_reset_for_reason(
            GL_NATIVE_MIDPOINT_RESET_PENDING_VIEW_MISMATCH);
        present_pending_current = 0;
        slot = native_view_prepare_surface(disp_x);
        if (slot < 0) return 0;
    }
    use_midpoint = native_midpoint_prepare_present(&had_work);
    if (present_pending_current && use_midpoint) {
        s_native_midpoint_diag.midpoint_candidate_pending_current++;
        use_midpoint = 0;
    }
    if (had_work)
        native_midpoint_current_target(
            disp_x, disp_y, s_native_view_canonical_width, disp_h,
            &current_x, &current_y);
    source_slot = slot;
    if (use_midpoint) {
        source_slot = native_view_prepare_surface(current_x);
        use_midpoint = source_slot >= 0 &&
            s_native_midpoint_seeded[source_slot];
        if (!use_midpoint)
            s_native_midpoint_diag.midpoint_candidate_view_unseeded++;
        if (use_midpoint) source_y = current_y;
    } else if (present_pending_current) {
        source_y = s_native_midpoint_pending_y;
    }
    if (!present_pending_current &&
        native_host_queue_prepare_present(use_midpoint) !=
            GPU_RENDER_TRANSACTION_OK) {
        gl_renderer_native_midpoint_cancel();
        return 0;
    }
    gl_perf_present_enter();
    ww = wh = 0;
    SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    letterbox_rect(ww, wh, &lx, &ly, &lw, &lh);
    frequency = SDL_GetPerformanceFrequency();
    period = frequency != 0u
        ? frequency / ((uint64_t)s_native_interpolation_denominator * 30u)
        : 0u;
    if (present_pending_current) {
        unsigned int emitted = 0u;
        const unsigned int subframes =
            s_native_interpolation_denominator / 2u;

        next_pending_phase = s_native_interpolation_phase_count;
        for (unsigned int phase = s_native_midpoint_pending_phase;
             phase < s_native_interpolation_phase_count; ++phase) {
            native_present_wait_next(period, frequency);
            native_present_swap_texture(
                native_view_phase_tex(source_slot, phase),
                s_native_view_width, VRAM_H,
                native_view_phase_fbo(source_slot, phase),
                native_view_phase_fbo(source_slot, phase),
                0, source_y, s_native_view_width, disp_h,
                disp_x, disp_y, s_native_view_canonical_width, disp_h, linear,
                ww, wh, lx, ly, lw, lh, GL_PRES_NATIVE_MIDPOINT,
                phase + 1u, s_native_interpolation_denominator,
                s_pending_native_view_geometry_hash[phase],
                s_pending_native_view_geometry_count[phase] != 0u, 1);
            last_path = GL_PRES_NATIVE_MIDPOINT;
            ++emitted;
        }
        while (emitted < subframes) {
            native_present_wait_next(period, frequency);
            native_present_swap_texture(
                s_native_midpoint_tex[slot], s_native_view_width, VRAM_H,
                s_native_midpoint_fbo[slot], s_native_midpoint_fbo[slot],
                0, source_y, s_native_view_width, disp_h,
                disp_x, disp_y, s_native_view_canonical_width, disp_h, linear,
                ww, wh, lx, ly, lw, lh, GL_PRES_NATIVE_CURRENT, 0u, 0u,
                s_pending_native_view_geometry_hash
                    [NATIVE_CURRENT_VARIANT],
                s_pending_native_view_geometry_count
                    [NATIVE_CURRENT_VARIANT] != 0u,
                emitted + 1u == subframes);
            last_path = GL_PRES_NATIVE_CURRENT;
            ++emitted;
        }
    } else if (use_midpoint) {
        const unsigned int first_half =
            s_native_interpolation_denominator / 2u;

        for (unsigned int phase = 0u; phase < first_half; ++phase) {
            native_present_wait_next(period, frequency);
            native_present_swap_texture(
                native_view_phase_tex(source_slot, phase),
                s_native_view_width, VRAM_H,
                native_view_phase_fbo(source_slot, phase),
                native_view_phase_fbo(source_slot, phase),
                0, source_y, s_native_view_width, disp_h,
                disp_x, disp_y, s_native_view_canonical_width, disp_h, linear,
                ww, wh, lx, ly, lw, lh, GL_PRES_NATIVE_MIDPOINT,
                phase + 1u, s_native_interpolation_denominator,
                s_native_view_geometry_hash[phase],
                s_native_view_geometry_count[phase] != 0u, 1);
        }
        last_path = GL_PRES_NATIVE_MIDPOINT;
        next_pending_phase = first_half;
    } else {
        const unsigned int subframes = s_native_midpoint_diag.suspended
            ? 1u : s_native_interpolation_denominator / 2u;

        for (unsigned int subframe = 0u; subframe < subframes; ++subframe) {
            native_present_wait_next(period, frequency);
            native_present_swap_texture(
                s_native_view_tex[slot], s_native_view_width, VRAM_H,
                s_native_view_fbo[slot], s_native_view_fbo[slot],
                0, source_y, s_native_view_width, disp_h,
                disp_x, disp_y, s_native_view_canonical_width, disp_h, linear,
                ww, wh, lx, ly, lw, lh, GL_PRES_NATIVE_CURRENT, 0u, 0u,
                s_native_view_geometry_hash[NATIVE_CURRENT_VARIANT],
                s_native_view_geometry_count[NATIVE_CURRENT_VARIANT] != 0u,
                subframe == 0u);
        }
    }
    gl_perf_present_exit(1);
    present_force_consumed();
    s_last_present_path = last_path;
    s_last_dx = disp_x;
    s_last_dy = disp_y;
    s_last_dw = s_native_view_width;
    s_last_dh = disp_h;
    coh_record(GL_COH_PRESENT, 0, disp_y, s_native_view_width - 1,
               disp_y + disp_h - 1);
    glb_transaction_original_presented();
    native_midpoint_finish_present(
        use_midpoint, present_pending_current, had_work,
        had_work ? current_x : disp_x, had_work ? current_y : disp_y,
        disp_y, s_native_view_width, disp_h, 1,
        next_pending_phase);
    /* Preserve the completed Native surface for this swap, then rebuild before
     * collecting the next source frame at the requested scale. */
    gl_maybe_apply_scale();
    native_midpoint_begin_after_present();
    return 1;
}

static int native_view_fbo_peek(GLuint fbo, int x, int y,
                                int w, int h, uint16_t *out) {
    const int physical_w = w * s_scale;
    const int physical_h = h * s_scale;
    uint32_t *pixels;

    if (!fbo || !out || x < 0 || y < 0 || w <= 0 || h <= 0 ||
        x + w > s_native_view_width || y + h > VRAM_H)
        return 0;
    flush_flat_batch();
    flush_tex_batch();
    pixels = (uint32_t *)malloc(
        (size_t)physical_w * physical_h * sizeof(*pixels));
    if (!pixels) return 0;
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, fbo);
    glReadPixels(x * s_scale, y * s_scale, physical_w, physical_h,
                 GL_BGRA, GL_UNSIGNED_BYTE, pixels);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    for (int row = 0; row < h; ++row) {
        for (int column = 0; column < w; ++column) {
            const uint32_t pixel = pixels[
                (size_t)(row * s_scale) * physical_w + column * s_scale];
            const uint32_t red = (pixel >> 16u) & 0xffu;
            const uint32_t green = (pixel >> 8u) & 0xffu;
            const uint32_t blue = pixel & 0xffu;

            out[row * w + column] = (uint16_t)(
                (red >> 3u) | ((green >> 3u) << 5u) |
                ((blue >> 3u) << 10u) | ((pixel >> 16u) & 0x8000u));
        }
    }
    free(pixels);
    return 1;
}

int gl_renderer_native_view_peek(int base_x, int x, int y,
                                  int w, int h, uint16_t *out) {
    const int slot = native_view_surface_slot(base_x, 0);

    if (slot < 0 || native_host_pending_flush_reason(7u) !=
            GPU_RENDER_TRANSACTION_OK)
        return 0;
    return native_view_fbo_peek(s_native_view_fbo[slot], x, y, w, h, out);
}

int gl_renderer_native_view_center_diff(
        uint32_t *count, int bbox[4], int samples[8][2],
        uint16_t samples_px[8][2]) {
    const int width = s_native_view_canonical_width;
    const int height = s_native_view_canonical_height;
    const int slot = native_view_surface_slot(s_last_dx, 0);
    uint16_t *canonical;
    uint16_t *native;
    uint32_t mismatch_count = 0u;
    int min_x = width, min_y = height, max_x = -1, max_y = -1;
    int sample_count = 0;

    if (!count || !bbox || !samples || !samples_px || slot < 0 ||
        !s_native_view_enabled || width <= 0 || height <= 0)
        return 0;
    canonical = (uint16_t *)malloc(
        (size_t)width * height * sizeof(*canonical));
    native = (uint16_t *)malloc((size_t)width * height * sizeof(*native));
    if (!canonical || !native) {
        free(canonical);
        free(native);
        return 0;
    }
    if (!gl_renderer_fbo_peek(
            s_last_dx, s_last_dy, width, height, canonical) ||
        !native_view_fbo_peek(
            s_native_view_fbo[slot], s_native_view_offset, s_last_dy,
            width, height, native)) {
        free(canonical);
        free(native);
        return 0;
    }
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t index = (size_t)y * width + x;

            if (canonical[index] == native[index]) continue;
            ++mismatch_count;
            if (x < min_x) min_x = x;
            if (x > max_x) max_x = x;
            if (y < min_y) min_y = y;
            if (y > max_y) max_y = y;
            if (sample_count < 8) {
                samples[sample_count][0] = x;
                samples[sample_count][1] = y;
                samples_px[sample_count][0] = canonical[index];
                samples_px[sample_count][1] = native[index];
                ++sample_count;
            }
        }
    }
    free(canonical);
    free(native);
    *count = mismatch_count;
    bbox[0] = mismatch_count ? min_x : 0;
    bbox[1] = mismatch_count ? min_y : 0;
    bbox[2] = mismatch_count ? max_x : 0;
    bbox[3] = mismatch_count ? max_y : 0;
    return sample_count + 1;
}

int gl_renderer_native_view_phase_peek(int base_x, unsigned int phase,
                                        int x, int y, int w, int h,
                                        uint16_t *out) {
    const int slot = native_view_surface_slot(base_x, 0);

    if (slot < 0 || phase >= s_native_interpolation_phase_count) return 0;
    return native_view_fbo_peek(
        native_view_phase_fbo(slot, phase), x, y, w, h, out);
}

static uint64_t canonical_digest_byte(uint64_t hash, uint8_t value) {
    return (hash ^ value) * UINT64_C(1099511628211);
}

static uint64_t canonical_digest_u32(uint64_t hash, uint32_t value) {
    for (unsigned int byte = 0u; byte < 4u; ++byte)
        hash = canonical_digest_byte(hash, (uint8_t)(value >> (byte * 8u)));
    return hash;
}

static int glb_canonical_framebuffer_digest(int display_x, int display_y,
                                            int display_width,
                                            int display_height,
                                            uint64_t *out_digest) {
    GLuint source_fbo = s_hr_fbo;
    size_t width;
    size_t height;
    size_t read_x;
    size_t read_y;
    size_t byte_count;
    uint64_t hash = UINT64_C(1469598103934665603);

    if (!out_digest || !glb_transaction_context_ready() || !source_fbo ||
        display_x < 0 || display_y < 0 || display_width <= 0 ||
        display_height <= 0 || display_x > VRAM_W - display_width ||
        display_y > VRAM_H - display_height || s_scale <= 0)
        return 0;
    read_x = (size_t)display_x * (size_t)s_scale;
    read_y = (size_t)display_y * (size_t)s_scale;
    width = (size_t)display_width * (size_t)s_scale;
    height = (size_t)display_height * (size_t)s_scale;
    if (width > SIZE_MAX / height / 4u) return 0;
    if (s_transaction && s_transaction->deferred_candidate_token !=
            GPU_RENDER_DEFERRED_CANDIDATE_NONE) {
        if (s_deferred_candidate.token !=
                s_transaction->deferred_candidate_token ||
            !s_deferred_candidate.framebuffer)
            return 0;
        source_fbo = s_deferred_candidate.framebuffer;
    } else {
        flush_flat_batch();
        flush_tex_batch();
        flush_cpu_upload();
    }
    byte_count = width * height * 4u;
    if (byte_count > s_canonical_digest_capacity) {
        uint8_t *pixels = (uint8_t *)realloc(
            s_canonical_digest_pixels, byte_count);
        if (!pixels) return 0;
        s_canonical_digest_pixels = pixels;
        s_canonical_digest_capacity = byte_count;
    }

    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, source_fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels((GLint)read_x, (GLint)read_y,
                 (GLsizei)width, (GLsizei)height,
                 GL_RGBA, GL_UNSIGNED_BYTE, s_canonical_digest_pixels);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    if (!glb_transaction_context_ready()) return 0;

    /* GL's bottom row is the canonical PS1 top scanline in this FBO. Alpha is
     * the hidden PS1 mask bit, so normalize it to opaque host RGBA8. */
    hash = canonical_digest_u32(hash, UINT32_C(0x58475246));
    hash = canonical_digest_u32(hash, (uint32_t)width);
    hash = canonical_digest_u32(hash, (uint32_t)height);
    hash = canonical_digest_u32(hash, UINT32_C(0x52474241));
    for (size_t pixel = 0u; pixel < width * height; ++pixel) {
        const uint8_t *rgba = &s_canonical_digest_pixels[pixel * 4u];
        hash = canonical_digest_byte(hash, rgba[0]);
        hash = canonical_digest_byte(hash, rgba[1]);
        hash = canonical_digest_byte(hash, rgba[2]);
        hash = canonical_digest_byte(hash, UINT8_C(0xff));
    }
    *out_digest = hash;
    return 1;
}

/* ---- atomic semantic-render transaction -------------------------------- */

static int glb_transaction_id_equal(GpuRenderTransactionId left,
                                    GpuRenderTransactionId right) {
    return left.scene_epoch == right.scene_epoch &&
           left.state_sequence == right.state_sequence;
}

static int glb_transaction_context_ready(void) {
    return s_ctx != NULL && s_win != NULL && s_raster_ok && s_vram != NULL &&
           SDL_GL_GetCurrentContext() == s_ctx &&
           SDL_GL_GetCurrentWindow() == s_win;
}

static int glb_transaction_prepare_original_present(void) {
    if (!s_transaction_force_original) return 1;
    if (!glb_transaction_context_ready()) return 0;
    glb_transaction_cleanup_deferred_staging();
    return 1;
}

static void glb_transaction_original_presented(void) {
    if (!s_transaction_force_original) return;
    s_transaction_force_original = 0;
#ifdef PSX_GL_TRANSACTION_TESTING
    s_transaction_test_diag.forced_original_presents++;
#endif
}

static int glb_transaction_interpolation_quiesced(void) {
    int quiesced;

    if (s_interp_mutex) SDL_LockMutex(s_interp_mutex);
    quiesced = !s_interp_enabled && s_interp_valid == 0;
    if (s_interp_mutex) SDL_UnlockMutex(s_interp_mutex);
    return quiesced;
}

static int glb_transaction_consume_gl_errors(void) {
    int saw_error = 0;

    while (glGetError() != GL_NO_ERROR) saw_error = 1;
    return saw_error;
}

static GpuRenderTransactionStatus glb_transaction_drain(void) {
    if (!glb_transaction_context_ready())
        return GPU_RENDER_TRANSACTION_CONTEXT_LOST;
    if (native_host_pending_flush_reason(8u) !=
        GPU_RENDER_TRANSACTION_OK)
        return GPU_RENDER_TRANSACTION_BACKEND_ERROR;
    flush_flat_batch();
    flush_tex_batch();
    flush_cpu_upload();
    pack_flush();
    glFlush();
    return glb_transaction_consume_gl_errors()
        ? GPU_RENDER_TRANSACTION_BACKEND_ERROR
        : GPU_RENDER_TRANSACTION_OK;
}

static int glb_fixed_floor(GpuRenderFixed16_16 value) {
    const int64_t wide = value;

    return wide >= 0
        ? (int)(wide / INT64_C(65536))
        : -(int)((-wide + INT64_C(65535)) / INT64_C(65536));
}

/* A Native fallback or UI producer can submit a PS1 transition/filter as an
 * axis-aligned 320x224/240 quad. Flat, Gouraud, and textured filters all cover
 * the complete display; the canonical pass remains unchanged while the Native
 * pass maps only the outer horizontal edges to the revealed columns. */
static int glb_native_view_fullscreen_quad(
        const GpuRenderSemantic *semantic) {
    int min_x = INT_MAX, max_x = INT_MIN;
    int min_y = INT_MAX, max_y = INT_MIN;
    int fullscreen_height;
    unsigned corner_mask = 0u;
    GpuDisplayInfo display = {0};

    if (!semantic || semantic->topology != GPU_RENDER_SEMANTIC_TRIANGLES ||
        semantic->triangle_count != 2u || semantic->line_count != 0u)
        return 0;
    for (uint8_t triangle_index = 0u;
         triangle_index < semantic->triangle_count; ++triangle_index) {
        const GpuRenderSemanticTriangle *triangle =
            &semantic->triangles[triangle_index];
        for (uint8_t vertex_index = 0u; vertex_index < 3u; ++vertex_index) {
            const GpuRenderSemanticVertex *vertex =
                &triangle->vertices[vertex_index];
            int x;
            int y;

            if (vertex->native_view_position) return 0;
            x = vertex->x / INT32_C(65536) + semantic->material.draw_offset_x -
                s_native_view_pass_base;
            y = vertex->y / INT32_C(65536) + semantic->material.draw_offset_y;
            if (x < min_x) min_x = x;
            if (x > max_x) max_x = x;
            if (y < min_y) min_y = y;
            if (y > max_y) max_y = y;
        }
    }
    fullscreen_height = s_native_view_canonical_height;
    gpu_get_display_info(&display);
    if (display.height > 0u &&
        display.height < (uint32_t)fullscreen_height)
        fullscreen_height = (int)display.height;
    /* PS1 games commonly draw their full-screen filters in the 224-line
     * active band while leaving the CRTC in its 240-line mode. Both 224 and
     * 240 are complete display bands, so do not center a 320x224 transition
     * merely because the current CRTC range reports 240 lines. */
    if (fullscreen_height > 224)
        fullscreen_height = 224;
    if (min_x > 0 || max_x < s_native_view_canonical_width ||
        min_y > 0 || max_y < fullscreen_height)
        return 0;

    /* Bounds alone also match unrelated two-triangle geometry. Require every
     * submitted vertex to be one of the four rectangle corners and require all
     * four corners to occur across the split. */
    for (uint8_t triangle_index = 0u;
         triangle_index < semantic->triangle_count; ++triangle_index) {
        const GpuRenderSemanticTriangle *triangle =
            &semantic->triangles[triangle_index];
        for (uint8_t vertex_index = 0u; vertex_index < 3u; ++vertex_index) {
            const GpuRenderSemanticVertex *vertex =
                &triangle->vertices[vertex_index];
            const int x = vertex->x / INT32_C(65536) +
                semantic->material.draw_offset_x - s_native_view_pass_base;
            const int y = vertex->y / INT32_C(65536) +
                semantic->material.draw_offset_y;
            unsigned corner;

            if ((x != min_x && x != max_x) || (y != min_y && y != max_y))
                return 0;
            corner = (x == max_x ? 1u : 0u) | (y == max_y ? 2u : 0u);
            corner_mask |= 1u << corner;
        }
    }
    return corner_mask == 0x0fu;
}

/* Native semantic equivalent of ws_nw_backdrop_stretch_quad(). The legacy
 * path sees GP0 vertices directly; Native receives two triangles, so classify
 * their combined bounds before mapping the horizontal edges to the wide view. */
static int glb_native_view_backdrop_quad(const GpuRenderSemantic *semantic) {
    enum { EDGE = 24 };
    int min_x = INT_MAX, max_x = INT_MIN;
    int min_y = INT_MAX, max_y = INT_MIN;

    if (!semantic || semantic->topology != GPU_RENDER_SEMANTIC_TRIANGLES ||
        semantic->triangle_count != 2u || semantic->line_count != 0u)
        return 0;
    for (uint8_t triangle_index = 0u;
         triangle_index < semantic->triangle_count; ++triangle_index) {
        const GpuRenderSemanticTriangle *triangle =
            &semantic->triangles[triangle_index];

        for (uint8_t vertex_index = 0u; vertex_index < 3u; ++vertex_index) {
            const GpuRenderSemanticVertex *vertex =
                &triangle->vertices[vertex_index];
            const int x = vertex->x / INT32_C(65536) +
                semantic->material.draw_offset_x - s_native_view_pass_base;
            const int y = vertex->y / INT32_C(65536) +
                semantic->material.draw_offset_y;

            if (vertex->native_view_position) return 0;
            if (x < min_x) min_x = x;
            if (x > max_x) max_x = x;
            if (y < min_y) min_y = y;
            if (y > max_y) max_y = y;
        }
    }
    if (min_x > EDGE || max_x < s_native_view_canonical_width - EDGE ||
        max_y - min_y < 64)
        return 0;
    for (uint8_t triangle_index = 0u;
         triangle_index < semantic->triangle_count; ++triangle_index) {
        const GpuRenderSemanticTriangle *triangle =
            &semantic->triangles[triangle_index];

        for (uint8_t vertex_index = 0u; vertex_index < 3u; ++vertex_index) {
            const GpuRenderSemanticVertex *vertex =
                &triangle->vertices[vertex_index];
            const int x = vertex->x / INT32_C(65536) +
                semantic->material.draw_offset_x - s_native_view_pass_base;
            const int y = vertex->y / INT32_C(65536) +
                semantic->material.draw_offset_y;

            if (!((x - min_x <= EDGE || max_x - x <= EDGE) &&
                  (y - min_y <= EDGE || max_y - y <= EDGE)))
                return 0;
        }
    }
    return 1;
}

static int glb_native_view_semantic_target_base(
        const GpuRenderSemantic *semantic) {
    const GpuRenderMaterial *material;
    int minimum_x = INT_MAX;
    int maximum_x = INT_MIN;

    if (!semantic || semantic->topology != GPU_RENDER_SEMANTIC_TRIANGLES ||
        semantic->triangle_count == 0u)
        return 0;
    material = &semantic->material;
    for (uint8_t triangle_index = 0u;
         triangle_index < semantic->triangle_count; ++triangle_index) {
        const GpuRenderSemanticTriangle *triangle =
            &semantic->triangles[triangle_index];

        for (uint8_t vertex_index = 0u; vertex_index < 3u; ++vertex_index) {
            const int x = triangle->vertices[vertex_index].x /
                              INT32_C(65536) + material->draw_offset_x;

            if (x < minimum_x) minimum_x = x;
            if (x > maximum_x) maximum_x = x;
        }
    }
    if (minimum_x < material->draw_area_left)
        minimum_x = material->draw_area_left;
    if (maximum_x > material->draw_area_right)
        maximum_x = material->draw_area_right;
    if (minimum_x > maximum_x)
        minimum_x = material->draw_area_left;
    return ((minimum_x / s_native_view_canonical_width) *
            s_native_view_canonical_width) & (VRAM_W - 1);
}

static int glb_native_view_semantic_reaches_reveal(
        const GpuRenderSemantic *semantic) {
    if (!semantic || semantic->topology != GPU_RENDER_SEMANTIC_TRIANGLES)
        return 0;
    for (uint8_t triangle_index = 0u;
         triangle_index < semantic->triangle_count; ++triangle_index) {
        const GpuRenderSemanticTriangle *triangle =
            &semantic->triangles[triangle_index];
        for (uint8_t vertex_index = 0u; vertex_index < 3u; ++vertex_index) {
            const GpuRenderSemanticVertex *vertex =
                &triangle->vertices[vertex_index];
            const int x = (vertex->native_view_position
                               ? vertex->native_view_x : vertex->x) /
                    INT32_C(65536) + semantic->material.draw_offset_x -
                s_native_view_pass_base;

            if (x < 0 || x >= s_native_view_canonical_width) return 1;
        }
    }
    return 0;
}

static int glb_native_view_overlay_x(int canonical_x, int fullscreen_overlay) {
    const int local_x = canonical_x - s_native_view_pass_base;
    int mapped_x;

    if (!fullscreen_overlay) {
        mapped_x = local_x + s_native_view_offset;
        if (local_x == s_native_view_canonical_width +
                           s_native_view_offset - 1)
            return s_native_view_width;
        return mapped_x;
    }
    if (local_x <= 0) return local_x;
    if (local_x >= s_native_view_canonical_width)
        return s_native_view_width;
    return local_x + s_native_view_offset;
}

static int glb_native_view_screen_2d_x(int canonical_x) {
    const int local_x = canonical_x - s_native_view_pass_base;
    const int64_t scaled = (int64_t)local_x * s_native_view_width;

    return scaled >= 0
        ? (int)(scaled / s_native_view_canonical_width)
        : -(int)((-scaled + s_native_view_canonical_width - 1) /
                 s_native_view_canonical_width);
}

static int glb_native_view_preserve_2d_translation_x(
        const GpuRenderSemantic *semantic) {
    int min_x = INT_MAX;
    int max_x = INT_MIN;

    for (uint8_t triangle = 0u;
         triangle < semantic->triangle_count; ++triangle)
        for (uint8_t vertex = 0u; vertex < 3u; ++vertex) {
            const int x = semantic->triangles[triangle].vertices[vertex].x /
                              INT32_C(65536) +
                          semantic->material.draw_offset_x;
            if (x < min_x) min_x = x;
            if (x > max_x) max_x = x;
        }
    if (min_x > max_x) return 0;
    {
        const int center_x = min_x + (max_x - min_x) / 2;
        const int canonical_center_x = s_native_view_pass_base +
                                       s_native_view_canonical_width / 2;
        const int anchor_x = center_x < canonical_center_x ? min_x
                           : center_x > canonical_center_x ? max_x
                                                          : center_x;
        return glb_native_view_screen_2d_x(anchor_x) - anchor_x;
    }
}

static void glb_transaction_snapshot(GlTransactionCheckpoint *checkpoint) {
    memcpy(checkpoint->vram, s_vram, sizeof(checkpoint->vram));

    checkpoint->off_x = s_off_x; checkpoint->off_y = s_off_y;
    checkpoint->area_x1 = s_area_x1; checkpoint->area_y1 = s_area_y1;
    checkpoint->area_x2 = s_area_x2; checkpoint->area_y2 = s_area_y2;
    checkpoint->semi_en = s_semi_en; checkpoint->semi_mode = s_semi_mode;
    checkpoint->mod_r = s_mod_r; checkpoint->mod_g = s_mod_g;
    checkpoint->mod_b = s_mod_b; checkpoint->mod_raw = s_mod_raw;
    checkpoint->dither = s_dither;
    checkpoint->mask_set = s_mask_set; checkpoint->mask_check = s_mask_check;
    checkpoint->tw_mask_x = s_tw_mask_x; checkpoint->tw_mask_y = s_tw_mask_y;
    checkpoint->tw_off_x = s_tw_off_x; checkpoint->tw_off_y = s_tw_off_y;
    checkpoint->tex_filter = s_tex_filter;
    checkpoint->stencil_valid = s_stencil_valid;

    checkpoint->gpu_dirty = s_gpu_dirty;
    checkpoint->pack_dirty = s_pack_dirty;
    checkpoint->up_nrects = s_up_nrects;
    memcpy(checkpoint->up_rects, s_up_rects, sizeof(checkpoint->up_rects));
    checkpoint->depth24_skip_up = s_depth24_skip_up;
    checkpoint->d24_skip_fb = s_d24_skip_fb;

    memcpy(checkpoint->present_dirty, s_present_dirty,
           sizeof(checkpoint->present_dirty));
    checkpoint->last_present_path = s_last_present_path;
    checkpoint->last_dx = s_last_dx; checkpoint->last_dy = s_last_dy;
    checkpoint->last_dw = s_last_dw; checkpoint->last_dh = s_last_dh;
    checkpoint->force_present_remaining = s_force_present_remaining;
    checkpoint->probe_skip = s_probe_skip;
    checkpoint->probe_swap = s_probe_swap;
    checkpoint->probe_dirty_marks = s_probe_dirty_marks;

    checkpoint->coh_seq = s_coh_seq;
    memcpy(checkpoint->rt_up_diag, s_rt_up_diag,
           sizeof(checkpoint->rt_up_diag));
    checkpoint->scene_prims = s_scene_prims;
    checkpoint->scene_prims_tex = s_scene_prims_tex;
    checkpoint->batch_total = s_batch_total;
    memcpy(checkpoint->batch_reason, s_batch_reason,
           sizeof(checkpoint->batch_reason));
    checkpoint->bd_gate = s_bd_gate; checkpoint->tb_gate = s_tb_gate;
    checkpoint->wide_suppress = s_wide_suppress;
    checkpoint->bdg_applied = s_bdg_applied;
    checkpoint->bdg_prims = s_bdg_prims;
    checkpoint->bdg_clearx = s_bdg_clearx;
    memcpy(checkpoint->ptrace, s_ptrace, sizeof(checkpoint->ptrace));
    checkpoint->ptrace_n = s_ptrace_n;
    checkpoint->tb_semi = s_tb_semi; checkpoint->tb_mask = s_tb_mask;
    checkpoint->tb_filter = s_tb_filter; checkpoint->tb_dither = s_tb_dither;
    memcpy(checkpoint->tb_twin, s_tb_twin, sizeof(checkpoint->tb_twin));
    checkpoint->fb_semi = s_fb_semi; checkpoint->fb_mask = s_fb_mask;
    checkpoint->fb_dither = s_fb_dither;
    checkpoint->cw_flush_ms = s_cw_flush_ms;
    checkpoint->cw_wide_ms = s_cw_wide_ms;
    checkpoint->cw_batches = s_cw_batches;
    checkpoint->cw_wide_sets = s_cw_wide_sets;
    checkpoint->cw_wide_cfgs = s_cw_wide_cfgs;
    checkpoint->cw_wide_clears = s_cw_wide_clears;
    checkpoint->cw_fbo_creates = s_cw_fbo_creates;
    checkpoint->cw_flush_depth = s_cw_flush_depth;
}

static void glb_transaction_restore_draw_state(
        const GlTransactionCheckpoint *checkpoint, int ensure_stencil) {
    if (ensure_stencil && checkpoint->mask_check && !s_stencil_valid)
        rebuild_mask_stencils();

    s_off_x = checkpoint->off_x; s_off_y = checkpoint->off_y;
    s_area_x1 = checkpoint->area_x1; s_area_y1 = checkpoint->area_y1;
    s_area_x2 = checkpoint->area_x2; s_area_y2 = checkpoint->area_y2;
    s_semi_en = checkpoint->semi_en; s_semi_mode = checkpoint->semi_mode;
    s_mod_r = checkpoint->mod_r; s_mod_g = checkpoint->mod_g;
    s_mod_b = checkpoint->mod_b; s_mod_raw = checkpoint->mod_raw;
    s_dither = checkpoint->dither;
    s_mask_set = checkpoint->mask_set; s_mask_check = checkpoint->mask_check;
    s_tw_mask_x = checkpoint->tw_mask_x;
    s_tw_mask_y = checkpoint->tw_mask_y;
    s_tw_off_x = checkpoint->tw_off_x;
    s_tw_off_y = checkpoint->tw_off_y;
    s_tex_filter = checkpoint->tex_filter;

    sw_set_draw_area(checkpoint->area_x1, checkpoint->area_y1,
                     checkpoint->area_x2, checkpoint->area_y2);
    sw_set_draw_offset(checkpoint->off_x, checkpoint->off_y);
    sw_set_semi_transparency(checkpoint->semi_en, checkpoint->semi_mode);
    sw_set_mask_bits(checkpoint->mask_set, checkpoint->mask_check);
    sw_set_texture_window((uint32_t)checkpoint->tw_mask_x |
                          ((uint32_t)checkpoint->tw_mask_y << 5) |
                          ((uint32_t)checkpoint->tw_off_x << 10) |
                          ((uint32_t)checkpoint->tw_off_y << 15));
    sw_set_color_modulation(checkpoint->mod_r, checkpoint->mod_g,
                            checkpoint->mod_b, checkpoint->mod_raw);
    sw_set_texture_filter(checkpoint->tex_filter);
}

static void glb_transaction_restore_snapshot_state(
        const GlTransactionCheckpoint *checkpoint) {
    glb_transaction_restore_draw_state(checkpoint, 0);
    s_stencil_valid = checkpoint->stencil_valid;

    s_gpu_dirty = checkpoint->gpu_dirty;
    s_pack_dirty = checkpoint->pack_dirty;
    s_up_nrects = checkpoint->up_nrects;
    memcpy(s_up_rects, checkpoint->up_rects, sizeof(checkpoint->up_rects));
    s_depth24_skip_up = checkpoint->depth24_skip_up;
    s_d24_skip_fb = checkpoint->d24_skip_fb;

    memcpy(s_present_dirty, checkpoint->present_dirty,
           sizeof(checkpoint->present_dirty));
    s_last_present_path = checkpoint->last_present_path;
    s_last_dx = checkpoint->last_dx; s_last_dy = checkpoint->last_dy;
    s_last_dw = checkpoint->last_dw; s_last_dh = checkpoint->last_dh;
    s_force_present_remaining = checkpoint->force_present_remaining;
    s_probe_skip = checkpoint->probe_skip;
    s_probe_swap = checkpoint->probe_swap;
    s_probe_dirty_marks = checkpoint->probe_dirty_marks;

    s_coh_seq = checkpoint->coh_seq;
    memcpy(s_rt_up_diag, checkpoint->rt_up_diag,
           sizeof(checkpoint->rt_up_diag));
    s_scene_prims = checkpoint->scene_prims;
    s_scene_prims_tex = checkpoint->scene_prims_tex;
    s_batch_total = checkpoint->batch_total;
    memcpy(s_batch_reason, checkpoint->batch_reason,
           sizeof(checkpoint->batch_reason));
    s_bd_gate = checkpoint->bd_gate; s_tb_gate = checkpoint->tb_gate;
    s_wide_suppress = checkpoint->wide_suppress;
    s_bdg_applied = checkpoint->bdg_applied;
    s_bdg_prims = checkpoint->bdg_prims;
    s_bdg_clearx = checkpoint->bdg_clearx;
    memcpy(s_ptrace, checkpoint->ptrace, sizeof(checkpoint->ptrace));
    s_ptrace_n = checkpoint->ptrace_n;
    s_tb_n = 0; s_tb_semi = checkpoint->tb_semi;
    s_tb_mask = checkpoint->tb_mask; s_tb_filter = checkpoint->tb_filter;
    s_tb_dither = checkpoint->tb_dither;
    memcpy(s_tb_twin, checkpoint->tb_twin, sizeof(checkpoint->tb_twin));
    s_fb_n = 0; s_fb_semi = checkpoint->fb_semi;
    s_fb_mask = checkpoint->fb_mask; s_fb_dither = checkpoint->fb_dither;
    s_cw_flush_ms = checkpoint->cw_flush_ms;
    s_cw_wide_ms = checkpoint->cw_wide_ms;
    s_cw_batches = checkpoint->cw_batches;
    s_cw_wide_sets = checkpoint->cw_wide_sets;
    s_cw_wide_cfgs = checkpoint->cw_wide_cfgs;
    s_cw_wide_clears = checkpoint->cw_wide_clears;
    s_cw_fbo_creates = checkpoint->cw_fbo_creates;
    s_cw_flush_depth = checkpoint->cw_flush_depth;
}

static void glb_transaction_discard_checkpoint(void) {
    const GpuRenderDeferredCandidateToken candidate_token = s_transaction
        ? s_transaction->deferred_candidate_token
        : GPU_RENDER_DEFERRED_CANDIDATE_NONE;

    if (s_transaction && s_ctx && SDL_GL_GetCurrentContext() == s_ctx) {
        glb_transaction_cleanup_deferred_staging();
        if (s_transaction->staging_fbo && p_glDeleteFramebuffers)
            p_glDeleteFramebuffers(1, &s_transaction->staging_fbo);
        if (s_transaction->staging_tex)
            glDeleteTextures(1, &s_transaction->staging_tex);
    } else if (s_transaction) {
        s_transaction_deferred_staging_fbo = s_transaction->staging_fbo;
        s_transaction_deferred_staging_tex = s_transaction->staging_tex;
    }
    free(s_transaction);
    s_transaction = NULL;
    if (candidate_token != GPU_RENDER_DEFERRED_CANDIDATE_NONE &&
        s_deferred_candidate.token == candidate_token)
        glb_deferred_candidate_discard_owned();
}

/* READY proves that the renderer still owns the current context. The explicit
 * swap API preserves that ownership contract and must not query it after SDL's
 * void SwapWindow call merely to decide how to release private staging. */
static void glb_transaction_discard_checkpoint_owned(void) {
    GlTransactionCheckpoint *checkpoint = s_transaction;
    GpuRenderDeferredCandidateToken candidate_token;

    if (!checkpoint) return;
    candidate_token = checkpoint->deferred_candidate_token;
    if (checkpoint->staging_fbo && p_glDeleteFramebuffers)
        p_glDeleteFramebuffers(1, &checkpoint->staging_fbo);
    if (checkpoint->staging_tex)
        glDeleteTextures(1, &checkpoint->staging_tex);
    free(checkpoint);
    s_transaction = NULL;
    if (candidate_token != GPU_RENDER_DEFERRED_CANDIDATE_NONE &&
        s_deferred_candidate.token == candidate_token)
        glb_deferred_candidate_discard_owned();
}

static void glb_transaction_cleanup_deferred_staging(void) {
    if (!s_ctx || SDL_GL_GetCurrentContext() != s_ctx) return;
    if (s_transaction_deferred_staging_fbo && p_glDeleteFramebuffers)
        p_glDeleteFramebuffers(1, &s_transaction_deferred_staging_fbo);
    if (s_transaction_deferred_staging_tex)
        glDeleteTextures(1, &s_transaction_deferred_staging_tex);
    s_transaction_deferred_staging_fbo = 0;
    s_transaction_deferred_staging_tex = 0;
}

static GpuRenderTransactionStatus glb_validate_semantic(
        const GpuRenderSemantic *semantic) {
    const GpuRenderMaterial *material;
    uint16_t encoded_depth;
    const int midpoint_pass = s_midpoint_pass_fbo != 0;

    if (!semantic) return GPU_RENDER_TRANSACTION_INVALID_ARGUMENT;
    if (semantic->screen_space_2d > GPU_RENDER_SCREEN_SPACE_2D_PRESERVE_SIZE ||
        semantic->native_view_effect >
            GPU_RENDER_NATIVE_VIEW_EFFECT_WAVE_GRID ||
        (semantic->native_view_effect == GPU_RENDER_NATIVE_VIEW_EFFECT_NONE &&
         semantic->native_view_effect_index != 0u) ||
        (semantic->native_view_effect ==
             GPU_RENDER_NATIVE_VIEW_EFFECT_WAVE_GRID &&
         semantic->native_view_effect_index >= NATIVE_VIEW_WAVE_PACKET_COUNT))
        return GPU_RENDER_TRANSACTION_VALIDATION_FAILED;
    material = &semantic->material;
    switch (material->texture_depth) {
    case GPU_RENDER_TEXTURE_4_BIT:
    case GPU_RENDER_TEXTURE_8_BIT:
    case GPU_RENDER_TEXTURE_15_BIT:
        break;
    default:
        return GPU_RENDER_TRANSACTION_UNSUPPORTED;
    }
    switch (material->blend_mode) {
    case GPU_RENDER_BLEND_AVERAGE:
    case GPU_RENDER_BLEND_ADD:
    case GPU_RENDER_BLEND_SUBTRACT:
    case GPU_RENDER_BLEND_ADD_QUARTER:
        break;
    default:
        return GPU_RENDER_TRANSACTION_UNSUPPORTED;
    }
    switch (material->shading) {
    case GPU_RENDER_SHADING_FLAT:
    case GPU_RENDER_SHADING_GOURAUD:
        break;
    default:
        return GPU_RENDER_TRANSACTION_UNSUPPORTED;
    }

    encoded_depth = (uint16_t)((material->tpage >> 7) & 3u);
    if (material->tpage > UINT16_C(0x01ff) || encoded_depth == 3u ||
        material->texture_page_x != (material->tpage & UINT16_C(0x000f)) ||
        material->texture_page_y != ((material->tpage >> 4) & 1u) ||
        material->blend_mode !=
            (GpuRenderBlendMode)((material->tpage >> 5) & 3u) ||
        material->texture_depth != (GpuRenderTextureDepth)encoded_depth ||
        material->clut_x > 1023u || (material->clut_x & 15u) != 0u ||
        material->clut_y > 511u ||
        material->draw_area_left > material->draw_area_right ||
        material->draw_area_top > material->draw_area_bottom ||
        material->draw_area_right > 1023u ||
        material->draw_area_bottom > 1023u ||
        material->draw_offset_x < -1024 || material->draw_offset_x > 1023 ||
        material->draw_offset_y < -1024 || material->draw_offset_y > 1023 ||
        material->texture_window_mask_x > 31u ||
        material->texture_window_mask_y > 31u ||
        material->texture_window_offset_x > 31u ||
        material->texture_window_offset_y > 31u ||
        material->textured > 1u || material->raw_texture > 1u ||
        material->semi_transparent > 1u || material->dither > 1u ||
        material->mask_set > 1u ||
        material->mask_check > 1u ||
        (material->raw_texture && !material->textured))
        return GPU_RENDER_TRANSACTION_VALIDATION_FAILED;

    if (semantic->topology == GPU_RENDER_SEMANTIC_LINES) {
        if (semantic->screen_space_2d || material->textured ||
            material->raw_texture ||
            semantic->triangle_count != 0u || semantic->line_count == 0u ||
            semantic->line_count > GPU_RENDER_SEMANTIC_LINE_CAPACITY)
            return GPU_RENDER_TRANSACTION_VALIDATION_FAILED;
        for (uint8_t line_index = 0u;
             line_index < semantic->line_count; line_index++) {
            for (int vertex_index = 0; vertex_index < 2; vertex_index++) {
                const GpuRenderSemanticVertex *vertex =
                    &semantic->lines[line_index].vertices[vertex_index];

                /* Only midpoint passes may interpolate positions between PSX
                 * pixels. Texture coordinates remain integer PSX texels. */
                if ((!midpoint_pass &&
                     (((uint32_t)vertex->x & UINT32_C(0xffff)) != 0u ||
                      ((uint32_t)vertex->y & UINT32_C(0xffff)) != 0u)) ||
                    ((uint32_t)vertex->u & UINT32_C(0xffff)) != 0u ||
                    ((uint32_t)vertex->v & UINT32_C(0xffff)) != 0u)
                    return GPU_RENDER_TRANSACTION_UNSUPPORTED;
                if (vertex->native_view_position > 1u ||
                    (!vertex->native_view_position &&
                     (vertex->native_view_x != 0 || vertex->native_view_y != 0)))
                    return GPU_RENDER_TRANSACTION_VALIDATION_FAILED;
                if (material->shading == GPU_RENDER_SHADING_FLAT &&
                    vertex_index != 0 &&
                    (vertex->r != semantic->lines[line_index].vertices[0].r ||
                     vertex->g != semantic->lines[line_index].vertices[0].g ||
                     vertex->b != semantic->lines[line_index].vertices[0].b))
                    return GPU_RENDER_TRANSACTION_VALIDATION_FAILED;
            }
        }
        return GPU_RENDER_TRANSACTION_OK;
    }
    if (semantic->topology != GPU_RENDER_SEMANTIC_TRIANGLES ||
        semantic->line_count != 0u || semantic->triangle_count == 0u ||
        semantic->triangle_count > GPU_RENDER_SEMANTIC_TRIANGLE_CAPACITY)
        return GPU_RENDER_TRANSACTION_VALIDATION_FAILED;

    for (uint8_t triangle_index = 0;
         triangle_index < semantic->triangle_count; triangle_index++) {
        const GpuRenderSemanticTriangle *triangle =
            &semantic->triangles[triangle_index];

        if (triangle->split_index != triangle_index ||
            triangle->split_count != semantic->triangle_count)
            return GPU_RENDER_TRANSACTION_VALIDATION_FAILED;
        for (int vertex_index = 0; vertex_index < 3; vertex_index++) {
            const GpuRenderSemanticVertex *vertex =
                &triangle->vertices[vertex_index];

            /* Midpoint passes retain 16.16 positions, including half-pixels.
             * GP0 texture coordinates must still address whole texels. */
            if ((!midpoint_pass &&
                 (((uint32_t)vertex->x & UINT32_C(0xffff)) != 0u ||
                  ((uint32_t)vertex->y & UINT32_C(0xffff)) != 0u)) ||
                ((uint32_t)vertex->u & UINT32_C(0xffff)) != 0u ||
                ((uint32_t)vertex->v & UINT32_C(0xffff)) != 0u)
                return GPU_RENDER_TRANSACTION_UNSUPPORTED;
            if (vertex->native_view_position > 1u ||
                (semantic->screen_space_2d && vertex->native_view_position) ||
                (!vertex->native_view_position &&
                 (vertex->native_view_x != 0 || vertex->native_view_y != 0)))
                return GPU_RENDER_TRANSACTION_VALIDATION_FAILED;
            if (material->shading == GPU_RENDER_SHADING_FLAT &&
                vertex_index != 0 &&
                (vertex->r != triangle->vertices[0].r ||
                 vertex->g != triangle->vertices[0].g ||
                 vertex->b != triangle->vertices[0].b))
                return GPU_RENDER_TRANSACTION_VALIDATION_FAILED;
        }
    }
    return GPU_RENDER_TRANSACTION_OK;
}

static uint32_t glb_semantic_rgb888(const GpuRenderSemanticVertex *vertex) {
    return (uint32_t)vertex->r | ((uint32_t)vertex->g << 8) |
           ((uint32_t)vertex->b << 16);
}

static uint16_t glb_semantic_rgb555(const GpuRenderSemanticVertex *vertex) {
    return (uint16_t)((vertex->r >> 3u) | ((vertex->g >> 3u) << 5u) |
                      ((vertex->b >> 3u) << 10u));
}

static GpuRenderTransactionStatus glb_transaction_begin(
        GpuRenderTransactionId transaction_id,
        uint64_t vram_mutation_serial) {
    GlTransactionCheckpoint *checkpoint;
    GpuRenderTransactionStatus status;

    if (s_transaction) return GPU_RENDER_TRANSACTION_INVALID_TRANSITION;
    if (s_transaction_force_original)
        return GPU_RENDER_TRANSACTION_STATE_REJECTED;
    if (!glb_transaction_context_ready())
        return GPU_RENDER_TRANSACTION_CONTEXT_LOST;
    if (!glb_transaction_interpolation_quiesced())
        return GPU_RENDER_TRANSACTION_STATE_REJECTED;
    if (s_scale_apply_pending) {
        if (s_scale != s_req_scale)
            return GPU_RENDER_TRANSACTION_STATE_REJECTED;
        s_scale_apply_pending = 0;
    }
    if (gpu_display_is_depth24() || s_depth24_skip_up)
        return GPU_RENDER_TRANSACTION_UNSUPPORTED;
    if (g_wide_w != 0 || g_wide_cur != 0)
        return GPU_RENDER_TRANSACTION_UNSUPPORTED;
    if (glb_transaction_consume_gl_errors())
        return GPU_RENDER_TRANSACTION_BACKEND_ERROR;

    checkpoint = (GlTransactionCheckpoint *)malloc(sizeof(*checkpoint));
    if (!checkpoint) return GPU_RENDER_TRANSACTION_BACKEND_ERROR;
    memset(checkpoint, 0, sizeof(*checkpoint));

    status = glb_transaction_drain();
    if (status != GPU_RENDER_TRANSACTION_OK) {
        free(checkpoint);
        return status;
    }
    ensure_cpu();
    glFinish();
    if (glb_transaction_consume_gl_errors() || s_fb_n != 0 || s_tb_n != 0 ||
        s_up_nrects != 0 || s_pack_dirty.set || s_gpu_dirty) {
        free(checkpoint);
        return GPU_RENDER_TRANSACTION_BACKEND_ERROR;
    }

    checkpoint->id = transaction_id;
    checkpoint->vram_serial = vram_mutation_serial;
    glb_transaction_snapshot(checkpoint);
    s_transaction = checkpoint;
    return GPU_RENDER_TRANSACTION_OK;
}

static void glb_deferred_candidate_discard_owned(void) {
    if (s_deferred_candidate.framebuffer && p_glDeleteFramebuffers &&
        s_ctx && SDL_GL_GetCurrentContext() == s_ctx)
        p_glDeleteFramebuffers(1, &s_deferred_candidate.framebuffer);
    if (s_deferred_candidate.texture && s_ctx &&
        SDL_GL_GetCurrentContext() == s_ctx)
        glDeleteTextures(1, &s_deferred_candidate.texture);
#ifdef PSX_GL_TRANSACTION_TESTING
    if (s_deferred_candidate.token != GPU_RENDER_DEFERRED_CANDIDATE_NONE)
        s_transaction_test_diag.deferred_candidate_discards++;
#endif
    memset(&s_deferred_candidate, 0, sizeof(s_deferred_candidate));
}

static GpuRenderTransactionStatus glb_deferred_candidate_capture(
        GpuRenderTransactionId transaction_id,
        GpuRenderDeferredCandidateToken *out_candidate_token) {
    GpuRenderTransactionStatus status;
    GLuint texture = 0;
    GLuint framebuffer = 0;
    GLsync fence;
    int width;
    int height;
    int context_ok;
    int saw_error;

    if (!out_candidate_token) return GPU_RENDER_TRANSACTION_INVALID_ARGUMENT;
    *out_candidate_token = GPU_RENDER_DEFERRED_CANDIDATE_NONE;
    if (!s_transaction || s_transaction->committed)
        return GPU_RENDER_TRANSACTION_INVALID_TRANSITION;
    if (!glb_transaction_id_equal(s_transaction->id, transaction_id))
        return GPU_RENDER_TRANSACTION_STATE_REJECTED;
    if (s_deferred_candidate.token != GPU_RENDER_DEFERRED_CANDIDATE_NONE)
        return GPU_RENDER_TRANSACTION_INVALID_TRANSITION;
    if (!glb_transaction_context_ready())
        return GPU_RENDER_TRANSACTION_CONTEXT_LOST;
    if (!glb_transaction_interpolation_quiesced() || s_scale_apply_pending ||
        gpu_display_is_depth24() || s_depth24_skip_up ||
        g_wide_w != 0 || g_wide_cur != 0)
        return GPU_RENDER_TRANSACTION_STATE_REJECTED;

    status = glb_transaction_drain();
    if (status != GPU_RENDER_TRANSACTION_OK) return status;
    width = VRAM_W * s_scale;
    height = VRAM_H * s_scale;
    texture = make_tex(GL_RGBA8, width, height,
                       GL_RGBA, GL_UNSIGNED_BYTE);
    if (!texture || !make_fbo(&framebuffer, texture, 0)) {
        if (framebuffer && p_glDeleteFramebuffers)
            p_glDeleteFramebuffers(1, &framebuffer);
        if (texture) glDeleteTextures(1, &texture);
        return GPU_RENDER_TRANSACTION_BACKEND_ERROR;
    }

    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_hr_fbo);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, framebuffer);
    p_glBlitFramebuffer(0, 0, width, height, 0, 0, width, height,
                        GL_COLOR_BUFFER_BIT, GL_NEAREST);
    fence = p_glFenceSync(PSXGL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    glFinish();
    if (fence) p_glDeleteSync(fence);
    context_ok = glb_transaction_context_ready();
    saw_error = context_ok ? glb_transaction_consume_gl_errors() : 0;
    if (!context_ok || !fence || saw_error) {
        if (framebuffer && p_glDeleteFramebuffers)
            p_glDeleteFramebuffers(1, &framebuffer);
        if (texture) glDeleteTextures(1, &texture);
        return context_ok ? GPU_RENDER_TRANSACTION_BACKEND_ERROR
                          : GPU_RENDER_TRANSACTION_CONTEXT_LOST;
    }

    if (s_deferred_candidate_next_token ==
        GPU_RENDER_DEFERRED_CANDIDATE_NONE)
        s_deferred_candidate_next_token = 1u;
    s_deferred_candidate.token = s_deferred_candidate_next_token++;
    s_deferred_candidate.visual_id = transaction_id;
    s_deferred_candidate.texture = texture;
    s_deferred_candidate.framebuffer = framebuffer;
    s_deferred_candidate.width = width;
    s_deferred_candidate.height = height;
    s_deferred_candidate.scale = s_scale;
    *out_candidate_token = s_deferred_candidate.token;
#ifdef PSX_GL_TRANSACTION_TESTING
    s_transaction_test_diag.deferred_candidate_captures++;
#endif
    return GPU_RENDER_TRANSACTION_OK;
}

static GpuRenderTransactionStatus glb_deferred_candidate_discard(
        GpuRenderDeferredCandidateToken candidate_token) {
    if (candidate_token == GPU_RENDER_DEFERRED_CANDIDATE_NONE)
        return GPU_RENDER_TRANSACTION_INVALID_ARGUMENT;
    if (s_deferred_candidate.token != candidate_token)
        return GPU_RENDER_TRANSACTION_STATE_REJECTED;
    glb_deferred_candidate_discard_owned();
    return GPU_RENDER_TRANSACTION_OK;
}

static GpuRenderTransactionStatus glb_deferred_transaction_begin(
        GpuRenderTransactionId transaction_id,
        uint64_t vram_mutation_serial,
        GpuRenderDeferredCandidateToken candidate_token) {
    GpuRenderTransactionStatus status;

    if (candidate_token == GPU_RENDER_DEFERRED_CANDIDATE_NONE ||
        s_deferred_candidate.token != candidate_token ||
        !glb_transaction_id_equal(s_deferred_candidate.visual_id,
                                  transaction_id) ||
        s_deferred_candidate.scale != s_scale ||
        s_deferred_candidate.width != VRAM_W * s_scale ||
        s_deferred_candidate.height != VRAM_H * s_scale)
        return GPU_RENDER_TRANSACTION_STATE_REJECTED;
    status = glb_transaction_begin(transaction_id, vram_mutation_serial);
    if (status != GPU_RENDER_TRANSACTION_OK) return status;
    s_transaction->deferred_candidate_token = candidate_token;
#ifdef PSX_GL_TRANSACTION_TESTING
    s_transaction_test_diag.deferred_transaction_begins++;
#endif
    return GPU_RENDER_TRANSACTION_OK;
}

static GpuRenderTransactionStatus glb_ordering_barrier(
        GpuRenderTransactionId transaction_id) {
    if (!s_transaction) return GPU_RENDER_TRANSACTION_INVALID_TRANSITION;
    if (!glb_transaction_id_equal(s_transaction->id, transaction_id))
        return GPU_RENDER_TRANSACTION_STATE_REJECTED;
    if (!glb_transaction_context_ready())
        return GPU_RENDER_TRANSACTION_CONTEXT_LOST;
    if (!glb_transaction_interpolation_quiesced())
        return GPU_RENDER_TRANSACTION_STATE_REJECTED;
    return glb_transaction_drain();
}

static GpuRenderTransactionStatus glb_draw_semantic_contents(
        const GpuRenderSemantic *semantic, int immediate, int native_view) {
    GpuRenderTransactionStatus status;
    const GpuRenderMaterial *material;
    const int fullscreen_overlay = native_view &&
        (glb_native_view_fullscreen_quad(semantic) ||
         (gpu_ws_nw_backdrop_enabled() &&
          glb_native_view_backdrop_quad(semantic)));
    const int screen_space_2d_mode = native_view
        ? semantic->screen_space_2d : GPU_RENDER_SCREEN_SPACE_2D_NONE;
    /* Interpolate semantics in 16.16, then quantize every Native phase through
     * the same integer PS1 raster. Mixing fractional and integer primitive
     * families opens otherwise shared edges at midpoint boundaries. */
    const int subpixel_pass = 0;
    uint32_t texture_window;

    status = glb_validate_semantic(semantic);
    if (status != GPU_RENDER_TRANSACTION_OK) return status;
    material = &semantic->material;
    texture_window = (uint32_t)material->texture_window_mask_x |
                     ((uint32_t)material->texture_window_mask_y << 5) |
                     ((uint32_t)material->texture_window_offset_x << 10) |
                     ((uint32_t)material->texture_window_offset_y << 15);

    glb_set_draw_area(material->draw_area_left, material->draw_area_top,
                      material->draw_area_right, material->draw_area_bottom);
    glb_set_draw_offset(material->draw_offset_x, material->draw_offset_y);
    glb_set_texture_window(texture_window);
    glb_set_mask_bits(material->mask_set, material->mask_check);
    glb_set_semi_transparency(material->semi_transparent,
                              material->blend_mode);
    glb_set_dither(material->dither);
    if (semantic->topology == GPU_RENDER_SEMANTIC_LINES) {
        for (uint8_t line_index = 0u;
             line_index < semantic->line_count; line_index++) {
            const GpuRenderSemanticLine *line = &semantic->lines[line_index];
            const GpuRenderSemanticVertex *first = &line->vertices[0];
            const GpuRenderSemanticVertex *second = &line->vertices[1];
            const int x0 = native_view && first->native_view_position
                ? glb_fixed_floor(first->native_view_x) +
                      material->draw_offset_x - s_native_view_pass_base
                : glb_fixed_floor(first->x) + material->draw_offset_x +
                      (native_view
                           ? s_native_view_offset - s_native_view_pass_base
                           : 0);
            const int y0 = glb_fixed_floor(
                               native_view && first->native_view_position
                                   ? first->native_view_y : first->y) +
                           material->draw_offset_y;
            const int x1 = native_view && second->native_view_position
                ? glb_fixed_floor(second->native_view_x) +
                      material->draw_offset_x - s_native_view_pass_base
                : glb_fixed_floor(second->x) + material->draw_offset_x +
                      (native_view
                           ? s_native_view_offset - s_native_view_pass_base
                           : 0);
            const int y1 = glb_fixed_floor(
                               native_view && second->native_view_position
                                   ? second->native_view_y : second->y) +
                           material->draw_offset_y;

            if (subpixel_pass) {
                const float x[2] = {
                    (native_view && first->native_view_position
                         ? (float)first->native_view_x
                         : (float)first->x) / 65536.0f +
                        material->draw_offset_x +
                        (native_view && !first->native_view_position
                             ? s_native_view_offset - s_native_view_pass_base
                             : native_view ? -s_native_view_pass_base : 0),
                    (native_view && second->native_view_position
                         ? (float)second->native_view_x
                         : (float)second->x) / 65536.0f +
                        material->draw_offset_x +
                        (native_view && !second->native_view_position
                             ? s_native_view_offset - s_native_view_pass_base
                             : native_view ? -s_native_view_pass_base : 0),
                };
                const float y[2] = {
                    (native_view && first->native_view_position
                         ? (float)first->native_view_y
                         : (float)first->y) / 65536.0f +
                        material->draw_offset_y,
                    (native_view && second->native_view_position
                         ? (float)second->native_view_y
                         : (float)second->y) / 65536.0f +
                        material->draw_offset_y,
                };
                const uint32_t colors[2] = {
                    glb_semantic_rgb888(first),
                    material->shading == GPU_RENDER_SHADING_GOURAUD
                        ? glb_semantic_rgb888(second)
                        : glb_semantic_rgb888(first),
                };

                gpu_line_subpixel(x, y, colors,
                    material->semi_transparent ? material->blend_mode : -1,
                    material->dither);
            } else if (material->shading == GPU_RENDER_SHADING_GOURAUD) {
                glb_draw_shaded_line(
                    x0, y0, glb_semantic_rgb555(&line->vertices[0]),
                    x1, y1, glb_semantic_rgb555(&line->vertices[1]));
            } else {
                glb_draw_line(x0, y0, x1, y1,
                              glb_semantic_rgb555(&line->vertices[0]));
            }
            if (!immediate) {
                status = glb_transaction_drain();
                if (status != GPU_RENDER_TRANSACTION_OK) return status;
            }
        }
        return GPU_RENDER_TRANSACTION_OK;
    }

    glb_set_color_modulation(semantic->triangles[0].vertices[0].r,
                             semantic->triangles[0].vertices[0].g,
                             semantic->triangles[0].vertices[0].b,
                             material->raw_texture);

    for (uint8_t triangle_index = 0;
         triangle_index < semantic->triangle_count; triangle_index++) {
        const GpuRenderSemanticTriangle *triangle =
            &semantic->triangles[triangle_index];
        int x[3], y[3], u[3], v[3];
        float subpixel_x[3], subpixel_y[3];
        uint32_t color24[3];

        for (int vertex_index = 0; vertex_index < 3; vertex_index++) {
            const GpuRenderSemanticVertex *vertex =
                &triangle->vertices[vertex_index];

            if (native_view && vertex->native_view_position) {
                x[vertex_index] = glb_fixed_floor(vertex->native_view_x) +
                                  material->draw_offset_x -
                                  s_native_view_pass_base;
                if (x[vertex_index] == s_native_view_width - 1)
                    x[vertex_index] = s_native_view_width;
                y[vertex_index] = glb_fixed_floor(vertex->native_view_y) +
                                  material->draw_offset_y;
            } else {
                const int canonical_x = glb_fixed_floor(vertex->x) +
                                         material->draw_offset_x;
                x[vertex_index] = native_view
                    ? (screen_space_2d_mode ==
                               GPU_RENDER_SCREEN_SPACE_2D_STRETCH
                           ? glb_native_view_screen_2d_x(canonical_x)
                           : screen_space_2d_mode ==
                                     GPU_RENDER_SCREEN_SPACE_2D_PRESERVE_SIZE
                               ? canonical_x +
                                     s_native_view_preserve_2d_translation_x
                               : glb_native_view_overlay_x(
                                     canonical_x, fullscreen_overlay))
                    : canonical_x;
                y[vertex_index] = glb_fixed_floor(vertex->y) +
                                  material->draw_offset_y;
            }
            u[vertex_index] = vertex->u / INT32_C(65536);
            v[vertex_index] = vertex->v / INT32_C(65536);
            color24[vertex_index] = glb_semantic_rgb888(vertex);
            subpixel_x[vertex_index] = (float)x[vertex_index];
            subpixel_y[vertex_index] = (float)y[vertex_index];
            if (subpixel_pass) {
                const float canonical_x = (float)vertex->x / 65536.0f +
                    material->draw_offset_x;

                if (native_view && vertex->native_view_position) {
                    subpixel_x[vertex_index] =
                        (float)vertex->native_view_x / 65536.0f +
                        material->draw_offset_x - s_native_view_pass_base;
                    subpixel_y[vertex_index] =
                        (float)vertex->native_view_y / 65536.0f +
                        material->draw_offset_y;
                    if (subpixel_x[vertex_index] ==
                        (float)s_native_view_width - 1.0f)
                        subpixel_x[vertex_index] = (float)s_native_view_width;
                } else if (!native_view) {
                    subpixel_x[vertex_index] = canonical_x;
                    subpixel_y[vertex_index] =
                        (float)vertex->y / 65536.0f +
                        material->draw_offset_y;
                } else if (screen_space_2d_mode ==
                           GPU_RENDER_SCREEN_SPACE_2D_STRETCH) {
                    subpixel_x[vertex_index] =
                        (canonical_x - s_native_view_pass_base) *
                        (float)s_native_view_width /
                        (float)s_native_view_canonical_width;
                } else if (screen_space_2d_mode ==
                           GPU_RENDER_SCREEN_SPACE_2D_PRESERVE_SIZE) {
                    subpixel_x[vertex_index] = canonical_x +
                        s_native_view_preserve_2d_translation_x;
                } else {
                    const float local_x = canonical_x -
                        s_native_view_pass_base;
                    if (fullscreen_overlay && local_x <= 0.0f)
                        subpixel_x[vertex_index] = local_x;
                    else if (fullscreen_overlay &&
                             local_x >= s_native_view_canonical_width)
                        subpixel_x[vertex_index] =
                            (float)s_native_view_width;
                    else
                        subpixel_x[vertex_index] = local_x +
                            s_native_view_offset;
                }
            }
        }

        if (subpixel_pass && material->textured) {
            float colors[9];
            for (int vertex_index = 0; vertex_index < 3; ++vertex_index) {
                const uint32_t color = color24[vertex_index];
                colors[vertex_index * 3 + 0] = (color & 0xffu) / 255.0f;
                colors[vertex_index * 3 + 1] =
                    ((color >> 8u) & 0xffu) / 255.0f;
                colors[vertex_index * 3 + 2] =
                    ((color >> 16u) & 0xffu) / 255.0f;
            }
            if (material->shading != GPU_RENDER_SHADING_GOURAUD) {
                for (int vertex_index = 1; vertex_index < 3; ++vertex_index) {
                    colors[vertex_index * 3 + 0] = colors[0];
                    colors[vertex_index * 3 + 1] = colors[1];
                    colors[vertex_index * 3 + 2] = colors[2];
                }
            }
            gpu_textured_triangle(
                x, y, u, v, colors, material->tpage, material->clut_x,
                material->clut_y, material->raw_texture,
                material->semi_transparent ? material->blend_mode : -1,
                material->dither && !material->raw_texture, NULL,
                subpixel_x, subpixel_y);
        } else if (material->textured) {
            if (material->shading == GPU_RENDER_SHADING_GOURAUD) {
                glb_draw_shaded_textured_triangle(
                    x[0], y[0], u[0], v[0], color24[0],
                    x[1], y[1], u[1], v[1], color24[1],
                    x[2], y[2], u[2], v[2], color24[2],
                    material->clut_x, material->clut_y, material->tpage,
                    material->raw_texture);
            } else {
                glb_set_color_modulation(triangle->vertices[0].r,
                                         triangle->vertices[0].g,
                                         triangle->vertices[0].b,
                                         material->raw_texture);
                glb_draw_textured_triangle(
                    x[0], y[0], u[0], v[0],
                    x[1], y[1], u[1], v[1],
                    x[2], y[2], u[2], v[2],
                    material->clut_x, material->clut_y, material->tpage);
            }
        } else if (subpixel_pass) {
            gpu_triangle_subpixel(
                subpixel_x, subpixel_y, color24,
                material->semi_transparent ? material->blend_mode : -1,
                material->dither);
        } else if (material->shading == GPU_RENDER_SHADING_GOURAUD) {
            glb_draw_gouraud_triangle_rgb888(x[0], y[0], color24[0],
                                             x[1], y[1], color24[1],
                                             x[2], y[2], color24[2]);
        } else {
            glb_draw_flat_triangle_rgb888(x[0], y[0], x[1], y[1],
                                          x[2], y[2], color24[0]);
        }

        /* Atomic oracle transactions drain each split before checkpoint
         * validation. The authoritative stream uses the ordinary PS1 batches,
         * whose insertion order and state-change flushes preserve draw order. */
        if (!immediate) {
            status = glb_transaction_drain();
            if (status != GPU_RENDER_TRANSACTION_OK) return status;
        }
    }
    return GPU_RENDER_TRANSACTION_OK;
}

static GpuRenderTransactionStatus glb_draw_semantic_native_contents(
        const GpuRenderSemantic *semantic, int base_x, GLuint target_fbo,
        int midpoint_pass) {
    GpuRenderTransactionStatus status;

    s_native_view_pass = 1;
    s_native_view_pass_fbo = target_fbo;
    if (midpoint_pass) s_midpoint_pass_fbo = target_fbo;
    s_native_view_pass_base = base_x;
    s_native_view_expand_x =
        glb_native_view_fullscreen_quad(semantic) ||
        (gpu_ws_nw_backdrop_enabled() &&
         glb_native_view_backdrop_quad(semantic)) ||
        glb_native_view_semantic_reaches_reveal(semantic);
    s_native_view_scale_2d = semantic->screen_space_2d;
    s_native_view_preserve_2d_translation_x =
        semantic->screen_space_2d ==
                GPU_RENDER_SCREEN_SPACE_2D_PRESERVE_SIZE
            ? glb_native_view_preserve_2d_translation_x(semantic)
            : 0;
    status = glb_draw_semantic_contents(semantic, 1, 1);
    flush_flat_batch();
    flush_tex_batch();
    s_native_view_scale_2d = 0;
    s_native_view_preserve_2d_translation_x = 0;
    s_native_view_expand_x = 0;
    s_native_view_pass = 0;
    s_native_view_pass_fbo = 0;
    s_native_view_pass_base = 0;
    if (midpoint_pass) {
        s_midpoint_pass_fbo = 0;
        if (status == GPU_RENDER_TRANSACTION_OK &&
            !native_midpoint_gl_ok(GL_NATIVE_MIDPOINT_GL_DRAW_VIEW))
            status = GPU_RENDER_TRANSACTION_BACKEND_ERROR;
    }
    return status;
}

static GpuRenderTransactionStatus glb_draw_semantic_phase_contents(
        const GpuRenderSemantic *semantic, unsigned int phase) {
    GpuRenderTransactionStatus status;
    NativeDrawState draw_state;

    native_draw_state_save(&draw_state);
    s_midpoint_pass_fbo = native_phase_fbo(phase);
    if (!s_midpoint_pass_fbo) return GPU_RENDER_TRANSACTION_BACKEND_ERROR;
    status = glb_draw_semantic_contents(semantic, 1, 0);
    flush_flat_batch();
    flush_tex_batch();
    s_midpoint_pass_fbo = 0;
    native_draw_state_restore(&draw_state);
    if (status == GPU_RENDER_TRANSACTION_OK &&
        !native_midpoint_gl_ok(GL_NATIVE_MIDPOINT_GL_DRAW_CANONICAL))
        status = GPU_RENDER_TRANSACTION_BACKEND_ERROR;
    return status;
}

static int native_view_wave_tile(
        const GpuRenderSemantic *semantic, int base_x,
        int *left, int *right, int *top, int *bottom) {
    const int32_t unit = INT32_C(65536);
    const GpuRenderSemanticVertex *top_left;
    const GpuRenderSemanticVertex *top_right;
    const GpuRenderSemanticVertex *bottom_left;
    const GpuRenderSemanticVertex *bottom_right;

    if (!semantic || semantic->topology != GPU_RENDER_SEMANTIC_TRIANGLES ||
        semantic->triangle_count != 2u || !semantic->material.textured ||
        semantic->material.raw_texture || semantic->material.semi_transparent ||
        semantic->material.texture_depth != GPU_RENDER_TEXTURE_15_BIT ||
        semantic->material.shading != GPU_RENDER_SHADING_FLAT ||
        semantic->material.texture_window_mask_x != 0u ||
        semantic->material.texture_window_mask_y != 0u ||
        semantic->material.draw_area_left > base_x ||
        semantic->material.draw_area_right <
            base_x + s_native_view_canonical_width - 1)
        return 0;

    top_left = &semantic->triangles[0].vertices[0];
    top_right = &semantic->triangles[0].vertices[1];
    bottom_left = &semantic->triangles[0].vertices[2];
    bottom_right = &semantic->triangles[1].vertices[2];
    if (semantic->triangles[1].vertices[0].x != bottom_left->x ||
        semantic->triangles[1].vertices[0].y != bottom_left->y ||
        semantic->triangles[1].vertices[1].x != top_right->x ||
        semantic->triangles[1].vertices[1].y != top_right->y ||
        top_right->x - top_left->x < 8 * unit ||
        top_right->x - top_left->x > 24 * unit ||
        bottom_right->x - bottom_left->x != top_right->x - top_left->x ||
        top_left->x != bottom_left->x || top_right->x != bottom_right->x ||
        top_left->y != top_right->y || bottom_left->y != bottom_right->y ||
        top_right->u - top_left->u != 16 * unit ||
        bottom_right->u - bottom_left->u != 16 * unit ||
        top_left->u != bottom_left->u || top_right->u != bottom_right->u ||
        bottom_left->v - top_left->v != 16 * unit ||
        bottom_right->v - top_right->v != 16 * unit ||
        top_left->v != top_right->v || bottom_left->v != bottom_right->v)
        return 0;

    {
        const GpuRenderSemanticVertex *vertices[4] = {
            top_left, top_right, bottom_left, bottom_right
        };

        for (int index = 0; index < 4; ++index) {
            if (vertices[index]->native_view_position ||
                vertices[index]->r != 128u || vertices[index]->g != 128u ||
                vertices[index]->b != 128u || vertices[index]->x % unit != 0 ||
                vertices[index]->y % unit != 0 || vertices[index]->u % unit != 0 ||
                vertices[index]->v % unit != 0)
                return 0;
        }
    }

    *left = top_left->x / unit + semantic->material.draw_offset_x - base_x;
    *right = top_right->x / unit + semantic->material.draw_offset_x - base_x;
    *top = top_left->y / unit + semantic->material.draw_offset_y;
    *bottom = bottom_left->y / unit + semantic->material.draw_offset_y;
    return 1;
}

static int32_t native_view_wave_vertex_x(
        const GpuRenderSemanticVertex *vertex) {
    return vertex->native_view_position ? vertex->native_view_x : vertex->x;
}

static int32_t native_view_wave_vertex_y(
        const GpuRenderSemanticVertex *vertex) {
    return vertex->native_view_position ? vertex->native_view_y : vertex->y;
}

static int native_view_wave_fixed_round(int64_t value) {
    return value >= 0
        ? (int)((value + INT64_C(32768)) / INT64_C(65536))
        : -(int)((-value + INT64_C(32768)) / INT64_C(65536));
}

static int native_view_wave_phase_tile(
        const GpuRenderSemantic *semantic, int base_x,
        int *left, int *right, int *top, int *bottom) {
    const int32_t unit = INT32_C(65536);
    const GpuRenderSemanticVertex *top_left;
    const GpuRenderSemanticVertex *top_right;
    const GpuRenderSemanticVertex *bottom_left;
    const GpuRenderSemanticVertex *bottom_right;
    int32_t top_left_x, top_left_y;
    int32_t top_right_x, top_right_y;
    int32_t bottom_left_x, bottom_left_y;
    int32_t bottom_right_x, bottom_right_y;

    if (!semantic || semantic->topology != GPU_RENDER_SEMANTIC_TRIANGLES ||
        semantic->triangle_count != 2u || !semantic->material.textured ||
        semantic->material.raw_texture || semantic->material.semi_transparent ||
        semantic->material.texture_depth != GPU_RENDER_TEXTURE_15_BIT ||
        semantic->material.shading != GPU_RENDER_SHADING_FLAT ||
        semantic->material.texture_window_mask_x != 0u ||
        semantic->material.texture_window_mask_y != 0u ||
        semantic->material.draw_area_left > base_x ||
        semantic->material.draw_area_right <
            base_x + s_native_view_canonical_width - 1)
        return 0;

    top_left = &semantic->triangles[0].vertices[0];
    top_right = &semantic->triangles[0].vertices[1];
    bottom_left = &semantic->triangles[0].vertices[2];
    bottom_right = &semantic->triangles[1].vertices[2];
    top_left_x = native_view_wave_vertex_x(top_left);
    top_left_y = native_view_wave_vertex_y(top_left);
    top_right_x = native_view_wave_vertex_x(top_right);
    top_right_y = native_view_wave_vertex_y(top_right);
    bottom_left_x = native_view_wave_vertex_x(bottom_left);
    bottom_left_y = native_view_wave_vertex_y(bottom_left);
    bottom_right_x = native_view_wave_vertex_x(bottom_right);
    bottom_right_y = native_view_wave_vertex_y(bottom_right);
    if (native_view_wave_vertex_x(&semantic->triangles[1].vertices[0]) !=
            bottom_left_x ||
        native_view_wave_vertex_y(&semantic->triangles[1].vertices[0]) !=
            bottom_left_y ||
        native_view_wave_vertex_x(&semantic->triangles[1].vertices[1]) !=
            top_right_x ||
        native_view_wave_vertex_y(&semantic->triangles[1].vertices[1]) !=
            top_right_y ||
        top_right_x - top_left_x < 8 * unit ||
        top_right_x - top_left_x > 24 * unit ||
        bottom_right_x - bottom_left_x != top_right_x - top_left_x ||
        top_left_x != bottom_left_x || top_right_x != bottom_right_x ||
        top_left_y != top_right_y || bottom_left_y != bottom_right_y ||
        top_right->u - top_left->u != 16 * unit ||
        bottom_right->u - bottom_left->u != 16 * unit ||
        top_left->u != bottom_left->u || top_right->u != bottom_right->u ||
        bottom_left->v - top_left->v != 16 * unit ||
        bottom_right->v - top_right->v != 16 * unit ||
        top_left->v != top_right->v || bottom_left->v != bottom_right->v)
        return 0;

    {
        const GpuRenderSemanticVertex *vertices[4] = {
            top_left, top_right, bottom_left, bottom_right
        };

        for (int index = 0; index < 4; ++index) {
            if (vertices[index]->r != 128u || vertices[index]->g != 128u ||
                vertices[index]->b != 128u || vertices[index]->u % unit != 0 ||
                vertices[index]->v % unit != 0)
                return 0;
        }
    }

    *left = native_view_wave_fixed_round(
        (int64_t)top_left_x +
        (int64_t)semantic->material.draw_offset_x * unit) - base_x;
    *right = native_view_wave_fixed_round(
        (int64_t)top_right_x +
        (int64_t)semantic->material.draw_offset_x * unit) - base_x;
    *top = native_view_wave_fixed_round(
        (int64_t)top_left_y +
        (int64_t)semantic->material.draw_offset_y * unit);
    *bottom = native_view_wave_fixed_round(
        (int64_t)bottom_left_y +
        (int64_t)semantic->material.draw_offset_y * unit);
    return 1;
}

static int native_view_wave_capture_tile(
        const GpuRenderSemantic *semantic, int base_x,
        NativeViewWaveTile *captured, int phase) {
    const GpuRenderSemanticVertex *top_left;
    int left, right, top, bottom;

    if (!(phase
            ? native_view_wave_phase_tile(
                semantic, base_x, &left, &right, &top, &bottom)
            : native_view_wave_tile(
                semantic, base_x, &left, &right, &top, &bottom)))
        return 0;
    top_left = &semantic->triangles[0].vertices[0];
    *captured = (NativeViewWaveTile){
        .left = left,
        .right = right,
        .top = top,
        .bottom = bottom,
        .texture_x = semantic->material.texture_page_x * 64,
        .texture_y = semantic->material.texture_page_y * 256,
        .u = top_left->u / INT32_C(65536),
        .v = top_left->v / INT32_C(65536),
        .draw_top = semantic->material.draw_area_top,
        .framebuffer_height = semantic->material.draw_area_bottom -
                              semantic->material.draw_area_top + 1,
    };
    return 1;
}

typedef struct NativeViewWaveGroup {
    int count;
    int packet[NATIVE_VIEW_WAVE_COLUMNS];
} NativeViewWaveGroup;

static void native_view_wave_sort_group(
        const NativeViewWaveTile tiles[NATIVE_VIEW_WAVE_PACKET_COUNT],
        NativeViewWaveGroup *group) {
    for (int index = 1; index < group->count; ++index) {
        const int packet = group->packet[index];
        int insertion = index;

        while (insertion > 0 &&
               tiles[group->packet[insertion - 1]].left > tiles[packet].left) {
            group->packet[insertion] = group->packet[insertion - 1];
            --insertion;
        }
        group->packet[insertion] = packet;
    }
}

static int native_view_wave_group_is_canonical(
        const NativeViewWaveTile tiles[NATIVE_VIEW_WAVE_PACKET_COUNT],
        const NativeViewWaveGroup *group) {
    const NativeViewWaveTile *left_tile = &tiles[group->packet[0]];

    return left_tile->texture_x + left_tile->u >= s_native_view_wave.base_x &&
        left_tile->texture_x + left_tile->u < s_native_view_wave.base_x +
                                              s_native_view_canonical_width;
}

static int native_view_wave_clip_vertical(
        int *source_top, int *source_bottom,
        int *destination_top, int *destination_bottom) {
    const int source_height = *source_bottom - *source_top;
    const int destination_height = *destination_bottom - *destination_top;

    if (source_height <= 0 || destination_height <= 0) return 0;
    if (*destination_top < 0) {
        const int clipped = -*destination_top;

        *source_top +=
            (clipped * source_height + destination_height - 1) /
                destination_height;
        *destination_top = 0;
    }
    if (*destination_bottom > VRAM_H) {
        const int clipped = *destination_bottom - VRAM_H;

        *source_bottom -=
            (clipped * source_height + destination_height - 1) /
                destination_height;
        *destination_bottom = VRAM_H;
    }
    return *source_top >= 0 && *source_bottom <= VRAM_H &&
        *source_bottom > *source_top &&
        *destination_bottom > *destination_top;
}

static int native_view_wave_append_group(
        unsigned int variant,
        const NativeViewWaveTile tiles[NATIVE_VIEW_WAVE_PACKET_COUNT],
        const NativeViewWaveGroup *group, int canonical_source) {
    const NativeViewWaveTile *left_tile = &tiles[group->packet[0]];
    const NativeViewWaveTile *left_destination = NULL;
    const NativeViewWaveTile *right_destination = NULL;
    const int page = left_tile->draw_top >= 256 ? 1 : 0;
    NativeViewWaveRow *present_row;
    int source_top;
    int source_bottom;
    int left_top;
    int left_bottom;
    int right_top;
    int right_bottom;
    int left_source_top;
    int left_source_bottom;
    int right_source_top;
    int right_source_bottom;

    if (variant >= NATIVE_VIEW_WAVE_VARIANTS ||
        s_native_view_wave.present_row_count[variant] >= NATIVE_VIEW_WAVE_ROWS)
        return 0;
    for (int column = 0; column < NATIVE_VIEW_WAVE_COLUMNS; ++column) {
        const NativeViewWaveTile *tile = &tiles[group->packet[column]];

        if (tile->bottom > tile->top) {
            right_destination = tile;
            break;
        }
    }
    for (int column = NATIVE_VIEW_WAVE_COLUMNS - 1; column >= 0; --column) {
        const NativeViewWaveTile *tile = &tiles[group->packet[column]];

        if (tile->bottom > tile->top) {
            left_destination = tile;
            break;
        }
    }
    if (left_destination == NULL || right_destination == NULL) return 1;
    left_top = left_destination->top;
    left_bottom = left_destination->bottom;
    right_top = right_destination->top;
    right_bottom = right_destination->bottom;
    if (canonical_source) {
        source_top = left_tile->texture_y + left_tile->v;
        source_bottom = source_top + 16;
        left_top -= 32;
        left_bottom -= 32;
        right_top -= 32;
        right_bottom -= 32;
        if (source_top >
            s_native_view_wave.vertical_anchor_source[variant][page]) {
            s_native_view_wave.vertical_anchor_source[variant][page] =
                source_top;
            s_native_view_wave.packed_vertical_offset[variant][page][0] =
                left_top - source_top;
            s_native_view_wave.packed_vertical_offset[variant][page][1] =
                right_top - source_top;
        }
    } else {
        const int packed_row_span = 5 * 16;

        if (left_tile->v < 0 || left_tile->v > 2 * packed_row_span ||
            left_tile->v % packed_row_span != 0)
            return 0;
        source_top = left_tile->draw_top + left_tile->framebuffer_height -
                     3 * 16 + (left_tile->v / packed_row_span) * 16;
        source_bottom = source_top + 16;
        if (s_native_view_wave.vertical_anchor_source[variant][page] >= 0) {
            left_top = source_top +
                s_native_view_wave.packed_vertical_offset[variant][page][0];
            left_bottom = source_bottom +
                s_native_view_wave.packed_vertical_offset[variant][page][0];
            right_top = source_top +
                s_native_view_wave.packed_vertical_offset[variant][page][1];
            right_bottom = source_bottom +
                s_native_view_wave.packed_vertical_offset[variant][page][1];
        }
    }
    if (source_top < 0 || source_bottom > VRAM_H ||
        source_bottom <= source_top)
        return 0;
    left_source_top = source_top;
    left_source_bottom = source_bottom;
    right_source_top = source_top;
    right_source_bottom = source_bottom;
    if (!native_view_wave_clip_vertical(
            &left_source_top, &left_source_bottom,
            &left_top, &left_bottom) ||
        !native_view_wave_clip_vertical(
            &right_source_top, &right_source_bottom,
            &right_top, &right_bottom))
        return 0;

    present_row = &s_native_view_wave.rows[variant]
        [s_native_view_wave.present_row_count[variant]++];
    present_row->left_source_top = left_source_top;
    present_row->left_source_bottom = left_source_bottom;
    present_row->right_source_top = right_source_top;
    present_row->right_source_bottom = right_source_bottom;
    present_row->left_top = left_top;
    present_row->left_bottom = left_bottom;
    present_row->right_top = right_top;
    present_row->right_bottom = right_bottom;
    for (int column = 0; column < NATIVE_VIEW_WAVE_COLUMNS; ++column) {
        const NativeViewWaveTile *tile = &tiles[group->packet[column]];

        if (column == 0) present_row->boundaries[0] = tile->left;
        present_row->boundaries[column + 1] = tile->right;
    }
    return 1;
}

static int native_view_wave_build_variant(unsigned int variant) {
    NativeViewWaveGroup groups[NATIVE_VIEW_WAVE_ROWS] = {0};
    const NativeViewWaveTile *tiles = s_native_view_wave.tiles[variant];

    for (int group = 0; group < NATIVE_VIEW_WAVE_ROWS; ++group) {
        for (int column = 0; column < NATIVE_VIEW_WAVE_COLUMNS; ++column)
            groups[group].packet[groups[group].count++] =
                group * NATIVE_VIEW_WAVE_COLUMNS + column;
        native_view_wave_sort_group(tiles, &groups[group]);
    }

    /* Canonical rows establish each page's source-to-destination offset. The
     * three packed rows can then be placed independently of linked-list order. */
    for (int source_kind = 0; source_kind < 2; ++source_kind) {
        for (int group = 0; group < NATIVE_VIEW_WAVE_ROWS; ++group) {
            const int canonical =
                native_view_wave_group_is_canonical(tiles, &groups[group]);

            if (canonical != (source_kind == 0))
                continue;
            if (!native_view_wave_append_group(
                    variant, tiles, &groups[group], canonical))
                return 0;
        }
    }
    return s_native_view_wave.present_row_count[variant] > 0;
}

static void native_view_wave_record(
        const GpuRenderSemantic *semantic,
        const GpuRenderSemantic *phase_semantics, int base_x, int slot) {
    const int phase_valid = phase_semantics != NULL;
    uint16_t packet;

    if (semantic == NULL || semantic->native_view_effect !=
            GPU_RENDER_NATIVE_VIEW_EFFECT_WAVE_GRID)
        return;
    s_native_view_wave_authenticated = 1;
    s_native_view_wave_authenticated_base_x = base_x;
    s_native_view_wave_authenticated_slot = slot;
    packet = semantic->native_view_effect_index;
    if (!s_native_view_wave.recording || s_native_view_wave.ready) {
        native_view_wave_reset();
        s_native_view_wave_diag.starts++;
        s_native_view_wave.recording = 1;
        s_native_view_wave.base_x = base_x;
        s_native_view_wave.slot = slot;
    }
    if (s_native_view_wave.base_x != base_x ||
        s_native_view_wave.slot != slot) {
        s_native_view_wave_diag.target_resets++;
        native_view_wave_reset();
        return;
    }
    if (packet >= NATIVE_VIEW_WAVE_PACKET_COUNT ||
        s_native_view_wave.tile_seen[packet] ||
        !native_view_wave_capture_tile(
            semantic, base_x,
            &s_native_view_wave.tiles[NATIVE_CURRENT_VARIANT]
                                     [packet], 0)) {
        s_native_view_wave_diag.invalid_row_resets++;
        native_view_wave_reset();
        return;
    }
    s_native_view_wave_diag.semantics++;
    for (unsigned int phase = 0u;
         phase < s_native_interpolation_phase_count; ++phase) {
        const GpuRenderSemantic *phase_semantic =
            phase_valid ? &phase_semantics[phase] : semantic;

        if (!native_view_wave_capture_tile(
                phase_semantic, base_x,
                &s_native_view_wave.tiles[phase]
                                         [packet], 1)) {
            s_native_view_wave_diag.invalid_row_resets++;
            native_view_wave_reset();
            return;
        }
    }
    s_native_view_wave.tile_seen[packet] = 1u;
    ++s_native_view_wave.packet_count;
    if (s_native_view_wave.packet_count < NATIVE_VIEW_WAVE_PACKET_COUNT) return;

    for (unsigned int phase = 0u;
         phase < s_native_interpolation_phase_count; ++phase) {
        if (!native_view_wave_build_variant(phase)) {
            s_native_view_wave_diag.invalid_row_resets++;
            native_view_wave_reset();
            return;
        }
    }
    if (!native_view_wave_build_variant(NATIVE_CURRENT_VARIANT)) {
        s_native_view_wave_diag.invalid_row_resets++;
        native_view_wave_reset();
        return;
    }
    s_native_view_wave.ready = 1;
    s_native_view_wave_diag.completed++;
}

void gl_renderer_native_wave_diag(
        GlRendererNativeWaveDiagnostics *out_diagnostics) {
    if (out_diagnostics == NULL) return;
    *out_diagnostics = s_native_view_wave_diag;
    out_diagnostics->current_packet_count = s_native_view_wave.packet_count;
    out_diagnostics->current_row_count =
        s_native_view_wave.packet_count / NATIVE_VIEW_WAVE_COLUMNS;
    out_diagnostics->current_base_x = s_native_view_wave.base_x;
    out_diagnostics->current_slot = s_native_view_wave.slot;
    out_diagnostics->current_recording = s_native_view_wave.recording;
    out_diagnostics->current_ready = s_native_view_wave.ready;
}

static uint64_t native_view_geometry_mix(uint64_t hash, uint64_t value) {
    hash ^= value + UINT64_C(0x9e3779b97f4a7c15) + (hash << 6u) +
            (hash >> 2u);
    return hash;
}

static uint64_t native_view_geometry_semantic_hash(
        const GpuRenderSemantic *semantic, int native_view) {
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    const GpuRenderMaterial *material = &semantic->material;

#define MIX_GEOMETRY(value) \
    hash = native_view_geometry_mix(hash, (uint64_t)(value))
    MIX_GEOMETRY(semantic->topology);
    MIX_GEOMETRY(semantic->screen_space_2d);
    MIX_GEOMETRY(semantic->native_view_effect);
    MIX_GEOMETRY(semantic->native_view_effect_index);
    MIX_GEOMETRY(semantic->triangle_count);
    MIX_GEOMETRY(semantic->line_count);
    MIX_GEOMETRY(material->draw_offset_x);
    MIX_GEOMETRY(material->draw_offset_y);
    MIX_GEOMETRY(material->draw_area_left);
    MIX_GEOMETRY(material->draw_area_top);
    MIX_GEOMETRY(material->draw_area_right);
    MIX_GEOMETRY(material->draw_area_bottom);
    MIX_GEOMETRY(material->tpage);
    MIX_GEOMETRY(material->clut_x);
    MIX_GEOMETRY(material->clut_y);
    MIX_GEOMETRY(material->shading);
    MIX_GEOMETRY(material->textured);
    MIX_GEOMETRY(material->raw_texture);
    MIX_GEOMETRY(material->semi_transparent);
    MIX_GEOMETRY(material->blend_mode);
    if (semantic->topology == GPU_RENDER_SEMANTIC_TRIANGLES) {
        for (uint8_t primitive = 0u; primitive < semantic->triangle_count;
             ++primitive)
            for (uint8_t index = 0u; index < 3u; ++index) {
                const GpuRenderSemanticVertex *vertex =
                    &semantic->triangles[primitive].vertices[index];
                MIX_GEOMETRY(native_view && vertex->native_view_position);
                MIX_GEOMETRY(native_view && vertex->native_view_position
                    ? vertex->native_view_x : vertex->x);
                MIX_GEOMETRY(native_view && vertex->native_view_position
                    ? vertex->native_view_y : vertex->y);
                MIX_GEOMETRY(vertex->u);
                MIX_GEOMETRY(vertex->v);
                MIX_GEOMETRY(vertex->r);
                MIX_GEOMETRY(vertex->g);
                MIX_GEOMETRY(vertex->b);
            }
    } else {
        for (uint8_t primitive = 0u; primitive < semantic->line_count;
             ++primitive)
            for (uint8_t index = 0u; index < 2u; ++index) {
                const GpuRenderSemanticVertex *vertex =
                    &semantic->lines[primitive].vertices[index];
                MIX_GEOMETRY(native_view && vertex->native_view_position);
                MIX_GEOMETRY(native_view && vertex->native_view_position
                    ? vertex->native_view_x : vertex->x);
                MIX_GEOMETRY(native_view && vertex->native_view_position
                    ? vertex->native_view_y : vertex->y);
                MIX_GEOMETRY(vertex->r);
                MIX_GEOMETRY(vertex->g);
                MIX_GEOMETRY(vertex->b);
            }
    }
#undef MIX_GEOMETRY
    return hash;
}

static void native_geometry_accumulate(
        unsigned int variant, const GpuRenderSemantic *semantic) {
    const uint64_t canonical_hash =
        native_view_geometry_semantic_hash(semantic, 0);
    const uint64_t native_view_hash =
        native_view_geometry_semantic_hash(semantic, 1);

    s_canonical_geometry_hash[variant] = native_view_geometry_mix(
        s_canonical_geometry_count[variant] == 0u
            ? UINT64_C(0xcbf29ce484222325)
            : s_canonical_geometry_hash[variant],
        canonical_hash);
    s_canonical_geometry_count[variant]++;
    s_native_view_geometry_hash[variant] = native_view_geometry_mix(
        s_native_view_geometry_count[variant] == 0u
            ? UINT64_C(0xcbf29ce484222325)
            : s_native_view_geometry_hash[variant],
        native_view_hash);
    s_native_view_geometry_count[variant]++;
}

static int native_host_semantic_depth_order(
        const GpuRenderSemantic *semantic, uint32_t *out_order);

static GpuRenderTransactionStatus native_host_queue_render_pass(
        size_t count, int phase) {
    GpuRenderTransactionStatus status = GPU_RENDER_TRANSACTION_OK;
    NativeDrawState draw_state;
    GLuint active_fbo = 0;
    int active_base = 0;
    int active_expand = 0;
    int active_scale_2d = 0;
    int active_preserve_x = 0;

    native_draw_state_save(&draw_state);
    for (size_t index = 0u; index < count; ++index)
        s_native_host_render_order[index] = index;
    if (phase >= 0 && s_native_interpolation_phase_count > 1u) {
        for (size_t source = 0u; source < count; ++source) {
            const NativeHostQueuedSemantic *extra =
                &s_native_host_queue[source];
            size_t current_position = 0u;
            size_t destination = 0u;

            if (!extra->phase_only ||
                (extra->phase_visibility_mask & (1u << phase)) == 0u)
                continue;
            while (current_position < count &&
                   s_native_host_render_order[current_position] != source)
                ++current_position;
            if (current_position == count) continue;
            memmove(&s_native_host_render_order[current_position],
                    &s_native_host_render_order[current_position + 1u],
                    (count - current_position - 1u) *
                        sizeof(s_native_host_render_order[0]));
            for (size_t order_index = 0u; order_index + 1u < count;
                 ++order_index) {
                const NativeHostQueuedSemantic *queued =
                    &s_native_host_queue[
                        s_native_host_render_order[order_index]];
                const GpuRenderSemantic *ordered_semantic;
                uint32_t order;

                if (queued->clear_margins ||
                    (queued->phase_only &&
                     (queued->phase_visibility_mask & (1u << phase)) == 0u))
                    continue;
                if (queued->phase_only) {
                    order = queued->phase_order[phase];
                } else {
                    ordered_semantic = queued->midpoint_valid
                        ? (phase == 0 ? &queued->midpoint
                                      : &queued->extra_phases[phase - 1])
                        : &queued->current;
                    if (!native_host_semantic_depth_order(
                            ordered_semantic, &order))
                        continue;
                }
                if (order >= extra->phase_order[phase])
                    destination = order_index + 1u;
                else
                    break;
            }
            memmove(&s_native_host_render_order[destination + 1u],
                    &s_native_host_render_order[destination],
                    (count - destination - 1u) *
                        sizeof(s_native_host_render_order[0]));
            s_native_host_render_order[destination] = source;
        }
    }
    for (size_t draw_index = 0u; draw_index < count; ++draw_index) {
        const size_t index = s_native_host_render_order[draw_index];
        const NativeHostQueuedSemantic *queued = &s_native_host_queue[index];
        const GpuRenderSemantic *semantic =
            phase >= 0 && queued->midpoint_valid
                ? (phase == 0 ? &queued->midpoint
                              : &queued->extra_phases[phase - 1])
                : &queued->current;
        const GLuint target_fbo = phase >= 0
            ? native_view_phase_fbo(queued->slot, (unsigned int)phase)
            : s_native_view_fbo[queued->slot];
        int expand;
        int preserve_x;

        if (phase >= 0 && !queued->midpoint_valid && queued->clear_margins)
            continue;
        if (phase < 0 && queued->phase_only) continue;
        if (phase >= 0 && queued->phase_only &&
            (queued->phase_visibility_mask & (1u << phase)) == 0u)
            continue;
        if (!target_fbo) {
            status = GPU_RENDER_TRANSACTION_BACKEND_ERROR;
            break;
        }
        if (queued->clear_margins) {
            const int right_x = s_native_view_offset +
                s_native_view_canonical_width;

            flush_flat_batch();
            flush_tex_batch();
            active_fbo = 0;
            if (s_native_view_offset > 0)
                native_view_fill_local_wrapped_y(
                    target_fbo, 0, queued->clear_y,
                    s_native_view_offset, queued->clear_h,
                    queued->clear_color);
            if (right_x < s_native_view_width)
                native_view_fill_local_wrapped_y(
                    target_fbo, right_x, queued->clear_y,
                    s_native_view_width - right_x, queued->clear_h,
                    queued->clear_color);
            continue;
        }
        expand = glb_native_view_fullscreen_quad(semantic) ||
            (gpu_ws_nw_backdrop_enabled() &&
             glb_native_view_backdrop_quad(semantic)) ||
            glb_native_view_semantic_reaches_reveal(semantic);
        preserve_x = semantic->screen_space_2d ==
                GPU_RENDER_SCREEN_SPACE_2D_PRESERVE_SIZE
            ? glb_native_view_preserve_2d_translation_x(semantic) : 0;
        if (active_fbo != 0 &&
            (active_fbo != target_fbo || active_base != queued->base_x ||
             active_expand != expand ||
             active_scale_2d != semantic->screen_space_2d ||
             active_preserve_x != preserve_x)) {
            flush_flat_batch();
            flush_tex_batch();
            active_fbo = 0;
        }
        if (active_fbo == 0) {
            s_native_view_pass = 1;
            s_native_view_pass_fbo = target_fbo;
            s_native_view_pass_base = queued->base_x;
            s_native_view_expand_x = expand;
            s_native_view_scale_2d = semantic->screen_space_2d;
            s_native_view_preserve_2d_translation_x = preserve_x;
            s_midpoint_pass_fbo = phase >= 0 ? target_fbo : 0;
            active_fbo = target_fbo;
            active_base = queued->base_x;
            active_expand = expand;
            active_scale_2d = semantic->screen_space_2d;
            active_preserve_x = preserve_x;
        }
        status = glb_draw_semantic_contents(semantic, 1, 1);
        if (status != GPU_RENDER_TRANSACTION_OK) break;
    }
    flush_flat_batch();
    flush_tex_batch();
    s_native_view_scale_2d = 0;
    s_native_view_preserve_2d_translation_x = 0;
    s_native_view_expand_x = 0;
    s_native_view_pass = 0;
    s_native_view_pass_fbo = 0;
    s_native_view_pass_base = 0;
    s_midpoint_pass_fbo = 0;
    native_draw_state_restore(&draw_state);
    if (status == GPU_RENDER_TRANSACTION_OK && phase >= 0 &&
        !native_midpoint_gl_ok(GL_NATIVE_MIDPOINT_GL_DRAW_VIEW))
        status = GPU_RENDER_TRANSACTION_BACKEND_ERROR;
    return status;
}

static const GpuRenderSemanticVertex *native_host_diag_primitive_vertex(
        const GpuRenderSemantic *semantic, size_t primitive,
        size_t vertex) {
    return semantic->topology == GPU_RENDER_SEMANTIC_TRIANGLES
        ? &semantic->triangles[primitive].vertices[vertex]
        : &semantic->lines[primitive].vertices[vertex];
}

static void native_host_diag_vertex_position(
        const NativeHostQueuedSemantic *queued,
        const GpuRenderSemantic *semantic,
        const GpuRenderSemanticVertex *vertex,
        int64_t *out_x, int64_t *out_y) {
    if (vertex->native_view_position) {
        *out_x = vertex->native_view_x +
            (int64_t)(semantic->material.draw_offset_x - queued->base_x) *
                INT64_C(65536);
        *out_y = vertex->native_view_y +
            (int64_t)semantic->material.draw_offset_y * INT64_C(65536);
    } else {
        *out_x = vertex->x +
            (int64_t)(semantic->material.draw_offset_x +
                      s_native_view_offset - queued->base_x) * INT64_C(65536);
        *out_y = vertex->y +
            (int64_t)semantic->material.draw_offset_y * INT64_C(65536);
    }
}

static int native_host_diag_fixed_floor(int64_t value) {
    return value >= 0
        ? (int)(value / INT64_C(65536))
        : -(int)((-value + INT64_C(65535)) / INT64_C(65536));
}

static int native_host_semantic_is_terrain(
        const GpuRenderSemantic *semantic);

static int native_host_semantic_depth_order(
        const GpuRenderSemantic *semantic, uint32_t *out_order) {
    const size_t primitive_count =
        semantic->topology == GPU_RENDER_SEMANTIC_TRIANGLES
            ? semantic->triangle_count : semantic->line_count;
    const size_t vertex_count =
        semantic->topology == GPU_RENDER_SEMANTIC_TRIANGLES ? 3u : 2u;
    uint32_t max_depth = 0u;
    int found = 0;

    for (size_t primitive = 0u; primitive < primitive_count; ++primitive)
        for (size_t vertex = 0u; vertex < vertex_count; ++vertex) {
            const GpuRenderSemanticVertex *position =
                native_host_diag_primitive_vertex(
                    semantic, primitive, vertex);

            if (!position->projective_position ||
                position->projective_view_z <= 0)
                continue;
            if ((uint32_t)position->projective_view_z > max_depth)
                max_depth = (uint32_t)position->projective_view_z;
            found = 1;
        }
    if (!found) return 0;
    *out_order = max_depth >> 4u;
    if (*out_order >= UINT32_C(0xf0))
        *out_order = UINT32_C(0xef);
    return 1;
}

static int native_host_temporal_phase_visible(
        const GpuRenderSemantic *semantic,
        const GpuRenderTemporalCullPolicy *policy,
        uint32_t *out_order) {
    const size_t primitive_count =
        semantic->topology == GPU_RENDER_SEMANTIC_TRIANGLES
            ? semantic->triangle_count : semantic->line_count;
    const size_t vertex_count =
        semantic->topology == GPU_RENDER_SEMANTIC_TRIANGLES ? 3u : 2u;
    int64_t min_x = INT64_MAX;
    int64_t min_y = INT64_MAX;
    int64_t max_x = INT64_MIN;
    int64_t max_y = INT64_MIN;
    int32_t min_depth = INT32_MAX;
    int32_t max_depth = INT32_MIN;
    int32_t last_depth = 0;
    int64_t depth_sum = 0;
    size_t depth_count = 0u;
    int found = 0;

    for (size_t primitive = 0u; primitive < primitive_count; ++primitive)
        for (size_t vertex = 0u; vertex < vertex_count; ++vertex) {
            const GpuRenderSemanticVertex *position =
                native_host_diag_primitive_vertex(
                    semantic, primitive, vertex);
            int32_t depth;

            if ((policy->flags & GPU_RENDER_TEMPORAL_CULL_PROJECTIVE) != 0u &&
                (!position->projective_position ||
                 position->projective_view_z <= 0))
                return 0;
            if (position->x < min_x) min_x = position->x;
            if (position->x > max_x) max_x = position->x;
            if (position->y < min_y) min_y = position->y;
            if (position->y > max_y) max_y = position->y;
            if (position->temporal_depth_valid) {
                depth = position->temporal_depth;
            } else if (position->projective_position) {
                depth = position->projective_view_z;
            } else if ((policy->flags & GPU_RENDER_TEMPORAL_CULL_DEPTH) != 0u) {
                return 0;
            } else {
                depth = 0;
            }
            if (depth < min_depth) min_depth = depth;
            if (depth > max_depth) max_depth = depth;
            last_depth = depth;
            depth_sum += depth;
            ++depth_count;
            found = 1;
        }
    if (!found) return 0;
    if ((policy->flags & GPU_RENDER_TEMPORAL_CULL_SCREEN) != 0u &&
        (max_x < policy->screen_left ||
         min_x >= policy->screen_right_exclusive ||
         max_y < policy->screen_top ||
         min_y >= policy->screen_bottom_exclusive))
        return 0;
    if ((policy->flags & GPU_RENDER_TEMPORAL_CULL_FRONT_FACE) != 0u) {
        const GpuRenderSemanticVertex *a;
        const GpuRenderSemanticVertex *b;
        const GpuRenderSemanticVertex *c;
        int64_t area;

        if (semantic->topology != GPU_RENDER_SEMANTIC_TRIANGLES ||
            semantic->triangle_count == 0u)
            return 0;
        a = &semantic->triangles[0].vertices[0];
        b = &semantic->triangles[0].vertices[1];
        c = &semantic->triangles[0].vertices[2];
        area = ((int64_t)b->x - a->x) * ((int64_t)c->y - a->y) -
            ((int64_t)b->y - a->y) * ((int64_t)c->x - a->x);
        if ((policy->front_face == GPU_RENDER_TEMPORAL_FRONT_POSITIVE &&
             area <= 0) ||
            (policy->front_face == GPU_RENDER_TEMPORAL_FRONT_NEGATIVE &&
             area >= 0))
            return 0;
    }
    if ((policy->flags & GPU_RENDER_TEMPORAL_CULL_DEPTH) != 0u) {
        const int32_t depth = policy->depth_mode ==
                GPU_RENDER_TEMPORAL_DEPTH_MINIMUM
            ? min_depth
            : (policy->depth_mode == GPU_RENDER_TEMPORAL_DEPTH_AVERAGE
                ? (int32_t)(depth_sum / (int64_t)depth_count)
                : (policy->depth_mode ==
                       GPU_RENDER_TEMPORAL_DEPTH_LAST_VERTEX
                    ? last_depth : max_depth));

        if (policy->depth_mode == GPU_RENDER_TEMPORAL_DEPTH_NONE ||
            depth < policy->depth_min_inclusive ||
            depth >= policy->depth_max_exclusive)
            return 0;
        *out_order = (uint32_t)depth >> policy->ordering_depth_shift;
    } else if (!native_host_semantic_depth_order(semantic, out_order)) {
        *out_order = 0u;
    }
    return 1;
}

static void native_host_queue_merge_temporal_phases(size_t count) {
    size_t remaining = 0u;

    for (size_t index = 0u; index < count; ++index)
        if (s_native_host_queue[index].temporal_order_valid == 1)
            ++remaining;
    while (remaining != 0u) {
        NativeHostQueuedSemantic extra;
        size_t source = 0u;
        size_t destination = 0u;

        while (source < count &&
               s_native_host_queue[source].temporal_order_valid != 1)
            ++source;
        if (source == count) break;
        extra = s_native_host_queue[source];
        memmove(&s_native_host_queue[source],
                &s_native_host_queue[source + 1u],
                (count - source - 1u) * sizeof(s_native_host_queue[0]));
        for (size_t index = 0u; index + 1u < count; ++index) {
            uint32_t order;

            if (s_native_host_queue[index].clear_margins ||
                s_native_host_queue[index].temporal_order_valid != 0 ||
                !native_host_semantic_depth_order(
                    &s_native_host_queue[index].current, &order))
                continue;
            if (order >= extra.phase_order[0])
                destination = index + 1u;
            else
                break;
        }
        memmove(&s_native_host_queue[destination + 1u],
                &s_native_host_queue[destination],
                (count - destination - 1u) * sizeof(s_native_host_queue[0]));
        extra.temporal_order_valid = 2;
        s_native_host_queue[destination] = extra;
        --remaining;
    }
    for (size_t index = 0u; index < count; ++index)
        if (s_native_host_queue[index].temporal_order_valid == 2)
            s_native_host_queue[index].temporal_order_valid = 1;
}

static int native_host_semantic_is_world_model(
        const GpuRenderSemantic *semantic);
static int native_host_semantic_is_terrain(
        const GpuRenderSemantic *semantic);
static GlRendererRetiredFailureEvent *native_host_record_retired_failure(
    GlRendererRetiredFailureReason reason,
    const GpuRenderSemantic *semantic, size_t previous_order,
    uint32_t group_id, uint32_t vertex_id, uint32_t auxiliary,
    int64_t value_a, int64_t value_b);
static void native_host_record_midpoint_failure(
    GlRendererRetiredFailureReason reason,
    const NativeHostQueuedSemantic *queued,
    const GpuRenderSemantic *current, const GpuRenderSemantic *midpoint,
    size_t primitive, size_t previous_order,
    const int current_x[3], const int current_y[3],
    const int midpoint_x[3], const int midpoint_y[3],
    int64_t value_a, int64_t value_b);

static void native_host_queue_snapshot_present(size_t count) {
    memset(s_producer_diag_vertices, 0, sizeof(s_producer_diag_vertices));
    for (size_t queue_index = 0u; queue_index < count; ++queue_index) {
        const NativeHostQueuedSemantic *queued = &s_native_host_queue[queue_index];
        const GpuRenderSemantic *current = &queued->current;
        const GpuRenderSemantic *midpoint = queued->midpoint_valid
            ? &queued->midpoint : current;
        GpuSemanticWorkloadMatchInfo match = {
            .kind = GPU_SEMANTIC_WORKLOAD_MATCH_UNKNOWN,
        };
        const size_t primitive_count =
            current->topology == GPU_RENDER_SEMANTIC_TRIANGLES
                ? current->triangle_count : current->line_count;
        const size_t vertex_count =
            current->topology == GPU_RENDER_SEMANTIC_TRIANGLES ? 3u : 2u;

        if (queued->clear_margins) continue;
        if (current->interpolation_identity.valid)
            (void)gpu_semantic_workload_match_info(
                &current->interpolation_identity, &match);
        for (size_t primitive = 0u; primitive < primitive_count; ++primitive) {
            GlRendererSemanticProducerItemDiagnostics *item;

            item = &s_native_host_diag_primitives[
                s_native_host_diag_primitive_total++ %
                    NATIVE_HOST_DIAG_PRIMITIVE_CAP];
            *item = (GlRendererSemanticProducerItemDiagnostics){
                .frame = s_frame_count,
                .scene_id = current->interpolation_identity.scene_id,
                .producer_id = current->interpolation_identity.producer_id,
                .primitive_id = current->interpolation_identity.primitive_id,
                .identity_valid = current->interpolation_identity.valid,
                .queue_order = (uint32_t)queue_index,
                .base_x = queued->base_x,
                .slot = queued->slot,
                .current_order = (uint32_t)match.current_order,
                .previous_order = (uint32_t)match.previous_order,
                .match_kind = (uint32_t)match.kind,
                .fallback_kind = (uint32_t)match.fallback_kind,
                .subprimitive_index = (uint32_t)primitive,
                .topology = current->topology,
                .screen_space_2d = current->screen_space_2d,
                .world_model = native_host_semantic_is_world_model(current),
                .tpage = current->material.tpage,
                .clut_x = current->material.clut_x,
                .clut_y = current->material.clut_y,
                .draw_offset_x = current->material.draw_offset_x,
                .draw_offset_y = current->material.draw_offset_y,
                .draw_area = {
                    current->material.draw_area_left,
                    current->material.draw_area_top,
                    current->material.draw_area_right,
                    current->material.draw_area_bottom,
                },
                .textured = current->material.textured,
                .raw_texture = current->material.raw_texture,
                .semi_transparent = current->material.semi_transparent,
                .previous_order_valid = match.previous_order_valid ? 1 : 0,
            };
            int current_pixel_x[3] = {0};
            int current_pixel_y[3] = {0};
            int midpoint_pixel_x[3] = {0};
            int midpoint_pixel_y[3] = {0};
            int64_t current_fixed_x[3] = {0};
            int64_t current_fixed_y[3] = {0};
            int64_t midpoint_fixed_x[3] = {0};
            int64_t midpoint_fixed_y[3] = {0};
            for (size_t vertex = 0u; vertex < vertex_count; ++vertex) {
                const GpuRenderSemanticVertex *current_vertex =
                    native_host_diag_primitive_vertex(
                        current, primitive, vertex);
                const GpuRenderSemanticVertex *midpoint_vertex =
                    native_host_diag_primitive_vertex(
                        midpoint, primitive, vertex);
                int64_t current_x, current_y, midpoint_x, midpoint_y;

                native_host_diag_vertex_position(
                    queued, current, current_vertex, &current_x, &current_y);
                native_host_diag_vertex_position(
                    queued, midpoint, midpoint_vertex,
                    &midpoint_x, &midpoint_y);
                current_pixel_x[vertex] =
                    native_host_diag_fixed_floor(current_x);
                current_pixel_y[vertex] =
                    native_host_diag_fixed_floor(current_y);
                midpoint_pixel_x[vertex] =
                    native_host_diag_fixed_floor(midpoint_x);
                midpoint_pixel_y[vertex] =
                    native_host_diag_fixed_floor(midpoint_y);
                current_fixed_x[vertex] = current_x;
                current_fixed_y[vertex] = current_y;
                midpoint_fixed_x[vertex] = midpoint_x;
                midpoint_fixed_y[vertex] = midpoint_y;
                if (vertex == 0u) {
                    item->raw_bounds[0] = glb_fixed_floor(current_vertex->x);
                    item->raw_bounds[1] = glb_fixed_floor(current_vertex->y);
                    item->raw_bounds[2] = item->raw_bounds[0];
                    item->raw_bounds[3] = item->raw_bounds[1];
                    item->uv_bounds[0] = current_vertex->u / INT32_C(65536);
                    item->uv_bounds[1] = current_vertex->v / INT32_C(65536);
                    item->uv_bounds[2] = item->uv_bounds[0];
                    item->uv_bounds[3] = item->uv_bounds[1];
                    item->current_bounds[0] = current_pixel_x[vertex];
                    item->current_bounds[1] = current_pixel_y[vertex];
                    item->current_bounds[2] = current_pixel_x[vertex];
                    item->current_bounds[3] = current_pixel_y[vertex];
                    item->midpoint_bounds[0] = midpoint_pixel_x[vertex];
                    item->midpoint_bounds[1] = midpoint_pixel_y[vertex];
                    item->midpoint_bounds[2] = midpoint_pixel_x[vertex];
                    item->midpoint_bounds[3] = midpoint_pixel_y[vertex];
                } else {
                    const int raw_x = glb_fixed_floor(current_vertex->x);
                    const int raw_y = glb_fixed_floor(current_vertex->y);
                    const int u = current_vertex->u / INT32_C(65536);
                    const int v = current_vertex->v / INT32_C(65536);

                    if (raw_x < item->raw_bounds[0]) item->raw_bounds[0] = raw_x;
                    if (raw_y < item->raw_bounds[1]) item->raw_bounds[1] = raw_y;
                    if (raw_x > item->raw_bounds[2]) item->raw_bounds[2] = raw_x;
                    if (raw_y > item->raw_bounds[3]) item->raw_bounds[3] = raw_y;
                    if (u < item->uv_bounds[0]) item->uv_bounds[0] = u;
                    if (v < item->uv_bounds[1]) item->uv_bounds[1] = v;
                    if (u > item->uv_bounds[2]) item->uv_bounds[2] = u;
                    if (v > item->uv_bounds[3]) item->uv_bounds[3] = v;
                    if (current_pixel_x[vertex] < item->current_bounds[0])
                        item->current_bounds[0] = current_pixel_x[vertex];
                    if (current_pixel_y[vertex] < item->current_bounds[1])
                        item->current_bounds[1] = current_pixel_y[vertex];
                    if (current_pixel_x[vertex] > item->current_bounds[2])
                        item->current_bounds[2] = current_pixel_x[vertex];
                    if (current_pixel_y[vertex] > item->current_bounds[3])
                        item->current_bounds[3] = current_pixel_y[vertex];
                    if (midpoint_pixel_x[vertex] < item->midpoint_bounds[0])
                        item->midpoint_bounds[0] = midpoint_pixel_x[vertex];
                    if (midpoint_pixel_y[vertex] < item->midpoint_bounds[1])
                        item->midpoint_bounds[1] = midpoint_pixel_y[vertex];
                    if (midpoint_pixel_x[vertex] > item->midpoint_bounds[2])
                        item->midpoint_bounds[2] = midpoint_pixel_x[vertex];
                    if (midpoint_pixel_y[vertex] > item->midpoint_bounds[3])
                        item->midpoint_bounds[3] = midpoint_pixel_y[vertex];
                }
                if (current_x != midpoint_x || current_y != midpoint_y)
                    ++item->moving_vertex_count;
                item->midpoint_delta_fixed += current_x >= midpoint_x
                    ? (uint64_t)(current_x - midpoint_x)
                    : (uint64_t)(midpoint_x - current_x);
                item->midpoint_delta_fixed += current_y >= midpoint_y
                    ? (uint64_t)(current_y - midpoint_y)
                    : (uint64_t)(midpoint_y - current_y);
                if (queued->midpoint_valid &&
                    midpoint_vertex->interpolation_vertex_identity_valid) {
                    size_t slot =
                        ((size_t)midpoint->interpolation_identity.scene_id ^
                         ((size_t)midpoint_vertex->interpolation_group_id << 7u) ^
                         midpoint_vertex->interpolation_vertex_id) &
                        (PRODUCER_DIAG_VERTEX_CAP - 1u);

                    for (size_t probe = 0u; probe < PRODUCER_DIAG_VERTEX_CAP;
                         ++probe) {
                        ProducerDiagVertex *entry =
                            &s_producer_diag_vertices[slot];

                        if (!entry->used) {
                            *entry = (ProducerDiagVertex){
                                .scene_id = midpoint->interpolation_identity.scene_id,
                                .group_id = midpoint_vertex->interpolation_group_id,
                                .vertex_id = midpoint_vertex->interpolation_vertex_id,
                                .x = midpoint_x,
                                .y = midpoint_y,
                                .used = 1,
                            };
                            break;
                        }
                        if (entry->scene_id ==
                                midpoint->interpolation_identity.scene_id &&
                            entry->group_id ==
                                midpoint_vertex->interpolation_group_id &&
                            entry->vertex_id ==
                                midpoint_vertex->interpolation_vertex_id) {
                            if (entry->x != midpoint_x || entry->y != midpoint_y)
                                native_host_record_retired_failure(
                                    GL_RETIRED_FAILURE_MIDPOINT_VERTEX_CONFLICT,
                                    current,
                                    match.previous_order_valid
                                        ? match.previous_order : SIZE_MAX,
                                    midpoint_vertex->interpolation_group_id,
                                    midpoint_vertex->interpolation_vertex_id,
                                    (uint32_t)primitive |
                                        (queued->phase_only
                                            ? UINT32_C(0x80000000) : 0u),
                                    midpoint_x - entry->x,
                                    midpoint_y - entry->y);
                            break;
                        }
                        slot = (slot + 1u) &
                            (PRODUCER_DIAG_VERTEX_CAP - 1u);
                    }
                }
            }
            if (queued->midpoint_valid &&
                current->interpolation_identity.valid &&
                current->topology == GPU_RENDER_SEMANTIC_TRIANGLES) {
                const size_t previous_order = match.previous_order_valid
                    ? match.previous_order : SIZE_MAX;

                item->current_area =
                    (int64_t)(current_pixel_x[1] - current_pixel_x[0]) *
                        (current_pixel_y[2] - current_pixel_y[0]) -
                    (int64_t)(current_pixel_y[1] - current_pixel_y[0]) *
                        (current_pixel_x[2] - current_pixel_x[0]);
                item->midpoint_area =
                    (int64_t)(midpoint_pixel_x[1] - midpoint_pixel_x[0]) *
                        (midpoint_pixel_y[2] - midpoint_pixel_y[0]) -
                    (int64_t)(midpoint_pixel_y[1] - midpoint_pixel_y[0]) *
                        (midpoint_pixel_x[2] - midpoint_pixel_x[0]);
                const int64_t current_fixed_area =
                    (current_fixed_x[1] - current_fixed_x[0]) *
                        (current_fixed_y[2] - current_fixed_y[0]) -
                    (current_fixed_y[1] - current_fixed_y[0]) *
                        (current_fixed_x[2] - current_fixed_x[0]);
                const int64_t midpoint_fixed_area =
                    (midpoint_fixed_x[1] - midpoint_fixed_x[0]) *
                        (midpoint_fixed_y[2] - midpoint_fixed_y[0]) -
                    (midpoint_fixed_y[1] - midpoint_fixed_y[0]) *
                        (midpoint_fixed_x[2] - midpoint_fixed_x[0]);
                if (item->current_area != 0 && item->midpoint_area == 0)
                        native_host_record_midpoint_failure(
                            GL_RETIRED_FAILURE_MIDPOINT_ZERO_AREA,
                            queued, current, midpoint, primitive,
                            previous_order, current_pixel_x, current_pixel_y,
                            midpoint_pixel_x, midpoint_pixel_y,
                            item->current_area,
                            item->midpoint_area);
                if (item->current_bounds[2] > item->current_bounds[0] &&
                        item->current_bounds[3] > item->current_bounds[1] &&
                        (item->midpoint_bounds[2] == item->midpoint_bounds[0] ||
                         item->midpoint_bounds[3] == item->midpoint_bounds[1]))
                        native_host_record_midpoint_failure(
                            GL_RETIRED_FAILURE_MIDPOINT_EXTENT_COLLAPSE,
                            queued, current, midpoint, primitive,
                            previous_order, current_pixel_x, current_pixel_y,
                            midpoint_pixel_x, midpoint_pixel_y,
                            item->current_area,
                            item->midpoint_area);
                if ((item->current_area < 0 && item->midpoint_area > 0) ||
                        (item->current_area > 0 && item->midpoint_area < 0))
                        native_host_record_midpoint_failure(
                            GL_RETIRED_FAILURE_MIDPOINT_WINDING_FLIP,
                            queued, current, midpoint, primitive,
                            previous_order, current_pixel_x, current_pixel_y,
                            midpoint_pixel_x, midpoint_pixel_y,
                            item->current_area,
                            item->midpoint_area);
                if (current_fixed_area != 0 && midpoint_fixed_area == 0)
                    native_host_record_midpoint_failure(
                        GL_RETIRED_FAILURE_MIDPOINT_FIXED_ZERO_AREA,
                        queued, current, midpoint, primitive,
                        previous_order, current_pixel_x, current_pixel_y,
                        midpoint_pixel_x, midpoint_pixel_y,
                        current_fixed_area, midpoint_fixed_area);
                if ((current_fixed_area < 0 && midpoint_fixed_area > 0) ||
                    (current_fixed_area > 0 && midpoint_fixed_area < 0))
                    native_host_record_midpoint_failure(
                        GL_RETIRED_FAILURE_MIDPOINT_FIXED_WINDING_FLIP,
                        queued, current, midpoint, primitive,
                        previous_order, current_pixel_x, current_pixel_y,
                        midpoint_pixel_x, midpoint_pixel_y,
                        current_fixed_area, midpoint_fixed_area);
            }
        }
    }
}

static void native_host_queue_capture_history(
        size_t count, unsigned int history_index) {
    NativeHostSemanticHistory *history =
        s_native_host_semantic_history[history_index];
    uint32_t generation =
        ++s_native_host_semantic_history_generation[history_index];

    if (generation == 0u)
        generation = ++s_native_host_semantic_history_generation[history_index];
    for (size_t queue_index = 0u; queue_index < count; ++queue_index) {
        const NativeHostQueuedSemantic *queued = &s_native_host_queue[queue_index];
        const GpuRenderSemantic *semantic = &queued->current;
        GpuSemanticWorkloadMatchInfo match;

        if (queued->clear_margins || queued->phase_only ||
            !semantic->interpolation_identity.valid)
            continue;
        if (gpu_semantic_workload_match_info(
                &semantic->interpolation_identity, &match) !=
                    GPU_SEMANTIC_WORKLOAD_OK ||
            match.current_order >= NATIVE_HOST_QUEUE_CAP)
            continue;
        history[match.current_order] = (NativeHostSemanticHistory){
            .semantic = *semantic,
            .base_x = queued->base_x,
            .slot = queued->slot,
            .generation = generation,
        };
    }
}

static int native_host_semantic_is_world_model(
        const GpuRenderSemantic *semantic) {
    const GpuRenderSemanticVertex *vertex;

    if (semantic->topology != GPU_RENDER_SEMANTIC_TRIANGLES ||
        semantic->triangle_count == 0u)
        return 0;
    vertex = &semantic->triangles[0].vertices[0];
    return vertex->interpolation_vertex_identity_valid &&
        (vertex->interpolation_group_id & UINT32_C(0xff000000)) ==
            UINT32_C(0x64000000);
}

static int native_host_semantic_is_terrain(
        const GpuRenderSemantic *semantic) {
    const GpuRenderSemanticVertex *vertex;

    if (semantic->topology != GPU_RENDER_SEMANTIC_TRIANGLES ||
        semantic->triangle_count == 0u)
        return 0;
    vertex = &semantic->triangles[0].vertices[0];
    return vertex->interpolation_vertex_identity_valid &&
        vertex->interpolation_group_id == UINT32_C(0x63000000);
}

static GlRendererRetiredFailureEvent *native_host_record_retired_failure(
        GlRendererRetiredFailureReason reason,
        const GpuRenderSemantic *semantic, size_t previous_order,
        uint32_t group_id, uint32_t vertex_id, uint32_t auxiliary,
        int64_t value_a, int64_t value_b) {
    GlRendererRetiredFailureEvent event;
    GlRendererRetiredFailureEvent *stored = NULL;

    if (semantic == NULL) return NULL;
    event = (GlRendererRetiredFailureEvent){
        .frame = s_frame_count,
        .scene_id = semantic->interpolation_identity.scene_id,
        .reason = (uint32_t)reason,
        .producer_id = semantic->interpolation_identity.producer_id,
        .primitive_id = semantic->interpolation_identity.primitive_id,
        .group_id = group_id,
        .vertex_id = vertex_id,
        .previous_order = (uint32_t)previous_order,
        .auxiliary = auxiliary,
        .value_a = value_a,
        .value_b = value_b,
    };
    if (s_retired_failure_event_count < RETIRED_FAILURE_EVENT_CAP) {
        stored = &s_retired_failure_events[s_retired_failure_event_count];
        *stored = event;
    } else {
        ++s_retired_failure_event_overflow;
    }
    ++s_retired_failure_event_count;
    return stored;
}

static void native_host_record_midpoint_failure(
        GlRendererRetiredFailureReason reason,
        const NativeHostQueuedSemantic *queued,
        const GpuRenderSemantic *current, const GpuRenderSemantic *midpoint,
        size_t primitive, size_t previous_order,
        const int current_x[3], const int current_y[3],
        const int midpoint_x[3], const int midpoint_y[3],
        int64_t value_a, int64_t value_b) {
    const GpuRenderSemanticVertex *identity_vertex =
        native_host_diag_primitive_vertex(current, primitive, 0u);
    GlRendererRetiredFailureEvent *event =
        native_host_record_retired_failure(
            reason, current, previous_order,
            identity_vertex->interpolation_group_id,
            identity_vertex->interpolation_vertex_id,
            (uint32_t)primitive |
                (queued->phase_only ? UINT32_C(0x80000000) : 0u),
            value_a, value_b);
    int current_min_x;
    int current_max_x;
    int midpoint_min_x;
    int midpoint_max_x;

    if (event == NULL) return;
    current_min_x = current_max_x = current_x[0];
    midpoint_min_x = midpoint_max_x = midpoint_x[0];
    for (size_t vertex = 0u; vertex < 3u; ++vertex) {
        const GpuRenderSemanticVertex *current_vertex =
            native_host_diag_primitive_vertex(current, primitive, vertex);
        const GpuRenderSemanticVertex *midpoint_vertex =
            native_host_diag_primitive_vertex(midpoint, primitive, vertex);

        event->current_x[vertex] = current_x[vertex];
        event->current_y[vertex] = current_y[vertex];
        event->midpoint_x[vertex] = midpoint_x[vertex];
        event->midpoint_y[vertex] = midpoint_y[vertex];
        event->current_z[vertex] = current_vertex->projective_view_z;
        event->midpoint_z[vertex] = midpoint_vertex->projective_view_z;
        if (current_x[vertex] < current_min_x)
            current_min_x = current_x[vertex];
        if (current_x[vertex] > current_max_x)
            current_max_x = current_x[vertex];
        if (midpoint_x[vertex] < midpoint_min_x)
            midpoint_min_x = midpoint_x[vertex];
        if (midpoint_x[vertex] > midpoint_max_x)
            midpoint_max_x = midpoint_x[vertex];
    }
    event->surface_width = s_native_view_width;
    event->base_x = queued->base_x;
    event->slot = queued->slot;
    event->current_edge_distance = current_min_x <
            s_native_view_width - 1 - current_max_x
        ? current_min_x : s_native_view_width - 1 - current_max_x;
    event->midpoint_edge_distance = midpoint_min_x <
            s_native_view_width - 1 - midpoint_max_x
        ? midpoint_min_x : s_native_view_width - 1 - midpoint_max_x;
}

static void native_host_record_workload_retired_issues(void) {
    const size_t total = gpu_semantic_workload_retired_issues(
        s_retired_issue_scratch, RETIRED_ISSUE_SCRATCH_CAP);
    const size_t count = total < RETIRED_ISSUE_SCRATCH_CAP
        ? total : RETIRED_ISSUE_SCRATCH_CAP;

    if (total > RETIRED_ISSUE_SCRATCH_CAP)
        s_retired_failure_event_overflow +=
            total - RETIRED_ISSUE_SCRATCH_CAP;
    for (size_t index = 0u; index < count; ++index) {
        const GpuSemanticWorkloadRetiredIssue *issue =
            &s_retired_issue_scratch[index];
        GpuRenderSemantic semantic = {0};

        semantic.interpolation_identity = (GpuRenderInterpolationIdentity){
            .scene_id = issue->scene_id,
            .producer_id = issue->producer_id,
            .primitive_id = issue->primitive_id,
            .valid = 1u,
        };
        native_host_record_retired_failure(
            (GlRendererRetiredFailureReason)issue->reason,
            &semantic, issue->previous_order, issue->group_id,
            issue->vertex_id, 0u, 0, 0);
    }
}

static int native_host_retired_context(
        const NativeHostSemanticHistory *history,
        const NativeHostSemanticContextHistory *previous_context_history,
        size_t previous_context_history_count,
        const NativeHostSemanticContextHistory *current_context_history,
        size_t current_context_history_count,
        const GpuRenderSemantic *semantic, size_t previous_order,
        size_t current_offset, size_t current_count,
        int *out_base_x, int *out_slot) {
    const uint32_t history_generation =
        s_native_host_semantic_history_generation[
            s_native_host_semantic_history_index];
    int found = 0;

    for (size_t index = 0u; index < current_context_history_count; ++index) {
        const NativeHostSemanticContextHistory *candidate =
            &current_context_history[index];

        if (candidate->identity.scene_id !=
                semantic->interpolation_identity.scene_id ||
            candidate->identity.producer_id !=
                semantic->interpolation_identity.producer_id)
            continue;
        if (!found) {
            *out_base_x = candidate->base_x;
            *out_slot = candidate->slot;
            found = 1;
        } else if (*out_base_x != candidate->base_x ||
                   *out_slot != candidate->slot) {
            return 0;
        }
    }
    if (found) return 2;
    if (s_native_host_semantic_history_valid &&
        previous_order < NATIVE_HOST_QUEUE_CAP &&
        history[previous_order].generation ==
            history_generation &&
        history[previous_order].semantic.interpolation_identity.scene_id ==
            semantic->interpolation_identity.scene_id &&
        history[previous_order].semantic.interpolation_identity.producer_id ==
            semantic->interpolation_identity.producer_id &&
        history[previous_order].semantic.interpolation_identity.primitive_id ==
            semantic->interpolation_identity.primitive_id) {
        *out_base_x = history[previous_order].base_x;
        *out_slot = history[previous_order].slot;
        return 1;
    }
    for (size_t index = 0u; index < previous_context_history_count; ++index) {
        const NativeHostSemanticContextHistory *candidate =
            &previous_context_history[index];

        if (candidate->identity.scene_id !=
                semantic->interpolation_identity.scene_id ||
            candidate->identity.producer_id !=
                semantic->interpolation_identity.producer_id ||
            candidate->identity.primitive_id !=
                semantic->interpolation_identity.primitive_id)
            continue;
        if (!found) {
            *out_base_x = candidate->base_x;
            *out_slot = candidate->slot;
            found = 1;
        } else if (*out_base_x != candidate->base_x ||
                   *out_slot != candidate->slot) {
            found = 0;
            break;
        }
    }
    if (found) return 1;
    const int terrain = native_host_semantic_is_terrain(semantic);
    const int world_model = native_host_semantic_is_world_model(semantic);

    if (!terrain && !world_model) return 0;
    found = 0;
    for (size_t current_index = 0u; current_index < current_count;
         ++current_index) {
        const size_t queue_index = current_offset + current_index;
        const NativeHostQueuedSemantic *queued =
            &s_native_host_queue[queue_index];
        const GpuRenderSemantic *current = &queued->current;

        if (queued->clear_margins || queued->phase_only ||
            !current->interpolation_identity.valid ||
            current->interpolation_identity.scene_id !=
                semantic->interpolation_identity.scene_id ||
            current->interpolation_identity.producer_id !=
                semantic->interpolation_identity.producer_id)
            continue;
        if (!found) {
            *out_base_x = queued->base_x;
            *out_slot = queued->slot;
            found = 1;
        } else if (*out_base_x != queued->base_x ||
                   *out_slot != queued->slot) {
            return 0;
        }
    }
    if (found) return 2;
    if (!world_model) return 0;
    for (size_t current_index = 0u; current_index < current_count;
         ++current_index) {
        const size_t queue_index = current_offset + current_index;
        const NativeHostQueuedSemantic *queued =
            &s_native_host_queue[queue_index];
        const GpuRenderSemantic *current = &queued->current;

        if (queued->clear_margins || queued->phase_only ||
            !current->interpolation_identity.valid ||
            current->interpolation_identity.scene_id !=
                semantic->interpolation_identity.scene_id ||
            !native_host_semantic_is_world_model(current))
            continue;
        if (!found) {
            *out_base_x = queued->base_x;
            *out_slot = queued->slot;
            found = 1;
        } else if (*out_base_x != queued->base_x ||
                   *out_slot != queued->slot) {
            return 0;
        }
    }
    return found ? 3 : 0;
}

static GpuRenderTransactionStatus native_host_queue_insert_retired(
        size_t *in_out_count) {
    const size_t retired_count = gpu_semantic_workload_retired_count();
    GpuSemanticWorkloadRetiredDiagnostics terrain = {0};
    const size_t current_count = *in_out_count;
    const size_t available = NATIVE_HOST_QUEUE_CAP - current_count;
    NativeHostSemanticHistory *history =
        s_native_host_semantic_history[
            s_native_host_semantic_history_index];
    const NativeHostSemanticContextHistory *context_history =
        s_native_host_semantic_context_history[
            s_native_host_semantic_history_index];
    const size_t context_history_count =
        s_native_host_semantic_context_history_count[
            s_native_host_semantic_history_index];
    const NativeHostSemanticContextHistory *current_context_history =
        s_native_host_semantic_context_history[
            s_native_host_semantic_history_index ^ 1u];
    const size_t current_context_history_count =
        s_native_host_semantic_context_history_count[
            s_native_host_semantic_history_index ^ 1u];
    size_t current_positions[GPU_SEMANTIC_WORKLOAD_CAPACITY];
    int retired_base_x[GPU_SEMANTIC_WORKLOAD_CAPACITY];
    int retired_slot[GPU_SEMANTIC_WORKLOAD_CAPACITY];
    size_t insert_count = 0u;

    for (size_t retired_index = 0u; retired_index < retired_count;
         ++retired_index)
        current_positions[retired_index] = SIZE_MAX;
    gpu_semantic_workload_retired_diagnostics(
        UINT32_C(0x8009932c), &terrain);
    native_host_record_workload_retired_issues();
    s_native_midpoint_diag.retired_terrain_unmatched_count +=
        terrain.unmatched;
    s_native_midpoint_diag.retired_terrain_eligible_count +=
        terrain.eligible;
    s_native_midpoint_diag.retired_terrain_missing_current_geometry_count +=
        terrain.unmatched - terrain.eligible;
    s_native_midpoint_diag.retired_terrain_missing_anchor_count +=
        terrain.missing_anchor;
    s_native_midpoint_diag.retired_terrain_scene_mismatch_count +=
        terrain.scene_mismatch;
    s_native_midpoint_diag.retired_terrain_position_mode_mismatch_count +=
        terrain.position_mode_mismatch;
    s_native_midpoint_diag.retired_terrain_material_position_mismatch_count +=
        terrain.material_position_mismatch;
    s_native_midpoint_diag.retired_terrain_anchor_overflow_count +=
        terrain.anchor_overflow;
    if (terrain.missing_anchor != 0u &&
        s_native_midpoint_diag.retired_terrain_missing_anchor_count ==
            terrain.missing_anchor) {
        s_native_midpoint_diag.first_retired_terrain_missing_primitive =
            terrain.first_missing_primitive_id;
        s_native_midpoint_diag.first_retired_terrain_missing_group =
            terrain.first_missing_group_id;
        s_native_midpoint_diag.first_retired_terrain_missing_vertex =
            terrain.first_missing_vertex_id;
    }
    if (retired_count == 0u) return GPU_RENDER_TRANSACTION_OK;
    s_native_midpoint_diag.retired_candidate_count += retired_count;
    for (size_t retired_index = 0u; retired_index < retired_count;
         ++retired_index) {
        GpuRenderSemantic semantic;
        size_t previous_order;
        int world_model;
        int terrain;
        int base_x;
        int slot;
        int context_kind;

        if (gpu_semantic_workload_retired(
                retired_index, &semantic, &previous_order) !=
                    GPU_SEMANTIC_WORKLOAD_OK)
            continue;
        world_model = native_host_semantic_is_world_model(&semantic);
        terrain = native_host_semantic_is_terrain(&semantic);
        if (world_model)
            ++s_native_midpoint_diag.retired_world_model_candidate_count;
        if (terrain)
            ++s_native_midpoint_diag.retired_terrain_candidate_count;
        /* Terrain authors its own depth-cull candidates. Generic retirement
         * rebuilds every missing triangle from anchors, including topology
         * changes, which opens cracks between otherwise continuous tiles. */
        if (terrain) continue;
        context_kind = native_host_retired_context(
            history, context_history, context_history_count,
            current_context_history, current_context_history_count,
            &semantic, previous_order, 0u, current_count,
            &base_x, &slot);
        if (context_kind == 0) {
            ++s_native_midpoint_diag.retired_history_miss_count;
            s_native_midpoint_diag.last_retired_history_miss_producer =
                semantic.interpolation_identity.producer_id;
            s_native_midpoint_diag.last_retired_history_miss_primitive =
                semantic.interpolation_identity.primitive_id;
            if (world_model)
                ++s_native_midpoint_diag.retired_world_model_history_miss_count;
            if (terrain)
                ++s_native_midpoint_diag.retired_terrain_history_miss_count;
            native_host_record_retired_failure(
                GL_RETIRED_FAILURE_HISTORY_MISS, &semantic, previous_order,
                0u, 0u, 0u, 0, 0);
            continue;
        }
        if (context_kind == 2) {
            ++s_native_midpoint_diag.retired_producer_history_recovery_count;
            if (terrain)
                ++s_native_midpoint_diag
                    .retired_terrain_history_recovery_count;
            if (world_model) {
                ++s_native_midpoint_diag
                    .retired_world_model_history_recovery_count;
                ++s_native_midpoint_diag
                    .retired_world_model_producer_context_recovery_count;
            }
        } else if (context_kind == 3) {
            ++s_native_midpoint_diag.retired_producer_history_recovery_count;
            if (world_model) {
                ++s_native_midpoint_diag
                    .retired_world_model_history_recovery_count;
                ++s_native_midpoint_diag
                    .retired_world_model_class_context_recovery_count;
            }
        }
        if (insert_count == available) {
            ++s_native_midpoint_diag.retired_capacity_miss_count;
            native_host_record_retired_failure(
                GL_RETIRED_FAILURE_CAPACITY, &semantic, previous_order,
                0u, 0u, 0u, 0, 0);
            continue;
        }
        current_positions[retired_index] = 0u;
        retired_base_x[retired_index] = base_x;
        retired_slot[retired_index] = slot;
        for (size_t current_index = 0u; current_index < current_count;
             ++current_index) {
            const NativeHostQueuedSemantic *queued =
                &s_native_host_queue[current_index];
            const GpuRenderSemantic *current = &queued->current;
            GpuSemanticWorkloadMatchInfo current_match = {0};

            if (queued->clear_margins) {
                current_positions[retired_index] = current_index + 1u;
                continue;
            }
            if (!current->interpolation_identity.valid ||
                gpu_semantic_workload_match_info(
                    &current->interpolation_identity, &current_match) !=
                        GPU_SEMANTIC_WORKLOAD_OK ||
                !current_match.previous_order_valid ||
                current_match.previous_order >= previous_order)
                continue;
            current_positions[retired_index] = current_index + 1u;
        }
        ++insert_count;
    }
    if (insert_count == 0u) return GPU_RENDER_TRANSACTION_OK;
    memmove(&s_native_host_queue[insert_count], &s_native_host_queue[0],
            current_count * sizeof(s_native_host_queue[0]));
    size_t current_index = 0u;
    size_t retired_index = 0u;
    size_t inserted = 0u;
    while (current_index <= current_count) {
        while (retired_index < retired_count) {
            GpuRenderSemantic semantic;
            NativeHostQueuedSemantic queued;
            size_t previous_order;

            if (current_positions[retired_index] == SIZE_MAX) {
                ++retired_index;
                continue;
            }
            if (current_positions[retired_index] > current_index) break;
            if (gpu_semantic_workload_retired(
                    retired_index, &semantic, &previous_order) !=
                        GPU_SEMANTIC_WORKLOAD_OK) {
                ++retired_index;
                continue;
            }
            queued = (NativeHostQueuedSemantic){
                .current = semantic,
                .base_x = retired_base_x[retired_index],
                .slot = retired_slot[retired_index],
                .midpoint_valid = 1,
                .phase_only = 1,
                .phase_visibility_mask = UINT8_MAX,
            };
            if (gpu_semantic_workload_retired_phases(
                    retired_index, s_native_interpolation_phase_count + 1u,
                    &queued.midpoint, s_native_interpolation_phase_count,
                    &previous_order) != GPU_SEMANTIC_WORKLOAD_OK) {
                ++s_native_midpoint_diag.retired_phase_failure_count;
                s_native_midpoint_diag.last_retired_phase_failure_producer =
                    semantic.interpolation_identity.producer_id;
                s_native_midpoint_diag.last_retired_phase_failure_primitive =
                    semantic.interpolation_identity.primitive_id;
                native_host_record_retired_failure(
                    GL_RETIRED_FAILURE_PHASE, &semantic, previous_order,
                    0u, 0u, 0u, 0, 0);
                ++retired_index;
                continue;
            }
            s_native_host_queue[current_index + inserted] = queued;
            if (native_host_semantic_is_world_model(&semantic))
                ++s_native_midpoint_diag.retired_world_model_inserted_count;
            if (native_host_semantic_is_terrain(&semantic))
                ++s_native_midpoint_diag.retired_terrain_inserted_count;
            ++inserted;
            ++retired_index;
        }
        if (current_index < current_count)
            s_native_host_queue[current_index + inserted] =
                s_native_host_queue[insert_count + current_index];
        ++current_index;
    }
    *in_out_count = current_count + inserted;
    s_native_midpoint_diag.retired_inserted_count += inserted;
    return GPU_RENDER_TRANSACTION_OK;
}

static GpuRenderTransactionStatus native_host_queue_flush(void) {
    GpuRenderTransactionStatus status = GPU_RENDER_TRANSACTION_OK;
    size_t count;

    if (s_native_host_queue_flushing || s_native_host_queue_count == 0u)
        return GPU_RENDER_TRANSACTION_OK;
    count = s_native_host_queue_count;
    s_native_host_queue_flushing = 1;
    flush_flat_batch();
    flush_tex_batch();
    status = native_host_queue_render_pass(count, -1);
    if (status == GPU_RENDER_TRANSACTION_OK &&
        !s_native_host_queue_midpoint_rendered)
        for (unsigned int phase = 0u;
             phase < s_native_interpolation_phase_count &&
             status == GPU_RENDER_TRANSACTION_OK; ++phase)
            status = native_host_queue_render_pass(count, (int)phase);
    if (s_native_host_queue_midpoint_rendered)
        s_native_midpoint_diag.deferred_current_flushes++;
    if (status == GPU_RENDER_TRANSACTION_OK) {
        s_native_host_queue_count = 0u;
        s_native_host_queue_midpoint_rendered = 0;
    }
    s_native_host_queue_flushing = 0;
    if (status != GPU_RENDER_TRANSACTION_OK)
        gl_renderer_native_midpoint_cancel();
    return status;
}

static GpuRenderTransactionStatus native_host_queue_prepare_present(
        int use_midpoint) {
    GpuRenderTransactionStatus status = GPU_RENDER_TRANSACTION_OK;
    size_t count = s_native_host_queue_count;
    const unsigned int next_history_index =
        s_native_host_semantic_history_index ^ 1u;

    if (s_native_host_queue_flushing || count == 0u)
        return GPU_RENDER_TRANSACTION_OK;
    s_native_host_queue_last_present_count = count;
    native_host_queue_capture_history(count, next_history_index);
    if (use_midpoint) {
        status = native_host_queue_insert_retired(&count);
        if (status != GPU_RENDER_TRANSACTION_OK) {
            gl_renderer_native_midpoint_cancel();
            return status;
        }
        s_native_host_queue_count = count;
        if (s_native_interpolation_phase_count == 1u)
            native_host_queue_merge_temporal_phases(count);
    }
    native_host_queue_snapshot_present(count);
    s_native_host_semantic_history_index = next_history_index;
    s_native_host_semantic_history_valid = 1;
    s_native_host_queue_flushing = 1;
    flush_flat_batch();
    flush_tex_batch();
    if (s_native_host_queue_midpoint_rendered) {
        status = native_host_queue_render_pass(count, -1);
        s_native_midpoint_diag.deferred_current_flushes++;
        s_native_host_queue_count = 0u;
        s_native_host_queue_midpoint_rendered = 0;
    } else if (use_midpoint) {
        for (unsigned int phase = 0u;
             phase < s_native_interpolation_phase_count &&
             status == GPU_RENDER_TRANSACTION_OK; ++phase)
            status = native_host_queue_render_pass(count, (int)phase);
        if (status == GPU_RENDER_TRANSACTION_OK) {
            s_native_host_queue_midpoint_rendered = 1;
            s_native_midpoint_diag.deferred_current_frames++;
        }
    } else {
        status = native_host_queue_render_pass(count, -1);
        s_native_host_queue_count = 0u;
    }
    s_native_host_queue_flushing = 0;
    if (status != GPU_RENDER_TRANSACTION_OK)
        gl_renderer_native_midpoint_cancel();
    return status;
}

static GpuRenderTransactionStatus native_host_pending_flush(void) {
    return native_host_queue_flush();
}

static GpuRenderTransactionStatus native_host_pending_flush_reason(
        unsigned int reason) {
    if (reason < 9u &&
        (s_native_host_queue_count != 0u ||
         s_native_host_queue_midpoint_rendered))
        s_native_midpoint_diag.host_queue_flush_reasons[reason]++;
    return native_host_pending_flush();
}

static GpuRenderTransactionStatus native_host_queue_push(
        const GpuRenderSemantic *semantic,
        const GpuRenderSemantic *phase_semantics,
        int base_x, int slot) {
    NativeHostQueuedSemantic *queued;

    if (semantic == NULL || slot < 0 || slot >= NATIVE_VIEW_MAX_SURF)
        return GPU_RENDER_TRANSACTION_INVALID_ARGUMENT;
    if (s_native_host_queue_midpoint_rendered) {
        GpuRenderTransactionStatus status = native_host_queue_flush();
        if (status != GPU_RENDER_TRANSACTION_OK) return status;
        if (!s_native_midpoint_diag.frame_open &&
            !s_native_midpoint_diag.suspended &&
            !s_native_midpoint_frame_blocked &&
            !gl_renderer_native_midpoint_begin())
            return GPU_RENDER_TRANSACTION_INVALID_TRANSITION;
    }
    if (s_native_host_queue_count == NATIVE_HOST_QUEUE_CAP) {
        GpuRenderTransactionStatus status = native_host_queue_flush();
        if (status != GPU_RENDER_TRANSACTION_OK) return status;
    }
    queued = &s_native_host_queue[s_native_host_queue_count++];
    queued->current = *semantic;
    queued->midpoint = phase_semantics != NULL
        ? phase_semantics[0] : *semantic;
    for (unsigned int phase = 1u;
         phase < s_native_interpolation_phase_count; ++phase)
        queued->extra_phases[phase - 1u] = phase_semantics != NULL
            ? phase_semantics[phase] : *semantic;
    queued->base_x = base_x;
    queued->slot = slot;
    queued->clear_margins = 0;
    queued->midpoint_valid = phase_semantics != NULL;
    queued->phase_only = 0;
    queued->temporal_order_valid = 0;
    queued->phase_visibility_mask = 0u;
    memset(queued->phase_order, 0, sizeof(queued->phase_order));
    if (s_native_midpoint_diag.frame_open &&
        semantic->interpolation_identity.valid) {
        const unsigned int history_index =
            s_native_host_semantic_history_index ^ 1u;
        size_t *count =
            &s_native_host_semantic_context_history_count[history_index];

        if (*count < NATIVE_HOST_QUEUE_CAP) {
            s_native_host_semantic_context_history[history_index][(*count)++] =
                (NativeHostSemanticContextHistory){
                    .identity = semantic->interpolation_identity,
                    .base_x = base_x,
                    .slot = slot,
                };
        }
    }
    return GPU_RENDER_TRANSACTION_OK;
}

static GpuRenderTransactionStatus native_host_queue_push_margin_clear(
        int slot, int y, int h, uint16_t color, int midpoint_valid) {
    NativeHostQueuedSemantic *queued;

    if (slot < 0 || slot >= NATIVE_VIEW_MAX_SURF || h <= 0)
        return GPU_RENDER_TRANSACTION_INVALID_ARGUMENT;
    if (s_native_host_queue_midpoint_rendered) {
        GpuRenderTransactionStatus status = native_host_queue_flush();
        if (status != GPU_RENDER_TRANSACTION_OK) return status;
    }
    if (s_native_host_queue_count == NATIVE_HOST_QUEUE_CAP) {
        GpuRenderTransactionStatus status = native_host_queue_flush();
        if (status != GPU_RENDER_TRANSACTION_OK) return status;
    }
    queued = &s_native_host_queue[s_native_host_queue_count++];
    queued->base_x = s_native_view_base[slot];
    queued->slot = slot;
    queued->clear_y = y;
    queued->clear_h = h;
    queued->clear_color = color;
    queued->clear_margins = 1;
    queued->midpoint_valid = midpoint_valid;
    queued->phase_only = 0;
    queued->temporal_order_valid = 0;
    queued->phase_visibility_mask = 0u;
    memset(queued->phase_order, 0, sizeof(queued->phase_order));
    return GPU_RENDER_TRANSACTION_OK;
}

static size_t producer_diag_semantic_vertex_count(
        const GpuRenderSemantic *semantic) {
    return semantic->topology == GPU_RENDER_SEMANTIC_TRIANGLES
        ? (size_t)semantic->triangle_count * 3u
        : (size_t)semantic->line_count * 2u;
}

static const GpuRenderSemanticVertex *producer_diag_semantic_vertex(
        const GpuRenderSemantic *semantic, size_t index) {
    return semantic->topology == GPU_RENDER_SEMANTIC_TRIANGLES
        ? &semantic->triangles[index / 3u].vertices[index % 3u]
        : &semantic->lines[index / 2u].vertices[index % 2u];
}

static int64_t producer_diag_fixed_floor(int64_t value) {
    if (value >= 0) return value / INT64_C(65536);
    return -((-value + INT64_C(65535)) / INT64_C(65536));
}

void gl_renderer_semantic_producer_diag(
        uint32_t producer_id,
        GlRendererSemanticProducerDiagnostics *out_diagnostics) {
    GlRendererSemanticProducerDiagnostics diagnostics = {
        .producer_id = producer_id,
    };
    GpuSemanticWorkloadRetiredDiagnostics retired = {0};

    if (out_diagnostics == NULL) return;
    gpu_semantic_workload_retired_diagnostics(
        producer_id, &retired);
    diagnostics.retired_unmatched = retired.unmatched;
    diagnostics.retired_candidates = retired.eligible;
    diagnostics.retired_missing_current_geometry =
        retired.unmatched - retired.eligible;
    memset(s_producer_diag_vertices, 0, sizeof(s_producer_diag_vertices));
    const size_t queue_count = s_native_host_queue_count != 0u
        ? s_native_host_queue_count : s_native_host_queue_last_present_count;
    for (size_t queue_index = 0u; queue_index < queue_count;
         ++queue_index) {
        const NativeHostQueuedSemantic *queued =
            &s_native_host_queue[queue_index];
        const GpuRenderSemantic *current = &queued->current;
        const GpuRenderSemantic *midpoint = &queued->midpoint;
        const size_t vertex_count =
            producer_diag_semantic_vertex_count(current);
        const size_t vertices_per_primitive =
            current->topology == GPU_RENDER_SEMANTIC_TRIANGLES ? 3u : 2u;
        size_t moving_vertices = 0u;
        uint64_t semantic_delta = 0u;

        if (queued->clear_margins ||
            !current->interpolation_identity.valid ||
            current->interpolation_identity.producer_id != producer_id)
            continue;
        if (queued->phase_only) {
            ++diagnostics.retired_inserted;
            continue;
        }
        ++diagnostics.semantic_count;
        {
            size_t previous_order;

            if (gpu_semantic_workload_previous_order(
                    &current->interpolation_identity, &previous_order) ==
                GPU_SEMANTIC_WORKLOAD_OK)
                s_producer_diag_previous_order[
                    diagnostics.matched_order_count++] = previous_order;
        }
        if (!queued->midpoint_valid) continue;
        ++diagnostics.midpoint_semantic_count;
        diagnostics.primitive_count += vertex_count / vertices_per_primitive;
        diagnostics.vertex_count += vertex_count;
        for (size_t vertex_index = 0u; vertex_index < vertex_count;
             ++vertex_index) {
            const GpuRenderSemanticVertex *current_vertex =
                producer_diag_semantic_vertex(current, vertex_index);
            const GpuRenderSemanticVertex *midpoint_vertex =
                producer_diag_semantic_vertex(midpoint, vertex_index);
            const int64_t current_x =
                (current_vertex->native_view_position
                     ? current_vertex->native_view_x : current_vertex->x) +
                (int64_t)current->material.draw_offset_x * INT64_C(65536);
            const int64_t current_y =
                (current_vertex->native_view_position
                     ? current_vertex->native_view_y : current_vertex->y) +
                (int64_t)current->material.draw_offset_y * INT64_C(65536);
            const int64_t midpoint_x =
                (midpoint_vertex->native_view_position
                     ? midpoint_vertex->native_view_x : midpoint_vertex->x) +
                (int64_t)midpoint->material.draw_offset_x * INT64_C(65536);
            const int64_t midpoint_y =
                (midpoint_vertex->native_view_position
                     ? midpoint_vertex->native_view_y : midpoint_vertex->y) +
                (int64_t)midpoint->material.draw_offset_y * INT64_C(65536);
            size_t slot;

            semantic_delta += current_x >= midpoint_x
                ? (uint64_t)(current_x - midpoint_x)
                : (uint64_t)(midpoint_x - current_x);
            semantic_delta += current_y >= midpoint_y
                ? (uint64_t)(current_y - midpoint_y)
                : (uint64_t)(midpoint_y - current_y);
            if (current_x != midpoint_x || current_y != midpoint_y)
                ++moving_vertices;
            if (vertex_index % vertices_per_primitive ==
                    vertices_per_primitive - 1u) {
                if (moving_vertices == 0u)
                    ++diagnostics.static_primitive_count;
                else if (moving_vertices == vertices_per_primitive)
                    ++diagnostics.fully_moving_primitive_count;
                else
                    ++diagnostics.partially_moving_primitive_count;
                moving_vertices = 0u;
            }
            if (!midpoint_vertex->interpolation_vertex_identity_valid)
                continue;
            slot = ((size_t)midpoint->interpolation_identity.scene_id ^
                    ((size_t)midpoint_vertex->interpolation_group_id << 7u) ^
                    midpoint_vertex->interpolation_vertex_id) &
                (PRODUCER_DIAG_VERTEX_CAP - 1u);
            for (size_t probe = 0u; probe < PRODUCER_DIAG_VERTEX_CAP;
                 ++probe) {
                ProducerDiagVertex *entry = &s_producer_diag_vertices[slot];

                if (!entry->used) {
                    *entry = (ProducerDiagVertex){
                        .scene_id = midpoint->interpolation_identity.scene_id,
                        .group_id = midpoint_vertex->interpolation_group_id,
                        .vertex_id = midpoint_vertex->interpolation_vertex_id,
                        .x = midpoint_x,
                        .y = midpoint_y,
                        .used = 1,
                    };
                    break;
                }
                if (entry->scene_id ==
                        midpoint->interpolation_identity.scene_id &&
                    entry->group_id ==
                        midpoint_vertex->interpolation_group_id &&
                    entry->vertex_id ==
                        midpoint_vertex->interpolation_vertex_id) {
                    ++diagnostics.duplicate_vertex_count;
                    if (entry->x != midpoint_x || entry->y != midpoint_y)
                        ++diagnostics.exact_vertex_conflict_count;
                    if (producer_diag_fixed_floor(entry->x) !=
                            producer_diag_fixed_floor(midpoint_x) ||
                        producer_diag_fixed_floor(entry->y) !=
                            producer_diag_fixed_floor(midpoint_y))
                        ++diagnostics.raster_vertex_conflict_count;
                    break;
                }
                slot = (slot + 1u) & (PRODUCER_DIAG_VERTEX_CAP - 1u);
            }
        }
        if (semantic_delta > diagnostics.max_midpoint_delta_fixed) {
            diagnostics.max_midpoint_delta_fixed = semantic_delta;
            diagnostics.max_midpoint_primitive_id =
                current->interpolation_identity.primitive_id;
        }
    }
    for (size_t right = 1u; right < diagnostics.matched_order_count; ++right)
        for (size_t left = 0u; left < right; ++left)
            if (s_producer_diag_previous_order[left] >
                    s_producer_diag_previous_order[right]) {
                const uint64_t regression =
                    s_producer_diag_previous_order[left] -
                    s_producer_diag_previous_order[right];

                ++diagnostics.previous_order_inversion_count;
                if (regression > diagnostics.max_previous_order_regression)
                    diagnostics.max_previous_order_regression = regression;
            }
    *out_diagnostics = diagnostics;
}

size_t gl_renderer_semantic_producer_items(
        uint32_t producer_id, uint64_t frame, size_t offset,
        GlRendererSemanticProducerItemDiagnostics *out_items, size_t capacity,
        size_t *out_total, uint64_t *out_frame) {
    const uint64_t available = s_native_host_diag_primitive_total <
            NATIVE_HOST_DIAG_PRIMITIVE_CAP
        ? s_native_host_diag_primitive_total
        : NATIVE_HOST_DIAG_PRIMITIVE_CAP;
    const uint64_t first = s_native_host_diag_primitive_total - available;
    uint64_t selected_frame = frame;
    size_t total = 0u;
    size_t emitted = 0u;

    if (selected_frame == UINT64_MAX) {
        for (uint64_t sequence = s_native_host_diag_primitive_total;
             sequence > first; --sequence) {
            const GlRendererSemanticProducerItemDiagnostics *item =
                &s_native_host_diag_primitives[
                    (sequence - 1u) % NATIVE_HOST_DIAG_PRIMITIVE_CAP];

            if (producer_id == UINT32_MAX ||
                item->producer_id == producer_id) {
                selected_frame = item->frame;
                break;
            }
        }
    }
    for (uint64_t sequence = first;
         sequence < s_native_host_diag_primitive_total; ++sequence) {
        const GlRendererSemanticProducerItemDiagnostics *item =
            &s_native_host_diag_primitives[
                sequence % NATIVE_HOST_DIAG_PRIMITIVE_CAP];

        if ((producer_id != UINT32_MAX &&
             item->producer_id != producer_id) ||
            item->frame != selected_frame)
            continue;
        if (total++ < offset) continue;
        if (out_items != NULL && emitted < capacity)
            out_items[emitted++] = *item;
    }
    if (out_total != NULL) *out_total = total;
    if (out_frame != NULL) *out_frame = selected_frame;
    return emitted;
}

size_t gl_renderer_retired_failure_events(
        GlRendererRetiredFailureEvent *out_events, size_t capacity) {
    const size_t stored = s_retired_failure_event_count <
            RETIRED_FAILURE_EVENT_CAP
        ? (size_t)s_retired_failure_event_count
        : RETIRED_FAILURE_EVENT_CAP;
    const size_t count = capacity < stored ? capacity : stored;

    if (out_events == NULL && capacity != 0u) return 0u;
    if (count != 0u)
        memcpy(out_events, s_retired_failure_events,
               count * sizeof(*out_events));
    return count;
}

uint64_t gl_renderer_retired_failure_event_total(void) {
    return s_retired_failure_event_count;
}

uint64_t gl_renderer_retired_failure_event_overflow(void) {
    return s_retired_failure_event_overflow;
}

static GpuRenderTransactionStatus glb_draw_semantic(
        GpuRenderTransactionId transaction_id,
        const GpuRenderSemantic *semantic) {
    if (!s_transaction) return GPU_RENDER_TRANSACTION_INVALID_TRANSITION;
    if (!glb_transaction_id_equal(s_transaction->id, transaction_id))
        return GPU_RENDER_TRANSACTION_STATE_REJECTED;
    if (!glb_transaction_context_ready())
        return GPU_RENDER_TRANSACTION_CONTEXT_LOST;
    if (!glb_transaction_interpolation_quiesced())
        return GPU_RENDER_TRANSACTION_STATE_REJECTED;
    return glb_draw_semantic_contents(semantic, 0, 0);
}

static GpuRenderTransactionStatus glb_stream_barrier(void) {
    if (!glb_transaction_context_ready())
        return GPU_RENDER_TRANSACTION_CONTEXT_LOST;
    if (!glb_transaction_interpolation_quiesced())
        return GPU_RENDER_TRANSACTION_STATE_REJECTED;
    return glb_transaction_consume_gl_errors()
        ? GPU_RENDER_TRANSACTION_BACKEND_ERROR
        : GPU_RENDER_TRANSACTION_OK;
}

static GpuRenderTransactionStatus glb_draw_semantic_immediate(
        const GpuRenderSemantic *semantic) {
    GpuRenderTransactionStatus status = GPU_RENDER_TRANSACTION_OK;
    GpuRenderSemantic phase_semantics[NATIVE_INTERPOLATION_MAX_PHASES];
    GpuSemanticWorkloadStatus workload_status =
        GPU_SEMANTIC_WORKLOAD_INVALID_TRANSITION;
    int base_x;
    int slot;

    if (!glb_transaction_context_ready())
        return GPU_RENDER_TRANSACTION_CONTEXT_LOST;
    if (!glb_transaction_interpolation_quiesced())
        return GPU_RENDER_TRANSACTION_STATE_REJECTED;
    slot = -1;
    base_x = 0;
    if (!s_native_midpoint_diag.frame_open &&
        !s_native_midpoint_diag.suspended &&
        !s_native_midpoint_frame_blocked)
        (void)gl_renderer_native_midpoint_begin();
    flush_cpu_upload();
    if (semantic->material.mask_check && !s_stencil_valid) {
        if (native_host_pending_flush_reason(6u) !=
            GPU_RENDER_TRANSACTION_OK)
            return GPU_RENDER_TRANSACTION_BACKEND_ERROR;
        rebuild_mask_stencils();
    }
    if (s_native_midpoint_diag.frame_open) {
        workload_status = gpu_semantic_workload_record_phases(
            semantic, s_native_interpolation_denominator,
            phase_semantics, s_native_interpolation_phase_count);
        if (workload_status == GPU_SEMANTIC_WORKLOAD_OK) {
            native_geometry_accumulate(
                NATIVE_CURRENT_VARIANT, semantic);
            for (unsigned int phase = 0u;
                 phase < s_native_interpolation_phase_count; ++phase)
                native_geometry_accumulate(phase, &phase_semantics[phase]);
        } else {
            native_midpoint_cancel_with_reason(
                GL_NATIVE_MIDPOINT_CANCEL_WORKLOAD_RECORD,
                (uint32_t)workload_status, semantic);
        }
    }
    if (s_native_view_enabled) {
        base_x = glb_native_view_semantic_target_base(semantic);
        slot = native_view_prepare_surface(base_x);
        if (slot < 0) {
            gl_renderer_native_midpoint_cancel();
            return GPU_RENDER_TRANSACTION_BACKEND_ERROR;
        }
    }
    status = glb_draw_semantic_contents(semantic, 1, 0);
    if (status != GPU_RENDER_TRANSACTION_OK) {
        gl_renderer_native_midpoint_cancel();
        return status;
    }
    /* Canonical guest VRAM remains in submission order. Host-only Native
     * surfaces queue until a VRAM coherency boundary, preserving the texture
     * snapshot while allowing current and midpoint passes to batch. */
    if (s_native_midpoint_diag.frame_open &&
        s_native_midpoint_diag.frame_valid &&
        !s_native_midpoint_current_pending &&
        s_native_midpoint_canonical_enabled &&
        workload_status == GPU_SEMANTIC_WORKLOAD_OK) {
        flush_flat_batch();
        flush_tex_batch();
        for (unsigned int phase = 0u;
             phase < s_native_interpolation_phase_count &&
             status == GPU_RENDER_TRANSACTION_OK; ++phase)
            status = glb_draw_semantic_phase_contents(
                &phase_semantics[phase], phase);
        if (status != GPU_RENDER_TRANSACTION_OK) {
            gl_renderer_native_midpoint_cancel();
            status = GPU_RENDER_TRANSACTION_OK;
        }
    }
    if (!s_native_view_enabled)
        return GPU_RENDER_TRANSACTION_OK;
    native_view_wave_record(
        semantic,
        s_native_midpoint_diag.frame_open &&
                !s_native_midpoint_current_pending &&
                s_native_midpoint_diag.frame_valid &&
                workload_status == GPU_SEMANTIC_WORKLOAD_OK
            ? phase_semantics : NULL,
        base_x, slot);
    status = native_host_queue_push(
        semantic,
        s_native_midpoint_diag.frame_open &&
                !s_native_midpoint_current_pending &&
                s_native_midpoint_diag.frame_valid &&
                workload_status == GPU_SEMANTIC_WORKLOAD_OK
            ? phase_semantics : NULL,
        base_x, slot);
    if (status != GPU_RENDER_TRANSACTION_OK)
        gl_renderer_native_midpoint_cancel();
    return status;
}

static GpuRenderTransactionStatus glb_draw_semantic_temporal_candidate(
        const GpuRenderSemantic *semantic,
        const GpuRenderTemporalCullPolicy *policy) {
    GpuRenderSemantic phases[NATIVE_INTERPOLATION_MAX_PHASES];
    GpuRenderTransactionStatus status;
    GpuSemanticWorkloadStatus workload_status;
    uint8_t phase_visibility_mask = 0u;
    uint32_t phase_order[NATIVE_INTERPOLATION_MAX_PHASES] = {0};
    GpuSemanticWorkloadDiagnostics workload_diagnostics;
    GpuRenderSemantic previous;
    size_t workload_count_before;
    uint32_t previous_order;
    int generate_phases;
    int base_x;
    int slot;

    if (semantic == NULL || policy == NULL)
        return GPU_RENDER_TRANSACTION_INVALID_ARGUMENT;
    ++s_native_midpoint_diag.temporal_candidate_count;
    if (!glb_transaction_context_ready())
        return GPU_RENDER_TRANSACTION_CONTEXT_LOST;
    if (!glb_transaction_interpolation_quiesced())
        return GPU_RENDER_TRANSACTION_STATE_REJECTED;
    if (!s_native_view_enabled) return GPU_RENDER_TRANSACTION_OK;
    if (!s_native_midpoint_diag.frame_open &&
        !s_native_midpoint_diag.suspended &&
        !s_native_midpoint_frame_blocked &&
        !gl_renderer_native_midpoint_begin())
        return GPU_RENDER_TRANSACTION_INVALID_TRANSITION;
    if (!s_native_midpoint_diag.frame_open ||
        !s_native_midpoint_diag.frame_valid)
        return GPU_RENDER_TRANSACTION_OK;
    generate_phases =
        (policy->flags & GPU_RENDER_TEMPORAL_FORCE_PHASES) != 0u ||
        (gpu_semantic_workload_previous(
            &semantic->interpolation_identity, &previous) ==
                GPU_SEMANTIC_WORKLOAD_OK &&
         native_host_temporal_phase_visible(
            &previous, policy, &previous_order));
    gpu_semantic_workload_diagnostics(&workload_diagnostics);
    workload_count_before = workload_diagnostics.current_count;
    workload_status = generate_phases
        ? gpu_semantic_workload_record_phases(
            semantic, s_native_interpolation_denominator,
            phases, s_native_interpolation_phase_count)
        : gpu_semantic_workload_record_endpoint(semantic);
    gpu_semantic_workload_diagnostics(&workload_diagnostics);
    if (workload_diagnostics.current_count >
        s_native_midpoint_diag.temporal_candidate_peak_workload_count)
        s_native_midpoint_diag.temporal_candidate_peak_workload_count =
            workload_diagnostics.current_count;
    if (workload_status != GPU_SEMANTIC_WORKLOAD_OK) {
        if (workload_status == GPU_SEMANTIC_WORKLOAD_CONFLICT &&
            workload_diagnostics.current_count > workload_count_before) {
            ++s_native_midpoint_diag.temporal_candidate_duplicate_count;
            ++s_native_midpoint_diag
                  .temporal_candidate_identity_collision_count;
        }
        if (s_native_midpoint_diag.temporal_candidate_record_failure_count ==
            0u) {
            s_native_midpoint_diag.temporal_candidate_first_failure_status =
                (uint32_t)workload_status;
            s_native_midpoint_diag
                .temporal_candidate_first_failure_workload_count =
                    workload_diagnostics.current_count;
            s_native_midpoint_diag.temporal_candidate_first_failure_producer =
                semantic->interpolation_identity.producer_id;
            s_native_midpoint_diag.temporal_candidate_first_failure_primitive =
                semantic->interpolation_identity.primitive_id;
        }
        ++s_native_midpoint_diag.temporal_candidate_record_failure_count;
        return GPU_RENDER_TRANSACTION_OK;
    }
    ++s_native_midpoint_diag.temporal_candidate_recorded_count;
    if (!generate_phases) return GPU_RENDER_TRANSACTION_OK;
    for (unsigned int phase = 0u;
         phase < s_native_interpolation_phase_count; ++phase) {
        if (!native_host_temporal_phase_visible(
                &phases[phase], policy, &phase_order[phase]))
            continue;
        phase_visibility_mask |= (uint8_t)(1u << phase);
    }
    if (phase_visibility_mask == 0u)
        return GPU_RENDER_TRANSACTION_OK;
    ++s_native_midpoint_diag.temporal_candidate_visible_count;
    base_x = glb_native_view_semantic_target_base(semantic);
    slot = native_view_prepare_surface(base_x);
    if (slot < 0) return GPU_RENDER_TRANSACTION_BACKEND_ERROR;
    status = native_host_queue_push(semantic, phases, base_x, slot);
    if (status == GPU_RENDER_TRANSACTION_OK) {
        NativeHostQueuedSemantic *queued =
            &s_native_host_queue[s_native_host_queue_count - 1u];

        queued->phase_only = 1;
        queued->temporal_order_valid = 1;
        queued->phase_visibility_mask = phase_visibility_mask;
        memcpy(queued->phase_order, phase_order, sizeof(queued->phase_order));
    }
    return status;
}

static GpuRenderTransactionStatus glb_validate_present(
        const GpuRenderPresent *present) {
    int64_t right, bottom;
    uint32_t expected_width, expected_height;

    if (!present) return GPU_RENDER_TRANSACTION_INVALID_ARGUMENT;
    if (present->path == GPU_RENDER_PRESENT_HIRES ||
        present->path == GPU_RENDER_PRESENT_WIDE)
        return GPU_RENDER_TRANSACTION_UNSUPPORTED;
    if (present->path != GPU_RENDER_PRESENT_CANONICAL)
        return GPU_RENDER_TRANSACTION_VALIDATION_FAILED;
    right = (int64_t)present->display_x + present->display_width;
    bottom = (int64_t)present->display_y + present->display_height;
    expected_width = (uint32_t)VRAM_W * (uint32_t)s_scale;
    expected_height = (uint32_t)VRAM_H * (uint32_t)s_scale;
    if (present->display_x < 0 || present->display_y < 0 ||
        present->display_width <= 0 || present->display_height <= 0 ||
        right > VRAM_W || bottom > VRAM_H ||
        present->surface_width != expected_width ||
        present->surface_height != expected_height ||
        present->scale != (uint32_t)s_scale || present->wide_base_x != 0 ||
        present->linear_filter > 1u || present->force_4_3 > 1u ||
        present->reserved[0] != 0u || present->reserved[1] != 0u)
        return GPU_RENDER_TRANSACTION_VALIDATION_FAILED;
    return GPU_RENDER_TRANSACTION_OK;
}

enum {
    GLB_TRANSACTION_FAULT_POST_COMPOSITION = 1,
    GLB_TRANSACTION_FAULT_FINAL_VALIDATION,
    GLB_TRANSACTION_FAULT_FINAL_BLIT,
};

static int glb_transaction_fault(int phase) {
#ifdef PSX_GL_TRANSACTION_TESTING
    if (s_transaction_test_fault == phase) {
        s_transaction_test_fault = GL_TRANSACTION_FAULT_NONE;
        s_transaction_test_diag.phase_failures++;
        s_transaction_test_diag.last_fault_phase = phase;
        return 1;
    }
#else
    (void)phase;
#endif
    return 0;
}

static int glb_transaction_create_staging(GlTransactionCheckpoint *checkpoint,
                                          int width, int height) {
    checkpoint->staging_tex = make_tex(GL_RGBA8, width, height,
                                       GL_RGBA, GL_UNSIGNED_BYTE);
    if (!checkpoint->staging_tex) return 0;
    if (!make_fbo(&checkpoint->staging_fbo, checkpoint->staging_tex, 0))
        return 0;
    checkpoint->staging_w = width;
    checkpoint->staging_h = height;
    return 1;
}

static GpuRenderTransactionStatus glb_commit_validate(
        GpuRenderTransactionId transaction_id,
        uint64_t current_vram_mutation_serial,
        const GpuRenderPresent *present) {
    GlTransactionCheckpoint *checkpoint;
    GpuRenderTransactionStatus status;
    GLsync fence;
    int context_ok, saw_error;
    int drawable_w = 0, drawable_h = 0;
    int lx, ly, lw, lh;
    GLuint composition_texture;

    if (!s_transaction) return GPU_RENDER_TRANSACTION_INVALID_TRANSITION;
    if (s_transaction->committed)
        return GPU_RENDER_TRANSACTION_INVALID_TRANSITION;
    if (!glb_transaction_id_equal(s_transaction->id, transaction_id))
        return GPU_RENDER_TRANSACTION_STATE_REJECTED;
    if (!glb_transaction_context_ready())
        return GPU_RENDER_TRANSACTION_CONTEXT_LOST;
    if (!glb_transaction_interpolation_quiesced() || s_scale_apply_pending)
        return GPU_RENDER_TRANSACTION_STATE_REJECTED;
    if (current_vram_mutation_serial != s_transaction->vram_serial)
        return GPU_RENDER_TRANSACTION_STATE_REJECTED;
    if (g_wide_w != 0 || g_wide_cur != 0)
        return GPU_RENDER_TRANSACTION_UNSUPPORTED;
    status = glb_validate_present(present);
    if (status != GPU_RENDER_TRANSACTION_OK) return status;
    checkpoint = s_transaction;
    composition_texture = s_hr_tex;
    if (checkpoint->deferred_candidate_token !=
        GPU_RENDER_DEFERRED_CANDIDATE_NONE) {
        if (s_deferred_candidate.token !=
                checkpoint->deferred_candidate_token ||
            !glb_transaction_id_equal(s_deferred_candidate.visual_id,
                                      transaction_id) ||
            s_deferred_candidate.scale != s_scale ||
            s_deferred_candidate.width != VRAM_W * s_scale ||
            s_deferred_candidate.height != VRAM_H * s_scale)
            return GPU_RENDER_TRANSACTION_STATE_REJECTED;
        composition_texture = s_deferred_candidate.texture;
    }
    SDL_GL_GetDrawableSize(s_win, &drawable_w, &drawable_h);
    if (drawable_w <= 0 || drawable_h <= 0)
        return GPU_RENDER_TRANSACTION_STATE_REJECTED;
    checkpoint->aspect_num = s_aspect_num;
    checkpoint->aspect_den = s_aspect_den;
    if (checkpoint->staging_tex || checkpoint->staging_fbo)
        return GPU_RENDER_TRANSACTION_INVALID_TRANSITION;
    if (!glb_transaction_create_staging(checkpoint, drawable_w, drawable_h))
        return GPU_RENDER_TRANSACTION_BACKEND_ERROR;

    status = glb_transaction_drain();
    if (status != GPU_RENDER_TRANSACTION_OK) return status;
    glb_transaction_restore_draw_state(checkpoint, 1);

    fence = p_glFenceSync(PSXGL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    glFinish();
    if (fence) p_glDeleteSync(fence);
    context_ok = glb_transaction_context_ready();
    saw_error = context_ok ? glb_transaction_consume_gl_errors() : 0;
    if (!context_ok) return GPU_RENDER_TRANSACTION_CONTEXT_LOST;
    if (!fence || saw_error)
        return GPU_RENDER_TRANSACTION_BACKEND_ERROR;

    if (present->force_4_3)
        letterbox_rect_aspect(drawable_w, drawable_h, 4, 3,
                              &lx, &ly, &lw, &lh);
    else
        letterbox_rect(drawable_w, drawable_h, &lx, &ly, &lw, &lh);

    /* Compose the complete present target privately. The overlay hook receives
     * the staging FBO and cannot make the transaction observable through the
     * default framebuffer. */
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, checkpoint->staging_fbo);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    glViewport(0, 0, drawable_w, drawable_h);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    present_target_quad(checkpoint->staging_fbo,
                        composition_texture, VRAM_W, VRAM_H,
                        present->display_x, present->display_y,
                        present->display_width, present->display_height,
                        present->linear_filter, lx, ly, lw, lh);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, checkpoint->staging_fbo);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, checkpoint->staging_fbo);
    psx_debug_overlay_pre_swap_target((unsigned int)checkpoint->staging_fbo);
    pres_prepare_staged(&checkpoint->staged_present_event,
                        checkpoint->staging_fbo,
                        present->display_x, present->display_y,
                        present->display_width, present->display_height,
                        lx, ly, lw, lh);
    glDisable(GL_SCISSOR_TEST);
#ifdef PSX_GL_TRANSACTION_TESTING
    s_transaction_test_diag.staging_compositions++;
#endif
    if (glb_transaction_fault(GLB_TRANSACTION_FAULT_POST_COMPOSITION))
        return GPU_RENDER_TRANSACTION_BACKEND_ERROR;

    fence = p_glFenceSync(PSXGL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    glFinish();
    if (fence) p_glDeleteSync(fence);
    context_ok = glb_transaction_context_ready();
    saw_error = context_ok ? glb_transaction_consume_gl_errors() : 0;
    if (!context_ok) return GPU_RENDER_TRANSACTION_CONTEXT_LOST;
    if (!fence || saw_error) return GPU_RENDER_TRANSACTION_BACKEND_ERROR;
    if (glb_transaction_fault(GLB_TRANSACTION_FAULT_FINAL_VALIDATION))
        return GPU_RENDER_TRANSACTION_BACKEND_ERROR;

    SDL_GL_GetDrawableSize(s_win, &drawable_w, &drawable_h);
    if (drawable_w != checkpoint->staging_w ||
        drawable_h != checkpoint->staging_h ||
        (!present->force_4_3 &&
         (s_aspect_num != checkpoint->aspect_num ||
          s_aspect_den != checkpoint->aspect_den)))
        return GPU_RENDER_TRANSACTION_STATE_REJECTED;
    if (!glb_transaction_context_ready())
        return GPU_RENDER_TRANSACTION_CONTEXT_LOST;
    if (glb_transaction_fault(GLB_TRANSACTION_FAULT_FINAL_BLIT))
        return GPU_RENDER_TRANSACTION_BACKEND_ERROR;

    /* The transaction's first and only default-framebuffer write. */
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
    p_glBlitFramebuffer(0, 0, drawable_w, drawable_h,
                        0, 0, drawable_w, drawable_h,
                        GL_COLOR_BUFFER_BIT, GL_NEAREST);
#ifdef PSX_GL_TRANSACTION_TESTING
    s_transaction_test_diag.final_blits++;
#endif

    fence = p_glFenceSync(PSXGL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    glFinish();
    if (fence) p_glDeleteSync(fence);
    context_ok = glb_transaction_context_ready();
    saw_error = context_ok ? glb_transaction_consume_gl_errors() : 0;
    if (!context_ok) return GPU_RENDER_TRANSACTION_CONTEXT_LOST;
    if (!fence || saw_error) return GPU_RENDER_TRANSACTION_BACKEND_ERROR;

    checkpoint->present = *present;
    checkpoint->committed = 1;
#ifdef PSX_GL_TRANSACTION_TESTING
    s_transaction_test_diag.commits_ready++;
#endif
    return GPU_RENDER_TRANSACTION_READY;
}

static int glb_transaction_restore_contents(void) {
    GlTransactionCheckpoint *checkpoint = s_transaction;
    int saw_error = 0;

    if (!checkpoint) return 0;
    if (glb_transaction_context_ready()) {
        saw_error = glb_transaction_consume_gl_errors();
        flush_flat_batch();
        flush_tex_batch();
        flush_cpu_upload();
        pack_flush();
        saw_error |= glb_transaction_consume_gl_errors();

        memcpy(s_vram, checkpoint->vram, sizeof(checkpoint->vram));
        s_fb_n = 0;
        s_tb_n = 0;
        s_up_nrects = 0;
        rect_clear(&s_pack_dirty);
        s_gpu_dirty = 0;
        s_stencil_valid = 1;
        up_add(0, 0, VRAM_W - 1, VRAM_H - 1);
        flush_cpu_upload();
        glFinish();
        saw_error |= glb_transaction_consume_gl_errors();
    } else {
        memcpy(s_vram, checkpoint->vram, sizeof(checkpoint->vram));
        saw_error = 1;
    }

    glb_transaction_restore_snapshot_state(checkpoint);
    if (saw_error) {
        /* The CPU checkpoint is authoritative even if GL restoration could not
         * complete. Make the next Original present retry a complete upload. */
        s_up_nrects = 0;
        rect_clear(&s_pack_dirty);
        s_gpu_dirty = 0;
        up_add(0, 0, VRAM_W - 1, VRAM_H - 1);
    }
    return saw_error;
}

static int glb_transaction_abort_pending(int force_original) {
    int committed;
    int saw_error;

    if (!s_transaction) return 0;
    committed = s_transaction->committed;
    saw_error = glb_transaction_restore_contents();
    glb_transaction_discard_checkpoint();
    if (force_original && committed) {
        if (s_force_present_remaining < 1) s_force_present_remaining = 1;
        s_last_present_path = -1;
        s_transaction_force_original = 1;
    }
    return saw_error;
}

static int glb_transaction_reject_other_present(void) {
    return s_transaction != NULL;
}

GlRendererTransactionSwapStatus gl_renderer_swap_ready_transaction(void) {
    GlTransactionCheckpoint *checkpoint = s_transaction;

    if (!checkpoint || !checkpoint->committed)
        return GL_RENDERER_TRANSACTION_SWAP_NOT_READY;

    /* Commit's final glGetError established the context/window contract. Keep
     * this as the literal first SDL/GL/window operation after READY. */
    SDL_GL_SwapWindow(s_win);
    s_probe_swap++;
#ifdef PSX_GL_TRANSACTION_TESTING
    s_transaction_test_diag.swaps++;
#endif

    hold_capture_drawable_target(checkpoint->staging_fbo,
                                 checkpoint->staging_w,
                                 checkpoint->staging_h);

    pres_publish_staged(&checkpoint->staged_present_event);
    present_dirty_rect(checkpoint->present.display_x,
                       checkpoint->present.display_y,
                       checkpoint->present.display_x +
                           checkpoint->present.display_width - 1,
                       checkpoint->present.display_y +
                           checkpoint->present.display_height - 1,
                       0);
    present_force_consumed();
    s_last_present_path = GL_PRES_VRAM;
    s_last_dx = checkpoint->present.display_x;
    s_last_dy = checkpoint->present.display_y;
    s_last_dw = checkpoint->present.display_width;
    s_last_dh = checkpoint->present.display_height;
    coh_record(GL_COH_PRESENT,
               checkpoint->present.display_x,
               checkpoint->present.display_y,
               checkpoint->present.display_x +
                   checkpoint->present.display_width - 1,
               checkpoint->present.display_y +
                   checkpoint->present.display_height - 1);
#ifdef PSX_GL_TRANSACTION_TESTING
    s_transaction_test_diag.publications++;
#endif
    glb_transaction_discard_checkpoint_owned();
    return GL_RENDERER_TRANSACTION_SWAP_SUCCESS;
}

int gl_renderer_cancel_ready_transaction(void) {
    if (!s_transaction || !s_transaction->committed) return 0;
    (void)glb_transaction_abort_pending(1);
    return 1;
}

static GpuRenderTransactionStatus glb_rollback(
        GpuRenderTransactionId transaction_id) {
    int saw_error;

    if (!s_transaction) return GPU_RENDER_TRANSACTION_INVALID_TRANSITION;
    if (!glb_transaction_id_equal(s_transaction->id, transaction_id))
        return GPU_RENDER_TRANSACTION_STATE_REJECTED;
    if (!glb_transaction_context_ready())
        return GPU_RENDER_TRANSACTION_CONTEXT_LOST;
    if (!glb_transaction_interpolation_quiesced())
        return GPU_RENDER_TRANSACTION_STATE_REJECTED;

    saw_error = glb_transaction_restore_contents();
    if (saw_error) return GPU_RENDER_TRANSACTION_BACKEND_ERROR;

    glb_transaction_discard_checkpoint();
    return GPU_RENDER_TRANSACTION_OK;
}

static const GpuRenderBackend GL_BACKEND = {
    .name = "opengl",
    .init = glb_init, .set_scale = glb_set_scale, .scale = glb_scale,
    .set_texture_filter = glb_set_texture_filter, .texture_filter = glb_texture_filter,
    .set_semi_transparency = glb_set_semi_transparency, .set_mask_bits = glb_set_mask_bits,
    .set_texture_window = glb_set_texture_window, .set_color_modulation = glb_set_color_modulation,
    .set_dither = glb_set_dither,
    .set_precise_triangle = glb_set_precise_triangle,
    .set_perspective_triangle = glb_set_perspective_triangle,
    .fill_rect = glb_fill_rect, .copy_rect = glb_copy_rect,
    .draw_flat_triangle = glb_draw_flat_triangle, .draw_gouraud_triangle = glb_draw_gouraud_triangle,
    .draw_flat_triangle_rgb888 = glb_draw_flat_triangle_rgb888,
    .draw_gouraud_triangle_rgb888 = glb_draw_gouraud_triangle_rgb888,
    .draw_textured_triangle = glb_draw_textured_triangle,
    .draw_shaded_textured_triangle = glb_draw_shaded_textured_triangle,
    .draw_flat_rect = glb_draw_flat_rect, .draw_textured_rect = glb_draw_textured_rect,
    .draw_textured_rect_scaled = glb_draw_textured_rect_scaled,
    .draw_line = glb_draw_line, .draw_shaded_line = glb_draw_shaded_line,
    .native_fill_rect = glb_native_fill_rect,
    .native_copy_rect = glb_native_copy_rect,
    .stream_barrier = glb_stream_barrier,
       .draw_semantic_immediate = glb_draw_semantic_immediate,
       .draw_semantic_temporal_candidate =
           glb_draw_semantic_temporal_candidate,
       .record_interpolation_anchors =
           gl_renderer_record_interpolation_anchors,
      .render_display = glb_render_display, .render_display_hires = glb_render_display_hires,
      .present_vram = glb_present_vram,
      .present_cpu_frame = glb_present_cpu_frame,
      .present_native_cpu_frame = glb_present_native_cpu_frame,
      .canonical_framebuffer_digest = glb_canonical_framebuffer_digest,
    .vram_write = glb_vram_write, .vram_read = glb_vram_read,
    .vram_transfer_in = glb_vram_transfer_in, .vram_transfer_out = glb_vram_transfer_out,
    .set_draw_area = glb_set_draw_area, .get_draw_area = glb_get_draw_area,
    .set_draw_offset = glb_set_draw_offset,
    .wide_configure = glb_wide_configure,
    .wide_set_target = glb_wide_set_target,
    .wide_disable_target = glb_wide_disable_target,
    .wide_clear = glb_wide_clear,
    .wide_clear_margins = glb_wide_clear_margins,
    .native_view_clear_margins = glb_native_view_clear_margins,
    .render_wide_display = glb_render_wide_display,
    .wide_dump_full = glb_wide_dump_full,
    .transaction_begin = glb_transaction_begin,
    .ordering_barrier = glb_ordering_barrier,
    .draw_semantic = glb_draw_semantic,
    .commit_validate = glb_commit_validate,
    .rollback = glb_rollback,
    .deferred_candidate_capture = glb_deferred_candidate_capture,
    .deferred_candidate_discard = glb_deferred_candidate_discard,
    .deferred_transaction_begin = glb_deferred_transaction_begin,
};

const GpuRenderBackend *gl_backend_get(void) { return &GL_BACKEND; }
