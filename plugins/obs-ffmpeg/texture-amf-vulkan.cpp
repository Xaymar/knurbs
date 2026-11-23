#include "texture-amf-vulkan.hpp"

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>

#include <AMF/core/Surface.h>
#include <AMF/core/VulkanAMF.h>

#include <obs-module.h>
#include <util/windows/device-enum.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

#define LOG_PREFIX "[texture-amf-vulkan] "

/* ------------------------------------------------------------------------- */
/* Loader                                                                    */

namespace {

#define VK_GLOBAL(X)                                                                   	X(vkCreateInstance)                                                            	X(vkEnumerateInstanceVersion) X(vkEnumerateInstanceLayerProperties) X(vkEnumerateInstanceExtensionProperties)

#define VK_INSTANCE(X)                                                                                   \
	X(vkDestroyInstance)                                                                             \
	X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties2)                                  \
		X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkEnumerateDeviceExtensionProperties)      \
			X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceMemoryProperties)           \
				X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkCreateDebugUtilsMessengerEXT) \
					X(vkDestroyDebugUtilsMessengerEXT)

#define VK_DEVICE(X)                                                                                    \
	X(vkDestroyDevice)                                                                              \
	X(vkDeviceWaitIdle) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements2)         \
		X(vkAllocateMemory) X(vkFreeMemory) X(vkBindImageMemory) X(vkCreateSemaphore)           \
			X(vkDestroySemaphore) X(vkImportSemaphoreWin32HandleKHR) X(vkWaitSemaphores) \
				X(vkSignalSemaphore)

struct vk_fns {
	HMODULE lib = nullptr;
	PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
#define DECL(f) PFN_##f f = nullptr;
	VK_GLOBAL(DECL) VK_INSTANCE(DECL) VK_DEVICE(DECL)
#undef DECL
};

/* The extensions this code needs itself, on top of AMF's. */
const char *const own_extensions[] = {
	VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
	VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
};

/* Validation for this device too, when libobs's is on (OBS_VULKAN_VALIDATION=1): counts its messages. */
volatile long validation_messages = 0;

VKAPI_ATTR VkBool32 VKAPI_CALL on_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
					  VkDebugUtilsMessageTypeFlagsEXT,
					  const VkDebugUtilsMessengerCallbackDataEXT *data, void *)
{
	if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
		InterlockedIncrement(&validation_messages);
		blog(LOG_WARNING, LOG_PREFIX "validation: %s", data->pMessage);
	}
	return VK_FALSE;
}

} // namespace

/* ------------------------------------------------------------------------- */

struct amf_vulkan::frame {
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	amf::AMFVulkanSurface surface = {};
};

struct amf_vulkan::impl {
	vk_fns f;
	VkInstance instance = VK_NULL_HANDLE;
	VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
	VkPhysicalDevice phys = VK_NULL_HANDLE;
	VkDevice dev = VK_NULL_HANDLE;
	amf::AMFVulkanDevice amf_dev = {};
	uint32_t vendor = 0;
	bool p010 = false;

	VkSemaphore ready = VK_NULL_HANDLE, release = VK_NULL_HANDLE;
	void *ready_handle = nullptr, *release_handle = nullptr;

	std::map<uint64_t, frame> frames; // by tex_id

	std::mutex hold_mutex; // AMF releases surfaces from its own threads
	std::multiset<uint64_t> held;
	uint64_t highest_held = 0, signalled = 0;

	void signal(uint64_t value)
	{
		if (value <= signalled || !release) {
			return;
		}
		VkSemaphoreSignalInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
		si.semaphore = release;
		si.value = value;
		if (f.vkSignalSemaphore(dev, &si) == VK_SUCCESS) {
			signalled = value;
		} else {
			blog(LOG_ERROR, LOG_PREFIX "vkSignalSemaphore(%llu) failed", (unsigned long long)value);
		}
	}

	VkSemaphore import_timeline(void *handle)
	{
		VkSemaphoreTypeCreateInfo ti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
		ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
		VkSemaphoreCreateInfo ci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti};
		VkSemaphore s = VK_NULL_HANDLE;
		if (f.vkCreateSemaphore(dev, &ci, nullptr, &s) != VK_SUCCESS) {
			return VK_NULL_HANDLE;
		}
		VkImportSemaphoreWin32HandleInfoKHR ii = {VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
		ii.semaphore = s;
		ii.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
		ii.handle = handle;
		if (f.vkImportSemaphoreWin32HandleKHR(dev, &ii) != VK_SUCCESS) {
			f.vkDestroySemaphore(dev, s, nullptr);
			return VK_NULL_HANDLE;
		}
		return s;
	}

