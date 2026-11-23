#pragma once

#include <stdint.h>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

// Pure bookkeeping: records each resource's last access and emits the synchronization2 barriers the next use needs.
// Calls no Vulkan function.
struct vk_access {
	VkPipelineStageFlags2 stage;
	VkAccessFlags2 access;
	VkImageLayout layout;                                  // ignored for buffers
	VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT; // images: the barrier's aspectMask (all of the image's)
};

struct vk_barriers {
	std::vector<VkImageMemoryBarrier2> images;
	std::vector<VkBufferMemoryBarrier2> buffers;
};

class vk_tracker {
public:
	void use_image(VkImage image, uint32_t family, const vk_access &needed, vk_barriers &out);
	void use_buffer(VkBuffer buffer, uint32_t family, const vk_access &needed, vk_barriers &out);
	void forget(VkImage image);
	void forget(VkBuffer buffer);

	// External resources (imported, or exportable) are shared with another API, device or process. Between
	// submissions they belong to VK_QUEUE_FAMILY_EXTERNAL in VK_IMAGE_LAYOUT_GENERAL: the first use in a submission
	// acquires them from there (keeping the contents), release_external hands every acquired one back.
	// `layout` is the layout the image is in when registered (GENERAL for imports). `owner` is the queue family
	// of the other side: VK_QUEUE_FAMILY_EXTERNAL (another Vulkan device of the same driver), or
	// VK_QUEUE_FAMILY_FOREIGN_EXT for dmabufs, which any driver or API may use.
	void set_external(VkImage image, VkImageAspectFlags aspect, VkImageLayout layout,
			  uint32_t owner = VK_QUEUE_FAMILY_EXTERNAL);
	void set_external(VkBuffer buffer, uint32_t owner = VK_QUEUE_FAMILY_EXTERNAL);
	// Records the release of every external resource acquired since the last call; the submission must follow.
	void release_external(uint32_t family, vk_barriers &out);
	// After a failed submission: forgets all state (layouts restart at UNDEFINED) except that external resources
	// stay external and released, in GENERAL.
	void reset();
	// Device rebuild: forgets every resource, external ones too (their handles are gone).
	void clear();
	// The layout last recorded for `image` (UNDEFINED when untracked).
	VkImageLayout layout_of(VkImage image) const;

private:
	// shortcut: images are tracked whole (every aspect the access names, all mips/layers). The only queue family
	// ownership transfers are those of external resources.
	struct state {
		VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
		VkPipelineStageFlags2 write_stage = 0; // source of the last write not yet superseded
		VkAccessFlags2 write_access = 0;
		VkPipelineStageFlags2 visible_stage = 0; // scope already covered by an emitted barrier
		VkAccessFlags2 visible_access = 0;
		VkPipelineStageFlags2 read_stage = 0;      // readers since the last write, a later write waits for them
		bool external = false;                     // see set_external
		bool owned = false;                        // external: acquired in the submission being recorded
		VkImageAspectFlags aspect = 0;             // external images: aspect of their barriers
		uint32_t owner = VK_QUEUE_FAMILY_EXTERNAL; // external: the other side's queue family
	};
	struct source {
		VkPipelineStageFlags2 stage;
		VkAccessFlags2 access;
		VkImageLayout old_layout;
	};
	// Updates `s` for `needed`; returns true and the barrier source when a barrier is required.
	static bool step(state &s, const vk_access &needed, bool check_layout, source &src);
	// Acquire of an external resource that is not owned: replaces step() for that use.
	static void acquire(state &s, const vk_access &needed, source &src);

	std::unordered_map<uint64_t, state> images_;
	std::unordered_map<uint64_t, state> buffers_;
	std::vector<uint64_t> acquired_images_, acquired_buffers_;
};
