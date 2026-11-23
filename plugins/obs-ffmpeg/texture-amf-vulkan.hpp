#pragma once

/*
 * AMF on the Vulkan renderer (docs/graphics-interop.md, "Texture encoders").
 *
 * AMF cannot create a Vulkan device of its own (InitVulkan(nullptr) fails), so the encoder creates one: on the
 * renderer's adapter (matched by device UUID), with every queue of every family and the extensions AMF asks for,
 * and hands it to InitVulkan. libobs's encoder frames are imported into that device as VkImages (zero copy) from
 * the NT handles in encoder_texture_shared, and wrapped as AMF surfaces.
 *
 * Queues: AMF submits from its own threads to the queues of this device, so nothing here ever submits to a queue;
 * only host operations are used (vkWaitSemaphores for libobs's ready timeline, vkSignalSemaphore for the release
 * timeline). That keeps the rule that two threads never submit to the same VkQueue.
 */

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct encoder_texture_shared;

namespace amf {
struct AMFVulkanSurface;
}

class amf_vulkan {
public:
	struct frame;

	// Creates the instance and device. `extensions` are AMF's (AMFContext1::GetVulkanDeviceExtensions).
	// Throws const char * on failure.
	amf_vulkan(const std::vector<const char *> &extensions, bool p010);
	~amf_vulkan();

	// AMFVulkanDevice for AMFContext1::InitVulkan; valid while this object lives.
	void *amf_device();

	// The imported image of a shared frame (cached by tex_id), after libobs's ready timeline reached its value.
	// Throws const char * on failure.
	amf::AMFVulkanSurface *surface(const encoder_texture_shared &shared);

	// AMF holds the frame with this release value until released() is called for it (repeats are counted).
	void hold(uint64_t release_value);
	// AMF released a surface; signals the release timeline as far as every earlier frame is released too.
	void released(uint64_t release_value);
	// Everything held is released (the encoder stops).
	void release_all();

	uint32_t vendor_id() const;

private:
	struct impl;
	std::unique_ptr<impl> d;
};

// The vendor of the renderer's adapter and the DXGI adapter index with its LUID, or -1 (graphics context needed).
int amf_vulkan_renderer_dxgi_index(uint32_t *vendor_id);
