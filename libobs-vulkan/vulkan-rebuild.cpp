// Device loss: detection (vk_check, and the frame ring's submit and wait results) and the rebuild, with the contract of
// libobs-d3d11's gs_device::RebuildDevice and device_register_loss_callbacks:
//
//  1. every registered device_loss_release(data) runs, in registration order;
//  2. the Vulkan objects of every live gs_* object are destroyed, then the device, allocator and instance;
//  3. a new instance and device are created on the same adapter (a failure is fatal: bcrash, as D3D11);
//  4. every object gets its Vulkan objects again, in place, so the pointers libobs and plugins hold stay valid:
//     textures with the data they were created with (D3D11 keeps a copy the same way), static vertex and index
//     buffers from their CPU data, samplers, shaders (pipelines are compiled again on first use), depth buffers,
//     stage surfaces, swapchains (new surfaces), timers, exportable textures and frame buffers (new memory: export
//     again), timelines (at their last value). Render targets and depth buffers come back with undefined contents.
//     Imported textures cannot be imported again (an import borrows the handle) and become placeholders of the same
//     size and format, except legacy KMT handles, which are opened again as D3D11 does;
//  5. every device_loss_rebuild(device, data) runs, with `device` the struct gs_vulkan_device of the new device
//     (graphics/graphics-vulkan.h) where D3D11 passes its ID3D11Device.
//
// Device-level state libobs set (render target, shaders, buffers, textures and samplers loaded, blend/depth state,
// viewport, projection, open debug markers) is kept, unlike D3D11, which clears it: every pointer stays valid.
//
// Detection: any VK_ERROR_DEVICE_LOST marks the device lost; the rebuild runs at the next safe point, where nothing of
// a frame is recorded: device_begin_frame, device_is_present_ready and the end of device_present. Until then
// submissions fail (as any failed submission: the timeline is signalled from the host, no waiter hangs) and readbacks
// return false.
#include "vulkan-subsystem.hpp"

#include <util/base.h>

#include <vector>

VkResult vk_check(gs_device *d, VkResult r)
{
	if (r == VK_ERROR_DEVICE_LOST && !d->frames.lost) {
		blog(LOG_ERROR, "[libobs-vulkan] VK_ERROR_DEVICE_LOST: the device will be rebuilt");
		d->frames.lost = true;
	}
	return r;
}

template<typename T> static std::vector<T *> snapshot(const std::unordered_set<T *> &set)
{
	return std::vector<T *>(set.begin(), set.end());
}

static void release_all(gs_device *d)
{
	for (gs_texture *t : d->objects.textures) {
		vk_texture_release(d, t);
	}
	for (gs_zstencil_buffer *zs : d->objects.zstencils) {
		vk_zstencil_release(d, zs);
	}
	for (gs_stage_surface *s : d->objects.stagesurfs) {
		vk_stagesurf_release(d, s);
	}
	vk_buffers_release(d);
	for (gs_shader *s : d->objects.shaders) {
		// Pipelines and layouts are device-level (vk_pipelines_free); the shader's own objects go here.
		vkDestroyDescriptorSetLayout(d->dev, s->set_layout, nullptr);
		for (VkSampler smp : s->vk_samplers) {
			vkDestroySampler(d->dev, smp, nullptr);
		}
		s->set_layout = VK_NULL_HANDLE;
		s->vk_samplers.clear();
		s->ubo = VK_NULL_HANDLE;
		s->ubo_value = 0;
		s->dirty = true;
	}
	for (gs_swap_chain *s : d->objects.swapchains) {
		vk_swapchain_release(s);
	}
	for (gs_shared_buffer *b : d->objects.sharedbufs) {
		vk_sharedbuf_release(d, b);
	}
	for (gs_timeline *t : d->objects.timelines) {
		vk_timeline_release(d, t);
	}
	// Timers keep nothing of their own: their queries are in the device's pools (vk_timers_free).
}

