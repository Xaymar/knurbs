#pragma once

#include <graphics/graphics.h>
#include <graphics/graphics-vulkan.h>
#include <graphics/matrix4.h>

#include <string.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include "vulkan-frame.hpp"
#include "vulkan-tracker.hpp"

// Fixed-function state the device_* setters change; defaults as D3D11. Only uint8_t members, so it has no padding
// and is compared and hashed bytewise.
struct vk_state {
	uint8_t blend = 1;
	uint8_t src_c = GS_BLEND_SRCALPHA, dst_c = GS_BLEND_INVSRCALPHA;
	uint8_t src_a = GS_BLEND_ONE, dst_a = GS_BLEND_INVSRCALPHA;
	uint8_t op = GS_BLEND_OP_ADD;
	uint8_t write_mask = 0xF; // VkColorComponentFlags
	// Depth/stencil; the draw resets it to these defaults in the key when there is no depth (or stencil) attachment.
	uint8_t depth = 1, depth_write = 1, depth_func = GS_LESS;
	uint8_t stencil = 0, stencil_write = 1;
	uint8_t front[4] = {GS_ALWAYS, GS_KEEP, GS_KEEP, GS_KEEP}; // test, fail, zfail, zpass
	uint8_t back[4] = {GS_ALWAYS, GS_KEEP, GS_KEEP, GS_KEEP};
	uint8_t cull = GS_BACK;
};

constexpr uint32_t vk_max_inputs = 16; // vertex shader input locations a pipeline key describes

// Everything a VkPipeline depends on. Scissor enable is not part of it: Vulkan always scissors, a disabled scissor
// is a full-target rectangle (dynamic state, as is the viewport). libobs has no fill mode setter (always solid).
struct vk_pipeline_key {
	gs_shader *vs = nullptr, *ps = nullptr;
	VkFormat color = VK_FORMAT_UNDEFINED, depth = VK_FORMAT_UNDEFINED;
	vk_state state;
	uint8_t topology = 0;                   // VkPrimitiveTopology
	uint8_t components[vk_max_inputs] = {}; // per vertex input location: components of the bound attribute

	bool operator==(const vk_pipeline_key &o) const
	{
		return vs == o.vs && ps == o.ps && color == o.color && depth == o.depth && topology == o.topology &&
		       !memcmp(&state, &o.state, sizeof(state)) &&
		       !memcmp(components, o.components, sizeof(components));
	}
};

template<> struct std::hash<vk_pipeline_key> {
	size_t operator()(const vk_pipeline_key &k) const noexcept
	{
		uint64_t h = 14695981039346656037ull; // FNV-1a
		auto mix = [&h](const void *p, size_t n) {
			for (size_t i = 0; i < n; ++i) {
				h = (h ^ ((const uint8_t *)p)[i]) * 1099511628211ull;
			}
		};
		mix(&k.vs, sizeof(k.vs));
		mix(&k.ps, sizeof(k.ps));
		mix(&k.color, sizeof(k.color));
		mix(&k.depth, sizeof(k.depth));
		mix(&k.state, sizeof(k.state));
		mix(&k.topology, 1);
		mix(k.components, sizeof(k.components));
		return (size_t)h;
	}
};

// Pipelines by key, each created once on first use; a failed creation is cached as VK_NULL_HANDLE (logged once, the
// draw is a no-op) so it is never retried. Layouts are per shader pair: set 0 = vertex shader, set 1 = pixel shader.
struct vk_pipeline_cache {
	gs_device *device = nullptr;
	VkPipelineCache cache = VK_NULL_HANDLE; // in memory only
	struct entry {
		VkPipeline pipeline;
		VkPipelineLayout layout;
	};
	std::unordered_map<vk_pipeline_key, entry> pipelines;
	struct layout_entry {
		gs_shader *vs, *ps;
		VkPipelineLayout layout;
	};
	std::vector<layout_entry> layouts; // searched only when a pipeline is created

	// O(1) lookup; creates the pipeline on the key's first use. `layout` (optional) receives its pipeline layout.
	VkPipeline get(const vk_pipeline_key &key, VkPipelineLayout *layout = nullptr);
	VkPipelineLayout layout(gs_shader *vs, gs_shader *ps);
	void forget(gs_shader *shader); // destroys (deferred) everything built from `shader`
};

