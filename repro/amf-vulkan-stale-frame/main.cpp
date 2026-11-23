// Standalone reproducer: AMF (Vulkan context) encodes the picture of an earlier input.
//
// AMF is initialised with AMFContext1::InitVulkan on a VkDevice made here (AMF's extension list, every queue of
// every family, every core feature). Each frame is a VK_FORMAT_G8_B8R8_2PLANE_420_UNORM image with the frame number
// written into it as big luma blocks (16 bits: 12 bits of frame number, 4 bits of check). The image is wrapped with
// CreateSurfaceFromVulkanNative and submitted to the encoder; the bitstream is decoded with libavcodec and every
// decoded picture is checked against the number it should carry. Stale pictures are reported with how far back
// they are.
//
// Everything that may matter is a switch, so the trigger can be narrowed (see README.md and --help).
// Exit code: 0 PASS, 1 FAIL (stale/garbled frames), 2 setup error.
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vulkan/vulkan.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <psapi.h>

#include "core/Factory.h"
#include "core/VulkanAMF.h"
#include "components/VideoEncoderVCE.h"
#include "components/VideoEncoderHEVC.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// ---------------------------------------------------------------- errors
[[noreturn]] void die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stdout, "SETUP ERROR: ");
	vfprintf(stdout, fmt, ap);
	fprintf(stdout, "\n");
	va_end(ap);
	fflush(stdout);
	exit(2);
}
#define VKC(expr)                                                                       \
	do {                                                                            \
		VkResult _r = (expr);                                                   \
		if (_r != VK_SUCCESS)                                                   \
			die("%s -> VkResult %d at line %d", #expr, (int)_r, __LINE__); \
	} while (0)
#define AMFC(expr)                                                                         \
	do {                                                                               \
		AMF_RESULT _a = (expr);                                                    \
		if (_a != AMF_OK)                                                          \
			die("%s -> AMF_RESULT %d at line %d", #expr, (int)_a, __LINE__); \
	} while (0)

// ---------------------------------------------------------------- options
struct options {
	uint32_t w = 1280, h = 720;
	int frames = 120;
	std::string codec = "avc";
	std::string gpu = "AMD";
	// producer
	std::string producer = "separate"; // separate: own VkDevice, images exported and imported (as OBS); same: AMF's device
	std::string pqueue = "graphics";   // same device only: graphics | compute1 (a compute queue AMF does not use) | amf
	int ring = 8;                      // 0: a fresh image every frame
	bool keep = false;                 // fresh images: never destroy (no memory reuse)
	std::string sync = "timeline";     // fence | timeline | idle | devidle | none | gpu (semaphore in AMFVulkanSurface::Sync)
	bool dedicated = true;
	bool export_mem = true; // AMF device memory exportable (as the plugin)
	std::string tiling = "optimal";
	bool format_list = true; // MUTABLE_FORMAT | EXTENDED_USAGE + R8/R8G8 format list (as libobs)
	int busy = 1;            // copies of the pattern per frame (more GPU work per frame)
	std::string input = "native"; // native: CreateSurfaceFromVulkanNative; alloc: AllocSurface(VULKAN) + copy (as VES)
	std::string release = "external"; // separate: queue family release of the image: external | none
	// AMF
	bool dx11 = false;      // InitDX11 on the context too (on the same adapter)
	int amf_queue = -1;     // AMF_CONTEXT_VULKAN_COMPUTE_QUEUE
	bool amf_timeline = false;
	bool preencode = true;  // as the OBS plugin
	int bframes = 0;
	int skip_frame = -1; // RATE_CONTROL_SKIP_FRAME_ENABLE: -1 = left at AMF's default
	std::string usage = "transcoding";
	bool trace = false;
	bool validate = false;
	// timing
	int stall_at = -1, stall_ms = 0; // sleep before producing this frame
	int stall_every = 0;             // ... and every N frames after it
	int fps_sleep_ms = 0;            // sleep per frame (real-time pacing)
	bool host_copy_check = false;
	std::string dump;                // write the bitstream here
	bool quiet = false;
	bool reuse_surface = false; // one AMFSurface per image for the whole run (CreateSurfaceFromVulkanNative once)
	bool spy = false, no_close = false;
	std::string pdev = "full"; // separate producer device: full | min
	bool rt = false;           // COLOR_ATTACHMENT usage (libobs's encoder frames are render targets)
};

void usage()
{
	printf(
		"amf-vulkan-stale-frame [options]\n"
		"  --frames N            frames to encode (120)\n"
		"  --size WxH            (1280x720)\n"
		"  --codec avc|hevc      (avc)\n"
		"  --gpu NAME            substring of the Vulkan device name, or index (AMD)\n"
		"  --producer separate|same   separate VkDevice exporting the images (as OBS) or AMF's own device (separate)\n"
		"  --pqueue graphics|compute1|amf   queue the producer uses on AMF's device (graphics)\n"
		"  --pdev full|min       separate producer: a device made like AMF's, or a minimal one (full)\n"
		"  --ring N              images reused round-robin; 0 = a fresh image every frame (8)\n"
		"  --keep                with --ring 0: never destroy images (no memory reuse)\n"
		"  --sync fence|timeline|idle|devidle|none|gpu   how the producer's copy is waited for before SubmitInput (timeline)\n"
		"  --dedicated 0|1       dedicated allocations (1)\n"
		"  --rt                  COLOR_ATTACHMENT usage on the images (as libobs's encoder frames)\n"
		"  --export 0|1          AMF-side memory allocated exportable (1)\n"
		"  --tiling optimal|linear (optimal)\n"
		"  --format-list 0|1     MUTABLE_FORMAT|EXTENDED_USAGE + R8/R8G8 view formats (1)\n"
		"  --busy N              copy the pattern N times per frame (1)\n"
		"  --input native|alloc  CreateSurfaceFromVulkanNative, or AllocSurface(VULKAN) and copy into it (native)\n"
		"  --release external|none   separate producer: release the image to VK_QUEUE_FAMILY_EXTERNAL (external)\n"
		"  --dx11                also InitDX11 the AMF context (same adapter)\n"
		"  --amf-queue N         AMF_CONTEXT_VULKAN_COMPUTE_QUEUE\n"
		"  --amf-timeline        AMF_CONTEXT_VULKAN_USE_TIMELINE_SEMAPHORES\n"
		"  --preencode 0|1       (1, as OBS)\n"
		"  --bframes N           (0)\n"
		"  --skip-frame 0|1      set RATE_CONTROL_SKIP_FRAME_ENABLE (left at AMF's default, as OBS)\n"
		"  --usage transcoding|lowlatency|ultralowlatency (transcoding)\n"
		"  --stall-at F --stall-ms M [--stall-every N]   sleep M ms before frame F (and every N after)\n"
		"  --pace MS             sleep MS per frame\n"
		"  --trace               AMF trace (debug level) to amf-trace.log\n"
		"  --validate            Vulkan validation layer\n"
		"  --dump FILE           write the elementary stream\n"
		"  --quiet               only the verdict line\n"
		"  --reuse-surface       wrap each image once (CreateSurfaceFromVulkanNative) and resubmit that AMFSurface\n"
		"  --spy                 log AMF's memory exports (vkGetMemoryWin32HandleKHR) and its CloseHandle of them\n"
		"  --no-close            keep the handles AMF exports open (AMF's CloseHandle of them does nothing)\n");
}

options parse(int argc, char **argv)
{
	options o;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		auto next = [&]() -> std::string {
			if (i + 1 >= argc)
				die("missing value after %s", a.c_str());
			return argv[++i];
		};
		auto num = [&]() { return atoi(next().c_str()); };
		if (a == "--help" || a == "-h") {
			usage();
			exit(0);
		} else if (a == "--frames") o.frames = num();
		else if (a == "--size") {
			std::string v = next();
			if (sscanf(v.c_str(), "%ux%u", &o.w, &o.h) != 2)
				die("bad --size");
		} else if (a == "--codec") o.codec = next();
		else if (a == "--gpu") o.gpu = next();
		else if (a == "--producer") o.producer = next();
		else if (a == "--pqueue") o.pqueue = next();
		else if (a == "--ring") o.ring = num();
		else if (a == "--keep") o.keep = true;
		else if (a == "--sync") o.sync = next();
		else if (a == "--dedicated") o.dedicated = num() != 0;
		else if (a == "--export") o.export_mem = num() != 0;
		else if (a == "--tiling") o.tiling = next();
		else if (a == "--format-list") o.format_list = num() != 0;
		else if (a == "--busy") o.busy = std::max(1, num());
		else if (a == "--input") o.input = next();
		else if (a == "--release") o.release = next();
		else if (a == "--dx11") o.dx11 = true;
		else if (a == "--amf-queue") o.amf_queue = num();
		else if (a == "--amf-timeline") o.amf_timeline = true;
		else if (a == "--preencode") o.preencode = num() != 0;
		else if (a == "--bframes") o.bframes = num();
		else if (a == "--skip-frame") o.skip_frame = num() != 0;
		else if (a == "--usage") o.usage = next();
		else if (a == "--stall-at") o.stall_at = num();
		else if (a == "--stall-ms") o.stall_ms = num();
		else if (a == "--stall-every") o.stall_every = num();
		else if (a == "--pace") o.fps_sleep_ms = num();
		else if (a == "--trace") o.trace = true;
		else if (a == "--validate") o.validate = true;
		else if (a == "--dump") o.dump = next();
		else if (a == "--quiet") o.quiet = true;
		else if (a == "--reuse-surface") o.reuse_surface = true;
		else if (a == "--spy") o.spy = true;
		else if (a == "--no-close") o.no_close = true;
		else if (a == "--pdev") o.pdev = next();
		else if (a == "--rt") o.rt = true;
		else die("unknown option %s (--help)", a.c_str());
	}
	if (o.w % 16 || o.h % 16)
		die("--size must be a multiple of 16");
	if (o.producer != "separate" && o.producer != "same")
		die("bad --producer");
	if (o.input == "alloc" && o.producer != "same")
		die("--input alloc needs --producer same");
	if (o.sync == "gpu" && o.producer != "same")
		die("--sync gpu needs --producer same");
	return o;
}

// ---------------------------------------------------------------- test pattern
// Top third: 8x2 cells, one bit each (40 = 0, 215 = 1): bits 0..11 frame number, 12..15 check. Below: a diagonal
// gradient that moves with the frame number (so the encoder has motion to code). Chroma: neutral.
constexpr uint8_t BIT0 = 40, BIT1 = 215;
uint32_t check_of(uint32_t n)
{
	return (~(n ^ (n >> 4) ^ (n >> 8))) & 15;
}
uint32_t code_of(uint32_t n)
{
	return (n & 0xFFF) | (check_of(n & 0xFFF) << 12);
}

void fill_frame(uint8_t *dst, uint32_t w, uint32_t h, uint32_t n)
{
	const uint32_t code = code_of(n);
	const uint32_t cw = w / 8, ch = h / 6, band = ch * 2;
	for (uint32_t y = 0; y < h; y++) {
		uint8_t *row = dst + (size_t)y * w;
		if (y < band) {
			for (uint32_t c = 0; c < 8; c++) {
				const uint32_t bit = (y / ch) * 8 + c;
				memset(row + c * cw, (code >> bit) & 1 ? BIT1 : BIT0, cw);
			}
		} else {
			for (uint32_t x = 0; x < w; x++)
				row[x] = (uint8_t)(16 + ((x + y + n * 8) % 220));
		}
	}
	memset(dst + (size_t)w * h, 128, (size_t)w * h / 2);
}

// Read the code back: -1 blank (black), -2 garbled (check fails), else the frame number.
int read_frame(const uint8_t *y_plane, int stride, uint32_t w, uint32_t h, uint32_t *raw_out)
{
	const uint32_t cw = w / 8, ch = h / 6;
	uint32_t code = 0, dark = 0;
	for (uint32_t bit = 0; bit < 16; bit++) {
		const uint32_t cx = (bit % 8) * cw + cw / 4, cy = (bit / 8) * ch + ch / 4;
		uint64_t sum = 0;
		for (uint32_t y = cy; y < cy + ch / 2; y++)
			for (uint32_t x = cx; x < cx + cw / 2; x++)
				sum += y_plane[(size_t)y * stride + x];
		const uint32_t mean = (uint32_t)(sum / ((cw / 2) * (ch / 2)));
		if (mean < 20)
			dark++;
		if (mean > 128)
			code |= 1u << bit;
	}
	*raw_out = code;
	if (dark == 16)
		return -1;
	const uint32_t n = code & 0xFFF;
	return (code >> 12) == check_of(n) ? (int)n : -2;
}

// ---------------------------------------------------------------- Vulkan
#define VK_GLOBAL_FNS(X) X(vkCreateInstance) X(vkEnumerateInstanceVersion) X(vkEnumerateInstanceExtensionProperties) X(vkEnumerateInstanceLayerProperties)
#define VK_INSTANCE_FNS(X)                                                                              \
	X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties2)           \
	X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties)             \
	X(vkGetPhysicalDeviceFeatures2) X(vkCreateDevice) X(vkGetDeviceProcAddr)                       \
	X(vkEnumerateDeviceExtensionProperties) X(vkGetPhysicalDeviceImageFormatProperties2)
