// External memory and synchronisation (docs/graphics-interop.md): exportable and imported textures, the pitch-linear
// frame buffer, timeline semaphores, adapter identity, and the legacy Windows shared texture and keyed mutex exports
// built on them.
//
// Ownership model: an external resource belongs to VK_QUEUE_FAMILY_EXTERNAL in VK_IMAGE_LAYOUT_GENERAL between this
// device's submissions. Its first use in a submission acquires it (vk_tracker::use_image), and vk_flush releases it
// again before submitting (vk_interop_prepare_submit). So the other side may use it whenever it holds the sync
// (a keyed mutex, or a timeline value this device signalled).
//
// Linux: opaque fds (VK_KHR_external_memory_fd, VK_KHR_external_semaphore_fd), dmabufs with explicit DRM format
// modifiers (VK_EXT_external_memory_dma_buf, VK_EXT_image_drm_format_modifier; owned by VK_QUEUE_FAMILY_FOREIGN_EXT
// between submissions) and sync_files. A driver offers OPAQUE_FD and DMA_BUF only as separate allocations, so an
// exportable texture has one type: single-plane textures are dmabufs (a renderable modifier picked by the driver),
// NV12/P010 render targets opaque fds, and NV12/P010 copy targets (no GS_RENDER_TARGET) are LINEAR dmabufs allocated
// with GBM and imported, because NVIDIA lays out a "LINEAR" NV12 image that Vulkan exports as a dmabuf differently
// from what it reports (spike C on vulkan-interop-spikes-linux).
#include "vulkan-subsystem.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_4.h>
#include <vulkan/vulkan_win32.h>
#endif

#ifndef _WIN32
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <gbm.h>
#endif

#include <util/base.h>

#include <errno.h>
#include <string.h>
#include <algorithm>
#include <type_traits>
#include <atomic>
#include <vector>

struct vk_interop {
#ifdef _WIN32
	PFN_vkGetMemoryWin32HandleKHR get_memory_handle = nullptr;
	PFN_vkGetMemoryWin32HandlePropertiesKHR get_memory_handle_properties = nullptr;
	PFN_vkGetSemaphoreWin32HandleKHR get_semaphore_handle = nullptr;
	PFN_vkImportSemaphoreWin32HandleKHR import_semaphore_handle = nullptr;
	// Reads the description (size, format, keyed mutex) of handles given to the legacy open_shared exports, which
	// carry nothing else. Created on first use on the adapter with this device's LUID; holds no texture.
	ID3D11Device1 *probe = nullptr;
	bool probe_failed = false;
#else
	PFN_vkGetMemoryFdKHR get_memory_fd = nullptr;
	PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties = nullptr;
	PFN_vkGetSemaphoreFdKHR get_semaphore_fd = nullptr;
	PFN_vkImportSemaphoreFdKHR import_semaphore_fd = nullptr;
	PFN_vkGetImageDrmFormatModifierPropertiesEXT get_modifier = nullptr;
	// The DRM render node of this device (VK_EXT_physical_device_drm), for GBM.
	bool has_render_node = false;
	dev_t render_node = 0;
	// GBM, loaded on first use (multi-plane dmabufs): libgbm.so.1 and a device on the render node.
	bool gbm_tried = false;
	void *gbm_lib = nullptr;
	int gbm_fd = -1;
	struct gbm_device *gbm = nullptr;
	decltype(&::gbm_create_device) create_device = nullptr;
	decltype(&::gbm_device_destroy) device_destroy = nullptr;
	decltype(&::gbm_device_is_format_supported) is_format_supported = nullptr;
	decltype(&::gbm_bo_create) bo_create = nullptr;
	decltype(&::gbm_bo_destroy) bo_destroy = nullptr;
	decltype(&::gbm_bo_get_fd) bo_get_fd = nullptr;
	decltype(&::gbm_bo_get_plane_count) bo_get_plane_count = nullptr;
	decltype(&::gbm_bo_get_offset) bo_get_offset = nullptr;
	decltype(&::gbm_bo_get_stride_for_plane) bo_get_stride_for_plane = nullptr;
	decltype(&::gbm_bo_get_modifier) bo_get_modifier = nullptr;
#endif
};

struct vk_keyed_mutex_info {
#ifdef _WIN32
	VkWin32KeyedMutexAcquireReleaseInfoKHR info = {VK_STRUCTURE_TYPE_WIN32_KEYED_MUTEX_ACQUIRE_RELEASE_INFO_KHR};
	std::vector<VkDeviceMemory> mem;
	std::vector<uint64_t> keys;
	std::vector<uint32_t> timeouts;
#endif
};

// device_shared_texture_available has no device argument: whether the last device created can import D3D11 textures.
static std::atomic<bool> shared_textures{false};

static constexpr uint32_t invalid_handle = 0xFFFFFFFFu; // GS_INVALID_HANDLE (Windows section of graphics.h)