	void destroy_frame(frame &fr)
	{
		if (fr.image) {
			f.vkDestroyImage(dev, fr.image, nullptr);
		}
		if (fr.memory) {
			f.vkFreeMemory(dev, fr.memory, nullptr);
		}
		fr = frame{};
	}
};

static bool load(vk_fns &f)
{
	f.lib = LoadLibraryA("vulkan-1.dll");
	if (!f.lib) {
		return false;
	}
	f.vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)GetProcAddress(f.lib, "vkGetInstanceProcAddr");
	if (!f.vkGetInstanceProcAddr) {
		return false;
	}
#define LOAD(fn) f.fn = (PFN_##fn)f.vkGetInstanceProcAddr(nullptr, #fn);
	VK_GLOBAL(LOAD)
#undef LOAD
	return f.vkCreateInstance && f.vkEnumerateInstanceVersion;
}

static bool has_extension(const std::vector<VkExtensionProperties> &all, const char *name)
{
	return std::any_of(all.begin(), all.end(),
			   [&](const VkExtensionProperties &e) { return strcmp(e.extensionName, name) == 0; });
}

amf_vulkan::amf_vulkan(const std::vector<const char *> &extensions, bool p010) : d(std::make_unique<impl>())
{
	vk_fns &f = d->f;
	d->p010 = p010;
	if (!load(f)) {
		throw "Cannot load vulkan-1.dll";
	}
	uint32_t api = 0;
	f.vkEnumerateInstanceVersion(&api);
	if (api < VK_API_VERSION_1_3) {
		throw "Vulkan 1.3 instance unavailable";
	}

	// The renderer's adapter (graphics context needed).
	struct gs_adapter_identity id = {};
	obs_enter_graphics();
	const bool have_id = gs_get_adapter_identity(&id);
	obs_leave_graphics();
	if (!have_id) {
		throw "No adapter identity from the renderer";
	}

	const char *v = getenv("OBS_VULKAN_VALIDATION");
	bool validate = v && !strcmp(v, "1");
	if (validate) {
		uint32_t n = 0;
		f.vkEnumerateInstanceLayerProperties(&n, nullptr);
		std::vector<VkLayerProperties> layers(n);
		f.vkEnumerateInstanceLayerProperties(&n, layers.data());
		validate = std::any_of(layers.begin(), layers.end(), [](const VkLayerProperties &l) {
			return strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0;
		});
	}
	const char *layer = "VK_LAYER_KHRONOS_validation";

	// AMF enables VK_KHR_swapchain on the device, which needs the surface extensions on the instance, and looks up
	// instance functions by their KHR names (vkGetPhysicalDeviceProperties2KHR), which a 1.3 instance only
	// returns with the extensions that introduced them.
	uint32_t iext_count = 0;
	f.vkEnumerateInstanceExtensionProperties(nullptr, &iext_count, nullptr);
	std::vector<VkExtensionProperties> iexts(iext_count);
	f.vkEnumerateInstanceExtensionProperties(nullptr, &iext_count, iexts.data());
	std::vector<const char *> instance_ext;
	for (const char *e :
	     {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
	      VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
	      VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
	      VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME}) {
		if (has_extension(iexts, e)) {
			instance_ext.push_back(e);
		}
	}
	if (validate) {
		instance_ext.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	}

	VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.pApplicationName = "OBS AMF encoder";
	app.apiVersion = VK_API_VERSION_1_3;
	VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	ici.enabledExtensionCount = (uint32_t)instance_ext.size();
	ici.ppEnabledExtensionNames = instance_ext.data();
	if (validate) {
		ici.enabledLayerCount = 1;
		ici.ppEnabledLayerNames = &layer;
	}
	if (f.vkCreateInstance(&ici, nullptr, &d->instance) != VK_SUCCESS) {
		throw "vkCreateInstance failed";
	}
#define LOAD(fn) f.fn = (PFN_##fn)f.vkGetInstanceProcAddr(d->instance, #fn);
	VK_INSTANCE(LOAD)
#undef LOAD
	if (validate && f.vkCreateDebugUtilsMessengerEXT) {
		VkDebugUtilsMessengerCreateInfoEXT mi = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
		mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
				     VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
		mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
				 VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
				 VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
		mi.pfnUserCallback = on_message;
		f.vkCreateDebugUtilsMessengerEXT(d->instance, &mi, nullptr, &d->messenger);
	}

	uint32_t count = 0;
	f.vkEnumeratePhysicalDevices(d->instance, &count, nullptr);
	std::vector<VkPhysicalDevice> phys(count);
	f.vkEnumeratePhysicalDevices(d->instance, &count, phys.data());
	for (VkPhysicalDevice p : phys) {
		VkPhysicalDeviceIDProperties idp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
		VkPhysicalDeviceProperties2 props = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &idp};
		f.vkGetPhysicalDeviceProperties2(p, &props);
		if (memcmp(idp.deviceUUID, id.device_uuid, VK_UUID_SIZE) == 0) {
			d->phys = p;
			d->vendor = props.properties.vendorID;
			blog(LOG_INFO, LOG_PREFIX "device: %s", props.properties.deviceName);
			break;
		}
	}
	if (!d->phys) {
		throw "The renderer's adapter is not a Vulkan device of this instance";
	}

	// Extensions: AMF's, then ours; the ones this device lacks are left out (AMF says which it cannot do without).
	uint32_t ext_count = 0;
	f.vkEnumerateDeviceExtensionProperties(d->phys, nullptr, &ext_count, nullptr);
	std::vector<VkExtensionProperties> available(ext_count);
	f.vkEnumerateDeviceExtensionProperties(d->phys, nullptr, &ext_count, available.data());
	std::vector<const char *> enable;
	for (const char *e : extensions) {
		if (has_extension(available, e)) {
			enable.push_back(e);
		} else {
			blog(LOG_WARNING, LOG_PREFIX "AMF asks for %s, which the device lacks", e);
		}
	}
	for (const char *e : own_extensions) {
		if (!has_extension(available, e)) {
			throw "The device cannot import NT handles";
		}
		if (std::none_of(enable.begin(), enable.end(), [&](const char *x) { return !strcmp(x, e); })) {
			enable.push_back(e);
		}
	}

	// Every queue of every family: AMF picks its own (compute, encode); nothing here submits.
	uint32_t family_count = 0;
	f.vkGetPhysicalDeviceQueueFamilyProperties(d->phys, &family_count, nullptr);
	std::vector<VkQueueFamilyProperties> families(family_count);
	f.vkGetPhysicalDeviceQueueFamilyProperties(d->phys, &family_count, families.data());
	std::vector<std::vector<float>> priorities(family_count);
	std::vector<VkDeviceQueueCreateInfo> queues;
	for (uint32_t i = 0; i < family_count; ++i) {
		priorities[i].assign(families[i].queueCount, 1.0f);
		VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
		qi.queueFamilyIndex = i;
		qi.queueCount = families[i].queueCount;
		qi.pQueuePriorities = priorities[i].data();
		queues.push_back(qi);
	}

	// Every core feature the device has (AMF documents none it needs; it fails without some, e.g. timelines).
	VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
	VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
	VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
	VkPhysicalDeviceFeatures2 features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
	f.vkGetPhysicalDeviceFeatures2(d->phys, &features);
	if (!f12.timelineSemaphore) {
		throw "No timeline semaphores";
	}
	// Robustness costs performance and AMF does not ask for it.
	features.features.robustBufferAccess = VK_FALSE;
	f13.robustImageAccess = VK_FALSE;

	VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &features};
	dci.queueCreateInfoCount = (uint32_t)queues.size();
	dci.pQueueCreateInfos = queues.data();
	dci.enabledExtensionCount = (uint32_t)enable.size();
	dci.ppEnabledExtensionNames = enable.data();
	if (f.vkCreateDevice(d->phys, &dci, nullptr, &d->dev) != VK_SUCCESS) {
		throw "vkCreateDevice failed";
	}