// Every live object of a device, for the device-loss rebuild (vulkan-rebuild.cpp), which recreates their Vulkan
// objects in place so the gs_* pointers libobs and plugins hold stay valid. Swapchain targets are not listed (their
// swapchain is).
struct vk_objects {
	std::unordered_set<gs_texture *> textures;
	std::unordered_set<gs_zstencil_buffer *> zstencils;
	std::unordered_set<gs_stage_surface *> stagesurfs;
	std::unordered_set<gs_sampler_state *> samplers;
	std::unordered_set<gs_vertex_buffer *> vertex_buffers;
	std::unordered_set<gs_index_buffer *> index_buffers;
	std::unordered_set<gs_shader *> shaders;
	std::unordered_set<gs_swap_chain *> swapchains;
	std::unordered_set<gs_timer *> timers;
	std::unordered_set<gs_shared_buffer *> sharedbufs;
	std::unordered_set<gs_timeline *> timelines;
};

// Timestamp queries for gs_timer_t: two consecutive queries per timer in fixed-size pools, recycled once the GPU has
// finished the frame a destroyed timer was last used in.
struct vk_timer_pools {
	static constexpr uint32_t pool_size = 512; // queries per VkQueryPool
	std::vector<VkQueryPool> pools;
	std::vector<uint32_t> free; // first query of each free pair
	uint32_t valid_bits = 0;    // graphics family timestampValidBits; 0: no timestamps (timers report nothing)
	double period = 1.0;        // nanoseconds per tick
	bool host_reset = false;    // hostQueryReset enabled
};

// Threading: every graphics export is called with libobs's graphics context held (gs_enter_context), so all calls on
// a device and its objects are serialised and nothing here locks. Only module globals are touched outside that lock:
// the glslang reference count (its mutex), the validation message counter and the host priority (atomics).
struct gs_device {
	uint32_t adapter = 0; // as passed to device_create; the rebuild after a device loss opens the same one
	VkInstance instance = VK_NULL_HANDLE;
	VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
	VkPhysicalDevice phys = VK_NULL_HANDLE;
	VkDevice dev = VK_NULL_HANDLE;
	vk_queue graphics;
	vk_queue transfer; // q == VK_NULL_HANDLE when the device has no distinct transfer family
	VmaAllocator vma = VK_NULL_HANDLE;
	vk_frame_ring frames;
	vk_tracker tracker;
	gs_texture *render_target = nullptr;
	gs_zstencil_buffer *zstencil = nullptr; // depth attachment of the render target; none when null
	enum gs_color_space space = GS_CS_SRGB;
	bool can_present = false; // VK_KHR_surface, the platform surface and VK_KHR_swapchain are enabled
	gs_swap_chain *swap = nullptr;
	VkSemaphore acquire_wait = VK_NULL_HANDLE; // signalled by the last acquire; the next submit waits on it
	std::vector<VkSemaphore> spare_sems;       // unsignalled binary semaphores for acquires
	VkPhysicalDeviceLimits limits = {};
	bool anisotropy = false;      // samplerAnisotropy enabled
	bool mirror_clamp = false;    // samplerMirrorClampToEdge enabled (GS_ADDRESS_MIRRORONCE)
	bool colorspace_ext = false;  // VK_EXT_swapchain_colorspace enabled (HDR and 16F swapchains)
	gs_vulkan_device native = {}; // device_get_device_obj
	vk_timer_pools timers;
	// Test hooks, read once by vk_device_init (see also frames.test_fail_submit and frames.test_lose_after).
	bool test_no_loader = false;       // OBS_VULKAN_TEST_NO_LOADER=1: behave as without vulkan-1.dll
	bool test_refuse_realtime = false; // OBS_VULKAN_TEST_REFUSE_REALTIME=1: a realtime priority is refused
	// Test hook OBS_VULKAN_TEST_MONITOR_HDR=1 (device_is_monitor_hdr and the swapchain colour space): every monitor
	// reports HDR, so the HDR swapchain paths run without an HDR display. Tests may toggle it.
	bool test_monitor_hdr = false;

