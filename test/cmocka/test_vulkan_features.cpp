// Native objects (device_get_device_obj, gs_texture_get_obj), GPU timers and timer ranges, debug markers, cube
// textures (unsupported), HDR detection and swapchain format choice, fast clear. Every test runs under the
// validation layer with synchronization validation and expects no messages.
#include <stdio.h>
#include <stdlib.h>

#include <vector>

#include <graphics/graphics.h>
#include <graphics/graphics-vulkan.h>
#include <graphics/vec4.h>
#include <util/platform.h>

#include "vulkan-subsystem.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

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
static void *(*get_device_obj)(gs_device_t *);
static void *(*texture_get_obj)(gs_texture_t *);
static gs_texture_t *(*texture_create)(gs_device_t *, uint32_t, uint32_t, enum gs_color_format, uint32_t,
				       const uint8_t **, uint32_t);
static bool (*texture_create_nv12)(gs_device_t *, gs_texture_t **, gs_texture_t **, uint32_t, uint32_t, uint32_t);
static void (*texture_destroy)(gs_texture_t *);
static void (*set_render_target)(gs_device_t *, gs_texture_t *, gs_zstencil_t *);
static void (*clear)(gs_device_t *, uint32_t, const struct vec4 *, float, uint8_t);
static void (*copy_texture)(gs_device_t *, gs_texture_t *, gs_texture_t *);
static void (*flush)(gs_device_t *);
static gs_timer_t *(*timer_create)(gs_device_t *);
static void (*timer_destroy)(gs_timer_t *);
static void (*timer_begin)(gs_timer_t *);
static void (*timer_end)(gs_timer_t *);
static bool (*timer_get_data)(gs_timer_t *, uint64_t *);
static gs_timer_range_t *(*timer_range_create)(gs_device_t *);
static void (*timer_range_destroy)(gs_timer_range_t *);
static void (*timer_range_begin)(gs_timer_range_t *);
static void (*timer_range_end)(gs_timer_range_t *);
static bool (*timer_range_get_data)(gs_timer_range_t *, bool *, uint64_t *);
static void (*marker_begin)(gs_device_t *, const char *, const float[4]);
static void (*marker_end)(gs_device_t *);
static gs_texture_t *(*cubetexture_create)(gs_device_t *, uint32_t, enum gs_color_format, uint32_t, const uint8_t **,
					   uint32_t);
static bool (*is_monitor_hdr)(gs_device_t *, void *);
static bool (*monitor_info)(gs_device_t *, void *, bool *, uint32_t *);
static bool (*choose_format)(const VkSurfaceFormatKHR *, uint32_t, enum gs_color_format, bool, uint32_t,
			     enum gs_color_space *, VkSurfaceFormatKHR *);
static bool (*fast_clear)(gs_device_t *);
static gs_swapchain_t *(*swapchain_create)(gs_device_t *, const struct gs_init_data *);
static void (*swapchain_destroy)(gs_swapchain_t *);
static void (*load_swapchain)(gs_device_t *, gs_swapchain_t *);
static void (*update_color_space)(gs_device_t *);
static enum gs_color_space (*get_color_space)(gs_device_t *);
static bool (*present_ready)(gs_device_t *);
static void (*present)(gs_device_t *);

template<typename T> static bool load(T &fn, const char *name)
{
	fn = (T)os_dlsym(module, name);
	if (!fn) {
		print_message("missing export: %s\n", name);
	}
	return fn != nullptr;
}