#define LOAD(fn) f.fn = (PFN_##fn)f.vkGetDeviceProcAddr(d->dev, #fn);
	VK_DEVICE(LOAD)
#undef LOAD
	if (!f.vkImportSemaphoreWin32HandleKHR || !f.vkWaitSemaphores || !f.vkSignalSemaphore) {
		throw "Device functions missing";
	}

	d->amf_dev.cbSizeof = sizeof(d->amf_dev);
	d->amf_dev.hInstance = d->instance;
	d->amf_dev.hPhysicalDevice = d->phys;
	d->amf_dev.hDevice = d->dev;
	blog(LOG_INFO, LOG_PREFIX "device created: %u queue families, %zu extensions", family_count, enable.size());
}

amf_vulkan::~amf_vulkan()
{
	vk_fns &f = d->f;
	if (d->dev) {
		release_all();
		f.vkDeviceWaitIdle(d->dev);
		for (auto &it : d->frames) {
			d->destroy_frame(it.second);
		}
		if (d->ready) {
			f.vkDestroySemaphore(d->dev, d->ready, nullptr);
		}
		if (d->release) {
			f.vkDestroySemaphore(d->dev, d->release, nullptr);
		}
		f.vkDestroyDevice(d->dev, nullptr);
	}
	if (d->messenger) {
		f.vkDestroyDebugUtilsMessengerEXT(d->instance, d->messenger, nullptr);
	}
	if (d->instance) {
		f.vkDestroyInstance(d->instance, nullptr);
	}
	const long n = validation_messages;
	if (n) {
		blog(LOG_WARNING, LOG_PREFIX "%ld validation messages on the AMF device", n);
	}
	if (f.lib) {
		FreeLibrary(f.lib);
	}
}

