/* The one Vulkan instance and device of the process (see vk_core.h for the
 * contract).
 *
 * Loading goes through SDL_Vulkan_LoadLibrary and volk, so the executable has
 * no link-time Vulkan dependency. volk's global vk* function pointers are the
 * entry points every consumer of the shared device calls; they never clash
 * with gpu_vk_renderer.c, which only declares its own p_vk* pointers. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(PSX_HAVE_VULKAN)

/* Built without Vulkan. The header needs volk, which a build without Vulkan
 * does not have, so only the entry points whose signatures need no Vulkan
 * types exist here; nothing outside the Vulkan build can reach the others. */
struct SDL_Window;

int vk_core_attach_window(struct SDL_Window *window) { (void)window; return 0; }
void vk_core_detach_window(void) {}
void vk_core_shutdown(void) {}
int vk_core_ready(void) { return 0; }
const char *vk_core_failure(void) { return "built without Vulkan"; }
uint64_t vk_core_validation_message_count(void) { return 0; }
int vk_core_info_json(char *out, int capacity)
{
    return snprintf(out, (size_t)capacity, "{\"available\":false}");
}
int vk_core_validation_json(char *out, int capacity, int count)
{
    (void)count;
    return snprintf(out, (size_t)capacity, "{\"available\":false}");
}

#else  /* PSX_HAVE_VULKAN */

#include <stdarg.h>

#include "psx_sdl.h"
#if defined(PSX_SDL3)
#include <SDL3/SDL_vulkan.h>
#else
#include <SDL_vulkan.h>
#endif
#if defined(_WIN32)
#include <windows.h>
#endif

#include "vk_core.h"
#include "vk_swapchain.h"
#include "crash_trace.h"

#define VKC_MAX_EXT 64
#define VKC_RING 256
#define VKC_MSG_BYTES 512
#define VKC_MAX_DEVICES 16
#define VKC_MAX_FAMILIES 32
#define VKC_CACHE_MAX_BYTES (256u * 1024u * 1024u)

/* PSX_VK_DISABLE tokens, in bit order. */
enum {
    VKC_OFF_INTERLOCK, VKC_OFF_STORES_ATOMICS, VKC_OFF_FLOAT64, VKC_OFF_INT64,
    VKC_OFF_ANISOTROPY, VKC_OFF_DUAL_SRC_BLEND, VKC_OFF_INDEPENDENT_BLEND,
    VKC_OFF_TIMELINE, VKC_OFF_PRESENT_ID, VKC_OFF_PRESENT_WAIT,
    VKC_OFF_PIPELINE_STATISTICS, VKC_OFF_TIMESTAMPS, VKC_OFF_DEBUG_UTILS,
    VKC_OFF_COUNT
};
static const char *const k_off_names[VKC_OFF_COUNT] = {
    "interlock", "stores_atomics", "float64", "int64", "anisotropy", "dual_src_blend",
    "independent_blend", "timeline", "present_id", "present_wait", "pipeline_statistics",
    "timestamps", "debug_utils"
};

typedef struct VkcMessage {
    uint64_t seq;
    uint32_t severity, type;
    int32_t id;
    char text[VKC_MSG_BYTES];
} VkcMessage;

typedef char VkcExtName[VK_MAX_EXTENSION_NAME_SIZE];

/* ---- state -------------------------------------------------------------- */
static int s_ready;
static int s_sdl_loaded;
static VkInstance s_instance;
static VkPhysicalDevice s_phys;
static VkDevice s_dev;
static VkQueue s_queue;
static uint32_t s_qfam;
static VkSurfaceKHR s_surface;
static struct SDL_Window *s_window;
static VmaAllocator s_alloc;
static VkPipelineCache s_cache;
static char s_cache_path[1024];
static uint32_t s_cache_loaded_bytes, s_cache_saved_bytes;
static int s_cache_save_failed;
static VkDebugUtilsMessengerEXT s_messenger;
static VkCommandPool s_oneshot_pool;
static VkCoreCaps s_caps;

static VkcExtName s_inst_exts[VKC_MAX_EXT];
static uint32_t s_inst_ext_count;
static VkcExtName s_dev_exts[VKC_MAX_EXT];
static uint32_t s_dev_ext_count;
static char s_driver_name[VK_MAX_DRIVER_NAME_SIZE];
static char s_driver_info[VK_MAX_DRIVER_INFO_SIZE];

static SDL_mutex *s_queue_mutex;     /* the single VkQueue */
static SDL_mutex *s_pool_mutex;      /* the one-shot command pool */
static SDL_mutex *s_diag_mutex;      /* message ring and failure records */

/* Created once and kept for the process lifetime: the validation callback can
 * run while the instance is being destroyed. */
static VkcMessage s_ring[VKC_RING];
static uint64_t s_msg_seq;           /* messages ever recorded */
static uint64_t s_total_warnings, s_total_errors, s_total_other;

static char s_failure[256];          /* first failing init step */
static char s_check_site[96];        /* first vk_core_check failure */
static VkResult s_check_result;
static int s_check_failed;
static char s_fatal_reason[256];     /* psx_fatal_halt keeps the pointer */

static void vkc_fail(const char *fmt, ...)
{
    if (s_failure[0]) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_failure, sizeof(s_failure), fmt, ap);
    va_end(ap);
}

static void vkc_ensure_mutexes(void)
{
    if (!s_queue_mutex) s_queue_mutex = SDL_CreateMutex();
    if (!s_pool_mutex) s_pool_mutex = SDL_CreateMutex();
    if (!s_diag_mutex) s_diag_mutex = SDL_CreateMutex();
}

static uint32_t vkc_min_version(uint32_t a, uint32_t b) { return a < b ? a : b; }

static int vkc_ext_listed(const VkExtensionProperties *props, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; ++i)
        if (!strcmp(props[i].extensionName, name)) return 1;
    return 0;
}

static int vkc_name_listed(const VkcExtName *names, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; ++i)
        if (!strcmp(names[i], name)) return 1;
    return 0;
}

static void vkc_add_name(VkcExtName *names, uint32_t *n, const char *name)
{
    if (*n >= VKC_MAX_EXT || vkc_name_listed(names, *n, name)) return;
    snprintf(names[*n], sizeof(names[*n]), "%s", name);
    ++*n;
}

static int vkc_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int vkc_contains_nocase(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    if (!nl) return 0;
    for (; *hay; ++hay) {
        size_t i = 0;
        while (i < nl && hay[i] && vkc_lower((unsigned char)hay[i]) == vkc_lower((unsigned char)needle[i])) ++i;
        if (i == nl) return 1;
    }
    return 0;
}

/* ---- validation / debug-utils messages ---------------------------------- */
static VKAPI_ATTR VkBool32 VKAPI_CALL vkc_debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT *data, void *user)
{
    (void)user;
    if (!s_diag_mutex) return VK_FALSE;
    SDL_LockMutex(s_diag_mutex);
    VkcMessage *m = &s_ring[s_msg_seq % VKC_RING];
    m->seq = ++s_msg_seq;
    m->severity = (uint32_t)severity;
    m->type = (uint32_t)type;
    m->id = data ? data->messageIdNumber : 0;
    snprintf(m->text, sizeof(m->text), "%s", data && data->pMessage ? data->pMessage : "");
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++s_total_errors;
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ++s_total_warnings;
    else ++s_total_other;
    SDL_UnlockMutex(s_diag_mutex);
    return VK_FALSE;
}