	// Device loss (vulkan-rebuild.cpp).
	vk_objects objects;
	std::vector<gs_device_loss> loss_callbacks;
	uint32_t rebuilds = 0; // completed rebuilds (read by tests)

	// Draw state, CPU side until device_draw.
	vk_state state;
	vk_pipeline_cache pipelines;
	gs_shader *cur_vs = nullptr, *cur_ps = nullptr;
	gs_vertex_buffer *cur_vb = nullptr;
	gs_index_buffer *cur_ib = nullptr;
	gs_texture *cur_textures[GS_MAX_TEXTURES] = {};
	gs_sampler_state *cur_samplers[GS_MAX_TEXTURES] = {};
	gs_rect viewport = {};
	gs_rect scissor = {};
	bool scissor_enabled = false;
	bool framebuffer_srgb = false;
	matrix4 proj;
	std::vector<matrix4> proj_stack;
	VkSampler default_sampler = VK_NULL_HANDLE; // for textures whose shader names no sampler_state
	// Colour view of the open dynamic rendering instance (VK_NULL_HANDLE: none). See vk_end_rendering.
	VkImageView rendering_view = VK_NULL_HANDLE;
	VkImageView rendering_depth = VK_NULL_HANDLE; // its depth view (VK_NULL_HANDLE: none)

	// External memory and synchronisation (vulkan-interop.cpp).
	VkPhysicalDeviceIDProperties id = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
	uint32_t vendor_id = 0, device_id = 0;
	bool ext_memory_win32 = false, ext_semaphore_win32 = false, ext_keyed_mutex = false;
	struct vk_interop *interop = nullptr; // extension entry points and the D3D11 probe device
	// Consumed by the next submission (vk_flush): timeline waits and signals, keyed mutex releases.
	vk_submit_extra pending;
	std::vector<VkDeviceMemory> km_release_mem;
	std::vector<uint64_t> km_release_key;

	gs_device() { matrix4_identity(&proj); }
};

struct vk_buffer {
	VkBuffer buffer = VK_NULL_HANDLE;
	VmaAllocation alloc = VK_NULL_HANDLE; // null for arena space
	VkDeviceSize offset = 0;
	uint32_t stride = 0;
};

struct gs_texture {
	gs_device *device;
	VkImage image = VK_NULL_HANDLE;
	VmaAllocation alloc = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;      // UNORM (or the format itself)
	VkImageView srgb_view = VK_NULL_HANDLE; // only for gs_is_srgb_format formats
	// GS_RENDER_TARGET only: mip 0, identity swizzle (BGRX's sampled views force A to one, an attachment must not).
	VkImageView rt_view = VK_NULL_HANDLE, rt_srgb_view = VK_NULL_HANDLE;
	VkFormat rt_format = VK_FORMAT_UNDEFINED, rt_srgb_format = VK_FORMAT_UNDEFINED;
	uint32_t width, height, levels;
	uint32_t depth = 1;                        // GS_TEXTURE_3D only
	enum gs_texture_type type = GS_TEXTURE_2D; // 2D or 3D (volume)
	gs_color_format format;
	bool dynamic = false; // GS_DYNAMIC: gs_texture_map hands out upload arena space (see gs_texture_map)
	// While mapped: arena space of frames[map_slot], counted in its `maps` so the slot never reuses it until unmapped.
	uint8_t *map_ptr = nullptr; // null when not mapped
	vk_buffer map_buf;
	VmaAllocation map_alloc = VK_NULL_HANDLE;
	uint32_t map_slot = 0;
	// Two-plane (NV12/P010) textures: device_texture_create_nv12/_p010 make one multi-planar image and two
	// gs_textures that are views of its planes (Y: GS_R8/GS_R16 at full size, UV: GS_R8G8/GS_RG16 at half size),
	// as D3D11's two views of one NV12 texture. Both share `image`/`alloc`; the last one destroyed frees them.
	// Copies and staging address the plane with `aspect`; barriers (the tracker) cover the whole image.
	VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT; // PLANE_0 (Y) or PLANE_1 (UV) for plane views
	VkFormat image_format = VK_FORMAT_UNDEFINED;           // plane views: the multi-planar format of `image`
	gs_texture *plane_peer = nullptr;                      // the other plane's view while it exists
	// External textures (vulkan-interop.cpp): imported or exportable memory in a dedicated VkDeviceMemory instead of
	// `alloc`, freed with the image. Plane views share it as they share `image`.
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkDeviceSize memory_size = 0;
	VkExternalMemoryHandleTypeFlags export_types = 0; // exportable as these
	enum gs_shared_layout layout = GS_SHARED_SINGLE;
	uint32_t create_flags = 0;            // GS_RENDER_TARGET as created (part of the export description)
	uint32_t shared_handle = 0xFFFFFFFFu; // legacy open_shared/open_nt_shared: the handle it was opened from
	bool keyed_mutex = false;             // imported D3D11 texture with a keyed mutex
	void *km_probe = nullptr; // keyed mutex textures: IDXGIKeyedMutex on the D3D11 probe (finite-timeout acquires)

