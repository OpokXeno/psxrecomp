/* The window swapchain and present pacing on vk_core (see vk_swapchain.h for
 * the contract). Extracted from the swapchain half of the original GL->Vulkan
 * presenter, whose policy it keeps: at least three images, nvp_pick_mode's
 * present-mode choice, present_id / present_wait queue-depth bounding, an
 * acquire semaphore ring, and a fence-guarded command buffer per frame in
 * flight. */

#include <stdint.h>

#if !defined(PSX_HAVE_VULKAN)

/* Built without Vulkan: vk_swapchain.h needs volk, which such a build does not
 * have, and nothing outside the Vulkan build can call into this file. */
typedef int vk_swapchain_unavailable_t;

#else  /* PSX_HAVE_VULKAN */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psx_sdl.h"
#include "vk_swapchain.h"

#define VKS_FRAMES 8                 /* maximum frames in flight */
#define VKS_MAX_IMAGES 8             /* swapchain images supported */
#define VKS_MAX_EXTRA 4              /* extra wait / signal semaphores per present */
#define VKS_ACQUIRE_TIMEOUT_NS 100000000ull
#define VKS_PRESENT_WAIT_TIMEOUT_NS 100000000ull
#define VKS_ACQUIRE_STAGES (VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT)

typedef struct VksFrame {
    VkCommandBuffer cmd;
    VkFence fence;
} VksFrame;

static int s_active;
static VkSwapchainKHR s_swapchain;
static VkFormat s_format;
static VkExtent2D s_extent;
static uint32_t s_count;
static VkImage s_images[VKS_MAX_IMAGES];
static VkImageView s_views[VKS_MAX_IMAGES];
static VkFramebuffer s_fbs[VKS_MAX_IMAGES];
static VkSemaphore s_present_sem[VKS_MAX_IMAGES];
static VkSemaphore s_acquire_sem[VKS_MAX_IMAGES + 1];
static uint32_t s_acquire_cursor;
static VkRenderPass s_render_pass;
static VkCommandPool s_pool;
static VksFrame s_frames[VKS_FRAMES];
static uint32_t s_frame_cursor;
static uint32_t s_frame_limit = 3u;
static VkSemaphore s_cur_acquire;    /* acquire semaphore of the frame being recorded */
static _Atomic int s_can_read;       /* swapchain images have TRANSFER_SRC */
static uint32_t s_mode_mask;         /* bit per supported VkPresentModeKHR 0..3 */
static VkPresentModeKHR s_present_mode;
static int s_have_mode;
static int s_win_w, s_win_h;         /* window pixel size at last creation */
static int s_need_recreate;
static uint64_t s_images_since_create;
static int s_applied_interval = 1;
static int s_applied_vsync = 1;
static char s_failure[128];          /* first failing create step */

static int s_use_present_id;         /* present ids are attached to presents */
static int s_use_present_wait;       /* present_wait enabled and loaded */
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

static _Atomic int s_req_interval = 1;
static _Atomic int s_req_vsync = 1;

/* capture handshake: 0 idle, 1 requested, 2 ready, 3 capturing */
static _Atomic int s_cap_state;
static unsigned char *s_cap_rgb;     /* ready result, owned until taken */
static int s_cap_w, s_cap_h;

/* diagnostics: written by the presenting thread, read from any thread */
static _Atomic int s_d_present_mode = -1;
static _Atomic uint32_t s_d_images, s_d_ext_w, s_d_ext_h;
static _Atomic uint64_t s_d_acquired, s_d_presented;
static _Atomic uint64_t s_d_acquire_timeouts, s_d_out_of_date, s_d_suboptimal;
static _Atomic uint64_t s_d_recreations, s_d_minimized_skips, s_d_failures;
static _Atomic uint64_t s_d_last_present_ns, s_d_max_block_ns;
static _Atomic int s_d_last_result;
static _Atomic int s_d_present_wait_on, s_d_present_depth;
static _Atomic uint64_t s_d_present_waits, s_d_present_wait_timeouts;
static _Atomic uint64_t s_d_present_wait_ns_total, s_d_present_wait_max_ns, s_d_last_present_id;
static int s_wait_diagnostics;
static _Atomic uint64_t s_d_frame_fence_ns, s_d_acquire_ns, s_d_submit_ns, s_d_present_ns;

#define VKS_ADD(v, n) atomic_fetch_add_explicit(&(v), (n), memory_order_relaxed)
#define VKS_GET(v) atomic_load_explicit(&(v), memory_order_relaxed)
#define VKS_SET(v, x) atomic_store_explicit(&(v), (x), memory_order_relaxed)

static uint64_t vks_now_ns(void)
{
#if defined(PSX_SDL3)
    return (uint64_t)SDL_GetTicksNS();
#else
    uint64_t c = SDL_GetPerformanceCounter(), f = SDL_GetPerformanceFrequency();
    return (c / f) * 1000000000ull + (c % f) * 1000000000ull / f;
#endif
}

static void vks_fail(VkResult r)
{
    VKS_ADD(s_d_failures, 1);
    VKS_SET(s_d_last_result, (int)r);
}

/* Records why vk_swapchain_create failed; the first reason wins. */
static int vks_create_fail(const char *reason)
{
    if (!s_failure[0]) snprintf(s_failure, sizeof(s_failure), "%s", reason);
    return 0;
}