void *amf_vulkan::amf_device()
{
	return &d->amf_dev;
}

uint32_t amf_vulkan::vendor_id() const
{
	return d->vendor;
}

amf::AMFVulkanSurface *amf_vulkan::surface(const encoder_texture_shared &shared)
{
	vk_fns &f = d->f;
	if (shared.tex.type != GS_HANDLE_WIN32 || !shared.tex.win32 || !shared.tex_id) {
		throw "libobs gave no exported frame";
	}
	if (shared.ready.type != GS_HANDLE_WIN32 || shared.release.type != GS_HANDLE_WIN32) {
		throw "libobs gave no NT timeline handles";
	}

	// Timelines: imported once (libobs keeps the same ones for the life of the frame pool).
	if (d->ready_handle != shared.ready.win32) {
		if (d->ready) {
			f.vkDestroySemaphore(d->dev, d->ready, nullptr);
		}
		d->ready = d->import_timeline(shared.ready.win32);
		d->ready_handle = d->ready ? shared.ready.win32 : nullptr;
		if (!d->ready) {
			throw "Cannot import the ready timeline";
		}
	}
	if (d->release_handle != shared.release.win32) {
		std::scoped_lock lock(d->hold_mutex);
		if (d->release) {
			f.vkDestroySemaphore(d->dev, d->release, nullptr);
		}
		d->release = d->import_timeline(shared.release.win32);
		d->release_handle = d->release ? shared.release.win32 : nullptr;
		if (!d->release) {
			throw "Cannot import the release timeline";
		}
	}

	frame &fr = d->frames[shared.tex_id];
	if (!fr.image) {
		const gs_shared_texture_desc &t = shared.tex;
		const bool p010 = t.layout == GS_SHARED_P010;
		if (t.layout != (d->p010 ? GS_SHARED_P010 : GS_SHARED_NV12)) {
			d->frames.erase(shared.tex_id);
			throw "Frame layout does not match the encoder's format";
		}
		// Exactly as libobs-vulkan creates its NV12/P010 encoder frames (vk_texture_image_info): an opaque
		// import needs the exporter's parameters.
		// The UINT views are the ones AMF creates.
		const VkFormat views[4] = {p010 ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM,
					   p010 ? VK_FORMAT_R16G16_UNORM : VK_FORMAT_R8G8_UNORM,
					   p010 ? VK_FORMAT_R16_UINT : VK_FORMAT_R8_UINT,
					   p010 ? VK_FORMAT_R16G16_UINT : VK_FORMAT_R8G8_UINT};
		VkExternalMemoryImageCreateInfo emi = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
		emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
		VkImageFormatListCreateInfo fl = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, &emi};
		fl.viewFormatCount = 4;
		fl.pViewFormats = views;
		VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &fl};
		ii.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
		ii.imageType = VK_IMAGE_TYPE_2D;
		ii.format = p010 ? VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16
				 : VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
		ii.extent = {t.width, t.height, 1};
		ii.mipLevels = 1;
		ii.arrayLayers = 1;
		ii.samples = VK_SAMPLE_COUNT_1_BIT;
		ii.tiling = VK_IMAGE_TILING_OPTIMAL;
		ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
			   VK_IMAGE_USAGE_TRANSFER_DST_BIT |
			   ((t.flags & GS_RENDER_TARGET) ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0);
		ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		if (f.vkCreateImage(d->dev, &ii, nullptr, &fr.image) != VK_SUCCESS) {
			d->frames.erase(shared.tex_id);
			throw "vkCreateImage failed";
		}

		VkImageMemoryRequirementsInfo2 mri = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
		mri.image = fr.image;
		VkMemoryRequirements2 mr = {VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
		f.vkGetImageMemoryRequirements2(d->dev, &mri, &mr);
		VkPhysicalDeviceMemoryProperties mp;
		f.vkGetPhysicalDeviceMemoryProperties(d->phys, &mp);
		uint32_t type = UINT32_MAX;
		for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
			if ((mr.memoryRequirements.memoryTypeBits & (1u << i)) &&
			    (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
				type = i;
				break;
			}
		}
		VkMemoryDedicatedAllocateInfo dai = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
		dai.image = fr.image;
		// AMF exports the memory of the surfaces it is given (vkGetMemoryWin32HandleKHR), so it is exportable here
		// too.
		VkExportMemoryAllocateInfo exp = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &dai};
		exp.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
		VkImportMemoryWin32HandleInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, &exp};
		imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
		imp.handle = t.win32;
		VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp};
		ai.allocationSize = t.size;
		ai.memoryTypeIndex = type;
		if (type == UINT32_MAX || f.vkAllocateMemory(d->dev, &ai, nullptr, &fr.memory) != VK_SUCCESS ||
		    f.vkBindImageMemory(d->dev, fr.image, fr.memory, 0) != VK_SUCCESS) {
			d->destroy_frame(fr);
			d->frames.erase(shared.tex_id);
			throw "Cannot import the frame's memory";
		}

		amf::AMFVulkanSurface &s = fr.surface;
		s.cbSizeof = sizeof(s);
		s.hImage = fr.image;
		s.hMemory = fr.memory;
		s.iSize = (amf_int64)t.size;
		s.eFormat = ii.format;
		s.iWidth = (amf_int32)t.width;
		s.iHeight = (amf_int32)t.height;
		// libobs leaves its external images in GENERAL between submissions.
		s.eCurrentLayout = VK_IMAGE_LAYOUT_GENERAL;
		s.eUsage = amf::AMF_SURFACE_USAGE_SHADER_RESOURCE | amf::AMF_SURFACE_USAGE_TRANSFER_SRC |
			   amf::AMF_SURFACE_USAGE_TRANSFER_DST |
			   ((t.flags & GS_RENDER_TARGET) ? amf::AMF_SURFACE_USAGE_RENDER_TARGET : 0);
		s.eAccess = amf::AMF_MEMORY_CPU_NONE;
		s.Sync.cbSizeof = sizeof(s.Sync);
		blog(LOG_DEBUG, LOG_PREFIX "imported frame %llu (%ux%u %s)", (unsigned long long)shared.tex_id, t.width,
		     t.height, p010 ? "P010" : "NV12");
	}

	// Host wait: nothing here may submit to the device's queues (AMF owns them).
	VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
	wi.semaphoreCount = 1;
	wi.pSemaphores = &d->ready;
	wi.pValues = &shared.ready_value;
	const VkResult r = f.vkWaitSemaphores(d->dev, &wi, 5000000000ull);
	if (r != VK_SUCCESS) {
		throw r == VK_TIMEOUT ? "Timed out waiting for libobs's frame" : "vkWaitSemaphores failed";
	}

	// AMF records its layout transitions from here; no semaphore to wait for.
	fr.surface.eCurrentLayout = VK_IMAGE_LAYOUT_GENERAL;
	fr.surface.Sync.hSemaphore = VK_NULL_HANDLE;
	fr.surface.Sync.bSubmitted = false;
	fr.surface.Sync.hFence = VK_NULL_HANDLE;
	return &fr.surface;
}