static void vkc_messenger_info(VkDebugUtilsMessengerCreateInfoEXT *mi)
{
    memset(mi, 0, sizeof(*mi));
    mi->sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    mi->messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    mi->messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    mi->pfnUserCallback = vkc_debug_callback;
}

/* ---- failure recording -------------------------------------------------- */
int vk_core_check(VkResult result, const char *site)
{
    switch (result) {
    case VK_SUCCESS:
    case VK_SUBOPTIMAL_KHR:
    case VK_TIMEOUT:
    case VK_NOT_READY:
    case VK_INCOMPLETE:
        return 1;
    default:
        break;
    }
    if (s_diag_mutex) SDL_LockMutex(s_diag_mutex);
    if (!s_check_failed) {
        s_check_failed = 1;
        s_check_result = result;
        snprintf(s_check_site, sizeof(s_check_site), "%s", site ? site : "?");
    }
    if (result == VK_ERROR_DEVICE_LOST) {
        snprintf(s_fatal_reason, sizeof(s_fatal_reason), "Vulkan device lost at %s (device %s)",
                 site ? site : "?", s_caps.device_name);
        if (s_diag_mutex) SDL_UnlockMutex(s_diag_mutex);
        psx_fatal_halt(s_fatal_reason);
        return 0;
    }
    if (s_diag_mutex) SDL_UnlockMutex(s_diag_mutex);
    return 0;
}

/* ---- PSX_VK_DISABLE ------------------------------------------------------ */
/* Returns 0 and names the bad token in the failure when one is unknown. */
static int vkc_parse_disable(uint32_t *mask_out)
{
    uint32_t mask = 0;
    const char *e = getenv("PSX_VK_DISABLE");
    while (e && *e) {
        const char *end = strchr(e, ',');
        size_t n = end ? (size_t)(end - e) : strlen(e);
        int known = 0;
        if (n == 3 && !strncmp(e, "all", 3)) {
            mask = (1u << VKC_OFF_COUNT) - 1u;
            known = 1;
        }
        for (uint32_t i = 0; i < VKC_OFF_COUNT && !known; ++i) {
            if (strlen(k_off_names[i]) == n && !strncmp(e, k_off_names[i], n)) {
                mask |= 1u << i;
                known = 1;
            }
        }
        if (!known) {
            vkc_fail("PSX_VK_DISABLE: unknown capability '%.*s'", (int)(n > 32 ? 32 : n), e);
            return 0;
        }
        e += n;
        if (*e == ',') ++e;
    }
    if (mask & (1u << VKC_OFF_PRESENT_ID)) mask |= 1u << VKC_OFF_PRESENT_WAIT;
    *mask_out = mask;
    return 1;
}

/* ---- instance ------------------------------------------------------------ */
static int vkc_load_sdl_vulkan(void)
{
#if defined(PSX_SDL3)
    if (!SDL_Vulkan_LoadLibrary(NULL))
#else
    if (SDL_Vulkan_LoadLibrary(NULL) != 0)
#endif
    {
        vkc_fail("SDL_Vulkan_LoadLibrary failed (no Vulkan driver?)");
        return 0;
    }
    s_sdl_loaded = 1;
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (!gipa) {
        vkc_fail("SDL has no vkGetInstanceProcAddr");
        return 0;
    }
    volkInitializeCustom(gipa);
    return 1;
}

static int vkc_create_instance(uint32_t *app_version_out)
{
    uint32_t ver = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&ver);
    if (ver < VK_API_VERSION_1_1) {
        vkc_fail("Vulkan instance older than 1.1");
        return 0;
    }
    uint32_t app_version = vkc_min_version(ver, VK_API_VERSION_1_3);
    *app_version_out = app_version;

    const char *exts[VKC_MAX_EXT];
    uint32_t ext_count = 0;
    unsigned sdl_count = 0;
#if defined(PSX_SDL3)
    const char *const *sdl_exts = SDL_Vulkan_GetInstanceExtensions(&sdl_count);
    if (!sdl_exts) {
        vkc_fail("SDL_Vulkan_GetInstanceExtensions failed");
        return 0;
    }
    for (unsigned i = 0; i < sdl_count && ext_count < VKC_MAX_EXT - 1; ++i) exts[ext_count++] = sdl_exts[i];
#else
    const char *sdl_names[VKC_MAX_EXT - 1];
    if (!SDL_Vulkan_GetInstanceExtensions(NULL, &sdl_count, NULL)) {
        vkc_fail("SDL_Vulkan_GetInstanceExtensions(count) failed");
        return 0;
    }
    if (sdl_count > VKC_MAX_EXT - 1) sdl_count = VKC_MAX_EXT - 1;
    if (!SDL_Vulkan_GetInstanceExtensions(NULL, &sdl_count, sdl_names)) {
        vkc_fail("SDL_Vulkan_GetInstanceExtensions(list) failed");
        return 0;
    }
    for (unsigned i = 0; i < sdl_count; ++i) exts[ext_count++] = sdl_names[i];