/* ---- swapchain ----------------------------------------------------------- */
static VkPresentModeKHR vks_pick_mode(int interval, int user_vsync)
{
#define VKS_HAS(m) ((s_mode_mask >> (m)) & 1u)
    const char *forced = getenv("PSX_VK_PRESENT_MODE");
    if (forced) {
        if (!strcmp(forced, "fifo")) return VK_PRESENT_MODE_FIFO_KHR;
        if (!strcmp(forced, "mailbox") && VKS_HAS(VK_PRESENT_MODE_MAILBOX_KHR)) return VK_PRESENT_MODE_MAILBOX_KHR;
        if (!strcmp(forced, "immediate") && VKS_HAS(VK_PRESENT_MODE_IMMEDIATE_KHR)) return VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    if (interval >= 1) return VK_PRESENT_MODE_FIFO_KHR;
    if (interval == -1)
        return VKS_HAS(VK_PRESENT_MODE_FIFO_RELAXED_KHR) ? VK_PRESENT_MODE_FIFO_RELAXED_KHR
                                                         : VK_PRESENT_MODE_FIFO_KHR;
    if (user_vsync != 0) {
        if (VKS_HAS(VK_PRESENT_MODE_MAILBOX_KHR)) return VK_PRESENT_MODE_MAILBOX_KHR;
        if (VKS_HAS(VK_PRESENT_MODE_IMMEDIATE_KHR)) return VK_PRESENT_MODE_IMMEDIATE_KHR;
        return VK_PRESENT_MODE_FIFO_KHR;
    }
    if (VKS_HAS(VK_PRESENT_MODE_IMMEDIATE_KHR)) return VK_PRESENT_MODE_IMMEDIATE_KHR;
    if (VKS_HAS(VK_PRESENT_MODE_MAILBOX_KHR)) return VK_PRESENT_MODE_MAILBOX_KHR;
    return VK_PRESENT_MODE_FIFO_KHR;
#undef VKS_HAS
}

static uint32_t vks_clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Per-swapchain objects: framebuffers, views and semaphores. The device must
 * be idle. */
static void vks_destroy_images(void)
{
    VkDevice dev = vk_core_device();
    for (uint32_t i = 0; i < VKS_MAX_IMAGES; ++i) {
        if (s_fbs[i]) vkDestroyFramebuffer(dev, s_fbs[i], NULL);
        if (s_views[i]) vkDestroyImageView(dev, s_views[i], NULL);
        if (s_present_sem[i]) vkDestroySemaphore(dev, s_present_sem[i], NULL);
        s_fbs[i] = VK_NULL_HANDLE;
        s_views[i] = VK_NULL_HANDLE;
        s_present_sem[i] = VK_NULL_HANDLE;
    }
    for (uint32_t i = 0; i < VKS_MAX_IMAGES + 1; ++i) {
        if (s_acquire_sem[i]) vkDestroySemaphore(dev, s_acquire_sem[i], NULL);
        s_acquire_sem[i] = VK_NULL_HANDLE;
    }
    s_acquire_cursor = 0;
}

/* (Re)creates the swapchain for the current window size and present policy.
 * Returns 1 on success, 2 when the window is minimized (no swapchain), 0 on
 * failure (the previous swapchain, if any, is kept). */
static int vks_create_swapchain(void)
{
    VkDevice dev = vk_core_device();
    VkPhysicalDevice phys = vk_core_physical_device();
    VkSurfaceKHR surface = vk_core_surface();
    VkSurfaceCapabilitiesKHR caps;
    VkResult r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps);
    if (r != VK_SUCCESS) { vks_fail(r); return 0; }

    uint32_t mn = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &mn, NULL);
    if (mn > 16) mn = 16;
    VkPresentModeKHR modes[16];
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &mn, modes);
    s_mode_mask = 0;
    for (uint32_t i = 0; i < mn; ++i)
        if ((uint32_t)modes[i] < 32) s_mode_mask |= 1u << (uint32_t)modes[i];

    int ww = 0, wh = 0;
    SDL_GetWindowSizeInPixels(vk_core_window(), &ww, &wh);
    s_win_w = ww;
    s_win_h = wh;
    VkExtent2D ext;
    if (caps.currentExtent.width != 0xFFFFFFFFu) {
        ext = caps.currentExtent;
    } else {
        ext.width = vks_clamp_u32((uint32_t)(ww > 0 ? ww : 0), caps.minImageExtent.width,
                                  caps.maxImageExtent.width);
        ext.height = vks_clamp_u32((uint32_t)(wh > 0 ? wh : 0), caps.minImageExtent.height,
                                   caps.maxImageExtent.height);
        if (ww <= 0 || wh <= 0) ext.width = ext.height = 0;
    }

    vk_core_device_wait_idle();
    if (ext.width == 0 || ext.height == 0) {
        vks_destroy_images();
        if (s_swapchain) {
            vkDestroySwapchainKHR(dev, s_swapchain, NULL);
            s_swapchain = VK_NULL_HANDLE;
        }
        s_count = 0;
        s_have_mode = 0;
        s_need_recreate = 0;
        VKS_SET(s_d_present_mode, -1);
        VKS_SET(s_d_images, 0);
        return 2;
    }

    uint32_t want_images = caps.minImageCount > s_frame_limit ? caps.minImageCount : s_frame_limit;
    /* Presentation-engine retention and render submissions are independent:
     * allow extra swap images without lengthening the CPU render-frame ring. */
    const char *image_count = getenv("PSX_VK_SWAPCHAIN_IMAGES");
    if (image_count) {
        const unsigned long count = strtoul(image_count, NULL, 10);
        if (count >= caps.minImageCount && count <= VKS_MAX_IMAGES) want_images = (uint32_t)count;
    }
    if (caps.maxImageCount != 0 && want_images > caps.maxImageCount) want_images = caps.maxImageCount;
    if (want_images > VKS_MAX_IMAGES) want_images = VKS_MAX_IMAGES;
    if (want_images < caps.minImageCount) {
        vks_create_fail("surface needs more swapchain images than supported");
        vks_fail(VK_ERROR_INITIALIZATION_FAILED);
        return 0;
    }
    const VkImageUsageFlags need = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if ((caps.supportedUsageFlags & need) != need) {
        vks_create_fail("swapchain images lack color attachment or transfer destination usage");
        vks_fail(VK_ERROR_INITIALIZATION_FAILED);
        return 0;
    }

    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) {
        for (uint32_t b = 1; b <= 8; b <<= 1) {
            if (caps.supportedCompositeAlpha & b) { alpha = (VkCompositeAlphaFlagBitsKHR)b; break; }
        }
    }

    VkPresentModeKHR mode = vks_pick_mode(s_applied_interval, s_applied_vsync);
    VkSwapchainKHR old = s_swapchain;
    VkSwapchainCreateInfoKHR sci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    sci.surface = surface;
    sci.minImageCount = want_images;
    sci.imageFormat = s_format;
    sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    sci.imageExtent = ext;
    sci.imageArrayLayers = 1;
    int can_read = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    sci.imageUsage = need | (can_read ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = alpha;
    sci.presentMode = mode;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = old;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    r = vkCreateSwapchainKHR(dev, &sci, NULL, &sc);
    if (r != VK_SUCCESS) { vks_fail(r); return 0; }
    vks_destroy_images();
    if (old) vkDestroySwapchainKHR(dev, old, NULL);
    s_swapchain = sc;
    /* Present ids of the old swapchain mean nothing to the new one. */
    s_sc_first_id = 0;
    s_last_presented_id = 0;
    atomic_store(&s_can_read, can_read);

    uint32_t cnt = 0;
    vkGetSwapchainImagesKHR(dev, sc, &cnt, NULL);
    if (cnt > VKS_MAX_IMAGES) {
        vks_create_fail("swapchain returned more images than supported");
        vkDestroySwapchainKHR(dev, sc, NULL);
        s_swapchain = VK_NULL_HANDLE;
        s_count = 0;
        vks_fail(VK_ERROR_INITIALIZATION_FAILED);
        return 0;
    }
    vkGetSwapchainImagesKHR(dev, sc, &cnt, s_images);
    s_count = cnt;
    VkSemaphoreCreateInfo semi = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (uint32_t i = 0; i < cnt; ++i) {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = s_images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = s_format;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fi.renderPass = s_render_pass;
        fi.attachmentCount = 1;
        fi.pAttachments = &s_views[i];
        fi.width = ext.width;
        fi.height = ext.height;
        fi.layers = 1;
        if (!vk_core_check(vkCreateImageView(dev, &vi, NULL, &s_views[i]), "swapchain image view") ||
            !vk_core_check(vkCreateFramebuffer(dev, &fi, NULL, &s_fbs[i]), "swapchain framebuffer") ||
            !vk_core_check(vkCreateSemaphore(dev, &semi, NULL, &s_present_sem[i]), "present semaphore")) {
            s_need_recreate = 1;
            vks_fail(VK_ERROR_INITIALIZATION_FAILED);
            return 0;
        }
    }
    for (uint32_t i = 0; i < cnt + 1; ++i) {
        if (!vk_core_check(vkCreateSemaphore(dev, &semi, NULL, &s_acquire_sem[i]), "acquire semaphore")) {
            s_need_recreate = 1;
            vks_fail(VK_ERROR_INITIALIZATION_FAILED);
            return 0;
        }
    }
    s_extent = ext;
    s_present_mode = mode;
    s_have_mode = 1;
    s_need_recreate = 0;
    s_images_since_create = 0;
    VKS_SET(s_d_present_mode, (int)mode);
    VKS_SET(s_d_images, cnt);
    VKS_SET(s_d_ext_w, ext.width);
    VKS_SET(s_d_ext_h, ext.height);
    VKS_ADD(s_d_recreations, 1);
    return 1;
}

