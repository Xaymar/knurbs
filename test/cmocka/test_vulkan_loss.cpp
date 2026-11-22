// Device loss: VK_ERROR_DEVICE_LOST is simulated with the frame ring's test hook (OBS_VULKAN_TEST_DEVICE_LOST, or
// frames.test_lose_after), the backend rebuilds at the next safe point with D3D11's contract (device_loss_release
// callbacks, then device_loss_rebuild) and every object keeps working: static textures have their data again, draws
// and readbacks work, timers, plane textures, exportable textures, timelines and swapchains are usable. The validation
// layer (with synchronization validation) must report nothing, also about the teardown of the old device.
//
// What a simulation cannot show: a real loss (TDR, driver reset or update) also invalidates the old VkDevice in the
// driver; here the old device is still healthy and is destroyed by the rebuild. The rebuild only relies on what the
// spec allows on a lost device (waits return, objects can be destroyed), which the simulation does not exercise.
#include <stdio.h>
#include <stdlib.h>

#include <string>
#include <vector>

#include <graphics/graphics.h>
#include <graphics/graphics-vulkan.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>
#include <util/bmem.h>
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
static gs_zstencil_t *(*zstencil_create)(gs_device_t *, uint32_t, uint32_t, enum gs_zstencil_format);
static void (*zstencil_destroy)(gs_zstencil_t *);
static void (*set_render_target)(gs_device_t *, gs_texture_t *, gs_zstencil_t *);
static void (*clear)(gs_device_t *, uint32_t, const struct vec4 *, float, uint8_t);
static void (*copy_texture)(gs_device_t *, gs_texture_t *, gs_texture_t *);
static gs_stagesurf_t *(*stagesurface_create)(gs_device_t *, uint32_t, uint32_t, enum gs_color_format);
static void (*stage_texture)(gs_device_t *, gs_stagesurf_t *, gs_texture_t *);
static bool (*stagesurface_map)(gs_stagesurf_t *, uint8_t **, uint32_t *);
static void (*stagesurface_unmap)(gs_stagesurf_t *);
static void (*stagesurface_destroy)(gs_stagesurf_t *);
static void (*flush)(gs_device_t *);
static void (*begin_frame)(gs_device_t *);
static gs_shader_t *(*vertexshader_create)(gs_device_t *, const char *, const char *, char **);
static gs_shader_t *(*pixelshader_create)(gs_device_t *, const char *, const char *, char **);
static gs_sparam_t *(*get_param_by_name)(gs_shader_t *, const char *);
static void (*shader_destroy)(gs_shader_t *);
static void (*set_vec4)(gs_sparam_t *, const struct vec4 *);
static gs_vertbuffer_t *(*vertexbuffer_create)(gs_device_t *, struct gs_vb_data *, uint32_t);
static void (*vertexbuffer_destroy)(gs_vertbuffer_t *);
static void (*load_vertexbuffer)(gs_device_t *, gs_vertbuffer_t *);
static void (*load_vertexshader)(gs_device_t *, gs_shader_t *);
static void (*load_pixelshader)(gs_device_t *, gs_shader_t *);
static void (*draw)(gs_device_t *, enum gs_draw_mode, uint32_t, uint32_t);
static void (*ortho)(gs_device_t *, float, float, float, float, float, float);
static void (*set_viewport)(gs_device_t *, int, int, int, int);
static void (*set_cull_mode)(gs_device_t *, enum gs_cull_mode);
static gs_samplerstate_t *(*samplerstate_create)(gs_device_t *, const struct gs_sampler_info *);
static void (*samplerstate_destroy)(gs_samplerstate_t *);
static gs_timer_t *(*timer_create)(gs_device_t *);
static void (*timer_destroy)(gs_timer_t *);
static void (*timer_begin)(gs_timer_t *);
static void (*timer_end)(gs_timer_t *);
static bool (*timer_get_data)(gs_timer_t *, uint64_t *);
static void (*register_loss)(gs_device_t *, const struct gs_device_loss *);
static void (*unregister_loss)(gs_device_t *, void *);
static void (*marker_begin)(gs_device_t *, const char *, const float[4]);
static void (*marker_end)(gs_device_t *);
static bool (*texture_create_exportable)(gs_device_t *, uint32_t, uint32_t, enum gs_color_format, enum gs_shared_layout,
					 uint32_t, gs_texture_t **, gs_texture_t **);