#endif

    uint32_t avail_n = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &avail_n, NULL);
    VkExtensionProperties *avail = (VkExtensionProperties *)calloc(avail_n ? avail_n : 1, sizeof(*avail));
    if (!avail) {
        vkc_fail("out of memory");
        return 0;
    }
    vkEnumerateInstanceExtensionProperties(NULL, &avail_n, avail);
    int have_debug_utils = vkc_ext_listed(avail, avail_n, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    free(avail);
    if (have_debug_utils) exts[ext_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;

    int want_validation = 0;
    const char *venv = getenv("PSX_VK_VALIDATION");
#if defined(NDEBUG)
    want_validation = venv && !strcmp(venv, "1");
#else
    want_validation = (venv && !strcmp(venv, "1")) || !(venv && !strcmp(venv, "0"));
#endif
    const char *layers[1];
    uint32_t layer_count = 0;
    if (want_validation && have_debug_utils) {
        uint32_t ln = 0;
        vkEnumerateInstanceLayerProperties(&ln, NULL);
        VkLayerProperties *lp = (VkLayerProperties *)calloc(ln ? ln : 1, sizeof(*lp));
        if (lp) {
            vkEnumerateInstanceLayerProperties(&ln, lp);
            for (uint32_t i = 0; i < ln; ++i) {
                if (!strcmp(lp[i].layerName, "VK_LAYER_KHRONOS_validation")) {
                    layers[layer_count++] = "VK_LAYER_KHRONOS_validation";
                    break;
                }
            }
            free(lp);
        }
    }

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "psxrecomp";
    app.apiVersion = app_version;
    VkDebugUtilsMessengerCreateInfoEXT mi;
    vkc_messenger_info(&mi);
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pNext = have_debug_utils ? &mi : NULL;    /* also reports instance create/destroy */
    ici.pApplicationInfo = &app;
    ici.enabledLayerCount = layer_count;
    ici.ppEnabledLayerNames = layers;
    ici.enabledExtensionCount = ext_count;
    ici.ppEnabledExtensionNames = exts;
    VkResult r = vkCreateInstance(&ici, NULL, &s_instance);
    if (r != VK_SUCCESS) {
        s_instance = VK_NULL_HANDLE;
        vkc_fail("vkCreateInstance failed (%d)", (int)r);
        return 0;
    }
    volkLoadInstanceOnly(s_instance);
    for (uint32_t i = 0; i < ext_count; ++i) vkc_add_name(s_inst_exts, &s_inst_ext_count, exts[i]);
    s_caps.validation = layer_count != 0;
    s_caps.debug_utils = (uint8_t)have_debug_utils;
    if (have_debug_utils) {
        r = vkCreateDebugUtilsMessengerEXT(s_instance, &mi, NULL, &s_messenger);
        if (r != VK_SUCCESS) {
            s_messenger = VK_NULL_HANDLE;
            vkc_fail("vkCreateDebugUtilsMessengerEXT failed (%d)", (int)r);
            return 0;
        }
    }
    return 1;
}

/* ---- device selection ---------------------------------------------------- */
typedef struct VkcPick {
    VkPhysicalDevice pd;
    VkPhysicalDeviceProperties props;
    VkExtensionProperties *exts;     /* malloc'd */
    uint32_t ext_count;
    uint32_t qfam;
    int score;
} VkcPick;

static int vkc_uuid_requested_ok(const VkCoreDeviceRequest *req, const uint8_t *dev_uuid,
                                 const uint8_t *drv_uuid)
{
    if (!req || req->device_uuid_count == 0) return 1;
    if (!req->driver_uuid || memcmp(drv_uuid, req->driver_uuid, VK_UUID_SIZE) != 0) return 0;
    for (uint32_t i = 0; i < req->device_uuid_count; ++i)
        if (!memcmp(dev_uuid, req->device_uuids[i], VK_UUID_SIZE)) return 1;
    return 0;
}

/* Fills pick when pd qualifies (pick->exts is then owned by the caller).
 * explicit_choice: PSX_VK_DEVICE named it, so software devices are allowed. */
static int vkc_evaluate(VkPhysicalDevice pd, const VkCoreDeviceRequest *req, int explicit_choice,
                        VkcPick *pick)
{
    memset(pick, 0, sizeof(*pick));
    pick->pd = pd;
    VkPhysicalDeviceIDProperties idp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
    VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    p2.pNext = &idp;
    vkGetPhysicalDeviceProperties2(pd, &p2);
    pick->props = p2.properties;
    if (VK_API_VERSION_MAJOR(p2.properties.apiVersion) == 1 &&
        VK_API_VERSION_MINOR(p2.properties.apiVersion) < 1) return 0;
    if (p2.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU && !explicit_choice) return 0;
    if (!vkc_uuid_requested_ok(req, idp.deviceUUID, idp.driverUUID)) return 0;

    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &en, NULL);
    VkExtensionProperties *ext = (VkExtensionProperties *)calloc(en ? en : 1, sizeof(*ext));
    if (!ext) return 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &en, ext);
    int ok = vkc_ext_listed(ext, en, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    for (uint32_t i = 0; req && i < req->extension_count && ok; ++i)
        ok = vkc_ext_listed(ext, en, req->extensions[i]);
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, NULL);
    if (qn > VKC_MAX_FAMILIES) qn = VKC_MAX_FAMILIES;
    VkQueueFamilyProperties qf[VKC_MAX_FAMILIES];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf);
    int found = 0;
    for (uint32_t f = 0; f < qn && ok && !found; ++f) {
        if ((qf[f].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
            (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            pick->qfam = f;
            found = 1;
        }
    }
    if (!ok || !found) {
        free(ext);
        return 0;
    }
    switch (p2.properties.deviceType) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: pick->score = 3; break;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: pick->score = 2; break;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: pick->score = 1; break;
    default: pick->score = 0; break;
    }
    pick->exts = ext;
    pick->ext_count = en;
    return 1;
}

static int vkc_select_device(const VkCoreDeviceRequest *req, VkcPick *best)
{
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(s_instance, &n, NULL);
    if (n > VKC_MAX_DEVICES) n = VKC_MAX_DEVICES;
    VkPhysicalDevice devs[VKC_MAX_DEVICES];
    vkEnumeratePhysicalDevices(s_instance, &n, devs);
    if (n == 0) {
        vkc_fail("no Vulkan physical device");
        return 0;
    }
    const char *want = getenv("PSX_VK_DEVICE");
    if (want && !want[0]) want = NULL;
    int numeric = 0;
    uint32_t want_index = 0;
    if (want) {
        numeric = 1;
        for (const char *c = want; *c; ++c)
            if (*c < '0' || *c > '9') numeric = 0;
        if (numeric) want_index = (uint32_t)strtoul(want, NULL, 10);
    }

    memset(best, 0, sizeof(*best));
    for (uint32_t i = 0; i < n; ++i) {
        if (want) {
            if (numeric) {
                if (i != want_index) continue;
            } else {
                VkPhysicalDeviceProperties pr;
                vkGetPhysicalDeviceProperties(devs[i], &pr);
                if (!vkc_contains_nocase(pr.deviceName, want)) continue;
            }
        }
        VkcPick cand;
        if (!vkc_evaluate(devs[i], req, want != NULL, &cand)) continue;
        if (!best->pd || cand.score > best->score) {
            free(best->exts);
            *best = cand;
        } else {
            free(cand.exts);
        }
        if (want) break;
    }
    if (!best->pd) {
        if (want) vkc_fail("PSX_VK_DEVICE=%.64s names no qualifying device (%u enumerated)", want, n);
        else vkc_fail("no Vulkan device qualifies (%u enumerated)", n);
        return 0;
    }
    return 1;
}

/* ---- device -------------------------------------------------------------- */
static int vkc_create_device(const VkCoreDeviceRequest *req, uint32_t app_version,
                             uint32_t disabled, const VkcPick *pick)
{
    s_phys = pick->pd;
    s_qfam = pick->qfam;
    const VkPhysicalDeviceProperties *pr = &pick->props;
    uint32_t eff = vkc_min_version(pr->apiVersion, app_version);
    int api12 = VK_API_VERSION_MINOR(eff) >= 2;

    int have_il = vkc_ext_listed(pick->exts, pick->ext_count, VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME);
    int have_tl_ext = !api12 && vkc_ext_listed(pick->exts, pick->ext_count, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    int have_pid = vkc_ext_listed(pick->exts, pick->ext_count, VK_KHR_PRESENT_ID_EXTENSION_NAME);
    int have_pw = vkc_ext_listed(pick->exts, pick->ext_count, VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
    int have_driver_props = api12 ||
        vkc_ext_listed(pick->exts, pick->ext_count, VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME);

    /* Query what the device supports. */
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT q_il = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT };
    VkPhysicalDeviceVulkan12Features q_v12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceTimelineSemaphoreFeatures q_tl = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES };
    VkPhysicalDevicePresentIdFeaturesKHR q_pid = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR };
    VkPhysicalDevicePresentWaitFeaturesKHR q_pw = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR };
    void *chain = NULL;