	// How it was created, so the device-loss rebuild can create it again (vulkan-rebuild.cpp).
	enum origin_t : uint8_t {
		ORIGIN_NONE,       // swapchain target (rebuilt with its swapchain)
		ORIGIN_CREATED,    // device_texture_create / device_voltexture_create
		ORIGIN_PLANAR,     // device_texture_create_nv12 / _p010
		ORIGIN_EXPORTABLE, // device_texture_create_exportable
		ORIGIN_IMPORTED,   // gs_texture_import, open_shared, open_nt_shared
	} origin = ORIGIN_NONE;
	uint32_t req_levels = 0, req_flags = 0;    // ORIGIN_CREATED: the arguments as passed
	std::vector<std::vector<uint8_t>> initial; // ORIGIN_CREATED: initial data per given level (empty: none)
	bool shared_nt = false;                    // open_shared (false) or open_nt_shared (true), with shared_handle
	gs_vulkan_texture native = {};             // gs_texture_get_obj
};

// Exportable pitch-linear NV12/P010 frame (gs_sharedbuf_*).
struct gs_shared_buffer {
	gs_device *device;
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkDeviceSize size = 0;
	uint32_t width, height;
	enum gs_shared_layout layout;
	uint32_t offsets[2], pitches[2];
};

struct gs_timeline {
	gs_device *device;
	VkSemaphore sem = VK_NULL_HANDLE;
	bool exportable = false;
	bool imported = false;   // the device-loss rebuild replaces an import by a local timeline (vulkan-rebuild.cpp)
	uint64_t last_value = 0; // highest value seen on the CPU side, the initial value after a rebuild
};

struct gs_timer {
	gs_device *device;
	uint32_t query = UINT32_MAX; // first of its two queries (vk_timer_pools); UINT32_MAX: none (no timestamps)
	uint64_t value = 0; // graphics timeline value of the submission holding the end timestamp; 0: not ended
	bool begun = false;
};

struct gs_timer_range {
	gs_device *device;
};

// Create parameters of an external (exportable or imported) texture, the same as device_texture_create (single
// plane, one level) and device_texture_create_nv12/_p010 use. False when the format or size is unsupported.
struct vk_image_desc {
	VkImageCreateInfo ii;
	VkImageFormatListCreateInfo fl; // chained by the caller when use_list
	VkFormat views[2];
	bool use_list;
};
bool vk_texture_image_info(gs_device *d, uint32_t width, uint32_t height, enum gs_color_format format,
			   enum gs_shared_layout layout, uint32_t flags, vk_image_desc &out);
// gs_texture(s) with views of `image` (memory bound by the caller, which keeps the image on failure).
bool vk_wrap_image(gs_device *d, VkImage image, uint32_t width, uint32_t height, enum gs_color_format format,
		   enum gs_shared_layout layout, uint32_t flags, gs_texture **tex, gs_texture **tex_uv);

// Interop setup and teardown (vulkan-interop.cpp): device extensions to enable when present, then entry points.
void vk_interop_extensions(std::vector<const char *> &wanted);
void vk_interop_init(gs_device *d, const std::vector<const char *> &enabled);
void vk_interop_free(gs_device *d);
// Releases a texture's km_probe (a COM object), called when the texture is destroyed.
void vk_interop_release(void *com);
// vk_flush: records the release of acquired external resources and moves d->pending (and keyed mutex releases)
// into `extra` for the submission; `km` backs extra.pnext and must outlive the submit.
struct vk_keyed_mutex_info;
void vk_interop_prepare_submit(gs_device *d, VkCommandBuffer cmd, vk_submit_extra &extra, vk_keyed_mutex_info *&km);
void vk_interop_finish_submit(vk_keyed_mutex_info *km);

