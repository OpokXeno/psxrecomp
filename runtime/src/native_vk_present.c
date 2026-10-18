/* Vulkan presentation for the Native OpenGL presenter (see
 * native_vk_present.h for the contract).
 *
 * Flow per frame: GL composes into an interop image (Vulkan memory imported
 * through GL_EXT_memory_object), signals gl_ready; Vulkan waits gl_ready,
 * blits the image flipped into a swapchain image, presents, and signals
 * vk_done which GL waits on before it reuses the same ring slot. Both
 * semaphores are binary and strictly balanced: every signal has exactly one
 * wait, including on acquire timeouts and a minimized window. */

#include "native_vk_present.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(PSX_HAVE_VULKAN)

/* Built without Vulkan: the interop path never activates. */
int nvp_probe(void) { return 0; }
int nvp_attach_window(struct SDL_Window *window) { (void)window; return 0; }
int nvp_active(void) { return 0; }
void nvp_set_present_policy(int swap_interval, int user_vsync)
{
    (void)swap_interval;
    (void)user_vsync;
}
unsigned int nvp_begin_frame(int width, int height)
{
    (void)width;
    (void)height;
    return 0;
}
int nvp_present(void) { return 0; }
int nvp_capture_presented(int timeout_ms, unsigned char **out_rgb, int *out_w, int *out_h)
{
    (void)timeout_ms;
    (void)out_rgb;
    (void)out_w;
    (void)out_h;
    return 0;
}
void nvp_release_gl_context(void) {}
void nvp_shutdown(void) {}
void nvp_diagnostics(NvpDiagnostics *out)
{
    memset(out, 0, sizeof(*out));
    out->present_mode = -1;
}
int nvp_diagnostics_json(char *out, int capacity)
{
    return snprintf(out, (size_t)capacity, "{\"active\":0}");
}

#else  /* PSX_HAVE_VULKAN */

#include <stdarg.h>
#include <stdatomic.h>
#include "psx_sdl.h"
#if defined(PSX_SDL3)
#include <SDL3/SDL_opengl.h>
#else
#include <SDL_opengl.h>
#endif
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif
#include "vk_core.h"
#include "vk_swapchain.h"
#if defined(_WIN32)
#include <vulkan/vulkan_win32.h>
#endif

#define NVP_RING 3            /* interop images and handover frames */
#define NVP_MAX_GL_DEV 8

/* GL values from the Khronos registry, defined here so the file does not
 * depend on which GL headers the platform ships. */
#define NVP_GL_FRAMEBUFFER                0x8D40
#define NVP_GL_READ_FRAMEBUFFER           0x8CA8
#define NVP_GL_DRAW_FRAMEBUFFER           0x8CA9
#define NVP_GL_DRAW_FRAMEBUFFER_BINDING   0x8CA6
#define NVP_GL_READ_FRAMEBUFFER_BINDING   0x8CAA
#define NVP_GL_COLOR_ATTACHMENT0          0x8CE0
#define NVP_GL_FRAMEBUFFER_COMPLETE       0x8CD5
#define NVP_GL_RGBA8                      0x8058
#define NVP_GL_NUM_EXTENSIONS             0x821D
#define NVP_GL_TEXTURE_TILING_EXT         0x9580
#define NVP_GL_DEDICATED_MEMORY_OBJECT_EXT 0x9581
#define NVP_GL_OPTIMAL_TILING_EXT         0x9584
#define NVP_GL_HANDLE_TYPE_OPAQUE_FD_EXT  0x9586
#define NVP_GL_HANDLE_TYPE_OPAQUE_WIN32_EXT 0x9587
#define NVP_GL_NUM_DEVICE_UUIDS_EXT       0x9596
#define NVP_GL_DEVICE_UUID_EXT            0x9597
#define NVP_GL_DRIVER_UUID_EXT            0x9598
#define NVP_GL_UUID_SIZE_EXT              16
#define NVP_GL_LAYOUT_TRANSFER_SRC_EXT    0x9592

#if defined(_WIN32)
#define NVP_GL_HANDLE_TYPE        NVP_GL_HANDLE_TYPE_OPAQUE_WIN32_EXT
#define NVP_VK_MEM_HANDLE         VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT
#define NVP_VK_SEM_HANDLE         VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT
#else
#define NVP_GL_HANDLE_TYPE        NVP_GL_HANDLE_TYPE_OPAQUE_FD_EXT
#define NVP_VK_MEM_HANDLE         VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT
#define NVP_VK_SEM_HANDLE         VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT
#endif