void vk_interop_extensions(std::vector<const char *> &wanted)
{
#ifdef _WIN32
	wanted.push_back(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
	wanted.push_back(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
	wanted.push_back(VK_KHR_WIN32_KEYED_MUTEX_EXTENSION_NAME);
#else
	wanted.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
	wanted.push_back(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
	wanted.push_back(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
	wanted.push_back(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
	wanted.push_back(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
#endif
}

void vk_interop_init(gs_device *d, const std::vector<const char *> &enabled)
{
	auto on = [&](const char *name) {
		for (const char *e : enabled) {
			if (!strcmp(e, name)) {
				return true;
			}
		}
		return false;
	};
	d->interop = new vk_interop;
#ifdef _WIN32
	d->ext_memory_win32 = on(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
	d->ext_semaphore_win32 = on(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
	d->ext_keyed_mutex = on(VK_KHR_WIN32_KEYED_MUTEX_EXTENSION_NAME);
	vk_interop &i = *d->interop;
	if (d->ext_memory_win32) {
		i.get_memory_handle =
			(PFN_vkGetMemoryWin32HandleKHR)vkGetDeviceProcAddr(d->dev, "vkGetMemoryWin32HandleKHR");
		i.get_memory_handle_properties = (PFN_vkGetMemoryWin32HandlePropertiesKHR)vkGetDeviceProcAddr(
			d->dev, "vkGetMemoryWin32HandlePropertiesKHR");
		d->ext_memory_win32 = i.get_memory_handle && i.get_memory_handle_properties;
	}
	if (d->ext_semaphore_win32) {
		i.get_semaphore_handle =
			(PFN_vkGetSemaphoreWin32HandleKHR)vkGetDeviceProcAddr(d->dev, "vkGetSemaphoreWin32HandleKHR");
		i.import_semaphore_handle = (PFN_vkImportSemaphoreWin32HandleKHR)vkGetDeviceProcAddr(
			d->dev, "vkImportSemaphoreWin32HandleKHR");
		d->ext_semaphore_win32 = i.get_semaphore_handle && i.import_semaphore_handle;
	}
	shared_textures = d->ext_memory_win32 && d->id.deviceLUIDValid;
	blog(LOG_INFO, "[libobs-vulkan] interop: external memory %s, external semaphores %s, keyed mutex %s",
	     d->ext_memory_win32 ? "yes" : "no", d->ext_semaphore_win32 ? "yes" : "no",
	     d->ext_keyed_mutex ? "yes" : "no");
#else
	vk_interop &i = *d->interop;
	d->ext_memory_fd = on(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
	d->ext_dmabuf = d->ext_memory_fd && on(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) &&
			on(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
	d->ext_semaphore_fd = on(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
	d->ext_foreign = on(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
	if (d->ext_memory_fd) {
		i.get_memory_fd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(d->dev, "vkGetMemoryFdKHR");
		i.get_memory_fd_properties =
			(PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(d->dev, "vkGetMemoryFdPropertiesKHR");
		d->ext_memory_fd = i.get_memory_fd && i.get_memory_fd_properties;
	}
	if (d->ext_dmabuf) {
		i.get_modifier = (PFN_vkGetImageDrmFormatModifierPropertiesEXT)vkGetDeviceProcAddr(
			d->dev, "vkGetImageDrmFormatModifierPropertiesEXT");
		d->ext_dmabuf = d->ext_memory_fd && i.get_modifier;
	}
	if (d->ext_semaphore_fd) {
		i.get_semaphore_fd = (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(d->dev, "vkGetSemaphoreFdKHR");
		i.import_semaphore_fd =
			(PFN_vkImportSemaphoreFdKHR)vkGetDeviceProcAddr(d->dev, "vkImportSemaphoreFdKHR");
		d->ext_semaphore_fd = i.get_semaphore_fd && i.import_semaphore_fd;
	}
	// The render node, when the driver tells (an extension that only needs to be supported, not enabled).
	uint32_t n = 0;
	vkEnumerateDeviceExtensionProperties(d->phys, nullptr, &n, nullptr);
	std::vector<VkExtensionProperties> exts(n);
	vkEnumerateDeviceExtensionProperties(d->phys, nullptr, &n, exts.data());
	for (const VkExtensionProperties &e : exts) {
		if (!strcmp(e.extensionName, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME)) {
			VkPhysicalDeviceDrmPropertiesEXT drm = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
			VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &drm};
			vkGetPhysicalDeviceProperties2(d->phys, &p2);
			i.has_render_node = drm.hasRender != VK_FALSE;
			i.render_node = makedev((unsigned)drm.renderMajor, (unsigned)drm.renderMinor);
		}
	}
	blog(LOG_INFO,
	     "[libobs-vulkan] interop: opaque fd memory %s, dmabuf with modifiers %s, fd semaphores %s, foreign queue "
	     "%s, render node %s",
	     d->ext_memory_fd ? "yes" : "no", d->ext_dmabuf ? "yes" : "no", d->ext_semaphore_fd ? "yes" : "no",
	     d->ext_foreign ? "yes" : "no", i.has_render_node ? "yes" : "no");
#endif
}

void vk_interop_release(void *com)
{
#ifdef _WIN32
	((IUnknown *)com)->Release();
#else
	(void)com;
#endif
}

void vk_interop_close_fd(int fd)
{
#ifndef _WIN32
	if (fd >= 0) {
		close(fd);
	}
#else
	(void)fd;
#endif
}

void vk_interop_free(gs_device *d)
{
	if (!d->interop) {
		return;
	}
#ifdef _WIN32
	if (d->interop->probe) {
		d->interop->probe->Release();
	}
#else
	vk_interop &i = *d->interop;
	if (i.gbm) {
		i.device_destroy(i.gbm);
	}
	if (i.gbm_fd >= 0) {
		close(i.gbm_fd);
	}
	if (i.gbm_lib) {
		dlclose(i.gbm_lib);
	}
#endif
	delete d->interop;
	d->interop = nullptr;
}

void vk_interop_prepare_submit(gs_device *d, VkCommandBuffer cmd, vk_submit_extra &extra, vk_keyed_mutex_info *&km)
{
	vk_barriers b;
	d->tracker.release_external(d->graphics.family, b);
	vk_record_barriers(cmd, b);
	extra = std::move(d->pending);
	d->pending = vk_submit_extra{};
	km = nullptr;
#ifdef _WIN32
	if (!d->km_release_mem.empty()) {
		km = new vk_keyed_mutex_info;
		km->mem = std::move(d->km_release_mem);
		km->keys = std::move(d->km_release_key);
		km->info.releaseCount = (uint32_t)km->mem.size();
		km->info.pReleaseSyncs = km->mem.data();
		km->info.pReleaseKeys = km->keys.data();
		extra.pnext = &km->info;
	}
#endif
	d->km_release_mem.clear();
	d->km_release_key.clear();
}

void vk_interop_finish_submit(vk_keyed_mutex_info *km)
{
	delete km;
}

/* ---- Helpers ------------------------------------------------------------------------------------------------- */

static VkExternalMemoryHandleTypeFlagBits memory_type(enum gs_handle_type type)
{
	switch (type) {
#ifdef _WIN32
	case GS_HANDLE_WIN32:
		return VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
	case GS_HANDLE_WIN32_D3D11:
		return VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
	case GS_HANDLE_WIN32_D3D11_KMT:
		return VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
#else
	case GS_HANDLE_FD:
		return VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	case GS_HANDLE_DMABUF:
		return VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
#endif
	default:
		return (VkExternalMemoryHandleTypeFlagBits)0;
	}
}

static VkExternalSemaphoreHandleTypeFlagBits semaphore_type(enum gs_handle_type type)
{
	switch (type) {
#ifdef _WIN32
	case GS_HANDLE_WIN32:
		return VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
	case GS_HANDLE_WIN32_D3D12_FENCE:
		return VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
#else
	case GS_HANDLE_FD:
		return VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif
	default:
		return (VkExternalSemaphoreHandleTypeFlagBits)0;
	}
}

static bool is_d3d11(enum gs_handle_type type)
{
	return type == GS_HANDLE_WIN32_D3D11 || type == GS_HANDLE_WIN32_D3D11_KMT;
}

static bool memory_ext(gs_device *d)
{
#ifdef _WIN32
	return d->ext_memory_win32;
#else
	return d->ext_memory_fd;
#endif
}

static bool semaphore_ext(gs_device *d)
{
#ifdef _WIN32
	return d->ext_semaphore_win32;
#else
	return d->ext_semaphore_fd;
#endif
}

// The queue family that owns an external resource between submissions: dmabufs may go to any driver or API.
static uint32_t external_owner(gs_device *d, VkExternalMemoryHandleTypeFlags types)
{
	return (types & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) && d->ext_foreign ? VK_QUEUE_FAMILY_FOREIGN_EXT
											  : VK_QUEUE_FAMILY_EXTERNAL;
}

#ifndef _WIN32
// Vulkan takes over an fd it imports successfully; an import borrows the caller's handle, so it imports a copy.
static int dup_fd(int fd)
{
	return fd >= 0 ? fcntl(fd, F_DUPFD_CLOEXEC, 0) : -1;
}

// GBM on this device's render node, loaded on first use. Null when there is no GBM or no render node.
static vk_interop *gbm_of(gs_device *d)
{
	vk_interop &i = *d->interop;
	if (i.gbm_tried) {
		return i.gbm ? &i : nullptr;
	}
	i.gbm_tried = true;
	if (!i.has_render_node) {
		blog(LOG_INFO, "[libobs-vulkan] GBM: the driver does not report a render node");
		return nullptr;
	}
	// The render node with the device number the driver reported.
	if (DIR *dir = opendir("/dev/dri")) {
		while (struct dirent *e = readdir(dir)) {
			if (strncmp(e->d_name, "renderD", 7)) {
				continue;
			}
			std::string path = std::string("/dev/dri/") + e->d_name;
			struct stat st;
			if (stat(path.c_str(), &st) == 0 && S_ISCHR(st.st_mode) && st.st_rdev == i.render_node) {
				i.gbm_fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
				break;
			}
		}
		closedir(dir);
	}
	if (i.gbm_fd < 0) {
		blog(LOG_WARNING, "[libobs-vulkan] GBM: render node %u:%u not found or not openable",
		     major(i.render_node), minor(i.render_node));
		return nullptr;
	}
	i.gbm_lib = dlopen("libgbm.so.1", RTLD_NOW | RTLD_LOCAL);
	bool ok = i.gbm_lib != nullptr;
	auto load = [&](auto &fn, const char *name) {
		fn = ok ? (std::remove_reference_t<decltype(fn)>)dlsym(i.gbm_lib, name) : nullptr;
		ok = ok && fn;
	};
	load(i.create_device, "gbm_create_device");
	load(i.device_destroy, "gbm_device_destroy");
	load(i.is_format_supported, "gbm_device_is_format_supported");
	load(i.bo_create, "gbm_bo_create");
	load(i.bo_destroy, "gbm_bo_destroy");
	load(i.bo_get_fd, "gbm_bo_get_fd");
	load(i.bo_get_plane_count, "gbm_bo_get_plane_count");
	load(i.bo_get_offset, "gbm_bo_get_offset");
	load(i.bo_get_stride_for_plane, "gbm_bo_get_stride_for_plane");
	load(i.bo_get_modifier, "gbm_bo_get_modifier");
	if (ok) {
		i.gbm = i.create_device(i.gbm_fd);
	}
	if (!i.gbm) {
		blog(LOG_WARNING, "[libobs-vulkan] GBM: %s", i.gbm_lib ? "gbm_create_device failed" : dlerror());
		return nullptr;
	}
	blog(LOG_INFO, "[libobs-vulkan] GBM: device on render node %u:%u", major(i.render_node), minor(i.render_node));
	return &i;
}
#endif

// DRM fourcc of a gs format or layout (dmabufs); 0 when there is none.
static uint32_t drm_fourcc(enum gs_color_format format, enum gs_shared_layout layout)
{
#ifdef _WIN32
	(void)format;
	(void)layout;
	return 0;
#else
	if (layout == GS_SHARED_NV12) {
		return DRM_FORMAT_NV12;
	}
	if (layout == GS_SHARED_P010) {
		return DRM_FORMAT_P010;
	}
	switch (format) {
	case GS_BGRA:
	case GS_BGRA_UNORM:
		return DRM_FORMAT_ARGB8888;
	case GS_BGRX:
	case GS_BGRX_UNORM:
		return DRM_FORMAT_XRGB8888;
	case GS_RGBA:
	case GS_RGBA_UNORM:
		return DRM_FORMAT_ABGR8888;
	case GS_R10G10B10A2:
		return DRM_FORMAT_ABGR2101010;
	case GS_RGBA16F:
		return DRM_FORMAT_ABGR16161616F;
	case GS_RGBA16:
		return DRM_FORMAT_ABGR16161616;
	case GS_R8:
		return DRM_FORMAT_R8;
	case GS_R8G8:
		return DRM_FORMAT_GR88;
	case GS_R16:
		return DRM_FORMAT_R16;
	case GS_RG16:
		return DRM_FORMAT_GR1616;
	default:
		return 0;
	}
#endif
}

// On NVIDIA (driver 621.x) a D3D11 NV12 texture imported into Vulkan reads its chroma plane from the wrong place
// (scattered 64-byte pieces of other rows, out of bounds at small sizes, eventually device loss), while luma and
// validation are clean. See repro/d3d11-nv12-vulkan-import on vulkan-interop-spikes. D3D11 sources convert to RGB
// first. Refused for both NV12 and P010 (same layout family, untested).
static bool d3d11_planar_refused(gs_device *d, enum gs_handle_type type, enum gs_shared_layout layout)
{
	return is_d3d11(type) && layout != GS_SHARED_SINGLE && d->vendor_id == 0x10DE;
}

// Whether `ii` (without its pNext) can be created for external memory of `ht`, and with which features.
static bool external_image_ok(gs_device *d, const vk_image_desc &desc, VkExternalMemoryHandleTypeFlagBits ht,
			      bool import, bool *dedicated = nullptr)
{
	VkPhysicalDeviceExternalImageFormatInfo ei = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
	ei.handleType = ht;
	VkPhysicalDeviceImageFormatInfo2 info = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &ei};
	VkImageFormatListCreateInfo fl = desc.fl;
	if (desc.use_list) {
		fl.pNext = nullptr;
		ei.pNext = &fl;
	}
	info.format = desc.ii.format;
	info.type = desc.ii.imageType;
	info.tiling = desc.ii.tiling;
	info.usage = desc.ii.usage;
	info.flags = desc.ii.flags;
	VkExternalImageFormatProperties ep = {VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
	VkImageFormatProperties2 props = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &ep};
	if (vkGetPhysicalDeviceImageFormatProperties2(d->phys, &info, &props) != VK_SUCCESS) {
		return false;
	}
	const VkImageFormatProperties &p = props.imageFormatProperties;
	if (desc.ii.extent.width > p.maxExtent.width || desc.ii.extent.height > p.maxExtent.height) {
		return false;
	}
	const VkExternalMemoryFeatureFlags f = ep.externalMemoryProperties.externalMemoryFeatures;
	if (dedicated) {
		*dedicated = (f & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) != 0;
	}
	return (f & (import ? VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT : VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)) !=
	       0;
}

// Device-local memory type in `bits`, else any.
static uint32_t pick_memory_type(gs_device *d, uint32_t bits)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(d->phys, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
			return i;
		}
	}
	for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
		if (bits & (1u << i)) {
			return i;
		}
	}
	return UINT32_MAX;
}

// sRGB-capable formats need MUTABLE_FORMAT, which some imports refuse: falls back to the UNORM gs format.
static enum gs_color_format unorm_format(enum gs_color_format f)
{
	switch (f) {
	case GS_RGBA:
		return GS_RGBA_UNORM;
	case GS_BGRA:
		return GS_BGRA_UNORM;
	case GS_BGRX:
		return GS_BGRX_UNORM;
	default:
		return f;
	}
}

static void set_external_textures(gs_device *d, gs_texture *tex, gs_texture *tex_uv, VkDeviceMemory memory,
				  VkDeviceSize size, VkExternalMemoryHandleTypeFlags export_types,
				  enum gs_shared_layout layout, uint32_t flags, bool keyed_mutex)
{
	for (gs_texture *t : {tex, tex_uv}) {
		if (!t) {
			continue;
		}
		// The memory is freed with the image, by whichever plane view goes last (destroy_texture_objects).
		t->memory = memory;
		t->memory_size = size;
		t->export_types = export_types;
		t->layout = layout;
		t->create_flags = flags & GS_RENDER_TARGET;
		t->keyed_mutex = keyed_mutex;
	}
	(void)d;
}

/* ---- Import -------------------------------------------------------------------------------------------------- */

#ifdef _WIN32
static ID3D11Device1 *probe_device(gs_device *d);

// The keyed mutex of a D3D11 texture, opened on the probe device: finite-timeout acquires wait there on the CPU,
// because drivers disagree on a keyed mutex acquire inside vkQueueSubmit2 that cannot succeed in time: NVIDIA
// (621.24) returns VK_TIMEOUT, AMD (Adrenalin 26.11.2) returns VK_SUCCESS, so the timeout cannot be detected.
static IDXGIKeyedMutex *open_probe_mutex(gs_device *d, HANDLE handle, bool nt)
{
	ID3D11Device1 *probe = probe_device(d);
	ID3D11Texture2D *tex = nullptr;
	if (!probe || FAILED(nt ? probe->OpenSharedResource1(handle, __uuidof(ID3D11Texture2D), (void **)&tex)
				: probe->OpenSharedResource(handle, __uuidof(ID3D11Texture2D), (void **)&tex))) {
		return nullptr;
	}
	IDXGIKeyedMutex *km = nullptr;
	if (FAILED(tex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&km))) {
		km = nullptr;
	}
	tex->Release(); // the mutex keeps the resource open
	return km;
}
#endif

#ifndef _WIN32
/* ---- Linux: DRM format modifiers ----------------------------------------------------------------------------- */

static std::vector<VkDrmFormatModifierProperties2EXT> format_modifiers(gs_device *d, VkFormat format)
{
	VkDrmFormatModifierPropertiesList2EXT list = {VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_2_EXT};
	VkFormatProperties2 fp = {VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &list};
	vkGetPhysicalDeviceFormatProperties2(d->phys, format, &fp);
	std::vector<VkDrmFormatModifierProperties2EXT> mods(list.drmFormatModifierCount);
	list.pDrmFormatModifierProperties = mods.data();
	vkGetPhysicalDeviceFormatProperties2(d->phys, format, &fp);
	mods.resize(list.drmFormatModifierCount);
	return mods;
}

static const VkDrmFormatModifierProperties2EXT *
find_modifier(const std::vector<VkDrmFormatModifierProperties2EXT> &mods, uint64_t modifier)
{
	for (const auto &m : mods) {
		if (m.drmFormatModifier == modifier) {
			return &m;
		}
	}
	return nullptr;
}

// The format features `usage` needs. Multi-planar images render through their plane views (EXTENDED_USAGE), so
// for them a colour attachment is checked on the view formats instead.
static VkFormatFeatureFlags2 usage_features(VkImageUsageFlags usage, bool planar)
{
	VkFormatFeatureFlags2 f = 0;
	if (usage & VK_IMAGE_USAGE_SAMPLED_BIT) {
		f |= VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT;
	}
	if (usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) {
		f |= VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT;
	}
	if (usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) {
		f |= VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
	}
	if ((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && !planar) {
		f |= VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT;
	}
	return f;
}

// Whether `id` (tiling DRM_FORMAT_MODIFIER) can be created with `modifier` for dmabuf import or export: the
// modifier's features cover the usage (for multi-planar render targets also on both plane view formats), and the
// image with its view format list exists for that handle type. *planes: the modifier's memory plane count.
static bool modifier_ok(gs_device *d, const vk_image_desc &id, uint64_t modifier, bool import,
			uint32_t *planes = nullptr)
{
	const bool planar = (id.ii.flags & VK_IMAGE_CREATE_EXTENDED_USAGE_BIT) != 0;
	const auto mods = format_modifiers(d, id.ii.format);
	const VkDrmFormatModifierProperties2EXT *m = find_modifier(mods, modifier);
	const VkFormatFeatureFlags2 need = usage_features(id.ii.usage, planar);
	if (!m || (m->drmFormatModifierTilingFeatures & need) != need) {
		return false;
	}
	if (planar && (id.ii.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
		for (int v = 0; v < 2; ++v) {
			const auto vm = format_modifiers(d, id.views[v]);
			const VkDrmFormatModifierProperties2EXT *p = find_modifier(vm, modifier);
			if (!p || !(p->drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT)) {
				return false;
			}
		}
	}
	VkPhysicalDeviceImageDrmFormatModifierInfoEXT mi = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT};
	mi.drmFormatModifier = modifier;
	mi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VkImageFormatListCreateInfo fl = id.fl;
	fl.pNext = nullptr;
	if (id.use_list) {
		mi.pNext = &fl;
	}
	VkPhysicalDeviceExternalImageFormatInfo ei = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
						      &mi};
	ei.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	VkPhysicalDeviceImageFormatInfo2 info = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &ei};
	info.format = id.ii.format;
	info.type = id.ii.imageType;
	info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
	info.usage = id.ii.usage;
	info.flags = id.ii.flags;
	VkExternalImageFormatProperties ep = {VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
	VkImageFormatProperties2 props = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &ep};
	if (vkGetPhysicalDeviceImageFormatProperties2(d->phys, &info, &props) != VK_SUCCESS) {
		return false;
	}
	const VkImageFormatProperties &p = props.imageFormatProperties;
	if (id.ii.extent.width > p.maxExtent.width || id.ii.extent.height > p.maxExtent.height) {
		return false;
	}
	if (planes) {
		*planes = m->drmFormatModifierPlaneCount;
	}
	const VkExternalMemoryFeatureFlags f = ep.externalMemoryProperties.externalMemoryFeatures;
	return (f & (import ? VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT : VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)) !=
	       0;
}

// Every modifier `id` can be imported or exported with.
static std::vector<uint64_t> usable_modifiers(gs_device *d, const vk_image_desc &id, bool import)
{
	std::vector<uint64_t> out;
	if (!d->ext_dmabuf) {
		return out;
	}
	for (const auto &m : format_modifiers(d, id.ii.format)) {
		if (modifier_ok(d, id, m.drmFormatModifier, import)) {
			out.push_back(m.drmFormatModifier);
		}
	}
	return out;
}

// The dmabuf image parameters for a gs format or layout: the texture's own (vk_texture_image_info) with DRM format
// modifier tiling.
static bool dmabuf_image_info(gs_device *d, uint32_t width, uint32_t height, enum gs_color_format format,
			      enum gs_shared_layout layout, uint32_t flags, vk_image_desc &id)
{
	if (!vk_texture_image_info(d, width, height, format, layout, flags, id)) {
		return false;
	}
	id.ii.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
	return true;
}

/* ---- Linux: imports ------------------------------------------------------------------------------------------ */

// Chains `next` at the end of `ii`'s pNext chain.
static void chain(VkImageCreateInfo &ii, void *next)
{
	auto *p = (VkBaseOutStructure *)&ii;
	while (p->pNext) {
		p = p->pNext;
	}
	p->pNext = (VkBaseOutStructure *)next;
}

static void chain_list(vk_image_desc &id)
{
	id.ii.pNext = nullptr;
	if (id.use_list) {
		id.fl.pNext = nullptr;
		id.ii.pNext = &id.fl;
	}
}

// Allocates `image`'s dedicated memory from a copy of `fd` (Vulkan takes over the fds it imports) and binds it.
// Never with VkExportMemoryAllocateInfo: NVIDIA (580.196.07) then allocates new memory instead of importing the
// dmabuf, without an error (repro/dmabuf-import-with-export-info).
static VkDeviceMemory import_fd_memory(gs_device *d, VkImage image, VkExternalMemoryHandleTypeFlagBits ht, int fd,
				       uint64_t opaque_size, VkDeviceSize &size)
{
	VkMemoryRequirements2 mr = {VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
	VkImageMemoryRequirementsInfo2 mri = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
	mri.image = image;
	vkGetImageMemoryRequirements2(d->dev, &mri, &mr);
	uint32_t bits = mr.memoryRequirements.memoryTypeBits;
	VkDeviceSize alloc_size = mr.memoryRequirements.size;
	if (ht == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) {
		VkMemoryFdPropertiesKHR fp = {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
		if (d->interop->get_memory_fd_properties(d->dev, ht, fd, &fp) != VK_SUCCESS) {
			blog(LOG_ERROR, "[libobs-vulkan] texture import: not a dmabuf this device can import");
			return VK_NULL_HANDLE;
		}
		bits &= fp.memoryTypeBits;
		const off_t end = lseek(fd, 0, SEEK_END);
		if (end >= 0 && (VkDeviceSize)end < alloc_size) {
			blog(LOG_ERROR, "[libobs-vulkan] texture import: dmabuf of %lld bytes, the image needs %llu",
			     (long long)end, (unsigned long long)alloc_size);
			return VK_NULL_HANDLE;
		}
	} else {
		alloc_size = opaque_size; // the exporter's allocation
	}
	const uint32_t type = pick_memory_type(d, bits);
	const int copy = dup_fd(fd);
	if (type == UINT32_MAX || copy < 0) {
		blog(LOG_ERROR, "[libobs-vulkan] texture import: no memory type (bits 0x%x) or dup failed", bits);
		if (copy >= 0) {
			close(copy);
		}
		return VK_NULL_HANDLE;
	}
	VkMemoryDedicatedAllocateInfo dai = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
	dai.image = image;
	VkImportMemoryFdInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, &dai};
	imp.handleType = ht;
	imp.fd = copy;
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp};
	ai.allocationSize = alloc_size;
	ai.memoryTypeIndex = type;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkResult r = vkAllocateMemory(d->dev, &ai, nullptr, &memory);
	if (r != VK_SUCCESS) {
		close(copy); // not taken over
		blog(LOG_ERROR,
		     "[libobs-vulkan] texture import: importing the memory failed (%d; size %llu, required %llu, type %u)",
		     (int)r, (unsigned long long)alloc_size, (unsigned long long)mr.memoryRequirements.size, type);
		return VK_NULL_HANDLE;
	}
	r = vkBindImageMemory(d->dev, image, memory, 0);
	if (r != VK_SUCCESS) {
		blog(LOG_ERROR, "[libobs-vulkan] texture import: vkBindImageMemory failed (%d)", (int)r);
		vkFreeMemory(d->dev, memory, nullptr);
		return VK_NULL_HANDLE;
	}
	size = alloc_size;
	return memory;
}

// The texture(s) on an image with imported memory; frees both on failure.
static bool wrap_imported(gs_device *d, const gs_shared_texture_desc &desc, enum gs_color_format format, VkImage image,
			  VkDeviceMemory memory, VkDeviceSize size, VkExternalMemoryHandleTypeFlagBits ht,
			  gs_texture **tex, gs_texture **tex_uv)
{
	gs_texture *uv = nullptr;
	if (!vk_wrap_image(d, image, desc.width, desc.height, format, desc.layout, desc.flags, tex, &uv)) {
		blog(LOG_ERROR, "[libobs-vulkan] texture import: view creation failed");
		vkFreeMemory(d->dev, memory, nullptr);
		vkDestroyImage(d->dev, image, nullptr);
		return false;
	}
	set_external_textures(d, *tex, uv, memory, size, 0, desc.layout, desc.flags, false);
	for (gs_texture *t : {*tex, uv}) {
		if (t) {
			t->origin = gs_texture::ORIGIN_IMPORTED;
		}
	}
	d->tracker.set_external(image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, external_owner(d, ht));
	if (tex_uv) {
		*tex_uv = uv;
	}
	return true;
}

// An opaque fd exported by Vulkan (this driver): the exporter's image parameters, optimal tiling, its allocation size.
static bool import_opaque_fd(gs_device *d, const gs_shared_texture_desc &desc, gs_texture **tex, gs_texture **tex_uv)
{
	const VkExternalMemoryHandleTypeFlagBits ht = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	vk_image_desc id;
	if (desc.fds[0] < 0 ||
	    !vk_texture_image_info(d, desc.width, desc.height, desc.format, desc.layout, desc.flags, id) ||
	    !external_image_ok(d, id, ht, true)) {
		blog(LOG_ERROR, "[libobs-vulkan] texture import: format %d layout %d %ux%u unsupported for opaque fds",
		     (int)desc.format, (int)desc.layout, desc.width, desc.height);
		return false;
	}
	VkExternalMemoryImageCreateInfo emi = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
	emi.handleTypes = ht;
	chain_list(id);
	chain(id.ii, &emi);
	VkImage image = VK_NULL_HANDLE;
	if (vkCreateImage(d->dev, &id.ii, nullptr, &image) != VK_SUCCESS) {
		blog(LOG_ERROR, "[libobs-vulkan] texture import: vkCreateImage failed");
		return false;
	}
	VkDeviceSize size = 0;
	VkDeviceMemory memory = import_fd_memory(d, image, ht, desc.fds[0], desc.size, size);
	if (!memory) {
		vkDestroyImage(d->dev, image, nullptr);
		return false;
	}
	return wrap_imported(d, desc, desc.format, image, memory, size, ht, tex, tex_uv);
}

// A dmabuf with an explicit modifier and one plane layout per memory plane. Every plane must be in the same buffer
// (fds of one dmabuf, as GBM, PipeWire and VA-API hand them out); disjoint planes are not supported.
static bool import_dmabuf(gs_device *d, const gs_shared_texture_desc &desc, gs_texture **tex, gs_texture **tex_uv)
{
	if (!d->ext_dmabuf) {
		blog(LOG_ERROR, "[libobs-vulkan] texture import: dmabufs with modifiers unsupported by this device");
		return false;
	}
	if (desc.modifier == GS_SHARED_MODIFIER_INVALID) {
		blog(LOG_ERROR,
		     "[libobs-vulkan] texture import: dmabuf without an explicit modifier (implicit modifiers "
		     "are unsupported)");
		return false;
	}
	const uint32_t planes = desc.num_planes ? desc.num_planes : 1;
	if (desc.fds[0] < 0 || planes > GS_SHARED_MAX_PLANES) {
		blog(LOG_ERROR, "[libobs-vulkan] texture import: dmabuf without fd or with %u planes", planes);
		return false;
	}
	struct stat first;
	if (fstat(desc.fds[0], &first) != 0) {
		return false;
	}
	for (uint32_t p = 1; p < planes; ++p) {
		struct stat st;
		if (desc.fds[p] >= 0 && desc.fds[p] != desc.fds[0] &&
		    (fstat(desc.fds[p], &st) != 0 || st.st_ino != first.st_ino || st.st_dev != first.st_dev)) {
			blog(LOG_ERROR, "[libobs-vulkan] texture import: dmabuf planes in separate buffers (disjoint) "
					"are unsupported");
			return false;
		}
	}
	enum gs_color_format format = desc.format;
	vk_image_desc id;
	uint32_t mod_planes = 0;
	bool ok = dmabuf_image_info(d, desc.width, desc.height, format, desc.layout, desc.flags, id) &&
		  modifier_ok(d, id, desc.modifier, true, &mod_planes);
	if (!ok && desc.layout == GS_SHARED_SINGLE && unorm_format(format) != format) {
		format = unorm_format(format); // no sRGB view if the modifier refuses MUTABLE_FORMAT
		ok = dmabuf_image_info(d, desc.width, desc.height, format, desc.layout, desc.flags, id) &&
		     modifier_ok(d, id, desc.modifier, true, &mod_planes);
	}
	if (!ok) {
		blog(LOG_ERROR,
		     "[libobs-vulkan] texture import: format %d layout %d %ux%u%s with modifier 0x%016llx unsupported",
		     (int)desc.format, (int)desc.layout, desc.width, desc.height,
		     (desc.flags & GS_RENDER_TARGET) ? " (render target)" : "", (unsigned long long)desc.modifier);
		return false;
	}
	if (mod_planes != planes) {
		blog(LOG_ERROR, "[libobs-vulkan] texture import: modifier 0x%016llx has %u memory planes, %u given",
		     (unsigned long long)desc.modifier, mod_planes, planes);
		return false;
	}
	VkSubresourceLayout layouts[GS_SHARED_MAX_PLANES] = {};
	for (uint32_t p = 0; p < planes; ++p) {
		layouts[p].offset = desc.offsets[p];
		layouts[p].rowPitch = desc.pitches[p];
	}
	VkImageDrmFormatModifierExplicitCreateInfoEXT ex = {
		VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
	ex.drmFormatModifier = desc.modifier;
	ex.drmFormatModifierPlaneCount = planes;
	ex.pPlaneLayouts = layouts;
	VkExternalMemoryImageCreateInfo emi = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &ex};
	emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	chain_list(id);
	chain(id.ii, &emi);
	VkImage image = VK_NULL_HANDLE;
	const VkResult r = vkCreateImage(d->dev, &id.ii, nullptr, &image);
	if (r != VK_SUCCESS) {
		blog(LOG_ERROR, "[libobs-vulkan] texture import: vkCreateImage for the dmabuf failed (%d)", (int)r);
		return false;
	}
	VkDeviceSize size = 0;
	VkDeviceMemory memory =
		import_fd_memory(d, image, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, desc.fds[0], 0, size);
	if (!memory) {
		vkDestroyImage(d->dev, image, nullptr);
		return false;
	}
	return wrap_imported(d, desc, format, image, memory, size, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, tex,
			     tex_uv);
}
#endif

static bool import_texture(gs_device *d, const gs_shared_texture_desc &desc, gs_texture **tex, gs_texture **tex_uv)
{
	*tex = nullptr;
	if (tex_uv) {
		*tex_uv = nullptr;
	}
	const VkExternalMemoryHandleTypeFlagBits ht = memory_type(desc.type);
	if (!ht || !memory_ext(d)) {
		blog(LOG_ERROR, "[libobs-vulkan] texture import: handle type %d unsupported", (int)desc.type);
		return false;
	}
	if (d3d11_planar_refused(d, desc.type, desc.layout)) {
		blog(LOG_ERROR, "[libobs-vulkan] texture import: D3D11 NV12/P010 import is refused on NVIDIA (wrong "
				"chroma plane, driver bug); convert to RGB on the D3D11 side");
		return false;
	}
	static const uint8_t zero[VK_UUID_SIZE] = {};
	if (memcmp(desc.device_uuid, zero, VK_UUID_SIZE) && memcmp(desc.device_uuid, d->id.deviceUUID, VK_UUID_SIZE)) {
		blog(LOG_ERROR, "[libobs-vulkan] texture import: exported by another adapter");
		return false;
	}
	const bool opaque = desc.type == GS_HANDLE_WIN32 || desc.type == GS_HANDLE_FD;
	if (opaque && !desc.size) {
		blog(LOG_ERROR, "[libobs-vulkan] texture import: opaque handles need the allocation size");
		return false;
	}
	if (desc.keyed_mutex && (!d->ext_keyed_mutex || !is_d3d11(desc.type))) {
		blog(LOG_ERROR, "[libobs-vulkan] texture import: keyed mutex unsupported");
		return false;
	}
#ifdef _WIN32
	HANDLE handle = (HANDLE)desc.win32;
	if (!handle) {
		return false;
	}

	enum gs_color_format format = desc.format;
	vk_image_desc id;
	bool ok = vk_texture_image_info(d, desc.width, desc.height, format, desc.layout, desc.flags, id) &&
		  external_image_ok(d, id, ht, true);
	if (!ok && desc.layout == GS_SHARED_SINGLE && unorm_format(format) != format && !opaque) {
		format = unorm_format(format); // no sRGB view: D3D11 refuses MUTABLE_FORMAT for this one
		ok = vk_texture_image_info(d, desc.width, desc.height, format, desc.layout, desc.flags, id) &&
		     external_image_ok(d, id, ht, true);
	}
	if (!ok) {
		blog(LOG_ERROR,
		     "[libobs-vulkan] texture import: format %d layout %d %ux%u unsupported for handle type %d",
		     (int)desc.format, (int)desc.layout, desc.width, desc.height, (int)desc.type);
		return false;
	}

	VkExternalMemoryImageCreateInfo emi = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
	emi.handleTypes = ht;
	if (id.use_list) {
		id.fl.pNext = &emi;
		id.ii.pNext = &id.fl;
	} else {
		id.ii.pNext = &emi;
	}
	VkImage image = VK_NULL_HANDLE;
	if (vkCreateImage(d->dev, &id.ii, nullptr, &image) != VK_SUCCESS) {
		blog(LOG_ERROR, "[libobs-vulkan] texture import: vkCreateImage failed");
		return false;
	}

	VkMemoryDedicatedRequirements dr = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
	VkMemoryRequirements2 mr = {VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dr};
	VkImageMemoryRequirementsInfo2 mri = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
	mri.image = image;
	vkGetImageMemoryRequirements2(d->dev, &mri, &mr);
	uint32_t bits = mr.memoryRequirements.memoryTypeBits;
	if (!opaque) {
		// Not allowed for the opaque types; those use the exporter's choice, which pick_memory_type repeats.
		VkMemoryWin32HandlePropertiesKHR hp = {VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
		if (d->interop->get_memory_handle_properties(d->dev, ht, handle, &hp) != VK_SUCCESS) {
			blog(LOG_ERROR, "[libobs-vulkan] texture import: handle not valid on this adapter");
			vkDestroyImage(d->dev, image, nullptr);
			return false;
		}
		bits &= hp.memoryTypeBits;
	}
	const uint32_t type = pick_memory_type(d, bits);

	// Every vendor reports DEDICATED_ONLY for these types (spike 2): always a dedicated allocation.
	VkMemoryDedicatedAllocateInfo dai = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
	dai.image = image;
	VkImportMemoryWin32HandleInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, &dai};
	imp.handleType = ht;
	imp.handle = handle;
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp};
	ai.allocationSize = opaque ? desc.size : mr.memoryRequirements.size;
	ai.memoryTypeIndex = type;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkResult r = type == UINT32_MAX ? VK_ERROR_FORMAT_NOT_SUPPORTED
					: vkAllocateMemory(d->dev, &ai, nullptr, &memory);
	if (r == VK_SUCCESS) {
		r = vkBindImageMemory(d->dev, image, memory, 0);
	}
	if (r != VK_SUCCESS) {
		blog(LOG_ERROR,
		     "[libobs-vulkan] texture import: importing the memory failed (%d; size %llu, required %llu, type %u)",
		     (int)r, (unsigned long long)ai.allocationSize, (unsigned long long)mr.memoryRequirements.size,
		     type);
		// NVIDIA (621.24) refuses Vulkan-exported memory in the process that exported it (any VkDevice, also a
		// duplicated handle) with VK_ERROR_OUT_OF_DEVICE_MEMORY; another process imports it fine.
		if (opaque && d->vendor_id == 0x10DE && r == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
			blog(LOG_ERROR, "[libobs-vulkan] texture import: NVIDIA cannot import memory exported by this "
					"process; import it in another process");
		}
		vkFreeMemory(d->dev, memory, nullptr);
		vkDestroyImage(d->dev, image, nullptr);
		return false;
	}
	gs_texture *uv = nullptr;
	if (!vk_wrap_image(d, image, desc.width, desc.height, format, desc.layout, desc.flags, tex, &uv)) {
		blog(LOG_ERROR, "[libobs-vulkan] texture import: view creation failed");
		vkFreeMemory(d->dev, memory, nullptr);
		vkDestroyImage(d->dev, image, nullptr);
		return false;
	}
	set_external_textures(d, *tex, uv, memory, ai.allocationSize, 0, desc.layout, desc.flags, desc.keyed_mutex);
	for (gs_texture *t : {*tex, uv}) {
		if (t) {
			t->origin = gs_texture::ORIGIN_IMPORTED;
		}
	}
	if (desc.keyed_mutex) {
		(*tex)->km_probe = open_probe_mutex(d, handle, desc.type == GS_HANDLE_WIN32_D3D11);
		if (!(*tex)->km_probe) {
			blog(LOG_WARNING, "[libobs-vulkan] texture import: keyed mutex not opened on the D3D11 probe; "
					  "only infinite acquires will work");
		}
	}
	d->tracker.set_external(image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
	if (tex_uv) {
		*tex_uv = uv;
	}
	return true;
#else
	return desc.type == GS_HANDLE_DMABUF ? import_dmabuf(d, desc, tex, tex_uv)
					     : import_opaque_fd(d, desc, tex, tex_uv);
#endif
}

/* ---- Legacy Windows shared textures -------------------------------------------------------------------------- */

#ifdef _WIN32
static ID3D11Device1 *probe_device(gs_device *d)
{
	vk_interop &i = *d->interop;
	if (i.probe || i.probe_failed) {
		return i.probe;
	}
	i.probe_failed = true;
	if (!d->id.deviceLUIDValid) {
		return nullptr;
	}
	IDXGIFactory4 *factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void **)&factory))) {
		return nullptr;
	}
	LUID luid;
	memcpy(&luid, d->id.deviceLUID, sizeof(luid));
	IDXGIAdapter *adapter = nullptr;
	HRESULT hr = factory->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter), (void **)&adapter);
	factory->Release();
	if (FAILED(hr)) {
		blog(LOG_ERROR, "[libobs-vulkan] shared textures: no DXGI adapter with this device's LUID");
		return nullptr;
	}
	const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
	ID3D11Device *dev = nullptr;
	hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &dev,
			       nullptr, nullptr);
	adapter->Release();
	if (FAILED(hr)) {
		blog(LOG_ERROR, "[libobs-vulkan] shared textures: D3D11CreateDevice failed (0x%08lx)",
		     (unsigned long)hr);
		return nullptr;
	}
	hr = dev->QueryInterface(__uuidof(ID3D11Device1), (void **)&i.probe);
	dev->Release();
	if (FAILED(hr)) {
		i.probe = nullptr;
		return nullptr;
	}
	i.probe_failed = false;
	return i.probe;
}

