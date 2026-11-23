#pragma once

/*
 * AMF on the Vulkan renderer (docs/graphics-interop.md, "Texture encoders").
 *
 * AMF cannot create a Vulkan device of its own (InitVulkan(nullptr) fails), so the encoder creates one: on the
 * renderer's adapter (matched by device UUID), with every queue of every family and the extensions AMF asks for,
 * and hands it to InitVulkan. libobs's encoder frames are imported into that device as VkImages (zero copy) from
 * the NT handles in encoder_texture_shared, and wrapped as AMF surfaces.
 *
 * Surfaces: each imported image gets its AMFSurface once (CreateSurfaceFromVulkanNative) and that same object is
 * resubmitted for as long as the image lives. AMF's Vulkan interop caches its own import of each new surface's memory
 * and now and then reuses the import of an older surface instead, which encodes the picture of an earlier frame
 * (docs/graphics-interop.md, "Driver issues"; repro/amf-vulkan-stale-frame). With one surface per image AMF imports
 * each image once and nothing in its cache is recycled. A frame libobs hands over again while AMF still holds its
 * surface (a repeated frame) gets a second surface for the image, also kept and reused. Since this code keeps a
 * reference, AMF's OnSurfaceDataRelease does not fire; AMF is done with a surface once it holds no reference to it
 * any more (collect()).
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
class AMFContext1;
class AMFSurface;
} // namespace amf

class amf_vulkan {
public:
	struct frame;

	// Creates the instance and device. `extensions` are AMF's (AMFContext1::GetVulkanDeviceExtensions).
	// Throws const char * on failure.
	amf_vulkan(const std::vector<const char *> &extensions, bool p010);
	~amf_vulkan();

	// AMFVulkanDevice for AMFContext1::InitVulkan; valid while this object lives.
	void *amf_device();

	// The AMF surface of a shared frame's image, after libobs's ready timeline reached its value: the image is
	// imported and wrapped once per tex_id (see above), properties cleared. The frame counts as held until AMF
	// gives the surface back (collect()). Throws const char * on failure.
	amf::AMFSurface *surface(const encoder_texture_shared &shared, amf::AMFContext1 *context);
	// Every surface AMF holds no reference to any more releases its frame: the release timeline is signalled as far
	// as every earlier frame is released too. Encode thread only, like surface().
	void collect();
	// Everything held is released (the encoder stops).
	void release_all();

	uint32_t vendor_id() const;

private:
	struct impl;
	std::unique_ptr<impl> d;
};

// The vendor of the renderer's adapter and the DXGI adapter index with its LUID, or -1 (graphics context needed).
int amf_vulkan_renderer_dxgi_index(uint32_t *vendor_id);