static bool (*texture_export)(gs_device_t *, gs_texture_t *, enum gs_handle_type, struct gs_shared_texture_desc *);
static gs_timeline_t *(*timeline_create)(gs_device_t *, uint64_t);
static void (*timeline_destroy)(gs_timeline_t *);
static bool (*timeline_signal_cpu)(gs_device_t *, gs_timeline_t *, uint64_t);
static uint64_t (*timeline_get_value)(gs_device_t *, gs_timeline_t *);
static bool (*timeline_signal)(gs_device_t *, gs_timeline_t *, uint64_t);
static gs_swapchain_t *(*swapchain_create)(gs_device_t *, const struct gs_init_data *);
static void (*swapchain_destroy)(gs_swapchain_t *);
static void (*load_swapchain)(gs_device_t *, gs_swapchain_t *);
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
		load(zstencil_create, "device_zstencil_create") & load(zstencil_destroy, "gs_zstencil_destroy") &
		load(set_render_target, "device_set_render_target") & load(clear, "device_clear") &
		load(copy_texture, "device_copy_texture") & load(stagesurface_create, "device_stagesurface_create") &
		load(stage_texture, "device_stage_texture") & load(stagesurface_map, "gs_stagesurface_map") &
		load(stagesurface_unmap, "gs_stagesurface_unmap") &
		load(stagesurface_destroy, "gs_stagesurface_destroy") & load(flush, "device_flush") &
		load(begin_frame, "device_begin_frame") & load(vertexshader_create, "device_vertexshader_create") &
		load(pixelshader_create, "device_pixelshader_create") &
		load(get_param_by_name, "gs_shader_get_param_by_name") & load(shader_destroy, "gs_shader_destroy") &
		load(set_vec4, "gs_shader_set_vec4") & load(vertexbuffer_create, "device_vertexbuffer_create") &
		load(vertexbuffer_destroy, "gs_vertexbuffer_destroy") &
		load(load_vertexbuffer, "device_load_vertexbuffer") &
		load(load_vertexshader, "device_load_vertexshader") &
		load(load_pixelshader, "device_load_pixelshader") & load(draw, "device_draw") &
		load(ortho, "device_ortho") & load(set_viewport, "device_set_viewport") &
		load(set_cull_mode, "device_set_cull_mode") & load(samplerstate_create, "device_samplerstate_create") &
		load(samplerstate_destroy, "gs_samplerstate_destroy") & load(timer_create, "device_timer_create") &
		load(timer_destroy, "gs_timer_destroy") & load(timer_begin, "gs_timer_begin") &
		load(timer_end, "gs_timer_end") & load(timer_get_data, "gs_timer_get_data") &
		load(register_loss, "device_register_loss_callbacks") &
		load(unregister_loss, "device_unregister_loss_callbacks") &
		load(marker_begin, "device_debug_marker_begin") & load(marker_end, "device_debug_marker_end") &
		load(texture_create_exportable, "device_texture_create_exportable") &
		load(texture_export, "device_texture_export") & load(timeline_create, "device_timeline_create") &
		load(timeline_destroy, "gs_timeline_destroy") &
		load(timeline_signal_cpu, "device_timeline_signal_cpu") &
		load(timeline_get_value, "device_timeline_get_value") &
		load(timeline_signal, "device_timeline_signal") & load(swapchain_create, "device_swapchain_create") &
		load(swapchain_destroy, "gs_swapchain_destroy") & load(load_swapchain, "device_load_swapchain") &
		load(present_ready, "device_is_present_ready") & load(present, "device_present");
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

/* ---- loss callbacks ---- */

struct recorder {
	std::string name;
	std::vector<std::string> *log;
	gs_device *d;
	void *rebuild_device = nullptr;
};

static void on_release(void *data)
{
	auto r = (recorder *)data;
	r->log->push_back(r->name + ".release");
}