/* ---- GL entry points ---------------------------------------------------- */
typedef const GLubyte *(APIENTRY *NvpPfnGetStringi)(GLenum, GLuint);
typedef void (APIENTRY *NvpPfnCreateMemoryObjects)(GLsizei, GLuint *);
typedef void (APIENTRY *NvpPfnDeleteMemoryObjects)(GLsizei, const GLuint *);
typedef void (APIENTRY *NvpPfnMemoryObjectParameteriv)(GLuint, GLenum, const GLint *);
#if defined(_WIN32)
typedef void (APIENTRY *NvpPfnImportMemory)(GLuint, GLuint64, GLenum, void *);
typedef void (APIENTRY *NvpPfnImportSemaphore)(GLuint, GLenum, void *);
#else
typedef void (APIENTRY *NvpPfnImportMemory)(GLuint, GLuint64, GLenum, GLint);
typedef void (APIENTRY *NvpPfnImportSemaphore)(GLuint, GLenum, GLint);
#endif
typedef void (APIENTRY *NvpPfnTexStorageMem2D)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLuint64);
typedef void (APIENTRY *NvpPfnGenSemaphores)(GLsizei, GLuint *);
typedef void (APIENTRY *NvpPfnDeleteSemaphores)(GLsizei, const GLuint *);
typedef void (APIENTRY *NvpPfnSemaphoreOp)(GLuint, GLuint, const GLuint *, GLuint, const GLuint *, const GLenum *);
typedef void (APIENTRY *NvpPfnGetUnsignedBytev)(GLenum, GLubyte *);
typedef void (APIENTRY *NvpPfnGetUnsignedBytei_v)(GLenum, GLuint, GLubyte *);
typedef void (APIENTRY *NvpPfnGenFramebuffers)(GLsizei, GLuint *);
typedef void (APIENTRY *NvpPfnDeleteFramebuffers)(GLsizei, const GLuint *);
typedef void (APIENTRY *NvpPfnBindFramebuffer)(GLenum, GLuint);
typedef void (APIENTRY *NvpPfnFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (APIENTRY *NvpPfnCheckFramebufferStatus)(GLenum);

static NvpPfnGetStringi              nvp_glGetStringi;
static NvpPfnCreateMemoryObjects     nvp_glCreateMemoryObjectsEXT;
static NvpPfnDeleteMemoryObjects     nvp_glDeleteMemoryObjectsEXT;
static NvpPfnMemoryObjectParameteriv nvp_glMemoryObjectParameterivEXT;
static NvpPfnImportMemory            nvp_glImportMemoryEXT;
static NvpPfnTexStorageMem2D         nvp_glTexStorageMem2DEXT;
static NvpPfnGenSemaphores           nvp_glGenSemaphoresEXT;
static NvpPfnDeleteSemaphores        nvp_glDeleteSemaphoresEXT;
static NvpPfnImportSemaphore         nvp_glImportSemaphoreEXT;
static NvpPfnSemaphoreOp             nvp_glSignalSemaphoreEXT;
static NvpPfnSemaphoreOp             nvp_glWaitSemaphoreEXT;
static NvpPfnGetUnsignedBytev        nvp_glGetUnsignedBytevEXT;
static NvpPfnGetUnsignedBytei_v      nvp_glGetUnsignedBytei_vEXT;
static NvpPfnGenFramebuffers         nvp_glGenFramebuffers;
static NvpPfnDeleteFramebuffers      nvp_glDeleteFramebuffers;
static NvpPfnBindFramebuffer         nvp_glBindFramebuffer;
static NvpPfnFramebufferTexture2D    nvp_glFramebufferTexture2D;
static NvpPfnCheckFramebufferStatus  nvp_glCheckFramebufferStatus;

/* ---- state -------------------------------------------------------------- */
typedef struct NvpImage {
    VkImage image;
    VkDeviceMemory memory;   /* explicit allocation: VMA cannot export memory */
    VkSemaphore gl_ready;    /* GL signals, Vulkan waits */
    VkSemaphore vk_done;     /* Vulkan signals, GL waits */
    GLuint tex, fbo, memobj;
    GLuint gl_sem_ready, gl_sem_done;
    int vk_done_pending;     /* Vulkan signaled vk_done, GL has not waited */
} NvpImage;

/* Command buffer and fence for a submit that only hands an interop image
 * over (no swapchain image to blit into). */
typedef struct NvpFrame {
    VkCommandBuffer cb;
    VkFence fence;
} NvpFrame;

#if defined(_WIN32)
static PFN_vkGetMemoryWin32HandleKHR nvp_get_memory_handle;
static PFN_vkGetSemaphoreWin32HandleKHR nvp_get_semaphore_handle;
#endif

static int s_probed;                 /* vk_core is initialized for the interop path */
static int s_active;
static VkCommandPool s_pool;
static NvpFrame s_frames[NVP_RING];
static uint32_t s_frame_cursor;

static GLubyte s_gl_dev_uuid[NVP_MAX_GL_DEV][NVP_GL_UUID_SIZE_EXT];
static int s_gl_dev_count;
static GLubyte s_gl_drv_uuid[NVP_GL_UUID_SIZE_EXT];

static NvpImage s_img[NVP_RING];
static int s_interop_live;           /* VK side of the interop images exists */
static int s_gl_live;                /* GL side exists in some context */
static int s_ever_interop;
static int s_iw, s_ih;
static uint32_t s_cursor;
static int s_begun;
static int s_applied_interval = 1;

static _Atomic int s_req_interval = 1;
static _Atomic int s_req_vsync = 1;

/* First reason the probe or the attach failed, for the debug server. */
static char s_probe_failure[192];

/* diagnostics: written by the presenter thread, read from any thread */
static _Atomic uint32_t s_d_int_w, s_d_int_h;
static _Atomic uint64_t s_d_frames_begun, s_d_frames_presented;
static _Atomic uint64_t s_d_interop_recreations, s_d_failures;
static _Atomic uint64_t s_d_last_present_ns, s_d_max_block_ns;
static _Atomic int s_d_last_result;

#define NVP_ADD(v, n) atomic_fetch_add_explicit(&(v), (n), memory_order_relaxed)
#define NVP_GET(v) atomic_load_explicit(&(v), memory_order_relaxed)
#define NVP_SET(v, x) atomic_store_explicit(&(v), (x), memory_order_relaxed)

static uint64_t nvp_now_ns(void)
{
#if defined(PSX_SDL3)
    return (uint64_t)SDL_GetTicksNS();
#else
    uint64_t c = SDL_GetPerformanceCounter(), f = SDL_GetPerformanceFrequency();
    return (c / f) * 1000000000ull + (c % f) * 1000000000ull / f;
#endif
}

static void nvp_fail(VkResult r)
{
    NVP_ADD(s_d_failures, 1);
    NVP_SET(s_d_last_result, (int)r);
}

/* Keeps the first reason; quotes and control characters are replaced so the
 * text can go into JSON as is. */
static void nvp_note(const char *fmt, ...)
{
    if (s_probe_failure[0]) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_probe_failure, sizeof(s_probe_failure), fmt, ap);
    va_end(ap);
    size_t n = strlen(s_probe_failure);
    while (n && s_probe_failure[n - 1] == '\n') s_probe_failure[--n] = '\0';
    for (char *c = s_probe_failure; *c; ++c)
        if (*c == '"' || *c == '\\' || (unsigned char)*c < 0x20) *c = ' ';
}