#define VK_DEVICE_FNS(X)                                                                                    \
	X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateImage) X(vkDestroyImage)                          \
	X(vkGetImageMemoryRequirements2) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindImageMemory)          \
	X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory)        \
	X(vkMapMemory) X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers)          \
	X(vkResetCommandBuffer) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdPipelineBarrier)      \
	X(vkCmdCopyBufferToImage) X(vkCreateFence) X(vkDestroyFence) X(vkResetFences) X(vkWaitForFences)   \
	X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkDeviceWaitIdle) X(vkCreateSemaphore) X(vkDestroySemaphore) \
	X(vkWaitSemaphores) X(vkGetMemoryWin32HandleKHR)
#define DECL(f) PFN_##f f = nullptr;
VK_GLOBAL_FNS(DECL)
VK_INSTANCE_FNS(DECL)
#undef DECL
PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;

struct device {
	VkDevice dev = VK_NULL_HANDLE;
#define DECL(f) PFN_##f f = nullptr;
	VK_DEVICE_FNS(DECL)
#undef DECL
	void load()
	{
#define LOAD(f) f = (PFN_##f)vkGetDeviceProcAddr(dev, #f);
		VK_DEVICE_FNS(LOAD)
#undef LOAD
	}
};

VkInstance instance;
VkPhysicalDevice phys;
VkPhysicalDeviceMemoryProperties memprops;
std::vector<VkQueueFamilyProperties> families;
uint8_t device_luid[VK_LUID_SIZE];
bool luid_valid = false;

bool has_ext(const std::vector<VkExtensionProperties> &all, const char *n)
{
	for (auto &e : all)
		if (!strcmp(e.extensionName, n))
			return true;
	return false;
}

uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
	for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want)
			return i;
	die("no memory type for bits 0x%x flags 0x%x", bits, want);
}

uint32_t family_with(VkQueueFlags want, VkQueueFlags not_)
{
	for (uint32_t i = 0; i < families.size(); i++)
		if ((families[i].queueFlags & want) == want && !(families[i].queueFlags & not_))
			return i;
	return UINT32_MAX;
}

void create_instance(const options &o)
{
	HMODULE lib = LoadLibraryA("vulkan-1.dll");
	if (!lib)
		die("cannot load vulkan-1.dll");
	vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)GetProcAddress(lib, "vkGetInstanceProcAddr");
#define LOAD(f) f = (PFN_##f)vkGetInstanceProcAddr(nullptr, #f);
	VK_GLOBAL_FNS(LOAD)
#undef LOAD
	uint32_t n = 0;
	vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
	std::vector<VkExtensionProperties> iexts(n);
	vkEnumerateInstanceExtensionProperties(nullptr, &n, iexts.data());
	// AMF enables VK_KHR_swapchain (needs the surface extensions) and looks instance functions up by their KHR names.
	std::vector<const char *> enable;
	for (const char *e : {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
			      VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
			      VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
			      VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
			      VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME})
		if (has_ext(iexts, e))
			enable.push_back(e);
	const char *layer = "VK_LAYER_KHRONOS_validation";
	VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.pApplicationName = "amf-vulkan-stale-frame";
	app.apiVersion = VK_API_VERSION_1_3;
	VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	ici.enabledExtensionCount = (uint32_t)enable.size();
	ici.ppEnabledExtensionNames = enable.data();
	if (o.validate) {
		ici.enabledLayerCount = 1;
		ici.ppEnabledLayerNames = &layer;
	}
	VKC(vkCreateInstance(&ici, nullptr, &instance));
#define LOAD(f) f = (PFN_##f)vkGetInstanceProcAddr(instance, #f);
	VK_INSTANCE_FNS(LOAD)
#undef LOAD

	vkEnumeratePhysicalDevices(instance, &n, nullptr);
	std::vector<VkPhysicalDevice> all(n);
	vkEnumeratePhysicalDevices(instance, &n, all.data());
	const bool by_index = !o.gpu.empty() && isdigit((unsigned char)o.gpu[0]);
	for (uint32_t i = 0; i < n; i++) {
		VkPhysicalDeviceDriverProperties drv = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
		VkPhysicalDeviceIDProperties idp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES, &drv};
		VkPhysicalDeviceProperties2 p = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &idp};
		vkGetPhysicalDeviceProperties2(all[i], &p);
		const bool pick = by_index ? (uint32_t)atoi(o.gpu.c_str()) == i
					   : strstr(p.properties.deviceName, o.gpu.c_str()) != nullptr;
		if (pick && !phys) {
			phys = all[i];
			memcpy(device_luid, idp.deviceLUID, VK_LUID_SIZE);
			luid_valid = idp.deviceLUIDValid;
			printf("GPU: %s, %s %s, Vulkan %u.%u.%u\n", p.properties.deviceName, drv.driverName, drv.driverInfo,
			       VK_API_VERSION_MAJOR(p.properties.apiVersion), VK_API_VERSION_MINOR(p.properties.apiVersion),
			       VK_API_VERSION_PATCH(p.properties.apiVersion));
		}
	}
	if (!phys)
		die("no Vulkan device matches --gpu %s", o.gpu.c_str());
	vkGetPhysicalDeviceMemoryProperties(phys, &memprops);
	vkGetPhysicalDeviceQueueFamilyProperties(phys, &n, nullptr);
	families.resize(n);
	vkGetPhysicalDeviceQueueFamilyProperties(phys, &n, families.data());
}

// AMF's device: AMF's extensions + external memory/semaphore, every queue of every family, every core feature.
device create_amf_device(const std::vector<const char *> &amf_exts)
{
	uint32_t n = 0;
	vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, nullptr);
	std::vector<VkExtensionProperties> avail(n);
	vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, avail.data());
	std::vector<const char *> enable;
	for (const char *e : amf_exts) {
		if (has_ext(avail, e))
			enable.push_back(e);
		else
			printf("warning: AMF asks for %s, which the device lacks\n", e);
	}
	for (const char *e : {VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME})
		if (std::none_of(enable.begin(), enable.end(), [&](const char *x) { return !strcmp(x, e); }))
			enable.push_back(e);

	std::vector<std::vector<float>> prio(families.size());
	std::vector<VkDeviceQueueCreateInfo> qcis;
	for (uint32_t i = 0; i < families.size(); i++) {
		prio[i].assign(families[i].queueCount, 1.0f);
		VkDeviceQueueCreateInfo q = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
		q.queueFamilyIndex = i;
		q.queueCount = families[i].queueCount;
		q.pQueuePriorities = prio[i].data();
		qcis.push_back(q);
	}
	VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
	VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
	VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
	VkPhysicalDeviceFeatures2 feat = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
	vkGetPhysicalDeviceFeatures2(phys, &feat);
	feat.features.robustBufferAccess = VK_FALSE;
	f13.robustImageAccess = VK_FALSE;
	VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &feat};
	dci.queueCreateInfoCount = (uint32_t)qcis.size();
	dci.pQueueCreateInfos = qcis.data();
	dci.enabledExtensionCount = (uint32_t)enable.size();
	dci.ppEnabledExtensionNames = enable.data();
	device d;
	VKC(vkCreateDevice(phys, &dci, nullptr, &d.dev));
	d.load();
	return d;
}

// The producer's own device (as libobs's renderer): one queue of `family`, external memory, timelines.
device create_producer_device(uint32_t family)
{
	const char *exts[] = {VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME};
	float one = 1.0f;
	VkDeviceQueueCreateInfo q = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	q.queueFamilyIndex = family;
	q.queueCount = 1;
	q.pQueuePriorities = &one;
	VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
	f12.timelineSemaphore = VK_TRUE;
	VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &f12};
	f13.synchronization2 = VK_TRUE;
	VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f13};
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &q;
	dci.enabledExtensionCount = 2;
	dci.ppEnabledExtensionNames = exts;
	device d;
	VKC(vkCreateDevice(phys, &dci, nullptr, &d.dev));
	d.load();
	return d;
}

const VkFormat NV12 = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
const VkFormat view_formats[2] = {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM};

VkImage make_image(device &d, const options &o, bool external)
{
	VkExternalMemoryImageCreateInfo emi = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
	emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
	VkImageFormatListCreateInfo fl = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, external ? &emi : nullptr};
	fl.viewFormatCount = 2;
	fl.pViewFormats = view_formats;
	VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ii.pNext = o.format_list ? (void *)&fl : (external ? (void *)&emi : nullptr);
	ii.flags = o.format_list ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT : 0;
	ii.imageType = VK_IMAGE_TYPE_2D;
	ii.format = NV12;
	ii.extent = {o.w, o.h, 1};
	ii.mipLevels = 1;
	ii.arrayLayers = 1;
	ii.samples = VK_SAMPLE_COUNT_1_BIT;
	ii.tiling = o.tiling == "linear" ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
	ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
		   (o.rt ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0);
	ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkImage img;
	VKC(d.vkCreateImage(d.dev, &ii, nullptr, &img));
	return img;
}