#define VKC_LINK(s) do { (s).pNext = chain; chain = &(s); } while (0)
    if (have_il) VKC_LINK(q_il);
    if (api12) VKC_LINK(q_v12);
    else if (have_tl_ext) VKC_LINK(q_tl);
    if (have_pid) VKC_LINK(q_pid);
    if (have_pid && have_pw) VKC_LINK(q_pw);
    f2.pNext = chain;
    vkGetPhysicalDeviceFeatures2(pick->pd, &f2);
    const VkPhysicalDeviceFeatures *sup = &f2.features;

#define VKC_ON(bit) (!(disabled & (1u << (bit))))
    VkCoreCaps *c = &s_caps;
    c->api_version = eff;
    c->vendor_id = pr->vendorID;
    c->device_id = pr->deviceID;
    c->driver_version = pr->driverVersion;
    c->device_type = pr->deviceType;
    snprintf(c->device_name, sizeof(c->device_name), "%s", pr->deviceName);
    c->fragment_shader_interlock = (uint8_t)(have_il && q_il.fragmentShaderPixelInterlock && VKC_ON(VKC_OFF_INTERLOCK));
    c->fragment_stores_and_atomics = (uint8_t)(sup->fragmentStoresAndAtomics && VKC_ON(VKC_OFF_STORES_ATOMICS));
    c->shader_float64 = (uint8_t)(sup->shaderFloat64 && VKC_ON(VKC_OFF_FLOAT64));
    c->shader_int64 = (uint8_t)(sup->shaderInt64 && VKC_ON(VKC_OFF_INT64));
    c->sampler_anisotropy = (uint8_t)(sup->samplerAnisotropy && VKC_ON(VKC_OFF_ANISOTROPY));
    c->dual_src_blend = (uint8_t)(sup->dualSrcBlend && VKC_ON(VKC_OFF_DUAL_SRC_BLEND));
    c->independent_blend = (uint8_t)(sup->independentBlend && VKC_ON(VKC_OFF_INDEPENDENT_BLEND));
    int tl_supported = api12 ? q_v12.timelineSemaphore : (have_tl_ext && q_tl.timelineSemaphore);
    c->timeline_semaphore = (uint8_t)(tl_supported && VKC_ON(VKC_OFF_TIMELINE));
    c->present_id = (uint8_t)(have_pid && q_pid.presentId && VKC_ON(VKC_OFF_PRESENT_ID));
    c->present_wait = (uint8_t)(c->present_id && have_pw && q_pw.presentWait && VKC_ON(VKC_OFF_PRESENT_WAIT));
    c->pipeline_statistics = (uint8_t)(sup->pipelineStatisticsQuery && VKC_ON(VKC_OFF_PIPELINE_STATISTICS));
    c->debug_utils = (uint8_t)(c->debug_utils && VKC_ON(VKC_OFF_DEBUG_UTILS));

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pick->pd, &qn, NULL);
    if (qn > VKC_MAX_FAMILIES) qn = VKC_MAX_FAMILIES;
    VkQueueFamilyProperties qf[VKC_MAX_FAMILIES];
    vkGetPhysicalDeviceQueueFamilyProperties(pick->pd, &qn, qf);
    c->timestamps = (uint8_t)(qf[s_qfam].timestampValidBits != 0 && VKC_ON(VKC_OFF_TIMESTAMPS));
    c->timestamp_period_ns = pr->limits.timestampPeriod;
    c->max_sampler_anisotropy = pr->limits.maxSamplerAnisotropy;
    c->max_image_dimension_2d = pr->limits.maxImageDimension2D;
    c->max_push_constants_size = pr->limits.maxPushConstantsSize;
    c->max_texel_buffer_elements = pr->limits.maxTexelBufferElements;
    c->max_storage_buffer_range = pr->limits.maxStorageBufferRange;
    c->max_uniform_buffer_range = pr->limits.maxUniformBufferRange;
    c->max_color_attachments = pr->limits.maxColorAttachments;
    c->min_uniform_buffer_offset_alignment = pr->limits.minUniformBufferOffsetAlignment;
    c->min_texel_buffer_offset_alignment = pr->limits.minTexelBufferOffsetAlignment;
    c->non_coherent_atom_size = pr->limits.nonCoherentAtomSize;

    /* Identity and driver details. */
    VkPhysicalDeviceIDProperties idp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
    VkPhysicalDeviceDriverProperties drv = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
    VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    p2.pNext = &idp;
    if (have_driver_props) idp.pNext = &drv;
    vkGetPhysicalDeviceProperties2(pick->pd, &p2);
    memcpy(c->device_uuid, idp.deviceUUID, VK_UUID_SIZE);
    memcpy(c->driver_uuid, idp.driverUUID, VK_UUID_SIZE);
    if (have_driver_props) {
        snprintf(s_driver_name, sizeof(s_driver_name), "%s", drv.driverName);
        snprintf(s_driver_info, sizeof(s_driver_info), "%s", drv.driverInfo);
    }

    /* Enable exactly the supported, unmasked set. */
    VkPhysicalDeviceFeatures2 e2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT e_il = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT };
    VkPhysicalDeviceVulkan12Features e_v12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceTimelineSemaphoreFeatures e_tl = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES };
    VkPhysicalDevicePresentIdFeaturesKHR e_pid = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR };
    VkPhysicalDevicePresentWaitFeaturesKHR e_pw = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR };
    e2.features.fragmentStoresAndAtomics = c->fragment_stores_and_atomics;
    e2.features.shaderFloat64 = c->shader_float64;
    e2.features.shaderInt64 = c->shader_int64;
    e2.features.samplerAnisotropy = c->sampler_anisotropy;
    e2.features.dualSrcBlend = c->dual_src_blend;
    e2.features.independentBlend = c->independent_blend;
    e2.features.pipelineStatisticsQuery = c->pipeline_statistics;
    chain = NULL;
    VkcExtName dev_exts[VKC_MAX_EXT];
    uint32_t dev_ext_count = 0;
    vkc_add_name(dev_exts, &dev_ext_count, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    for (uint32_t i = 0; req && i < req->extension_count; ++i)
        vkc_add_name(dev_exts, &dev_ext_count, req->extensions[i]);
    if (c->fragment_shader_interlock) {
        e_il.fragmentShaderPixelInterlock = VK_TRUE;
        VKC_LINK(e_il);
        vkc_add_name(dev_exts, &dev_ext_count, VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME);
    }
    if (c->timeline_semaphore) {
        if (api12) {
            e_v12.timelineSemaphore = VK_TRUE;
            VKC_LINK(e_v12);
        } else {
            e_tl.timelineSemaphore = VK_TRUE;
            VKC_LINK(e_tl);
            vkc_add_name(dev_exts, &dev_ext_count, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
        }
    }
    if (c->present_id) {
        e_pid.presentId = VK_TRUE;
        VKC_LINK(e_pid);
        vkc_add_name(dev_exts, &dev_ext_count, VK_KHR_PRESENT_ID_EXTENSION_NAME);
    }
    if (c->present_wait) {
        e_pw.presentWait = VK_TRUE;
        VKC_LINK(e_pw);
        vkc_add_name(dev_exts, &dev_ext_count, VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
    }
#undef VKC_LINK
    e2.pNext = chain;

    const char *ext_ptrs[VKC_MAX_EXT];
    for (uint32_t i = 0; i < dev_ext_count; ++i) ext_ptrs[i] = dev_exts[i];
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = s_qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext = &e2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = dev_ext_count;
    dci.ppEnabledExtensionNames = ext_ptrs;
    VkResult r = vkCreateDevice(s_phys, &dci, NULL, &s_dev);
    if (r != VK_SUCCESS) {
        s_dev = VK_NULL_HANDLE;
        vkc_fail("vkCreateDevice failed (%d)", (int)r);
        return 0;
    }
    for (uint32_t i = 0; i < dev_ext_count; ++i) vkc_add_name(s_dev_exts, &s_dev_ext_count, dev_exts[i]);
    volkLoadDevice(s_dev);
    vkGetDeviceQueue(s_dev, s_qfam, 0, &s_queue);
    return 1;
#undef VKC_ON
}

static int vkc_create_allocator(void)
{
    VmaVulkanFunctions funcs;
    memset(&funcs, 0, sizeof(funcs));
    funcs.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    funcs.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.physicalDevice = s_phys;
    ci.device = s_dev;
    ci.instance = s_instance;
    ci.pVulkanFunctions = &funcs;
    ci.vulkanApiVersion = VK_MAKE_API_VERSION(0, VK_API_VERSION_MAJOR(s_caps.api_version),
                                              VK_API_VERSION_MINOR(s_caps.api_version), 0);
    VkResult r = vmaCreateAllocator(&ci, &s_alloc);
    if (r != VK_SUCCESS) {
        s_alloc = NULL;
        vkc_fail("vmaCreateAllocator failed (%d)", (int)r);
        return 0;
    }
    return 1;
}

/* ---- pipeline cache ------------------------------------------------------ */
/* Reads the cache file when its header belongs to this device and driver.
 * Returns a malloc'd blob or NULL (start empty). */
static void *vkc_read_cache(const char *path, size_t *size_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    void *data = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        long len = ftell(f);
        if (len >= (long)sizeof(VkPipelineCacheHeaderVersionOne) && (unsigned long)len <= VKC_CACHE_MAX_BYTES &&
            fseek(f, 0, SEEK_SET) == 0) {
            data = malloc((size_t)len);
            if (data && fread(data, 1, (size_t)len, f) == (size_t)len) {
                VkPipelineCacheHeaderVersionOne h;
                memcpy(&h, data, sizeof(h));
                VkPhysicalDeviceProperties pr;
                vkGetPhysicalDeviceProperties(s_phys, &pr);
                if (h.headerSize >= sizeof(h) && h.headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
                    h.vendorID == pr.vendorID && h.deviceID == pr.deviceID &&
                    !memcmp(h.pipelineCacheUUID, pr.pipelineCacheUUID, VK_UUID_SIZE)) {
                    *size_out = (size_t)len;
                    fclose(f);
                    return data;
                }
            }
            free(data);
            data = NULL;
        }
    }
    fclose(f);
    return data;
}

static int vkc_create_pipeline_cache(const VkCoreDeviceRequest *req)
{
    size_t blob_size = 0;
    void *blob = NULL;
    if (req && req->pipeline_cache_path && req->pipeline_cache_path[0]) {
        snprintf(s_cache_path, sizeof(s_cache_path), "%s", req->pipeline_cache_path);
        blob = vkc_read_cache(s_cache_path, &blob_size);
    }
    VkPipelineCacheCreateInfo ci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    ci.initialDataSize = blob ? blob_size : 0;
    ci.pInitialData = blob;
    VkResult r = vkCreatePipelineCache(s_dev, &ci, NULL, &s_cache);
    if (r != VK_SUCCESS && blob) {
        /* The driver rejected the blob; an empty cache is always valid. */
        free(blob);
        blob = NULL;
        ci.initialDataSize = 0;
        ci.pInitialData = NULL;
        r = vkCreatePipelineCache(s_dev, &ci, NULL, &s_cache);
    }
    s_cache_loaded_bytes = blob ? (uint32_t)blob_size : 0;
    free(blob);
    if (r != VK_SUCCESS) {
        s_cache = VK_NULL_HANDLE;
        vkc_fail("vkCreatePipelineCache failed (%d)", (int)r);
        return 0;
    }
    return 1;
}

static int vkc_replace_file(const char *tmp, const char *path)
{
#if defined(_WIN32)
    return MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(tmp, path) == 0;
#endif
}

static void vkc_save_pipeline_cache(void)
{
    if (!s_cache || !s_cache_path[0]) return;
    size_t size = 0;
    if (vkGetPipelineCacheData(s_dev, s_cache, &size, NULL) != VK_SUCCESS || size == 0) {
        s_cache_save_failed = 1;
        return;
    }
    void *data = malloc(size);
    if (!data) {
        s_cache_save_failed = 1;
        return;
    }
    VkResult r = vkGetPipelineCacheData(s_dev, s_cache, &size, data);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
        free(data);
        s_cache_save_failed = 1;
        return;
    }
    char tmp[1040];
    snprintf(tmp, sizeof(tmp), "%s.tmp", s_cache_path);
    FILE *f = fopen(tmp, "wb");
    int ok = f != NULL;
    if (f) {
        ok = fwrite(data, 1, size, f) == size;
        ok = fclose(f) == 0 && ok;
    }
    free(data);
    if (ok) ok = vkc_replace_file(tmp, s_cache_path);
    if (ok) s_cache_saved_bytes = (uint32_t)size;
    else s_cache_save_failed = 1;
}