/* ---- GL probe ----------------------------------------------------------- */
static int nvp_gl_load_one(void **slot, const char *name)
{
    *slot = (void *)SDL_GL_GetProcAddress(name);
    if (!*slot) {
        nvp_note("GL entry point %s missing\n", name);
        return 0;
    }
    return 1;
}

static int nvp_probe_gl(void)
{
    if (!nvp_gl_load_one((void **)&nvp_glGetStringi, "glGetStringi")) return 0;

    GLint n = 0;
    glGetIntegerv(NVP_GL_NUM_EXTENSIONS, &n);
    int have_mem = 0, have_sem = 0, have_mem_h = 0, have_sem_h = 0;
#if defined(_WIN32)
    const char *mem_h = "GL_EXT_memory_object_win32", *sem_h = "GL_EXT_semaphore_win32";
#else
    const char *mem_h = "GL_EXT_memory_object_fd", *sem_h = "GL_EXT_semaphore_fd";
#endif
    for (GLint i = 0; i < n; ++i) {
        const char *e = (const char *)nvp_glGetStringi(GL_EXTENSIONS, (GLuint)i);
        if (!e) continue;
        if (!strcmp(e, "GL_EXT_memory_object")) have_mem = 1;
        else if (!strcmp(e, "GL_EXT_semaphore")) have_sem = 1;
        else if (!strcmp(e, mem_h)) have_mem_h = 1;
        else if (!strcmp(e, sem_h)) have_sem_h = 1;
    }
    if (!have_mem || !have_sem || !have_mem_h || !have_sem_h) {
        nvp_note("GL lacks %s%s%s%s\n",
                have_mem ? "" : "GL_EXT_memory_object ",
                have_sem ? "" : "GL_EXT_semaphore ",
                have_mem_h ? "" : mem_h, have_sem_h ? "" : sem_h);
        return 0;
    }

#if defined(_WIN32)
    const char *imp_mem = "glImportMemoryWin32HandleEXT", *imp_sem = "glImportSemaphoreWin32HandleEXT";
#else
    const char *imp_mem = "glImportMemoryFdEXT", *imp_sem = "glImportSemaphoreFdEXT";
#endif
    if (!nvp_gl_load_one((void **)&nvp_glCreateMemoryObjectsEXT, "glCreateMemoryObjectsEXT") ||
        !nvp_gl_load_one((void **)&nvp_glDeleteMemoryObjectsEXT, "glDeleteMemoryObjectsEXT") ||
        !nvp_gl_load_one((void **)&nvp_glMemoryObjectParameterivEXT, "glMemoryObjectParameterivEXT") ||
        !nvp_gl_load_one((void **)&nvp_glImportMemoryEXT, imp_mem) ||
        !nvp_gl_load_one((void **)&nvp_glTexStorageMem2DEXT, "glTexStorageMem2DEXT") ||
        !nvp_gl_load_one((void **)&nvp_glGenSemaphoresEXT, "glGenSemaphoresEXT") ||
        !nvp_gl_load_one((void **)&nvp_glDeleteSemaphoresEXT, "glDeleteSemaphoresEXT") ||
        !nvp_gl_load_one((void **)&nvp_glImportSemaphoreEXT, imp_sem) ||
        !nvp_gl_load_one((void **)&nvp_glSignalSemaphoreEXT, "glSignalSemaphoreEXT") ||
        !nvp_gl_load_one((void **)&nvp_glWaitSemaphoreEXT, "glWaitSemaphoreEXT") ||
        !nvp_gl_load_one((void **)&nvp_glGetUnsignedBytevEXT, "glGetUnsignedBytevEXT") ||
        !nvp_gl_load_one((void **)&nvp_glGetUnsignedBytei_vEXT, "glGetUnsignedBytei_vEXT") ||
        !nvp_gl_load_one((void **)&nvp_glGenFramebuffers, "glGenFramebuffers") ||
        !nvp_gl_load_one((void **)&nvp_glDeleteFramebuffers, "glDeleteFramebuffers") ||
        !nvp_gl_load_one((void **)&nvp_glBindFramebuffer, "glBindFramebuffer") ||
        !nvp_gl_load_one((void **)&nvp_glFramebufferTexture2D, "glFramebufferTexture2D") ||
        !nvp_gl_load_one((void **)&nvp_glCheckFramebufferStatus, "glCheckFramebufferStatus"))
        return 0;

    GLint nuuid = 0;
    glGetIntegerv(NVP_GL_NUM_DEVICE_UUIDS_EXT, &nuuid);
    if (nuuid <= 0) {
        nvp_note("GL reports no device UUID\n");
        return 0;
    }
    if (nuuid > NVP_MAX_GL_DEV) nuuid = NVP_MAX_GL_DEV;
    s_gl_dev_count = (int)nuuid;
    for (int i = 0; i < s_gl_dev_count; ++i)
        nvp_glGetUnsignedBytei_vEXT(NVP_GL_DEVICE_UUID_EXT, (GLuint)i, s_gl_dev_uuid[i]);
    nvp_glGetUnsignedBytevEXT(NVP_GL_DRIVER_UUID_EXT, s_gl_drv_uuid);
    return 1;
}

