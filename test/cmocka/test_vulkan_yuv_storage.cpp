// Probe: can a compute shader write the planes of a multi-planar YUV image directly? For every Vulkan device and
// each 4:2:0 / 4:2:2 two-plane format (8, 10 (X6, 3PACK16) and 16 bits) this logs, and checks for consistency:
//   - the plane-compatible formats (R8/R8G8 or R16/R16G16, UNORM and UINT) with STORAGE_IMAGE in optimal tiling,
//   - vkGetPhysicalDeviceImageFormatProperties2 for the multi-planar format created MUTABLE_FORMAT | EXTENDED_USAGE
//     with STORAGE usage and those formats in a VkImageFormatListCreateInfo, also as exportable external memory,
//   - that such an image can really be created and given per-plane views with VkImageViewUsageCreateInfo{STORAGE}.
// libobs-vulkan (vulkan-yuv.cpp) applies the same rule before it uses its compute conversion into images.
// Pure Vulkan, no libobs: it runs wherever a loader exists and skips without one. With the validation layer present,
// any message fails the test.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include <vulkan/vulkan.h>

// cmocka.h redefines `inline`, so it must come after the C++ standard headers.
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
extern "C" {
#include <cmocka.h>
}

#ifdef _WIN32
static constexpr VkExternalMemoryHandleTypeFlagBits export_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
static constexpr VkExternalMemoryHandleTypeFlagBits export_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif

static int validation_messages;

static VKAPI_ATTR VkBool32 VKAPI_CALL on_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
						 VkDebugUtilsMessageTypeFlagsEXT,
						 const VkDebugUtilsMessengerCallbackDataEXT *data, void *)
{
	if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
		fprintf(stderr, "validation: %s\n", data->pMessage);
		++validation_messages;
	}
	return VK_FALSE;
}

struct planar {
	const char *name;
	VkFormat format;
	bool wide; // 16-bit plane formats (10 and 16 bits) instead of 8-bit
};

static const planar formats[] = {
	{"G8_B8R8_2PLANE_420 (NV12)", VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, false},
	{"G10X6_B10X6R10X6_2PLANE_420_3PACK16 (P010)", VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16, true},
	{"G16_B16R16_2PLANE_420", VK_FORMAT_G16_B16R16_2PLANE_420_UNORM, true},
	{"G8_B8R8_2PLANE_422 (NV16)", VK_FORMAT_G8_B8R8_2PLANE_422_UNORM, false},
	{"G10X6_B10X6R10X6_2PLANE_422_3PACK16 (P210)", VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16, true},
	{"G16_B16R16_2PLANE_422 (P216)", VK_FORMAT_G16_B16R16_2PLANE_422_UNORM, true},
};

struct view_format {
	const char *name;
	VkFormat format;
};

// Y plane then UV plane candidates, UNORM first.
static const view_format narrow_y[] = {{"R8_UNORM", VK_FORMAT_R8_UNORM}, {"R8_UINT", VK_FORMAT_R8_UINT}};
static const view_format narrow_uv[] = {{"R8G8_UNORM", VK_FORMAT_R8G8_UNORM}, {"R8G8_UINT", VK_FORMAT_R8G8_UINT}};
static const view_format wide_y[] = {{"R16_UNORM", VK_FORMAT_R16_UNORM}, {"R16_UINT", VK_FORMAT_R16_UINT}};
static const view_format wide_uv[] = {{"R16G16_UNORM", VK_FORMAT_R16G16_UNORM}, {"R16G16_UINT", VK_FORMAT_R16G16_UINT}};

static std::string driver_version(const VkPhysicalDeviceProperties &p)
{
	char s[64];
	const uint32_t v = p.driverVersion;
	if (p.vendorID == 0x10DE) { // NVIDIA: 10.8.8.6
		snprintf(s, sizeof(s), "%u.%u", v >> 22, (v >> 14) & 0xFF);
	} else {
		snprintf(s, sizeof(s), "%u.%u.%u", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v),
			 VK_API_VERSION_PATCH(v));
	}
	return s;
}

static bool image_props(VkPhysicalDevice phys, VkFormat format, const VkFormat *views, uint32_t nviews,
			VkImageUsageFlags usage, bool external)
{
	VkPhysicalDeviceExternalImageFormatInfo ext = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
	ext.handleType = export_type;
	VkImageFormatListCreateInfo fl = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, external ? &ext : nullptr};
	fl.viewFormatCount = nviews;
	fl.pViewFormats = views;
	VkPhysicalDeviceImageFormatInfo2 info = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &fl};
	info.format = format;
	info.type = VK_IMAGE_TYPE_2D;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = usage;
	info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
	VkExternalImageFormatProperties eprops = {VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
	VkImageFormatProperties2 props = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, external ? &eprops : nullptr};
	if (vkGetPhysicalDeviceImageFormatProperties2(phys, &info, &props) != VK_SUCCESS) {
		return false;
	}
	return !external ||
	       (eprops.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT);
}

