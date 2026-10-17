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

#include <stdatomic.h>
#include "psx_sdl.h"
#if defined(PSX_SDL3)
#include <SDL3/SDL_vulkan.h>
#include <SDL3/SDL_opengl.h>
#else
#include <SDL_vulkan.h>
#include <SDL_opengl.h>
#endif
#if defined(_WIN32)
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#else
#include <unistd.h>
#endif
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#if defined(_WIN32)
#include <vulkan/vulkan_win32.h>
#endif

#define NVP_SAY(...) fprintf(stdout, "psxrecomp: vk-present: " __VA_ARGS__)

#define NVP_RING 3            /* interop images and frames in flight */
#define NVP_MAX_SC 8          /* swapchain images supported */
#define NVP_MAX_GL_DEV 8
#define NVP_ACQUIRE_TIMEOUT_NS 100000000ull
#define NVP_PRESENT_WAIT_TIMEOUT_NS 100000000ull

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

/* ---- Vulkan entry points ------------------------------------------------ */
static PFN_vkGetInstanceProcAddr nvp_vkGetInstanceProcAddr;
static PFN_vkGetDeviceProcAddr   nvp_vkGetDeviceProcAddr;

#define NVP_VK_GLOBAL_FUNCS(X) \
    X(vkCreateInstance)

#define NVP_VK_INSTANCE_FUNCS(X) \
    X(vkDestroyInstance) \
    X(vkEnumeratePhysicalDevices) \
    X(vkGetPhysicalDeviceProperties2) \
    X(vkGetPhysicalDeviceFeatures2) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceFormatProperties) \
    X(vkGetPhysicalDeviceImageFormatProperties2) \
    X(vkGetPhysicalDeviceExternalSemaphoreProperties) \
    X(vkEnumerateDeviceExtensionProperties) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR) \
    X(vkDestroySurfaceKHR) \
    X(vkCreateDevice)

#if defined(_WIN32)
#define NVP_VK_PLATFORM_FUNCS(X) \
    X(vkGetMemoryWin32HandleKHR) \
    X(vkGetSemaphoreWin32HandleKHR)
#else
#define NVP_VK_PLATFORM_FUNCS(X) \
    X(vkGetMemoryFdKHR) \
    X(vkGetSemaphoreFdKHR)
#endif

#define NVP_VK_DEVICE_FUNCS(X) \
    X(vkDestroyDevice) \
    X(vkGetDeviceQueue) \
    X(vkDeviceWaitIdle) \
    X(vkCreateCommandPool) \
    X(vkDestroyCommandPool) \
    X(vkAllocateCommandBuffers) \
    X(vkFreeCommandBuffers) \
    X(vkResetCommandBuffer) \
    X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) \
    X(vkQueueSubmit) \
    X(vkCreateFence) \
    X(vkDestroyFence) \
    X(vkWaitForFences) \
    X(vkResetFences) \
    X(vkCreateSemaphore) \
    X(vkDestroySemaphore) \
    X(vkCreateImage) \
    X(vkDestroyImage) \
    X(vkGetImageMemoryRequirements) \
    X(vkAllocateMemory) \
    X(vkFreeMemory) \
    X(vkBindImageMemory) \
    X(vkCmdPipelineBarrier) \
    X(vkCmdBlitImage) \
    X(vkCmdClearColorImage) \
    X(vkCmdCopyImageToBuffer) \
    X(vkCreateBuffer) \
    X(vkDestroyBuffer) \
    X(vkGetBufferMemoryRequirements) \
    X(vkBindBufferMemory) \
    X(vkMapMemory) \
    X(vkUnmapMemory) \
    X(vkCreateSwapchainKHR) \
    X(vkDestroySwapchainKHR) \
    X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) \
    X(vkQueuePresentKHR) \
    NVP_VK_PLATFORM_FUNCS(X)

#define NVP_DECL(n) static PFN_##n nvp_##n;
NVP_VK_GLOBAL_FUNCS(NVP_DECL)
NVP_VK_INSTANCE_FUNCS(NVP_DECL)
NVP_VK_DEVICE_FUNCS(NVP_DECL)
#undef NVP_DECL
static PFN_vkEnumerateInstanceVersion nvp_vkEnumerateInstanceVersion;
static PFN_vkWaitForPresentKHR nvp_vkWaitForPresentKHR;  /* optional, device level */

/* ---- state -------------------------------------------------------------- */
typedef struct NvpImage {
    VkImage image;
    VkDeviceMemory memory;
    VkSemaphore gl_ready;    /* GL signals, Vulkan waits */
    VkSemaphore vk_done;     /* Vulkan signals, GL waits */
    GLuint tex, fbo, memobj;
    GLuint gl_sem_ready, gl_sem_done;
    int vk_done_pending;     /* Vulkan signaled vk_done, GL has not waited */
} NvpImage;

typedef struct NvpFrame {
    VkCommandBuffer cb;
    VkFence fence;
} NvpFrame;

static int s_probed;                 /* instance + device exist */
static int s_active;
static VkInstance s_instance;
static VkPhysicalDevice s_phys;
static VkDevice s_dev;
static VkQueue s_queue;
static uint32_t s_qfam;
static VkCommandPool s_pool;
static VkPhysicalDeviceMemoryProperties s_memprops;
static int s_has_present_id, s_has_present_wait;
static int s_use_present_wait;       /* present_id + present_wait enabled and loaded */
/* Max not-yet-displayed FIFO images (0 = unbounded, PSX_VK_PRESENT_DEPTH 0-3).
 * Depth 2 (the previous image must be on screen before another is queued) is
 * one refresh shorter, but every compose must then land between that scanout
 * and the compositor's next deadline; on a slow GPU it falls into a stable
 * half-rate pattern. Depth 3 keeps full rate and still drops ~1 refresh of
 * queue latency versus the unbounded 3-image FIFO. */
static int s_present_depth = 3;
static uint64_t s_present_id;        /* last id handed out, strictly increasing */
static uint64_t s_sc_first_id;       /* first id presented on the current swapchain */
static uint64_t s_last_presented_id; /* last id of a successful present, this swapchain */
static int s_sdl_vk_loaded;

static GLubyte s_gl_dev_uuid[NVP_MAX_GL_DEV][NVP_GL_UUID_SIZE_EXT];
static int s_gl_dev_count;
static GLubyte s_gl_drv_uuid[NVP_GL_UUID_SIZE_EXT];

static SDL_Window *s_win;
static VkSurfaceKHR s_surface;
static VkFormat s_sc_format;
static VkSwapchainKHR s_swapchain;
static _Atomic int s_sc_can_read;    /* swapchain images have TRANSFER_SRC */
static VkExtent2D s_extent;
static uint32_t s_sc_count;
static VkImage s_sc_images[NVP_MAX_SC];
static VkSemaphore s_render_done[NVP_MAX_SC];
static VkSemaphore s_acquire_sem[NVP_MAX_SC + 1];
static uint32_t s_acquire_cursor;
static uint32_t s_mode_mask;         /* bit per supported VkPresentModeKHR 0..3 */
static VkPresentModeKHR s_present_mode;
static int s_have_mode;
static int s_win_w, s_win_h;         /* window pixel size at last creation */
static int s_minimized;
static int s_need_recreate;
static uint64_t s_images_since_create;

static NvpFrame s_frames[NVP_RING];
static uint32_t s_frame_cursor;

static NvpImage s_img[NVP_RING];
static int s_interop_live;           /* VK side of the interop images exists */
static int s_gl_live;                /* GL side exists in some context */
static int s_ever_interop;
static int s_iw, s_ih;
static uint32_t s_cursor;
static int s_begun;
static int s_applied_interval = 1;
static int s_applied_vsync = 1;