static int setup(void **)
{
#ifdef _WIN32
	_putenv_s("OBS_VULKAN_VALIDATION", "1");
#else
	setenv("OBS_VULKAN_VALIDATION", "1", 1);
#endif
	module = os_dlopen(OBS_VULKAN_MODULE);
	if (!module) {
		return -1;
	}
	bool ok =
		load(create, "device_create") & load(destroy, "device_destroy") &
		load(messages, "obs_vulkan_validation_messages") & load(get_device_obj, "device_get_device_obj") &
		load(texture_get_obj, "gs_texture_get_obj") & load(texture_create, "device_texture_create") &
		load(texture_create_nv12, "device_texture_create_nv12") & load(texture_destroy, "gs_texture_destroy") &
		load(set_render_target, "device_set_render_target") & load(clear, "device_clear") &
		load(copy_texture, "device_copy_texture") & load(flush, "device_flush") &
		load(timer_create, "device_timer_create") & load(timer_destroy, "gs_timer_destroy") &
		load(timer_begin, "gs_timer_begin") & load(timer_end, "gs_timer_end") &
		load(timer_get_data, "gs_timer_get_data") & load(timer_range_create, "device_timer_range_create") &
		load(timer_range_destroy, "gs_timer_range_destroy") & load(timer_range_begin, "gs_timer_range_begin") &
		load(timer_range_end, "gs_timer_range_end") & load(timer_range_get_data, "gs_timer_range_get_data") &
		load(marker_begin, "device_debug_marker_begin") & load(marker_end, "device_debug_marker_end") &
		load(cubetexture_create, "device_cubetexture_create") & load(is_monitor_hdr, "device_is_monitor_hdr") &
		load(monitor_info, "obs_vulkan_test_monitor_info") &
		load(choose_format, "obs_vulkan_test_choose_surface_format") &
		load(fast_clear, "device_can_adapter_fast_clear") & load(swapchain_create, "device_swapchain_create") &
		load(swapchain_destroy, "gs_swapchain_destroy") & load(load_swapchain, "device_load_swapchain") &
		load(update_color_space, "device_update_color_space") &
		load(get_color_space, "device_get_color_space") & load(present_ready, "device_is_present_ready") &
		load(present, "device_present");
	return ok ? 0 : -1;
}

static int teardown(void **)
{
	os_dlclose(module);
	return 0;
}

static gs_device *open_device()
{
	gs_device_t *device = nullptr;
	int ret = create(&device, 0);
	if (ret == GS_ERROR_NOT_SUPPORTED) {
		skip();
	}
	assert_int_equal(ret, GS_SUCCESS);
	if (messages() < 0) {
		destroy(device);
		const char *rq = getenv("OBS_VULKAN_REQUIRE_VALIDATION");
		if (rq && !strcmp(rq, "1")) {
			fail_msg("validation layer required (OBS_VULKAN_REQUIRE_VALIDATION=1) but missing");
		}
		skip();
	}
	return device;
}

static void close_device(gs_device *d)
{
	destroy(d);
	assert_int_equal(messages(), 0);
}

/* ---- native objects ---- */

static void test_device_obj_has_the_handles(void **)
{
	gs_device *d = open_device();
	auto *n = (const gs_vulkan_device *)get_device_obj(d);
	assert_non_null(n);
	assert_int_equal(n->version, GS_VULKAN_OBJECTS_VERSION);
	assert_true(n->instance == d->instance);
	assert_true(n->physical_device == d->phys);
	assert_true(n->device == d->dev);
	assert_true(n->queue == d->graphics.q);
	assert_int_equal(n->queue_family_index, d->graphics.family);
	assert_int_equal(n->queue_index, 0);
	assert_true(n->api_version >= VK_API_VERSION_1_3);
	assert_non_null(n->get_instance_proc_addr);
	// Usable as documented: a device-level entry point through the instance's loader.
	auto gdpa = (PFN_vkGetDeviceProcAddr)n->get_instance_proc_addr(n->instance, "vkGetDeviceProcAddr");
	assert_non_null(gdpa);
	assert_non_null(gdpa(n->device, "vkQueueSubmit2"));
	close_device(d);
}