/* Applies pending policy and size changes. Called at the top of each frame. */
static void vks_refresh_swapchain(void)
{
    int ww = 0, wh = 0;
    SDL_GetWindowSizeInPixels(vk_core_window(), &ww, &wh);
    int need = s_need_recreate || !s_swapchain || ww != s_win_w || wh != s_win_h ||
               (s_have_mode && vks_pick_mode(s_applied_interval, s_applied_vsync) != s_present_mode);
    if (need) (void)vks_create_swapchain();
}

/* ---- create / destroy ------------------------------------------------------ */
static int vks_pick_format(void)
{
    VkPhysicalDevice phys = vk_core_physical_device();
    VkSurfaceKHR surface = vk_core_surface();
    uint32_t fn = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fn, NULL);
    if (fn > 64) fn = 64;
    VkSurfaceFormatKHR fmts[64];
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fn, fmts);
    static const VkFormat prefer[2] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM };
    for (int p = 0; p < 2; ++p) {
        for (uint32_t i = 0; i < fn; ++i) {
            if (fmts[i].format == prefer[p] && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                s_format = prefer[p];
                return 1;
            }
        }
    }
    return vks_create_fail("surface offers no UNORM format");
}

static int vks_create_render_pass(void)
{
    VkAttachmentDescription att;
    memset(&att, 0, sizeof(att));
    att.format = s_format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub;
    memset(&sub, 0, sizeof(sub));
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep;
    memset(&dep, 0, sizeof(dep));
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo ci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    ci.attachmentCount = 1;
    ci.pAttachments = &att;
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    ci.dependencyCount = 1;
    ci.pDependencies = &dep;
    if (!vk_core_check(vkCreateRenderPass(vk_core_device(), &ci, NULL, &s_render_pass), "swapchain render pass")) {
        s_render_pass = VK_NULL_HANDLE;
        return vks_create_fail("render pass creation failed");
    }
    vk_core_name(VK_OBJECT_TYPE_RENDER_PASS, (uint64_t)(uintptr_t)s_render_pass, "swapchain render pass");
    return 1;
}

