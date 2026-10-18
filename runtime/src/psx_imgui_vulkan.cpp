/* Shared Dear ImGui Vulkan renderer backend for Native on Vulkan (see psx_imgui_vulkan.h). */
#include "psx_imgui_vulkan.h"

#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include "native_vk_presenter.h"
#include "vk_core.h"

extern "C" bool psx_imgui_vulkan_backend_init(void)
{
    ImGui_ImplVulkan_InitInfo info = {};
    info.ApiVersion = VK_API_VERSION_1_1;
    info.Instance = vk_core_instance();
    info.PhysicalDevice = vk_core_physical_device();
    info.Device = vk_core_device();
    info.QueueFamily = vk_core_queue_family();
    vkGetDeviceQueue(info.Device, info.QueueFamily, 0, &info.Queue);
    info.DescriptorPoolSize = 16;
    info.RenderPass = native_vk_ui_render_pass();
    info.MinImageCount = 2;
    info.ImageCount = 2;
    info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    // The backend submits to the queue itself (font upload): keep every other
    // submitter (executor, presenter) out for the duration.
    vk_core_queue_lock();
    bool ok = ImGui_ImplVulkan_Init(&info);
    if (ok) ok = ImGui_ImplVulkan_CreateFontsTexture();
    vk_core_queue_unlock();
    return ok;
}

extern "C" void psx_imgui_vulkan_backend_shutdown(void)
{
    (void)vk_core_device_wait_idle();
    ImGui_ImplVulkan_Shutdown();
}
