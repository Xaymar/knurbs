// External memory and synchronisation in libobs-vulkan (docs/graphics-interop.md), Linux: dmabufs with DRM format
// modifiers (exported by Vulkan, allocated with GBM, imported, read on the CPU through mmap), opaque fds between two
// VkDevices ordered by a shared timeline, the pitch-linear NV12/P010 frame buffer as an opaque fd, sync_files both
// ways, DRM syncobj timeline points (gs_sync_*, PipeWire's explicit sync), the dmabuf exports of graphics.h that
// linux-pipewire uses, and a child process that receives the fds over a socket (SCM_RIGHTS), writes the texture and
// signals back.
//
// Every test runs under the validation layer with synchronization validation and expects no messages; each skips
// without a Vulkan 1.3 device.
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>

#include <drm_fourcc.h>
#include <gbm.h>
#include <linux/dma-buf.h>
#include <xf86drm.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <util/bmem.h>
#include <util/platform.h>

// cmocka.h redefines `inline`, so it must come after the C++ standard headers.
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
extern "C" {
#include <cmocka.h>
}

static void *module;
static int (*create)(gs_device_t **, uint32_t);
static void (*destroy)(gs_device_t *);
static int (*messages)(void);
static gs_texture_t *(*texture_create)(gs_device_t *, uint32_t, uint32_t, enum gs_color_format, uint32_t,
				       const uint8_t **, uint32_t);
static void (*texture_destroy)(gs_texture_t *);
static uint32_t (*texture_width)(const gs_texture_t *);
static uint32_t (*texture_height)(const gs_texture_t *);
static enum gs_color_format (*texture_format)(const gs_texture_t *);
static bool (*create_nv12)(gs_device_t *, gs_texture_t **, gs_texture_t **, uint32_t, uint32_t, uint32_t);
static bool (*create_p010)(gs_device_t *, gs_texture_t **, gs_texture_t **, uint32_t, uint32_t, uint32_t);
static void (*set_render_target)(gs_device_t *, gs_texture_t *, gs_zstencil_t *);
static void (*clear)(gs_device_t *, uint32_t, const struct vec4 *, float, uint8_t);
static void (*copy_texture)(gs_device_t *, gs_texture_t *, gs_texture_t *);
static gs_stagesurf_t *(*stagesurface_create)(gs_device_t *, uint32_t, uint32_t, enum gs_color_format);
static gs_stagesurf_t *(*stagesurface_create_nv12)(gs_device_t *, uint32_t, uint32_t);
static void (*stage_texture)(gs_device_t *, gs_stagesurf_t *, gs_texture_t *);
static bool (*stagesurface_map)(gs_stagesurf_t *, uint8_t **, uint32_t *);
static void (*stagesurface_unmap)(gs_stagesurf_t *);
static void (*stagesurface_destroy)(gs_stagesurf_t *);
static void (*flush)(gs_device_t *);

static bool (*get_identity)(gs_device_t *, struct gs_adapter_identity *);
static bool (*supported)(gs_device_t *, enum gs_handle_type, enum gs_shared_layout, enum gs_color_format, bool);
static bool (*create_exportable)(gs_device_t *, uint32_t, uint32_t, enum gs_color_format, enum gs_shared_layout,
				 uint32_t, gs_texture_t **, gs_texture_t **);
static bool (*texture_export)(gs_device_t *, gs_texture_t *, enum gs_handle_type, struct gs_shared_texture_desc *);
static bool (*texture_import)(gs_device_t *, const struct gs_shared_texture_desc *, gs_texture_t **, gs_texture_t **);
static gs_sharedbuf_t *(*sharedbuf_create)(gs_device_t *, uint32_t, uint32_t, enum gs_shared_layout);
static void (*sharedbuf_destroy)(gs_sharedbuf_t *);
static bool (*sharedbuf_copy)(gs_device_t *, gs_sharedbuf_t *, gs_texture_t *);
static bool (*sharedbuf_export)(gs_device_t *, gs_sharedbuf_t *, enum gs_handle_type, struct gs_shared_texture_desc *);
static bool (*read_sharedbuf)(gs_sharedbuf_t *, void *, uint64_t);
static gs_timeline_t *(*timeline_create)(gs_device_t *, uint64_t);
static gs_timeline_t *(*timeline_import)(gs_device_t *, const struct gs_shared_handle *);
static bool (*timeline_export)(gs_device_t *, gs_timeline_t *, enum gs_handle_type, struct gs_shared_handle *);
static void (*timeline_destroy)(gs_timeline_t *);
static bool (*timeline_signal)(gs_device_t *, gs_timeline_t *, uint64_t);
static bool (*timeline_wait)(gs_device_t *, gs_timeline_t *, uint64_t);
static uint64_t (*timeline_value)(gs_device_t *, gs_timeline_t *);
static int (*timeline_wait_cpu)(gs_device_t *, gs_timeline_t *, uint64_t, uint64_t);
static bool (*timeline_signal_cpu)(gs_device_t *, gs_timeline_t *, uint64_t);
static bool (*sync_file_export)(gs_device_t *, struct gs_shared_handle *);
static bool (*sync_file_wait)(gs_device_t *, const struct gs_shared_handle *);

// The dmabuf and explicit sync exports of graphics.h (linux-pipewire).
static gs_texture_t *(*create_from_dmabuf)(gs_device_t *, unsigned int, unsigned int, uint32_t, enum gs_color_format,
					   uint32_t, const int *, const uint32_t *, const uint32_t *, const uint64_t *);
static bool (*query_dmabuf_caps)(gs_device_t *, enum gs_dmabuf_flags *, uint32_t **, size_t *);
static bool (*query_dmabuf_mods)(gs_device_t *, uint32_t, uint64_t **, size_t *);
static bool (*query_sync)(gs_device_t *);
static gs_sync_t *(*sync_create)(gs_device_t *);
static gs_sync_t *(*sync_from_point)(gs_device_t *, int, uint64_t);
static void (*sync_destroy)(gs_device_t *, gs_sync_t *);
static bool (*sync_export_point)(gs_device_t *, gs_sync_t *, int, uint64_t);
static bool (*sync_signal_point)(gs_device_t *, int, uint64_t);
static bool (*sync_wait)(gs_device_t *, gs_sync_t *);

template<typename T> static bool load(T &fn, const char *name)
{
	fn = (T)os_dlsym(module, name);
	if (!fn) {
		print_message("missing export: %s\n", name);
	}
	return fn != nullptr;
}