/* ---- init / shutdown ------------------------------------------------------ */
static void vkc_teardown(void)
{
    if (s_dev) vkDeviceWaitIdle(s_dev);
    if (s_cache) {
        vkc_save_pipeline_cache();
        vkDestroyPipelineCache(s_dev, s_cache, NULL);
    }
    s_cache = VK_NULL_HANDLE;
    if (s_oneshot_pool) vkDestroyCommandPool(s_dev, s_oneshot_pool, NULL);
    s_oneshot_pool = VK_NULL_HANDLE;
    if (s_alloc) vmaDestroyAllocator(s_alloc);
    s_alloc = NULL;
    if (s_surface) vkDestroySurfaceKHR(s_instance, s_surface, NULL);
    s_surface = VK_NULL_HANDLE;
    s_window = NULL;
    if (s_dev) vkDestroyDevice(s_dev, NULL);
    s_dev = VK_NULL_HANDLE;
    s_queue = VK_NULL_HANDLE;
    if (s_messenger) vkDestroyDebugUtilsMessengerEXT(s_instance, s_messenger, NULL);
    s_messenger = VK_NULL_HANDLE;
    if (s_instance) vkDestroyInstance(s_instance, NULL);
    s_instance = VK_NULL_HANDLE;
    s_phys = VK_NULL_HANDLE;
    if (s_sdl_loaded) {
        volkFinalize();
        SDL_Vulkan_UnloadLibrary();
    }
    s_sdl_loaded = 0;
    s_ready = 0;
}