static void on_rebuild(void *device, void *data)
{
	auto r = (recorder *)data;
	r->log->push_back(r->name + ".rebuild");
	r->rebuild_device = device;
	// The new device is complete by now: its handles are in the struct the callback receives.
	auto n = (const gs_vulkan_device *)device;
	assert_true(n->device == r->d->dev);
	assert_true(n->queue == r->d->graphics.q);
}

/* ---- fixture ---- */

constexpr uint32_t N = 4;

static const char *vs_solid = "uniform float4x4 ViewProj;\n"
			      "struct V { float4 pos : POSITION; };\n"
			      "V main(V v) { V o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); return o; }\n";
static const char *ps_color = "uniform float4 color;\n"
			      "float4 main(float4 pos : POSITION) : TARGET { return color; }\n";

static gs_shader_t *shader(gs_device *d, enum gs_shader_type type, const char *text)
{
	char *errors = nullptr;
	gs_shader_t *s = type == GS_SHADER_VERTEX ? vertexshader_create(d, text, "test", &errors)
						  : pixelshader_create(d, text, "test", &errors);
	if (!s) {
		print_message("%s\n", errors ? errors : "(no message)");
	}
	bfree(errors);
	assert_non_null(s);
	return s;
}

// Full-target quad (triangle strip) in pixel space.
static gs_vertbuffer_t *quad(gs_device *d)
{
	gs_vb_data *vbd = gs_vbdata_create();
	vbd->num = 4;
	vbd->points = (vec3 *)bmalloc(sizeof(vec3) * 4);
	vec3_set(&vbd->points[0], 0, 0, 0);
	vec3_set(&vbd->points[1], (float)N, 0, 0);
	vec3_set(&vbd->points[2], 0, (float)N, 0);
	vec3_set(&vbd->points[3], (float)N, (float)N, 0);
	gs_vertbuffer_t *vb = vertexbuffer_create(d, vbd, 0);
	assert_non_null(vb);
	return vb;
}

// Reads `tex` (N x N RGBA) back through `ss`; false when the map fails.
static bool read_rgba(gs_device *d, gs_stagesurf_t *ss, gs_texture_t *tex, std::vector<uint8_t> &out)
{
	stage_texture(d, ss, tex);
	uint8_t *data;
	uint32_t linesize;
	if (!stagesurface_map(ss, &data, &linesize)) {
		return false;
	}
	out.resize(N * N * 4);
	for (uint32_t y = 0; y < N; ++y) {
		memcpy(out.data() + y * N * 4, data + y * linesize, N * 4);
	}
	stagesurface_unmap(ss);
	return true;
}

// Draws the solid-colour quad into rt (with zs) and checks every pixel.
static void draw_and_check(gs_device *d, gs_texture_t *rt, gs_zstencil_t *zs, gs_shader_t *vs, gs_shader_t *ps,
			   gs_vertbuffer_t *vb, gs_stagesurf_t *ss, const vec4 &color, const uint8_t expect[4])
{
	set_render_target(d, rt, zs);
	set_viewport(d, 0, 0, N, N);
	ortho(d, 0, (float)N, 0, (float)N, -100, 100);
	set_cull_mode(d, GS_NEITHER);
	vec4 black;
	vec4_zero(&black);
	clear(d, GS_CLEAR_COLOR | GS_CLEAR_DEPTH, &black, 1.0f, 0);
	load_vertexshader(d, vs);
	load_pixelshader(d, ps);
	set_vec4(get_param_by_name(ps, "color"), &color);
	load_vertexbuffer(d, vb);
	draw(d, GS_TRISTRIP, 0, 0);
	std::vector<uint8_t> px;
	assert_true(read_rgba(d, ss, rt, px));
	for (uint32_t i = 0; i < N * N; ++i) {
		assert_memory_equal(&px[i * 4], expect, 4);
	}
}