static bool dxgi_to_gs(DXGI_FORMAT f, enum gs_color_format &format, enum gs_shared_layout &layout)
{
	layout = GS_SHARED_SINGLE;
	switch (f) {
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		format = GS_BGRA;
		return true;
	case DXGI_FORMAT_B8G8R8X8_TYPELESS:
	case DXGI_FORMAT_B8G8R8X8_UNORM:
	case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
		format = GS_BGRX;
		return true;
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		format = GS_RGBA;
		return true;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS:
	case DXGI_FORMAT_R10G10B10A2_UNORM:
		format = GS_R10G10B10A2;
		return true;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS:
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
		format = GS_RGBA16F;
		return true;
	case DXGI_FORMAT_R8_UNORM:
		format = GS_R8;
		return true;
	case DXGI_FORMAT_R8G8_UNORM:
		format = GS_R8G8;
		return true;
	case DXGI_FORMAT_NV12:
		format = GS_UNKNOWN;
		layout = GS_SHARED_NV12;
		return true;
	case DXGI_FORMAT_P010:
		format = GS_UNKNOWN;
		layout = GS_SHARED_P010;
		return true;
	default:
		return false;
	}
}

// Fills desc from the D3D11 description of a legacy (KMT) or NT handle.
static bool describe_d3d11_handle(gs_device *d, HANDLE handle, bool nt, gs_shared_texture_desc &desc)
{
	ID3D11Device1 *probe = probe_device(d);
	if (!probe) {
		return false;
	}
	ID3D11Texture2D *tex = nullptr;
	HRESULT hr = nt ? probe->OpenSharedResource1(handle, __uuidof(ID3D11Texture2D), (void **)&tex)
			: probe->OpenSharedResource(handle, __uuidof(ID3D11Texture2D), (void **)&tex);
	if (FAILED(hr)) {
		blog(LOG_ERROR, "[libobs-vulkan] shared texture: cannot open handle 0x%p (0x%08lx)", handle,
		     (unsigned long)hr);
		return false;
	}
	D3D11_TEXTURE2D_DESC td;
	tex->GetDesc(&td);
	tex->Release();
	desc = gs_shared_texture_desc{};
	desc.type = nt ? GS_HANDLE_WIN32_D3D11 : GS_HANDLE_WIN32_D3D11_KMT;
	desc.win32 = handle;
	for (int &fd : desc.fds) {
		fd = -1;
	}
	desc.width = td.Width;
	desc.height = td.Height;
	desc.flags = (td.BindFlags & D3D11_BIND_RENDER_TARGET) ? GS_RENDER_TARGET : 0;
	desc.keyed_mutex = (td.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) != 0;
	desc.modifier = GS_SHARED_MODIFIER_INVALID;
	if (td.MipLevels != 1 || td.ArraySize != 1 || td.SampleDesc.Count != 1 ||
	    !dxgi_to_gs(td.Format, desc.format, desc.layout)) {
		blog(LOG_ERROR, "[libobs-vulkan] shared texture: unsupported D3D11 texture (format %d, %u levels)",
		     (int)td.Format, td.MipLevels);
		return false;
	}
	return true;
}