static int vkc_init_impl(const VkCoreDeviceRequest *req)
{
    uint32_t disabled = 0;
    if (!vkc_parse_disable(&disabled)) return 0;
    if (!vkc_load_sdl_vulkan()) return 0;
    uint32_t app_version = 0;
    if (!vkc_create_instance(&app_version)) return 0;
    VkcPick pick;
    if (!vkc_select_device(req, &pick)) return 0;
    int ok = vkc_create_device(req, app_version, disabled, &pick);
    free(pick.exts);
    if (!ok) return 0;
    if (!vkc_create_allocator()) return 0;
    if (!vkc_create_pipeline_cache(req)) return 0;
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = s_qfam;
    VkResult r = vkCreateCommandPool(s_dev, &pci, NULL, &s_oneshot_pool);
    if (r != VK_SUCCESS) {
        s_oneshot_pool = VK_NULL_HANDLE;
        vkc_fail("vkCreateCommandPool failed (%d)", (int)r);
        return 0;
    }
    return 1;
}

/* A second init succeeds when the existing device already satisfies req. */
static int vkc_request_satisfied(const VkCoreDeviceRequest *req)
{
    if (!req) return 1;
    if (!vkc_uuid_requested_ok(req, s_caps.device_uuid, s_caps.driver_uuid)) {
        vkc_fail("the existing Vulkan device does not match the requested device UUID");
        return 0;
    }
    for (uint32_t i = 0; i < req->extension_count; ++i) {
        if (!vkc_name_listed(s_dev_exts, s_dev_ext_count, req->extensions[i])) {
            vkc_fail("the existing Vulkan device lacks extension %.64s", req->extensions[i]);
            return 0;
        }
    }
    return 1;
}

int vk_core_init(const VkCoreDeviceRequest *request)
{
    if (s_ready) {
        s_failure[0] = '\0';
        return vkc_request_satisfied(request);
    }
    vkc_ensure_mutexes();
    SDL_LockMutex(s_diag_mutex);
    s_failure[0] = '\0';
    s_check_failed = 0;
    s_check_site[0] = '\0';
    SDL_UnlockMutex(s_diag_mutex);
    memset(&s_caps, 0, sizeof(s_caps));
    s_inst_ext_count = s_dev_ext_count = 0;
    s_driver_name[0] = s_driver_info[0] = '\0';
    s_cache_path[0] = '\0';
    s_cache_loaded_bytes = s_cache_saved_bytes = 0;
    s_cache_save_failed = 0;
    if (!vkc_init_impl(request)) {
        vkc_teardown();
        return 0;
    }
    s_ready = 1;
    return 1;
}

void vk_core_shutdown(void)
{
    if (!s_ready) return;
    vkc_teardown();
}

int vk_core_attach_window(struct SDL_Window *window)
{
    if (!s_ready || !window || s_surface) return 0;
#if defined(PSX_SDL3)
    if (!SDL_Vulkan_CreateSurface(window, s_instance, NULL, &s_surface))
#else
    if (!SDL_Vulkan_CreateSurface(window, s_instance, &s_surface))
#endif
    {
        s_surface = VK_NULL_HANDLE;
        vkc_fail("SDL_Vulkan_CreateSurface failed");
        return 0;
    }
    VkBool32 present = VK_FALSE;
    VkResult r = vkGetPhysicalDeviceSurfaceSupportKHR(s_phys, s_qfam, s_surface, &present);
    if (r != VK_SUCCESS || !present) {
        vkDestroySurfaceKHR(s_instance, s_surface, NULL);
        s_surface = VK_NULL_HANDLE;
        vkc_fail("queue family cannot present to the window");
        return 0;
    }
    s_window = window;
    return 1;
}

void vk_core_detach_window(void)
{
    if (s_surface) vkDestroySurfaceKHR(s_instance, s_surface, NULL);
    s_surface = VK_NULL_HANDLE;
    s_window = NULL;
}

int vk_core_ready(void) { return s_ready; }
const char *vk_core_failure(void) { return s_failure; }
const VkCoreCaps *vk_core_caps(void) { return &s_caps; }

VkInstance       vk_core_instance(void) { return s_instance; }
VkPhysicalDevice vk_core_physical_device(void) { return s_phys; }
VkDevice         vk_core_device(void) { return s_dev; }
uint32_t         vk_core_queue_family(void) { return s_qfam; }
VkSurfaceKHR     vk_core_surface(void) { return s_surface; }
struct SDL_Window *vk_core_window(void) { return s_window; }
VmaAllocator     vk_core_allocator(void) { return s_alloc; }
VkPipelineCache  vk_core_pipeline_cache(void) { return s_cache; }

/* ---- queue ---------------------------------------------------------------- */
VkResult vk_core_submit(uint32_t count, const VkSubmitInfo *submits, VkFence fence)
{
    SDL_LockMutex(s_queue_mutex);
    VkResult r = vkQueueSubmit(s_queue, count, submits, fence);
    SDL_UnlockMutex(s_queue_mutex);
    vk_core_check(r, "vkQueueSubmit");
    return r;
}

void vk_core_queue_lock(void) { SDL_LockMutex(s_queue_mutex); }
void vk_core_queue_unlock(void) { SDL_UnlockMutex(s_queue_mutex); }

VkResult vk_core_present(const VkPresentInfoKHR *info)
{
    SDL_LockMutex(s_queue_mutex);
    VkResult r = vkQueuePresentKHR(s_queue, info);
    SDL_UnlockMutex(s_queue_mutex);
    /* Out-of-date and surface loss are the swapchain's to handle. */
    if (r == VK_ERROR_DEVICE_LOST) vk_core_check(r, "vkQueuePresentKHR");
    return r;
}

VkResult vk_core_queue_wait_idle(void)
{
    SDL_LockMutex(s_queue_mutex);
    VkResult r = vkQueueWaitIdle(s_queue);
    SDL_UnlockMutex(s_queue_mutex);
    vk_core_check(r, "vkQueueWaitIdle");
    return r;
}

VkResult vk_core_device_wait_idle(void)
{
    SDL_LockMutex(s_queue_mutex);   /* waits on the queue, which is externally synchronized */
    VkResult r = vkDeviceWaitIdle(s_dev);
    SDL_UnlockMutex(s_queue_mutex);
    vk_core_check(r, "vkDeviceWaitIdle");
    return r;
}

/* ---- resources ------------------------------------------------------------ */
void vk_core_name(VkObjectType type, uint64_t handle, const char *name)
{
    if (!s_caps.debug_utils || !name || !handle) return;
    VkDebugUtilsObjectNameInfoEXT ni = { VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT };
    ni.objectType = type;
    ni.objectHandle = handle;
    ni.pObjectName = name;
    vkSetDebugUtilsObjectNameEXT(s_dev, &ni);
}

int vk_core_image_create(VkCoreImage *out, VkFormat format, uint32_t width, uint32_t height,
                         uint32_t mip_levels, VkImageUsageFlags usage, const char *name)
{
    return vk_core_image_create_array(out, format, width, height, mip_levels, 1u, usage, name);
}

