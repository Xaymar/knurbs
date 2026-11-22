#pragma once

/*
 * Native objects of the Vulkan renderer (GS_DEVICE_VULKAN), as returned by gs_get_device_obj() and
 * gs_texture_get_obj().
 *
 * IN-PROCESS ONLY. These are Vulkan handles of the renderer's own VkInstance and VkDevice, valid only inside the
 * process that created them, and only while the graphics context is entered (gs_enter_context). Code that may run
 * in another process (a plugin host, a helper process) must not use them: share memory and synchronisation through
 * the interop API instead (gs_texture_create_exportable, gs_texture_export / gs_texture_import, gs_timeline_*,
 * gs_sharedbuf_*; see docs/graphics-interop.md), which only passes OS handles.
 *
 * Rules for in-process users:
 *  - Nothing here is owned by the caller: never destroy, free or re-bind any of these handles.
 *  - The pointers stay valid until the object is destroyed. After a device loss (gs_register_loss_callbacks) the
 *    renderer has a new instance and device: every handle read before device_loss_release is dead, and the
 *    structs hold the new handles by the time device_loss_rebuild runs. device_loss_rebuild's `device` argument
 *    is the struct gs_vulkan_device of the new device.
 *  - Work submitted to `queue` must be externally synchronised with the renderer (only while the graphics context
 *    is entered) and is not ordered with the renderer's recorded work unless the caller calls gs_flush() first and
 *    uses its own semaphores; prefer the interop timelines.
 *  - `image_layout` is the layout the renderer last recorded for the image, valid after gs_flush() and with the
 *    context entered. A caller that changes it must transition it back before the renderer uses the texture again.
 *
 * Only the Vulkan renderer returns these. Check gs_get_device_type() == GS_DEVICE_VULKAN first.
 */

#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

/* gs_get_device_obj() */
struct gs_vulkan_device {
	uint32_t version;     /* GS_VULKAN_OBJECTS_VERSION */
	uint32_t api_version; /* VkApplicationInfo::apiVersion the instance was created with */
	VkInstance instance;
	VkPhysicalDevice physical_device;
	VkDevice device;
	/* The graphics queue every renderer submission goes to. */
	VkQueue queue;
	uint32_t queue_family_index;
	uint32_t queue_index;
	/* Loader entry point for this instance (vkGetDeviceProcAddr can be had from it). */
	PFN_vkGetInstanceProcAddr get_instance_proc_addr;
};

/* gs_texture_get_obj() */
struct gs_vulkan_texture {
	uint32_t version; /* GS_VULKAN_OBJECTS_VERSION */
	VkImage image;
	/* Sampled view (all mips; BGRX views force alpha to one), and its sRGB variant or VK_NULL_HANDLE. */
	VkImageView view;
	VkImageView srgb_view;
	/* Format of `image`. For NV12/P010 plane textures, the multi-planar format; `aspect` names the plane. */
	VkFormat format;
	VkImageAspectFlags aspect;
	/* See "Rules" above. */
	VkImageLayout image_layout;
	uint32_t width, height, depth, levels;
};

#define GS_VULKAN_OBJECTS_VERSION 1

#ifdef __cplusplus
}
#endif