static void test_texture_obj_has_the_handles(void **)
{
	gs_device *d = open_device();
	gs_texture_t *t = texture_create(d, 16, 8, GS_RGBA, 1, nullptr, GS_RENDER_TARGET);
	assert_non_null(t);
	set_render_target(d, t, nullptr);
	vec4 c;
	vec4_set(&c, 1, 0, 0, 1);
	clear(d, GS_CLEAR_COLOR, &c, 1.0f, 0);
	flush(d);
	auto *n = (const gs_vulkan_texture *)texture_get_obj(t);
	assert_non_null(n);
	assert_int_equal(n->version, GS_VULKAN_OBJECTS_VERSION);
	assert_true(n->image == t->image);
	assert_true(n->view == t->view);
	assert_true(n->srgb_view == t->srgb_view && n->srgb_view != VK_NULL_HANDLE); // GS_RGBA has an sRGB view
	assert_int_equal(n->format, VK_FORMAT_R8G8B8A8_UNORM);
	assert_int_equal(n->aspect, VK_IMAGE_ASPECT_COLOR_BIT);
	assert_int_equal(n->image_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL); // the clear
	assert_int_equal(n->width, 16);
	assert_int_equal(n->height, 8);
	assert_int_equal(n->levels, 1);
	set_render_target(d, nullptr, nullptr);

	gs_texture_t *y = nullptr, *uv = nullptr;
	if (texture_create_nv12(d, &y, &uv, 64, 32, 0)) {
		auto *ny = (const gs_vulkan_texture *)texture_get_obj(y);
		assert_int_equal(ny->format, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM);
		assert_int_equal(ny->aspect, VK_IMAGE_ASPECT_PLANE_0_BIT);
		assert_int_equal(ny->image_layout, VK_IMAGE_LAYOUT_UNDEFINED); // never used
		auto *nuv = (const gs_vulkan_texture *)texture_get_obj(uv);
		assert_true(nuv->image == ny->image);
		assert_int_equal(nuv->aspect, VK_IMAGE_ASPECT_PLANE_1_BIT);
		assert_int_equal(nuv->width, 32);
		texture_destroy(y);
		texture_destroy(uv);
	}
	assert_null(texture_get_obj(nullptr));
	texture_destroy(t);
	close_device(d);
}

/* ---- timers ---- */

// Clears a big target `n` times between begin and end: real GPU work the timer must see.
static void timed_clears(gs_device *d, gs_texture_t *rt, int n)
{
	set_render_target(d, rt, nullptr);
	vec4 c;
	for (int i = 0; i < n; ++i) {
		vec4_set(&c, (float)i / n, 0, 0, 1);
		clear(d, GS_CLEAR_COLOR, &c, 1.0f, 0);
	}
}

static void test_timer_measures_gpu_work(void **)
{
	gs_device *d = open_device();
	if (!d->timers.valid_bits) {
		destroy(d);
		skip(); // no timestamps on this queue: covered by test_timer_range_reports
	}
	gs_texture_t *rt = texture_create(d, 2048, 2048, GS_RGBA16F, 1, nullptr, GS_RENDER_TARGET);
	assert_non_null(rt);
	gs_timer_range_t *range = timer_range_create(d);
	gs_timer_t *empty = timer_create(d), *busy = timer_create(d);
	assert_non_null(range);
	assert_non_null(empty);
	assert_non_null(busy);

	uint64_t ticks = 0;
	assert_false(timer_get_data(busy, &ticks)); // never begun

	timer_range_begin(range);
	timer_begin(empty);
	timer_end(empty);
	timer_begin(busy);
	timed_clears(d, rt, 64);
	timer_end(busy);
	timer_range_end(range);
	assert_false(timer_get_data(busy, &ticks)); // not submitted yet
	flush(d);

	bool disjoint = true;
	uint64_t freq = 0;
	assert_true(timer_range_get_data(range, &disjoint, &freq));
	assert_false(disjoint);
	assert_true(freq == 1000000000ull);

	uint64_t busy_ns = 0, empty_ns = 0;
	assert_true(timer_get_data(busy, &busy_ns));
	assert_true(timer_get_data(empty, &empty_ns));
	print_message("64 clears of 2048x2048 RGBA16F: %llu ns (empty timer %llu ns)\n", (unsigned long long)busy_ns,
		      (unsigned long long)empty_ns);
	assert_true(busy_ns > 1000);          // 512 MiB of clears take far more than 1 us
	assert_true(busy_ns < 2000000000ull); // and less than 2 s
	assert_true(busy_ns > empty_ns);

	timer_destroy(empty);
	timer_destroy(busy);
	timer_range_destroy(range);
	set_render_target(d, nullptr, nullptr);
	texture_destroy(rt);
	close_device(d);
}