int vk_core_image_create_array(VkCoreImage *out, VkFormat format, uint32_t width, uint32_t height,
                               uint32_t mip_levels, uint32_t layers, VkImageUsageFlags usage, const char *name)
{
    memset(out, 0, sizeof(*out));
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent.width = width;
    ci.extent.height = height;
    ci.extent.depth = 1;
    ci.mipLevels = mip_levels ? mip_levels : 1;
    ci.arrayLayers = layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai;
    memset(&ai, 0, sizeof(ai));
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VkResult r = vmaCreateImage(s_alloc, &ci, &ai, &out->image, &out->allocation, NULL);
    if (!vk_core_check(r, "vmaCreateImage")) {
        memset(out, 0, sizeof(*out));
        return 0;
    }
    out->format = format;
    out->width = width;
    out->height = height;
    out->mip_levels = ci.mipLevels;
    /* A transfer-only image cannot have a view. */
    if (usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)) {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = out->image;
        vi.viewType = layers > 1u ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = ci.mipLevels;
        vi.subresourceRange.layerCount = layers;
        r = vkCreateImageView(s_dev, &vi, NULL, &out->view);
        if (!vk_core_check(r, "vkCreateImageView")) {
            vmaDestroyImage(s_alloc, out->image, out->allocation);
            memset(out, 0, sizeof(*out));
            return 0;
        }
    }
    vk_core_name(VK_OBJECT_TYPE_IMAGE, (uint64_t)(uintptr_t)out->image, name);
    vk_core_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t)(uintptr_t)out->view, name);
    return 1;
}

void vk_core_image_destroy(VkCoreImage *image)
{
    if (image->view) vkDestroyImageView(s_dev, image->view, NULL);
    if (image->image) vmaDestroyImage(s_alloc, image->image, image->allocation);
    memset(image, 0, sizeof(*image));
}

int vk_core_buffer_create(VkCoreBuffer *out, VkDeviceSize size, VkBufferUsageFlags usage,
                          VkCoreMemory memory, const char *name)
{
    memset(out, 0, sizeof(*out));
    VkBufferCreateInfo ci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    ci.size = size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo ai;
    memset(&ai, 0, sizeof(ai));
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    switch (memory) {
    case VK_CORE_MEMORY_UPLOAD:
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;
    case VK_CORE_MEMORY_READBACK:
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;
    default:
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        break;
    }
    VmaAllocationInfo info;
    VkResult r = vmaCreateBuffer(s_alloc, &ci, &ai, &out->buffer, &out->allocation, &info);
    if (!vk_core_check(r, "vmaCreateBuffer")) {
        memset(out, 0, sizeof(*out));
        return 0;
    }
    out->size = size;
    out->mapped = memory == VK_CORE_MEMORY_DEVICE ? NULL : info.pMappedData;
    vk_core_name(VK_OBJECT_TYPE_BUFFER, (uint64_t)(uintptr_t)out->buffer, name);
    return 1;
}

void vk_core_buffer_destroy(VkCoreBuffer *buffer)
{
    if (buffer->buffer) vmaDestroyBuffer(s_alloc, buffer->buffer, buffer->allocation);
    memset(buffer, 0, sizeof(*buffer));
}

void vk_core_buffer_flush(const VkCoreBuffer *buffer, VkDeviceSize offset, VkDeviceSize size)
{
    vmaFlushAllocation(s_alloc, buffer->allocation, offset, size);
}

void vk_core_buffer_invalidate(const VkCoreBuffer *buffer, VkDeviceSize offset, VkDeviceSize size)
{
    vmaInvalidateAllocation(s_alloc, buffer->allocation, offset, size);
}

VkShaderModule vk_core_shader_module(const uint32_t *code, size_t bytes, const char *name)
{
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    VkResult r = vkCreateShaderModule(s_dev, &ci, NULL, &module);
    if (!vk_core_check(r, "vkCreateShaderModule")) return VK_NULL_HANDLE;
    vk_core_name(VK_OBJECT_TYPE_SHADER_MODULE, (uint64_t)(uintptr_t)module, name);
    return module;
}

/* ---- one-shot commands ----------------------------------------------------- */
VkCommandBuffer vk_core_oneshot_begin(void)
{
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = s_oneshot_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    SDL_LockMutex(s_pool_mutex);
    VkResult r = vkAllocateCommandBuffers(s_dev, &ai, &cmd);
    SDL_UnlockMutex(s_pool_mutex);
    if (!vk_core_check(r, "vkAllocateCommandBuffers")) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!vk_core_check(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer")) {
        SDL_LockMutex(s_pool_mutex);
        vkFreeCommandBuffers(s_dev, s_oneshot_pool, 1, &cmd);
        SDL_UnlockMutex(s_pool_mutex);
        return VK_NULL_HANDLE;
    }
    return cmd;
}

int vk_core_oneshot_end_wait(VkCommandBuffer cmd)
{
    int ok = 0;
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (vk_core_check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer") &&
        vk_core_check(vkCreateFence(s_dev, &fci, NULL, &fence), "vkCreateFence")) {
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        ok = vk_core_check(vk_core_submit(1, &si, fence), "oneshot submit") &&
             vk_core_check(vkWaitForFences(s_dev, 1, &fence, VK_TRUE, UINT64_MAX), "oneshot wait");
    }
    if (fence) vkDestroyFence(s_dev, fence, NULL);
    SDL_LockMutex(s_pool_mutex);
    vkFreeCommandBuffers(s_dev, s_oneshot_pool, 1, &cmd);
    SDL_UnlockMutex(s_pool_mutex);
    return ok;
}

/* Access and stage masks for one side of a layout transition. */
static void vkc_layout_sync(VkImageLayout layout, int is_dst, VkAccessFlags *access,
                            VkPipelineStageFlags *stage)
{
    switch (layout) {
    case VK_IMAGE_LAYOUT_UNDEFINED:
        if (!is_dst) {
            /* ALL_COMMANDS, not TOP_OF_PIPE: a transition out of UNDEFINED may
             * directly follow a semaphore wait (swapchain acquire) and must
             * execute after it, whatever stage the wait names. */
            *access = 0;
            *stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            return;
        }
        break;
    case VK_IMAGE_LAYOUT_GENERAL:
        *access = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        *stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        return;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        *access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        *stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        return;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        *access = VK_ACCESS_SHADER_READ_BIT;
        *stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        return;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        *access = VK_ACCESS_TRANSFER_READ_BIT;
        *stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        return;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        *access = VK_ACCESS_TRANSFER_WRITE_BIT;
        *stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        return;
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
        /* Presentation reads through the semaphore, not an access mask. As a
         * source, BOTTOM_OF_PIPE orders after a render pass that ended in it. */
        *access = 0;
        *stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        return;
    default:
        break;
    }
    *access = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    *stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
}

void vk_core_image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    VkPipelineStageFlags src_stage, dst_stage;
    vkc_layout_sync(from, 0, &b.srcAccessMask, &src_stage);
    vkc_layout_sync(to, 1, &b.dstAccessMask, &dst_stage);
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    b.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

/* ---- diagnostics ----------------------------------------------------------- */
typedef struct VkcJson {
    char *out;
    int cap;
    int len;
} VkcJson;

static void vkc_jf(VkcJson *j, const char *fmt, ...)
{
    int room = j->len < j->cap ? j->cap - j->len : 0;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(room ? j->out + j->len : NULL, (size_t)room, fmt, ap);
    va_end(ap);
    if (n > 0) j->len += n;
}

static void vkc_jc(VkcJson *j, char c)
{
    if (j->len + 1 < j->cap) {
        j->out[j->len] = c;
        j->out[j->len + 1] = '\0';
    }
    ++j->len;
}