// True for a plane view of a two-plane image.
inline bool vk_is_plane(const gs_texture *t)
{
	return t->image_format != VK_FORMAT_UNDEFINED;
}

// Depth/stencil buffer: one image and one attachment view, never sampled (as D3D11's DSV-only texture).
struct gs_zstencil_buffer {
	gs_device *device;
	VkImage image = VK_NULL_HANDLE;
	VmaAllocation alloc = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	VkFormat format = VK_FORMAT_UNDEFINED;
	VkImageAspectFlags aspect = 0; // DEPTH, plus STENCIL for formats that have it
	uint32_t width, height;
	enum gs_zstencil_format gs_format = GS_ZS_NONE; // as created (rebuild)
};

struct gs_stage_surface {
	gs_device *device;
	VkBuffer buffer = VK_NULL_HANDLE;
	VmaAllocation alloc = VK_NULL_HANDLE;
	void *mapped = nullptr; // persistently mapped
	uint32_t width, height, linesize;
	gs_color_format format; // GS_UNKNOWN for NV12/P010 surfaces, as D3D11
	// NV12/P010 surfaces (device_stagesurface_create_nv12/_p010): the multi-planar format whose Y plane view is
	// staged into it; the UV plane follows the Y plane's `height` rows at the same linesize, as D3D11's mapping.
	VkFormat planar = VK_FORMAT_UNDEFINED;
	uint64_t copy_value = 0; // graphics timeline value of the submission holding the last copy; 0: never staged
};

struct gs_swap_chain {
	gs_device *device;
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	VkSwapchainKHR swapchain = VK_NULL_HANDLE; // null while the window is 0x0 or minimized
	VkSurfaceFormatKHR surface_format = {};
	uint32_t cx = 0, cy = 0; // requested size
	std::vector<VkImage> images;
	std::vector<VkImageView> views;         // per image; the target's rt_view while it is acquired
	std::vector<VkSemaphore> present_sems;  // per image: reusable once that image is acquired again
	gs_texture target;                      // `image` is the acquired image, null when none is
	uint32_t image = UINT32_MAX;            // acquired index
	bool recreate = false;                  // out of date, suboptimal or after a failed submit
	enum gs_color_space space = GS_CS_SRGB; // as D3D11's get_next_space for the window's monitor (choose_format)
	struct gs_window window = {};           // as created; the device-loss rebuild creates the surface again
	enum gs_color_format init_format = GS_UNKNOWN; // gs_init_data::format, the SDR format
	bool test_not_ready = false;                   // test hook: acquire behaves as VK_NOT_READY
	bool test_suboptimal = false;                  // test hook: a successful acquire reports VK_SUBOPTIMAL_KHR
	uint32_t builds = 0;                           // swapchains created (read by tests; handles may be reused)
};

struct gs_sampler_state {
	gs_device *device;
	VkSampler sampler = VK_NULL_HANDLE;
	gs_sampler_info info;
};

// Static data lives in host-visible buffers written once at creation. GS_DYNAMIC data is copied into the recording
// frame's upload arena on its first draw in each frame and after each flush, so a flush never waits for the GPU.
struct gs_vertex_buffer {
	gs_device *device;
	gs_vb_data *data; // owned
	bool dynamic;
	bool dirty = true;
	// points, normals, tangents, colors, then one per tvarray entry; buffer is null when the data has none.
	std::vector<vk_buffer> attrs;
	uint64_t upload_value = 0; // dynamic: graphics.next_value of the frame attrs point into
};

struct gs_index_buffer {
	gs_device *device;
	enum gs_index_type type;
	void *indices; // owned (bfree)
	size_t num;
	bool dynamic;
	bool dirty = true;
	vk_buffer buf;
	uint64_t upload_value = 0;
};