static gs_texture *open_legacy(gs_device *d, uint32_t handle, bool nt)
{
	gs_shared_texture_desc desc;
	if (!memory_ext(d) || !describe_d3d11_handle(d, (HANDLE)(uintptr_t)handle, nt, desc)) {
		return nullptr;
	}
	if (desc.layout != GS_SHARED_SINGLE) {
		// One gs_texture cannot hold both planes; gs_texture_import returns the two plane views.
		blog(LOG_ERROR,
		     "[libobs-vulkan] device_texture_open_%sshared: NV12/P010 handles need gs_texture_import",
		     nt ? "nt_" : "");
		return nullptr;
	}
	if (desc.keyed_mutex && !d->ext_keyed_mutex) {
		blog(LOG_WARNING, "[libobs-vulkan] shared texture has a keyed mutex but VK_KHR_win32_keyed_mutex is "
				  "missing; opened without it");
		desc.keyed_mutex = false;
	}
	gs_texture *tex = nullptr;
	if (!import_texture(d, desc, &tex, nullptr)) {
		return nullptr;
	}
	tex->shared_handle = handle;
	tex->shared_nt = nt;
	return tex;
}
#endif

/* ---- Exportable textures ------------------------------------------------------------------------------------- */

// The handle type of frame buffers (gs_sharedbuf_t): opaque, for CUDA.
static VkExternalMemoryHandleTypeFlagBits export_buffer_type()
{
#ifdef _WIN32
	return VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
	return VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif
}

#ifndef _WIN32
// GBM can allocate a LINEAR NV12/P010 dmabuf on this device.
static bool gbm_planar_ok(gs_device *d, enum gs_shared_layout layout)
{
	vk_interop *g = d->ext_dmabuf ? gbm_of(d) : nullptr;
	return g && g->is_format_supported(g->gbm, drm_fourcc(GS_UNKNOWN, layout), GBM_BO_USE_LINEAR);
}
#endif

// The one handle type an exportable texture is created for (see the top of this file). Linux: single-plane
// textures are dmabufs, NV12/P010 render targets opaque fds, NV12/P010 copy targets GBM dmabufs.
static VkExternalMemoryHandleTypeFlagBits export_memory_type(gs_device *d, enum gs_shared_layout layout, uint32_t flags)
{
#ifdef _WIN32
	(void)d;
	(void)layout;
	(void)flags;
	return VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
	if (layout == GS_SHARED_SINGLE) {
		return d->ext_dmabuf ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
				     : VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	}
	if (!(flags & GS_RENDER_TARGET) && gbm_planar_ok(d, layout)) {
		return VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	}
	return VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif
}

// Records a release of `image` from UNDEFINED to GENERAL for the external owner, submits it and waits for it, so a
// fresh exportable resource is in the hand-off state before its handle can leave this device.
static bool initial_release(gs_device *d, VkImage image, VkBuffer buffer, uint32_t owner = VK_QUEUE_FAMILY_EXTERNAL)
{
	VkCommandBuffer cmd = vk_cmd(d);
	VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
	VkImageMemoryBarrier2 ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
	VkBufferMemoryBarrier2 bb = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
	if (image) {
		ib.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
		ib.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
		ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		ib.srcQueueFamilyIndex = d->graphics.family;
		ib.dstQueueFamilyIndex = owner;
		ib.image = image;
		ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		di.imageMemoryBarrierCount = 1;
		di.pImageMemoryBarriers = &ib;
	} else {
		bb.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
		bb.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
		bb.srcQueueFamilyIndex = d->graphics.family;
		bb.dstQueueFamilyIndex = owner;
		bb.buffer = buffer;
		bb.size = VK_WHOLE_SIZE;
		di.bufferMemoryBarrierCount = 1;
		di.pBufferMemoryBarriers = &bb;
	}
	vkCmdPipelineBarrier2(cmd, &di);
	if (!vk_flush(d)) {
		return false;
	}
	const uint64_t value = d->graphics.next_value - 1;
	VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
	wi.semaphoreCount = 1;
	wi.pSemaphores = &d->graphics.timeline;
	wi.pValues = &value;
	return vk_check(d, vkWaitSemaphores(d->dev, &wi, UINT64_MAX)) == VK_SUCCESS;
}

// Dedicated, exportable device-local memory for `image` or `buffer`.
static VkDeviceMemory allocate_exportable(gs_device *d, VkImage image, VkBuffer buffer,
					  VkExternalMemoryHandleTypeFlagBits ht, VkDeviceSize &size)
{
	VkMemoryDedicatedRequirements dr = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
	VkMemoryRequirements2 mr = {VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dr};
	if (image) {
		VkImageMemoryRequirementsInfo2 mri = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
		mri.image = image;
		vkGetImageMemoryRequirements2(d->dev, &mri, &mr);
	} else {
		VkBufferMemoryRequirementsInfo2 mri = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2};
		mri.buffer = buffer;
		vkGetBufferMemoryRequirements2(d->dev, &mri, &mr);
	}
	const uint32_t type = pick_memory_type(d, mr.memoryRequirements.memoryTypeBits);
	if (type == UINT32_MAX) {
		return VK_NULL_HANDLE;
	}
	VkMemoryDedicatedAllocateInfo dai = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
	dai.image = image;
	dai.buffer = buffer;
	VkExportMemoryAllocateInfo ex = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &dai};
	ex.handleTypes = ht;
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ex};
	ai.allocationSize = mr.memoryRequirements.size;
	ai.memoryTypeIndex = type;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	if (vkAllocateMemory(d->dev, &ai, nullptr, &memory) != VK_SUCCESS) {
		return VK_NULL_HANDLE;
	}
	size = ai.allocationSize;
	return memory;
}

