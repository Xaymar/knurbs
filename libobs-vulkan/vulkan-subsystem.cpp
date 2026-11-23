// Graphics exports: device lifetime, render target, copy, clear, staging, flush, native objects and debug markers
// here. Unsupported: cube textures (no user, see device_cubetexture_create), GDI textures; Linux dmabuf and sync are
// still stubs. Textures and stage surfaces live in vulkan-texture.cpp, swapchains, present and HDR in
// vulkan-swapchain.cpp, buffers and samplers in vulkan-buffers.cpp, state, pipelines and draw in vulkan-pipeline.cpp,
// timers in vulkan-timer.cpp, device loss in vulkan-rebuild.cpp.
#include "vulkan-subsystem.hpp"

#include <graphics/vec4.h>
#include <util/base.h>

#include <algorithm>
#include <vector>

VkCommandBuffer vk_frame_cmd(gs_device *d)
{
	if (!d->frames.recording) {
		d->frames.begin();
	}
	return d->frames.frames[d->frames.index].cmd;
}

VkCommandBuffer vk_cmd(gs_device *d)
{
	vk_end_rendering(d);
	return vk_frame_cmd(d);
}

bool vk_flush(gs_device *d, VkSemaphore *signal)
{
	if (!d->frames.recording) {
		return true;
	}
	vk_end_rendering(d);
	// External resources go back to their other owners at the end of every submission, with the caller's timeline
	// waits/signals and keyed mutex releases (vulkan-interop.cpp).
	vk_submit_extra extra;
	vk_keyed_mutex_info *km = nullptr;
	vk_interop_prepare_submit(d, d->frames.frames[d->frames.index].cmd, extra, km);
	// The submission waits on a pending acquire; see submit_pending (vulkan-swapchain.cpp) for one with no commands.
	VkSemaphore wait = d->acquire_wait;
	d->acquire_wait = VK_NULL_HANDLE;
	const bool ok = d->frames.submit(d->graphics, wait ? &wait : nullptr, signal, &extra);
	vk_interop_finish_submit(km);
	if (ok) {
		if (wait) {
			// Unsignalled again once this submission (just made the ring's latest) has run.
			d->frames.defer_destroy([d, wait] { d->spare_sems.push_back(wait); });
		}
		return true;
	}
	// The tracker recorded layouts the failed command buffer never reached; restart from UNDEFINED (contents lost),
	// external resources from GENERAL.
	d->tracker.reset();
	if (wait) {
		vk_consume_semaphore(d, wait);
	}
	return false;
}

void vk_record_barriers(VkCommandBuffer cmd, const vk_barriers &b)
{
	if (b.images.empty() && b.buffers.empty()) {
		return;
	}
	VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
	di.imageMemoryBarrierCount = (uint32_t)b.images.size();
	di.pImageMemoryBarriers = b.images.data();
	di.bufferMemoryBarrierCount = (uint32_t)b.buffers.size();
	di.pBufferMemoryBarriers = b.buffers.data();
	vkCmdPipelineBarrier2(cmd, &di);
}

