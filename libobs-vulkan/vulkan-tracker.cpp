#include "vulkan-tracker.hpp"

// shortcut: omits acceleration-structure and video write bits, upgrade when those are used.
static constexpr VkAccessFlags2 write_bits =
	VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
	VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_HOST_WRITE_BIT |
	VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

static bool is_write(const vk_access &a)
{
	return (a.access & write_bits) != 0;
}

template<class T> static uint64_t key(T handle)
{
	return (uint64_t)handle;
}

bool vk_tracker::step(state &s, const vk_access &needed, bool check_layout, source &src)
{
	bool same_layout = !check_layout || s.layout == needed.layout;
	bool write = is_write(needed);
	// Coalesce a read when there is no pending write, or an earlier barrier already made it visible to this use.
	bool covered = s.write_stage == 0 ||
		       ((needed.stage & ~s.visible_stage) == 0 && (needed.access & ~s.visible_access) == 0);
	if (!write && same_layout && covered) {
		s.read_stage |= needed.stage;
		return false;
	}

	// A write waits for the previous write and for every reader since (execution dependency, no access for reads).
	src = {s.write_stage | s.read_stage, s.write_access, s.layout};
	if (write) {
		s.write_stage = needed.stage;
		s.write_access = needed.access & write_bits;
		// The write's own result is visible to nothing yet, not even a read in its own stage.
		s.visible_stage = 0;
		s.visible_access = 0;
		s.read_stage = 0;
	} else {
		// The write stays pending for further readers in other stages.
		if (!same_layout) {
			s.visible_stage = 0;
			s.visible_access = 0;
		}
		s.visible_stage |= needed.stage;
		s.visible_access |= needed.access;
		s.read_stage |= needed.stage;
	}
	s.layout = needed.layout;
	return true;
}

void vk_tracker::acquire(state &s, const vk_access &needed, source &src)
{
	// The other side's writes become visible to everything in this submission (the acquire's destination scope is
	// all commands), so afterwards no write is pending unless this use writes.
	src = {VK_PIPELINE_STAGE_2_NONE, 0, s.layout};
	const bool write = is_write(needed);
	s.owned = true;
	s.layout = needed.layout;
	s.write_stage = write ? needed.stage : 0;
	s.write_access = write ? needed.access & write_bits : 0;
	s.visible_stage = 0;
	s.visible_access = 0;
	s.read_stage = write ? 0 : needed.stage;
}

void vk_tracker::use_image(VkImage image, uint32_t family, const vk_access &needed, vk_barriers &out)
{
	source src;
	state &s = images_[key(image)];
	if (s.external && !s.owned) {
		acquire(s, needed, src);
		acquired_images_.push_back(key(image));
		VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
		b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
		b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
		b.oldLayout = src.old_layout;
		b.newLayout = needed.layout;
		b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
		b.dstQueueFamilyIndex = family;
		b.image = image;
		b.subresourceRange = {s.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
		out.images.push_back(b);
		return;
	}
	if (!step(s, needed, true, src)) {
		return;
	}

	VkImageMemoryBarrier2 b = {};
	b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
	b.srcStageMask = src.stage ? src.stage : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
	b.srcAccessMask = src.access;
	b.dstStageMask = needed.stage;
	b.dstAccessMask = needed.access;
	b.oldLayout = src.old_layout;
	b.newLayout = needed.layout;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = image;
	b.subresourceRange = {needed.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
	out.images.push_back(b);
}

void vk_tracker::use_buffer(VkBuffer buffer, uint32_t family, const vk_access &needed, vk_barriers &out)
{
	bool first = buffers_.find(key(buffer)) == buffers_.end();
	source src;
	if (!first) {
		state &s = buffers_[key(buffer)];
		if (s.external && !s.owned) {
			acquire(s, needed, src);
			acquired_buffers_.push_back(key(buffer));
			VkBufferMemoryBarrier2 b = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
			b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
			b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
			b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
			b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
			b.dstQueueFamilyIndex = family;
			b.buffer = buffer;
			b.size = VK_WHOLE_SIZE;
			out.buffers.push_back(b);
			return;
		}
	}
	bool barrier = step(buffers_[key(buffer)], needed, false, src);
	if (first || !barrier) // first use is not a hazard
		return;

	VkBufferMemoryBarrier2 b = {};
	b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
	b.srcStageMask = src.stage;
	b.srcAccessMask = src.access;
	b.dstStageMask = needed.stage;
	b.dstAccessMask = needed.access;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.buffer = buffer;
	b.offset = 0;
	b.size = VK_WHOLE_SIZE;
	out.buffers.push_back(b);
}

static void erase_key(std::vector<uint64_t> &v, uint64_t k)
{
	for (size_t i = 0; i < v.size(); ++i) {
		if (v[i] == k) {
			v.erase(v.begin() + (ptrdiff_t)i);
			return;
		}
	}
}

void vk_tracker::forget(VkImage image)
{
	images_.erase(key(image));
	erase_key(acquired_images_, key(image));
}

void vk_tracker::forget(VkBuffer buffer)
{
	buffers_.erase(key(buffer));
	erase_key(acquired_buffers_, key(buffer));
}

void vk_tracker::set_external(VkImage image, VkImageAspectFlags aspect, VkImageLayout layout)
{
	state s;
	s.external = true;
	s.aspect = aspect;
	s.layout = layout;
	images_[key(image)] = s;
}

void vk_tracker::set_external(VkBuffer buffer)
{
	state s;
	s.external = true;
	buffers_[key(buffer)] = s;
}

void vk_tracker::release_external(uint32_t family, vk_barriers &out)
{
	for (uint64_t k : acquired_images_) {
		state &s = images_[k];
		VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
		b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		b.srcAccessMask = s.write_access;
		b.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
		b.oldLayout = s.layout;
		b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		b.srcQueueFamilyIndex = family;
		b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
		b.image = (VkImage)k;
		b.subresourceRange = {s.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
		out.images.push_back(b);
		const VkImageAspectFlags aspect = s.aspect;
		s = state{};
		s.external = true;
		s.aspect = aspect;
		s.layout = VK_IMAGE_LAYOUT_GENERAL;
	}
	for (uint64_t k : acquired_buffers_) {
		state &s = buffers_[k];
		VkBufferMemoryBarrier2 b = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
		b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		b.srcAccessMask = s.write_access;
		b.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
		b.srcQueueFamilyIndex = family;
		b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
		b.buffer = (VkBuffer)k;
		b.size = VK_WHOLE_SIZE;
		out.buffers.push_back(b);
		s = state{};
		s.external = true;
	}
	acquired_images_.clear();
	acquired_buffers_.clear();
}

void vk_tracker::reset()
{
	auto keep = [](std::unordered_map<uint64_t, state> &m) {
		for (auto it = m.begin(); it != m.end();) {
			if (!it->second.external) {
				it = m.erase(it);
				continue;
			}
			const VkImageAspectFlags aspect = it->second.aspect;
			it->second = state{};
			it->second.external = true;
			it->second.aspect = aspect;
			it->second.layout = VK_IMAGE_LAYOUT_GENERAL;
			++it;
		}
	};
	keep(images_);
	keep(buffers_);
	acquired_images_.clear();
	acquired_buffers_.clear();
}