void amf_vulkan::hold(uint64_t release_value)
{
	std::scoped_lock lock(d->hold_mutex);
	d->held.insert(release_value);
	d->highest_held = std::max(d->highest_held, release_value);
}

void amf_vulkan::released(uint64_t release_value)
{
	std::scoped_lock lock(d->hold_mutex);
	auto it = d->held.find(release_value);
	if (it != d->held.end()) {
		d->held.erase(it);
	}
	// Values only increase: signal up to just below the oldest frame still held.
	d->signal(d->held.empty() ? d->highest_held : *d->held.begin() - 1);
}

void amf_vulkan::release_all()
{
	std::scoped_lock lock(d->hold_mutex);
	d->held.clear();
	d->signal(d->highest_held);
}

int amf_vulkan_renderer_dxgi_index(uint32_t *vendor_id)
{
	struct gs_adapter_identity id = {};
	if (!gs_get_adapter_identity(&id) || !id.luid_valid) {
		return -1;
	}
	if (vendor_id) {
		*vendor_id = id.vendor_id;
	}
	struct search {
		uint64_t luid;
		int index = -1;
	} s;
	memcpy(&s.luid, id.luid, sizeof(s.luid));
	enum_graphics_device_luids(
		[](void *param, uint32_t idx, uint64_t luid) {
			auto p = (search *)param;
			if (luid == p->luid) {
				p->index = (int)idx;
				return false;
			}
			return true;
		},
		&s);
	return s.index;
}