// Memory for `img`: exportable and/or imported from `import_handle`.
VkDeviceMemory bind_memory(device &d, VkImage img, const options &o, bool exportable, HANDLE import_handle,
			   VkDeviceSize *size_out)
{
	VkImageMemoryRequirementsInfo2 mri = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
	mri.image = img;
	VkMemoryRequirements2 mr = {VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
	d.vkGetImageMemoryRequirements2(d.dev, &mri, &mr);
	const void *chain = nullptr;
	VkMemoryDedicatedAllocateInfo dai = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
	dai.image = img;
	if (o.dedicated)
		chain = &dai;
	VkExportMemoryAllocateInfo exp = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, chain};
	exp.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
	if (exportable)
		chain = &exp;
	VkImportMemoryWin32HandleInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, chain};
	imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
	imp.handle = import_handle;
	if (import_handle)
		chain = &imp;
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, chain};
	ai.allocationSize = size_out && *size_out ? *size_out : mr.memoryRequirements.size;
	ai.memoryTypeIndex = mem_type(mr.memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VkDeviceMemory mem;
	VKC(d.vkAllocateMemory(d.dev, &ai, nullptr, &mem));
	VKC(d.vkBindImageMemory(d.dev, img, mem, 0));
	if (size_out)
		*size_out = ai.allocationSize;
	return mem;
}

struct staging {
	VkBuffer buf = VK_NULL_HANDLE;
	VkDeviceMemory mem = VK_NULL_HANDLE;
	uint8_t *map = nullptr;
};

staging make_staging(device &d, VkDeviceSize size)
{
	staging s;
	VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bi.size = size;
	bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	VKC(d.vkCreateBuffer(d.dev, &bi, nullptr, &s.buf));
	VkMemoryRequirements mr;
	d.vkGetBufferMemoryRequirements(d.dev, s.buf, &mr);
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = mr.size;
	ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
								 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VKC(d.vkAllocateMemory(d.dev, &ai, nullptr, &s.mem));
	VKC(d.vkBindBufferMemory(d.dev, s.buf, s.mem, 0));
	VKC(d.vkMapMemory(d.dev, s.mem, 0, VK_WHOLE_SIZE, 0, (void **)&s.map));
	return s;
}

// ---------------------------------------------------------------- AMF
amf::AMFFactory *factory;
amf_uint64 amf_version;
extern bool spy, no_close;
void patch_imports(HMODULE mod);

void load_amf(const options &o)
{
	HMODULE lib = LoadLibraryW(AMF_DLL_NAME);
	if (!lib)
		die("cannot load %s", AMF_DLL_NAMEA);
	auto query = (AMFQueryVersion_Fn)GetProcAddress(lib, AMF_QUERY_VERSION_FUNCTION_NAME);
	auto init = (AMFInit_Fn)GetProcAddress(lib, AMF_INIT_FUNCTION_NAME);
	if (!query || !init)
		die("amfrt64.dll lacks AMFInit/AMFQueryVersion");
	AMFC(query(&amf_version));
	AMFC(init(AMF_FULL_VERSION, &factory));
	if (spy || no_close) {
		// amfrt64.dll in System32 only loads the driver's runtime, amfrtdrv64.dll
		HMODULE drv = GetModuleHandleA("amfrtdrv64.dll");
		patch_imports(drv ? drv : lib);
	}
	wchar_t path[MAX_PATH] = {};
	GetModuleFileNameW(lib, path, MAX_PATH);
	printf("AMF runtime %u.%u.%u.%u (%ls), SDK headers %u.%u.%u\n", (unsigned)AMF_GET_MAJOR_VERSION(amf_version),
	       (unsigned)AMF_GET_MINOR_VERSION(amf_version), (unsigned)AMF_GET_SUBMINOR_VERSION(amf_version),
	       (unsigned)AMF_GET_BUILD_VERSION(amf_version), path, AMF_VERSION_MAJOR, AMF_VERSION_MINOR,
	       AMF_VERSION_RELEASE);
	amf::AMFTrace *trace = nullptr;
	factory->GetTrace(&trace);
	if (o.trace) {
		trace->SetGlobalLevel(AMF_TRACE_DEBUG);
		trace->SetWriterLevel(AMF_TRACE_WRITER_FILE, AMF_TRACE_DEBUG);
		trace->SetPath(L"amf-trace.log");
		trace->EnableWriter(AMF_TRACE_WRITER_FILE, true);
		trace->EnableWriter(AMF_TRACE_WRITER_CONSOLE, false);
		trace->EnableWriter(AMF_TRACE_WRITER_DEBUG_OUTPUT, false);
	} else {
		trace->SetGlobalLevel(AMF_TRACE_ERROR);
		trace->EnableWriter(AMF_TRACE_WRITER_CONSOLE, false);
		trace->EnableWriter(AMF_TRACE_WRITER_DEBUG_OUTPUT, false);
	}
}

ID3D11Device *make_d3d11()
{
	IDXGIFactory1 *f = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&f)))
		die("CreateDXGIFactory1");
	IDXGIAdapter1 *a = nullptr;
	for (UINT i = 0; f->EnumAdapters1(i, &a) == S_OK; i++) {
		DXGI_ADAPTER_DESC1 desc;
		a->GetDesc1(&desc);
		if (luid_valid && !memcmp(&desc.AdapterLuid, device_luid, sizeof(LUID)))
			break;
		a->Release();
		a = nullptr;
	}
	if (!a)
		die("no DXGI adapter with the Vulkan device's LUID");
	ID3D11Device *dev = nullptr;
	const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
	if (FAILED(D3D11CreateDevice(a, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr,
				     nullptr)))
		die("D3D11CreateDevice");
	return dev;
}

// ---------------------------------------------------------------- frames
struct slot {
	// AMF device side
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkDeviceSize size = 0;
	amf::AMFVulkanSurface surf = {};
	// producer side (separate device only)
	VkImage pimage = VK_NULL_HANDLE;
	VkDeviceMemory pmemory = VK_NULL_HANDLE;
	bool initialized = false;
	std::atomic<bool> busy{false};
	int frame = -1;
	amf::AMFSurfacePtr held; // --reuse-surface
};

struct observer : amf::AMFSurfaceObserver {
	std::mutex m;
	std::vector<int> released; // slot ids
	void AMF_STD_CALL OnSurfaceDataRelease(amf::AMFSurface *s) override
	{
		amf_int64 id = -1;
		s->GetProperty(L"ReproSlot", &id);
		std::scoped_lock l(m);
		released.push_back((int)id);
	}
};

struct packet {
	std::vector<uint8_t> data;
	int64_t pts;
};

// ---------------------------------------------------------------- spying on AMF (--spy, --no-close)
// amfrt64.dll's imports of GetProcAddress and CloseHandle are patched: GetProcAddress("vkGetInstanceProcAddr") hands
// AMF a wrapper whose vkGetDeviceProcAddr wraps vkGetMemoryWin32HandleKHR, so every memory export AMF makes is
// logged (memory -> NT handle), and so is every CloseHandle of such a handle. --no-close makes AMF's CloseHandle of
// an exported memory handle a no-op (the handle stays open, so its value is never reused).
bool spy = false, no_close = false;
std::mutex spy_m;
std::vector<std::pair<HANDLE, VkDeviceMemory>> spy_open; // exported by AMF, not yet closed
int spy_exports = 0, spy_closes = 0, spy_value_reuse = 0;
std::vector<HANDLE> spy_seen_values;
PFN_vkGetDeviceProcAddr real_gdpa;
PFN_vkGetMemoryWin32HandleKHR real_export;
decltype(&CloseHandle) real_close;
decltype(&GetProcAddress) real_gpa;

