// GPU timers (gs_timer_t) and timer ranges (gs_timer_range_t) for the source profiler (util/source-profiler.c).
//
// A timer is a pair of timestamp queries written into the frame being recorded (vkCmdWriteTimestamp2 is allowed
// inside a rendering instance, so draws are not split). Results are returned in nanoseconds with a frequency of
// 1 GHz: ticks times timestampPeriod, so a non-integer period (AMD) loses nothing. The source profiler creates and
// destroys one timer per source per frame, so queries come from shared pools and are recycled once the GPU is done.
//
// Timer ranges carry no queries: Vulkan timestamps have no disjoint state (D3D11's TIMESTAMP_DISJOINT covers clock
// changes, which Vulkan's timestampPeriod rules out). A range is disjoint only when the graphics queue has no
// timestamps at all (timestampValidBits 0), in which case every timer reports nothing.
#include "vulkan-subsystem.hpp"

#include <util/base.h>

#include <algorithm>

static constexpr uint64_t ns_frequency = 1000000000ull;

void vk_timers_init(gs_device *d, uint32_t valid_bits, bool host_reset)
{
	vk_timer_pools &p = d->timers;
	p.valid_bits = valid_bits;
	p.period = d->limits.timestampPeriod > 0.0f ? d->limits.timestampPeriod : 1.0;
	p.host_reset = host_reset;
	if (!valid_bits) {
		blog(LOG_WARNING, "[libobs-vulkan] graphics queue has no timestamps: GPU timers report nothing");
	}
}

void vk_timers_free(gs_device *d)
{
	vk_timer_pools &p = d->timers;
	for (VkQueryPool pool : p.pools) {
		vkDestroyQueryPool(d->dev, pool, nullptr);
	}
	p.pools.clear();
	p.free.clear();
	for (gs_timer *t : d->objects.timers) {
		t->query = UINT32_MAX;
		t->value = 0;
		t->begun = false;
	}
}

static VkQueryPool pool_of(gs_device *d, uint32_t query, uint32_t &slot)
{
	slot = query % vk_timer_pools::pool_size;
	return d->timers.pools[query / vk_timer_pools::pool_size];
}

// A reset pair of queries, or UINT32_MAX (no timestamps, or no memory).
static uint32_t take_queries(gs_device *d)
{
	vk_timer_pools &p = d->timers;
	if (!p.valid_bits) {
		return UINT32_MAX;
	}
	if (p.free.empty()) {
		VkQueryPoolCreateInfo ci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
		ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
		ci.queryCount = vk_timer_pools::pool_size;
		VkQueryPool pool = VK_NULL_HANDLE;
		if (vkCreateQueryPool(d->dev, &ci, nullptr, &pool) != VK_SUCCESS) {
			blog(LOG_ERROR, "[libobs-vulkan] device_timer_create: vkCreateQueryPool failed");
			return UINT32_MAX;
		}
		const uint32_t base = (uint32_t)p.pools.size() * vk_timer_pools::pool_size;
		p.pools.push_back(pool);
		for (uint32_t i = vk_timer_pools::pool_size; i >= 2; i -= 2) {
			p.free.push_back(base + i - 2); // popped from the back: lowest first
		}
	}
	const uint32_t q = p.free.back();
	p.free.pop_back();
	// Every query must be reset before it is written. The pair is unused by the GPU here: fresh, or returned to the
	// free list only after the frame that last wrote it finished (gs_timer_destroy).
	uint32_t slot;
	VkQueryPool pool = pool_of(d, q, slot);
	if (p.host_reset) {
		vkResetQueryPool(d->dev, pool, slot, 2);
	} else {
		vkCmdResetQueryPool(vk_cmd(d), pool, slot,
				    2); // outside rendering, ahead of the writes in the same queue
	}
	return q;
}

void vk_timer_recreate(gs_device *d, gs_timer *t)
{
	t->query = take_queries(d);
	t->value = 0;
	t->begun = false;
}

static void write_timestamp(gs_device *d, gs_timer *t, uint32_t index, VkPipelineStageFlags2 stage)
{
	uint32_t slot;
	VkQueryPool pool = pool_of(d, t->query, slot);
	vkCmdWriteTimestamp2(vk_frame_cmd(d), stage, pool, slot + index);
}