static bool load_all()
{
	return load(create, "device_create") & load(destroy, "device_destroy") &
	       load(messages, "obs_vulkan_validation_messages") & load(texture_create, "device_texture_create") &
	       load(texture_destroy, "gs_texture_destroy") & load(texture_width, "gs_texture_get_width") &
	       load(texture_height, "gs_texture_get_height") & load(texture_format, "gs_texture_get_color_format") &
	       load(create_nv12, "device_texture_create_nv12") & load(create_p010, "device_texture_create_p010") &
	       load(set_render_target, "device_set_render_target") & load(clear, "device_clear") &
	       load(copy_texture, "device_copy_texture") & load(stagesurface_create, "device_stagesurface_create") &
	       load(stagesurface_create_nv12, "device_stagesurface_create_nv12") &
	       load(stage_texture, "device_stage_texture") & load(stagesurface_map, "gs_stagesurface_map") &
	       load(stagesurface_unmap, "gs_stagesurface_unmap") &
	       load(stagesurface_destroy, "gs_stagesurface_destroy") & load(flush, "device_flush") &
	       load(get_identity, "device_get_adapter_identity") & load(supported, "device_shared_texture_supported") &
	       load(create_exportable, "device_texture_create_exportable") &
	       load(texture_export, "device_texture_export") & load(texture_import, "device_texture_import") &
	       load(sharedbuf_create, "device_sharedbuf_create") & load(sharedbuf_destroy, "gs_sharedbuf_destroy") &
	       load(sharedbuf_copy, "device_sharedbuf_copy_texture") &
	       load(sharedbuf_export, "device_sharedbuf_export") &
	       load(read_sharedbuf, "obs_vulkan_test_read_sharedbuf") &
	       load(timeline_create, "device_timeline_create") & load(timeline_import, "device_timeline_import") &
	       load(timeline_export, "device_timeline_export") & load(timeline_destroy, "gs_timeline_destroy") &
	       load(timeline_signal, "device_timeline_signal") & load(timeline_wait, "device_timeline_wait") &
	       load(timeline_value, "device_timeline_get_value") & load(timeline_wait_cpu, "device_timeline_wait_cpu") &
	       load(timeline_signal_cpu, "device_timeline_signal_cpu") &
	       load(sync_file_export, "device_sync_file_export") & load(sync_file_wait, "device_sync_file_wait") &
	       load(create_from_dmabuf, "device_texture_create_from_dmabuf") &
	       load(query_dmabuf_caps, "device_query_dmabuf_capabilities") &
	       load(query_dmabuf_mods, "device_query_dmabuf_modifiers_for_format") &
	       load(query_sync, "device_query_sync_capabilities") & load(sync_create, "device_sync_create") &
	       load(sync_from_point, "device_sync_create_from_syncobj_timeline_point") &
	       load(sync_destroy, "device_sync_destroy") &
	       load(sync_export_point, "device_sync_export_syncobj_timeline_point") &
	       load(sync_signal_point, "device_sync_signal_syncobj_timeline_point") &
	       load(sync_wait, "device_sync_wait");
}

static int setup(void **)
{
	setenv("OBS_VULKAN_VALIDATION", "1", 1);
	module = os_dlopen(OBS_VULKAN_MODULE);
	return module && load_all() ? 0 : -1;
}

static int teardown(void **)
{
	os_dlclose(module);
	return 0;
}

// Runs fn; ends the process when it takes longer than 30 s (a lost hand-off would hang ctest). See
// test_vulkan_interop.cpp for why the watchdog is detached.
static std::atomic<int> watch_generation{0};
template<typename F> static void within_30s(F fn)
{
	const int gen = ++watch_generation;
	std::thread([gen] {
		for (int i = 0; i < 600; ++i) {
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			if (watch_generation != gen) {
				return;
			}
		}
		fprintf(stderr, "watchdog: timed out after 30 s\n");
		fflush(stderr);
		_Exit(2);
	}).detach();
	fn();
	++watch_generation;
}

static int banked = 0;
static void bank_messages()
{
	if (messages() > 0) {
		banked += messages();
	}
}

static gs_device_t *device_or_skip()
{
	banked = 0;
	gs_device_t *d = nullptr;
	int r = create(&d, 0);
	if (r == GS_ERROR_NOT_SUPPORTED) {
		skip();
	}
	assert_int_equal(r, GS_SUCCESS);
	return d;
}

static void expect_no_messages()
{
	if (messages() < 0) {
		const char *rq = getenv("OBS_VULKAN_REQUIRE_VALIDATION");
		if (rq && !strcmp(rq, "1")) {
			fail_msg("validation layer required (OBS_VULKAN_REQUIRE_VALIDATION=1) but missing");
		}
		print_message("validation layer missing, not checked\n");
		return;
	}
	assert_int_equal(messages() + banked, 0);
}

static void close_fds(struct gs_shared_texture_desc &desc)
{
	for (int &fd : desc.fds) {
		if (fd >= 0) {
			close(fd);
			fd = -1;
		}
	}
}

static void close_handle(struct gs_shared_handle &h)
{
	if (h.fd >= 0) {
		close(h.fd);
		h.fd = -1;
	}
}

// Whether the fd is a sync_file that has signalled (poll() reports it readable).
static bool sync_file_signalled(int fd, int timeout_ms)
{
	struct pollfd p = {fd, POLLIN, 0};
	return poll(&p, 1, timeout_ms) == 1 && (p.revents & POLLIN);
}

/* ---- GBM and the render node (the test's own, as another process would) ------------------------------------ */

struct render_node {
	int fd = -1;
	struct gbm_device *gbm = nullptr;

	bool open_first()
	{
		DIR *dir = opendir("/dev/dri");
		if (!dir) {
			return false;
		}
		std::vector<std::string> nodes;
		while (struct dirent *e = readdir(dir)) {
			if (!strncmp(e->d_name, "renderD", 7)) {
				nodes.push_back(std::string("/dev/dri/") + e->d_name);
			}
		}
		closedir(dir);
		for (auto &n : nodes) {
			fd = open(n.c_str(), O_RDWR | O_CLOEXEC);
			if (fd >= 0) {
				gbm = gbm_create_device(fd);
				return gbm != nullptr;
			}
		}
		return false;
	}

	~render_node()
	{
		if (gbm) {
			gbm_device_destroy(gbm);
		}
		if (fd >= 0) {
			close(fd);
		}
	}
};

// A GBM buffer object described as a dmabuf (one fd per plane, each its own dup of the buffer).
static struct gs_shared_texture_desc describe_bo(struct gbm_bo *bo, uint32_t w, uint32_t h, enum gs_color_format format,
						 enum gs_shared_layout layout)
{
	struct gs_shared_texture_desc desc = {};
	for (int &fd : desc.fds) {
		fd = -1;
	}
	desc.type = GS_HANDLE_DMABUF;
	desc.width = w;
	desc.height = h;
	desc.format = format;
	desc.layout = layout;
	desc.num_planes = (uint32_t)gbm_bo_get_plane_count(bo);
	const uint64_t mod = gbm_bo_get_modifier(bo);
	desc.modifier = mod == DRM_FORMAT_MOD_INVALID ? DRM_FORMAT_MOD_LINEAR : mod;
	for (uint32_t p = 0; p < desc.num_planes; ++p) {
		desc.fds[p] = gbm_bo_get_fd(bo);
		desc.offsets[p] = gbm_bo_get_offset(bo, (int)p);
		desc.pitches[p] = gbm_bo_get_stride_for_plane(bo, (int)p);
	}
	return desc;
}

// CPU access to a LINEAR dmabuf, bracketed by DMA_BUF_IOCTL_SYNC.
struct dmabuf_map {
	int fd;
	size_t size;
	uint8_t *data = nullptr;
	uint64_t flags;