struct gs_shader_param {
	gs_shader *shader;
	std::string name;
	enum gs_shader_param_type type;
	int array_count;               // 0: not an array
	std::vector<uint8_t> def;      // default value as the effect parser stored it
	uint32_t offset = UINT32_MAX;  // std140 offset in the uniform block; UINT32_MAX for textures
	uint32_t stride = 0;           // array element stride in the block
	uint32_t binding = 0;          // textures: binding (combined image sampler) in the shader's set
	size_t sampler = SIZE_MAX;     // textures: index into gs_shader::samplers it is sampled with, if any
	gs_texture *texture = nullptr; // textures: bound by gs_shader_set_texture / set_val
	bool srgb = false;
	gs_samplerstate_t *next_sampler = nullptr;
	bool warned = false;          // unsupported type, or (textures) unset at a draw, already logged once
	bool feedback_warned = false; // textures: sampled while bound as the render target, already logged once
};

struct gs_shader {
	gs_device *device;
	enum gs_shader_type type;
	std::vector<uint32_t> spirv;
	std::vector<gs_shader_param> params; // never resized after creation; gs_sparam_t points into it
	uint32_t set = 0;                    // descriptor set: 0 vertex, 1 pixel
	std::vector<uint8_t> uniforms;       // CPU shadow of the std140 block at binding 0 of `set`; empty when none
	bool dirty = true;                   // uniforms changed since the last upload (device_draw clears it)
	int max_varying = -1;                // highest varying location the shader declares; -1: none
	VkDescriptorSetLayout set_layout = VK_NULL_HANDLE; // created with the first pipeline using the shader
	std::vector<VkSampler> vk_samplers;                // per `samplers` entry, created by the first draw
	// Last upload of `uniforms` into an upload arena, reused while not dirty in the same frame.
	VkBuffer ubo = VK_NULL_HANDLE;
	VkDeviceSize ubo_offset = 0;
	uint64_t ubo_value = 0;                // graphics.next_value of the frame it was written into
	std::vector<std::string> inputs;       // vertex shaders: semantic per input location
	std::vector<gs_sampler_info> samplers; // the shader's sampler_state blocks
	gs_shader_param *viewproj = nullptr, *world = nullptr;
};

// HLSL (as emitted by the effect parser) -> Vulkan GLSL -> SPIR-V. Empty on failure, with `error` set.
// When `shader` is given, its params, uniform block size, inputs and samplers are filled from the same parse.
// C++ exceptions become errors. Inputs known to crash the shared parser (recursive or very deep #defines, bracket
// nesting over 256) are rejected before it runs; any other crash inside it or glslang is fatal (logged on Windows).
std::vector<uint32_t> hlsl_to_spirv(const char *hlsl, enum gs_shader_type type, std::string &error,
				    gs_shader *shader = nullptr, const char *file = nullptr);
// glslang process lifetime: device_create takes a reference, device_destroy drops it; compiles hold their own.
void vk_glslang_ref();
void vk_glslang_unref();

// VK_FORMAT_UNDEFINED for formats the backend does not support.
VkFormat vk_format(enum gs_color_format format);

// Command buffer of the frame being recorded, outside any rendering (ends an open one); begins a frame if none is.
// Every recording outside a draw goes through this, which is what ends dynamic rendering (see vk_end_rendering).
VkCommandBuffer vk_cmd(gs_device *d);
// The same without ending rendering; only device_draw uses it.
VkCommandBuffer vk_frame_cmd(gs_device *d);
// Ends the open dynamic rendering instance, if any.
void vk_end_rendering(gs_device *d);
// Begins a dynamic rendering instance into `view` of `rt` (and `zs`'s depth view, if given) unless that one is open
// and `b` is empty. Ends any other instance first, registers the attachments with the tracker and records `b` and
// the tracker's barriers. Returns the frame's command buffer.
VkCommandBuffer vk_begin_rendering(gs_device *d, gs_texture *rt, VkImageView view, gs_zstencil_buffer *zs,
				   vk_barriers &b);