static _Atomic int s_req_interval = 1;
static _Atomic int s_req_vsync = 1;

/* debug readback handshake: 0 idle, 1 requested, 2 ready, 3 capturing */
static _Atomic int s_cap_state;
static unsigned char *s_cap_rgb;     /* ready result, owned until taken */
static int s_cap_w, s_cap_h;

/* diagnostics: written by the presenter thread, read from any thread */
static _Atomic int s_d_present_mode = -1;
static _Atomic uint32_t s_d_sc_images, s_d_ext_w, s_d_ext_h, s_d_int_w, s_d_int_h;
static _Atomic uint64_t s_d_frames_begun, s_d_frames_presented, s_d_images_presented;
static _Atomic uint64_t s_d_acquire_timeouts, s_d_out_of_date, s_d_suboptimal;
static _Atomic uint64_t s_d_sc_recreations, s_d_interop_recreations;
static _Atomic uint64_t s_d_minimized_skips, s_d_failures;
static _Atomic uint64_t s_d_last_present_ns, s_d_max_block_ns;
static _Atomic int s_d_last_result;
static _Atomic int s_d_present_wait_on, s_d_present_depth;
static _Atomic uint64_t s_d_present_waits, s_d_present_wait_timeouts;
static _Atomic uint64_t s_d_present_wait_ns_total, s_d_present_wait_max_ns, s_d_last_present_id;

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

/* ---- loading ------------------------------------------------------------ */
static int nvp_load_global(void)
{
#define NVP_LOAD(n) nvp_##n = (PFN_##n)nvp_vkGetInstanceProcAddr(NULL, #n); \
    if (!nvp_##n) { NVP_SAY("missing " #n "\n"); return 0; }
    NVP_VK_GLOBAL_FUNCS(NVP_LOAD)
#undef NVP_LOAD
    nvp_vkEnumerateInstanceVersion = (PFN_vkEnumerateInstanceVersion)
        nvp_vkGetInstanceProcAddr(NULL, "vkEnumerateInstanceVersion");
    return 1;
}

static int nvp_load_instance(void)
{
#define NVP_LOAD(n) nvp_##n = (PFN_##n)nvp_vkGetInstanceProcAddr(s_instance, #n); \
    if (!nvp_##n) { NVP_SAY("missing " #n "\n"); return 0; }
    NVP_VK_INSTANCE_FUNCS(NVP_LOAD)
#undef NVP_LOAD
    nvp_vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)
        nvp_vkGetInstanceProcAddr(s_instance, "vkGetDeviceProcAddr");
    if (!nvp_vkGetDeviceProcAddr) { NVP_SAY("missing vkGetDeviceProcAddr\n"); return 0; }
    return 1;
}

static int nvp_load_device(void)
{
#define NVP_LOAD(n) nvp_##n = (PFN_##n)nvp_vkGetDeviceProcAddr(s_dev, #n); \
    if (!nvp_##n) { NVP_SAY("missing " #n "\n"); return 0; }
    NVP_VK_DEVICE_FUNCS(NVP_LOAD)
#undef NVP_LOAD
    return 1;
}

/* ---- GL probe ----------------------------------------------------------- */
static int nvp_gl_load_one(void **slot, const char *name)
{
    *slot = (void *)SDL_GL_GetProcAddress(name);
    if (!*slot) {
        NVP_SAY("GL entry point %s missing\n", name);
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
        NVP_SAY("GL lacks %s%s%s%s\n",
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
        NVP_SAY("GL reports no device UUID\n");
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
    if (im->gl_ready) nvp_vkDestroySemaphore(s_dev, im->gl_ready, NULL);
    if (im->vk_done) nvp_vkDestroySemaphore(s_dev, im->vk_done, NULL);
    if (im->image) nvp_vkDestroyImage(s_dev, im->image, NULL);
    if (im->memory) nvp_vkFreeMemory(s_dev, im->memory, NULL);
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
    if (s_dev) nvp_vkDeviceWaitIdle(s_dev);
    for (int i = 0; i < NVP_RING; ++i) {
        if (with_gl && s_gl_live) nvp_destroy_image_gl(&s_img[i]);
        if (s_dev) nvp_destroy_image_vk(&s_img[i]);
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

static void nvp_destroy_sc_sync(void)
{
    for (uint32_t i = 0; i < NVP_MAX_SC; ++i) {
        if (s_render_done[i]) nvp_vkDestroySemaphore(s_dev, s_render_done[i], NULL);
        s_render_done[i] = VK_NULL_HANDLE;
    }
    for (uint32_t i = 0; i < NVP_MAX_SC + 1; ++i) {
        if (s_acquire_sem[i]) nvp_vkDestroySemaphore(s_dev, s_acquire_sem[i], NULL);
        s_acquire_sem[i] = VK_NULL_HANDLE;
    }
    s_acquire_cursor = 0;
}

static void nvp_teardown(void)
{
    if (s_dev) nvp_vkDeviceWaitIdle(s_dev);
    if (s_interop_live) nvp_destroy_interop(0);
    if (s_dev) {
        nvp_destroy_sc_sync();
        for (int i = 0; i < NVP_RING; ++i) {
            if (s_frames[i].fence) nvp_vkDestroyFence(s_dev, s_frames[i].fence, NULL);
            if (s_frames[i].cb && s_pool) nvp_vkFreeCommandBuffers(s_dev, s_pool, 1, &s_frames[i].cb);
        }
        if (s_swapchain) nvp_vkDestroySwapchainKHR(s_dev, s_swapchain, NULL);
        if (s_pool) nvp_vkDestroyCommandPool(s_dev, s_pool, NULL);
    }
    memset(s_frames, 0, sizeof(s_frames));
    atomic_store(&s_sc_can_read, 0);
    if (s_cap_rgb) free(s_cap_rgb);
    s_cap_rgb = NULL;
    atomic_store(&s_cap_state, 0);
    s_swapchain = VK_NULL_HANDLE;
    s_pool = VK_NULL_HANDLE;
    if (s_surface && s_instance) nvp_vkDestroySurfaceKHR(s_instance, s_surface, NULL);
    s_surface = VK_NULL_HANDLE;
    if (s_dev) nvp_vkDestroyDevice(s_dev, NULL);
    s_dev = VK_NULL_HANDLE;
    s_queue = VK_NULL_HANDLE;
    if (s_instance && nvp_vkDestroyInstance) nvp_vkDestroyInstance(s_instance, NULL);
    s_instance = VK_NULL_HANDLE;
    s_phys = VK_NULL_HANDLE;
    if (s_sdl_vk_loaded) SDL_Vulkan_UnloadLibrary();
    s_sdl_vk_loaded = 0;

    s_probed = 0;
    s_active = 0;
    s_win = NULL;
    s_sc_count = 0;
    s_have_mode = 0;
    s_mode_mask = 0;
    s_minimized = 0;
    s_need_recreate = 0;
    s_win_w = s_win_h = 0;
    s_extent.width = s_extent.height = 0;
    s_frame_cursor = 0;
    s_cursor = 0;
    s_begun = 0;
    s_ever_interop = 0;
    s_images_since_create = 0;
    s_has_present_id = s_has_present_wait = 0;
    s_use_present_wait = 0;
    nvp_vkWaitForPresentKHR = NULL;
    s_sc_first_id = 0;
    s_last_presented_id = 0;
    NVP_SET(s_d_present_wait_on, 0);
    NVP_SET(s_d_present_mode, -1);
    NVP_SET(s_d_sc_images, 0);
    NVP_SET(s_d_ext_w, 0);
    NVP_SET(s_d_ext_h, 0);
}

/* ---- probe -------------------------------------------------------------- */
static int nvp_device_has_ext(const VkExtensionProperties *props, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; ++i)
        if (!strcmp(props[i].extensionName, name)) return 1;
    return 0;
}

/* Checks the chosen physical device for everything the interop path needs
 * and picks the queue family. Prints the reason and returns 0 when unusable. */
static int nvp_check_device(VkPhysicalDevice pd)
{
    uint32_t en = 0;
    nvp_vkEnumerateDeviceExtensionProperties(pd, NULL, &en, NULL);
    VkExtensionProperties *ext = (VkExtensionProperties *)calloc(en ? en : 1, sizeof(*ext));
    if (!ext) return 0;
    nvp_vkEnumerateDeviceExtensionProperties(pd, NULL, &en, ext);
#if defined(_WIN32)
    const char *need[3] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                            VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
                            VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME };
#else
    const char *need[3] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                            VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                            VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME };
#endif
    int ok = 1;
    for (int i = 0; i < 3; ++i) {
        if (!nvp_device_has_ext(ext, en, need[i])) {
            NVP_SAY("Vulkan device lacks %s\n", need[i]);
            ok = 0;
        }
    }
    s_has_present_id = nvp_device_has_ext(ext, en, VK_KHR_PRESENT_ID_EXTENSION_NAME);
    s_has_present_wait = nvp_device_has_ext(ext, en, VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
    free(ext);
    if (!ok) return 0;

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
    if (nvp_vkGetPhysicalDeviceImageFormatProperties2(pd, &ifi, &ifp) != VK_SUCCESS ||
        !(efp.externalMemoryProperties.externalMemoryFeatures &
          VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)) {
        NVP_SAY("Vulkan cannot export RGBA8 images\n");
        return 0;
    }

    VkPhysicalDeviceExternalSemaphoreInfo esi = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO };
    esi.handleType = NVP_VK_SEM_HANDLE;
    VkExternalSemaphoreProperties esp = { VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES };
    nvp_vkGetPhysicalDeviceExternalSemaphoreProperties(pd, &esi, &esp);
    if (!(esp.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT)) {
        NVP_SAY("Vulkan cannot export semaphores\n");
        return 0;
    }

    VkFormatProperties fp;
    nvp_vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_R8G8B8A8_UNORM, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)) {
        NVP_SAY("RGBA8 cannot be a blit source\n");
        return 0;
    }

    uint32_t qn = 0;
    nvp_vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, NULL);
    if (qn > 16) qn = 16;
    VkQueueFamilyProperties qf[16];
    nvp_vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf);
    for (uint32_t f = 0; f < qn; ++f) {
        if (qf[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            s_qfam = f;
            return 1;
        }
    }
    NVP_SAY("no graphics queue family\n");
    return 0;
}