// The source profiler's pattern: one timer per source per frame, read a frame or two later, destroyed after reading,
// for more frames than one query pool holds. Exercises pool growth and recycling.
static void test_timers_per_frame_are_recycled(void **)
{
	gs_device *d = open_device();
	gs_texture_t *rt = texture_create(d, 64, 64, GS_RGBA, 1, nullptr, GS_RENDER_TARGET);
	std::vector<std::vector<gs_timer_t *>> frames;
	const int sources = 40, nframes = 30; // 1200 timers, 2400 queries, more than four pools at once
	int read = 0;
	for (int f = 0; f < nframes; ++f) {
		std::vector<gs_timer_t *> timers;
		for (int s = 0; s < sources; ++s) {
			gs_timer_t *t = timer_create(d);
			timer_begin(t);
			timed_clears(d, rt, 1);
			timer_end(t);
			timers.push_back(t);
		}
		flush(d);
		frames.push_back(timers);
		if (frames.size() > 2) { // processing delayed by two frames
			for (gs_timer_t *t : frames.front()) {
				uint64_t ns;
				read += timer_get_data(t, &ns) ? 1 : 0;
				timer_destroy(t);
			}
			frames.erase(frames.begin());
		}
	}
	for (auto &timers : frames) {
		for (gs_timer_t *t : timers) {
			timer_destroy(t);
		}
	}
	if (d->timers.valid_bits) {
		assert_int_equal(read, sources * (nframes - 2));
		// Recycled: far fewer pools than 2400 queries would need without reuse.
		assert_true(d->timers.pools.size() <= 4);
	} else {
		assert_int_equal(read, 0);
	}
	set_render_target(d, nullptr, nullptr);
	texture_destroy(rt);
	close_device(d);
}

static void test_timer_range_reports(void **)
{
	gs_device *d = open_device();
	gs_timer_range_t *range = timer_range_create(d);
	bool disjoint = false;
	uint64_t freq = 0;
	assert_true(timer_range_get_data(range, &disjoint, &freq)); // even unused: the profiler reads it
	assert_true(freq == 1000000000ull);
	assert_true(disjoint == (d->timers.valid_bits == 0));
	timer_range_destroy(range);
	close_device(d);
}

/* ---- debug markers ---- */

static void test_debug_markers_span_submissions(void **)
{
	gs_device *d = open_device();
	assert_non_null(d->frames.begin_label); // VK_EXT_debug_utils is always there with the validation layer
	gs_texture_t *rt = texture_create(d, 8, 8, GS_RGBA, 1, nullptr, GS_RENDER_TARGET);
	const float red[4] = {1, 0, 0, 1};
	marker_begin(d, "outer", red);
	timed_clears(d, rt, 1);
	marker_begin(d, "inner", nullptr);
	flush(d); // both labels end in this command buffer and begin again in the next
	assert_int_equal(d->frames.labels.size(), 2);
	timed_clears(d, rt, 1);
	marker_end(d);
	flush(d);
	timed_clears(d, rt, 1);
	marker_end(d);
	marker_end(d); // unbalanced: ignored
	assert_true(d->frames.labels.empty());
	flush(d);
	marker_begin(d, "open at destroy", red); // never ended: the submit in destroy closes it
	set_render_target(d, nullptr, nullptr);
	texture_destroy(rt);
	close_device(d);
}

/* ---- cube textures ---- */

static void test_cube_textures_are_unsupported(void **)
{
	gs_device *d = open_device();
	assert_null(cubetexture_create(d, 16, GS_RGBA, 1, nullptr, 0));
	close_device(d);
}

/* ---- HDR ---- */