// Copies `data` into the recording frame's upload arena (begins a frame if none is); buf.buffer/offset point at it.
// A full arena grows by a new chunk; never waits. False when out of memory.
bool vk_arena_upload(gs_device *d, const void *data, VkDeviceSize size, VkDeviceSize align, vk_buffer &buf);
// The same without the copy: reserves `size` bytes and returns their mapped address (nullptr when out of memory); the
// caller writes them and flushes `alloc`. Chunks are never moved or freed before device teardown, so the address
// stays valid; the space is reused once the slot is begun again (unless vk_frame::maps holds the arena).
uint8_t *vk_arena_alloc(gs_device *d, VkDeviceSize size, VkDeviceSize align, vk_buffer &buf, VmaAllocation &alloc);
// GS_DYNAMIC buffers: copies the CPU data into the recording frame's arena unless that is already done and current.
bool vk_upload_dynamic(gs_device *d, gs_vertex_buffer *vb);
bool vk_upload_dynamic(gs_device *d, gs_index_buffer *ib);
// Host-visible, persistently mapped buffer.
bool vk_create_host_buffer(gs_device *d, VkDeviceSize size, VkBufferUsageFlags usage, VmaAllocationCreateFlags access,
			   VkBuffer &buffer, VmaAllocation &alloc, void *&mapped);
// Device teardown (GPU idle): destroys pipelines, layouts and the default sampler.
void vk_pipelines_free(gs_device *d);
VkSampler vk_create_sampler(gs_device *d, const gs_sampler_info &info);
// Submits the frame being recorded, if any, waiting on a pending acquire and signalling `signal` (binary).
// False when the submission failed; `signal` is then not signalled.
bool vk_flush(gs_device *d, VkSemaphore *signal = nullptr);
// Error path: waits on `sem`, an acquire's semaphore whose submission failed, so it can go back to spare_sems.
void vk_consume_semaphore(gs_device *d, VkSemaphore sem);
// Records b as one vkCmdPipelineBarrier2; nothing when empty.
void vk_record_barriers(VkCommandBuffer cmd, const vk_barriers &b);

// Creates the instance, picks adapter `adapter`, creates device, queues and allocator.
// Returns GS_SUCCESS, GS_ERROR_FAIL (bad adapter) or GS_ERROR_NOT_SUPPORTED; leaks nothing on failure.
int vk_device_init(gs_device *d, uint32_t adapter);
void vk_device_free(gs_device *d);
// The instance and device part of vk_device_init (no test hooks read), for d->adapter. The rebuild uses it.
int vk_device_open(gs_device *d);
// Destroys everything vk_device_open created and the device-level objects made since (frames, pipelines, spare
// semaphores, timer pools, interop), and nulls their handles; objects in d->objects must be released first.
void vk_device_close(gs_device *d);

/* ---- Device loss (vulkan-rebuild.cpp) -------------------------------------------------------------------------- */

// Records a VK_ERROR_DEVICE_LOST result; returns r.
VkResult vk_check(gs_device *d, VkResult r);
inline bool vk_lost(const gs_device *d)
{
	return d->frames.lost;
}
// At a safe point (device_begin_frame, device_is_present_ready, device_present): rebuilds the device when it was
// lost. As D3D11's RebuildDevice: device_loss_release callbacks, every object's Vulkan objects destroyed, a new
// instance and device, every object created again in place, then device_loss_rebuild callbacks.
void vk_handle_loss(gs_device *d);

// Per-type parts of the rebuild. release: destroy the object's Vulkan objects now (the GPU is idle) and null them.
// recreate: create them again on the new device from what the object kept; false (logged) when that failed, the
// object then has none and its uses fail as for an unset object.
void vk_texture_release(gs_device *d, gs_texture *t);
bool vk_texture_recreate(gs_device *d, gs_texture *t); // ORIGIN_CREATED and ORIGIN_PLANAR
// Gives `t` (and its plane peer, when t is a Y view) the Vulkan objects of the texture(s) `make` creates with t's
// parameters: one texture, or both plane views. A UV view whose Y view exists is left to that one (true).
bool vk_texture_adopt(gs_device *d, gs_texture *t, const std::function<bool(gs_texture **, gs_texture **)> &make);
// A texture of t's size, format (or planar layout) and render-target flag with undefined contents, in place of an
// external texture whose memory cannot be had again (vulkan-interop.cpp).
bool vk_texture_recreate_placeholder(gs_device *d, gs_texture *t);
bool vk_interop_recreate_texture(gs_device *d, gs_texture *t); // ORIGIN_EXPORTABLE and ORIGIN_IMPORTED
void vk_zstencil_release(gs_device *d, gs_zstencil_buffer *zs);
bool vk_zstencil_recreate(gs_device *d, gs_zstencil_buffer *zs);
void vk_stagesurf_release(gs_device *d, gs_stage_surface *s);
bool vk_stagesurf_recreate(gs_device *d, gs_stage_surface *s);
void vk_buffers_release(gs_device *d);  // vertex and index buffers and sampler states
bool vk_buffers_recreate(gs_device *d); // false when any failed
void vk_swapchain_release(gs_swap_chain *s);
bool vk_swapchain_recreate(gs_swap_chain *s);
void vk_sharedbuf_release(gs_device *d, gs_shared_buffer *b);
bool vk_sharedbuf_recreate(gs_device *d, gs_shared_buffer *b);
void vk_timeline_release(gs_device *d, gs_timeline *t);
bool vk_timeline_recreate(gs_device *d, gs_timeline *t);