/* ---- teardown ----------------------------------------------------------- */
static void nvp_destroy_image_vk(NvpImage *im)
{
    VkDevice dev = vk_core_device();
    if (im->gl_ready) vkDestroySemaphore(dev, im->gl_ready, NULL);
    if (im->vk_done) vkDestroySemaphore(dev, im->vk_done, NULL);
    if (im->image) vkDestroyImage(dev, im->image, NULL);
    if (im->memory) vkFreeMemory(dev, im->memory, NULL);
    im->gl_ready = im->vk_done = VK_NULL_HANDLE;
    im->image = VK_NULL_HANDLE;
    im->memory = VK_NULL_HANDLE;
}

static void nvp_destroy_image_gl(NvpImage *im)
{
    if (im->fbo) nvp_glDeleteFramebuffers(1, &im->fbo);
    if (im->tex) glDeleteTextures(1, &im->tex);
    if (im->memobj) nvp_glDeleteMemoryObjectsEXT(1, &im->memobj);
    if (im->gl_sem_ready) nvp_glDeleteSemaphoresEXT(1, &im->gl_sem_ready);
    if (im->gl_sem_done) nvp_glDeleteSemaphoresEXT(1, &im->gl_sem_done);
    im->fbo = im->tex = im->memobj = im->gl_sem_ready = im->gl_sem_done = 0;
}

/* Drops every interop object. with_gl: also the GL side (the owning context
 * must be current); otherwise only the Vulkan side is released. */
static void nvp_destroy_interop(int with_gl)
{
    if (vk_core_device()) vk_core_device_wait_idle();
    for (int i = 0; i < NVP_RING; ++i) {
        if (with_gl && s_gl_live) nvp_destroy_image_gl(&s_img[i]);
        if (vk_core_device()) nvp_destroy_image_vk(&s_img[i]);
        memset(&s_img[i], 0, sizeof(s_img[i]));
    }
    s_interop_live = 0;
    s_gl_live = 0;
    s_iw = s_ih = 0;
    s_cursor = 0;
    s_begun = 0;
    NVP_SET(s_d_int_w, 0);
    NVP_SET(s_d_int_h, 0);
}

static void nvp_destroy_frames(void)
{
    VkDevice dev = vk_core_device();
    for (int i = 0; i < NVP_RING; ++i) {
        if (s_frames[i].fence) vkDestroyFence(dev, s_frames[i].fence, NULL);
        if (s_frames[i].cb && s_pool) vkFreeCommandBuffers(dev, s_pool, 1, &s_frames[i].cb);
    }
    if (s_pool) vkDestroyCommandPool(dev, s_pool, NULL);
    memset(s_frames, 0, sizeof(s_frames));
    s_pool = VK_NULL_HANDLE;
}

static void nvp_teardown(void)
{
    if (vk_core_device()) {
        vk_core_device_wait_idle();
        if (s_interop_live) nvp_destroy_interop(0);
        nvp_destroy_frames();
    }
    vk_swapchain_destroy();
    vk_core_detach_window();
    vk_core_shutdown();

    s_probed = 0;
    s_active = 0;
    s_frame_cursor = 0;
    s_cursor = 0;
    s_begun = 0;
    s_ever_interop = 0;
}

/* ---- probe -------------------------------------------------------------- */
/* Checks the chosen device for everything the interop path needs beyond what
 * vk_core_init guaranteed (the external memory and semaphore extensions). */
static int nvp_check_interop_device(VkPhysicalDevice pd)
{
    VkPhysicalDeviceExternalImageFormatInfo efi = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO };
    efi.handleType = NVP_VK_MEM_HANDLE;
    VkPhysicalDeviceImageFormatInfo2 ifi = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2 };
    ifi.pNext = &efi;
    ifi.format = VK_FORMAT_R8G8B8A8_UNORM;
    ifi.type = VK_IMAGE_TYPE_2D;
    ifi.tiling = VK_IMAGE_TILING_OPTIMAL;
    ifi.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VkExternalImageFormatProperties efp = { VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES };
    VkImageFormatProperties2 ifp = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2 };
    ifp.pNext = &efp;
    if (vkGetPhysicalDeviceImageFormatProperties2(pd, &ifi, &ifp) != VK_SUCCESS ||
        !(efp.externalMemoryProperties.externalMemoryFeatures &
          VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)) {
        nvp_note("Vulkan cannot export RGBA8 images");
        return 0;
    }

    VkPhysicalDeviceExternalSemaphoreInfo esi = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO };
    esi.handleType = NVP_VK_SEM_HANDLE;
    VkExternalSemaphoreProperties esp = { VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES };
    vkGetPhysicalDeviceExternalSemaphoreProperties(pd, &esi, &esp);
    if (!(esp.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT)) {
        nvp_note("Vulkan cannot export semaphores");
        return 0;
    }

    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_R8G8B8A8_UNORM, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)) {
        nvp_note("RGBA8 cannot be a blit source");
        return 0;
    }
    return 1;
}

int nvp_probe(void)
{
    if (s_probed) return 1;
    s_probe_failure[0] = '\0';
    const char *env = getenv("PSX_VK_PRESENT");
    if (env && env[0] == '0') {
        nvp_note("disabled by PSX_VK_PRESENT=0");
        return 0;
    }
    if (!nvp_probe_gl()) return 0;

#if defined(_WIN32)
    static const char *const dev_exts[2] = { VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
                                             VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME };
#else
    static const char *const dev_exts[2] = { VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                                             VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME };
#endif
    VkCoreDeviceRequest req;
    memset(&req, 0, sizeof(req));
    req.device_uuids = (const uint8_t (*)[VK_UUID_SIZE])s_gl_dev_uuid;
    req.device_uuid_count = (uint32_t)s_gl_dev_count;
    req.driver_uuid = s_gl_drv_uuid;
    req.extensions = dev_exts;
    req.extension_count = 2;
    if (!vk_core_init(&req)) {
        nvp_note("%s", vk_core_failure());
        return 0;
    }
#if defined(_WIN32)
    nvp_get_memory_handle = (PFN_vkGetMemoryWin32HandleKHR)vkGetDeviceProcAddr(vk_core_device(), "vkGetMemoryWin32HandleKHR");
    nvp_get_semaphore_handle = (PFN_vkGetSemaphoreWin32HandleKHR)vkGetDeviceProcAddr(vk_core_device(), "vkGetSemaphoreWin32HandleKHR");
    if (!nvp_get_memory_handle || !nvp_get_semaphore_handle) {
        nvp_note("Win32 external handle entry points missing");
        nvp_teardown();
        return 0;
    }
#endif
    if (!nvp_check_interop_device(vk_core_physical_device())) {
        nvp_teardown();
        return 0;
    }
    s_probed = 1;
    return 1;
}