static int nvp_probe_vk(void)
{
#if defined(PSX_SDL3)
    if (!SDL_Vulkan_LoadLibrary(NULL))
#else
    if (SDL_Vulkan_LoadLibrary(NULL) != 0)
#endif
    {
        NVP_SAY("SDL_Vulkan_LoadLibrary failed (no Vulkan ICD?)\n");
        return 0;
    }
    s_sdl_vk_loaded = 1;
    nvp_vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (!nvp_vkGetInstanceProcAddr) {
        NVP_SAY("no vkGetInstanceProcAddr from SDL\n");
        return 0;
    }
    if (!nvp_load_global()) return 0;
    uint32_t ver = VK_API_VERSION_1_0;
    if (nvp_vkEnumerateInstanceVersion) nvp_vkEnumerateInstanceVersion(&ver);
    if (ver < VK_API_VERSION_1_1) {
        NVP_SAY("Vulkan instance older than 1.1\n");
        return 0;
    }

    unsigned ext_count = 0;
    const char *exts[16];
#if defined(PSX_SDL3)
    const char *const *sdl_exts = SDL_Vulkan_GetInstanceExtensions(&ext_count);
    if (!sdl_exts) {
        NVP_SAY("SDL_Vulkan_GetInstanceExtensions failed\n");
        return 0;
    }
    if (ext_count > 16) ext_count = 16;
    for (unsigned i = 0; i < ext_count; ++i) exts[i] = sdl_exts[i];
#else
    if (!SDL_Vulkan_GetInstanceExtensions(NULL, &ext_count, NULL)) {
        NVP_SAY("SDL_Vulkan_GetInstanceExtensions(count) failed\n");
        return 0;
    }
    if (ext_count > 16) ext_count = 16;
    if (!SDL_Vulkan_GetInstanceExtensions(NULL, &ext_count, exts)) {
        NVP_SAY("SDL_Vulkan_GetInstanceExtensions(list) failed\n");
        return 0;
    }
#endif
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "psxrecomp";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = ext_count;
    ici.ppEnabledExtensionNames = exts;
    if (nvp_vkCreateInstance(&ici, NULL, &s_instance) != VK_SUCCESS) {
        NVP_SAY("vkCreateInstance failed\n");
        return 0;
    }
    if (!nvp_load_instance()) return 0;

    uint32_t n = 0;
    nvp_vkEnumeratePhysicalDevices(s_instance, &n, NULL);
    if (n > 8) n = 8;
    VkPhysicalDevice devs[8];
    nvp_vkEnumeratePhysicalDevices(s_instance, &n, devs);
    VkPhysicalDevice match = VK_NULL_HANDLE;
    char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] = "";
    for (uint32_t i = 0; i < n && !match; ++i) {
        VkPhysicalDeviceIDProperties idp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
        VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        p2.pNext = &idp;
        nvp_vkGetPhysicalDeviceProperties2(devs[i], &p2);
        if (p2.properties.apiVersion < VK_API_VERSION_1_1) continue;
        if (memcmp(idp.driverUUID, s_gl_drv_uuid, VK_UUID_SIZE) != 0) continue;
        for (int g = 0; g < s_gl_dev_count; ++g) {
            if (!memcmp(idp.deviceUUID, s_gl_dev_uuid[g], VK_UUID_SIZE)) {
                match = devs[i];
                snprintf(name, sizeof(name), "%s", p2.properties.deviceName);
                break;
            }
        }
    }
    if (!match) {
        NVP_SAY("GL and Vulkan devices differ\n");
        return 0;
    }
    s_phys = match;
    if (!nvp_check_device(s_phys)) return 0;
    nvp_vkGetPhysicalDeviceMemoryProperties(s_phys, &s_memprops);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo q = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    q.queueFamilyIndex = s_qfam;
    q.queueCount = 1;
    q.pQueuePriorities = &prio;
#if defined(_WIN32)
    const char *dev_exts[5] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                                VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
                                VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME };
#else
    const char *dev_exts[5] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                                VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                                VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME };
#endif
    uint32_t dev_ext_count = 3;
    VkPhysicalDevicePresentIdFeaturesKHR pid_f = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR };
    VkPhysicalDevicePresentWaitFeaturesKHR pw_f = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR };
    int want_pw = 0;
    if (s_has_present_id && s_has_present_wait) {
        VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        pid_f.pNext = &pw_f;
        f2.pNext = &pid_f;
        nvp_vkGetPhysicalDeviceFeatures2(s_phys, &f2);
        if (pid_f.presentId == VK_TRUE && pw_f.presentWait == VK_TRUE) {
            want_pw = 1;
            dev_exts[dev_ext_count++] = VK_KHR_PRESENT_ID_EXTENSION_NAME;
            dev_exts[dev_ext_count++] = VK_KHR_PRESENT_WAIT_EXTENSION_NAME;
        }
    }
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &q;
    dci.enabledExtensionCount = dev_ext_count;
    dci.ppEnabledExtensionNames = dev_exts;
    if (want_pw) {
        /* Features are enabled by chaining the structs the query filled in. */
        pid_f.presentId = VK_TRUE;
        pw_f.presentWait = VK_TRUE;
        pw_f.pNext = NULL;
        pid_f.pNext = &pw_f;
        dci.pNext = &pid_f;
    }
    if (nvp_vkCreateDevice(s_phys, &dci, NULL, &s_dev) != VK_SUCCESS) {
        s_dev = VK_NULL_HANDLE;
        NVP_SAY("vkCreateDevice failed\n");
        return 0;
    }
    if (!nvp_load_device()) {
        if (nvp_vkDestroyDevice) nvp_vkDestroyDevice(s_dev, NULL);
        s_dev = VK_NULL_HANDLE;
        return 0;
    }
    nvp_vkGetDeviceQueue(s_dev, s_qfam, 0, &s_queue);
    if (want_pw) {
        nvp_vkWaitForPresentKHR = (PFN_vkWaitForPresentKHR)
            nvp_vkGetDeviceProcAddr(s_dev, "vkWaitForPresentKHR");
        s_use_present_wait = nvp_vkWaitForPresentKHR != NULL;
    }
    NVP_SET(s_d_present_wait_on, s_use_present_wait);

    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = s_qfam;
    if (nvp_vkCreateCommandPool(s_dev, &pci, NULL, &s_pool) != VK_SUCCESS) {
        s_pool = VK_NULL_HANDLE;
        NVP_SAY("vkCreateCommandPool failed\n");
        return 0;
    }
    NVP_SAY("device = %s (present_id=%d present_wait=%d)\n", name,
            s_has_present_id, s_has_present_wait);
    if (s_use_present_wait) NVP_SAY("present_wait queue depth %d\n", s_present_depth);
    else NVP_SAY("present_wait unavailable\n");
    return 1;
}

int nvp_probe(void)
{
    if (s_probed) return 1;
    const char *env = getenv("PSX_VK_PRESENT");
    if (env && env[0] == '0') {
        NVP_SAY("disabled by PSX_VK_PRESENT=0\n");
        return 0;
    }
    if (!nvp_probe_gl()) return 0;
    const char *denv = getenv("PSX_VK_PRESENT_DEPTH");
    s_present_depth = 3;
    if (denv && denv[0] >= '0' && denv[0] <= '3' && !denv[1])
        s_present_depth = denv[0] - '0';
    NVP_SET(s_d_present_depth, s_present_depth);
    if (!nvp_probe_vk()) {
        nvp_teardown();
        return 0;
    }
    s_probed = 1;
    return 1;
}

/* ---- swapchain ---------------------------------------------------------- */
static VkPresentModeKHR nvp_pick_mode(int interval, int user_vsync)
{
#define NVP_HAS(m) ((s_mode_mask >> (m)) & 1u)
    if (interval >= 1) return VK_PRESENT_MODE_FIFO_KHR;
    if (interval == -1)
        return NVP_HAS(VK_PRESENT_MODE_FIFO_RELAXED_KHR) ? VK_PRESENT_MODE_FIFO_RELAXED_KHR
                                                         : VK_PRESENT_MODE_FIFO_KHR;
    if (user_vsync != 0) {
        if (NVP_HAS(VK_PRESENT_MODE_MAILBOX_KHR)) return VK_PRESENT_MODE_MAILBOX_KHR;
        if (NVP_HAS(VK_PRESENT_MODE_IMMEDIATE_KHR)) return VK_PRESENT_MODE_IMMEDIATE_KHR;
        return VK_PRESENT_MODE_FIFO_KHR;
    }
    if (NVP_HAS(VK_PRESENT_MODE_IMMEDIATE_KHR)) return VK_PRESENT_MODE_IMMEDIATE_KHR;
    if (NVP_HAS(VK_PRESENT_MODE_MAILBOX_KHR)) return VK_PRESENT_MODE_MAILBOX_KHR;
    return VK_PRESENT_MODE_FIFO_KHR;
#undef NVP_HAS
}

static uint32_t nvp_clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* (Re)creates the swapchain for the current window size and present policy.
 * Returns 1 on success, 2 when the window is minimized (no swapchain), 0 on
 * failure (the previous swapchain, if any, is kept). */
static int nvp_create_swapchain(void)
{
    VkSurfaceCapabilitiesKHR caps;
    VkResult r = nvp_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s_phys, s_surface, &caps);
    if (r != VK_SUCCESS) { nvp_fail(r); return 0; }

    uint32_t mn = 0;
    nvp_vkGetPhysicalDeviceSurfacePresentModesKHR(s_phys, s_surface, &mn, NULL);
    if (mn > 16) mn = 16;
    VkPresentModeKHR modes[16];
    nvp_vkGetPhysicalDeviceSurfacePresentModesKHR(s_phys, s_surface, &mn, modes);
    s_mode_mask = 0;
    for (uint32_t i = 0; i < mn; ++i)
        if ((uint32_t)modes[i] < 32) s_mode_mask |= 1u << (uint32_t)modes[i];

    int ww = 0, wh = 0;
    SDL_GetWindowSizeInPixels(s_win, &ww, &wh);
    s_win_w = ww;
    s_win_h = wh;
    VkExtent2D ext;
    if (caps.currentExtent.width != 0xFFFFFFFFu) {
        ext = caps.currentExtent;
    } else {
        ext.width = nvp_clamp_u32((uint32_t)(ww > 0 ? ww : 0), caps.minImageExtent.width,
                                  caps.maxImageExtent.width);
        ext.height = nvp_clamp_u32((uint32_t)(wh > 0 ? wh : 0), caps.minImageExtent.height,
                                   caps.maxImageExtent.height);
        if (ww <= 0 || wh <= 0) ext.width = ext.height = 0;
    }

    nvp_vkDeviceWaitIdle(s_dev);
    if (ext.width == 0 || ext.height == 0) {
        if (s_swapchain) {
            nvp_vkDestroySwapchainKHR(s_dev, s_swapchain, NULL);
            s_swapchain = VK_NULL_HANDLE;
        }
        nvp_destroy_sc_sync();
        s_sc_count = 0;
        s_have_mode = 0;
        s_minimized = 1;
        s_need_recreate = 0;
        NVP_SET(s_d_present_mode, -1);
        NVP_SET(s_d_sc_images, 0);
        return 2;
    }
    s_minimized = 0;

    uint32_t want_images = caps.minImageCount > 3 ? caps.minImageCount : 3;
    if (caps.maxImageCount != 0 && want_images > caps.maxImageCount) want_images = caps.maxImageCount;
    if (want_images > NVP_MAX_SC) want_images = NVP_MAX_SC;
    if (want_images < caps.minImageCount) {
        NVP_SAY("surface needs %u swapchain images, more than supported\n", caps.minImageCount);
        nvp_fail(VK_ERROR_INITIALIZATION_FAILED);
        return 0;
    }
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        NVP_SAY("swapchain images cannot be transfer destinations\n");
        nvp_fail(VK_ERROR_INITIALIZATION_FAILED);
        return 0;
    }

    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) {
        for (uint32_t b = 1; b <= 8; b <<= 1) {
            if (caps.supportedCompositeAlpha & b) { alpha = (VkCompositeAlphaFlagBitsKHR)b; break; }
        }
    }

    VkPresentModeKHR mode = nvp_pick_mode(s_applied_interval, s_applied_vsync);
    VkSwapchainKHR old = s_swapchain;
    VkSwapchainCreateInfoKHR sci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    sci.surface = s_surface;
    sci.minImageCount = want_images;
    sci.imageFormat = s_sc_format;
    sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    sci.imageExtent = ext;
    sci.imageArrayLayers = 1;
    int can_read = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     (can_read ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = alpha;
    sci.presentMode = mode;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = old;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    r = nvp_vkCreateSwapchainKHR(s_dev, &sci, NULL, &sc);
    if (r != VK_SUCCESS) { nvp_fail(r); return 0; }
    if (old) nvp_vkDestroySwapchainKHR(s_dev, old, NULL);
    s_swapchain = sc;
    /* Present ids of the old swapchain mean nothing to the new one. */
    s_sc_first_id = 0;
    s_last_presented_id = 0;
    atomic_store(&s_sc_can_read, can_read);
    nvp_destroy_sc_sync();

    uint32_t cnt = 0;
    nvp_vkGetSwapchainImagesKHR(s_dev, sc, &cnt, NULL);
    if (cnt > NVP_MAX_SC) {
        NVP_SAY("swapchain returned %u images, more than supported\n", cnt);
        nvp_vkDestroySwapchainKHR(s_dev, sc, NULL);
        s_swapchain = VK_NULL_HANDLE;
        s_sc_count = 0;
        nvp_fail(VK_ERROR_INITIALIZATION_FAILED);
        return 0;
    }
    nvp_vkGetSwapchainImagesKHR(s_dev, sc, &cnt, s_sc_images);
    s_sc_count = cnt;
    VkSemaphoreCreateInfo semi = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (uint32_t i = 0; i < cnt; ++i) {
        if (nvp_vkCreateSemaphore(s_dev, &semi, NULL, &s_render_done[i]) != VK_SUCCESS) {
            s_need_recreate = 1;
            nvp_fail(VK_ERROR_INITIALIZATION_FAILED);
            return 0;
        }
    }
    for (uint32_t i = 0; i < cnt + 1; ++i) {
        if (nvp_vkCreateSemaphore(s_dev, &semi, NULL, &s_acquire_sem[i]) != VK_SUCCESS) {
            s_need_recreate = 1;
            nvp_fail(VK_ERROR_INITIALIZATION_FAILED);
            return 0;
        }
    }
    s_extent = ext;
    s_present_mode = mode;
    s_have_mode = 1;
    s_need_recreate = 0;
    s_images_since_create = 0;
    NVP_SET(s_d_present_mode, (int)mode);
    NVP_SET(s_d_sc_images, cnt);
    NVP_SET(s_d_ext_w, ext.width);
    NVP_SET(s_d_ext_h, ext.height);
    NVP_ADD(s_d_sc_recreations, 1);
    return 1;
}