static void test_loss_rebuilds_every_object(void **)
{
	gs_device *d = open_device();
	std::vector<std::string> log;
	recorder a = {"a", &log, d}, b = {"b", &log, d};
	const gs_device_loss cb_a = {on_release, on_rebuild, &a}, cb_b = {on_release, on_rebuild, &b};
	register_loss(d, &cb_a);
	register_loss(d, &cb_b);

	// A static texture with data: restored by the rebuild from the copy it keeps.
	std::vector<uint8_t> pattern(N * N * 4);
	for (size_t i = 0; i < pattern.size(); ++i) {
		pattern[i] = (uint8_t)(i * 7 + 3);
	}
	const uint8_t *levels[1] = {pattern.data()};
	gs_texture_t *image = texture_create(d, N, N, GS_RGBA, 1, levels, 0);
	gs_texture_t *rt = texture_create(d, N, N, GS_RGBA, 1, nullptr, GS_RENDER_TARGET);
	gs_texture_t *copy = texture_create(d, N, N, GS_RGBA, 1, nullptr, GS_RENDER_TARGET);
	gs_zstencil_t *zs = zstencil_create(d, N, N, GS_Z24_S8);
	gs_stagesurf_t *ss = stagesurface_create(d, N, N, GS_RGBA);
	gs_shader_t *vs = shader(d, GS_SHADER_VERTEX, vs_solid), *ps = shader(d, GS_SHADER_PIXEL, ps_color);
	gs_vertbuffer_t *vb = quad(d);
	gs_sampler_info si = {};
	si.filter = GS_FILTER_LINEAR;
	si.max_anisotropy = 1;
	gs_samplerstate_t *sampler = samplerstate_create(d, &si);
	gs_timer_t *timer = timer_create(d);
	gs_texture_t *y = nullptr, *uv = nullptr;
	const bool nv12 = texture_create_nv12(d, &y, &uv, 64, 32, GS_RENDER_TARGET);
	gs_texture_t *exp = nullptr;
	const bool exportable = texture_create_exportable(d, N, N, GS_BGRA, GS_SHARED_SINGLE, 0, &exp, nullptr);
	gs_timeline_t *tl = timeline_create(d, 5);
	assert_non_null(image);
	assert_non_null(rt);
	assert_non_null(zs);
	assert_non_null(ss);
	assert_non_null(sampler);
	assert_non_null(timer);
	assert_non_null(tl);

	vec4 red;
	vec4_set(&red, 1, 0, 0, 1);
	const uint8_t red8[4] = {255, 0, 0, 255};
	draw_and_check(d, rt, zs, vs, ps, vb, ss, red, red8);
	assert_true(timeline_signal_cpu(d, tl, 7));

	// Lose the device on the next submission; a debug marker is open across it.
	const float white[4] = {1, 1, 1, 1};
	marker_begin(d, "across the loss", white);
	timer_begin(timer);
	copy_texture(d, copy, image);
	timer_end(timer);
	d->frames.test_lose_after = 1;
	flush(d);
	assert_true(vk_lost(d));
	std::vector<uint8_t> px;
	assert_false(read_rgba(d, ss, copy, px)); // nothing runs on a lost device
	uint64_t ns;
	assert_false(timer_get_data(timer, &ns));
	assert_true(log.empty()); // rebuilt at a safe point, not inside the failing call

	const VkImage old_image = ((const gs_vulkan_texture *)texture_get_obj(image))->image;
	(void)old_image;
	begin_frame(d);
	assert_false(vk_lost(d));
	assert_int_equal(d->rebuilds, 1);
	const std::vector<std::string> order = {"a.release", "b.release", "a.rebuild", "b.rebuild"};
	assert_true(log == order);
	assert_ptr_equal(a.rebuild_device, get_device_obj(d));
	assert_ptr_equal(b.rebuild_device, get_device_obj(d));
	assert_int_equal(d->frames.labels.size(), 1); // still open on the new device

	// The static texture has its data again.
	copy_texture(d, copy, image);
	assert_true(read_rgba(d, ss, copy, px));
	assert_memory_equal(px.data(), pattern.data(), pattern.size());
	// Shaders, buffers, render target and depth buffer draw again.
	vec4 green;
	vec4_set(&green, 0, 1, 0, 1);
	const uint8_t green8[4] = {0, 255, 0, 255};
	draw_and_check(d, rt, zs, vs, ps, vb, ss, green, green8);
	marker_end(d);
	// The sampler, the timer, the plane textures and the exportable texture have new objects.
	assert_non_null(((gs_sampler_state *)sampler)->sampler);
	if (d->timers.valid_bits) {
		timer_begin(timer);
		clear(d, GS_CLEAR_COLOR, &green, 1.0f, 0);
		timer_end(timer);
		flush(d);
		assert_true(timer_get_data(timer, &ns));
	}
	if (nv12) {
		auto ny = (const gs_vulkan_texture *)texture_get_obj(y);
		auto nuv = (const gs_vulkan_texture *)texture_get_obj(uv);
		assert_non_null(ny->image);
		assert_true(ny->image == nuv->image);
		assert_ptr_equal(y->plane_peer, uv);
		assert_ptr_equal(uv->plane_peer, y);
		set_render_target(d, uv, nullptr);
		vec4 half;
		vec4_set(&half, 0.5f, 0.5f, 0, 0);
		clear(d, GS_CLEAR_COLOR, &half, 1.0f, 0);
		set_render_target(d, nullptr, nullptr);
	}
	if (exportable) {
		assert_int_equal(exp->origin, gs_texture::ORIGIN_EXPORTABLE);
		gs_shared_texture_desc desc;
		assert_true(texture_export(d, exp, GS_HANDLE_WIN32, &desc));
		assert_non_null(desc.win32);
#ifdef _WIN32
		CloseHandle((HANDLE)desc.win32);
#endif
	}
	// The timeline carries on from its last value.
	assert_true(timeline_get_value(d, tl) >= 7);
	assert_true(timeline_signal(d, tl, 9));
	flush(d);

	// Unregistered callbacks are not called; a second loss rebuilds again.
	unregister_loss(d, &b);
	log.clear();
	d->frames.test_lose_after = 1;
	set_render_target(d, rt, nullptr);
	clear(d, GS_CLEAR_COLOR, &green, 1.0f, 0);
	flush(d);
	assert_true(vk_lost(d));
	begin_frame(d);
	assert_int_equal(d->rebuilds, 2);
	const std::vector<std::string> only_a = {"a.release", "a.rebuild"};
	assert_true(log == only_a);
	draw_and_check(d, rt, zs, vs, ps, vb, ss, red, red8);

	unregister_loss(d, &a);
	set_render_target(d, nullptr, nullptr);
	if (nv12) {
		texture_destroy(y);
		texture_destroy(uv);
	}
	if (exportable) {
		texture_destroy(exp);
	}
	timeline_destroy(tl);
	timer_destroy(timer);
	samplerstate_destroy(sampler);
	vertexbuffer_destroy(vb);
	shader_destroy(vs);
	shader_destroy(ps);
	stagesurface_destroy(ss);
	zstencil_destroy(zs);
	texture_destroy(copy);
	texture_destroy(rt);
	texture_destroy(image);
	destroy(d);
	assert_int_equal(messages(), 0);
}