	dmabuf_map(int f, uint64_t rw) : fd(f), flags(rw)
	{
		size = (size_t)lseek(fd, 0, SEEK_END);
		void *p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		assert_true(p != MAP_FAILED);
		data = (uint8_t *)p;
		struct dma_buf_sync s = {DMA_BUF_SYNC_START | flags};
		assert_int_equal(ioctl(fd, DMA_BUF_IOCTL_SYNC, &s), 0);
	}
	~dmabuf_map()
	{
		struct dma_buf_sync s = {DMA_BUF_SYNC_END | flags};
		ioctl(fd, DMA_BUF_IOCTL_SYNC, &s);
		munmap(data, size);
	}
};

/* ---- Patterns ----------------------------------------------------------------------------------------------- */

static uint32_t bgra_px(uint32_t x, uint32_t y, int gen)
{
	const uint8_t b = (uint8_t)(x * 2 + gen * 7), g = (uint8_t)(y * 3 + gen * 13),
		      r = (uint8_t)((x ^ y) + gen * 31);
	return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static std::vector<uint32_t> vk_read_bgra(gs_device_t *d, gs_texture_t *t)
{
	const uint32_t w = texture_width(t), h = texture_height(t);
	gs_stagesurf_t *s = stagesurface_create(d, w, h, GS_BGRA);
	assert_non_null(s);
	stage_texture(d, s, t);
	uint8_t *data;
	uint32_t linesize;
	assert_true(stagesurface_map(s, &data, &linesize));
	std::vector<uint32_t> out(w * h);
	for (uint32_t y = 0; y < h; ++y) {
		memcpy(&out[y * w], data + (size_t)y * linesize, w * 4);
	}
	stagesurface_unmap(s);
	stagesurface_destroy(s);
	return out;
}

static void vk_clear(gs_device_t *d, gs_texture_t *t, float r, float g, float b)
{
	struct vec4 c;
	vec4_set(&c, r, g, b, 1.0f);
	set_render_target(d, t, nullptr);
	clear(d, GS_CLEAR_COLOR, &c, 1.0f, 0);
	set_render_target(d, nullptr, nullptr);
}

static bool all_equal(const std::vector<uint32_t> &v, uint32_t px)
{
	for (uint32_t p : v) {
		if (p != px) {
			print_message("got 0x%08x, expected 0x%08x\n", p, px);
			return false;
		}
	}
	return true;
}

static const uint32_t red = 0xFFFF0000u, green = 0xFF00FF00u, blue = 0xFF0000FFu; // as BGRA bytes in a uint32_t

static uint8_t nv12_y(uint32_t x, uint32_t y)
{
	return (uint8_t)(16 + ((x + y * 2) % 220));
}

static uint8_t nv12_uv(uint32_t x, uint32_t y, int c)
{
	return (uint8_t)(c ? 255 - x * 3 - y : 30 + x * 2 + y * 3);
}

// The NV12 pattern, Y rows then UV rows, `w` bytes per row.
static std::vector<uint8_t> nv12_expected(uint32_t w, uint32_t h)
{
	std::vector<uint8_t> e(w * h * 3 / 2);
	for (uint32_t j = 0; j < h; ++j) {
		for (uint32_t i = 0; i < w; ++i) {
			e[j * w + i] = nv12_y(i, j);
		}
	}
	for (uint32_t j = 0; j < h / 2; ++j) {
		for (uint32_t i = 0; i < w / 2; ++i) {
			e[w * h + j * w + i * 2] = nv12_uv(i, j, 0);
			e[w * h + j * w + i * 2 + 1] = nv12_uv(i, j, 1);
		}
	}
	return e;
}

// Copies the NV12 pattern into both planes of an NV12 texture.
static void fill_nv12(gs_device_t *d, gs_texture_t *y, gs_texture_t *uv, uint32_t w, uint32_t h)
{
	const std::vector<uint8_t> e = nv12_expected(w, h);
	const uint8_t *dy = e.data(), *duv = e.data() + (size_t)w * h;
	gs_texture_t *ty = texture_create(d, w, h, GS_R8, 1, &dy, 0);
	gs_texture_t *tuv = texture_create(d, w / 2, h / 2, GS_R8G8, 1, &duv, 0);
	assert_non_null(ty);
	assert_non_null(tuv);
	copy_texture(d, y, ty);
	copy_texture(d, uv, tuv);
	texture_destroy(ty);
	texture_destroy(tuv);
}

// Rows of a staged NV12 texture that differ from `expect`.
static size_t nv12_bad_rows(gs_device_t *d, gs_texture_t *y, uint32_t w, uint32_t h, const std::vector<uint8_t> &expect)
{
	gs_stagesurf_t *s = stagesurface_create_nv12(d, w, h);
	if (!s) {
		return SIZE_MAX;
	}
	stage_texture(d, s, y);
	uint8_t *data;
	uint32_t linesize;
	size_t bad = SIZE_MAX;
	if (stagesurface_map(s, &data, &linesize)) {
		bad = 0;
		for (uint32_t r = 0; r < h * 3 / 2; ++r) {
			bad += memcmp(data + (size_t)r * linesize, &expect[(size_t)r * w], w) != 0;
		}
		stagesurface_unmap(s);
	}
	stagesurface_destroy(s);
	return bad;
}

/* ---- Tests -------------------------------------------------------------------------------------------------- */

static void test_identity_and_support(void **)
{
	gs_device_t *d = device_or_skip();
	struct gs_adapter_identity id;
	assert_true(get_identity(d, &id));
	static const uint8_t zero[16] = {};
	assert_true(memcmp(id.device_uuid, zero, 16) != 0);
	assert_false(id.luid_valid); // a Windows thing
	// Single-plane textures export as dmabufs, NV12/P010 as opaque fds (render targets) or GBM dmabufs.
	assert_true(supported(d, GS_HANDLE_DMABUF, GS_SHARED_SINGLE, GS_BGRA, false));
	assert_false(supported(d, GS_HANDLE_FD, GS_SHARED_SINGLE, GS_BGRA, false));
	assert_true(supported(d, GS_HANDLE_FD, GS_SHARED_NV12, GS_UNKNOWN, false));
	assert_true(supported(d, GS_HANDLE_FD, GS_SHARED_P010, GS_UNKNOWN, false));
	// Imports of every type.
	assert_true(supported(d, GS_HANDLE_FD, GS_SHARED_SINGLE, GS_BGRA, true));
	assert_true(supported(d, GS_HANDLE_FD, GS_SHARED_NV12, GS_UNKNOWN, true));
	assert_true(supported(d, GS_HANDLE_DMABUF, GS_SHARED_SINGLE, GS_BGRA, true));
	assert_true(supported(d, GS_HANDLE_DMABUF, GS_SHARED_SINGLE, GS_RGBA, true));
	assert_true(supported(d, GS_HANDLE_DMABUF, GS_SHARED_NV12, GS_UNKNOWN, true));
	// The Windows types are not offered here, and sync files are not textures.
	assert_false(supported(d, GS_HANDLE_WIN32, GS_SHARED_SINGLE, GS_BGRA, false));
	assert_false(supported(d, GS_HANDLE_WIN32_D3D11, GS_SHARED_SINGLE, GS_BGRA, true));
	assert_false(supported(d, GS_HANDLE_SYNC_FD, GS_SHARED_SINGLE, GS_BGRA, true));
	destroy(d);
	expect_no_messages();
}

// The formats and modifiers linux-pipewire offers its producers: every listed (format, modifier) imports.
static void test_dmabuf_capabilities(void **)
{
	gs_device_t *d = device_or_skip();
	enum gs_dmabuf_flags flags;
	uint32_t *formats = nullptr;
	size_t nformats = 0;
	assert_true(query_dmabuf_caps(d, &flags, &formats, &nformats));
	assert_int_equal(flags, GS_DMABUF_FLAG_NONE); // implicit modifiers are unsupported
	bool argb = false, nv12 = false;
	for (size_t i = 0; i < nformats; ++i) {
		argb |= formats[i] == DRM_FORMAT_ARGB8888;
		nv12 |= formats[i] == DRM_FORMAT_NV12;
		uint64_t *mods = nullptr;
		size_t nmods = 0;
		assert_true(query_dmabuf_mods(d, formats[i], &mods, &nmods));
		assert_true(nmods > 0);
		bfree(mods);
	}
	bfree(formats);
	assert_true(argb);
	assert_true(nv12);
	uint64_t *mods = nullptr;
	size_t nmods = 0;
	assert_true(query_dmabuf_mods(d, DRM_FORMAT_ARGB8888, &mods, &nmods));
	bool linear = false;
	for (size_t i = 0; i < nmods; ++i) {
		linear |= mods[i] == DRM_FORMAT_MOD_LINEAR;
	}
	bfree(mods);
	assert_true(linear);
	assert_false(query_dmabuf_mods(d, DRM_FORMAT_C8, &mods, &nmods)); // no gs format for it
	destroy(d);
	expect_no_messages();
}

// Export on device A, import on device B, ordered by a shared timeline both ways: BGRA as a dmabuf (the driver's
// renderable modifier), NV12 as an opaque fd. Imports borrow the fds (closed right after), exports are new each time.
static void test_export_import_second_device(void **)
{
	gs_device_t *a = device_or_skip();
	bank_messages();
	gs_device_t *b = nullptr;
	assert_int_equal(create(&b, 0), GS_SUCCESS);
	within_30s([&] {
		struct gs_adapter_identity ia, ib;
		assert_true(get_identity(a, &ia) && get_identity(b, &ib));
		assert_memory_equal(ia.device_uuid, ib.device_uuid, 16);

		const uint32_t w = 64, h = 32;
		gs_texture_t *ta = nullptr;
		assert_true(create_exportable(a, w, h, GS_BGRA, GS_SHARED_SINGLE, GS_RENDER_TARGET, &ta, nullptr));
		gs_timeline_t *tla = timeline_create(a, 0);
		assert_non_null(tla);
		struct gs_shared_texture_desc desc;
		assert_false(texture_export(a, ta, GS_HANDLE_FD, &desc)); // not this texture's type
		assert_false(texture_export(a, ta, GS_HANDLE_WIN32, &desc));
		assert_true(texture_export(a, ta, GS_HANDLE_DMABUF, &desc));
		assert_int_equal(desc.type, GS_HANDLE_DMABUF);
		assert_int_equal(desc.width, w);
		assert_int_equal(desc.height, h);
		assert_int_equal(desc.format, texture_format(ta));
		assert_true(desc.num_planes >= 1);
		assert_true(desc.fds[0] >= 0);
		assert_true(desc.pitches[0] >= w * 4);
		assert_true(desc.modifier != GS_SHARED_MODIFIER_INVALID);
		print_message("BGRA render target exported with modifier 0x%016llx, pitch %u\n",
			      (unsigned long long)desc.modifier, desc.pitches[0]);
		struct gs_shared_texture_desc again;
		assert_true(texture_export(a, ta, GS_HANDLE_DMABUF, &again));
		assert_true(again.fds[0] != desc.fds[0]); // a new handle per export
		assert_int_equal(again.modifier, desc.modifier);
		close_fds(again);
		struct gs_shared_handle th;
		assert_true(timeline_export(a, tla, GS_HANDLE_FD, &th));
		assert_int_equal(th.type, GS_HANDLE_FD);
		struct gs_shared_handle not_fd;
		assert_false(timeline_export(a, tla, GS_HANDLE_SYNC_FD, &not_fd)); // timelines are opaque fds

		gs_timeline_t *tlb = timeline_import(b, &th);
		assert_non_null(tlb);
		close_handle(th); // imports borrow
		gs_texture_t *tb = nullptr;
		assert_true(texture_import(b, &desc, &tb, nullptr));
		close_fds(desc);

		// A writes, B reads after value 1.
		vk_clear(a, ta, 0.0f, 0.0f, 1.0f);
		assert_true(timeline_signal(a, tla, 1));
		assert_true(timeline_wait(b, tlb, 1));
		assert_true(all_equal(vk_read_bgra(b, tb), blue));
		// B writes, A reads after value 2.
		vk_clear(b, tb, 0.0f, 1.0f, 0.0f);
		assert_true(timeline_signal(b, tlb, 2));
		assert_true(timeline_wait(a, tla, 2));
		assert_true(all_equal(vk_read_bgra(a, ta), green));
		assert_int_equal(timeline_value(a, tla), 2);

		// A wrong device UUID is refused.
		struct gs_shared_texture_desc bad;
		assert_true(texture_export(a, ta, GS_HANDLE_DMABUF, &bad));
		bad.device_uuid[0] ^= 0xFF;
		gs_texture_t *none = nullptr;
		assert_false(texture_import(b, &bad, &none, nullptr));
		close_fds(bad);

		// NV12 render target: an opaque fd. A fills both planes, B stages them.
		const uint32_t nw = 256, nh = 128;
		gs_texture_t *ya = nullptr, *uva = nullptr;
		assert_true(create_exportable(a, nw, nh, GS_UNKNOWN, GS_SHARED_NV12, GS_RENDER_TARGET, &ya, &uva));
		fill_nv12(a, ya, uva, nw, nh);
		assert_false(texture_export(a, uva, GS_HANDLE_DMABUF, &desc));
		assert_true(texture_export(a, uva, GS_HANDLE_FD, &desc)); // either plane view exports the image
		assert_int_equal(desc.width, nw);
		assert_int_equal(desc.layout, GS_SHARED_NV12);
		assert_true(desc.size > 0);
		gs_texture_t *yb = nullptr, *uvb = nullptr;
		assert_true(texture_import(b, &desc, &yb, &uvb));
		close_fds(desc);
		assert_true(timeline_signal(a, tla, 3));
		// The validation layer cannot see that tla and tlb share a payload: it checks A's signal of 3 when it
		// retires that submission, and if B's 4 has already been seen by then it reports 3 < 4
		// (VUID-VkSubmitInfo2-semaphore-03882), although 3 ran first (B waits for it). A retires it here.
		assert_int_equal(timeline_wait_cpu(a, tla, 3, 5000000000ull), 0);
		assert_true(timeline_wait(b, tlb, 3));
		assert_int_equal(nv12_bad_rows(b, yb, nw, nh, nv12_expected(nw, nh)), 0);
		// B is done reading before A's next use: B signals, A waits on the CPU.
		assert_true(timeline_signal(b, tlb, 4));
		assert_int_equal(timeline_wait_cpu(a, tla, 4, 5000000000ull), 0);

		texture_destroy(yb);
		texture_destroy(uvb);
		texture_destroy(ya);
		texture_destroy(uva);
		texture_destroy(tb);
		texture_destroy(ta);
		timeline_destroy(tlb);
		timeline_destroy(tla);
	});
	destroy(b);
	bank_messages();
	destroy(a);
	expect_no_messages();
}

// NV12/P010 copy targets (no GS_RENDER_TARGET) are LINEAR dmabufs from GBM: what Vulkan writes is where the
// description says, as a foreign consumer (VA-API, a compositor, PipeWire) reads it. Read here through mmap.
static void gbm_copy_target_case(bool p010)
{
	gs_device_t *d = device_or_skip();
	within_30s([&] {
		const uint32_t w = 256, h = 128;
		const enum gs_shared_layout layout = p010 ? GS_SHARED_P010 : GS_SHARED_NV12;
		if (!supported(d, GS_HANDLE_DMABUF, layout, GS_UNKNOWN, false)) {
			print_message("GBM cannot allocate %s here, not checked\n", p010 ? "P010" : "NV12");
			return;
		}
		gs_texture_t *y = nullptr, *uv = nullptr;
		assert_true(create_exportable(d, w, h, GS_UNKNOWN, layout, 0, &y, &uv));
		struct gs_shared_texture_desc desc;
		assert_false(texture_export(d, y, GS_HANDLE_FD, &desc));
		assert_true(texture_export(d, y, GS_HANDLE_DMABUF, &desc));
		assert_int_equal(desc.modifier, DRM_FORMAT_MOD_LINEAR);
		assert_int_equal(desc.num_planes, 2);
		assert_int_equal(desc.layout, layout);
		gs_timeline_t *t = timeline_create(d, 0);
		std::vector<uint8_t> expect;
		if (p010) {
			// Constant planes: Y 0.5, UV (0.25, 0.75) through 16-bit textures.
			std::vector<uint16_t> py(w * h, 0x8000), puv(w * h / 2);
			for (size_t i = 0; i < puv.size(); i += 2) {
				puv[i] = 0x4000;
				puv[i + 1] = 0xC000;
			}
			const uint8_t *dy = (const uint8_t *)py.data(), *duv = (const uint8_t *)puv.data();
			gs_texture_t *ty = texture_create(d, w, h, GS_R16, 1, &dy, 0);
			gs_texture_t *tuv = texture_create(d, w / 2, h / 2, GS_RG16, 1, &duv, 0);
			copy_texture(d, y, ty);
			copy_texture(d, uv, tuv);
			texture_destroy(ty);
			texture_destroy(tuv);
		} else {
			fill_nv12(d, y, uv, w, h);
			expect = nv12_expected(w, h);
		}
		assert_true(timeline_signal(d, t, 1));
		assert_int_equal(timeline_wait_cpu(d, t, 1, 5000000000ull), 0);
		size_t bad = 0;
		{
			dmabuf_map m(desc.fds[0], DMA_BUF_SYNC_READ);
			const uint32_t bpp = p010 ? 2 : 1;
			for (uint32_t j = 0; j < h; ++j) {
				const uint8_t *row = m.data + desc.offsets[0] + (size_t)j * desc.pitches[0];
				if (p010) {
					for (uint32_t i = 0; i < w; ++i) {
						bad += ((const uint16_t *)row)[i] != 0x8000;
					}
				} else {
					bad += memcmp(row, &expect[(size_t)j * w], w * bpp) != 0;
				}
			}
			for (uint32_t j = 0; j < h / 2; ++j) {
				const uint8_t *row = m.data + desc.offsets[1] + (size_t)j * desc.pitches[1];
				if (p010) {
					for (uint32_t i = 0; i < w; i += 2) {
						bad += ((const uint16_t *)row)[i] != 0x4000 ||
						       ((const uint16_t *)row)[i + 1] != 0xC000;
					}
				} else {
					bad += memcmp(row, &expect[(size_t)w * h + (size_t)j * w], w) != 0;
				}
			}
		}
		assert_int_equal(bad, 0);
		// A GBM copy target is no render target.
		gs_texture_t *rt = nullptr, *rt_uv = nullptr;
		assert_true(create_exportable(d, w, h, GS_UNKNOWN, layout, GS_RENDER_TARGET, &rt, &rt_uv));
		struct gs_shared_texture_desc rt_desc;
		assert_false(texture_export(d, rt, GS_HANDLE_DMABUF, &rt_desc)); // render targets are opaque fds
		assert_true(texture_export(d, rt, GS_HANDLE_FD, &rt_desc));
		close_fds(rt_desc);
		texture_destroy(rt);
		texture_destroy(rt_uv);
		close_fds(desc);
		texture_destroy(y);
		texture_destroy(uv);
		timeline_destroy(t);
	});
	destroy(d);
	expect_no_messages();
}

static void test_gbm_nv12_copy_target(void **)
{
	gbm_copy_target_case(false);
}

static void test_gbm_p010_copy_target(void **)
{
	gbm_copy_target_case(true);
}

// dmabufs another process allocated with GBM and wrote on the CPU: LINEAR BGRA through gs_texture_import and the
// legacy gs_texture_create_from_dmabuf, NV12 through gs_texture_import (both plane views).
static void test_import_gbm_dmabufs(void **)
{
	gs_device_t *d = device_or_skip();
	within_30s([&] {
		render_node node;
		if (!node.open_first()) {
			print_message("no render node with GBM, not checked\n");
			return;
		}
		const uint32_t w = 96, h = 40;
		struct gbm_bo *bo = gbm_bo_create(node.gbm, w, h, GBM_FORMAT_ARGB8888, GBM_BO_USE_LINEAR);
		assert_non_null(bo);
		struct gs_shared_texture_desc desc = describe_bo(bo, w, h, GS_BGRA, GS_SHARED_SINGLE);
		{
			dmabuf_map m(desc.fds[0], DMA_BUF_SYNC_WRITE);
			for (uint32_t y = 0; y < h; ++y) {
				for (uint32_t x = 0; x < w; ++x) {
					const uint32_t px = bgra_px(x, y, 1);
					memcpy(m.data + desc.offsets[0] + (size_t)y * desc.pitches[0] + x * 4, &px, 4);
				}
			}
		}
		gs_texture_t *t = nullptr;
		assert_true(texture_import(d, &desc, &t, nullptr));
		std::vector<uint32_t> got = vk_read_bgra(d, t);
		size_t bad = 0;
		for (uint32_t y = 0; y < h; ++y) {
			for (uint32_t x = 0; x < w; ++x) {
				bad += got[y * w + x] != bgra_px(x, y, 1);
			}
		}
		assert_int_equal(bad, 0);
		texture_destroy(t);

		// The same buffer through the legacy export, as linux-pipewire calls it.
		const uint32_t stride = desc.pitches[0], offset = desc.offsets[0];
		const uint64_t mod = desc.modifier;
		gs_texture_t *legacy =
			create_from_dmabuf(d, w, h, DRM_FORMAT_ARGB8888, GS_BGRA, 1, desc.fds, &stride, &offset, &mod);
		assert_non_null(legacy);
		got = vk_read_bgra(d, legacy);
		assert_int_equal(got[5 * w + 7], bgra_px(7, 5, 1));
		texture_destroy(legacy);
		// Implicit modifiers are refused.
		const uint64_t implicit = DRM_FORMAT_MOD_INVALID;
		assert_null(create_from_dmabuf(d, w, h, DRM_FORMAT_ARGB8888, GS_BGRA, 1, desc.fds, &stride, &offset,
					       &implicit));
		// LINEAR cannot be rendered to here: an import as a render target is refused, not broken.
		desc.flags = GS_RENDER_TARGET;
		gs_texture_t *rt = nullptr;
		const bool rt_ok = texture_import(d, &desc, &rt, nullptr);
		if (rt_ok) {
			texture_destroy(rt);
		}
		print_message("LINEAR BGRA dmabuf as a render target: %s\n", rt_ok ? "imported" : "refused");
		close_fds(desc);
		gbm_bo_destroy(bo);

		// NV12: one buffer, two planes.
		const uint32_t nw = 128, nh = 64;
		bo = gbm_bo_create(node.gbm, nw, nh, GBM_FORMAT_NV12, GBM_BO_USE_LINEAR);
		assert_non_null(bo);
		desc = describe_bo(bo, nw, nh, GS_UNKNOWN, GS_SHARED_NV12);
		assert_int_equal(desc.num_planes, 2);
		const std::vector<uint8_t> e = nv12_expected(nw, nh);
		{
			dmabuf_map m(desc.fds[0], DMA_BUF_SYNC_WRITE);
			for (uint32_t j = 0; j < nh; ++j) {
				memcpy(m.data + desc.offsets[0] + (size_t)j * desc.pitches[0], &e[(size_t)j * nw], nw);
			}
			for (uint32_t j = 0; j < nh / 2; ++j) {
				memcpy(m.data + desc.offsets[1] + (size_t)j * desc.pitches[1],
				       &e[(size_t)nw * nh + (size_t)j * nw], nw);
			}
		}
		gs_texture_t *y = nullptr, *uv = nullptr;
		assert_true(texture_import(d, &desc, &y, &uv));
		assert_non_null(uv);
		assert_int_equal(texture_width(uv), nw / 2);
		assert_int_equal(nv12_bad_rows(d, y, nw, nh, e), 0);
		texture_destroy(y);
		texture_destroy(uv);
		// NV12 through the legacy single-texture export is refused (one gs_texture cannot hold both planes).
		assert_null(create_from_dmabuf(d, nw, nh, DRM_FORMAT_NV12, GS_R8, 2, desc.fds, desc.pitches,
					       desc.offsets, &desc.modifier));
		close_fds(desc);
		gbm_bo_destroy(bo);
	});
	destroy(d);
	expect_no_messages();
}

static void test_timeline_cpu(void **)
{
	gs_device_t *d = device_or_skip();
	within_30s([&] {
		gs_timeline_t *t = timeline_create(d, 5);
		assert_non_null(t);
		assert_int_equal(timeline_value(d, t), 5);
		assert_true(timeline_signal_cpu(d, t, 7));
		assert_false(timeline_signal_cpu(d, t, 6)); // only forward
		assert_int_equal(timeline_wait_cpu(d, t, 7, 0), 0);
		assert_int_equal(timeline_wait_cpu(d, t, 8, 1000000), ETIMEDOUT);
		assert_true(timeline_wait(d, t, 9));
		assert_true(timeline_signal(d, t, 10));
		assert_int_equal(timeline_wait_cpu(d, t, 10, 1000000), ETIMEDOUT);
		assert_true(timeline_signal_cpu(d, t, 9));
		assert_int_equal(timeline_wait_cpu(d, t, 10, 5000000000ull), 0);
		// An imported fd reads the exporter's value and moves with it.
		struct gs_shared_handle h;
		assert_true(timeline_export(d, t, GS_HANDLE_FD, &h));
		gs_timeline_t *i = timeline_import(d, &h);
		close_handle(h);
		assert_non_null(i);
		assert_int_equal(timeline_value(d, i), 10);
		assert_true(timeline_signal_cpu(d, t, 11));
		assert_int_equal(timeline_wait_cpu(d, i, 11, 5000000000ull), 0);
		timeline_destroy(i);
		timeline_destroy(t);
	});
	destroy(d);
	expect_no_messages();
}

// sync_files both ways: B renders into a texture shared with A and exports a sync_file after it; A's submission after
// gs_sync_file_wait reads B's pixels. A sync_file exported from work that waits on a timeline needs that
// timeline's signal submitted first (VUID-VkSemaphoreGetFdInfoKHR-handleType-03254): a CPU signal that comes later
// cannot gate it, so B's export is not checked for being pending here (spike D showed it is).
static void test_sync_file(void **)
{
	gs_device_t *a = device_or_skip();
	bank_messages();
	gs_device_t *b = nullptr;
	assert_int_equal(create(&b, 0), GS_SUCCESS);
	within_30s([&] {
		const uint32_t w = 512, h = 512;
		gs_texture_t *tb = nullptr;
		assert_true(create_exportable(b, w, h, GS_BGRA, GS_SHARED_SINGLE, GS_RENDER_TARGET, &tb, nullptr));
		struct gs_shared_texture_desc desc;
		assert_true(texture_export(b, tb, GS_HANDLE_DMABUF, &desc));
		gs_texture_t *ta = nullptr;
		assert_true(texture_import(a, &desc, &ta, nullptr));
		close_fds(desc);
		for (int round = 0; round < 3; ++round) {
			const float c = round == 1 ? 1.0f : 0.0f;
			vk_clear(b, tb, c, 1.0f - c, round == 2 ? 1.0f : 0.0f);
			struct gs_shared_handle sf;
			assert_true(sync_file_export(b, &sf));
			assert_int_equal(sf.type, GS_HANDLE_SYNC_FD);
			assert_true(sf.fd >= 0);
			struct gs_shared_handle wrong = {GS_HANDLE_FD, nullptr, sf.fd};
			assert_false(sync_file_wait(a, &wrong));
			assert_true(sync_file_wait(a, &sf));
			close_handle(sf); // the wait borrows it
			const uint32_t want = round == 0 ? green : round == 1 ? red : 0xFF00FF00u | 0xFFu;
			assert_true(all_equal(vk_read_bgra(a, ta), want));
			// A is done reading before B's next clear: A's own sync_file, waited on by B.
			struct gs_shared_handle back;
			assert_true(sync_file_export(a, &back));
			assert_true(sync_file_wait(b, &back));
			assert_true(sync_file_signalled(back.fd, 5000));
			close_handle(back);
		}
		// A signalled sync_file (fd -1) and an export with nothing recorded.
		struct gs_shared_handle none = {GS_HANDLE_SYNC_FD, nullptr, -1};
		assert_true(sync_file_wait(a, &none));
		struct gs_shared_handle idle;
		assert_true(sync_file_export(a, &idle));
		assert_true(sync_file_signalled(idle.fd, 5000));
		close_handle(idle);
		texture_destroy(ta);
		texture_destroy(tb);
	});
	destroy(b);
	bank_messages();
	destroy(a);
	expect_no_messages();
}

// PipeWire explicit sync (gs_sync_*): the producer's acquire point gates this device's work, and the release point
// gets this device's fence. The syncobj comes from the test's own render node fd, as another process's would.
static void test_syncobj_timeline_points(void **)
{
	gs_device_t *d = device_or_skip();
	within_30s([&] {
		if (!query_sync(d)) {
			print_message("no explicit sync (render node or SYNC_FD missing), not checked\n");
			return;
		}
		render_node node;
		assert_true(node.open_first());
		uint32_t obj = 0;
		assert_int_equal(drmSyncobjCreate(node.fd, 0, &obj), 0);
		int obj_fd = -1;
		assert_int_equal(drmSyncobjHandleToFD(node.fd, obj, &obj_fd), 0);

		// Acquire point 1, signalled by the "producer" from another thread while device_sync_create_from_* waits
		// for it to have a fence.
		std::thread producer([&] {
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			uint64_t p = 1;
			drmSyncobjTimelineSignal(node.fd, &obj, &p, 1);
		});
		gs_sync_t *acquire = sync_from_point(d, obj_fd, 1);
		producer.join();
		assert_non_null(acquire);
		assert_true(sync_wait(d, acquire));
		sync_destroy(d, acquire);
		gs_texture_t *t = texture_create(d, 16, 16, GS_BGRA, 1, nullptr, GS_RENDER_TARGET);
		vk_clear(d, t, 0.0f, 1.0f, 0.0f);
		// Release point 2: this device's fence after the clear.
		gs_sync_t *release = sync_create(d);
		assert_non_null(release);
		assert_true(sync_export_point(d, release, obj_fd, 2));
		sync_destroy(d, release);
		uint64_t p2 = 2;
		assert_int_equal(drmSyncobjTimelineWait(node.fd, &obj, &p2, 1, INT64_MAX, 0, nullptr), 0);
		assert_true(all_equal(vk_read_bgra(d, t), green));
		// A CPU signal of point 3.
		assert_true(sync_signal_point(d, obj_fd, 3));
		uint64_t value = 0;
		assert_int_equal(drmSyncobjQuery(node.fd, &obj, &value, 1), 0);
		assert_int_equal(value, 3);
		texture_destroy(t);
		close(obj_fd);
		drmSyncobjDestroy(node.fd, obj);
	});
	destroy(d);
	expect_no_messages();
}

// The pitch-linear frame buffer as an opaque fd: both planes land at the reported offsets and pitches.
static void sharedbuf_case(bool p010)
{
	gs_device_t *d = device_or_skip();
	within_30s([&] {
		const uint32_t w = 200, h = 120; // pitch is padded: 200 (400) bytes -> 256 (512)
		const enum gs_shared_layout layout = p010 ? GS_SHARED_P010 : GS_SHARED_NV12;
		gs_sharedbuf_t *buf = sharedbuf_create(d, w, h, layout);
		assert_non_null(buf);
		struct gs_shared_texture_desc desc;
		assert_false(sharedbuf_export(d, buf, GS_HANDLE_DMABUF, &desc));
		assert_true(sharedbuf_export(d, buf, GS_HANDLE_FD, &desc));
		assert_true(desc.fds[0] >= 0);
		assert_int_equal(desc.num_planes, 2);
		assert_int_equal(desc.pitches[0], p010 ? 512 : 256);
		assert_int_equal(desc.offsets[1], desc.pitches[0] * h);
		assert_true(desc.size >= (uint64_t)desc.pitches[0] * h * 3 / 2);
		close_fds(desc);

		gs_texture_t *y = nullptr, *uv = nullptr;
		assert_true((p010 ? create_p010 : create_nv12)(d, &y, &uv, w, h, GS_RENDER_TARGET));
		std::vector<uint8_t> expect;
		if (p010) {
			vk_clear(d, y, 0.5f, 0.0f, 0.0f);
			vk_clear(d, uv, 0.25f, 0.75f, 0.0f);
		} else {
			fill_nv12(d, y, uv, w, h);
			expect = nv12_expected(w, h);
		}
		assert_true(sharedbuf_copy(d, buf, y));
		assert_false(sharedbuf_copy(d, buf, uv)); // needs the Y plane
		std::vector<uint8_t> out(desc.offsets[1] + (size_t)desc.pitches[1] * h / 2);
		assert_true(read_sharedbuf(buf, out.data(), out.size()));
		size_t bad = 0;
		if (p010) {
			auto off = [&](uint32_t o, int want) {
				const int v = (out[o] | (out[o + 1] << 8)) >> 6;
				return v < want - 1 || v > want + 1;
			};
			for (uint32_t j = 0; j < h; ++j) {
				for (uint32_t i = 0; i < w; ++i) {
					bad += off(j * desc.pitches[0] + i * 2, 512);
				}
			}
			for (uint32_t j = 0; j < h / 2; ++j) {
				for (uint32_t i = 0; i < w / 2; ++i) {
					const uint32_t o = desc.offsets[1] + j * desc.pitches[1] + i * 4;
					bad += off(o, 256) || off(o + 2, 767);
				}
			}
		} else {
			for (uint32_t j = 0; j < h; ++j) {
				bad += memcmp(&out[(size_t)j * desc.pitches[0]], &expect[(size_t)j * w], w) != 0;
			}
			for (uint32_t j = 0; j < h / 2; ++j) {
				bad += memcmp(&out[desc.offsets[1] + (size_t)j * desc.pitches[1]],
					      &expect[(size_t)w * h + (size_t)j * w], w) != 0;
			}
		}
		assert_int_equal(bad, 0);
		texture_destroy(y);
		texture_destroy(uv);
		sharedbuf_destroy(buf);
	});
	destroy(d);
	expect_no_messages();
}

static void test_sharedbuf_nv12(void **)
{
	sharedbuf_case(false);
}

static void test_sharedbuf_p010(void **)
{
	sharedbuf_case(true);
}

/* ---- Cross-process ------------------------------------------------------------------------------------------ */

// What the parent sends with the fds (SCM_RIGHTS, in this order: the BGRA dmabuf's planes, the NV12 opaque fd, the
// timeline). The fd fields of the descriptions are filled by the receiver.
struct child_msg {
	struct gs_shared_texture_desc desc, nv12_desc;
};

static const uint32_t cross_nv12_w = 256, cross_nv12_h = 128;

static bool send_fds(int sock, const void *data, size_t size, const std::vector<int> &fds)
{
	struct iovec iov = {(void *)data, size};
	std::vector<char> ctrl(CMSG_SPACE(sizeof(int) * fds.size()));
	struct msghdr m = {};
	m.msg_iov = &iov;
	m.msg_iovlen = 1;
	m.msg_control = ctrl.data();
	m.msg_controllen = ctrl.size();
	struct cmsghdr *c = CMSG_FIRSTHDR(&m);
	c->cmsg_level = SOL_SOCKET;
	c->cmsg_type = SCM_RIGHTS;
	c->cmsg_len = CMSG_LEN(sizeof(int) * fds.size());
	memcpy(CMSG_DATA(c), fds.data(), sizeof(int) * fds.size());
	return sendmsg(sock, &m, 0) == (ssize_t)size;
}

static bool recv_fds(int sock, void *data, size_t size, std::vector<int> &fds)
{
	struct iovec iov = {data, size};
	char ctrl[CMSG_SPACE(sizeof(int) * 8)];
	struct msghdr m = {};
	m.msg_iov = &iov;
	m.msg_iovlen = 1;
	m.msg_control = ctrl;
	m.msg_controllen = sizeof(ctrl);
	if (recvmsg(sock, &m, MSG_CMSG_CLOEXEC) != (ssize_t)size) {
		return false;
	}
	for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
		if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
			const size_t n = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
			fds.resize(n);
			memcpy(fds.data(), CMSG_DATA(c), n * sizeof(int));
		}
	}
	return true;
}

