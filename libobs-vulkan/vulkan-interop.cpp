// External memory and synchronisation (docs/graphics-interop.md): exportable and imported textures, the pitch-linear
// frame buffer, timeline semaphores, adapter identity, and the legacy Windows shared texture and keyed mutex exports
// built on them.
//
// Ownership model: an external resource belongs to VK_QUEUE_FAMILY_EXTERNAL in VK_IMAGE_LAYOUT_GENERAL between this
// device's submissions. Its first use in a submission acquires it (vk_tracker::use_image), and vk_flush releases it
// again before submitting (vk_interop_prepare_submit). So the other side may use it whenever it holds the sync
// (a keyed mutex, or a timeline value this device signalled).
//
// shortcut: Windows only so far. The Linux types (opaque fd, dmabuf, sync_file) are declared in the API and return
// false here until the Linux side implements them (VK_KHR_external_memory_fd, VK_EXT_external_memory_dma_buf,
// VK_EXT_image_drm_format_modifier, VK_KHR_external_semaphore_fd).
#include "vulkan-subsystem.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_4.h>
#include <vulkan/vulkan_win32.h>
#endif

#include <util/base.h>

#include <errno.h>
#include <string.h>
#include <algorithm>
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
	(void)wanted;
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
	(void)on;
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

void vk_interop_free(gs_device *d)
{
	if (!d->interop) {
		return;
	}
#ifdef _WIN32
	if (d->interop->probe) {
		d->interop->probe->Release();
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
	return d->ext_memory_win32;
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
	const bool opaque = !is_d3d11(desc.type);
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
	return false;
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

static VkExternalMemoryHandleTypeFlagBits export_memory_type()
{
#ifdef _WIN32
	return VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
	return (VkExternalMemoryHandleTypeFlagBits)0; // shortcut: OPAQUE_FD on Linux
#endif
}

// Records a release of `image` from UNDEFINED to GENERAL for the external owner, submits it and waits for it, so a
// fresh exportable resource is in the hand-off state before its handle can leave this device.
static bool initial_release(gs_device *d, VkImage image, VkBuffer buffer)
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
		ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
		ib.image = image;
		ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		di.imageMemoryBarrierCount = 1;
		di.pImageMemoryBarriers = &ib;
	} else {
		bb.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
		bb.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
		bb.srcQueueFamilyIndex = d->graphics.family;
		bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
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
static VkDeviceMemory allocate_exportable(gs_device *d, VkImage image, VkBuffer buffer, VkDeviceSize &size)
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
	ex.handleTypes = export_memory_type();
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

static bool create_exportable(gs_device *d, uint32_t width, uint32_t height, enum gs_color_format format,
			      enum gs_shared_layout layout, uint32_t flags, gs_texture **tex, gs_texture **tex_uv)
{
	const VkExternalMemoryHandleTypeFlagBits ht = export_memory_type();
	// Two-plane frames the compute conversion can write get STORAGE usage; the flag goes into the export
	// description, so importers create their image the same way. Decided here, not by the caller.
	flags &= ~GS_SHADER_STORAGE;
	if (vk_yuv_storage(d, layout)) {
		flags |= GS_SHADER_STORAGE;
	}
	vk_image_desc id;
	if (!ht || !memory_ext(d) || !vk_texture_image_info(d, width, height, format, layout, flags, id) ||
	    !external_image_ok(d, id, ht, false)) {
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
	VkImage image = VK_NULL_HANDLE;
	if (vkCreateImage(d->dev, &id.ii, nullptr, &image) != VK_SUCCESS) {
		return false;
	}
	VkDeviceSize size = 0;
	VkDeviceMemory memory = allocate_exportable(d, image, VK_NULL_HANDLE, size);
	gs_texture *uv = nullptr;
	if (!memory || vkBindImageMemory(d->dev, image, memory, 0) != VK_SUCCESS ||
	    !vk_wrap_image(d, image, width, height, format, layout, flags, tex, &uv)) {
		blog(LOG_ERROR, "[libobs-vulkan] device_texture_create_exportable: allocation failed");
		vkFreeMemory(d->dev, memory, nullptr);
		vkDestroyImage(d->dev, image, nullptr);
		return false;
	}
	set_external_textures(d, *tex, uv, memory, size, ht, layout, flags, false);
	for (gs_texture *t : {*tex, uv}) {
		if (t) {
			t->origin = gs_texture::ORIGIN_EXPORTABLE;
		}
	}
	if (tex_uv) {
		*tex_uv = uv;
	}
	if (!initial_release(d, image, VK_NULL_HANDLE)) {
		gs_texture_destroy(*tex);
		gs_texture_destroy(uv);
		*tex = nullptr;
		if (tex_uv) {
			*tex_uv = nullptr;
		}
		return false;
	}
	d->tracker.set_external(image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
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
	return false;
#endif
}

/* ---- Pitch-linear frame buffer ------------------------------------------------------------------------------- */

static uint32_t plane_bytes(enum gs_shared_layout layout)
{
	return layout == GS_SHARED_P010 ? 2 : 1;
}

static gs_shared_buffer *create_sharedbuf(gs_device *d, uint32_t width, uint32_t height, enum gs_shared_layout layout)
{
	const VkExternalMemoryHandleTypeFlagBits ht = export_memory_type();
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
	    !(b->memory = allocate_exportable(d, VK_NULL_HANDLE, b->buffer, size)) ||
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
	if (!ht || !memory_ext(device) || (!import && ht != export_memory_type()) ||
	    (import && d3d11_planar_refused(device, type, layout))) {
		return false;
	}
	vk_image_desc id;
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
	if (!export_memory(device, buf->memory, export_memory_type(), type, *desc)) {
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
	if (!ht || !d->ext_semaphore_win32) {
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

EXPORT gs_timeline_t *device_timeline_create(gs_device_t *device, uint64_t initial_value)
{
#ifdef _WIN32
	const VkExternalSemaphoreHandleTypeFlagBits ht = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
	const VkExternalSemaphoreHandleTypeFlagBits ht = (VkExternalSemaphoreHandleTypeFlagBits)0;
#endif
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
	return nullptr;
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
	(void)ht;
	return false;
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
#ifdef _WIN32
	const VkExternalSemaphoreHandleTypeFlagBits ht = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
	const VkExternalSemaphoreHandleTypeFlagBits ht = (VkExternalSemaphoreHandleTypeFlagBits)0;
#endif
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