static void nvp_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to,
                        VkAccessFlags src_access, VkAccessFlags dst_access,
                        VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage,
                        uint32_t src_family, uint32_t dst_family)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = src_family;
    b.dstQueueFamilyIndex = dst_family;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

static void nvp_acquire_interop(VkCommandBuffer cb, VkImage img)
{
    nvp_barrier(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                0, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_QUEUE_FAMILY_EXTERNAL, vk_core_queue_family());
}

static void nvp_release_interop(VkCommandBuffer cb, VkImage img)
{
    nvp_barrier(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_READ_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, vk_core_queue_family(), VK_QUEUE_FAMILY_EXTERNAL);
}

static VkResult nvp_wait_reset_frame(NvpFrame *f)
{
    VkResult r = vkWaitForFences(vk_core_device(), 1, &f->fence, VK_TRUE, UINT64_MAX);
    if (r != VK_SUCCESS) return r;
    r = vkResetFences(vk_core_device(), 1, &f->fence);
    if (r != VK_SUCCESS) return r;
    return vkResetCommandBuffer(f->cb, 0);
}

/* ---- interop images ----------------------------------------------------- */
static int nvp_find_memory_type(uint32_t bits)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(vk_core_physical_device(), &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            return (int)i;
    return -1;
}

static int nvp_gl_error_pending(void)
{
    int e = 0;
    while (glGetError() != 0) e = 1;
    return e;
}

/* Creates a Vulkan semaphore that can be exported and imports it into GL. */
static int nvp_make_semaphore(VkSemaphore *vs, GLuint *gs)
{
    VkExportSemaphoreCreateInfo exp = { VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO };
    exp.handleTypes = NVP_VK_SEM_HANDLE;
    VkSemaphoreCreateInfo ci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    ci.pNext = &exp;
    if (vkCreateSemaphore(vk_core_device(), &ci, NULL, vs) != VK_SUCCESS) { *vs = VK_NULL_HANDLE; return 0; }
#if defined(_WIN32)
    VkSemaphoreGetWin32HandleInfoKHR gi = { VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR };
    gi.semaphore = *vs;
    gi.handleType = NVP_VK_SEM_HANDLE;
    HANDLE h = NULL;
    if (nvp_get_semaphore_handle(vk_core_device(), &gi, &h) != VK_SUCCESS) return 0;
    nvp_glGenSemaphoresEXT(1, gs);
    nvp_gl_error_pending();
    nvp_glImportSemaphoreEXT(*gs, NVP_GL_HANDLE_TYPE, (void *)h);
    CloseHandle(h);  /* the Win32 import duplicates the handle */
    return !nvp_gl_error_pending();
#else
    VkSemaphoreGetFdInfoKHR gi = { VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR };
    gi.semaphore = *vs;
    gi.handleType = NVP_VK_SEM_HANDLE;
    int fd = -1;
    if (vkGetSemaphoreFdKHR(vk_core_device(), &gi, &fd) != VK_SUCCESS) return 0;
    nvp_glGenSemaphoresEXT(1, gs);
    nvp_gl_error_pending();
    nvp_glImportSemaphoreEXT(*gs, NVP_GL_HANDLE_TYPE, fd);
    if (nvp_gl_error_pending()) {
        close(fd);  /* GL takes ownership only on success */
        return 0;
    }
    return 1;
#endif
}

static int nvp_create_image(NvpImage *im, int w, int h)
{
    VkExternalMemoryImageCreateInfo eci = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
    eci.handleTypes = NVP_VK_MEM_HANDLE;
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.pNext = &eci;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent.width = (uint32_t)w;
    ci.extent.height = (uint32_t)h;
    ci.extent.depth = 1;
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(vk_core_device(), &ci, NULL, &im->image) != VK_SUCCESS) {
        im->image = VK_NULL_HANDLE;
        return 0;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk_core_device(), im->image, &req);
    int type = nvp_find_memory_type(req.memoryTypeBits);
    if (type < 0) return 0;
    VkMemoryDedicatedAllocateInfo ded = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
    ded.image = im->image;
    VkExportMemoryAllocateInfo exp = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
    exp.pNext = &ded;
    exp.handleTypes = NVP_VK_MEM_HANDLE;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.pNext = &exp;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = (uint32_t)type;
    if (vkAllocateMemory(vk_core_device(), &ai, NULL, &im->memory) != VK_SUCCESS) {
        im->memory = VK_NULL_HANDLE;
        return 0;
    }
    if (vkBindImageMemory(vk_core_device(), im->image, im->memory, 0) != VK_SUCCESS) return 0;

    nvp_gl_error_pending();
    nvp_glCreateMemoryObjectsEXT(1, &im->memobj);
    GLint one = 1;
    nvp_glMemoryObjectParameterivEXT(im->memobj, NVP_GL_DEDICATED_MEMORY_OBJECT_EXT, &one);