extern "C" {

EXPORT const char *device_get_name(void)
{
	return "Vulkan";
}

EXPORT int device_get_type(void)
{
	return GS_DEVICE_VULKAN;
}

EXPORT const char *device_preprocessor_name(void)
{
	return "_vulkan";
}

EXPORT int device_create(gs_device_t **device, uint32_t adapter)
{
	*device = nullptr;
	auto d = new gs_device;
	int ret = vk_device_init(d, adapter); // frees everything it created on failure
	if (ret != GS_SUCCESS) {
		delete d;
		return ret;
	}
	d->pipelines.device = d; // vk_device_rebuild sets it again
	vk_glslang_ref();        // keeps glslang's built-in symbol tables alive between compiles
	*device = d;
	return GS_SUCCESS;
}

EXPORT void device_destroy(gs_device_t *device)
{
	if (!device) {
		return;
	}
	vk_device_free(device);
	delete device;
	vk_glslang_unref();
}

// Vulkan has no current-context state; commands are recorded per frame and submitted explicitly.
EXPORT void device_enter_context(gs_device_t *device) {}

EXPORT void device_leave_context(gs_device_t *device) {}

EXPORT const char *gpu_get_driver_version(void)
{
	return vk_driver_version();
}

EXPORT const char *gpu_get_renderer(void)
{
	return vk_adapter_name();
}

EXPORT bool device_enum_adapters(gs_device_t *device, bool (*callback)(void *, const char *, uint32_t), void *param)
{
	uint32_t n = 0;
	vkEnumeratePhysicalDevices(device->instance, &n, nullptr);
	std::vector<VkPhysicalDevice> phys(n);
	vkEnumeratePhysicalDevices(device->instance, &n, phys.data());
	for (uint32_t i = 0; i < n; ++i) {
		VkPhysicalDeviceProperties props;
		vkGetPhysicalDeviceProperties(phys[i], &props);
		if (!callback(param, props.deviceName, i)) {
			break;
		}
	}
	return true;
}

// struct gs_vulkan_device (graphics/graphics-vulkan.h): in-process only, see there.
EXPORT void *device_get_device_obj(gs_device_t *device)
{
	return &device->native;
}

// Cube textures are not supported: nothing in libobs or the plugins creates one or renders into one (no effect
// declares a texture_cube), and D3D11 is their only user upstream. Logged so a new caller is noticed.
EXPORT gs_texture_t *device_cubetexture_create(gs_device_t *device, uint32_t size, enum gs_color_format color_format,
					       uint32_t levels, const uint8_t **data, uint32_t flags)
{
	(void)device, (void)size, (void)color_format, (void)levels, (void)data, (void)flags;
	blog(LOG_ERROR, "[libobs-vulkan] device_cubetexture_create: cube textures are not supported");
	return nullptr;
}

EXPORT gs_texture_t *device_get_render_target(const gs_device_t *device)
{
	return device->render_target;
}

EXPORT gs_zstencil_t *device_get_zstencil_target(const gs_device_t *device)
{
	return device->zstencil;
}

// shortcut: swapchains have no depth buffer (D3D11 substitutes the swapchain's for a null zstencil); OBS creates its
// display swapchains with GS_ZS_NONE.
EXPORT void device_set_render_target_with_color_space(gs_device_t *device, gs_texture_t *tex, gs_zstencil_t *zstencil,
						      enum gs_color_space space)
{
	// As D3D11: no texture means the loaded swapchain's.
	device->render_target = tex ? tex : device->swap ? &device->swap->target : nullptr;
	device->zstencil = zstencil;
	device->space = space;
}

EXPORT void device_set_render_target(gs_device_t *device, gs_texture_t *tex, gs_zstencil_t *zstencil)
{
	device_set_render_target_with_color_space(device, tex, zstencil, GS_CS_SRGB);
}

// See device_cubetexture_create: no cube texture can exist, so there is nothing to bind.
EXPORT void device_set_cube_render_target(gs_device_t *device, gs_texture_t *cubetex, int side, gs_zstencil_t *zstencil)
{
	(void)device, (void)cubetex, (void)side, (void)zstencil;
	blog(LOG_ERROR, "[libobs-vulkan] device_set_cube_render_target: cube textures are not supported");
}

// As D3D11's get_copy_compare_format: a format and its _UNORM variant may be copied into each other.
static enum gs_color_format copy_format(enum gs_color_format f)
{
	return f == GS_RGBA_UNORM ? GS_RGBA : f == GS_BGRX_UNORM ? GS_BGRX : f == GS_BGRA_UNORM ? GS_BGRA : f;
}

// Size of the Y plane of the two-plane image `t` is a plane view of.
static void luma_size(const gs_texture *t, uint32_t &w, uint32_t &h)
{
	const bool uv = t->aspect == VK_IMAGE_ASPECT_PLANE_1_BIT;
	w = uv ? t->width * 2 : t->width;
	h = uv ? t->height * 2 : t->height;
}

// Both planes of the two-plane image `t` is a plane view of: the Y plane, then the UV plane.
static void plane_extents(const gs_texture *t, VkExtent3D out[2])
{
	uint32_t w, h;
	luma_size(t, w, h);
	out[0] = {w, h, 1};
	out[1] = {w / 2, h / 2, 1};
}

// As D3D11: 2D textures of the same copy format (see copy_format); src_w/src_h 0 mean
// "to the source's edge". shortcut: mip 0 only (D3D11's whole-texture copy copies every level; OBS copies
// single-level textures).
//
// Plane views (NV12/P010) copy as D3D11's views of one NV12 texture do: a whole-texture copy (device_copy_texture,
// or every offset and size 0) between plane views of two images of the same two-plane format copies both planes,
// whichever plane the views are (CopyResource copies the underlying texture). libobs relies on it on Windows, where
// queue_frame (obs-video.c) copies only the Y texture of an encoder frame. Elsewhere it copies the UV texture too,
// which then copies that plane again: correct, at the cost of one extra chroma copy on that (duplicate frame) path.
// Any other copy involving a plane view copies only that plane (CopySubresourceRegion of subresource 0, and a plane
// to or from an ordinary R8/RG8 (R16/RG16) texture).
EXPORT void device_copy_texture_region(gs_device_t *device, gs_texture_t *dst, uint32_t dst_x, uint32_t dst_y,
				       gs_texture_t *src, uint32_t src_x, uint32_t src_y, uint32_t src_w,
				       uint32_t src_h)
{
	const char *error = !src || !dst                                               ? "texture is NULL"
			    : src->type != GS_TEXTURE_2D || dst->type != GS_TEXTURE_2D ? "not a 2D texture"
			    : !src->image || !dst->image || src->image == dst->image   ? "same or no image"
			    : copy_format(src->format) != copy_format(dst->format)     ? "formats do not match"
			    : src_x >= src->width || src_y >= src->height              ? "source offset out of range"
										       : nullptr;
	const uint32_t w = src_w ? src_w : src && src_x < src->width ? src->width - src_x : 0;
	const uint32_t h = src_h ? src_h : src && src_y < src->height ? src->height - src_y : 0;
	if (!error &&
	    (src_x + w > src->width || src_y + h > src->height || dst_x + w > dst->width || dst_y + h > dst->height)) {
		error = "region does not fit";
	}
	if (error) {
		blog(LOG_ERROR, "[libobs-vulkan] device_copy_texture: %s", error);
		return;
	}
	VkCommandBuffer cmd = vk_cmd(device);
	const uint32_t fam = device->graphics.family;
	vk_barriers b;
	device->tracker.use_image(
		src->image, fam,
		{VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL}, b);
	device->tracker.use_image(dst->image, fam,
				  {VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
				   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL},
				  b);
	vk_record_barriers(cmd, b);
	const bool whole = !dst_x && !dst_y && !src_x && !src_y && !src_w && !src_h;
	if (whole && vk_is_plane(src) && vk_is_plane(dst) && src->image_format == dst->image_format) {
		// Same formats and sizes (checked above), so the same plane of images of the same size.
		VkImageCopy regions[2] = {};
		VkExtent3D ext[2];
		plane_extents(src, ext);
		for (int i = 0; i < 2; ++i) {
			const VkImageAspectFlags plane = i ? VK_IMAGE_ASPECT_PLANE_1_BIT : VK_IMAGE_ASPECT_PLANE_0_BIT;
			regions[i].srcSubresource = regions[i].dstSubresource = {plane, 0, 0, 1};
			regions[i].extent = ext[i];
		}
		vkCmdCopyImage(cmd, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->image,
			       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, regions);
		return;
	}
	VkImageCopy region = {};
	region.srcSubresource = {src->aspect, 0, 0, 1};
	region.dstSubresource = {dst->aspect, 0, 0, 1};
	region.srcOffset = {(int32_t)src_x, (int32_t)src_y, 0};
	region.dstOffset = {(int32_t)dst_x, (int32_t)dst_y, 0};
	region.extent = {w, h, 1};
	vkCmdCopyImage(cmd, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->image,
		       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

EXPORT void device_copy_texture(gs_device_t *device, gs_texture_t *dst, gs_texture_t *src)
{
	device_copy_texture_region(device, dst, 0, 0, src, 0, 0, 0, 0);
}

// Staging into an NV12/P010 surface (device_stagesurface_create_nv12/_p010) takes the Y plane view of a two-plane
// image of the same format and size and copies both planes, as D3D11's CopyResource into its NV12 staging texture:
// the Y rows, then the UV rows right after them at the same linesize. A plane view staged into an ordinary surface of
// its own format and size copies only that plane.
EXPORT void obs_vulkan_test_stage_mip(gs_device_t *device, gs_stagesurf_t *dst, gs_texture_t *src, uint32_t level)
{
	const bool planar = dst && dst->planar;
	if (!dst || !src || level >= src->levels || dst->width != std::max(src->width >> level, 1u) ||
	    dst->height != std::max(src->height >> level, 1u) ||
	    (planar ? src->image_format != dst->planar || src->aspect != VK_IMAGE_ASPECT_PLANE_0_BIT
		    : dst->format != src->format)) {
		blog(LOG_ERROR, "[libobs-vulkan] device_stage_texture: size or format mismatch");
		return;
	}
	VkCommandBuffer cmd = vk_cmd(device);
	const uint32_t fam = device->graphics.family;
	vk_barriers b;
	device->tracker.use_image(
		src->image, fam,
		{VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL}, b);
	device->tracker.use_buffer(dst->buffer, fam, {VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT}, b);
	vk_record_barriers(cmd, b);

	VkBufferImageCopy regions[2] = {};
	regions[0].imageSubresource = {src->aspect, level, 0, 1};
	regions[0].imageExtent = {dst->width, dst->height, 1};
	uint32_t n = 1;
	if (planar) {
		// Tightly packed rows of either plane are `linesize` bytes; the offset is a multiple of 4 (even sizes).
		VkExtent3D ext[2];
		plane_extents(src, ext);
		regions[1].bufferOffset = (VkDeviceSize)dst->linesize * dst->height;
		regions[1].imageSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1};
		regions[1].imageExtent = ext[1];
		n = 2;
	}
	vkCmdCopyImageToBuffer(cmd, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->buffer, n, regions);

	// Makes the copy available to the host once the submission's timeline value signals.
	vk_barriers host;
	device->tracker.use_buffer(dst->buffer, fam, {VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT}, host);
	vk_record_barriers(cmd, host);
	dst->copy_value = device->graphics.next_value; // the value the frame being recorded will signal
}

EXPORT void device_stage_texture(gs_device_t *device, gs_stagesurf_t *dst, gs_texture_t *src)
{
	obs_vulkan_test_stage_mip(device, dst, src, 0);
}

// Safe point for the device-loss rebuild when nothing presents (no display open).
EXPORT void device_begin_frame(gs_device_t *device)
{
	vk_handle_loss(device);
}

EXPORT void device_end_scene(gs_device_t *device) {}

// Transfer clears outside rendering (vk_cmd ends an open instance; the next draw's tracker barrier orders them before
// the attachment accesses). A load op would save the barrier. As D3D11, depth and stencil clear the current depth
// buffer; a flag for an aspect its format lacks is ignored.
EXPORT void device_clear(gs_device_t *device, uint32_t clear_flags, const struct vec4 *color, float depth,
			 uint8_t stencil)
{
	const vk_access clear = {VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
				 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL};
	gs_texture *rt = device->render_target;
	if ((clear_flags & GS_CLEAR_COLOR) && rt && rt->image && color && vk_is_plane(rt) && rt->rt_view) {
		// vkCmdClearColorImage rejects formats that need a Y'CbCr conversion: clear the plane view as an
		// attachment. Not a load op: on NVIDIA (Linux 580) a loadOp CLEAR of the Y plane view is dropped.
		vk_barriers b;
		VkCommandBuffer cmd = vk_begin_rendering(device, rt, rt->rt_view, nullptr, b);
		VkClearAttachment ca = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {{{color->x, color->y, color->z, color->w}}}};
		VkClearRect rect = {{{0, 0}, {rt->width, rt->height}}, 0, 1};
		vkCmdClearAttachments(cmd, 1, &ca, 1, &rect);
	} else if ((clear_flags & GS_CLEAR_COLOR) && rt && rt->image && color && !vk_is_plane(rt)) {
		// no image: swapchain not acquired
		VkCommandBuffer cmd = vk_cmd(device);
		vk_barriers b;
		device->tracker.use_image(rt->image, device->graphics.family, clear, b);
		vk_record_barriers(cmd, b);

		VkClearColorValue value = {{color->x, color->y, color->z, color->w}};
		VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0,
						 VK_REMAINING_ARRAY_LAYERS};
		vkCmdClearColorImage(cmd, rt->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
	}

	gs_zstencil_buffer *zs = device->zstencil;
	const VkImageAspectFlags wanted = ((clear_flags & GS_CLEAR_DEPTH) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
					  ((clear_flags & GS_CLEAR_STENCIL) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
	const VkImageAspectFlags aspects = zs ? zs->aspect & wanted : 0;
	if (aspects) {
		VkCommandBuffer cmd = vk_cmd(device);
		vk_barriers b;
		vk_access a = clear;
		a.aspect = zs->aspect; // the layout transition covers the whole image
		device->tracker.use_image(zs->image, device->graphics.family, a, b);
		vk_record_barriers(cmd, b);

		// Vulkan rejects depth outside [0, 1]; D3D11 clamps it.
		VkClearDepthStencilValue value = {std::clamp(depth, 0.0f, 1.0f), stencil};
		VkImageSubresourceRange range = {aspects, 0, 1, 0, 1};
		vkCmdClearDepthStencilImage(cmd, zs->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
	}
}

EXPORT void device_flush(gs_device_t *device)
{
	vk_flush(device);
}

// struct gs_vulkan_texture (graphics/graphics-vulkan.h): in-process only, see there. Filled on each call, so the
// layout is the one last recorded.
EXPORT void *gs_texture_get_obj(gs_texture_t *tex)
{
	if (!tex) {
		return nullptr;
	}
	gs_vulkan_texture &n = tex->native;
	n = gs_vulkan_texture{};
	n.version = GS_VULKAN_OBJECTS_VERSION;
	n.image = tex->image;
	n.view = tex->view;
	n.srgb_view = tex->srgb_view;
	n.format = vk_is_plane(tex) ? tex->image_format : tex->rt_format ? tex->rt_format : vk_format(tex->format);
	n.aspect = tex->aspect;
	n.image_layout = tex->image ? tex->device->tracker.layout_of(tex->image) : VK_IMAGE_LAYOUT_UNDEFINED;
	n.width = tex->width;
	n.height = tex->height;
	n.depth = tex->depth;
	n.levels = tex->levels;
	return &n;
}

EXPORT void gs_cubetexture_destroy(gs_texture_t *cubetex)
{
	(void)cubetex; // none can exist (device_cubetexture_create)
}

EXPORT uint32_t gs_cubetexture_get_size(const gs_texture_t *cubetex)
{
	(void)cubetex;
	return 0;
}

EXPORT enum gs_color_format gs_cubetexture_get_color_format(const gs_texture_t *cubetex)
{
	(void)cubetex;
	return GS_UNKNOWN;
}

// VK_EXT_debug_utils labels in the recorded commands (RenderDoc, Nsight, PIX); no-ops without the extension. A label
// open across a submission is ended and begun again in the next command buffer (vk_frame_ring::labels).
EXPORT void device_debug_marker_begin(gs_device_t *device, const char *markername, const float color[4])
{
	if (!device->frames.begin_label) {
		return;
	}
	VkCommandBuffer cmd = vk_frame_cmd(device); // begins a frame first, which re-opens the labels already open
	vk_frame_ring::label l = {markername ? markername : "", {1.0f, 1.0f, 1.0f, 1.0f}};
	if (color) {
		for (int i = 0; i < 4; ++i) {
			l.color[i] = color[i];
		}
	}
	device->frames.labels.push_back(l);
	device->frames.record_label(cmd, device->frames.labels.back());
}

EXPORT void device_debug_marker_end(gs_device_t *device)
{
	if (!device->frames.end_label || device->frames.labels.empty()) {
		return;
	}
	VkCommandBuffer cmd = vk_frame_cmd(device); // a new frame has re-opened this one too
	device->frames.labels.pop_back();
	device->frames.end_label(cmd);
}

#ifdef __APPLE__

EXPORT gs_texture_t *device_texture_create_from_iosurface(gs_device_t *dev, void *iosurf)
{
	return {};
}

EXPORT gs_texture_t *device_texture_open_shared(gs_device_t *dev, uint32_t handle)
{
	return {};
}

EXPORT bool gs_texture_rebind_iosurface(gs_texture_t *texture, void *iosurf)
{
	return {};
}

EXPORT bool device_shared_texture_available(void)
{
	return {};
}

#elif _WIN32

EXPORT bool device_gdi_texture_available(void)
{
	return {};
}

// device_shared_texture_available and the shared texture exports live in vulkan-interop.cpp.

#elif defined(__linux__) || defined(__FreeBSD__) || defined(__DragonFly__)

// The dmabuf and explicit sync exports live in vulkan-interop.cpp.

#endif
}