static void test_surface_format_choice_matches_d3d11(void **)
{
	const VkSurfaceFormatKHR srgb = {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
	const VkSurfaceFormatKHR scrgb = {VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT};
	const VkSurfaceFormatKHR hdr10 = {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT};
	const VkSurfaceFormatKHR all[] = {srgb, hdr10, scrgb};
	enum gs_color_space space;
	VkSurfaceFormatKHR out;

	// SDR 8-bit monitor: the init format, sRGB.
	assert_true(choose_format(all, 3, GS_BGRA, false, 8, &space, &out));
	assert_int_equal(space, GS_CS_SRGB);
	assert_int_equal(out.format, srgb.format);
	assert_int_equal(out.colorSpace, srgb.colorSpace);
	// HDR monitor: scRGB, as D3D11's GS_CS_709_SCRGB with RGBA16F and G10_NONE_P709.
	assert_true(choose_format(all, 3, GS_BGRA, true, 10, &space, &out));
	assert_int_equal(space, GS_CS_709_SCRGB);
	assert_int_equal(out.format, scrgb.format);
	assert_int_equal(out.colorSpace, scrgb.colorSpace);
	// SDR monitor with more than 8 bits: GS_CS_SRGB_16F, the same scRGB swapchain.
	assert_true(choose_format(all, 3, GS_BGRA, false, 10, &space, &out));
	assert_int_equal(space, GS_CS_SRGB_16F);
	assert_int_equal(out.format, scrgb.format);
	// HDR monitor without scRGB on the surface: SDR, never HDR10 (libobs has no PQ output space).
	const VkSurfaceFormatKHR no_scrgb[] = {srgb, hdr10};
	assert_true(choose_format(no_scrgb, 2, GS_BGRA, true, 10, &space, &out));
	assert_int_equal(space, GS_CS_SRGB);
	assert_int_equal(out.format, srgb.format);
	// No usable format at all.
	assert_false(choose_format(&hdr10, 1, GS_BGRA, false, 8, &space, &out));
}

#ifdef _WIN32
static const uint32_t win_size = 64;

static HWND create_window()
{
	static ATOM cls = 0;
	if (!cls) {
		WNDCLASSW wc = {};
		wc.lpfnWndProc = DefWindowProcW;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.lpszClassName = L"obs_vulkan_features_test";
		cls = RegisterClassW(&wc);
	}
	HWND hwnd = CreateWindowExW(0, L"obs_vulkan_features_test", L"", WS_POPUP, 0, 0, win_size, win_size, nullptr,
				    nullptr, GetModuleHandleW(nullptr), nullptr);
	if (!hwnd) {
		skip();
	}
	return hwnd;
}

// The test does not link the loader: through the device's own entry point (gs_vulkan_device).
static std::vector<VkSurfaceFormatKHR> surface_formats(gs_device *d, VkSurfaceKHR surface)
{
	auto n = (const gs_vulkan_device *)get_device_obj(d);
	auto fn = (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)n->get_instance_proc_addr(
		n->instance, "vkGetPhysicalDeviceSurfaceFormatsKHR");
	assert_non_null(fn);
	uint32_t count = 0;
	fn(d->phys, surface, &count, nullptr);
	std::vector<VkSurfaceFormatKHR> f(count);
	fn(d->phys, surface, &count, f.data());
	f.resize(count);
	return f;
}

static void test_monitor_hdr_detection(void **)
{
	gs_device *d = open_device();
	HMONITOR primary = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
	bool hdr = true;
	uint32_t bits = 0;
	assert_true(monitor_info(d, primary, &hdr, &bits));
	print_message("primary monitor: HDR %d, %u bits per colour\n", (int)hdr, bits);
	assert_true(bits >= 6 && bits <= 16);
	assert_true(is_monitor_hdr(d, primary) == hdr);
	d->test_monitor_hdr = true;
	assert_true(is_monitor_hdr(d, primary));
	d->test_monitor_hdr = false;
	assert_false(is_monitor_hdr(d, nullptr));
	close_device(d);
}

// With the HDR hook the swapchain takes scRGB when the surface offers it (NVIDIA lists it on SDR displays too),
// and update_color_space switches back to SDR when the hook goes; without scRGB on the surface it stays SDR.
static void test_hdr_swapchain_follows_the_monitor(void **)
{
	gs_device *d = open_device();
	if (!d->can_present) {
		destroy(d);
		skip();
	}
	HWND hwnd = create_window();
	gs_init_data data = {};
	data.window.hwnd = hwnd;
	data.cx = data.cy = win_size;
	data.format = GS_BGRA;
	data.num_backbuffers = 2;

	d->test_monitor_hdr = true;
	gs_swapchain_t *s = swapchain_create(d, &data);
	assert_non_null(s);
	load_swapchain(d, s);
	bool has_scrgb = false;
	for (const VkSurfaceFormatKHR &sf : surface_formats(d, s->surface)) {
		print_message("  surface format %d, colour space %d\n", (int)sf.format, (int)sf.colorSpace);
		has_scrgb |= sf.format == VK_FORMAT_R16G16B16A16_SFLOAT &&
			     sf.colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;
	}
	has_scrgb = has_scrgb && d->colorspace_ext;
	print_message("scRGB surface format offered: %d\n", (int)has_scrgb);
	if (has_scrgb) {
		assert_int_equal(s->space, GS_CS_709_SCRGB);
		assert_int_equal(s->surface_format.format, VK_FORMAT_R16G16B16A16_SFLOAT);
		assert_int_equal(s->surface_format.colorSpace, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT);
		assert_int_equal(s->target.format, GS_RGBA16F);
		assert_int_equal(get_color_space(d), GS_CS_709_SCRGB);
	} else {
		assert_int_equal(s->space, GS_CS_SRGB);
	}
	// Render a frame into it: clear in scRGB (1.0 = SDR white) and present.
	if (present_ready(d)) {
		vec4 c;
		vec4_set(&c, 2.0f, 1.0f, 0.5f, 1.0f);
		clear(d, GS_CLEAR_COLOR, &c, 1.0f, 0);
		present(d);
	}

	const uint32_t builds = s->builds;
	d->test_monitor_hdr = false;
	update_color_space(d);
	bool real_hdr;
	uint32_t real_bits;
	monitor_info(d, MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &real_hdr, &real_bits);
	if (!real_hdr && real_bits <= 8) {
		assert_int_equal(s->space, GS_CS_SRGB);
		assert_int_equal(s->surface_format.format, VK_FORMAT_B8G8R8A8_UNORM);
		assert_int_equal(get_color_space(d), GS_CS_SRGB);
		assert_true(s->builds == builds + (has_scrgb ? 1 : 0)); // rebuilt only when the space changed
	}
	update_color_space(d);
	assert_true(s->builds <= builds + 1); // unchanged space: no rebuild
	if (present_ready(d)) {
		vec4 c;
		vec4_set(&c, 0, 1, 0, 1);
		clear(d, GS_CLEAR_COLOR, &c, 1.0f, 0);
		present(d);
	}
	swapchain_destroy(s);
	DestroyWindow(hwnd);
	close_device(d);
}
#endif

static void test_fast_clear(void **)
{
	gs_device *d = open_device();
	assert_true(fast_clear(d));
	close_device(d);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_device_obj_has_the_handles),
		cmocka_unit_test(test_texture_obj_has_the_handles),
		cmocka_unit_test(test_timer_measures_gpu_work),
		cmocka_unit_test(test_timers_per_frame_are_recycled),
		cmocka_unit_test(test_timer_range_reports),
		cmocka_unit_test(test_debug_markers_span_submissions),
		cmocka_unit_test(test_cube_textures_are_unsupported),
		cmocka_unit_test(test_surface_format_choice_matches_d3d11),
#ifdef _WIN32
		cmocka_unit_test(test_monitor_hdr_detection),
		cmocka_unit_test(test_hdr_swapchain_follows_the_monitor),
#endif
		cmocka_unit_test(test_fast_clear),
	};
	return cmocka_run_group_tests(tests, setup, teardown);
}