#ifndef _WIN32
// An NV12/P010 copy target: a LINEAR dmabuf from GBM, imported. Vulkan's own NV12 dmabuf export is not laid out as it
// reports on NVIDIA, GBM's is (spike C). The dmabuf fd keeps the buffer alive; the GBM object is not kept.
static bool create_gbm_exportable(gs_device *d, uint32_t width, uint32_t height, enum gs_shared_layout layout,
				  uint32_t flags, gs_texture **tex, gs_texture **tex_uv)
{
	vk_interop *g = gbm_of(d);
	struct gbm_bo *bo = g ? g->bo_create(g->gbm, width, height, drm_fourcc(GS_UNKNOWN, layout), GBM_BO_USE_LINEAR)
			      : nullptr;
	if (!bo) {
		blog(LOG_ERROR, "[libobs-vulkan] device_texture_create_exportable: GBM allocation of %ux%u %s failed",
		     width, height, layout == GS_SHARED_P010 ? "P010" : "NV12");
		return false;
	}
	gs_shared_texture_desc desc = {};
	for (int &fd : desc.fds) {
		fd = -1;
	}
	desc.type = GS_HANDLE_DMABUF;
	desc.width = width;
	desc.height = height;
	desc.format = GS_UNKNOWN;
	desc.layout = layout;
	desc.flags = flags;
	desc.fds[0] = g->bo_get_fd(bo);
	desc.num_planes = (uint32_t)g->bo_get_plane_count(bo);
	const uint64_t mod = g->bo_get_modifier(bo);
	desc.modifier = mod == DRM_FORMAT_MOD_INVALID ? DRM_FORMAT_MOD_LINEAR : mod; // GBM_BO_USE_LINEAR
	for (uint32_t p = 0; p < desc.num_planes && p < GS_SHARED_MAX_PLANES; ++p) {
		desc.offsets[p] = g->bo_get_offset(bo, (int)p);
		desc.pitches[p] = g->bo_get_stride_for_plane(bo, (int)p);
	}
	g->bo_destroy(bo);
	if (desc.fds[0] < 0 || !import_dmabuf(d, desc, tex, tex_uv)) {
		if (desc.fds[0] >= 0) {
			close(desc.fds[0]);
		}
		return false;
	}
	// Both plane views keep the fd (exports hand out copies) and the layout GBM chose; it is closed with the image,
	// by whichever view goes last.
	for (gs_texture *t : {*tex, *tex_uv}) {
		t->dmabuf_fd = desc.fds[0];
		t->dmabuf_planes = desc.num_planes;
		t->dmabuf_modifier = desc.modifier;
		memcpy(t->dmabuf_offsets, desc.offsets, sizeof(t->dmabuf_offsets));
		memcpy(t->dmabuf_pitches, desc.pitches, sizeof(t->dmabuf_pitches));
	}
	return true;
}
#endif

static bool create_exportable(gs_device *d, uint32_t width, uint32_t height, enum gs_color_format format,
			      enum gs_shared_layout layout, uint32_t flags, gs_texture **tex, gs_texture **tex_uv)
{
	*tex = nullptr;
	if (tex_uv) {
		*tex_uv = nullptr;
	}
	const VkExternalMemoryHandleTypeFlagBits ht = memory_ext(d) ? export_memory_type(d, layout, flags)
								    : (VkExternalMemoryHandleTypeFlagBits)0;
	const uint32_t owner = external_owner(d, ht);
	gs_texture *uv = nullptr;
	VkImage image = VK_NULL_HANDLE;
#ifndef _WIN32
	if (ht == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT && layout != GS_SHARED_SINGLE) {
		if (!create_gbm_exportable(d, width, height, layout, flags, tex, &uv)) {
			return false;
		}
		image = (*tex)->image;
		set_external_textures(d, *tex, uv, (*tex)->memory, (*tex)->memory_size, ht, layout, flags, false);
	} else
#endif
	{
		vk_image_desc id;
		bool ok = ht && vk_texture_image_info(d, width, height, format, layout, flags, id);
		std::vector<uint64_t> mods;
#ifndef _WIN32
		VkImageDrmFormatModifierListCreateInfoEXT ml = {
			VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT};
		if (ok && ht == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) {
			// Every modifier that fits; the driver picks one (on NVIDIA a block-linear one: LINEAR cannot be
			// rendered to).
			id.ii.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
			mods = usable_modifiers(d, id, false);
			ok = !mods.empty();
			ml.drmFormatModifierCount = (uint32_t)mods.size();
			ml.pDrmFormatModifiers = mods.data();
		}
#endif
		ok = ok && (id.ii.tiling != VK_IMAGE_TILING_OPTIMAL || external_image_ok(d, id, ht, false));
		if (!ok) {
			blog(LOG_ERROR,
			     "[libobs-vulkan] device_texture_create_exportable: format %d layout %d %ux%u unsupported",
			     (int)format, (int)layout, width, height);
			return false;
		}
		VkExternalMemoryImageCreateInfo emi = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
		emi.handleTypes = ht;
		if (id.use_list) {
			id.fl.pNext = &emi;
			id.ii.pNext = &id.fl;
		} else {
			id.ii.pNext = &emi;
		}
#ifndef _WIN32
		if (!mods.empty()) {
			emi.pNext = &ml;
		}
#endif
		if (vkCreateImage(d->dev, &id.ii, nullptr, &image) != VK_SUCCESS) {
			return false;
		}
		VkDeviceSize size = 0;
		VkDeviceMemory memory = allocate_exportable(d, image, VK_NULL_HANDLE, ht, size);
		if (!memory || vkBindImageMemory(d->dev, image, memory, 0) != VK_SUCCESS ||
		    !vk_wrap_image(d, image, width, height, format, layout, flags, tex, &uv)) {
			blog(LOG_ERROR, "[libobs-vulkan] device_texture_create_exportable: allocation failed");
			vkFreeMemory(d->dev, memory, nullptr);
			vkDestroyImage(d->dev, image, nullptr);
			return false;
		}
		set_external_textures(d, *tex, uv, memory, size, ht, layout, flags, false);
	}
	for (gs_texture *t : {*tex, uv}) {
		if (t) {
			t->origin = gs_texture::ORIGIN_EXPORTABLE;
		}
	}
	if (tex_uv) {
		*tex_uv = uv;
	}
	if (!initial_release(d, image, VK_NULL_HANDLE, owner)) {
		gs_texture_destroy(*tex);
		gs_texture_destroy(uv);
		*tex = nullptr;
		if (tex_uv) {
			*tex_uv = nullptr;
		}
		return false;
	}
	d->tracker.set_external(image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, owner);
	return true;
}

static bool export_memory(gs_device *d, VkDeviceMemory memory, VkExternalMemoryHandleTypeFlags allowed,
			  enum gs_handle_type type, gs_shared_texture_desc &desc)
{
	const VkExternalMemoryHandleTypeFlagBits ht = memory_type(type);
	if (!ht || !(allowed & ht)) {
		blog(LOG_ERROR, "[libobs-vulkan] export: not exportable as handle type %d", (int)type);
		return false;
	}
	desc = gs_shared_texture_desc{};
	for (int &fd : desc.fds) {
		fd = -1;
	}
	desc.type = type;
	desc.modifier = GS_SHARED_MODIFIER_INVALID;
	memcpy(desc.device_uuid, d->id.deviceUUID, VK_UUID_SIZE);
#ifdef _WIN32
	VkMemoryGetWin32HandleInfoKHR gi = {VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
	gi.memory = memory;
	gi.handleType = ht;
	HANDLE h = nullptr;
	if (d->interop->get_memory_handle(d->dev, &gi, &h) != VK_SUCCESS || !h) {
		blog(LOG_ERROR, "[libobs-vulkan] export: vkGetMemoryWin32HandleKHR failed");
		return false;
	}
	desc.win32 = h;
	return true;
#else
	VkMemoryGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
	gi.memory = memory;
	gi.handleType = ht;
	int fd = -1;
	if (d->interop->get_memory_fd(d->dev, &gi, &fd) != VK_SUCCESS || fd < 0) {
		blog(LOG_ERROR, "[libobs-vulkan] export: vkGetMemoryFdKHR failed");
		return false;
	}
	desc.fds[0] = fd;
	return true;
#endif
}

#ifndef _WIN32
// A dmabuf's modifier and per-plane layout, as the driver laid out the image; each further plane gets its own fd.
static bool describe_dmabuf(gs_device *d, gs_texture *tex, gs_shared_texture_desc &desc)
{
	VkImageDrmFormatModifierPropertiesEXT mp = {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT};
	if (d->interop->get_modifier(d->dev, tex->image, &mp) != VK_SUCCESS) {
		blog(LOG_ERROR, "[libobs-vulkan] export: vkGetImageDrmFormatModifierPropertiesEXT failed");
		return false;
	}
	const VkFormat format = tex->aspect == VK_IMAGE_ASPECT_COLOR_BIT ? vk_format(tex->format) : tex->image_format;
	const auto mods = format_modifiers(d, format);
	const VkDrmFormatModifierProperties2EXT *m = find_modifier(mods, mp.drmFormatModifier);
	const uint32_t planes = m ? m->drmFormatModifierPlaneCount : 0;
	if (!planes || planes > GS_SHARED_MAX_PLANES) {
		blog(LOG_ERROR, "[libobs-vulkan] export: modifier 0x%016llx has no usable plane count",
		     (unsigned long long)mp.drmFormatModifier);
		return false;
	}
	desc.modifier = mp.drmFormatModifier;
	desc.num_planes = planes;
	static const VkImageAspectFlagBits memory_planes[4] = {VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,
							       VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
							       VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT,
							       VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT};
	for (uint32_t p = 0; p < planes; ++p) {
		VkImageSubresource sub = {(VkImageAspectFlags)memory_planes[p], 0, 0};
		VkSubresourceLayout l = {};
		vkGetImageSubresourceLayout(d->dev, tex->image, &sub, &l);
		desc.offsets[p] = (uint32_t)l.offset;
		desc.pitches[p] = (uint32_t)l.rowPitch;
		if (p > 0) {
			desc.fds[p] = dup_fd(desc.fds[0]);
			if (desc.fds[p] < 0) {
				return false;
			}
		}
	}
	return true;
}
#endif

/* ---- Pitch-linear frame buffer ------------------------------------------------------------------------------- */

static uint32_t plane_bytes(enum gs_shared_layout layout)
{
	return layout == GS_SHARED_P010 ? 2 : 1;
}

static gs_shared_buffer *create_sharedbuf(gs_device *d, uint32_t width, uint32_t height, enum gs_shared_layout layout)
{
	const VkExternalMemoryHandleTypeFlagBits ht = export_buffer_type();
	if (!ht || !memory_ext(d) || layout == GS_SHARED_SINGLE || !width || !height || (width & 1) || (height & 1)) {
		blog(LOG_ERROR, "[libobs-vulkan] device_sharedbuf_create: unsupported (%ux%u, layout %d)", width,
		     height, (int)layout);
		return nullptr;
	}
	auto b = new gs_shared_buffer{d};
	b->width = width;
	b->height = height;
	b->layout = layout;
	// 256-byte rows: CUDA's preferred pitch alignment, and a multiple of every plane's texel size.
	const uint32_t pitch = (width * plane_bytes(layout) + 255) & ~255u;
	b->pitches[0] = b->pitches[1] = pitch;
	b->offsets[0] = 0;
	b->offsets[1] = pitch * height;
	b->size = (VkDeviceSize)pitch * height * 3 / 2;

	VkPhysicalDeviceExternalBufferInfo ebi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO};
	ebi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
		    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	ebi.handleType = ht;
	VkExternalBufferProperties ebp = {VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
	vkGetPhysicalDeviceExternalBufferProperties(d->phys, &ebi, &ebp);
	if (!(ebp.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)) {
		blog(LOG_ERROR, "[libobs-vulkan] device_sharedbuf_create: buffers are not exportable");
		delete b;
		return nullptr;
	}
	VkExternalMemoryBufferCreateInfo emb = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
	emb.handleTypes = ht;
	VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &emb};
	bi.size = b->size;
	bi.usage = ebi.usage;
	VkDeviceSize size = 0;
	if (vkCreateBuffer(d->dev, &bi, nullptr, &b->buffer) != VK_SUCCESS ||
	    !(b->memory = allocate_exportable(d, VK_NULL_HANDLE, b->buffer, ht, size)) ||
	    vkBindBufferMemory(d->dev, b->buffer, b->memory, 0) != VK_SUCCESS ||
	    !initial_release(d, VK_NULL_HANDLE, b->buffer)) {
		blog(LOG_ERROR, "[libobs-vulkan] device_sharedbuf_create: allocation failed");
		vkFreeMemory(d->dev, b->memory, nullptr);
		vkDestroyBuffer(d->dev, b->buffer, nullptr);
		delete b;
		return nullptr;
	}
	b->size = size; // the allocation, which an importer must use
	d->tracker.set_external(b->buffer);
	d->objects.sharedbufs.insert(b);
	return b;
}

/* ---- Exports ------------------------------------------------------------------------------------------------- */