/* Quoted, escaped JSON string. */
static void vkc_js(VkcJson *j, const char *s)
{
    vkc_jc(j, '"');
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            vkc_jc(j, '\\');
            vkc_jc(j, (char)c);
        } else if (c < 0x20) {
            vkc_jf(j, "\\u%04x", (unsigned)c);
        } else {
            vkc_jc(j, (char)c);
        }
    }
    vkc_jc(j, '"');
}

static void vkc_json_names(VkcJson *j, const VkcExtName *names, uint32_t n)
{
    vkc_jc(j, '[');
    for (uint32_t i = 0; i < n; ++i) {
        if (i) vkc_jc(j, ',');
        vkc_js(j, names[i]);
    }
    vkc_jc(j, ']');
}

int vk_core_info_json(char *out, int capacity)
{
    VkcJson j = { out, capacity, 0 };
    if (capacity > 0) out[0] = '\0';
    if (s_diag_mutex) SDL_LockMutex(s_diag_mutex);
    vkc_jf(&j, "{\"available\":true,\"ready\":%d,\"failure\":", s_ready);
    vkc_js(&j, s_failure);
    vkc_jf(&j, ",\"first_check_failure\":{\"failed\":%d,\"result\":%d,\"site\":", s_check_failed,
           (int)s_check_result);
    vkc_js(&j, s_check_site);
    uint64_t warnings = s_total_warnings, errors = s_total_errors, other = s_total_other;
    uint64_t seq = s_msg_seq;
    if (s_diag_mutex) SDL_UnlockMutex(s_diag_mutex);
    vkc_jf(&j, "},\"validation\":{\"active\":%d,\"warnings\":%llu,\"errors\":%llu,\"other\":%llu,"
               "\"messages\":%llu}",
           s_caps.validation, (unsigned long long)warnings, (unsigned long long)errors,
           (unsigned long long)other, (unsigned long long)seq);
    if (s_ready) {
        const VkCoreCaps *c = &s_caps;
        vkc_jf(&j, ",\"device\":{\"name\":");
        vkc_js(&j, c->device_name);
        vkc_jf(&j, ",\"vendor_id\":%u,\"device_id\":%u,\"driver_version\":%u,\"type\":%d,"
                   "\"api_version\":\"%u.%u.%u\",\"queue_family\":%u,\"driver_name\":",
               c->vendor_id, c->device_id, c->driver_version, (int)c->device_type,
               VK_API_VERSION_MAJOR(c->api_version), VK_API_VERSION_MINOR(c->api_version),
               VK_API_VERSION_PATCH(c->api_version), s_qfam);
        vkc_js(&j, s_driver_name);
        vkc_jf(&j, ",\"driver_info\":");
        vkc_js(&j, s_driver_info);
        vkc_jf(&j, "},\"caps\":{\"fragment_shader_interlock\":%d,\"fragment_stores_and_atomics\":%d,"
                   "\"shader_float64\":%d,\"shader_int64\":%d,\"sampler_anisotropy\":%d,"
                   "\"dual_src_blend\":%d,\"independent_blend\":%d,\"timeline_semaphore\":%d,"
                   "\"present_id\":%d,\"present_wait\":%d,\"pipeline_statistics\":%d,"
                   "\"timestamps\":%d,\"debug_utils\":%d,\"validation\":%d,"
                   "\"timestamp_period_ns\":%.6f,\"max_sampler_anisotropy\":%.1f,"
                   "\"max_image_dimension_2d\":%u,\"max_push_constants_size\":%u,"
                   "\"max_texel_buffer_elements\":%u,\"max_uniform_buffer_range\":%u,"
                   "\"max_color_attachments\":%u,\"min_uniform_buffer_offset_alignment\":%llu,"
                   "\"min_texel_buffer_offset_alignment\":%llu,\"non_coherent_atom_size\":%llu}",
               c->fragment_shader_interlock, c->fragment_stores_and_atomics, c->shader_float64,
               c->shader_int64, c->sampler_anisotropy, c->dual_src_blend, c->independent_blend,
               c->timeline_semaphore, c->present_id, c->present_wait, c->pipeline_statistics,
               c->timestamps, c->debug_utils, c->validation, (double)c->timestamp_period_ns,
               (double)c->max_sampler_anisotropy, c->max_image_dimension_2d,
               c->max_push_constants_size, c->max_texel_buffer_elements, c->max_uniform_buffer_range,
               c->max_color_attachments, (unsigned long long)c->min_uniform_buffer_offset_alignment,
               (unsigned long long)c->min_texel_buffer_offset_alignment,
               (unsigned long long)c->non_coherent_atom_size);
        vkc_jf(&j, ",\"instance_extensions\":");
        vkc_json_names(&j, s_inst_exts, s_inst_ext_count);
        vkc_jf(&j, ",\"device_extensions\":");
        vkc_json_names(&j, s_dev_exts, s_dev_ext_count);
        vkc_jf(&j, ",\"pipeline_cache\":{\"path\":");
        vkc_js(&j, s_cache_path);
        vkc_jf(&j, ",\"loaded_bytes\":%u,\"saved_bytes\":%u,\"save_failed\":%d}", s_cache_loaded_bytes,
               s_cache_saved_bytes, s_cache_save_failed);
    }
    vkc_jf(&j, ",\"swapchain\":");
    int room = j.len < j.cap ? j.cap - j.len : 0;
    int n = vk_swapchain_diagnostics_json(room ? j.out + j.len : NULL, room);
    if (n > 0) j.len += n;
    vkc_jc(&j, '}');
    return j.len;
}

uint64_t vk_core_validation_message_count(void)
{
    if (!s_diag_mutex) return 0;
    SDL_LockMutex(s_diag_mutex);
    uint64_t n = s_msg_seq;
    SDL_UnlockMutex(s_diag_mutex);
    return n;
}

int vk_core_validation_json(char *out, int capacity, int count)
{
    VkcJson j = { out, capacity, 0 };
    if (capacity > 0) out[0] = '\0';
    if (s_diag_mutex) SDL_LockMutex(s_diag_mutex);
    uint64_t kept = s_msg_seq < VKC_RING ? s_msg_seq : VKC_RING;
    uint64_t want = (count > 0 && (uint64_t)count < kept) ? (uint64_t)count : kept;
    vkc_jf(&j, "{\"available\":true,\"total\":%llu,\"kept\":%llu,\"warnings\":%llu,\"errors\":%llu,"
               "\"messages\":[",
           (unsigned long long)s_msg_seq, (unsigned long long)kept,
           (unsigned long long)s_total_warnings, (unsigned long long)s_total_errors);
    for (uint64_t i = 0; i < want; ++i) {
        const VkcMessage *m = &s_ring[(s_msg_seq - want + i) % VKC_RING];
        const char *sev = (m->severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "error" :
                          (m->severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ? "warning" :
                          (m->severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) ? "info" : "verbose";
        vkc_jf(&j, "%s{\"seq\":%llu,\"severity\":\"%s\",\"type\":%u,\"id\":%d,\"message\":", i ? "," : "",
               (unsigned long long)m->seq, sev, m->type, m->id);
        vkc_js(&j, m->text);
        vkc_jc(&j, '}');
    }
    if (s_diag_mutex) SDL_UnlockMutex(s_diag_mutex);
    vkc_jf(&j, "]}");
    return j.len;
}

#endif  /* PSX_HAVE_VULKAN */
