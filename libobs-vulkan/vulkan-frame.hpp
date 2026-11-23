#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <array>
#include <functional>
#include <string>
#include <vector>

struct vk_queue {
	VkQueue q = VK_NULL_HANDLE;
	uint32_t family = 0;
	VkSemaphore timeline = VK_NULL_HANDLE;
	uint64_t next_value = 1;
	std::vector<uint64_t> failed_values; // timeline values whose submission failed (signalled from the host)
	float priority = 0.5f;               // granted: 0.25 low, 0.5 medium (default), 0.75 high, 1.0 realtime
	// VK_KHR_global_priority level asked for, and the one vkCreateDevice accepted (0: none, the driver default).
	VkQueueGlobalPriorityKHR requested = (VkQueueGlobalPriorityKHR)0;
	VkQueueGlobalPriorityKHR global_priority = (VkQueueGlobalPriorityKHR)0;
	// The globalPriority actually chained to this queue's VkDeviceQueueCreateInfo (0: none was chained).
	VkQueueGlobalPriorityKHR create_info_priority = (VkQueueGlobalPriorityKHR)0;
};

// Extra synchronisation for one submission (interop: imported or exported timelines, keyed mutexes).
struct vk_submit_extra {
	std::vector<VkSemaphoreSubmitInfo> waits, signals;
	const void *pnext = nullptr; // chained to VkSubmitInfo2 (VkWin32KeyedMutexAcquireReleaseInfoKHR)
};

// One host-visible, persistently mapped block of a frame's upload arena.
struct vk_arena_chunk {
	VkBuffer buffer = VK_NULL_HANDLE;
	VmaAllocation alloc = VK_NULL_HANDLE;
	uint8_t *mapped = nullptr;
	VkDeviceSize size = 0, used = 0;
};

struct vk_frame {
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkSemaphore done_sem = VK_NULL_HANDLE; // timeline the last submit signals
	uint64_t done_value = 0;               // 0 when not in flight
	std::vector<std::function<void()>> deferred;
	// Uniform, dynamic vertex/index and gs_texture_map data, and descriptor sets, recorded into this frame; both are
	// reset (not freed) when the slot is reused, i.e. once the GPU has finished with them.
	std::vector<vk_arena_chunk> arena;
	size_t arena_chunk = 0; // chunk new allocations come from
	std::vector<VkDescriptorPool> desc_pools;
	size_t desc_pool = 0; // desc_pools entry new sets come from
	uint32_t maps = 0; // gs_texture_map space in the arena not yet unmapped: begin() keeps the arena while nonzero

	// Runs fn once the GPU has finished this frame's submission.
	void defer_destroy(std::function<void()> fn) { deferred.push_back(std::move(fn)); }
};

struct vk_frame_ring {
	static constexpr uint32_t count = 3;

	VkDevice dev = VK_NULL_HANDLE;
	VmaAllocator vma = VK_NULL_HANDLE; // frees the arenas in destroy()
	uint32_t family = 0;
	vk_frame frames[count];
	uint32_t index = 0;
	bool recording = false; // frames[index] is between begin() and submit()
	// Test hook OBS_VULKAN_TEST_FAIL_SUBMIT=1 (read by vk_device_init): every submit fails. Tests may toggle it.
	bool test_fail_submit = false;
	// Test hook OBS_VULKAN_TEST_DEVICE_LOST=N (read by vk_device_init): the N-th submit from now reports
	// VK_ERROR_DEVICE_LOST without reaching the driver, and every later one does too until the device is rebuilt
	// (test_lost), as on a lost device. 0: off. Tests may set it.
	uint32_t test_lose_after = 0;
	bool test_lost = false;
	// A VK_ERROR_DEVICE_LOST was seen (vk_check, submit, begin); the next safe point rebuilds (vk_handle_loss).
	bool lost = false;

	// Open debug labels (device_debug_marker_begin), innermost last. A label region must begin and end in one
	// command buffer, so submit() ends the open ones before vkEndCommandBuffer and begin() opens them again.
	struct label {
		std::string name;
		std::array<float, 4> color;
	};
	std::vector<label> labels;
	PFN_vkCmdBeginDebugUtilsLabelEXT begin_label = nullptr; // null without VK_EXT_debug_utils
	PFN_vkCmdEndDebugUtilsLabelEXT end_label = nullptr;
	void record_label(VkCommandBuffer cmd, const label &l) const;

	bool init(VkDevice dev, uint32_t family);
	void destroy();

	// Waits only if the slot's previous submission is still in flight; returns it with `cmd` recording.
	vk_frame &begin();
	// Ends and submits the current frame on q (same family as init). wait/signal are optional binary semaphores,
	// extra adds timeline waits and signals and a pNext chain. On failure the queue is drained, the timeline value
	// (and extra's timeline signals) are signalled from the host so no waiter hangs, the value is added to
	// q.failed_values, then the frame's deferred work runs. `result` (optional) receives vkQueueSubmit2's result;
	// a success code other than VK_SUCCESS (VK_TIMEOUT of a keyed mutex acquire) counts as submitted.
	bool submit(vk_queue &q, VkSemaphore *wait, VkSemaphore *signal, const vk_submit_extra *extra = nullptr,
		    VkResult *result = nullptr);
	// Runs fn once every submission so far, and the frame being recorded, has finished on the GPU.
	void defer_destroy(std::function<void()> fn);
};
