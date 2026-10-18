// Single implementation translation unit for the Vulkan Memory Allocator (v3.4.0).
// Built only when PSX_HAVE_VULKAN is defined (runtime.cmake). vk_core passes
// vkGetInstanceProcAddr / vkGetDeviceProcAddr from volk through
// VmaVulkanFunctions, so VMA never links the Vulkan loader directly.

#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include "volk.h"
#include "vk_mem_alloc.h"