/* Applies pending policy and size changes. Called at the top of each frame. */
static void nvp_refresh_swapchain(void)
{
    int ww = 0, wh = 0;
    SDL_GetWindowSizeInPixels(s_win, &ww, &wh);
    int need = s_need_recreate || !s_swapchain || ww != s_win_w || wh != s_win_h ||
               (s_have_mode && nvp_pick_mode(s_applied_interval, s_applied_vsync) != s_present_mode);
    if (need) (void)nvp_create_swapchain();
}

/* ---- command helpers ---------------------------------------------------- */
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
    nvp_vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

static void nvp_acquire_interop(VkCommandBuffer cb, VkImage img)
{
    nvp_barrier(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                0, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_QUEUE_FAMILY_EXTERNAL, s_qfam);
}

static void nvp_release_interop(VkCommandBuffer cb, VkImage img)
{
    nvp_barrier(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_READ_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s_qfam, VK_QUEUE_FAMILY_EXTERNAL);
}

static VkResult nvp_wait_reset_frame(NvpFrame *f)
{
    VkResult r = nvp_vkWaitForFences(s_dev, 1, &f->fence, VK_TRUE, UINT64_MAX);
    if (r != VK_SUCCESS) return r;
    r = nvp_vkResetFences(s_dev, 1, &f->fence);
    if (r != VK_SUCCESS) return r;
    return nvp_vkResetCommandBuffer(f->cb, 0);
}