VKAPI_ATTR VkResult VKAPI_CALL spy_export(VkDevice d, const VkMemoryGetWin32HandleInfoKHR *info, HANDLE *h)
{
	VkResult r = real_export(d, info, h);
	std::scoped_lock l(spy_m);
	spy_exports++;
	const bool reused = std::find(spy_seen_values.begin(), spy_seen_values.end(), *h) != spy_seen_values.end();
	if (reused)
		spy_value_reuse++;
	else
		spy_seen_values.push_back(*h);
	spy_open.push_back({*h, info->memory});
	if (spy)
		printf("spy: AMF exported memory %p -> handle %p%s\n", (void *)info->memory, *h,
		       reused ? " (value seen before)" : "");
	return r;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL spy_gdpa(VkDevice d, const char *name)
{
	if (!strcmp(name, "vkGetMemoryWin32HandleKHR")) {
		real_export = (PFN_vkGetMemoryWin32HandleKHR)real_gdpa(d, name);
		return real_export ? (PFN_vkVoidFunction)spy_export : nullptr;
	}
	if (!strcmp(name, "vkGetDeviceProcAddr"))
		return (PFN_vkVoidFunction)spy_gdpa;
	return real_gdpa(d, name);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL spy_gipa(VkInstance i, const char *name)
{
	if (!strcmp(name, "vkGetDeviceProcAddr")) {
		real_gdpa = (PFN_vkGetDeviceProcAddr)vkGetInstanceProcAddr(i, name);
		return (PFN_vkVoidFunction)spy_gdpa;
	}
	if (!strcmp(name, "vkGetInstanceProcAddr"))
		return (PFN_vkVoidFunction)spy_gipa;
	if (!strcmp(name, "vkGetMemoryWin32HandleKHR")) {
		real_export = (PFN_vkGetMemoryWin32HandleKHR)vkGetInstanceProcAddr(i, name);
		return real_export ? (PFN_vkVoidFunction)spy_export : nullptr;
	}
	if (spy && !strncmp(name, "vkGetMemory", 11))
		printf("spy: AMF looks up %s\n", name);
	return vkGetInstanceProcAddr(i, name);
}
FARPROC WINAPI spy_getprocaddress(HMODULE m, LPCSTR name)
{
	if (spy && (uintptr_t)name > 0xFFFF && !strncmp(name, "vk", 2))
		printf("spy: AMF GetProcAddress(%s)\n", name);
	if ((uintptr_t)name > 0xFFFF && !strcmp(name, "vkGetInstanceProcAddr"))
		return (FARPROC)spy_gipa;
	if ((uintptr_t)name > 0xFFFF && !strcmp(name, "vkGetDeviceProcAddr")) {
		real_gdpa = (PFN_vkGetDeviceProcAddr)real_gpa(m, name);
		return (FARPROC)spy_gdpa;
	}
	return real_gpa(m, name);
}
BOOL WINAPI spy_closehandle(HANDLE h)
{
	{
		std::scoped_lock l(spy_m);
		auto it = std::find_if(spy_open.begin(), spy_open.end(), [&](auto &p) { return p.first == h; });
		if (it != spy_open.end()) {
			spy_closes++;
			if (spy)
				printf("spy: AMF %s handle %p (memory %p)\n", no_close ? "would close" : "closes", h,
				       (void *)it->second);
			spy_open.erase(it);
			if (no_close)
				return TRUE;
		}
	}
	return real_close(h);
}

void patch_imports(HMODULE mod)
{
	auto base = (uint8_t *)mod;
	auto dos = (IMAGE_DOS_HEADER *)base;
	auto nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
	auto &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	int patched = 0;
	for (auto imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); imp->Name; imp++) {
		auto names = (IMAGE_THUNK_DATA *)(base + imp->OriginalFirstThunk);
		auto funcs = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
		for (; names->u1.AddressOfData; names++, funcs++) {
			if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
				continue;
			const char *fn = (const char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name;
			void *repl = nullptr;
			if (!strcmp(fn, "GetProcAddress")) {
				real_gpa = (decltype(real_gpa))funcs->u1.Function;
				repl = (void *)spy_getprocaddress;
			} else if (!strcmp(fn, "CloseHandle")) {
				real_close = (decltype(real_close))funcs->u1.Function;
				repl = (void *)spy_closehandle;
			}
			if (repl) {
				DWORD old;
				VirtualProtect(&funcs->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
				funcs->u1.Function = (ULONG_PTR)repl;
				VirtualProtect(&funcs->u1.Function, sizeof(void *), old, &old);
				patched++;
			}
		}
	}
	char name[MAX_PATH];
	GetModuleFileNameA(mod, name, MAX_PATH);
	printf("spy: patched %d imports of %s\n", patched, name);
}

} // namespace

int main(int argc, char **argv)
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	options o = parse(argc, argv);
	spy = o.spy;
	no_close = o.no_close;
	create_instance(o);
	load_amf(o);

	amf::AMFContextPtr ctx;
	AMFC(factory->CreateContext(&ctx));
	amf::AMFContext1Ptr ctx1(ctx);
	if (!ctx1)
		die("no AMFContext1");
	amf_size next = 0;
	AMFC(ctx1->GetVulkanDeviceExtensions(&next, nullptr));
	std::vector<const char *> amf_exts(next);
	AMFC(ctx1->GetVulkanDeviceExtensions(&next, amf_exts.data()));

	device ad = create_amf_device(amf_exts);
	const uint32_t gfx_family = family_with(VK_QUEUE_GRAPHICS_BIT, 0);
	const uint32_t comp_family = family_with(VK_QUEUE_COMPUTE_BIT, VK_QUEUE_GRAPHICS_BIT);

	if (o.dx11) {
		ID3D11Device *d3d = make_d3d11();
		AMFC(ctx->InitDX11(d3d, amf::AMF_DX11_1));
	}
	if (o.amf_queue >= 0)
		AMFC(ctx->SetProperty(AMF_CONTEXT_VULKAN_COMPUTE_QUEUE, (amf_int64)o.amf_queue));
	if (o.amf_timeline)
		AMFC(ctx->SetProperty(AMF_CONTEXT_VULKAN_USE_TIMELINE_SEMAPHORES, true));
	amf::AMFVulkanDevice amf_dev = {};
	amf_dev.cbSizeof = sizeof amf_dev;
	amf_dev.hInstance = instance;
	amf_dev.hPhysicalDevice = phys;
	amf_dev.hDevice = ad.dev;
	AMFC(ctx1->InitVulkan(&amf_dev));
	if (o.spy) {
		HMODULE mods[512];
		DWORD need = 0;
		EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need);
		for (DWORD i = 0; i < need / sizeof(HMODULE); i++) {
			char name[MAX_PATH];
			GetModuleFileNameA(mods[i], name, MAX_PATH);
			if (!strstr(name, "Windows\\System32\\") || strstr(name, "amd") || strstr(name, "amf") ||
			    strstr(name, "DriverStore"))
				printf("spy: module %s\n", name);
		}
	}

	// producer device / queue
	const bool separate = o.producer == "separate";
	device pd_own;
	device &pd = separate ? pd_own : ad;
	uint32_t pfamily = gfx_family, pindex = 0;
	bool lock_amf_queue = false;
	if (separate) {
		// "full": a second device made exactly like AMF's (all features); "min": one queue, external memory only
		pd_own = o.pdev == "full" ? create_amf_device(amf_exts) : create_producer_device(gfx_family);
	} else if (o.pqueue == "compute1") {
		pfamily = comp_family;
		pindex = families[comp_family].queueCount > 1 ? 1 : 0;
		if (o.amf_queue == (int)pindex)
			pindex = 0;
	} else if (o.pqueue == "amf") {
		pfamily = comp_family;
		pindex = o.amf_queue >= 0 ? (uint32_t)o.amf_queue : 0;
		lock_amf_queue = true;
	} else if (o.pqueue != "graphics") {
		die("bad --pqueue");
	}
	VkQueue pq;
	pd.vkGetDeviceQueue(pd.dev, pfamily, pindex, &pq);

	// encoder
	const bool hevc = o.codec == "hevc";
	amf::AMFComponentPtr enc;
	AMFC(factory->CreateComponent(ctx, hevc ? AMFVideoEncoder_HEVC : AMFVideoEncoderVCE_AVC, &enc));
	if (hevc) {
		AMFC(enc->SetProperty(AMF_VIDEO_ENCODER_HEVC_USAGE,
				      o.usage == "lowlatency"	     ? AMF_VIDEO_ENCODER_HEVC_USAGE_LOW_LATENCY
				      : o.usage == "ultralowlatency" ? AMF_VIDEO_ENCODER_HEVC_USAGE_ULTRA_LOW_LATENCY
								     : AMF_VIDEO_ENCODER_HEVC_USAGE_TRANSCODING));
		AMFC(enc->SetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMESIZE, ::AMFConstructSize(o.w, o.h)));
		AMFC(enc->SetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMERATE, ::AMFConstructRate(60, 1)));
		AMFC(enc->SetProperty(AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD,
				      AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR));
		AMFC(enc->SetProperty(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, (amf_int64)20000000));
		AMFC(enc->SetProperty(AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE, (amf_int64)20000000));
		enc->SetProperty(AMF_VIDEO_ENCODER_HEVC_LOWLATENCY_MODE, false);
		enc->SetProperty(AMF_VIDEO_ENCODER_HEVC_PREENCODE_ENABLE, o.preencode);
		if (o.skip_frame >= 0)
			AMFC(enc->SetProperty(AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_SKIP_FRAME_ENABLE, o.skip_frame != 0));
	} else {
		AMFC(enc->SetProperty(AMF_VIDEO_ENCODER_USAGE,
				      o.usage == "lowlatency"	     ? AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY
				      : o.usage == "ultralowlatency" ? AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY
								     : AMF_VIDEO_ENCODER_USAGE_TRANSCODING));
		AMFC(enc->SetProperty(AMF_VIDEO_ENCODER_FRAMESIZE, ::AMFConstructSize(o.w, o.h)));
		AMFC(enc->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE, ::AMFConstructRate(60, 1)));
		AMFC(enc->SetProperty(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD, AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR));
		AMFC(enc->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, (amf_int64)20000000));
		AMFC(enc->SetProperty(AMF_VIDEO_ENCODER_PEAK_BITRATE, (amf_int64)20000000));
		enc->SetProperty(AMF_VIDEO_ENCODER_LOWLATENCY_MODE, false);
		enc->SetProperty(AMF_VIDEO_ENCODER_PREENCODE_ENABLE, o.preencode);
		enc->SetProperty(AMF_VIDEO_ENCODER_B_PIC_PATTERN, (amf_int64)o.bframes);
		if (o.skip_frame >= 0)
			AMFC(enc->SetProperty(AMF_VIDEO_ENCODER_RATE_CONTROL_SKIP_FRAME_ENABLE, o.skip_frame != 0));
	}
	AMFC(enc->Init(amf::AMF_SURFACE_NV12, o.w, o.h));
	{
		bool skip = false;
		AMF_RESULT r = enc->GetProperty(hevc ? AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_SKIP_FRAME_ENABLE
						     : AMF_VIDEO_ENCODER_RATE_CONTROL_SKIP_FRAME_ENABLE,
					      &skip);
		if (!o.quiet)
			printf("rate-control skip frame: %s (%s)\n", r == AMF_OK ? (skip ? "on" : "off") : "unknown",
			       o.skip_frame >= 0 ? "set" : "AMF default");
	}

	// frames
	const VkDeviceSize frame_bytes = (VkDeviceSize)o.w * o.h * 3 / 2;
	const int nslots = o.ring > 0 ? o.ring : o.frames;
	std::vector<slot> slots(nslots);
	const int nstage = 4;
	std::vector<staging> stages(nstage);
	for (auto &s : stages)
		s = make_staging(pd, frame_bytes);
	VkCommandPool pool;
	VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	cpi.queueFamilyIndex = pfamily;
	VKC(pd.vkCreateCommandPool(pd.dev, &cpi, nullptr, &pool));
	std::vector<VkCommandBuffer> cmds(nstage);
	VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cai.commandPool = pool;
	cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cai.commandBufferCount = nstage;
	VKC(pd.vkAllocateCommandBuffers(pd.dev, &cai, cmds.data()));
	std::vector<VkFence> fences(nstage);
	for (auto &f : fences) {
		VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		VKC(pd.vkCreateFence(pd.dev, &fi, nullptr, &f));
	}
	VkSemaphoreTypeCreateInfo sti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
	sti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
	VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &sti};
	VkSemaphore timeline;
	VKC(pd.vkCreateSemaphore(pd.dev, &sci, nullptr, &timeline));
	std::vector<VkSemaphore> binsems(nstage);
	for (auto &s : binsems) {
		VkSemaphoreCreateInfo bci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		VKC(pd.vkCreateSemaphore(pd.dev, &bci, nullptr, &s));
	}

	observer obs;
	std::vector<packet> packets;
	int destroyed = 0;
	auto destroy_slot = [&](slot &s) {
		ad.vkDestroyImage(ad.dev, s.image, nullptr);
		ad.vkFreeMemory(ad.dev, s.memory, nullptr);
		if (separate) {
			pd.vkDestroyImage(pd.dev, s.pimage, nullptr);
			pd.vkFreeMemory(pd.dev, s.pmemory, nullptr);
		}
		s.image = s.pimage = VK_NULL_HANDLE;
		s.memory = s.pmemory = VK_NULL_HANDLE;
		destroyed++;
	};
	auto pump = [&]() {
		for (;;) {
			amf::AMFDataPtr data;
			AMF_RESULT r = enc->QueryOutput(&data);
			if (data) {
				amf::AMFBufferPtr buf(data);
				packet p;
				p.data.assign((uint8_t *)buf->GetNative(), (uint8_t *)buf->GetNative() + buf->GetSize());
				p.pts = data->GetPts();
				packets.push_back(std::move(p));
				continue;
			}
			if (r != AMF_OK && r != AMF_REPEAT && r != AMF_EOF && r != AMF_NEED_MORE_INPUT)
				die("QueryOutput -> %d", (int)r);
			break;
		}
		std::vector<int> rel;
		{
			std::scoped_lock l(obs.m);
			rel.swap(obs.released);
		}
		for (int id : rel) {
			if (id < 0 || id >= nslots)
				continue;
			slots[id].busy = false;
			if (o.ring == 0 && !o.keep && o.input == "native")
				destroy_slot(slots[id]);
		}
	};

	std::vector<void *> surf_ptrs; // AMFSurface object of each frame
	auto t0 = std::chrono::steady_clock::now();
	uint64_t tl_value = 0;
	int max_held = 0;
	for (int n = 0; n < o.frames; n++) {
		pump();
		if (o.stall_at >= 0 && (n == o.stall_at || (o.stall_every > 0 && n > o.stall_at &&
							     (n - o.stall_at) % o.stall_every == 0)))
			std::this_thread::sleep_for(std::chrono::milliseconds(o.stall_ms));
		if (o.fps_sleep_ms)
			std::this_thread::sleep_for(std::chrono::milliseconds(o.fps_sleep_ms));

		const int sid = o.ring > 0 ? n % o.ring : n;
		slot &s = slots[sid];
		// wait for AMF to give the slot back
		auto tw = std::chrono::steady_clock::now();
		auto amf_holds = [](slot &x) {
			if (!x.held)
				return false;
			x.held->Acquire();
			return x.held->Release() > 1; // a reference besides ours: AMF still has it
		};
		while (s.busy || (o.reuse_surface && amf_holds(s))) {
			pump();
			if (std::chrono::steady_clock::now() - tw > std::chrono::seconds(5))
				die("AMF has not released slot %d (frame %d) after 5 s; try a larger --ring", sid, s.frame);
			std::this_thread::sleep_for(std::chrono::microseconds(200));
		}
		{
			int held = 0;
			for (auto &x : slots)
				held += x.busy || (o.reuse_surface && amf_holds(x)) ? 1 : 0;
			max_held = std::max(max_held, held);
		}

		amf::AMFSurfacePtr surf;
		VkImage target = VK_NULL_HANDLE;
		VkImageLayout old_layout = VK_IMAGE_LAYOUT_UNDEFINED, new_layout = VK_IMAGE_LAYOUT_GENERAL;
		amf::AMFVulkanSurface *vs = nullptr;
		if (o.input == "alloc") {
			AMFC(ctx->AllocSurface(amf::AMF_MEMORY_VULKAN, amf::AMF_SURFACE_NV12, o.w, o.h, &surf));
			auto *view = (amf::AMFVulkanView *)surf->GetPlaneAt(0)->GetNative();
			vs = view->pSurface;
			target = vs->hImage;
			old_layout = (VkImageLayout)vs->eCurrentLayout;
			new_layout = old_layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_GENERAL : old_layout;
		} else {
			if (!s.image) {
				s.image = make_image(ad, o, separate || o.export_mem);
				if (separate) {
					s.pimage = make_image(pd, o, true);
					VkDeviceSize psize = 0;
					s.pmemory = bind_memory(pd, s.pimage, o, true, nullptr, &psize);
					VkMemoryGetWin32HandleInfoKHR gi = {VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
					gi.memory = s.pmemory;
					gi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
					HANDLE h = nullptr;
					VKC(pd.vkGetMemoryWin32HandleKHR(pd.dev, &gi, &h));
					s.size = psize;
					s.memory = bind_memory(ad, s.image, o, o.export_mem, h, &s.size);
					CloseHandle(h);
				} else {
					s.memory = bind_memory(ad, s.image, o, o.export_mem, nullptr, &s.size);
				}
				s.initialized = false;
				s.surf = {};
				s.surf.cbSizeof = sizeof s.surf;
				s.surf.hImage = s.image;
				s.surf.hMemory = s.memory;
				s.surf.iSize = (amf_int64)s.size;
				s.surf.eFormat = NV12;
				s.surf.iWidth = (amf_int32)o.w;
				s.surf.iHeight = (amf_int32)o.h;
				s.surf.eUsage = amf::AMF_SURFACE_USAGE_SHADER_RESOURCE | amf::AMF_SURFACE_USAGE_TRANSFER_SRC |
						amf::AMF_SURFACE_USAGE_TRANSFER_DST |
						(o.rt ? amf::AMF_SURFACE_USAGE_RENDER_TARGET : 0);
				s.surf.eAccess = amf::AMF_MEMORY_CPU_NONE;
				s.surf.Sync.cbSizeof = sizeof s.surf.Sync;
			}
			target = separate ? s.pimage : s.image;
			old_layout = s.initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
			if (s.held) // AMF's view of the image: the struct it keeps for this surface
				vs = ((amf::AMFVulkanView *)s.held->GetPlaneAt(0)->GetNative())->pSurface;
			if (!separate && s.initialized)
				old_layout = (VkImageLayout)(vs ? vs->eCurrentLayout : s.surf.eCurrentLayout); // AMF may have changed it
		}
		s.frame = n;

		// produce: host pattern -> staging -> image (busy times), leave in GENERAL
		const int st = n % nstage;
		VKC(pd.vkWaitForFences(pd.dev, 1, &fences[st], VK_TRUE, UINT64_MAX));
		VKC(pd.vkResetFences(pd.dev, 1, &fences[st]));
		fill_frame(stages[st].map, o.w, o.h, (uint32_t)n);
		VkCommandBuffer cb = cmds[st];
		VKC(pd.vkResetCommandBuffer(cb, 0));
		VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		VKC(pd.vkBeginCommandBuffer(cb, &bi));
		VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		b.oldLayout = old_layout;
		b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		if (separate && o.release == "external" && s.initialized) {
			b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
			b.dstQueueFamilyIndex = pfamily;
		}
		b.image = target;
		b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		pd.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
					nullptr, 0, nullptr, 1, &b);
		VkBufferImageCopy regions[2] = {};
		regions[0].imageSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1};
		regions[0].imageExtent = {o.w, o.h, 1};
		regions[1].bufferOffset = (VkDeviceSize)o.w * o.h;
		regions[1].imageSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1};
		regions[1].imageExtent = {o.w / 2, o.h / 2, 1};
		for (int k = 0; k < o.busy; k++) {
			pd.vkCmdCopyBufferToImage(cb, stages[st].buf, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2,
						  regions);
			if (k + 1 < o.busy) {
				VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
				mb.srcAccessMask = mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				pd.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
							1, &mb, 0, nullptr, 0, nullptr);
			}
		}
		b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		b.newLayout = new_layout;
		b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		if (separate && o.release == "external") {
			b.srcQueueFamilyIndex = pfamily;
			b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
		}
		pd.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
					nullptr, 0, nullptr, 1, &b);
		VKC(pd.vkEndCommandBuffer(cb));

		VkTimelineSemaphoreSubmitInfo tsi = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
		VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
		si.commandBufferCount = 1;
		si.pCommandBuffers = &cb;
		std::vector<VkSemaphore> signal;
		std::vector<uint64_t> values;
		signal.push_back(timeline);
		values.push_back(++tl_value);
		if (o.sync == "gpu") {
			signal.push_back(binsems[st]);
			values.push_back(0);
		}
		tsi.signalSemaphoreValueCount = (uint32_t)values.size();
		tsi.pSignalSemaphoreValues = values.data();
		si.pNext = &tsi;
		si.signalSemaphoreCount = (uint32_t)signal.size();
		si.pSignalSemaphores = signal.data();
		// --input alloc: AMF's last use of its surface (binary semaphore) is waited for on the GPU
		VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		uint64_t zero = 0;
		VkSemaphore amf_wait = VK_NULL_HANDLE;
		if (vs && vs->Sync.hSemaphore && vs->Sync.bSubmitted) {
			amf_wait = vs->Sync.hSemaphore;
			vs->Sync.bSubmitted = false;
			si.waitSemaphoreCount = 1;
			si.pWaitSemaphores = &amf_wait;
			si.pWaitDstStageMask = &wait_stage;
			tsi.waitSemaphoreValueCount = 1;
			tsi.pWaitSemaphoreValues = &zero;
		}
		if (lock_amf_queue)
			ctx1->LockVulkan();
		VKC(pd.vkQueueSubmit(pq, 1, &si, fences[st]));
		if (lock_amf_queue)
			ctx1->UnlockVulkan();
		s.initialized = true;

		if (o.sync == "fence") {
			VKC(pd.vkWaitForFences(pd.dev, 1, &fences[st], VK_TRUE, UINT64_MAX));
		} else if (o.sync == "timeline") {
			VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
			wi.semaphoreCount = 1;
			wi.pSemaphores = &timeline;
			wi.pValues = &tl_value;
			VKC(pd.vkWaitSemaphores(pd.dev, &wi, UINT64_MAX));
		} else if (o.sync == "idle") {
			if (lock_amf_queue)
				ctx1->LockVulkan();
			VKC(pd.vkQueueWaitIdle(pq));
			if (lock_amf_queue)
				ctx1->UnlockVulkan();
		} else if (o.sync == "devidle") {
			ctx1->LockVulkan();
			VKC(pd.vkDeviceWaitIdle(pd.dev));
			if (separate)
				VKC(ad.vkDeviceWaitIdle(ad.dev));
			ctx1->UnlockVulkan();
		} else if (o.sync != "none" && o.sync != "gpu") {
			die("bad --sync");
		}

		if (o.input == "alloc") {
			vs->eCurrentLayout = new_layout;
		} else {
			amf::AMFVulkanSurface &ns = vs ? *vs : s.surf;
			ns.eCurrentLayout = VK_IMAGE_LAYOUT_GENERAL;
			ns.Sync.hFence = VK_NULL_HANDLE;
			if (o.sync == "gpu") {
				ns.Sync.hSemaphore = binsems[st];
				ns.Sync.bSubmitted = true;
			} else {
				ns.Sync.hSemaphore = VK_NULL_HANDLE;
				ns.Sync.bSubmitted = false;
			}
			if (s.held) {
				surf = s.held;
			} else {
				AMFC(ctx1->CreateSurfaceFromVulkanNative(&s.surf, &surf, &obs));
				if (o.reuse_surface)
					s.held = surf;
			}
		}
		surf_ptrs.push_back(surf.GetPtr());
		surf->SetPts((amf_pts)n * 10000000 / 60);
		surf->SetProperty(L"ReproSlot", (amf_int64)sid);
		s.busy = o.input == "native" && !o.reuse_surface;

		for (;;) {
			AMF_RESULT r = enc->SubmitInput(surf);
			if (r == AMF_OK || r == AMF_NEED_MORE_INPUT)
				break;
			if (r != AMF_INPUT_FULL)
				die("SubmitInput(frame %d) -> %d", n, (int)r);
			pump();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		surf = nullptr;
		pump();
	}
	AMFC(enc->Drain());
	for (auto td = std::chrono::steady_clock::now();;) {
		amf::AMFDataPtr data;
		AMF_RESULT r = enc->QueryOutput(&data);
		if (data) {
			amf::AMFBufferPtr buf(data);
			packet p;
			p.data.assign((uint8_t *)buf->GetNative(), (uint8_t *)buf->GetNative() + buf->GetSize());
			p.pts = data->GetPts();
			packets.push_back(std::move(p));
			continue;
		}
		if (r == AMF_EOF)
			break;
		if (std::chrono::steady_clock::now() - td > std::chrono::seconds(10))
			die("drain timed out");
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	enc->Terminate();
	pump();

	if (!o.dump.empty()) {
		FILE *f = fopen(o.dump.c_str(), "wb");
		for (auto &p : packets)
			fwrite(p.data.data(), 1, p.data.size(), f);
		fclose(f);
	}

	// decode + check
	const AVCodec *dec = avcodec_find_decoder(hevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
	AVCodecContext *dc = avcodec_alloc_context3(dec);
	dc->thread_count = 1;
	if (avcodec_open2(dc, dec, nullptr) < 0)
		die("avcodec_open2");
	AVPacket *pk = av_packet_alloc();
	AVFrame *fr = av_frame_alloc();
	std::vector<int> got;
	std::vector<uint32_t> raws;
	auto drain_dec = [&]() {
		while (avcodec_receive_frame(dc, fr) == 0) {
			uint32_t raw = 0;
			got.push_back(read_frame(fr->data[0], fr->linesize[0], o.w, o.h, &raw));
			raws.push_back(raw);
			av_frame_unref(fr);
		}
	};
	for (auto &p : packets) {
		std::vector<uint8_t> buf(p.data.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
		memcpy(buf.data(), p.data.data(), p.data.size());
		pk->data = buf.data();
		pk->size = (int)p.data.size();
		pk->pts = p.pts;
		if (avcodec_send_packet(dc, pk) < 0)
			die("avcodec_send_packet");
		drain_dec();
	}
	avcodec_send_packet(dc, nullptr);
	drain_dec();

	int stale = 0, garbled = 0, blank = 0, max_back = 0;
	std::string detail;
	for (size_t i = 0; i < got.size(); i++) {
		const int v = got[i];
		if (v == (int)i)
			continue;
		char line[160];
		if (v == -1) {
			blank++;
			snprintf(line, sizeof line, "  frame %zu: blank (black) picture\n", i);
		} else if (v == -2) {
			garbled++;
			snprintf(line, sizeof line, "  frame %zu: garbled picture (code 0x%04x)\n", i, raws[i]);
		} else {
			stale++;
			max_back = std::max(max_back, (int)i - v);
			snprintf(line, sizeof line, "  frame %zu: shows frame %d (%+d)%s\n", i, v, v - (int)i,
				 i < surf_ptrs.size() && v < (int)surf_ptrs.size() && surf_ptrs[i] == surf_ptrs[v]
					 ? " [same AMFSurface object address]"
					 : "");
			if (o.spy && i < surf_ptrs.size() && v < (int)surf_ptrs.size()) {
				detail += line;
				snprintf(line, sizeof line, "    AMFSurface %p, frame %d's was %p\n", surf_ptrs[i], v,
					 surf_ptrs[v]);
			}
		}
		detail += line;
	}
	const bool count_ok = (int)got.size() == o.frames;
	const bool pass = count_ok && !stale && !garbled && !blank;
	if (!o.quiet) {
		printf("config: producer=%s pqueue=%s ring=%d%s sync=%s dedicated=%d export=%d tiling=%s format-list=%d "
		       "busy=%d input=%s dx11=%d amf-queue=%d amf-timeline=%d preencode=%d bframes=%d usage=%s codec=%s "
		       "stall-at=%d stall-ms=%d stall-every=%d pace=%d\n",
		       o.producer.c_str(), separate ? "-" : o.pqueue.c_str(), o.ring, o.keep ? "(keep)" : "",
		       o.sync.c_str(), o.dedicated, o.export_mem, o.tiling.c_str(), o.format_list, o.busy,
		       o.input.c_str(), o.dx11, o.amf_queue, o.amf_timeline, o.preencode, o.bframes, o.usage.c_str(),
		       o.codec.c_str(), o.stall_at, o.stall_ms, o.stall_every, o.fps_sleep_ms);
		printf("encoded %d frames in %.2f s, %zu packets, %zu decoded, AMF held up to %d input surfaces, %d images "
		       "destroyed\n",
		       o.frames, secs, packets.size(), got.size(), max_held, destroyed);
		printf("%s", detail.c_str());
		if (o.spy || o.no_close)
			printf("spy: AMF exported %d memory handles (%d with a value seen before), closed %d%s\n",
			       spy_exports, spy_value_reuse, spy_closes, o.no_close ? " (suppressed)" : "");
	}
	printf("%s: %d stale (up to %d frames back), %d garbled, %d blank, %zu/%d decoded\n", pass ? "PASS" : "FAIL",
	       stale, max_back, garbled, blank, got.size(), o.frames);
	fflush(stdout);
	// Skip teardown: AMF and the devices are released by process exit.
	TerminateProcess(GetCurrentProcess(), pass ? 0 : 1);
	return pass ? 0 : 1;
}