static int vks_create_frames(void)
{
    VkDevice dev = vk_core_device();
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = vk_core_queue_family();
    if (!vk_core_check(vkCreateCommandPool(dev, &pci, NULL, &s_pool), "swapchain command pool")) {
        s_pool = VK_NULL_HANDLE;
        return vks_create_fail("command pool creation failed");
    }
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = s_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (int i = 0; i < VKS_FRAMES; ++i) {
        if (!vk_core_check(vkAllocateCommandBuffers(dev, &cai, &s_frames[i].cmd), "swapchain command buffer")) {
            s_frames[i].cmd = VK_NULL_HANDLE;
            return vks_create_fail("command buffer allocation failed");
        }
        if (!vk_core_check(vkCreateFence(dev, &fci, NULL, &s_frames[i].fence), "swapchain fence")) {
            s_frames[i].fence = VK_NULL_HANDLE;
            return vks_create_fail("fence creation failed");
        }
    }
    return 1;
}

/* Presents one cleared frame so the compositor maps the window. */
static int vks_present_black(void)
{
    VkSwapFrame f;
    VkSwapAcquire a = vk_swapchain_acquire(&f);
    if (a == VK_SWAP_ACQUIRE_SKIP) return 1;
    if (a != VK_SWAP_ACQUIRE_OK) return 0;
    vk_swapchain_frame_layout(&f, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue black = { { 0.0f, 0.0f, 0.0f, 1.0f } };
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(f.cmd, f.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    if (!vk_swapchain_present(&f, 0, NULL, NULL, 0, NULL)) return 0;
    return vk_core_check(vkWaitForFences(vk_core_device(), 1, &s_frames[f.slot].fence, VK_TRUE, UINT64_MAX),
                         "first present wait");
}

static int vks_create_impl(void)
{
    const char *in_flight = getenv("PSX_VK_IN_FLIGHT_IMAGES");
    s_frame_limit = 3u;
    if (in_flight) {
        unsigned long count = strtoul(in_flight, NULL, 10);
        if (count >= 3u && count <= VKS_FRAMES) s_frame_limit = (uint32_t)count;
    }
    if (!vk_core_ready() || !vk_core_surface() || !vk_core_window())
        return vks_create_fail("vk_core has no window attached");
    if (!vks_pick_format()) return 0;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(vk_core_physical_device(), s_format, &fp);
    const VkFormatFeatureFlags feat = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    if ((fp.optimalTilingFeatures & feat) != feat)
        return vks_create_fail("swapchain format cannot be a color attachment and blit destination");
    if (!vks_create_render_pass()) return 0;
    if (!vks_create_frames()) return 0;

    const VkCoreCaps *caps = vk_core_caps();
    s_use_present_id = caps->present_id;
    s_use_present_wait = caps->present_wait && vkWaitForPresentKHR != NULL;
    const char *denv = getenv("PSX_VK_PRESENT_DEPTH");
    s_present_depth = 3;
    if (denv && denv[0] >= '0' && denv[0] <= '3' && !denv[1]) s_present_depth = denv[0] - '0';
    VKS_SET(s_d_present_depth, s_present_depth);
    VKS_SET(s_d_present_wait_on, s_use_present_wait);

    s_applied_interval = atomic_load(&s_req_interval);
    s_applied_vsync = atomic_load(&s_req_vsync);
    if (vks_create_swapchain() == 0) return vks_create_fail("swapchain creation failed");
    s_active = 1;
    if (s_swapchain && !vks_present_black()) {
        s_active = 0;
        return vks_create_fail("first present failed");
    }
    return 1;
}

int vk_swapchain_create(void)
{
    const char *waits = getenv("PSX_VK_WAIT_DIAGNOSTICS");
    s_wait_diagnostics = waits && waits[0] == '1';
    s_failure[0] = '\0';
    if (!vks_create_impl()) {
        vk_swapchain_destroy();
        return 0;
    }
    return 1;
}

void vk_swapchain_destroy(void)
{
    VkDevice dev = vk_core_device();
    if (dev) {
        vk_core_device_wait_idle();
        vks_destroy_images();
        for (int i = 0; i < VKS_FRAMES; ++i) {
            if (s_frames[i].fence) vkDestroyFence(dev, s_frames[i].fence, NULL);
            if (s_frames[i].cmd && s_pool) vkFreeCommandBuffers(dev, s_pool, 1, &s_frames[i].cmd);
        }
        if (s_swapchain) vkDestroySwapchainKHR(dev, s_swapchain, NULL);
        if (s_pool) vkDestroyCommandPool(dev, s_pool, NULL);
        if (s_render_pass) vkDestroyRenderPass(dev, s_render_pass, NULL);
    }
    memset(s_frames, 0, sizeof(s_frames));
    s_swapchain = VK_NULL_HANDLE;
    s_pool = VK_NULL_HANDLE;
    s_render_pass = VK_NULL_HANDLE;
    atomic_store(&s_can_read, 0);
    free(s_cap_rgb);
    s_cap_rgb = NULL;
    atomic_store(&s_cap_state, 0);

    s_active = 0;
    s_count = 0;
    s_have_mode = 0;
    s_mode_mask = 0;
    s_need_recreate = 0;
    s_win_w = s_win_h = 0;
    s_extent.width = s_extent.height = 0;
    s_frame_cursor = 0;
    s_cur_acquire = VK_NULL_HANDLE;
    s_images_since_create = 0;
    s_use_present_id = s_use_present_wait = 0;
    s_sc_first_id = 0;
    s_last_presented_id = 0;
    VKS_SET(s_d_present_wait_on, 0);
    VKS_SET(s_d_present_mode, -1);
    VKS_SET(s_d_images, 0);
    VKS_SET(s_d_ext_w, 0);
    VKS_SET(s_d_ext_h, 0);
}

int vk_swapchain_active(void) { return s_active; }

void vk_swapchain_set_present_policy(int swap_interval, int user_vsync)
{
    atomic_store(&s_req_interval, swap_interval);
    atomic_store(&s_req_vsync, user_vsync);
}

VkRenderPass vk_swapchain_render_pass(void) { return s_render_pass; }

/* ---- frame ------------------------------------------------------------------ */
VkSwapAcquire vk_swapchain_acquire(VkSwapFrame *frame)
{
    if (!s_active) return VK_SWAP_ACQUIRE_FAILED;
    VkDevice dev = vk_core_device();
    s_applied_interval = atomic_load(&s_req_interval);
    s_applied_vsync = atomic_load(&s_req_vsync);
    vks_refresh_swapchain();
    if (!s_swapchain) {
        VKS_ADD(s_d_minimized_skips, 1);
        return VK_SWAP_ACQUIRE_SKIP;
    }

    uint32_t slot = s_frame_cursor;
    VksFrame *f = &s_frames[slot];
    uint64_t wait_started = s_wait_diagnostics ? vks_now_ns() : 0u;
    VkResult vr = vkWaitForFences(dev, 1, &f->fence, VK_TRUE, UINT64_MAX);
    if (s_wait_diagnostics) VKS_ADD(s_d_frame_fence_ns, vks_now_ns() - wait_started);
    if (!vk_core_check(vr, "frame fence wait")) { vks_fail(vr); return VK_SWAP_ACQUIRE_FAILED; }

    if (s_use_present_wait && s_present_depth > 0 &&
        (s_present_mode == VK_PRESENT_MODE_FIFO_KHR || s_present_mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR) &&
        s_last_presented_id != 0 && s_last_presented_id > (uint64_t)(s_present_depth - 1)) {
        /* Bound the not-yet-displayed images: wait for the image queued depth
         * presents ago to reach the screen. */
        uint64_t target = s_last_presented_id - (uint64_t)(s_present_depth - 1);
        if (target > 0 && target >= s_sc_first_id) {
            uint64_t w0 = vks_now_ns();
            VkResult wr = vkWaitForPresentKHR(dev, s_swapchain, target, VKS_PRESENT_WAIT_TIMEOUT_NS);
            uint64_t wdt = vks_now_ns() - w0;
            VKS_ADD(s_d_present_waits, 1);
            VKS_ADD(s_d_present_wait_ns_total, wdt);
            if (wdt > VKS_GET(s_d_present_wait_max_ns)) VKS_SET(s_d_present_wait_max_ns, wdt);
            if (wr == VK_TIMEOUT) {
                VKS_ADD(s_d_present_wait_timeouts, 1);
            } else if (wr == VK_ERROR_OUT_OF_DATE_KHR) {
                VKS_ADD(s_d_out_of_date, 1);
                s_need_recreate = 1;
            } else if (wr != VK_SUCCESS && wr != VK_SUBOPTIMAL_KHR) {
                vks_fail(wr);
                vk_core_check(wr, "vkWaitForPresentKHR");
            }
        }
    }

    VkSemaphore acq = s_acquire_sem[s_acquire_cursor];
    uint32_t ii = 0;
    wait_started = s_wait_diagnostics ? vks_now_ns() : 0u;
    vr = vkAcquireNextImageKHR(dev, s_swapchain, VKS_ACQUIRE_TIMEOUT_NS, acq, VK_NULL_HANDLE, &ii);
    if (s_wait_diagnostics) VKS_ADD(s_d_acquire_ns, vks_now_ns() - wait_started);
    if (vr != VK_SUCCESS && vr != VK_SUBOPTIMAL_KHR) {
        if (vr == VK_ERROR_OUT_OF_DATE_KHR) {
            VKS_ADD(s_d_out_of_date, 1);
            s_need_recreate = 1;
        } else if (vr == VK_TIMEOUT || vr == VK_NOT_READY) {
            VKS_ADD(s_d_acquire_timeouts, 1);
        } else {
            vks_fail(vr);
            vk_core_check(vr, "vkAcquireNextImageKHR");
            s_need_recreate = 1;
        }
        return VK_SWAP_ACQUIRE_FAILED;
    }
    s_acquire_cursor = (s_acquire_cursor + 1) % (s_count + 1);
    if (vr == VK_SUBOPTIMAL_KHR) {
        VKS_ADD(s_d_suboptimal, 1);
        /* A swapchain that is suboptimal from its first image would otherwise
         * be rebuilt every frame. */
        if (s_images_since_create >= 60) s_need_recreate = 1;
    }
    s_frame_cursor = (s_frame_cursor + 1) % s_frame_limit;
    s_cur_acquire = acq;

    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!vk_core_check(vkResetFences(dev, 1, &f->fence), "frame fence reset") ||
        !vk_core_check(vkResetCommandBuffer(f->cmd, 0), "frame command buffer reset") ||
        !vk_core_check(vkBeginCommandBuffer(f->cmd, &bi), "frame command buffer begin")) {
        vks_fail(VK_ERROR_INITIALIZATION_FAILED);
        return VK_SWAP_ACQUIRE_FAILED;
    }
    frame->cmd = f->cmd;
    frame->image = s_images[ii];
    frame->view = s_views[ii];
    frame->framebuffer = s_fbs[ii];
    frame->format = s_format;
    frame->width = s_extent.width;
    frame->height = s_extent.height;
    frame->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    frame->image_index = ii;
    frame->slot = slot;
    VKS_ADD(s_d_acquired, 1);
    return VK_SWAP_ACQUIRE_OK;
}

void vk_swapchain_frame_layout(VkSwapFrame *frame, VkImageLayout layout)
{
    if (frame->layout == layout) return;
    vk_core_image_barrier(frame->cmd, frame->image, frame->layout, layout);
    frame->layout = layout;
}

void vk_swapchain_begin_render_pass(VkSwapFrame *frame)
{
    VkClearValue black;
    memset(&black, 0, sizeof(black));
    black.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo rb = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rb.renderPass = s_render_pass;
    rb.framebuffer = frame->framebuffer;
    rb.renderArea.extent.width = frame->width;
    rb.renderArea.extent.height = frame->height;
    rb.clearValueCount = 1;
    rb.pClearValues = &black;
    vkCmdBeginRenderPass(frame->cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
}

void vk_swapchain_end_render_pass(VkSwapFrame *frame)
{
    vkCmdEndRenderPass(frame->cmd);
    frame->layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
}

/* ---- capture ----------------------------------------------------------------- */
/* Presenting thread, after the frame's last write. Copies the image into buf
 * and leaves it in PRESENT_SRC. */
static void vks_record_capture(VkSwapFrame *frame, const VkCoreBuffer *buf)
{
    vk_swapchain_frame_layout(frame, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy region;
    memset(&region, 0, sizeof(region));
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = frame->width;
    region.imageExtent.height = frame->height;
    region.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(frame->cmd, frame->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf->buffer, 1,
                           &region);
    vk_swapchain_frame_layout(frame, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    VkBufferMemoryBarrier bb = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.buffer = buf->buffer;
    bb.offset = 0;
    bb.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(frame->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1,
                         &bb, 0, NULL);
}

/* Presenting thread, after the submit that copied into buf returned
 * submit_r. Waits for that submit (the fence is not reset), converts the
 * pixels to top-down RGB8, frees buf and publishes the result. Always frees
 * buf, and always leaves s_cap_state at 2 (ready) or 0 (failed). */
static void vks_cap_finish(const VkSwapFrame *frame, VkCoreBuffer *buf, VkResult submit_r)
{
    unsigned char *rgb = NULL;
    int w = (int)frame->width, h = (int)frame->height;
    int ok = 0;
    if (submit_r == VK_SUCCESS) {
        VkResult wr = vkWaitForFences(vk_core_device(), 1, &s_frames[frame->slot].fence, VK_TRUE, 1000000000ull);
        if (wr != VK_SUCCESS) {
            vk_core_device_wait_idle();  /* the copy may still be using buf */
        } else {
            vk_core_buffer_invalidate(buf, 0, VK_WHOLE_SIZE);
            size_t n = (size_t)w * (size_t)h;
            rgb = (unsigned char *)malloc(n * 3);
            if (rgb) {
                const unsigned char *src = (const unsigned char *)buf->mapped;
                int bgr = frame->format == VK_FORMAT_B8G8R8A8_UNORM;
                for (size_t i = 0; i < n; ++i) {
                    rgb[i * 3 + 0] = src[i * 4 + (bgr ? 2 : 0)];
                    rgb[i * 3 + 1] = src[i * 4 + 1];
                    rgb[i * 3 + 2] = src[i * 4 + (bgr ? 0 : 2)];
                }
                ok = 1;
            }
        }
    }
    vk_core_buffer_destroy(buf);
    if (!ok) {
        free(rgb);
        atomic_store(&s_cap_state, 0);
        return;
    }
    free(s_cap_rgb);
    s_cap_rgb = rgb;
    s_cap_w = w;
    s_cap_h = h;
    atomic_store(&s_cap_state, 2);
}

int vk_swapchain_capture_presented(int timeout_ms, unsigned char **out_rgb, int *out_w, int *out_h)
{
    if (!out_rgb || !out_w || !out_h) return 0;
    *out_rgb = NULL;
    *out_w = 0;
    *out_h = 0;
    if (!s_active || !atomic_load(&s_can_read)) return 0;
    int expect = 0;
    if (!atomic_compare_exchange_strong(&s_cap_state, &expect, 1)) return 0;

    uint64_t deadline = vks_now_ns() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0) * 1000000ull;
    int st = atomic_load(&s_cap_state);
    while (st == 1 && vks_now_ns() < deadline) {
        SDL_Delay(2);
        st = atomic_load(&s_cap_state);
    }
    if (st == 1) {
        expect = 1;
        if (atomic_compare_exchange_strong(&s_cap_state, &expect, 0)) return 0;
        st = expect;  /* the presenter moved on before the timeout was taken */
    }
    if (st == 3) {
        deadline = vks_now_ns() + 1000ull * 1000000ull;
        while ((st = atomic_load(&s_cap_state)) == 3 && vks_now_ns() < deadline)
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

/* ---- present ------------------------------------------------------------------ */
int vk_swapchain_present(VkSwapFrame *frame, uint32_t extra_wait_count, const VkSemaphore *extra_waits,
                         const VkPipelineStageFlags *extra_wait_stages, uint32_t extra_signal_count,
                         const VkSemaphore *extra_signals)
{
    if (extra_wait_count > VKS_MAX_EXTRA || extra_signal_count > VKS_MAX_EXTRA) {
        vks_fail(VK_ERROR_INITIALIZATION_FAILED);
        return 0;
    }
    uint64_t t0 = vks_now_ns();
    frame->submitted = 0;

    /* A requested capture reads the finished image. */
    VkCoreBuffer cap_buf;
    memset(&cap_buf, 0, sizeof(cap_buf));
    int capturing = 0;
    int want = 1;
    if (atomic_load(&s_can_read) && atomic_compare_exchange_strong(&s_cap_state, &want, 3)) {
        if (vk_core_buffer_create(&cap_buf, (VkDeviceSize)frame->width * frame->height * 4,
                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_CORE_MEMORY_READBACK, "capture"))
            capturing = 1;
        else
            atomic_store(&s_cap_state, 0);
    }
    if (capturing) vks_record_capture(frame, &cap_buf);
    else vk_swapchain_frame_layout(frame, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    VkResult r = vkEndCommandBuffer(frame->cmd);
    if (!vk_core_check(r, "frame command buffer end")) {
        vks_fail(r);
        if (capturing) vks_cap_finish(frame, &cap_buf, r);
        return 0;
    }

    VkSemaphore waits[1 + VKS_MAX_EXTRA], signals[1 + VKS_MAX_EXTRA];
    VkPipelineStageFlags stages[1 + VKS_MAX_EXTRA];
    uint32_t nw = 0, ns = 0;
    waits[nw] = s_cur_acquire;
    stages[nw++] = VKS_ACQUIRE_STAGES;
    for (uint32_t i = 0; i < extra_wait_count; ++i) {
        waits[nw] = extra_waits[i];
        stages[nw++] = extra_wait_stages[i];
    }
    signals[ns++] = s_present_sem[frame->image_index];
    for (uint32_t i = 0; i < extra_signal_count; ++i) signals[ns++] = extra_signals[i];
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = nw;
    si.pWaitSemaphores = waits;
    si.pWaitDstStageMask = stages;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &frame->cmd;
    si.signalSemaphoreCount = ns;
    si.pSignalSemaphores = signals;
    uint64_t wait_started = s_wait_diagnostics ? vks_now_ns() : 0u;
    r = vk_core_submit(1, &si, s_frames[frame->slot].fence);
    if (s_wait_diagnostics) VKS_ADD(s_d_submit_ns, vks_now_ns() - wait_started);
    if (capturing) vks_cap_finish(frame, &cap_buf, r);
    if (r != VK_SUCCESS) {
        vks_fail(r);
        return 0;
    }
    frame->submitted = 1;

    VkSwapchainKHR sc = s_swapchain;
    uint32_t ii = frame->image_index;
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &s_present_sem[ii];
    pi.swapchainCount = 1;
    pi.pSwapchains = &sc;
    pi.pImageIndices = &ii;
    VkPresentIdKHR pid = { VK_STRUCTURE_TYPE_PRESENT_ID_KHR };
    uint64_t pres_id = 0;
    if (s_use_present_id) {
        pres_id = ++s_present_id;
        pid.swapchainCount = 1;
        pid.pPresentIds = &pres_id;
        pi.pNext = &pid;
    }
    wait_started = s_wait_diagnostics ? vks_now_ns() : 0u;
    r = vk_core_present(&pi);
    if (s_wait_diagnostics) VKS_ADD(s_d_present_ns, vks_now_ns() - wait_started);
    int queued = 0;
    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
        if (pres_id) {
            if (!s_sc_first_id) s_sc_first_id = pres_id;
            s_last_presented_id = pres_id;
            VKS_SET(s_d_last_present_id, pres_id);
        }
        queued = 1;
        s_images_since_create++;
        VKS_ADD(s_d_presented, 1);
        if (r == VK_SUBOPTIMAL_KHR) {
            VKS_ADD(s_d_suboptimal, 1);
            if (s_images_since_create >= 60) s_need_recreate = 1;
        }
    } else if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        VKS_ADD(s_d_out_of_date, 1);
        s_need_recreate = 1;
    } else {
        vks_fail(r);
        s_need_recreate = 1;
    }
    uint64_t t1 = vks_now_ns();
    if (t1 - t0 > VKS_GET(s_d_max_block_ns)) VKS_SET(s_d_max_block_ns, t1 - t0);
    VKS_SET(s_d_last_present_ns, t1);
    return queued;
}

/* ---- diagnostics ---------------------------------------------------------------- */
void vk_swapchain_diagnostics(VkSwapDiagnostics *out)
{
    memset(out, 0, sizeof(*out));
    out->active = s_active;
    out->present_mode = VKS_GET(s_d_present_mode);
    out->requested_interval = atomic_load(&s_req_interval);
    out->requested_vsync = atomic_load(&s_req_vsync);
    out->images = VKS_GET(s_d_images);
    out->extent_width = VKS_GET(s_d_ext_w);
    out->extent_height = VKS_GET(s_d_ext_h);
    out->frames_acquired = VKS_GET(s_d_acquired);
    out->frames_presented = VKS_GET(s_d_presented);
    out->acquire_timeouts = VKS_GET(s_d_acquire_timeouts);
    out->out_of_date = VKS_GET(s_d_out_of_date);
    out->suboptimal = VKS_GET(s_d_suboptimal);
    out->recreations = VKS_GET(s_d_recreations);
    out->minimized_skips = VKS_GET(s_d_minimized_skips);
    out->failures = VKS_GET(s_d_failures);
    out->last_present_ns = VKS_GET(s_d_last_present_ns);
    out->max_present_block_ns = VKS_GET(s_d_max_block_ns);
    out->last_vk_result = VKS_GET(s_d_last_result);
    out->present_wait = VKS_GET(s_d_present_wait_on);
    out->present_depth = VKS_GET(s_d_present_depth);
    out->present_waits = VKS_GET(s_d_present_waits);
    out->present_wait_timeouts = VKS_GET(s_d_present_wait_timeouts);
    out->present_wait_ns_total = VKS_GET(s_d_present_wait_ns_total);
    out->present_wait_max_ns = VKS_GET(s_d_present_wait_max_ns);
    out->last_present_id = VKS_GET(s_d_last_present_id);
}

int vk_swapchain_diagnostics_json(char *out, int capacity)
{
    VkSwapDiagnostics d;
    vk_swapchain_diagnostics(&d);
    /* s_failure is written only while the swapchain is created, on the main
     * thread, before it is active; it holds plain ASCII. */
    return snprintf(out, (size_t)capacity,
        "{\"active\":%d,\"present_mode\":%d,\"requested_interval\":%d,\"requested_vsync\":%d,"
        "\"images\":%u,\"extent_width\":%u,\"extent_height\":%u,"
        "\"frames_acquired\":%llu,\"frames_presented\":%llu,"
        "\"acquire_timeouts\":%llu,\"out_of_date\":%llu,\"suboptimal\":%llu,"
        "\"recreations\":%llu,\"minimized_skips\":%llu,\"failures\":%llu,"
        "\"last_present_ns\":%llu,\"max_present_block_ns\":%llu,\"last_vk_result\":%d,"
        "\"present_wait\":%d,\"present_depth\":%d,\"present_waits\":%llu,"
        "\"present_wait_timeouts\":%llu,\"present_wait_ns_total\":%llu,"
        "\"present_wait_max_ns\":%llu,\"last_present_id\":%llu,\"create_failure\":\"%s\","
        "\"frame_fence_ns\":%llu,\"acquire_ns\":%llu,\"submit_ns\":%llu,\"present_ns\":%llu}",
        d.active, d.present_mode, d.requested_interval, d.requested_vsync,
        (unsigned)d.images, (unsigned)d.extent_width, (unsigned)d.extent_height,
        (unsigned long long)d.frames_acquired, (unsigned long long)d.frames_presented,
        (unsigned long long)d.acquire_timeouts, (unsigned long long)d.out_of_date,
        (unsigned long long)d.suboptimal, (unsigned long long)d.recreations,
        (unsigned long long)d.minimized_skips, (unsigned long long)d.failures,
        (unsigned long long)d.last_present_ns, (unsigned long long)d.max_present_block_ns,
        d.last_vk_result, d.present_wait, d.present_depth, (unsigned long long)d.present_waits,
        (unsigned long long)d.present_wait_timeouts, (unsigned long long)d.present_wait_ns_total,
        (unsigned long long)d.present_wait_max_ns, (unsigned long long)d.last_present_id, s_failure,
        (unsigned long long)VKS_GET(s_d_frame_fence_ns), (unsigned long long)VKS_GET(s_d_acquire_ns),
        (unsigned long long)VKS_GET(s_d_submit_ns), (unsigned long long)VKS_GET(s_d_present_ns));
}

#endif  /* PSX_HAVE_VULKAN */