// Child: imports the textures and timeline, waits for 1, checks the parent's red and NV12 pattern, clears green,
// signals 2.
static int child_main(int sock)
{
	child_msg msg;
	std::vector<int> fds;
	if (!recv_fds(sock, &msg, sizeof(msg), fds) || fds.size() != msg.desc.num_planes + 2) {
		fprintf(stderr, "child: no message\n");
		return 10;
	}
	for (uint32_t p = 0; p < GS_SHARED_MAX_PLANES; ++p) {
		msg.desc.fds[p] = p < msg.desc.num_planes ? fds[p] : -1;
		msg.nv12_desc.fds[p] = -1;
	}
	msg.nv12_desc.fds[0] = fds[msg.desc.num_planes];
	struct gs_shared_handle th = {GS_HANDLE_FD, nullptr, fds[msg.desc.num_planes + 1]};
	setenv("OBS_VULKAN_VALIDATION", "1", 1);
	module = os_dlopen(OBS_VULKAN_MODULE);
	if (!module || !load_all()) {
		return 11;
	}
	gs_device_t *d = nullptr;
	if (create(&d, 0) != GS_SUCCESS) {
		return 12;
	}
	gs_texture_t *tex = nullptr, *y = nullptr, *uv = nullptr;
	gs_timeline_t *t = timeline_import(d, &th);
	if (!texture_import(d, &msg.desc, &tex, nullptr) || !texture_import(d, &msg.nv12_desc, &y, &uv) || !t) {
		return 13;
	}
	for (int fd : fds) {
		close(fd);
	}
	int ret = 0;
	if (!timeline_wait(d, t, 1)) {
		ret = 14;
	} else if (!all_equal(vk_read_bgra(d, tex), red)) {
		ret = 15;
	} else if (nv12_bad_rows(d, y, cross_nv12_w, cross_nv12_h, nv12_expected(cross_nv12_w, cross_nv12_h)) != 0) {
		fprintf(stderr, "child: NV12 rows differ\n");
		ret = 18;
	}
	vk_clear(d, tex, 0.0f, 1.0f, 0.0f);
	if (!timeline_signal(d, t, 2) || timeline_wait_cpu(d, t, 2, 10000000000ull) != 0) {
		ret = ret ? ret : 16;
	}
	texture_destroy(tex);
	texture_destroy(y);
	texture_destroy(uv);
	timeline_destroy(t);
	destroy(d);
	if (messages() > 0) {
		fprintf(stderr, "child: %d validation messages\n", messages());
		ret = ret ? ret : 17;
	}
	return ret;
}