// Creates a 64x64 image of `format` with STORAGE usage and both plane views for storage; true when all succeed.
static bool create_storage_views(VkDevice dev, VkPhysicalDevice phys, VkFormat format, const VkFormat views[4],
				 VkFormat y, VkFormat uv)
{
	VkImageFormatListCreateInfo fl = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
	fl.viewFormatCount = 4;
	fl.pViewFormats = views;
	VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &fl};
	ii.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
	ii.imageType = VK_IMAGE_TYPE_2D;
	ii.format = format;
	ii.extent = {64, 64, 1};
	ii.mipLevels = 1;
	ii.arrayLayers = 1;
	ii.samples = VK_SAMPLE_COUNT_1_BIT;
	ii.tiling = VK_IMAGE_TILING_OPTIMAL;
	ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
		   VK_IMAGE_USAGE_STORAGE_BIT;
	VkImage image = VK_NULL_HANDLE;
	if (vkCreateImage(dev, &ii, nullptr, &image) != VK_SUCCESS) {
		return false;
	}
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(dev, image, &mr);
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(phys, &mp);
	uint32_t type = UINT32_MAX;
	for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; ++i) {
		if ((mr.memoryTypeBits & (1u << i)) &&
		    (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
			type = i;
		}
	}
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = mr.size;
	ai.memoryTypeIndex = type;
	VkDeviceMemory mem = VK_NULL_HANDLE;
	bool ok = type != UINT32_MAX && vkAllocateMemory(dev, &ai, nullptr, &mem) == VK_SUCCESS &&
		  vkBindImageMemory(dev, image, mem, 0) == VK_SUCCESS;
	for (int plane = 0; ok && plane < 2; ++plane) {
		VkImageViewUsageCreateInfo vu = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
		vu.usage = VK_IMAGE_USAGE_STORAGE_BIT;
		VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, &vu};
		vi.image = image;
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format = plane ? uv : y;
		vi.subresourceRange = {(VkImageAspectFlags)(plane ? VK_IMAGE_ASPECT_PLANE_1_BIT
								  : VK_IMAGE_ASPECT_PLANE_0_BIT),
				       0, 1, 0, 1};
		VkImageView view = VK_NULL_HANDLE;
		ok = vkCreateImageView(dev, &vi, nullptr, &view) == VK_SUCCESS;
		vkDestroyImageView(dev, view, nullptr);
	}
	vkDestroyImage(dev, image, nullptr);
	vkFreeMemory(dev, mem, nullptr);
	return ok;
}