/* ---- Timers (vulkan-timer.cpp) --------------------------------------------------------------------------------- */

// Reads timestampValidBits/timestampPeriod; called by vk_device_open.
void vk_timers_init(gs_device *d, uint32_t valid_bits, bool host_reset);
// Destroys the query pools (GPU idle); live timers keep no query (vk_timer_recreate gives them new ones).
void vk_timers_free(gs_device *d);
void vk_timer_recreate(gs_device *d, gs_timer *t);

/* ---- HDR (vulkan-swapchain.cpp) -------------------------------------------------------------------------------- */

// The colour space and swapchain surface format libobs-d3d11 would pick for a window on a monitor that is HDR
// (`hdr`) or has `bits` bits per colour: GS_CS_709_SCRGB on HDR monitors, GS_CS_SRGB_16F on SDR monitors of more
// than 8 bits (both R16G16B16A16_SFLOAT in EXTENDED_SRGB_LINEAR, as D3D11's G10_NONE_P709), else GS_CS_SRGB in the
// init format. A space whose format `formats` lacks falls back to GS_CS_SRGB. False when not even that exists.
bool vk_choose_surface_format(const std::vector<VkSurfaceFormatKHR> &formats, enum gs_color_format init_format,
			      bool hdr, uint32_t bits, enum gs_color_space &space, VkSurfaceFormatKHR &out);

// Name and driver version of the adapter chosen by the last successful vk_device_init.
const char *vk_adapter_name();
const char *vk_driver_version();

extern "C" {
// Test hook: validation messages since the last vk_device_init, or -1 when no validation layer is active.
EXPORT int obs_vulkan_validation_messages(void);
// Test hook: device_stage_texture for mip `level`; dst must have that level's size.
EXPORT void obs_vulkan_test_stage_mip(gs_device_t *device, gs_stagesurf_t *dst, gs_texture_t *src, uint32_t level);
// Test hook: glslang process references currently held (devices plus running compiles).
EXPORT int obs_vulkan_glslang_refs(void);
// Test hook: reads back the first `size` bytes of a gs_sharedbuf_t (waits for the GPU).
EXPORT bool obs_vulkan_test_read_sharedbuf(gs_sharedbuf_t *buf, void *dst, uint64_t size);
// Test hook: what device_is_monitor_hdr reads for `monitor` (HMONITOR): HDR (ST.2084 output) and bits per colour.
EXPORT bool obs_vulkan_test_monitor_info(gs_device_t *device, void *monitor, bool *hdr, uint32_t *bits);
// Test hook: vk_choose_surface_format.
EXPORT bool obs_vulkan_test_choose_surface_format(const VkSurfaceFormatKHR *formats, uint32_t count,
						  enum gs_color_format init_format, bool hdr, uint32_t bits,
						  enum gs_color_space *space, VkSurfaceFormatKHR *out);
// Host API (optional, not a graphics import): global priority the graphics queue of devices created from now on
// asks for, as a VkQueueGlobalPriorityKHR value (128 low, the default; 256 medium; 512 high; 1024 realtime).
// OBS_VULKAN_REALTIME=1 overrides it with realtime. Returns 0 for any other value (unchanged), 1 otherwise.
EXPORT int obs_vulkan_set_global_priority(int level);
}