extern "C" {

EXPORT bool device_get_adapter_identity(gs_device_t *device, struct gs_adapter_identity *identity)
{
	*identity = gs_adapter_identity{};
	memcpy(identity->device_uuid, device->id.deviceUUID, VK_UUID_SIZE);
	memcpy(identity->driver_uuid, device->id.driverUUID, VK_UUID_SIZE);
	memcpy(identity->luid, device->id.deviceLUID, VK_LUID_SIZE);
	identity->luid_valid = device->id.deviceLUIDValid != VK_FALSE;
	identity->vendor_id = device->vendor_id;
	identity->device_id = device->device_id;
	return true;
}

EXPORT bool device_shared_texture_supported(gs_device_t *device, enum gs_handle_type type, enum gs_shared_layout layout,
					    enum gs_color_format format, bool import)
{
	const VkExternalMemoryHandleTypeFlagBits ht = memory_type(type);
	if (!ht || !memory_ext(device) ||
	    (!import && ht != export_memory_type(device, layout, 0) &&
	     ht != export_memory_type(device, layout, GS_RENDER_TARGET)) ||
	    (import && d3d11_planar_refused(device, type, layout))) {
		return false;
	}
	vk_image_desc id;
#ifndef _WIN32
	if (ht == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) {
		if (!import && layout != GS_SHARED_SINGLE) {
			return true; // GBM allocates it (export_memory_type checked that GBM has the format)
		}
		if (!dmabuf_image_info(device, 64, 64, format, layout, 0, id)) {
			return false;
		}
		if (!usable_modifiers(device, id, import).empty()) {
			return true;
		}
		return import && layout == GS_SHARED_SINGLE && unorm_format(format) != format &&
		       dmabuf_image_info(device, 64, 64, unorm_format(format), layout, 0, id) &&
		       !usable_modifiers(device, id, import).empty();
	}
#endif
	if (!vk_texture_image_info(device, 64, 64, format, layout, 0, id)) {
		return false;
	}
	if (external_image_ok(device, id, ht, import)) {
		return true;
	}
	// D3D11 imports fall back to the UNORM format when MUTABLE_FORMAT is refused (import_texture).
	return import && is_d3d11(type) && layout == GS_SHARED_SINGLE && unorm_format(format) != format &&
	       vk_texture_image_info(device, 64, 64, unorm_format(format), layout, 0, id) &&
	       external_image_ok(device, id, ht, import);
}

EXPORT bool device_texture_create_exportable(gs_device_t *device, uint32_t width, uint32_t height,
					     enum gs_color_format format, enum gs_shared_layout layout, uint32_t flags,
					     gs_texture_t **tex, gs_texture_t **tex_uv)
{
	return create_exportable(device, width, height, format, layout, flags, tex, tex_uv);
}

EXPORT bool device_texture_export(gs_device_t *device, gs_texture_t *tex, enum gs_handle_type type,
				  struct gs_shared_texture_desc *desc)
{
	if (!tex->export_types) {
		blog(LOG_ERROR,
		     "[libobs-vulkan] device_texture_export: not created by device_texture_create_exportable");
		return false;
	}
#ifndef _WIN32
	if (tex->dmabuf_fd >= 0) {
		// GBM memory: copies of its fd, its layout.
		if (type != GS_HANDLE_DMABUF) {
			blog(LOG_ERROR, "[libobs-vulkan] export: not exportable as handle type %d", (int)type);
			return false;
		}
		*desc = gs_shared_texture_desc{};
		for (int &fd : desc->fds) {
			fd = -1;
		}
		desc->type = type;
		memcpy(desc->device_uuid, device->id.deviceUUID, VK_UUID_SIZE);
		desc->modifier = tex->dmabuf_modifier;
		desc->num_planes = tex->dmabuf_planes;
		for (uint32_t p = 0; p < tex->dmabuf_planes; ++p) {
			desc->fds[p] = dup_fd(tex->dmabuf_fd);
			desc->offsets[p] = tex->dmabuf_offsets[p];
			desc->pitches[p] = tex->dmabuf_pitches[p];
			if (desc->fds[p] < 0) {
				for (int &fd : desc->fds) {
					if (fd >= 0) {
						close(fd);
						fd = -1;
					}
				}
				return false;
			}
		}
	} else
#endif
		if (!export_memory(device, tex->memory, tex->export_types, type, *desc)) {
		return false;
	}
	const bool uv = tex->aspect == VK_IMAGE_ASPECT_PLANE_1_BIT;
	desc->width = uv ? tex->width * 2 : tex->width;
	desc->height = uv ? tex->height * 2 : tex->height;
	desc->format = tex->layout == GS_SHARED_SINGLE ? tex->format : GS_UNKNOWN;
	desc->layout = tex->layout;
	desc->flags = tex->create_flags;
	desc->size = tex->memory_size;
#ifndef _WIN32
	if (type == GS_HANDLE_DMABUF && tex->dmabuf_fd < 0 && !describe_dmabuf(device, tex, *desc)) {
		for (int &fd : desc->fds) {
			if (fd >= 0) {
				close(fd);
				fd = -1;
			}
		}
		return false;
	}
#endif
	return true;
}

EXPORT bool device_texture_import(gs_device_t *device, const struct gs_shared_texture_desc *desc, gs_texture_t **tex,
				  gs_texture_t **tex_uv)
{
	return import_texture(device, *desc, tex, tex_uv);
}

EXPORT gs_sharedbuf_t *device_sharedbuf_create(gs_device_t *device, uint32_t width, uint32_t height,
					       enum gs_shared_layout layout)
{
	return create_sharedbuf(device, width, height, layout);
}

EXPORT void gs_sharedbuf_destroy(gs_sharedbuf_t *buf)
{
	if (!buf) {
		return;
	}
	gs_device *d = buf->device;
	d->objects.sharedbufs.erase(buf);
	d->tracker.forget(buf->buffer);
	d->frames.defer_destroy([dev = d->dev, buffer = buf->buffer, memory = buf->memory] {
		vkDestroyBuffer(dev, buffer, nullptr);
		vkFreeMemory(dev, memory, nullptr);
	});
	delete buf;
}

EXPORT bool device_sharedbuf_copy_texture(gs_device_t *device, gs_sharedbuf_t *buf, gs_texture_t *tex_y)
{
	const VkFormat want = buf->layout == GS_SHARED_P010 ? VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16
							    : VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
	if (tex_y->image_format != want || tex_y->aspect != VK_IMAGE_ASPECT_PLANE_0_BIT || tex_y->width != buf->width ||
	    tex_y->height != buf->height) {
		blog(LOG_ERROR,
		     "[libobs-vulkan] device_sharedbuf_copy_texture: needs the Y plane of a %ux%u %s texture",
		     buf->width, buf->height, buf->layout == GS_SHARED_P010 ? "P010" : "NV12");
		return false;
	}
	gs_device *d = device;
	VkCommandBuffer cmd = vk_cmd(d);
	vk_barriers b;
	d->tracker.use_image(
		tex_y->image, d->graphics.family,
		{VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL}, b);
	d->tracker.use_buffer(buf->buffer, d->graphics.family,
			      {VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED},
			      b);
	vk_record_barriers(cmd, b);
	const uint32_t bpp = plane_bytes(buf->layout);
	VkBufferImageCopy r[2] = {};
	r[0].bufferOffset = buf->offsets[0];
	r[0].bufferRowLength = buf->pitches[0] / bpp;
	r[0].imageSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1};
	r[0].imageExtent = {buf->width, buf->height, 1};
	r[1].bufferOffset = buf->offsets[1];
	r[1].bufferRowLength = buf->pitches[1] / (2 * bpp);
	r[1].imageSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1};
	r[1].imageExtent = {buf->width / 2, buf->height / 2, 1};
	vkCmdCopyImageToBuffer(cmd, tex_y->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf->buffer, 2, r);
	return true;
}

EXPORT bool device_sharedbuf_export(gs_device_t *device, gs_sharedbuf_t *buf, enum gs_handle_type type,
				    struct gs_shared_texture_desc *desc)
{
	if (!export_memory(device, buf->memory, export_buffer_type(), type, *desc)) {
		return false;
	}
	desc->width = buf->width;
	desc->height = buf->height;
	desc->format = GS_UNKNOWN;
	desc->layout = buf->layout;
	desc->size = buf->size;
	desc->num_planes = 2;
	for (int i = 0; i < 2; ++i) {
		desc->offsets[i] = buf->offsets[i];
		desc->pitches[i] = buf->pitches[i];
	}
	return true;
}

/* ---- Timelines ----------------------------------------------------------------------------------------------- */

static VkSemaphore create_semaphore(gs_device *d, uint64_t initial, VkExternalSemaphoreHandleTypeFlags export_types)
{
	VkExportSemaphoreCreateInfo ex = {VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
	ex.handleTypes = export_types;
	VkSemaphoreTypeCreateInfo ti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, export_types ? &ex : nullptr};
	ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
	ti.initialValue = initial;
	VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti};
	VkSemaphore sem = VK_NULL_HANDLE;
	vkCreateSemaphore(d->dev, &si, nullptr, &sem);
	return sem;
}

static bool timeline_handle_ok(gs_device *d, VkExternalSemaphoreHandleTypeFlagBits ht, bool import)
{
	if (!ht || !semaphore_ext(d)) {
		return false;
	}
	VkSemaphoreTypeCreateInfo ti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
	ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
	VkPhysicalDeviceExternalSemaphoreInfo info = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO, &ti};
	info.handleType = ht;
	VkExternalSemaphoreProperties props = {VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
	vkGetPhysicalDeviceExternalSemaphoreProperties(d->phys, &info, &props);
	return (props.externalSemaphoreFeatures & (import ? VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT
							  : VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT)) != 0;
}

static VkExternalSemaphoreHandleTypeFlagBits timeline_export_type()
{
#ifdef _WIN32
	return VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
	return VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif
}

EXPORT gs_timeline_t *device_timeline_create(gs_device_t *device, uint64_t initial_value)
{
	const VkExternalSemaphoreHandleTypeFlagBits ht = timeline_export_type();
	// Not exportable when the driver cannot: still a working timeline on this device, gs_timeline_export fails.
	const bool exportable = timeline_handle_ok(device, ht, false);
	VkSemaphore sem = create_semaphore(device, initial_value, exportable ? ht : 0);
	if (!sem) {
		blog(LOG_ERROR, "[libobs-vulkan] device_timeline_create: vkCreateSemaphore failed");
		return nullptr;
	}
	auto t = new gs_timeline{device};
	t->sem = sem;
	t->exportable = exportable;
	t->last_value = initial_value;
	device->objects.timelines.insert(t);
	return t;
}