static bool storage(VkPhysicalDevice phys, VkFormat f)
{
	VkFormatProperties p;
	vkGetPhysicalDeviceFormatProperties(phys, f, &p);
	return (p.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
}

static void probe(void **)
{
	VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.pApplicationName = "test_vulkan_yuv_storage";
	app.apiVersion = VK_API_VERSION_1_3;

	uint32_t nlayers = 0;
	vkEnumerateInstanceLayerProperties(&nlayers, nullptr);
	std::vector<VkLayerProperties> layers(nlayers);
	vkEnumerateInstanceLayerProperties(&nlayers, layers.data());
	bool validation = false;
	for (const VkLayerProperties &l : layers) {
		validation = validation || !strcmp(l.layerName, "VK_LAYER_KHRONOS_validation");
	}
	const char *layer = "VK_LAYER_KHRONOS_validation";
	const char *ext = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
	VkDebugUtilsMessengerCreateInfoEXT mi = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
	mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
			     VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
	mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
	mi.pfnUserCallback = on_message;
	VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, validation ? &mi : nullptr};
	ici.pApplicationInfo = &app;
	ici.enabledLayerCount = validation ? 1 : 0;
	ici.ppEnabledLayerNames = &layer;
	ici.enabledExtensionCount = validation ? 1 : 0;
	ici.ppEnabledExtensionNames = &ext;
	VkInstance instance = VK_NULL_HANDLE;
	if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
		print_message("no Vulkan 1.3 instance, skipped\n");
		skip();
	}
	print_message("validation layer: %s\n", validation ? "on" : "not installed");

	uint32_t ndev = 0;
	vkEnumeratePhysicalDevices(instance, &ndev, nullptr);
	std::vector<VkPhysicalDevice> devs(ndev);
	vkEnumeratePhysicalDevices(instance, &ndev, devs.data());
	uint32_t probed = 0;
	for (VkPhysicalDevice phys : devs) {
		VkPhysicalDeviceDriverProperties drv = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
		VkPhysicalDeviceProperties2 props = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &drv};
		vkGetPhysicalDeviceProperties2(phys, &props);
		const VkPhysicalDeviceProperties &p = props.properties;
		print_message("device: %s (vendor 0x%04X, device 0x%04X), driver %s %s (%s), Vulkan %u.%u.%u\n",
			      p.deviceName, p.vendorID, p.deviceID, drv.driverName, drv.driverInfo,
			      driver_version(p).c_str(), VK_API_VERSION_MAJOR(p.apiVersion),
			      VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
		if (p.apiVersion < VK_API_VERSION_1_3 || p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
			print_message("  skipped (not a Vulkan 1.3 GPU)\n");
			continue;
		}
		VkPhysicalDeviceFeatures feats;
		vkGetPhysicalDeviceFeatures(phys, &feats);
		print_message("  shaderStorageImageExtendedFormats %d, shaderStorageImageWriteWithoutFormat %d\n",
			      feats.shaderStorageImageExtendedFormats, feats.shaderStorageImageWriteWithoutFormat);

		uint32_t family = UINT32_MAX, nfam = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(phys, &nfam, nullptr);
		std::vector<VkQueueFamilyProperties> fams(nfam);
		vkGetPhysicalDeviceQueueFamilyProperties(phys, &nfam, fams.data());
		for (uint32_t i = 0; i < nfam && family == UINT32_MAX; ++i) {
			if (fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
				family = i;
			}
		}
		const float prio = 1.0f;
		VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
		qi.queueFamilyIndex = family;
		qi.queueCount = 1;
		qi.pQueuePriorities = &prio;
		VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
		VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
		vkGetPhysicalDeviceFeatures2(phys, &f2);
		VkPhysicalDeviceVulkan11Features en11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
		en11.samplerYcbcrConversion = f11.samplerYcbcrConversion; // multi-planar image views need it enabled
		VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &en11};
		di.queueCreateInfoCount = 1;
		di.pQueueCreateInfos = &qi;
		VkDevice dev = VK_NULL_HANDLE;
		assert_int_equal(vkCreateDevice(phys, &di, nullptr, &dev), VK_SUCCESS);
		++probed;

		for (const planar &pf : formats) {
			VkFormatProperties fp;
			vkGetPhysicalDeviceFormatProperties(phys, pf.format, &fp);
			const VkFormatFeatureFlags of = fp.optimalTilingFeatures;
			if (!(of & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT)) {
				print_message("  %-46s not supported\n", pf.name);
				continue;
			}
			const view_format *ys = pf.wide ? wide_y : narrow_y;
			const view_format *uvs = pf.wide ? wide_uv : narrow_uv;
			const VkFormat list[4] = {ys[0].format, uvs[0].format, ys[1].format, uvs[1].format};
			const VkImageUsageFlags base = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
						       VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			const bool img =
				image_props(phys, pf.format, list, 4, base | VK_IMAGE_USAGE_STORAGE_BIT, false);
			const bool img_rt = image_props(
				phys, pf.format, list, 4,
				base | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, false);
			const bool exp = image_props(
				phys, pf.format, list, 4,
				base | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, true);
			print_message("  %-46s features 0x%08X (storage %d, disjoint %d); "
				      "MUTABLE|EXTENDED_USAGE+STORAGE: image %d, +COLOR_ATTACHMENT %d, exportable %d\n",
				      pf.name, of, !!(of & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT),
				      !!(of & VK_FORMAT_FEATURE_DISJOINT_BIT), img, img_rt, exp);
			for (int i = 0; i < 2; ++i) {
				const bool sy = storage(phys, ys[i].format), suv = storage(phys, uvs[i].format);
				bool views = false;
				if (img && sy && suv) {
					views = create_storage_views(dev, phys, pf.format, list, ys[i].format,
								     uvs[i].format);
					// The format queries promised it; a refusal here is a driver bug worth knowing about.
					assert_true(views);
				}
				print_message("    plane views %-6s / %-12s STORAGE_IMAGE: Y %d, UV %d; storage views "
					      "created: %s\n",
					      ys[i].name, uvs[i].name, sy, suv,
					      img && sy && suv ? (views ? "yes" : "NO") : "not attempted");
			}
		}
		vkDestroyDevice(dev, nullptr);
	}
	vkDestroyInstance(instance, nullptr);
	if (!probed) {
		print_message("no Vulkan 1.3 GPU, skipped\n");
		skip();
	}
	assert_int_equal(validation_messages, 0);
}

int main()
{
	const struct CMUnitTest tests[] = {cmocka_unit_test(probe)};
	return cmocka_run_group_tests(tests, nullptr, nullptr);
}