static void test_cross_process(void **)
{
	gs_device_t *d = device_or_skip();
	within_30s([&] {
		const uint32_t w = 64, h = 64;
		gs_texture_t *tex = nullptr;
		assert_true(create_exportable(d, w, h, GS_BGRA, GS_SHARED_SINGLE, GS_RENDER_TARGET, &tex, nullptr));
		gs_timeline_t *t = timeline_create(d, 0);
		assert_non_null(t);
		gs_texture_t *y = nullptr, *uv = nullptr;
		assert_true(create_exportable(d, cross_nv12_w, cross_nv12_h, GS_UNKNOWN, GS_SHARED_NV12,
					      GS_RENDER_TARGET, &y, &uv));
		fill_nv12(d, y, uv, cross_nv12_w, cross_nv12_h);
		child_msg msg = {};
		assert_true(texture_export(d, tex, GS_HANDLE_DMABUF, &msg.desc));
		assert_true(texture_export(d, y, GS_HANDLE_FD, &msg.nv12_desc));
		struct gs_shared_handle th;
		assert_true(timeline_export(d, t, GS_HANDLE_FD, &th));

		// The child gets one end of a socket pair as fd 3 and nothing else (every other fd here is CLOEXEC).
		int sv[2];
		assert_int_equal(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv), 0);
		char exe[4096];
		const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
		assert_true(n > 0);
		exe[n] = 0;
		const pid_t pid = fork();
		assert_true(pid >= 0);
		if (pid == 0) {
			dup2(sv[1], 3);
			execl(exe, exe, "child", (char *)nullptr);
			_exit(99);
		}
		close(sv[1]);
		std::vector<int> fds;
		for (uint32_t p = 0; p < msg.desc.num_planes; ++p) {
			fds.push_back(msg.desc.fds[p]);
		}
		fds.push_back(msg.nv12_desc.fds[0]);
		fds.push_back(th.fd);
		assert_true(send_fds(sv[0], &msg, sizeof(msg), fds));
		close(sv[0]);
		close_fds(msg.desc);
		close_fds(msg.nv12_desc);
		close_handle(th);

		vk_clear(d, tex, 1.0f, 0.0f, 0.0f);
		assert_true(timeline_signal(d, t, 1));
		assert_true(timeline_wait(d, t, 2));
		assert_true(all_equal(vk_read_bgra(d, tex), green));

		int status = 0;
		assert_int_equal(waitpid(pid, &status, 0), pid);
		assert_true(WIFEXITED(status));
		assert_int_equal(WEXITSTATUS(status), 0);
		texture_destroy(tex);
		texture_destroy(y);
		texture_destroy(uv);
		timeline_destroy(t);
	});
	destroy(d);
	expect_no_messages();
}

int main(int argc, char **argv)
{
	setvbuf(stdout, nullptr, _IONBF, 0); // nothing lost if the process ends early
	if (argc > 1 && !strcmp(argv[1], "child")) {
		return child_main(3);
	}
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_identity_and_support),
		cmocka_unit_test(test_dmabuf_capabilities),
		cmocka_unit_test(test_export_import_second_device),
		cmocka_unit_test(test_gbm_nv12_copy_target),
		cmocka_unit_test(test_gbm_p010_copy_target),
		cmocka_unit_test(test_import_gbm_dmabufs),
		cmocka_unit_test(test_timeline_cpu),
		cmocka_unit_test(test_sync_file),
		cmocka_unit_test(test_syncobj_timeline_points),
		cmocka_unit_test(test_sharedbuf_nv12),
		cmocka_unit_test(test_sharedbuf_p010),
		cmocka_unit_test(test_cross_process),
	};
	return cmocka_run_group_tests(tests, setup, teardown);
}