// A plane view whose Y view was destroyed before the loss is rebuilt alone.
static void test_loss_rebuilds_a_lone_plane_view(void **)
{
	gs_device *d = open_device();
	gs_texture_t *y = nullptr, *uv = nullptr;
	if (!texture_create_nv12(d, &y, &uv, 64, 32, GS_RENDER_TARGET)) {
		destroy(d);
		skip();
	}
	texture_destroy(y);
	d->frames.test_lose_after = 1;
	vec4 c;
	vec4_set(&c, 0.5f, 0.5f, 0, 0);
	set_render_target(d, uv, nullptr);
	clear(d, GS_CLEAR_COLOR, &c, 1.0f, 0);
	flush(d);
	begin_frame(d);
	assert_int_equal(d->rebuilds, 1);
	assert_null(uv->plane_peer);
	assert_non_null(uv->image);
	assert_int_equal(uv->width, 32);
	clear(d, GS_CLEAR_COLOR, &c, 1.0f, 0);
	flush(d);
	set_render_target(d, nullptr, nullptr);
	texture_destroy(uv);
	destroy(d);
	assert_int_equal(messages(), 0);
}

// OBS_VULKAN_TEST_DEVICE_LOST=N loses the device on the N-th submission.
static void test_loss_env_hook(void **)
{
#ifdef _WIN32
	_putenv_s("OBS_VULKAN_TEST_DEVICE_LOST", "2");
#else
	setenv("OBS_VULKAN_TEST_DEVICE_LOST", "2", 1);
#endif
	gs_device *d = open_device();
#ifdef _WIN32
	_putenv_s("OBS_VULKAN_TEST_DEVICE_LOST", "");
#else
	unsetenv("OBS_VULKAN_TEST_DEVICE_LOST");
#endif
	assert_int_equal(d->frames.test_lose_after, 2);
	gs_texture_t *rt = texture_create(d, N, N, GS_RGBA, 1, nullptr, GS_RENDER_TARGET);
	set_render_target(d, rt, nullptr);
	vec4 c;
	vec4_set(&c, 1, 1, 1, 1);
	clear(d, GS_CLEAR_COLOR, &c, 1.0f, 0);
	flush(d);
	assert_false(vk_lost(d));
	clear(d, GS_CLEAR_COLOR, &c, 1.0f, 0);
	flush(d);
	assert_true(vk_lost(d));
	begin_frame(d);
	assert_false(vk_lost(d));
	assert_int_equal(d->rebuilds, 1);
	clear(d, GS_CLEAR_COLOR, &c, 1.0f, 0);
	flush(d);
	assert_false(vk_lost(d)); // one-shot
	set_render_target(d, nullptr, nullptr);
	texture_destroy(rt);
	destroy(d);
	assert_int_equal(messages(), 0);
}