extern "C" {

EXPORT gs_timer_t *device_timer_create(gs_device_t *device)
{
	auto t = new gs_timer{device};
	t->query = take_queries(device);
	device->objects.timers.insert(t);
	return t;
}

EXPORT gs_timer_range_t *device_timer_range_create(gs_device_t *device)
{
	return new gs_timer_range{device};
}

EXPORT void gs_timer_destroy(gs_timer_t *timer)
{
	if (!timer) {
		return;
	}
	gs_device *d = timer->device;
	d->objects.timers.erase(timer);
	if (timer->query != UINT32_MAX) {
		// Reusable once every submission so far (which may write it) has finished.
		d->frames.defer_destroy([d, q = timer->query] { d->timers.free.push_back(q); });
	}
	delete timer;
}

// One timestamp before and one after the work recorded in between, as D3D11's pair of TIMESTAMP queries: each is
// written once all earlier work has finished (ALL_COMMANDS, as a D3D11 timestamp), so a timer measures only its own
// work. A timer
// is begun and ended once (the source profiler creates one per render); beginning it again restarts it.
EXPORT void gs_timer_begin(gs_timer_t *timer)
{
	if (!timer || timer->query == UINT32_MAX) {
		return;
	}
	gs_device *d = timer->device;
	if (timer->value || timer->begun) {
		// Reused: its queries need a reset first, which only a fresh pair guarantees here.
		d->frames.defer_destroy([d, q = timer->query] { d->timers.free.push_back(q); });
		timer->query = take_queries(d);
		timer->value = 0;
		if (timer->query == UINT32_MAX) {
			return;
		}
	}
	write_timestamp(d, timer, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
	timer->begun = true;
}

EXPORT void gs_timer_end(gs_timer_t *timer)
{
	if (!timer || timer->query == UINT32_MAX || !timer->begun) {
		return;
	}
	gs_device *d = timer->device;
	write_timestamp(d, timer, 1, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
	timer->begun = false;
	timer->value = d->graphics.next_value; // the value the frame being recorded will signal
}

// Nanoseconds between begin and end (gs_timer_range_get_data reports 1 GHz). False until the submission holding
// the end has been made, when it failed, or when the device has no timestamps. Once submitted it waits for that
// submission: the source profiler reads timers frames later, so this normally returns at once.
EXPORT bool gs_timer_get_data(gs_timer_t *timer, uint64_t *ticks)
{
	if (!timer || timer->query == UINT32_MAX || !timer->value) {
		return false;
	}
	gs_device *d = timer->device;
	const uint64_t value = timer->value;
	const auto &failed = d->graphics.failed_values;
	if (vk_lost(d) || value >= d->graphics.next_value ||
	    std::find(failed.begin(), failed.end(), value) != failed.end()) {
		return false;
	}
	VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
	wi.semaphoreCount = 1;
	wi.pSemaphores = &d->graphics.timeline;
	wi.pValues = &value;
	if (vk_check(d, vkWaitSemaphores(d->dev, &wi, UINT64_MAX)) != VK_SUCCESS) {
		return false;
	}
	uint32_t slot;
	VkQueryPool pool = pool_of(d, timer->query, slot);
	uint64_t ts[2] = {};
	if (vk_check(d, vkGetQueryPoolResults(d->dev, pool, slot, 2, sizeof(ts), ts, sizeof(uint64_t),
					      VK_QUERY_RESULT_64_BIT)) != VK_SUCCESS) {
		return false;
	}
	const uint32_t bits = d->timers.valid_bits;
	const uint64_t mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
	const uint64_t delta = (ts[1] - ts[0]) & mask; // wraps correctly within the valid bits
	*ticks = (uint64_t)((double)delta * d->timers.period + 0.5);
	return true;
}

EXPORT void gs_timer_range_destroy(gs_timer_range_t *range)
{
	delete range;
}

EXPORT void gs_timer_range_begin(gs_timer_range_t *range)
{
	(void)range;
}

EXPORT void gs_timer_range_end(gs_timer_range_t *range)
{
	(void)range;
}

// Always fills both, so a caller dividing by the frequency never divides by zero.
EXPORT bool gs_timer_range_get_data(gs_timer_range_t *range, bool *disjoint, uint64_t *frequency)
{
	if (disjoint) {
		*disjoint = !range || !range->device->timers.valid_bits;
	}
	if (frequency) {
		*frequency = ns_frequency;
	}
	return range != nullptr;
}
}