#if defined(_WIN32)
    VkMemoryGetWin32HandleInfoKHR gi = { VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR };
    gi.memory = im->memory;
    gi.handleType = NVP_VK_MEM_HANDLE;
    HANDLE hnd = NULL;
    if (nvp_get_memory_handle(vk_core_device(), &gi, &hnd) != VK_SUCCESS) return 0;
    nvp_glImportMemoryEXT(im->memobj, (GLuint64)req.size, NVP_GL_HANDLE_TYPE, (void *)hnd);
    CloseHandle(hnd);  /* the Win32 import duplicates the handle */
    if (nvp_gl_error_pending()) return 0;
#else
    VkMemoryGetFdInfoKHR gi = { VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR };
    gi.memory = im->memory;
    gi.handleType = NVP_VK_MEM_HANDLE;
    int fd = -1;
    if (vkGetMemoryFdKHR(vk_core_device(), &gi, &fd) != VK_SUCCESS) return 0;
    nvp_glImportMemoryEXT(im->memobj, (GLuint64)req.size, NVP_GL_HANDLE_TYPE, fd);
    if (nvp_gl_error_pending()) {
        close(fd);  /* GL takes ownership only on success */
        return 0;
    }
#endif

    GLint prev_tex = 0, prev_draw = 0, prev_read = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glGetIntegerv(NVP_GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
    glGetIntegerv(NVP_GL_READ_FRAMEBUFFER_BINDING, &prev_read);
    glGenTextures(1, &im->tex);
    glBindTexture(GL_TEXTURE_2D, im->tex);
    glTexParameteri(GL_TEXTURE_2D, NVP_GL_TEXTURE_TILING_EXT, NVP_GL_OPTIMAL_TILING_EXT);
    nvp_glTexStorageMem2DEXT(GL_TEXTURE_2D, 1, NVP_GL_RGBA8, w, h, im->memobj, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    nvp_glGenFramebuffers(1, &im->fbo);
    nvp_glBindFramebuffer(NVP_GL_FRAMEBUFFER, im->fbo);
    nvp_glFramebufferTexture2D(NVP_GL_FRAMEBUFFER, NVP_GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, im->tex, 0);
    int complete = nvp_glCheckFramebufferStatus(NVP_GL_FRAMEBUFFER) == NVP_GL_FRAMEBUFFER_COMPLETE;
    nvp_glBindFramebuffer(NVP_GL_DRAW_FRAMEBUFFER, (GLuint)prev_draw);
    nvp_glBindFramebuffer(NVP_GL_READ_FRAMEBUFFER, (GLuint)prev_read);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
    if (!complete || nvp_gl_error_pending()) return 0;

    if (!nvp_make_semaphore(&im->gl_ready, &im->gl_sem_ready)) return 0;
    if (!nvp_make_semaphore(&im->vk_done, &im->gl_sem_done)) return 0;
    return 1;
}

/* Moves every new image to TRANSFER_SRC_OPTIMAL and releases it to the
 * external queue family, so the first present acquires it like any other. */
static int nvp_init_image_layouts(void)
{
    VkCommandBuffer cb = vk_core_oneshot_begin();
    if (!cb) return 0;
    for (int i = 0; i < NVP_RING; ++i) {
        nvp_barrier(cb, s_img[i].image, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
        nvp_release_interop(cb, s_img[i].image);
    }
    return vk_core_oneshot_end_wait(cb);
}

static int nvp_create_interop(int w, int h)
{
    s_interop_live = 1;
    s_gl_live = 1;
    for (int i = 0; i < NVP_RING; ++i) {
        if (!nvp_create_image(&s_img[i], w, h)) {
            nvp_destroy_interop(1);
            return 0;
        }
    }
    if (!nvp_init_image_layouts()) {
        nvp_destroy_interop(1);
        return 0;
    }
    return 1;
}

/* ---- public: policy ----------------------------------------------------- */
void nvp_set_present_policy(int swap_interval, int user_vsync)
{
    atomic_store(&s_req_interval, swap_interval);
    atomic_store(&s_req_vsync, user_vsync);
    vk_swapchain_set_present_policy(swap_interval, user_vsync);
}

int nvp_active(void) { return s_active; }

/* ---- attach ------------------------------------------------------------- */
static int nvp_create_frames(void)
{
    VkDevice dev = vk_core_device();
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = vk_core_queue_family();
    if (vkCreateCommandPool(dev, &pci, NULL, &s_pool) != VK_SUCCESS) {
        s_pool = VK_NULL_HANDLE;
        nvp_note("vkCreateCommandPool failed");
        return 0;
    }
    /* Command buffers and fences (created signaled). */
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = s_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (int i = 0; i < NVP_RING; ++i) {
        if (vkAllocateCommandBuffers(dev, &cai, &s_frames[i].cb) != VK_SUCCESS) {
            s_frames[i].cb = VK_NULL_HANDLE;
            return 0;
        }
        if (vkCreateFence(dev, &fci, NULL, &s_frames[i].fence) != VK_SUCCESS) {
            s_frames[i].fence = VK_NULL_HANDLE;
            return 0;
        }
    }
    return 1;
}

static int nvp_attach_impl(SDL_Window *window)
{
    if (!s_probed || !window) return 0;
    if (!vk_core_attach_window(window)) {
        nvp_note("%s", vk_core_failure());
        return 0;
    }
    if (!nvp_create_frames()) return 0;
    if (!vk_swapchain_create()) {
        nvp_note("swapchain creation failed");
        return 0;
    }
    return 1;
}

int nvp_attach_window(SDL_Window *window)
{
    if (!nvp_attach_impl(window)) {
        nvp_teardown();
        return 0;
    }
    s_active = 1;
    return 1;
}

/* ---- frame -------------------------------------------------------------- */
unsigned int nvp_begin_frame(int width, int height)
{
    if (!s_active) return 0;
    if (s_begun) return s_img[s_cursor].fbo;
    if (width <= 0 || height <= 0) return 0;

    s_applied_interval = atomic_load(&s_req_interval);

    if (!s_interop_live || s_iw != width || s_ih != height) {
        if (s_interop_live) {
            glFinish();
            nvp_destroy_interop(1);
        }
        if (!nvp_create_interop(width, height)) {
            nvp_fail(VK_ERROR_INITIALIZATION_FAILED);
            return 0;
        }
        s_iw = width;
        s_ih = height;
        if (s_ever_interop) NVP_ADD(s_d_interop_recreations, 1);
        s_ever_interop = 1;
        NVP_SET(s_d_int_w, (uint32_t)width);
        NVP_SET(s_d_int_h, (uint32_t)height);
    }

    NvpImage *im = &s_img[s_cursor];
    if (im->vk_done_pending) {
        /* GPU-side wait: the CPU does not block. */
        GLenum layout = NVP_GL_LAYOUT_TRANSFER_SRC_EXT;
        nvp_glWaitSemaphoreEXT(im->gl_sem_done, 0, NULL, 1, &im->tex, &layout);
        im->vk_done_pending = 0;
    }
    s_begun = 1;
    NVP_ADD(s_d_frames_begun, 1);
    return im->fbo;
}

/* Submits a command buffer that only moves the interop image between queue
 * families, with no swapchain image to blit into, so the binary semaphores
 * stay balanced. first: wait gl_ready and acquire the image from GL's queue
 * family. last: release it back and signal vk_done. */
static VkResult nvp_submit_handover(NvpFrame *f, NvpImage *im, int first, int last)
{
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkResult r = vkBeginCommandBuffer(f->cb, &bi);
    if (r != VK_SUCCESS) return r;
    if (first) nvp_acquire_interop(f->cb, im->image);
    if (last) nvp_release_interop(f->cb, im->image);
    r = vkEndCommandBuffer(f->cb);
    if (r != VK_SUCCESS) return r;

    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    if (first) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &im->gl_ready;
        si.pWaitDstStageMask = &stage;
    }
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f->cb;
    if (last) {
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &im->vk_done;
    }
    return vk_core_submit(1, &si, f->fence);
}

/* Closes a handover with a submit of its own on the next handover frame. */
static VkResult nvp_close_handover(NvpImage *im, int first)
{
    NvpFrame *f = &s_frames[s_frame_cursor];
    s_frame_cursor = (s_frame_cursor + 1) % NVP_RING;
    VkResult r = nvp_wait_reset_frame(f);
    if (r != VK_SUCCESS) return r;
    return nvp_submit_handover(f, im, first, 1);
}

/* Records the flipped blit from the interop image into the swapchain frame. */
static void nvp_record_blit(VkSwapFrame *fr, NvpImage *im)
{
    vk_swapchain_frame_layout(fr, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    /* GL is bottom-up, Vulkan top-down: flip while blitting. */
    VkImageBlit blit;
    memset(&blit, 0, sizeof(blit));
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[0].y = s_ih;
    blit.srcOffsets[1].x = s_iw;
    blit.srcOffsets[1].z = 1;
    blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.dstSubresource.layerCount = 1;
    blit.dstOffsets[1].x = (int32_t)fr->width;
    blit.dstOffsets[1].y = (int32_t)fr->height;
    blit.dstOffsets[1].z = 1;
    VkFilter filter = ((uint32_t)s_iw == fr->width && (uint32_t)s_ih == fr->height)
                          ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    vkCmdBlitImage(fr->cmd, im->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, fr->image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, filter);
}

int nvp_present(void)
{
    if (!s_active || !s_begun) return 0;
    uint64_t t0 = nvp_now_ns();
    uint32_t idx = s_cursor;
    NvpImage *im = &s_img[idx];

    GLenum layout = NVP_GL_LAYOUT_TRANSFER_SRC_EXT;
    nvp_glSignalSemaphoreEXT(im->gl_sem_ready, 0, NULL, 1, &im->tex, &layout);
    glFlush();  /* the signal must be submitted before Vulkan waits on it */

    int repeats = s_applied_interval >= 2 ? s_applied_interval : 1;
    int balanced = 0, presented = 0, skipped_minimized = 0, hard_fail = 0;
    for (int r = 0; r < repeats && !hard_fail; ++r) {
        int first = (r == 0);
        int last = (r == repeats - 1);
        VkSwapFrame fr;
        VkSwapAcquire acq = vk_swapchain_acquire(&fr);
        if (acq != VK_SWAP_ACQUIRE_OK) {
            /* Nothing to blit into (minimized, or the acquire failed): only
             * the interop hand-over is submitted. */
            if (acq == VK_SWAP_ACQUIRE_SKIP) skipped_minimized = 1;
            VkResult vr = nvp_close_handover(im, first);
            if (vr != VK_SUCCESS) { nvp_fail(vr); hard_fail = 1; break; }
            balanced = 1;
            break;
        }

        if (first) nvp_acquire_interop(fr.cmd, im->image);
        nvp_record_blit(&fr, im);
        if (last) nvp_release_interop(fr.cmd, im->image);

        VkSwapDiagnostics before, after;
        vk_swapchain_diagnostics(&before);
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        int ok = vk_swapchain_present(&fr, first ? 1 : 0, &im->gl_ready, &wait_stage,
                                      last ? 1 : 0, &im->vk_done);
        if (fr.submitted && last) balanced = 1;
        if (ok) {
            presented++;
            continue;
        }
        vk_swapchain_diagnostics(&after);
        if (!fr.submitted || after.failures != before.failures) {
            /* A failed submit or present: hard failure. */
            hard_fail = 1;
        } else if (!last) {
            /* Out of date: close the interop hand-over with a submit of its own. */
            VkResult vr = nvp_close_handover(im, 0);
            if (vr == VK_SUCCESS) balanced = 1;
            else hard_fail = 1;
        }
        break;
    }

    if (balanced) im->vk_done_pending = 1;
    s_cursor = (s_cursor + 1) % NVP_RING;
    s_begun = 0;
    uint64_t t1 = nvp_now_ns();
    uint64_t dt = t1 - t0;
    if (dt > NVP_GET(s_d_max_block_ns)) NVP_SET(s_d_max_block_ns, dt);
    NVP_SET(s_d_last_present_ns, t1);
    if (presented > 0) NVP_ADD(s_d_frames_presented, 1);
    return (presented > 0 || skipped_minimized) && !hard_fail;
}

int nvp_capture_presented(int timeout_ms, unsigned char **out_rgb, int *out_w, int *out_h)
{
    if (!s_active) {
        if (out_rgb) *out_rgb = NULL;
        if (out_w) *out_w = 0;
        if (out_h) *out_h = 0;
        return 0;
    }
    return vk_swapchain_capture_presented(timeout_ms, out_rgb, out_w, out_h);
}

void nvp_release_gl_context(void)
{
    if (!vk_core_device() || !s_interop_live) return;
    glFinish();
    nvp_destroy_interop(1);
}

void nvp_shutdown(void)
{
    nvp_teardown();
}

/* ---- diagnostics -------------------------------------------------------- */
void nvp_diagnostics(NvpDiagnostics *out)
{
    VkSwapDiagnostics sw;
    vk_swapchain_diagnostics(&sw);
    memset(out, 0, sizeof(*out));
    out->active = s_active;
    out->present_mode = sw.present_mode;
    out->requested_interval = atomic_load(&s_req_interval);
    out->requested_vsync = atomic_load(&s_req_vsync);
    out->swapchain_images = sw.images;
    out->extent_width = sw.extent_width;
    out->extent_height = sw.extent_height;
    out->interop_width = NVP_GET(s_d_int_w);
    out->interop_height = NVP_GET(s_d_int_h);
    out->frames_begun = NVP_GET(s_d_frames_begun);
    out->frames_presented = NVP_GET(s_d_frames_presented);
    out->images_presented = sw.frames_presented;
    out->acquire_timeouts = sw.acquire_timeouts;
    out->out_of_date = sw.out_of_date;
    out->suboptimal = sw.suboptimal;
    out->swapchain_recreations = sw.recreations;
    out->interop_recreations = NVP_GET(s_d_interop_recreations);
    out->minimized_skips = sw.minimized_skips;
    out->failures = NVP_GET(s_d_failures) + sw.failures;
    out->last_present_ns = NVP_GET(s_d_last_present_ns);
    out->max_present_block_ns = NVP_GET(s_d_max_block_ns);
    out->last_vk_result = sw.failures ? sw.last_vk_result : NVP_GET(s_d_last_result);
    out->present_wait = sw.present_wait;
    out->present_depth = sw.present_depth;
    out->present_waits = sw.present_waits;
    out->present_wait_timeouts = sw.present_wait_timeouts;
    out->present_wait_ns_total = sw.present_wait_ns_total;
    out->present_wait_max_ns = sw.present_wait_max_ns;
    out->last_present_id = sw.last_present_id;
}

int nvp_diagnostics_json(char *out, int capacity)
{
    NvpDiagnostics d;
    nvp_diagnostics(&d);
    return snprintf(out, (size_t)capacity,
        "{\"active\":%d,\"present_mode\":%d,\"requested_interval\":%d,\"requested_vsync\":%d,"
        "\"swapchain_images\":%u,\"extent_width\":%u,\"extent_height\":%u,"
        "\"interop_width\":%u,\"interop_height\":%u,"
        "\"frames_begun\":%llu,\"frames_presented\":%llu,\"images_presented\":%llu,"
        "\"acquire_timeouts\":%llu,\"out_of_date\":%llu,\"suboptimal\":%llu,"
        "\"swapchain_recreations\":%llu,\"interop_recreations\":%llu,"
        "\"minimized_skips\":%llu,\"failures\":%llu,"
        "\"last_present_ns\":%llu,\"max_present_block_ns\":%llu,\"last_vk_result\":%d,"
        "\"present_wait\":%d,\"present_depth\":%d,\"present_waits\":%llu,"
        "\"present_wait_timeouts\":%llu,\"present_wait_ns_total\":%llu,"
        "\"present_wait_max_ns\":%llu,\"last_present_id\":%llu,\"probe_failure\":\"%s\"}",
        d.active, d.present_mode, d.requested_interval, d.requested_vsync,
        (unsigned)d.swapchain_images, (unsigned)d.extent_width, (unsigned)d.extent_height,
        (unsigned)d.interop_width, (unsigned)d.interop_height,
        (unsigned long long)d.frames_begun, (unsigned long long)d.frames_presented,
        (unsigned long long)d.images_presented, (unsigned long long)d.acquire_timeouts,
        (unsigned long long)d.out_of_date, (unsigned long long)d.suboptimal,
        (unsigned long long)d.swapchain_recreations, (unsigned long long)d.interop_recreations,
        (unsigned long long)d.minimized_skips, (unsigned long long)d.failures,
        (unsigned long long)d.last_present_ns, (unsigned long long)d.max_present_block_ns,
        d.last_vk_result, d.present_wait, d.present_depth,
        (unsigned long long)d.present_waits, (unsigned long long)d.present_wait_timeouts,
        (unsigned long long)d.present_wait_ns_total, (unsigned long long)d.present_wait_max_ns,
        (unsigned long long)d.last_present_id, s_probe_failure);
}

#endif  /* PSX_HAVE_VULKAN */