/* ---- interop images ----------------------------------------------------- */
static int nvp_find_memory_type(uint32_t bits)
{
    for (uint32_t i = 0; i < s_memprops.memoryTypeCount; ++i)
        if ((bits & (1u << i)) &&
            (s_memprops.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            return (int)i;
    return -1;
}

/* Host-visible, host-coherent memory for the debug readback buffer. */
static int nvp_find_host_memory_type(uint32_t bits)
{
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < s_memprops.memoryTypeCount; ++i)
        if ((bits & (1u << i)) &&
            (s_memprops.memoryTypes[i].propertyFlags & want) == want)
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
    if (nvp_vkCreateSemaphore(s_dev, &ci, NULL, vs) != VK_SUCCESS) { *vs = VK_NULL_HANDLE; return 0; }
#if defined(_WIN32)
    VkSemaphoreGetWin32HandleInfoKHR gi = { VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR };
    gi.semaphore = *vs;
    gi.handleType = NVP_VK_SEM_HANDLE;
    HANDLE h = NULL;
    if (nvp_vkGetSemaphoreWin32HandleKHR(s_dev, &gi, &h) != VK_SUCCESS) return 0;
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
    if (nvp_vkGetSemaphoreFdKHR(s_dev, &gi, &fd) != VK_SUCCESS) return 0;
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
    if (nvp_vkCreateImage(s_dev, &ci, NULL, &im->image) != VK_SUCCESS) {
        im->image = VK_NULL_HANDLE;
        return 0;
    }
    VkMemoryRequirements req;
    nvp_vkGetImageMemoryRequirements(s_dev, im->image, &req);
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
    if (nvp_vkAllocateMemory(s_dev, &ai, NULL, &im->memory) != VK_SUCCESS) {
        im->memory = VK_NULL_HANDLE;
        return 0;
    }
    if (nvp_vkBindImageMemory(s_dev, im->image, im->memory, 0) != VK_SUCCESS) return 0;

    nvp_gl_error_pending();
    nvp_glCreateMemoryObjectsEXT(1, &im->memobj);
    GLint one = 1;
    nvp_glMemoryObjectParameterivEXT(im->memobj, NVP_GL_DEDICATED_MEMORY_OBJECT_EXT, &one);
#if defined(_WIN32)
    VkMemoryGetWin32HandleInfoKHR gi = { VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR };
    gi.memory = im->memory;
    gi.handleType = NVP_VK_MEM_HANDLE;
    HANDLE hnd = NULL;
    if (nvp_vkGetMemoryWin32HandleKHR(s_dev, &gi, &hnd) != VK_SUCCESS) return 0;
    nvp_glImportMemoryEXT(im->memobj, (GLuint64)req.size, NVP_GL_HANDLE_TYPE, (void *)hnd);
    CloseHandle(hnd);  /* the Win32 import duplicates the handle */
    if (nvp_gl_error_pending()) return 0;
#else
    VkMemoryGetFdInfoKHR gi = { VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR };
    gi.memory = im->memory;
    gi.handleType = NVP_VK_MEM_HANDLE;
    int fd = -1;
    if (nvp_vkGetMemoryFdKHR(s_dev, &gi, &fd) != VK_SUCCESS) return 0;
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
    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = s_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (nvp_vkAllocateCommandBuffers(s_dev, &ai, &cb) != VK_SUCCESS) return 0;
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = VK_NULL_HANDLE;
    int ok = 0;
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (nvp_vkCreateFence(s_dev, &fci, NULL, &fence) != VK_SUCCESS) goto out;
    if (nvp_vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) goto out;
    for (int i = 0; i < NVP_RING; ++i) {
        nvp_barrier(cb, s_img[i].image, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
        nvp_release_interop(cb, s_img[i].image);
    }
    if (nvp_vkEndCommandBuffer(cb) != VK_SUCCESS) goto out;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    if (nvp_vkQueueSubmit(s_queue, 1, &si, fence) != VK_SUCCESS) goto out;
    ok = nvp_vkWaitForFences(s_dev, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
out:
    if (fence) nvp_vkDestroyFence(s_dev, fence, NULL);
    nvp_vkFreeCommandBuffers(s_dev, s_pool, 1, &cb);
    return ok;
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
}

int nvp_active(void) { return s_active; }

/* ---- attach ------------------------------------------------------------- */
/* Presents one cleared frame so the compositor maps the window. */
static int nvp_present_black(void)
{
    NvpFrame *f = &s_frames[0];
    uint32_t ii = 0;
    VkSemaphore acq = s_acquire_sem[s_acquire_cursor];
    s_acquire_cursor = (s_acquire_cursor + 1) % (s_sc_count + 1);
    VkResult r = nvp_vkAcquireNextImageKHR(s_dev, s_swapchain, 1000000000ull, acq, VK_NULL_HANDLE, &ii);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { nvp_fail(r); return 0; }
    if (nvp_wait_reset_frame(f) != VK_SUCCESS) return 0;
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    nvp_vkBeginCommandBuffer(f->cb, &bi);
    nvp_barrier(f->cb, s_sc_images[ii], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
    VkClearColorValue black = { { 0.0f, 0.0f, 0.0f, 1.0f } };
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    nvp_vkCmdClearColorImage(f->cb, s_sc_images[ii], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    nvp_barrier(f->cb, s_sc_images[ii], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
    nvp_vkEndCommandBuffer(f->cb);
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &acq;
    si.pWaitDstStageMask = &stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f->cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &s_render_done[ii];
    r = nvp_vkQueueSubmit(s_queue, 1, &si, f->fence);
    if (r != VK_SUCCESS) { nvp_fail(r); return 0; }
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &s_render_done[ii];
    pi.swapchainCount = 1;
    pi.pSwapchains = &s_swapchain;
    pi.pImageIndices = &ii;
    VkPresentIdKHR pid = { VK_STRUCTURE_TYPE_PRESENT_ID_KHR };
    uint64_t pres_id = 0;
    if (s_use_present_wait) {
        pres_id = ++s_present_id;
        pid.swapchainCount = 1;
        pid.pPresentIds = &pres_id;
        pi.pNext = &pid;
    }
    r = nvp_vkQueuePresentKHR(s_queue, &pi);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { nvp_fail(r); return 0; }
    if (pres_id) {
        if (!s_sc_first_id) s_sc_first_id = pres_id;
        s_last_presented_id = pres_id;
        NVP_SET(s_d_last_present_id, pres_id);
    }
    return nvp_vkWaitForFences(s_dev, 1, &f->fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
}

static int nvp_attach_impl(SDL_Window *window)
{
    if (!s_probed || !window) return 0;
    s_win = window;
#if defined(PSX_SDL3)
    if (!SDL_Vulkan_CreateSurface(window, s_instance, NULL, &s_surface))
#else
    if (!SDL_Vulkan_CreateSurface(window, s_instance, &s_surface))
#endif
    {
        s_surface = VK_NULL_HANDLE;
        NVP_SAY("SDL_Vulkan_CreateSurface failed\n");
        return 0;
    }
    VkBool32 present = VK_FALSE;
    nvp_vkGetPhysicalDeviceSurfaceSupportKHR(s_phys, s_qfam, s_surface, &present);
    if (!present) {
        NVP_SAY("queue family cannot present to the window\n");
        return 0;
    }

    uint32_t fn = 0;
    nvp_vkGetPhysicalDeviceSurfaceFormatsKHR(s_phys, s_surface, &fn, NULL);
    if (fn > 64) fn = 64;
    VkSurfaceFormatKHR fmts[64];
    nvp_vkGetPhysicalDeviceSurfaceFormatsKHR(s_phys, s_surface, &fn, fmts);
    static const VkFormat prefer[2] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM };
    int found = 0;
    for (int p = 0; p < 2 && !found; ++p) {
        for (uint32_t i = 0; i < fn; ++i) {
            if (fmts[i].format == prefer[p] &&
                fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                s_sc_format = prefer[p];
                found = 1;
                break;
            }
        }
    }
    if (!found) {
        NVP_SAY("surface offers no UNORM format\n");
        return 0;
    }
    VkFormatProperties fp;
    nvp_vkGetPhysicalDeviceFormatProperties(s_phys, s_sc_format, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        NVP_SAY("swapchain format cannot be a blit destination\n");
        return 0;
    }

    /* Frame ring: command buffers and fences (created signaled). */
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = s_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (int i = 0; i < NVP_RING; ++i) {
        if (nvp_vkAllocateCommandBuffers(s_dev, &cai, &s_frames[i].cb) != VK_SUCCESS) {
            s_frames[i].cb = VK_NULL_HANDLE;
            return 0;
        }
        if (nvp_vkCreateFence(s_dev, &fci, NULL, &s_frames[i].fence) != VK_SUCCESS) {
            s_frames[i].fence = VK_NULL_HANDLE;
            return 0;
        }
    }

    s_applied_interval = atomic_load(&s_req_interval);
    s_applied_vsync = atomic_load(&s_req_vsync);
    if (nvp_create_swapchain() == 0) {
        NVP_SAY("swapchain creation failed\n");
        return 0;
    }
    if (s_swapchain && !nvp_present_black()) {
        NVP_SAY("first present failed\n");
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
    NVP_SAY("swapchain %ux%u, %u images, present mode %d\n", s_extent.width, s_extent.height,
            s_sc_count, s_have_mode ? (int)s_present_mode : -1);
    return 1;
}

/* ---- frame -------------------------------------------------------------- */
unsigned int nvp_begin_frame(int width, int height)
{
    if (!s_active) return 0;
    if (s_begun) return s_img[s_cursor].fbo;
    if (width <= 0 || height <= 0) return 0;

    s_applied_interval = atomic_load(&s_req_interval);
    s_applied_vsync = atomic_load(&s_req_vsync);
    nvp_refresh_swapchain();

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

/* Records and submits one command buffer for presentation repeat `r`.
 * have_image: a swapchain image (index ii, acquire semaphore acq) takes the
 * blit; without it only the interop hand-over is submitted so the binary
 * semaphores stay balanced. first: wait gl_ready and acquire the image from
 * GL's queue family. last: release it back and signal vk_done. cap_buf: a
 * host-readable buffer that receives the presented image (debug readback),
 * or VK_NULL_HANDLE. */
static VkResult nvp_submit_repeat(NvpFrame *f, NvpImage *im, int have_image, uint32_t ii,
                                  VkSemaphore acq, int first, int last, VkBuffer cap_buf)
{
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkResult r = nvp_vkBeginCommandBuffer(f->cb, &bi);
    if (r != VK_SUCCESS) return r;
    if (first) nvp_acquire_interop(f->cb, im->image);
    if (have_image) {
        VkImage sc = s_sc_images[ii];
        nvp_barrier(f->cb, sc, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
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
        blit.dstOffsets[1].x = (int32_t)s_extent.width;
        blit.dstOffsets[1].y = (int32_t)s_extent.height;
        blit.dstOffsets[1].z = 1;
        VkFilter filter = ((uint32_t)s_iw == s_extent.width && (uint32_t)s_ih == s_extent.height)
                              ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        nvp_vkCmdBlitImage(f->cb, im->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, filter);
        if (cap_buf != VK_NULL_HANDLE) {
            /* Debug readback: copy the presented image into cap_buf, then
             * hand it to present as before. */
            nvp_barrier(f->cb, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
            VkBufferImageCopy region;
            memset(&region, 0, sizeof(region));
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent.width = s_extent.width;
            region.imageExtent.height = s_extent.height;
            region.imageExtent.depth = 1;
            nvp_vkCmdCopyImageToBuffer(f->cb, sc, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, cap_buf, 1, &region);
            nvp_barrier(f->cb, sc, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        0, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
            VkBufferMemoryBarrier bb = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
            bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            bb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bb.buffer = cap_buf;
            bb.offset = 0;
            bb.size = VK_WHOLE_SIZE;
            nvp_vkCmdPipelineBarrier(f->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                     0, 0, NULL, 1, &bb, 0, NULL);
        } else {
            nvp_barrier(f->cb, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_QUEUE_FAMILY_IGNORED,
                        VK_QUEUE_FAMILY_IGNORED);
        }
    }
    if (last) nvp_release_interop(f->cb, im->image);
    r = nvp_vkEndCommandBuffer(f->cb);
    if (r != VK_SUCCESS) return r;

    VkSemaphore waits[2], signals[2];
    VkPipelineStageFlags stages[2] = { VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT };
    uint32_t nw = 0, ns = 0;
    if (first) waits[nw++] = im->gl_ready;
    if (have_image) waits[nw++] = acq;
    if (have_image) signals[ns++] = s_render_done[ii];
    if (last) signals[ns++] = im->vk_done;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = nw;
    si.pWaitSemaphores = waits;
    si.pWaitDstStageMask = stages;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f->cb;
    si.signalSemaphoreCount = ns;
    si.pSignalSemaphores = signals;
    return nvp_vkQueueSubmit(s_queue, 1, &si, f->fence);
}

/* ---- debug readback ----------------------------------------------------- */
static void nvp_cap_free(VkBuffer buf, VkDeviceMemory mem)
{
    if (buf) nvp_vkDestroyBuffer(s_dev, buf, NULL);
    if (mem) nvp_vkFreeMemory(s_dev, mem, NULL);
}

/* Presenter thread. Creates the host-readable buffer that receives one
 * presented swapchain image. Returns 0 with both handles null on failure. */
static int nvp_cap_create(VkBuffer *buf, VkDeviceMemory *mem)
{
    *buf = VK_NULL_HANDLE;
    *mem = VK_NULL_HANDLE;
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = (VkDeviceSize)s_extent.width * s_extent.height * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (bci.size == 0 || nvp_vkCreateBuffer(s_dev, &bci, NULL, buf) != VK_SUCCESS) {
        *buf = VK_NULL_HANDLE;
        return 0;
    }
    int ok = 0;
    VkMemoryRequirements req;
    nvp_vkGetBufferMemoryRequirements(s_dev, *buf, &req);
    int type = nvp_find_host_memory_type(req.memoryTypeBits);
    if (type >= 0) {
        VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = (uint32_t)type;
        if (nvp_vkAllocateMemory(s_dev, &ai, NULL, mem) == VK_SUCCESS &&
            nvp_vkBindBufferMemory(s_dev, *buf, *mem, 0) == VK_SUCCESS)
            ok = 1;
    }
    if (!ok) {
        nvp_cap_free(*buf, *mem);
        *buf = VK_NULL_HANDLE;
        *mem = VK_NULL_HANDLE;
    }
    return ok;
}

/* Presenter thread, after nvp_submit_repeat that copied into buf returned
 * submit_r. Waits for that submit (the fence is not reset), converts the
 * pixels to top-down RGB8, frees buf and publishes the result. Always frees
 * buf, and always leaves s_cap_state at 2 (ready) or 0 (failed). */
static void nvp_cap_finish(NvpFrame *f, VkBuffer buf, VkDeviceMemory mem, VkResult submit_r)
{
    unsigned char *rgb = NULL;
    int w = (int)s_extent.width, h = (int)s_extent.height;
    int ok = 0;
    if (submit_r == VK_SUCCESS) {
        VkResult wr = nvp_vkWaitForFences(s_dev, 1, &f->fence, VK_TRUE, 1000000000ull);
        if (wr != VK_SUCCESS) {
            nvp_vkDeviceWaitIdle(s_dev);  /* the copy may still be using buf */
        } else {
            void *p = NULL;
            if (nvp_vkMapMemory(s_dev, mem, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS) {
                size_t n = (size_t)w * (size_t)h;
                rgb = (unsigned char *)malloc(n * 3);
                if (rgb) {
                    const unsigned char *src = (const unsigned char *)p;
                    int bgr = s_sc_format == VK_FORMAT_B8G8R8A8_UNORM;
                    for (size_t i = 0; i < n; ++i) {
                        rgb[i * 3 + 0] = src[i * 4 + (bgr ? 2 : 0)];
                        rgb[i * 3 + 1] = src[i * 4 + 1];
                        rgb[i * 3 + 2] = src[i * 4 + (bgr ? 0 : 2)];
                    }
                    ok = 1;
                }
                nvp_vkUnmapMemory(s_dev, mem);
            }
        }
    }
    nvp_cap_free(buf, mem);
    if (!ok) {
        free(rgb);
        atomic_store(&s_cap_state, 0);
        return;
    }
    if (s_cap_rgb) free(s_cap_rgb);
    s_cap_rgb = rgb;
    s_cap_w = w;
    s_cap_h = h;
    atomic_store(&s_cap_state, 2);
}

int nvp_capture_presented(int timeout_ms, unsigned char **out_rgb, int *out_w, int *out_h)
{
    if (!out_rgb || !out_w || !out_h) return 0;
    *out_rgb = NULL;
    *out_w = 0;
    *out_h = 0;
    if (!s_active || !atomic_load(&s_sc_can_read)) return 0;
    int expect = 0;
    if (!atomic_compare_exchange_strong(&s_cap_state, &expect, 1)) return 0;

    uint64_t deadline = nvp_now_ns() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0) * 1000000ull;
    int st = atomic_load(&s_cap_state);
    while (st == 1 && nvp_now_ns() < deadline) {
        SDL_Delay(2);
        st = atomic_load(&s_cap_state);
    }
    if (st == 1) {
        expect = 1;
        if (atomic_compare_exchange_strong(&s_cap_state, &expect, 0)) return 0;
        st = expect;  /* the presenter moved on before the timeout was taken */
    }
    if (st == 3) {
        deadline = nvp_now_ns() + 1000ull * 1000000ull;
        while ((st = atomic_load(&s_cap_state)) == 3 && nvp_now_ns() < deadline)
            SDL_Delay(2);
        /* The presenter bounds its own work, so it always leaves 3. Keep
         * waiting so its result is consumed here and no stale 2 remains. */
        while (st == 3) {
            SDL_Delay(2);
            st = atomic_load(&s_cap_state);
        }
    }
    if (st != 2) return 0;

    *out_rgb = s_cap_rgb;
    *out_w = s_cap_w;
    *out_h = s_cap_h;
    s_cap_rgb = NULL;
    atomic_store(&s_cap_state, 0);
    return 1;
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
        NvpFrame *f = &s_frames[s_frame_cursor];
        s_frame_cursor = (s_frame_cursor + 1) % NVP_RING;
        VkResult vr = nvp_wait_reset_frame(f);
        if (vr != VK_SUCCESS) { nvp_fail(vr); hard_fail = 1; break; }

        int have_image = 0;
        uint32_t ii = 0;
        VkSemaphore acq = VK_NULL_HANDLE;
        if (!s_swapchain) {
            NVP_ADD(s_d_minimized_skips, 1);
            skipped_minimized = 1;
        } else {
            if (s_use_present_wait && s_present_depth > 0 &&
                (s_present_mode == VK_PRESENT_MODE_FIFO_KHR ||
                 s_present_mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR) &&
                s_last_presented_id != 0 &&
                s_last_presented_id > (uint64_t)(s_present_depth - 1)) {
                /* Bound the not-yet-displayed images: wait for the image
                 * queued depth presents ago to reach the screen. */
                uint64_t target = s_last_presented_id - (uint64_t)(s_present_depth - 1);
                if (target > 0 && target >= s_sc_first_id) {
                    uint64_t w0 = nvp_now_ns();
                    VkResult wr = nvp_vkWaitForPresentKHR(s_dev, s_swapchain, target,
                                                          NVP_PRESENT_WAIT_TIMEOUT_NS);
                    uint64_t wdt = nvp_now_ns() - w0;
                    NVP_ADD(s_d_present_waits, 1);
                    NVP_ADD(s_d_present_wait_ns_total, wdt);
                    if (wdt > NVP_GET(s_d_present_wait_max_ns)) NVP_SET(s_d_present_wait_max_ns, wdt);
                    if (wr == VK_TIMEOUT) {
                        NVP_ADD(s_d_present_wait_timeouts, 1);
                    } else if (wr == VK_ERROR_OUT_OF_DATE_KHR) {
                        NVP_ADD(s_d_out_of_date, 1);
                        s_need_recreate = 1;
                    } else if (wr != VK_SUCCESS && wr != VK_SUBOPTIMAL_KHR) {
                        nvp_fail(wr);
                    }
                }
            }
            acq = s_acquire_sem[s_acquire_cursor];
            vr = nvp_vkAcquireNextImageKHR(s_dev, s_swapchain, NVP_ACQUIRE_TIMEOUT_NS, acq,
                                           VK_NULL_HANDLE, &ii);
            if (vr == VK_SUCCESS || vr == VK_SUBOPTIMAL_KHR) {
                have_image = 1;
                s_acquire_cursor = (s_acquire_cursor + 1) % (s_sc_count + 1);
                if (vr == VK_SUBOPTIMAL_KHR) {
                    NVP_ADD(s_d_suboptimal, 1);
                    /* A swapchain that is suboptimal from its first image would
                     * otherwise be rebuilt every frame. */
                    if (s_images_since_create >= 60) s_need_recreate = 1;
                }
            } else {
                if (vr == VK_ERROR_OUT_OF_DATE_KHR) {
                    NVP_ADD(s_d_out_of_date, 1);
                    s_need_recreate = 1;
                } else if (vr == VK_TIMEOUT || vr == VK_NOT_READY) {
                    NVP_ADD(s_d_acquire_timeouts, 1);
                } else {
                    nvp_fail(vr);
                    s_need_recreate = 1;
                }
            }
        }

        int last = (r == repeats - 1) || !have_image;
        VkBuffer cap_buf = VK_NULL_HANDLE;
        VkDeviceMemory cap_mem = VK_NULL_HANDLE;
        if (r == 0 && have_image && atomic_load(&s_sc_can_read)) {
            int want = 1;
            if (atomic_compare_exchange_strong(&s_cap_state, &want, 3)) {
                if (!nvp_cap_create(&cap_buf, &cap_mem)) atomic_store(&s_cap_state, 0);
            }
        }
        vr = nvp_submit_repeat(f, im, have_image, ii, acq, first, last, cap_buf);
        if (cap_buf != VK_NULL_HANDLE) nvp_cap_finish(f, cap_buf, cap_mem, vr);
        if (vr != VK_SUCCESS) { nvp_fail(vr); hard_fail = 1; break; }
        if (last) balanced = 1;
        if (!have_image) break;

        VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &s_render_done[ii];
        pi.swapchainCount = 1;
        pi.pSwapchains = &s_swapchain;
        pi.pImageIndices = &ii;
        VkPresentIdKHR pid = { VK_STRUCTURE_TYPE_PRESENT_ID_KHR };
        uint64_t pres_id = 0;
        if (s_use_present_wait) {
            pres_id = ++s_present_id;
            pid.swapchainCount = 1;
            pid.pPresentIds = &pres_id;
            pi.pNext = &pid;
        }
        vr = nvp_vkQueuePresentKHR(s_queue, &pi);
        if (vr == VK_SUCCESS || vr == VK_SUBOPTIMAL_KHR) {
            if (pres_id) {
                if (!s_sc_first_id) s_sc_first_id = pres_id;
                s_last_presented_id = pres_id;
                NVP_SET(s_d_last_present_id, pres_id);
            }
            presented++;
            s_images_since_create++;
            NVP_ADD(s_d_images_presented, 1);
            if (vr == VK_SUBOPTIMAL_KHR) {
                NVP_ADD(s_d_suboptimal, 1);
                if (s_images_since_create >= 60) s_need_recreate = 1;
            }
        } else if (vr == VK_ERROR_OUT_OF_DATE_KHR) {
            NVP_ADD(s_d_out_of_date, 1);
            s_need_recreate = 1;
            if (!last) {
                /* Close the interop hand-over with a submit of its own. */
                NvpFrame *g = &s_frames[s_frame_cursor];
                s_frame_cursor = (s_frame_cursor + 1) % NVP_RING;
                if (nvp_wait_reset_frame(g) == VK_SUCCESS &&
                    nvp_submit_repeat(g, im, 0, 0, VK_NULL_HANDLE, 0, 1, VK_NULL_HANDLE) == VK_SUCCESS)
                    balanced = 1;
                else
                    hard_fail = 1;
            }
            break;
        } else {
            nvp_fail(vr);
            s_need_recreate = 1;
            hard_fail = 1;
        }
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

void nvp_release_gl_context(void)
{
    if (!s_dev || !s_interop_live) return;
    glFinish();
    nvp_destroy_interop(1);
}

void nvp_shutdown(void)
{
    int was_active = s_active;
    nvp_teardown();
    if (was_active) NVP_SAY("shut down\n");
}

/* ---- diagnostics -------------------------------------------------------- */
void nvp_diagnostics(NvpDiagnostics *out)
{
    memset(out, 0, sizeof(*out));
    out->active = s_active;
    out->present_mode = NVP_GET(s_d_present_mode);
    out->requested_interval = atomic_load(&s_req_interval);
    out->requested_vsync = atomic_load(&s_req_vsync);
    out->swapchain_images = NVP_GET(s_d_sc_images);
    out->extent_width = NVP_GET(s_d_ext_w);
    out->extent_height = NVP_GET(s_d_ext_h);
    out->interop_width = NVP_GET(s_d_int_w);
    out->interop_height = NVP_GET(s_d_int_h);
    out->frames_begun = NVP_GET(s_d_frames_begun);
    out->frames_presented = NVP_GET(s_d_frames_presented);
    out->images_presented = NVP_GET(s_d_images_presented);
    out->acquire_timeouts = NVP_GET(s_d_acquire_timeouts);
    out->out_of_date = NVP_GET(s_d_out_of_date);
    out->suboptimal = NVP_GET(s_d_suboptimal);
    out->swapchain_recreations = NVP_GET(s_d_sc_recreations);
    out->interop_recreations = NVP_GET(s_d_interop_recreations);
    out->minimized_skips = NVP_GET(s_d_minimized_skips);
    out->failures = NVP_GET(s_d_failures);
    out->last_present_ns = NVP_GET(s_d_last_present_ns);
    out->max_present_block_ns = NVP_GET(s_d_max_block_ns);
    out->last_vk_result = NVP_GET(s_d_last_result);
    out->present_wait = NVP_GET(s_d_present_wait_on);
    out->present_depth = NVP_GET(s_d_present_depth);
    out->present_waits = NVP_GET(s_d_present_waits);
    out->present_wait_timeouts = NVP_GET(s_d_present_wait_timeouts);
    out->present_wait_ns_total = NVP_GET(s_d_present_wait_ns_total);
    out->present_wait_max_ns = NVP_GET(s_d_present_wait_max_ns);
    out->last_present_id = NVP_GET(s_d_last_present_id);
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
        "\"present_wait_max_ns\":%llu,\"last_present_id\":%llu}",
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
        (unsigned long long)d.last_present_id);
}

#endif  /* PSX_HAVE_VULKAN */