#ifdef _WIN32
// Lost while presenting: rebuilt at the end of device_present (as D3D11's Present path), with a new surface and
// swapchain for the same window, and the next frame presents.
static void test_loss_in_present_rebuilds_the_swapchain(void **)
{
	gs_device *d = open_device();
	if (!d->can_present) {
		destroy(d);
		skip();
	}
	static ATOM cls = 0;
	if (!cls) {
		WNDCLASSW wc = {};
		wc.lpfnWndProc = DefWindowProcW;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.lpszClassName = L"obs_vulkan_loss_test";
		cls = RegisterClassW(&wc);
	}
	HWND hwnd = CreateWindowExW(0, L"obs_vulkan_loss_test", L"", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr,
				    GetModuleHandleW(nullptr), nullptr);
	assert_non_null(hwnd);
	gs_init_data data = {};
	data.window.hwnd = hwnd;
	data.cx = data.cy = 64;
	data.format = GS_BGRA;
	data.num_backbuffers = 2;
	gs_swapchain_t *s = swapchain_create(d, &data);
	assert_non_null(s);
	load_swapchain(d, s);
	vec4 c;
	vec4_set(&c, 0, 0, 1, 1);
	int presented = 0;
	for (int i = 0; i < 20 && !presented; ++i) {
		if (present_ready(d)) {
			clear(d, GS_CLEAR_COLOR, &c, 1.0f, 0);
			present(d);
			++presented;
		} else {
			os_sleep_ms(5);
		}
	}
	assert_int_equal(presented, 1);
	const uint32_t builds = s->builds;
	presented = 0;
	for (int i = 0; i < 20 && !presented; ++i) {
		if (present_ready(d)) {
			clear(d, GS_CLEAR_COLOR, &c, 1.0f, 0);
			d->frames.test_lose_after = 1;
			present(d);
			++presented;
		} else {
			os_sleep_ms(5);
		}
	}
	assert_int_equal(presented, 1);
	assert_int_equal(d->rebuilds, 1);
	assert_false(vk_lost(d));
	assert_true(s->builds > builds);
	assert_ptr_equal(d->swap, s);
	presented = 0;
	for (int i = 0; i < 40 && presented < 2; ++i) {
		if (present_ready(d)) {
			clear(d, GS_CLEAR_COLOR, &c, 1.0f, 0);
			present(d);
			++presented;
		} else {
			os_sleep_ms(5);
		}
	}
	assert_int_equal(presented, 2);
	swapchain_destroy(s);
	DestroyWindow(hwnd);
	destroy(d);
	assert_int_equal(messages(), 0);
}
#endif

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_loss_rebuilds_every_object),
		cmocka_unit_test(test_loss_rebuilds_a_lone_plane_view),
		cmocka_unit_test(test_loss_env_hook),
#ifdef _WIN32
		cmocka_unit_test(test_loss_in_present_rebuilds_the_swapchain),
#endif
	};
	return cmocka_run_group_tests(tests, setup, teardown);
}