// Objects are created again in dependency order: nothing depends on another object's Vulkan handles except plane
// views (handled by vk_texture_adopt) and swapchains (none). Recreation records uploads and initial releases into
// frames of the new device.
static bool recreate_all(gs_device *d)
{
	bool ok = vk_buffers_recreate(d);
	for (gs_texture *t : snapshot(d->objects.textures)) {
		if (!d->objects.textures.count(t)) {
			continue;
		}
		const bool external = t->origin == gs_texture::ORIGIN_EXPORTABLE ||
				      t->origin == gs_texture::ORIGIN_IMPORTED;
		if (!(external ? vk_interop_recreate_texture(d, t) : vk_texture_recreate(d, t))) {
			blog(LOG_ERROR, "[libobs-vulkan] device rebuild: a %ux%u texture could not be created again",
			     t->width, t->height);
			ok = false;
		}
	}
	for (gs_zstencil_buffer *zs : snapshot(d->objects.zstencils)) {
		ok = vk_zstencil_recreate(d, zs) && ok;
	}
	for (gs_stage_surface *s : d->objects.stagesurfs) {
		ok = vk_stagesurf_recreate(d, s) && ok;
	}
	for (gs_swap_chain *s : d->objects.swapchains) {
		ok = vk_swapchain_recreate(s) && ok;
	}
	for (gs_timer *t : d->objects.timers) {
		vk_timer_recreate(d, t);
	}
	for (gs_shared_buffer *b : snapshot(d->objects.sharedbufs)) {
		ok = vk_sharedbuf_recreate(d, b) && ok;
	}
	for (gs_timeline *t : d->objects.timelines) {
		ok = vk_timeline_recreate(d, t) && ok;
	}
	return ok;
}

static void rebuild(gs_device *d)
{
	blog(LOG_WARNING, "[libobs-vulkan] device lost: rebuilding the device and every resource");

	// Callbacks may register or unregister others: run over a copy, as the list is at this point.
	const std::vector<gs_device_loss> callbacks = d->loss_callbacks;
	for (const gs_device_loss &cb : callbacks) {
		if (cb.device_loss_release) {
			cb.device_loss_release(cb.data);
		}
	}

	// The open rendering instance, the recorded frame and everything deferred go with the frames: frames.destroy()
	// waits for the device (a lost one returns at once) and runs deferred destruction while the device still exists.
	d->rendering_view = VK_NULL_HANDLE;
	d->rendering_depth = VK_NULL_HANDLE;
	if (d->dev) {
		vkDeviceWaitIdle(d->dev);
	}
	d->frames.destroy();
	release_all(d);
	vk_device_close(d);
	d->tracker.clear();
	d->pending = vk_submit_extra{};
	d->km_release_mem.clear();
	d->km_release_key.clear();

	const int ret = vk_device_open(d);
	d->frames.lost = false;
	d->frames.test_lost = false;
	if (ret != GS_SUCCESS) {
		bcrash("[libobs-vulkan] device rebuild: the device could not be created again (%d)", ret);
		return;
	}
	d->pipelines.device = d;

	if (!recreate_all(d)) {
		blog(LOG_ERROR, "[libobs-vulkan] device rebuild: some objects were not created again; their uses fail");
	}
	// The swapchain colour space may differ on the new device (another output, no colour space extension).
	if (d->swap && d->render_target == &d->swap->target) {
		d->space = d->swap->space;
	}
	vk_flush(d); // uploads of the restored textures
	++d->rebuilds;
	blog(LOG_WARNING, "[libobs-vulkan] device rebuilt (%u)", d->rebuilds);

	for (const gs_device_loss &cb : d->loss_callbacks) {
		if (cb.device_loss_rebuild) {
			cb.device_loss_rebuild(&d->native, cb.data);
		}
	}
}

void vk_handle_loss(gs_device *d)
{
	if (d && d->frames.lost) {
		rebuild(d);
	}
}

extern "C" {

// As D3D11: appended; device_loss_release runs in this order, then device_loss_rebuild.
EXPORT void device_register_loss_callbacks(gs_device_t *device, const struct gs_device_loss *callbacks)
{
	device->loss_callbacks.push_back(*callbacks);
}

// As D3D11: removes the first registration with this `data`.
EXPORT void device_unregister_loss_callbacks(gs_device_t *device, void *data)
{
	for (auto it = device->loss_callbacks.begin(); it != device->loss_callbacks.end(); ++it) {
		if (it->data == data) {
			device->loss_callbacks.erase(it);
			break;
		}
	}
}
}