EXPORT gs_timeline_t *device_timeline_import(gs_device_t *device, const struct gs_shared_handle *handle)
{
	const VkExternalSemaphoreHandleTypeFlagBits ht = semaphore_type(handle->type);
	if (!timeline_handle_ok(device, ht, true)) {
		blog(LOG_ERROR, "[libobs-vulkan] device_timeline_import: handle type %d unsupported",
		     (int)handle->type);
		return nullptr;
	}
#ifdef _WIN32
	VkSemaphore sem = create_semaphore(device, 0, 0);
	if (!sem) {
		return nullptr;
	}
	VkImportSemaphoreWin32HandleInfoKHR ii = {VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
	ii.semaphore = sem;
	ii.handleType = ht;
	ii.handle = (HANDLE)handle->win32;
	if (device->interop->import_semaphore_handle(device->dev, &ii) != VK_SUCCESS) {
		blog(LOG_ERROR, "[libobs-vulkan] device_timeline_import: vkImportSemaphoreWin32HandleKHR failed");
		vkDestroySemaphore(device->dev, sem, nullptr);
		return nullptr;
	}
	auto t = new gs_timeline{device};
	t->sem = sem;
	t->imported = true;
	device->objects.timelines.insert(t);
	return t;
#else
	const int copy = dup_fd(handle->fd); // Vulkan takes over the fd it imports; an import borrows the handle
	VkSemaphore sem = copy >= 0 ? create_semaphore(device, 0, 0) : VK_NULL_HANDLE;
	if (!sem) {
		if (copy >= 0) {
			close(copy);
		}
		return nullptr;
	}
	VkImportSemaphoreFdInfoKHR ii = {VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
	ii.semaphore = sem;
	ii.handleType = ht;
	ii.fd = copy;
	if (device->interop->import_semaphore_fd(device->dev, &ii) != VK_SUCCESS) {
		blog(LOG_ERROR, "[libobs-vulkan] device_timeline_import: vkImportSemaphoreFdKHR failed");
		close(copy);
		vkDestroySemaphore(device->dev, sem, nullptr);
		return nullptr;
	}
	auto t = new gs_timeline{device};
	t->sem = sem;
	t->imported = true;
	uint64_t v = 0;
	if (vkGetSemaphoreCounterValue(device->dev, sem, &v) == VK_SUCCESS) {
		t->last_value = v;
	}
	device->objects.timelines.insert(t);
	return t;
#endif
}

EXPORT bool device_timeline_export(gs_device_t *device, gs_timeline_t *timeline, enum gs_handle_type type,
				   struct gs_shared_handle *handle)
{
	const VkExternalSemaphoreHandleTypeFlagBits ht = semaphore_type(type);
	*handle = gs_shared_handle{GS_HANDLE_NONE, nullptr, -1};
#ifdef _WIN32
	if (!timeline->exportable || ht != VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT) {
		blog(LOG_ERROR, "[libobs-vulkan] device_timeline_export: not exportable as handle type %d", (int)type);
		return false;
	}
	VkSemaphoreGetWin32HandleInfoKHR gi = {VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR};
	gi.semaphore = timeline->sem;
	gi.handleType = ht;
	HANDLE h = nullptr;
	if (device->interop->get_semaphore_handle(device->dev, &gi, &h) != VK_SUCCESS || !h) {
		blog(LOG_ERROR, "[libobs-vulkan] device_timeline_export: vkGetSemaphoreWin32HandleKHR failed");
		return false;
	}
	handle->type = type;
	handle->win32 = h;
	return true;
#else
	if (!timeline->exportable || ht != VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT) {
		blog(LOG_ERROR, "[libobs-vulkan] device_timeline_export: not exportable as handle type %d", (int)type);
		return false;
	}
	VkSemaphoreGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
	gi.semaphore = timeline->sem;
	gi.handleType = ht;
	int fd = -1;
	if (device->interop->get_semaphore_fd(device->dev, &gi, &fd) != VK_SUCCESS || fd < 0) {
		blog(LOG_ERROR, "[libobs-vulkan] device_timeline_export: vkGetSemaphoreFdKHR failed");
		return false;
	}
	handle->type = type;
	handle->fd = fd;
	return true;
#endif
}

static void drop_pending(std::vector<VkSemaphoreSubmitInfo> &v, VkSemaphore sem)
{
	for (size_t i = v.size(); i-- > 0;) {
		if (v[i].semaphore == sem) {
			v.erase(v.begin() + (ptrdiff_t)i);
		}
	}
}

EXPORT void gs_timeline_destroy(gs_timeline_t *timeline)
{
	if (!timeline) {
		return;
	}
	gs_device *d = timeline->device;
	d->objects.timelines.erase(timeline);
	drop_pending(d->pending.waits, timeline->sem);
	drop_pending(d->pending.signals, timeline->sem);
	d->frames.defer_destroy([dev = d->dev, sem = timeline->sem] { vkDestroySemaphore(dev, sem, nullptr); });
	delete timeline;
}

EXPORT bool device_timeline_signal(gs_device_t *device, gs_timeline_t *timeline, uint64_t value)
{
	VkSemaphoreSubmitInfo s = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
	s.semaphore = timeline->sem;
	s.value = value;
	s.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	device->pending.signals.push_back(s);
	vk_frame_cmd(device); // a submission even when nothing is recorded
	// After a device loss the new timeline starts here, so waiters on it do not hang (vk_timeline_recreate).
	timeline->last_value = std::max(timeline->last_value, value);
	return vk_flush(device);
}

EXPORT bool device_timeline_wait(gs_device_t *device, gs_timeline_t *timeline, uint64_t value)
{
	// Work recorded before the wait does not wait.
	if (device->frames.recording && !vk_flush(device)) {
		return false;
	}
	VkSemaphoreSubmitInfo s = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
	s.semaphore = timeline->sem;
	s.value = value;
	s.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	device->pending.waits.push_back(s);
	return true;
}

EXPORT uint64_t device_timeline_get_value(gs_device_t *device, gs_timeline_t *timeline)
{
	uint64_t v = 0;
	if (vk_check(device, vkGetSemaphoreCounterValue(device->dev, timeline->sem, &v)) == VK_SUCCESS) {
		timeline->last_value = std::max(timeline->last_value, v);
	}
	return v;
}

EXPORT int device_timeline_wait_cpu(gs_device_t *device, gs_timeline_t *timeline, uint64_t value, uint64_t timeout_ns)
{
	VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
	wi.semaphoreCount = 1;
	wi.pSemaphores = &timeline->sem;
	wi.pValues = &value;
	const VkResult r = vk_check(device, vkWaitSemaphores(device->dev, &wi, timeout_ns));
	if (r == VK_SUCCESS) {
		timeline->last_value = std::max(timeline->last_value, value);
	}
	return r == VK_SUCCESS ? 0 : r == VK_TIMEOUT ? ETIMEDOUT : -1;
}

EXPORT bool device_timeline_signal_cpu(gs_device_t *device, gs_timeline_t *timeline, uint64_t value)
{
	uint64_t cur = 0;
	if (vkGetSemaphoreCounterValue(device->dev, timeline->sem, &cur) != VK_SUCCESS || value <= cur) {
		return false; // a timeline only moves forward
	}
	VkSemaphoreSignalInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
	si.semaphore = timeline->sem;
	si.value = value;
	if (vk_check(device, vkSignalSemaphore(device->dev, &si)) != VK_SUCCESS) {
		return false;
	}
	timeline->last_value = value;
	return true;
}

/* ---- Sync files (Linux) -------------------------------------------------------------------------------------- */

#ifndef _WIN32
static bool sync_fd_ok(gs_device *d)
{
	if (!d->ext_semaphore_fd) {
		return false;
	}
	VkPhysicalDeviceExternalSemaphoreInfo info = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
	info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
	VkExternalSemaphoreProperties props = {VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
	vkGetPhysicalDeviceExternalSemaphoreProperties(d->phys, &info, &props);
	const VkExternalSemaphoreFeatureFlags need = VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT |
						     VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
	return (props.externalSemaphoreFeatures & need) == need;
}

// A sync_file fd signalled once all work submitted so far has finished (a binary semaphore signalled by a
// submission after it), or -1.
static int export_sync_file(gs_device *d)
{
	if (!sync_fd_ok(d)) {
		blog(LOG_ERROR, "[libobs-vulkan] sync file export: SYNC_FD semaphores unsupported");
		return -1;
	}
	VkExportSemaphoreCreateInfo ex = {VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
	ex.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
	VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ex};
	VkSemaphore sem = VK_NULL_HANDLE;
	if (vkCreateSemaphore(d->dev, &si, nullptr, &sem) != VK_SUCCESS) {
		return -1;
	}
	VkSemaphoreSubmitInfo s = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
	s.semaphore = sem;
	s.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; // with every earlier submission in its first scope
	d->pending.signals.push_back(s);
	vk_frame_cmd(d); // a submission even when nothing is recorded
	int fd = -1;
	if (vk_flush(d)) {
		VkSemaphoreGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
		gi.semaphore = sem;
		gi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
		if (d->interop->get_semaphore_fd(d->dev, &gi, &fd) != VK_SUCCESS) {
			blog(LOG_ERROR, "[libobs-vulkan] sync file export: vkGetSemaphoreFdKHR failed");
			fd = -1;
		}
	}
	// Destroyed once the submission that signals it has finished.
	d->frames.defer_destroy([dev = d->dev, sem] { vkDestroySemaphore(dev, sem, nullptr); });
	return fd;
}

// The next submission waits on the GPU for the sync_file (a copy of `fd`); work recorded before does not wait.
static bool wait_sync_file(gs_device *d, int fd)
{
	if (fd < 0) {
		return true; // a signalled sync_file (as Vulkan's SYNC_FD imports treat -1)
	}
	if (!sync_fd_ok(d)) {
		blog(LOG_ERROR, "[libobs-vulkan] sync file wait: SYNC_FD semaphores unsupported");
		return false;
	}
	if (d->frames.recording && !vk_flush(d)) {
		return false;
	}
	VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	VkSemaphore sem = VK_NULL_HANDLE;
	const int copy = dup_fd(fd);
	if (copy < 0 || vkCreateSemaphore(d->dev, &si, nullptr, &sem) != VK_SUCCESS) {
		if (copy >= 0) {
			close(copy);
		}
		return false;
	}
	VkImportSemaphoreFdInfoKHR ii = {VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
	ii.semaphore = sem;
	ii.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT; // required for SYNC_FD
	ii.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
	ii.fd = copy;
	if (d->interop->import_semaphore_fd(d->dev, &ii) != VK_SUCCESS) {
		blog(LOG_ERROR, "[libobs-vulkan] sync file wait: vkImportSemaphoreFdKHR failed");
		close(copy);
		vkDestroySemaphore(d->dev, sem, nullptr);
		return false;
	}
	VkSemaphoreSubmitInfo w = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
	w.semaphore = sem;
	w.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	d->pending.waits.push_back(w);
	// Begins the submission that waits, so the semaphore goes once that one has finished.
	vk_frame_cmd(d);
	d->frames.defer_destroy([dev = d->dev, sem] { vkDestroySemaphore(dev, sem, nullptr); });
	return true;
}

// Sync files are Linux-only: not exported on Windows, where libobs's gs_sync_file_* then return false.
EXPORT bool device_sync_file_export(gs_device_t *device, struct gs_shared_handle *handle)
{
	*handle = gs_shared_handle{GS_HANDLE_NONE, nullptr, -1};
	const int fd = export_sync_file(device);
	if (fd < 0) {
		return false;
	}
	handle->type = GS_HANDLE_SYNC_FD;
	handle->fd = fd;
	return true;
}

EXPORT bool device_sync_file_wait(gs_device_t *device, const struct gs_shared_handle *handle)
{
	if (handle->type != GS_HANDLE_SYNC_FD) {
		blog(LOG_ERROR, "[libobs-vulkan] device_sync_file_wait: not a sync file (type %d)", (int)handle->type);
		return false;
	}
	return wait_sync_file(device, handle->fd);
}
#endif

// Test hook: copies the first `size` bytes of `buf` to `dst` (after all work recorded so far) and waits for them.
EXPORT bool obs_vulkan_test_read_sharedbuf(gs_sharedbuf_t *buf, void *dst, uint64_t size)
{
	gs_device *d = buf->device;
	if (size > buf->size) {
		return false;
	}
	VkBuffer host;
	VmaAllocation alloc;
	void *mapped;
	if (!vk_create_host_buffer(d, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
				   VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT, host, alloc, mapped)) {
		return false;
	}
	VkCommandBuffer cmd = vk_cmd(d);
	vk_barriers b;
	d->tracker.use_buffer(buf->buffer, d->graphics.family,
			      {VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED},
			      b);
	vk_record_barriers(cmd, b);
	VkBufferCopy r = {0, 0, size};
	vkCmdCopyBuffer(cmd, buf->buffer, host, 1, &r);
	// Host reads after the copy: the submission's completion (timeline wait) plus this barrier make it visible.
	VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
	mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
	mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
	mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
	mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
	VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
	di.memoryBarrierCount = 1;
	di.pMemoryBarriers = &mb;
	vkCmdPipelineBarrier2(cmd, &di);
	bool ok = vk_flush(d);
	const uint64_t value = d->graphics.next_value - 1;
	VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
	wi.semaphoreCount = 1;
	wi.pSemaphores = &d->graphics.timeline;
	wi.pValues = &value;
	ok = ok && vk_check(d, vkWaitSemaphores(d->dev, &wi, UINT64_MAX)) == VK_SUCCESS;
	if (ok) {
		vmaInvalidateAllocation(d->vma, alloc, 0, VK_WHOLE_SIZE);
		memcpy(dst, mapped, (size_t)size);
	}
	d->frames.defer_destroy([vma = d->vma, host, alloc] { vmaDestroyBuffer(vma, host, alloc); });
	return ok;
}

/* ---- Linux: the dmabuf and explicit sync exports of graphics.h (linux-pipewire) ----------------------------- */

#if defined(__linux__) || defined(__FreeBSD__) || defined(__DragonFly__)

// One of gs_sync_create* (explicit sync, as libobs-opengl's EGL fences): a sync_file, or -1 when already signalled.
struct gs_sync {
	int fd = -1;
};

// The render node (for DRM syncobjs): GBM's, opened on first use.
static int drm_fd(gs_device *d)
{
	vk_interop *g = gbm_of(d);
	return g ? g->gbm_fd : -1;
}

EXPORT struct gs_texture *device_texture_create_from_dmabuf(gs_device_t *device, unsigned int width,
							    unsigned int height, uint32_t drm_format,
							    enum gs_color_format color_format, uint32_t n_planes,
							    const int *fds, const uint32_t *strides,
							    const uint32_t *offsets, const uint64_t *modifiers)
{
	if (n_planes == 0 || n_planes > GS_SHARED_MAX_PLANES) {
		return nullptr;
	}
	if (drm_format == DRM_FORMAT_NV12 || drm_format == DRM_FORMAT_P010) {
		// One gs_texture cannot hold both planes (gs_texture_import gives both plane views).
		blog(LOG_ERROR, "[libobs-vulkan] device_texture_create_from_dmabuf: NV12/P010 need gs_texture_import");
		return nullptr;
	}
	gs_shared_texture_desc desc = {};
	desc.type = GS_HANDLE_DMABUF;
	desc.width = width;
	desc.height = height;
	desc.format = color_format;
	desc.layout = GS_SHARED_SINGLE;
	desc.num_planes = n_planes;
	desc.modifier = modifiers ? modifiers[0] : GS_SHARED_MODIFIER_INVALID;
	for (uint32_t p = 0; p < GS_SHARED_MAX_PLANES; ++p) {
		desc.fds[p] = p < n_planes ? fds[p] : -1;
		desc.offsets[p] = p < n_planes ? offsets[p] : 0;
		desc.pitches[p] = p < n_planes ? strides[p] : 0;
	}
	if (drm_fourcc(color_format, GS_SHARED_SINGLE) != drm_format) {
		blog(LOG_WARNING,
		     "[libobs-vulkan] device_texture_create_from_dmabuf: DRM format 0x%08x is not the one of gs format %d "
		     "(0x%08x); imported as the gs format",
		     drm_format, (int)color_format, drm_fourcc(color_format, GS_SHARED_SINGLE));
	}
	gs_texture *tex = nullptr;
	return import_dmabuf(device, desc, &tex, nullptr) ? tex : nullptr;
}

// The DRM formats a dmabuf can be imported as (with an explicit modifier; implicit modifiers are unsupported).
EXPORT bool device_query_dmabuf_capabilities(gs_device_t *device, enum gs_dmabuf_flags *dmabuf_flags,
					     uint32_t **drm_formats, size_t *n_formats)
{
	*dmabuf_flags = GS_DMABUF_FLAG_NONE;
	*drm_formats = nullptr;
	*n_formats = 0;
	if (!device->ext_dmabuf) {
		return false;
	}
	static const enum gs_color_format formats[] = {GS_BGRA,   GS_BGRX, GS_RGBA, GS_R10G10B10A2, GS_RGBA16F,
						       GS_RGBA16, GS_R8,   GS_R8G8, GS_R16,         GS_RG16};
	std::vector<uint32_t> out;
	for (enum gs_color_format f : formats) {
		if (device_shared_texture_supported(device, GS_HANDLE_DMABUF, GS_SHARED_SINGLE, f, true)) {
			out.push_back(drm_fourcc(f, GS_SHARED_SINGLE));
		}
	}
	for (enum gs_shared_layout l : {GS_SHARED_NV12, GS_SHARED_P010}) {
		if (device_shared_texture_supported(device, GS_HANDLE_DMABUF, l, GS_UNKNOWN, true)) {
			out.push_back(drm_fourcc(GS_UNKNOWN, l));
		}
	}
	*drm_formats = (uint32_t *)bmemdup(out.data(), out.size() * sizeof(uint32_t));
	*n_formats = out.size();
	return true;
}

static bool format_of_fourcc(uint32_t fourcc, enum gs_color_format &format, enum gs_shared_layout &layout)
{
	static const enum gs_color_format formats[] = {GS_BGRA,   GS_BGRX, GS_RGBA, GS_R10G10B10A2, GS_RGBA16F,
						       GS_RGBA16, GS_R8,   GS_R8G8, GS_R16,         GS_RG16};
	for (enum gs_shared_layout l : {GS_SHARED_NV12, GS_SHARED_P010}) {
		if (drm_fourcc(GS_UNKNOWN, l) == fourcc) {
			format = GS_UNKNOWN;
			layout = l;
			return true;
		}
	}
	for (enum gs_color_format f : formats) {
		if (drm_fourcc(f, GS_SHARED_SINGLE) == fourcc) {
			format = f;
			layout = GS_SHARED_SINGLE;
			return true;
		}
	}
	return false;
}

// The modifiers a dmabuf of `drm_format` can be imported with, for sampling.
EXPORT bool device_query_dmabuf_modifiers_for_format(gs_device_t *device, uint32_t drm_format, uint64_t **modifiers,
						     size_t *n_modifiers)
{
	*modifiers = nullptr;
	*n_modifiers = 0;
	enum gs_color_format format;
	enum gs_shared_layout layout;
	vk_image_desc id;
	if (!device->ext_dmabuf || !format_of_fourcc(drm_format, format, layout) ||
	    !dmabuf_image_info(device, 64, 64, format, layout, 0, id)) {
		return false;
	}
	std::vector<uint64_t> mods = usable_modifiers(device, id, true);
	if (mods.empty() && layout == GS_SHARED_SINGLE && unorm_format(format) != format &&
	    dmabuf_image_info(device, 64, 64, unorm_format(format), layout, 0, id)) {
		mods = usable_modifiers(device, id, true);
	}
	*modifiers = (uint64_t *)bmemdup(mods.data(), mods.size() * sizeof(uint64_t));
	*n_modifiers = mods.size();
	return true;
}

// X11 pixmaps (GLX): X11 is not supported.
EXPORT struct gs_texture *device_texture_create_from_pixmap(gs_device_t *, uint32_t, uint32_t, enum gs_color_format,
							    uint32_t, void *)
{
	return nullptr;
}

EXPORT bool device_query_sync_capabilities(gs_device_t *device)
{
	return sync_fd_ok(device) && drm_fd(device) >= 0;
}

EXPORT gs_sync_t *device_sync_create(gs_device_t *device)
{
	const int fd = export_sync_file(device);
	if (fd < 0) {
		return nullptr;
	}
	auto s = new gs_sync;
	s->fd = fd;
	return s;
}

// Waits on the CPU until the point has a fence (the producer submitted its work), then takes that fence as a
// sync_file: a Vulkan semaphore cannot import a DRM syncobj itself (spike D).
EXPORT gs_sync_t *device_sync_create_from_syncobj_timeline_point(gs_device_t *device, int syncobj_fd,
								 uint64_t timeline_point)
{
	const int fd = drm_fd(device);
	uint32_t handle = 0, tmp = 0;
	if (fd < 0 || drmSyncobjFDToHandle(fd, syncobj_fd, &handle) != 0) {
		blog(LOG_ERROR, "[libobs-vulkan] device_sync_create_from_syncobj_timeline_point: not a syncobj of the "
				"render node");
		return nullptr;
	}
	gs_sync *s = nullptr;
	int sync_file = -1;
	if (drmSyncobjTimelineWait(fd, &handle, &timeline_point, 1, INT64_MAX, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE,
				   nullptr) == 0 &&
	    drmSyncobjCreate(fd, 0, &tmp) == 0 && drmSyncobjTransfer(fd, tmp, 0, handle, timeline_point, 0) == 0 &&
	    drmSyncobjExportSyncFile(fd, tmp, &sync_file) == 0) {
		s = new gs_sync;
		s->fd = sync_file;
	} else {
		blog(LOG_ERROR, "[libobs-vulkan] device_sync_create_from_syncobj_timeline_point: point %llu not usable",
		     (unsigned long long)timeline_point);
	}
	if (tmp) {
		drmSyncobjDestroy(fd, tmp);
	}
	drmSyncobjDestroy(fd, handle);
	return s;
}

EXPORT void device_sync_destroy(gs_device_t *, gs_sync_t *sync_ptr)
{
	auto sync = (gs_sync *)sync_ptr;
	if (!sync) {
		return;
	}
	if (sync->fd >= 0) {
		close(sync->fd);
	}
	delete sync;
}

// Attaches the sync's fence to the point (the consumer's release point): sync_file into a binary syncobj, then
// transferred to the timeline.
EXPORT bool device_sync_export_syncobj_timeline_point(gs_device_t *device, gs_sync_t *sync_ptr, int syncobj_fd,
						      uint64_t timeline_point)
{
	auto sync = (gs_sync *)sync_ptr;
	const int fd = drm_fd(device);
	uint32_t handle = 0, tmp = 0;
	if (fd < 0 || drmSyncobjFDToHandle(fd, syncobj_fd, &handle) != 0) {
		return false;
	}
	bool ok = drmSyncobjCreate(fd, sync->fd < 0 ? DRM_SYNCOBJ_CREATE_SIGNALED : 0, &tmp) == 0 &&
		  (sync->fd < 0 || drmSyncobjImportSyncFile(fd, tmp, sync->fd) == 0) &&
		  drmSyncobjTransfer(fd, handle, timeline_point, tmp, 0, 0) == 0;
	if (tmp) {
		drmSyncobjDestroy(fd, tmp);
	}
	drmSyncobjDestroy(fd, handle);
	return ok;
}

EXPORT bool device_sync_signal_syncobj_timeline_point(gs_device_t *device, int syncobj_fd, uint64_t timeline_point)
{
	const int fd = drm_fd(device);
	uint32_t handle = 0;
	if (fd < 0 || drmSyncobjFDToHandle(fd, syncobj_fd, &handle) != 0) {
		return false;
	}
	const bool ok = drmSyncobjTimelineSignal(fd, &handle, &timeline_point, 1) == 0;
	drmSyncobjDestroy(fd, handle);
	return ok;
}

// The next submission waits for the sync on the GPU.
EXPORT bool device_sync_wait(gs_device_t *device, gs_sync_t *sync)
{
	return wait_sync_file(device, ((gs_sync *)sync)->fd);
}

#endif

/* ---- Legacy Windows exports ---------------------------------------------------------------------------------- */

#ifdef _WIN32

EXPORT bool device_shared_texture_available(void)
{
	return shared_textures;
}

EXPORT gs_texture_t *device_texture_open_shared(gs_device_t *device, uint32_t handle)
{
	return open_legacy(device, handle, false);
}

EXPORT gs_texture_t *device_texture_open_nt_shared(gs_device_t *device, uint32_t handle)
{
	return open_legacy(device, handle, true);
}

// The handle a texture was opened from (as D3D11 for opened textures). Textures created by this backend have none:
// Vulkan cannot create D3D11 share handles, so GS_SHARED_TEX / GS_SHARED_KM_TEX do not apply.
EXPORT uint32_t device_texture_get_shared_handle(gs_texture_t *tex)
{
	return tex->shared_handle;
}

// Flushes recorded work, then acquires `key` in a submission of its own, so the call returns once the texture is this
// device's (as IDXGIKeyedMutex::AcquireSync). Work recorded afterwards runs while it holds the mutex;
// device_texture_release_sync releases it at the end of the submission holding that work.
//
// The Vulkan acquire never times out: a finite timeout first waits on the CPU for `key` through the D3D11 probe's
// view of the same mutex and hands it straight back, then Vulkan acquires it with an infinite timeout. A Vulkan
// acquire with a timeout is not reliable: NVIDIA (621.24) returns VK_TIMEOUT, AMD (26.11.2) VK_SUCCESS without
// the mutex. Between the hand-back and the Vulkan acquire only a third party acquiring the same key could intervene.
EXPORT int device_texture_acquire_sync(gs_texture_t *tex, uint64_t key, uint32_t ms)
{
	gs_device *d = tex->device;
	if (!tex->keyed_mutex) {
		return -1;
	}
	if (ms != INFINITE) {
		IDXGIKeyedMutex *km = (IDXGIKeyedMutex *)tex->km_probe;
		if (!km) {
			return -1;
		}
		const HRESULT hr = km->AcquireSync(key, ms);
		if (hr == (HRESULT)WAIT_TIMEOUT) {
			return ETIMEDOUT;
		}
		if (FAILED(hr) || FAILED(km->ReleaseSync(key))) {
			return -1;
		}
	}
	if (d->frames.recording && !vk_flush(d)) {
		return -1;
	}
	vk_keyed_mutex_info km;
	km.mem.push_back(tex->memory);
	km.keys.push_back(key);
	km.timeouts.push_back(INFINITE);
	km.info.acquireCount = 1;
	km.info.pAcquireSyncs = km.mem.data();
	km.info.pAcquireKeys = km.keys.data();
	km.info.pAcquireTimeouts = km.timeouts.data();
	vk_submit_extra extra;
	extra.pnext = &km.info;
	d->frames.begin();
	return d->frames.submit(d->graphics, nullptr, nullptr, &extra) ? 0 : -1;
}

EXPORT int device_texture_release_sync(gs_texture_t *tex, uint64_t key)
{
	gs_device *d = tex->device;
	if (!tex->keyed_mutex) {
		return -1;
	}
	d->km_release_mem.push_back(tex->memory);
	d->km_release_key.push_back(key);
	vk_frame_cmd(d); // a submission even when nothing is recorded
	return vk_flush(d) ? 0 : -1;
}

#endif
}

/* ---- Device-loss rebuild (vulkan-rebuild.cpp) ------------------------------------------------------------------- */

bool vk_interop_recreate_texture(gs_device *d, gs_texture *t)
{
	const bool uv = t->aspect == VK_IMAGE_ASPECT_PLANE_1_BIT;
	const uint32_t w = uv ? t->width * 2 : t->width, h = uv ? t->height * 2 : t->height;
	if (t->origin == gs_texture::ORIGIN_EXPORTABLE) {
		// New memory, so new handles: exported ones are dead, consumers export again after device_loss_rebuild.
		const enum gs_color_format format = t->layout == GS_SHARED_SINGLE ? t->format : GS_UNKNOWN;
		const enum gs_shared_layout layout = t->layout;
		const uint32_t flags = t->create_flags;
		return vk_texture_adopt(d, t, [&](gs_texture **a, gs_texture **b) {
			return create_exportable(d, w, h, format, layout, flags, a, b);
		});
	}
	// Imported: the memory belongs to another API or process, and an import borrows its handle, so none is kept. A
	// legacy KMT handle is a name, opened again as D3D11 does. Anything else (or a failed open) gets a placeholder of
	// the same size and format with undefined contents; its owner imports again from its device_loss_rebuild.
#ifdef _WIN32
	if (t->shared_handle != invalid_handle && !t->shared_nt) {
		const uint32_t handle = t->shared_handle;
		if (vk_texture_adopt(d, t, [&](gs_texture **a, gs_texture **) {
			    *a = open_legacy(d, handle, false);
			    return *a != nullptr;
		    })) {
			return true;
		}
	}
#endif
	blog(LOG_WARNING,
	     "[libobs-vulkan] device rebuild: imported %ux%u texture replaced by a placeholder; its owner must import "
	     "it again",
	     w, h);
	return vk_texture_recreate_placeholder(d, t);
}

void vk_sharedbuf_release(gs_device *d, gs_shared_buffer *b)
{
	vkDestroyBuffer(d->dev, b->buffer, nullptr);
	vkFreeMemory(d->dev, b->memory, nullptr);
	b->buffer = VK_NULL_HANDLE;
	b->memory = VK_NULL_HANDLE;
}

bool vk_sharedbuf_recreate(gs_device *d, gs_shared_buffer *b)
{
	gs_shared_buffer *fresh = create_sharedbuf(d, b->width, b->height, b->layout);
	if (!fresh) {
		return false;
	}
	d->objects.sharedbufs.erase(fresh);
	*b = *fresh;
	delete fresh;
	return true;
}

void vk_timeline_release(gs_device *d, gs_timeline *t)
{
	vkDestroySemaphore(d->dev, t->sem, nullptr);
	t->sem = VK_NULL_HANDLE;
}

// A local timeline at the last value seen (an import's other side is on the lost device too, or must hand over a
// new handle), exportable when the device can.
bool vk_timeline_recreate(gs_device *d, gs_timeline *t)
{
	const VkExternalSemaphoreHandleTypeFlagBits ht = timeline_export_type();
	if (t->imported) {
		blog(LOG_WARNING,
		     "[libobs-vulkan] device rebuild: imported timeline replaced by a local one at %llu; "
		     "its owner must import it again",
		     (unsigned long long)t->last_value);
	}
	t->exportable = timeline_handle_ok(d, ht, false);
	t->imported = false;
	t->sem = create_semaphore(d, t->last_value, t->exportable ? ht : 0);
	return t->sem != VK_NULL_HANDLE;
}
