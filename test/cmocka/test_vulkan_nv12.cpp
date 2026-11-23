// NV12/P010 textures in libobs-vulkan: availability, two-plane creation and lifetime, rendering into and sampling
// from the plane views, copies (whole-texture copies take both planes, as D3D11) and staging into ordinary and
// NV12/P010 stage surfaces. Every test runs under the validation layer with synchronization validation and expects
// zero messages; each skips without a Vulkan 1.3 device.
#include <stdio.h>
#include <stdlib.h>

#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/bmem.h>
#include <util/platform.h>

#include "vulkan-subsystem.hpp"

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
static bool (*nv12_available)(gs_device_t *);
static bool (*p010_available)(gs_device_t *);
static bool (*create_nv12)(gs_device_t *, gs_texture_t **, gs_texture_t **, uint32_t, uint32_t, uint32_t);
static bool (*create_p010)(gs_device_t *, gs_texture_t **, gs_texture_t **, uint32_t, uint32_t, uint32_t);
static gs_stagesurf_t *(*stagesurface_create_nv12)(gs_device_t *, uint32_t, uint32_t);
static gs_stagesurf_t *(*stagesurface_create_p010)(gs_device_t *, uint32_t, uint32_t);
static gs_texture_t *(*texture_create)(gs_device_t *, uint32_t, uint32_t, enum gs_color_format, uint32_t,
				       const uint8_t **, uint32_t);
static void (*texture_destroy)(gs_texture_t *);
static uint32_t (*texture_width)(const gs_texture_t *);
static uint32_t (*texture_height)(const gs_texture_t *);
static enum gs_color_format (*texture_format)(const gs_texture_t *);
static void (*set_render_target)(gs_device_t *, gs_texture_t *, gs_zstencil_t *);
static void (*clear)(gs_device_t *, uint32_t, const struct vec4 *, float, uint8_t);
static void (*copy_texture)(gs_device_t *, gs_texture_t *, gs_texture_t *);
static void (*copy_texture_region)(gs_device_t *, gs_texture_t *, uint32_t, uint32_t, gs_texture_t *, uint32_t,
				   uint32_t, uint32_t, uint32_t);
static gs_stagesurf_t *(*stagesurface_create)(gs_device_t *, uint32_t, uint32_t, enum gs_color_format);
static enum gs_color_format (*stagesurface_format)(const gs_stagesurf_t *);
static void (*stage_texture)(gs_device_t *, gs_stagesurf_t *, gs_texture_t *);
static bool (*stagesurface_map)(gs_stagesurf_t *, uint8_t **, uint32_t *);
static void (*stagesurface_unmap)(gs_stagesurf_t *);
static void (*stagesurface_destroy)(gs_stagesurf_t *);
static void (*flush)(gs_device_t *);
static gs_shader_t *(*vertexshader_create)(gs_device_t *, const char *, const char *, char **);
static gs_shader_t *(*pixelshader_create)(gs_device_t *, const char *, const char *, char **);
static void (*shader_destroy)(gs_shader_t *);
static gs_sparam_t *(*get_param_by_name)(gs_shader_t *, const char *);
static void (*set_vec4)(gs_sparam_t *, const struct vec4 *);
static void (*set_texture)(gs_sparam_t *, gs_texture_t *);
static gs_vertbuffer_t *(*vertexbuffer_create)(gs_device_t *, struct gs_vb_data *, uint32_t);
static void (*vertexbuffer_destroy)(gs_vertbuffer_t *);
static void (*load_vertexbuffer)(gs_device_t *, gs_vertbuffer_t *);
static void (*load_vertexshader)(gs_device_t *, gs_shader_t *);
static void (*load_pixelshader)(gs_device_t *, gs_shader_t *);
static void (*draw)(gs_device_t *, enum gs_draw_mode, uint32_t, uint32_t);
static void (*ortho)(gs_device_t *, float, float, float, float, float, float);
static void (*set_viewport)(gs_device_t *, int, int, int, int);
static void (*set_cull_mode)(gs_device_t *, enum gs_cull_mode);
static void (*enable_blending)(gs_device_t *, bool);

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
	bool ok = load(create, "device_create") & load(destroy, "device_destroy") &
		  load(messages, "obs_vulkan_validation_messages") & load(nv12_available, "device_nv12_available") &
		  load(p010_available, "device_p010_available") & load(create_nv12, "device_texture_create_nv12") &
		  load(create_p010, "device_texture_create_p010") &
		  load(stagesurface_create_nv12, "device_stagesurface_create_nv12") &
		  load(stagesurface_create_p010, "device_stagesurface_create_p010") &
		  load(texture_create, "device_texture_create") & load(texture_destroy, "gs_texture_destroy") &
		  load(texture_width, "gs_texture_get_width") & load(texture_height, "gs_texture_get_height") &
		  load(texture_format, "gs_texture_get_color_format") &
		  load(set_render_target, "device_set_render_target") & load(clear, "device_clear") &
		  load(copy_texture, "device_copy_texture") & load(copy_texture_region, "device_copy_texture_region") &
		  load(stagesurface_create, "device_stagesurface_create") &
		  load(stagesurface_format, "gs_stagesurface_get_color_format") &
		  load(stage_texture, "device_stage_texture") & load(stagesurface_map, "gs_stagesurface_map") &
		  load(stagesurface_unmap, "gs_stagesurface_unmap") &
		  load(stagesurface_destroy, "gs_stagesurface_destroy") & load(flush, "device_flush") &
		  load(vertexshader_create, "device_vertexshader_create") &
		  load(pixelshader_create, "device_pixelshader_create") & load(shader_destroy, "gs_shader_destroy") &
		  load(get_param_by_name, "gs_shader_get_param_by_name") & load(set_vec4, "gs_shader_set_vec4") &
		  load(set_texture, "gs_shader_set_texture") & load(vertexbuffer_create, "device_vertexbuffer_create") &
		  load(vertexbuffer_destroy, "gs_vertexbuffer_destroy") &
		  load(load_vertexbuffer, "device_load_vertexbuffer") &
		  load(load_vertexshader, "device_load_vertexshader") &
		  load(load_pixelshader, "device_load_pixelshader") & load(draw, "device_draw") &
		  load(ortho, "device_ortho") & load(set_viewport, "device_set_viewport") &
		  load(set_cull_mode, "device_set_cull_mode") & load(enable_blending, "device_enable_blending");
	return ok ? 0 : -1;
}

static int teardown(void **)
{
	os_dlclose(module);
	return 0;
}

// Runs fn on this thread; aborts the process when it takes longer than 5 s (a hang would stall ctest).
template<typename F> static void within_5s(F fn)
{
	std::promise<void> done;
	std::thread watchdog([f = done.get_future()]() mutable {
		if (f.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
			fprintf(stderr, "watchdog: timed out after 5 s\n");
			fflush(stderr);
			_Exit(2);
		}
	});
	fn();
	done.set_value();
	watchdog.join();
}

/* ---- fixture ---- */

constexpr uint32_t W = 16, H = 8; // luma size; chroma is 8x4

struct kind {
	const char *name;
	bool p010;
	enum gs_color_format y, uv;
	uint32_t bpp; // bytes per Y texel
};
static const kind nv12 = {"NV12", false, GS_R8, GS_R8G8, 1};
static const kind p010 = {"P010", true, GS_R16, GS_RG16, 2};

struct fixture {
	gs_device *d = nullptr;
	std::vector<gs_shader_t *> shaders;
	std::vector<gs_vertbuffer_t *> vbs;
};

static fixture *open_fixture(const kind &k)
{
	gs_device_t *device = nullptr;
	const int ret = create(&device, 0);
	if (ret == GS_ERROR_NOT_SUPPORTED) {
		skip();
	}
	assert_int_equal(ret, GS_SUCCESS);
	if (!(k.p010 ? p010_available : nv12_available)(device)) {
		destroy(device);
		print_message("%s textures not available on this device, skipped\n", k.name);
		skip();
	}
	auto f = new fixture;
	f->d = device;
	set_cull_mode(device, GS_NEITHER);
	enable_blending(device, false);
	return f;
}

static void close_fixture(fixture *f)
{
	for (gs_vertbuffer_t *vb : f->vbs) {
		vertexbuffer_destroy(vb);
	}
	for (gs_shader_t *s : f->shaders) {
		shader_destroy(s);
	}
	destroy(f->d);
	delete f;
	const int n = messages();
	if (n < 0) {
		const char *rq = getenv("OBS_VULKAN_REQUIRE_VALIDATION");
		if (rq && !strcmp(rq, "1")) {
			fail_msg("validation layer required (OBS_VULKAN_REQUIRE_VALIDATION=1) but missing");
		}
		print_message("validation layer missing, not checked\n");
	} else {
		assert_int_equal(n, 0);
	}
}

static void create_planes(fixture *f, const kind &k, gs_texture_t *&y, gs_texture_t *&uv, uint32_t flags)
{
	y = uv = nullptr;
	assert_true((k.p010 ? create_p010 : create_nv12)(f->d, &y, &uv, W, H, flags));
	assert_non_null(y);
	assert_non_null(uv);
}

static gs_shader_t *shader(fixture *f, enum gs_shader_type type, const char *text)
{
	char *errors = nullptr;
	gs_shader_t *s = type == GS_SHADER_VERTEX ? vertexshader_create(f->d, text, "test", &errors)
						  : pixelshader_create(f->d, text, "test", &errors);
	if (!s) {
		print_message("%s\n", errors ? errors : "(no message)");
	}
	bfree(errors);
	assert_non_null(s);
	f->shaders.push_back(s);
	return s;
}

// Quad from (x0, y0) to (x1, y1) in pixels as a triangle strip, with uv 0..1.
static gs_vertbuffer_t *quad(fixture *f, float x0, float y0, float x1, float y1)
{
	gs_vb_data *v = gs_vbdata_create();
	v->num = 4;
	v->points = (vec3 *)bzalloc(sizeof(vec3) * 4);
	v->num_tex = 1;
	v->tvarray = (gs_tvertarray *)bzalloc(sizeof(gs_tvertarray));
	v->tvarray[0].width = 2;
	vec2 *uv = (vec2 *)bzalloc(sizeof(vec2) * 4);
	v->tvarray[0].array = uv;
	const float xs[4] = {x0, x1, x0, x1}, ys[4] = {y0, y0, y1, y1};
	for (int i = 0; i < 4; ++i) {
		vec3_set(&v->points[i], xs[i], ys[i], 0.0f);
		vec2_set(&uv[i], i & 1 ? 1.0f : 0.0f, i & 2 ? 1.0f : 0.0f);
	}
	gs_vertbuffer_t *vb = vertexbuffer_create(f->d, v, 0);
	assert_non_null(vb);
	f->vbs.push_back(vb);
	return vb;
}

static const char *vs_text =
	"uniform float4x4 ViewProj;\n"
	"struct V { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
	"V main(V v) { V o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }\n";
static const char *ps_color = "uniform float4 color;\n"
			      "struct V { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
			      "float4 main(V v) : TARGET { return color; }\n";
static const char *ps_sample = "uniform texture2d image;\n"
			       "sampler_state pt { Filter = Point; AddressU = Clamp; AddressV = Clamp; };\n"
			       "struct V { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
			       "float4 main(V v) : TARGET { return image.Sample(pt, v.uv); }\n";

// Renders a quad of `color` covering (x0, y0)..(x1, y1) of `target` (pixel coordinates of that plane).
static void fill(fixture *f, gs_texture_t *target, float x0, float y0, float x1, float y1, float r, float g)
{
	const uint32_t w = texture_width(target), h = texture_height(target);
	set_render_target(f->d, target, nullptr);
	ortho(f->d, 0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
	set_viewport(f->d, 0, 0, (int)w, (int)h);
	gs_shader_t *vs = shader(f, GS_SHADER_VERTEX, vs_text);
	gs_shader_t *ps = shader(f, GS_SHADER_PIXEL, ps_color);
	struct vec4 c;
	vec4_set(&c, r, g, 0.0f, 1.0f);
	set_vec4(get_param_by_name(ps, "color"), &c);
	load_vertexshader(f->d, vs);
	load_pixelshader(f->d, ps);
	load_vertexbuffer(f->d, quad(f, x0, y0, x1, y1));
	draw(f->d, GS_TRISTRIP, 0, 0);
}

static void clear_plane(fixture *f, gs_texture_t *target, float r, float g)
{
	struct vec4 c;
	vec4_set(&c, r, g, 0.0f, 1.0f);
	set_render_target(f->d, target, nullptr);
	clear(f->d, GS_CLEAR_COLOR, &c, 1.0f, 0);
}

// Copies the mapped surface into tightly packed rows of `row` bytes each.
static std::vector<uint8_t> map_rows(gs_stagesurf_t *s, uint32_t row, uint32_t rows, uint32_t *linesize_out = nullptr)
{
	std::vector<uint8_t> out;
	bool mapped = false;
	within_5s([&] {
		uint8_t *data;
		uint32_t linesize;
		mapped = stagesurface_map(s, &data, &linesize);
		if (mapped) {
			for (uint32_t y = 0; y < rows; ++y) {
				out.insert(out.end(), data + (size_t)y * linesize, data + (size_t)y * linesize + row);
			}
			if (linesize_out) {
				*linesize_out = linesize;
			}
			stagesurface_unmap(s);
		}
	});
	assert_true(mapped);
	return out;
}

// One plane read back through an ordinary stage surface of its own format.
static std::vector<uint8_t> read_plane(fixture *f, gs_texture_t *plane)
{
	const uint32_t w = texture_width(plane), h = texture_height(plane);
	const enum gs_color_format fmt = texture_format(plane);
	gs_stagesurf_t *s = stagesurface_create(f->d, w, h, fmt);
	assert_non_null(s);
	stage_texture(f->d, s, plane);
	std::vector<uint8_t> px = map_rows(s, w * gs_get_format_bpp(fmt) / 8, h);
	stagesurface_destroy(s);
	return px;
}

// Component `c` of texel (x, y) in tightly packed plane data, as a unorm value in [0, 1].
static float texel(const std::vector<uint8_t> &px, const kind &k, uint32_t w, uint32_t comps, uint32_t x, uint32_t y,
		   uint32_t c)
{
	const size_t i = ((size_t)y * w + x) * comps + c;
	if (!k.p010) {
		return px[i] / 255.0f;
	}
	uint16_t v;
	memcpy(&v, &px[i * 2], 2);
	return v / 65535.0f;
}

// P010 keeps 10 bits (the high bits of each word): one 10-bit step of tolerance, as one 8-bit step for NV12.
static void expect_unorm(float got, float want, const char *what, uint32_t x, uint32_t y)
{
	const float tol = 1.5f / 255.0f;
	if (got < want - tol || got > want + tol) {
		fail_msg("%s (%u, %u): got %f, expected %f", what, x, y, got, want);
	}
}

// Expected Y plane: `base` everywhere, `fill` in the left half. UV plane: (u0, v0), and (u1, v1) top-left quadrant.
struct pattern {
	float base, fill, u0, v0, u1, v1;
};

static void draw_pattern(fixture *f, gs_texture_t *y, gs_texture_t *uv, const pattern &p)
{
	clear_plane(f, y, p.base, 0.0f);
	clear_plane(f, uv, p.u0, p.v0);
	fill(f, y, 0, 0, W / 2, H, p.fill, 0.0f);
	fill(f, uv, 0, 0, W / 4, H / 4, p.u1, p.v1);
}

static void check_y(const std::vector<uint8_t> &px, const kind &k, const pattern &p)
{
	for (uint32_t y = 0; y < H; ++y) {
		for (uint32_t x = 0; x < W; ++x) {
			expect_unorm(texel(px, k, W, 1, x, y, 0), x < W / 2 ? p.fill : p.base, "Y", x, y);
		}
	}
}

static void check_uv(const std::vector<uint8_t> &px, const kind &k, const pattern &p)
{
	for (uint32_t y = 0; y < H / 2; ++y) {
		for (uint32_t x = 0; x < W / 2; ++x) {
			const bool in = x < W / 4 && y < H / 4;
			expect_unorm(texel(px, k, W / 2, 2, x, y, 0), in ? p.u1 : p.u0, "U", x, y);
			expect_unorm(texel(px, k, W / 2, 2, x, y, 1), in ? p.v1 : p.v0, "V", x, y);
		}
	}
}

// Both planes through an NV12/P010 stage surface: Y rows, then UV rows at the same linesize.
static void check_planar_stage(fixture *f, const kind &k, gs_texture_t *y, const pattern &p)
{
	gs_stagesurf_t *s = (k.p010 ? stagesurface_create_p010 : stagesurface_create_nv12)(f->d, W, H);
	assert_non_null(s);
	assert_int_equal(stagesurface_format(s), GS_UNKNOWN); // as D3D11
	stage_texture(f->d, s, y);
	uint32_t linesize = 0;
	const std::vector<uint8_t> all = map_rows(s, W * k.bpp, H * 3 / 2, &linesize);
	assert_true(linesize >= W * k.bpp);
	check_y(std::vector<uint8_t>(all.begin(), all.begin() + W * k.bpp * H), k, p);
	check_uv(std::vector<uint8_t>(all.begin() + W * k.bpp * H, all.end()), k, p);
	stagesurface_destroy(s);
}

/* ---- tests ---- */

static void test_available(void **)
{
	gs_device_t *device = nullptr;
	const int ret = create(&device, 0);
	if (ret == GS_ERROR_NOT_SUPPORTED) {
		skip();
	}
	assert_int_equal(ret, GS_SUCCESS);
	const bool n = nv12_available(device), p = p010_available(device);
	auto renderer = (const char *(*)(void))os_dlsym(module, "gpu_get_renderer");
	print_message("%s: NV12 %s, P010 %s\n", renderer ? renderer() : "?", n ? "available" : "unavailable",
		      p ? "available" : "unavailable");
	// Both GPUs this backend is tested on (NVIDIA, AMD) support both; a device without them must say so.
	destroy(device);
	assert_true(n);
	assert_true(p);
}

static void create_and_destroy(const kind &k)
{
	fixture *f = open_fixture(k);
	gs_texture_t *y, *uv;
	create_planes(f, k, y, uv, GS_RENDER_TARGET | GS_SHARED_KM_TEX);
	assert_int_equal(texture_width(y), W);
	assert_int_equal(texture_height(y), H);
	assert_int_equal(texture_format(y), k.y);
	assert_int_equal(texture_width(uv), W / 2);
	assert_int_equal(texture_height(uv), H / 2);
	assert_int_equal(texture_format(uv), k.uv);
	assert_true(y->image == uv->image); // one two-plane image, two views
	assert_true(y->rt_view && uv->rt_view && y->view && uv->view);

	// Either order: the image goes with the last view.
	texture_destroy(y);
	assert_null(uv->plane_peer);
	clear_plane(f, uv, 0.5f, 0.5f); // still usable alone
	read_plane(f, uv);
	texture_destroy(uv);

	create_planes(f, k, y, uv, GS_RENDER_TARGET);
	texture_destroy(uv);
	clear_plane(f, y, 0.5f, 0.0f);
	read_plane(f, y);
	texture_destroy(y);

	// Not a render target: sampled and copied only.
	create_planes(f, k, y, uv, 0);
	assert_null(y->rt_view);
	texture_destroy(y);
	texture_destroy(uv);

	// Odd sizes fail (libobs checks them too, before calling in).
	y = uv = nullptr;
	assert_false((k.p010 ? create_p010 : create_nv12)(f->d, &y, &uv, W + 1, H, GS_RENDER_TARGET));
	assert_null(y);
	assert_null(uv);
	close_fixture(f);
}

static void test_create_destroy_nv12(void **)
{
	create_and_destroy(nv12);
}

static void test_create_destroy_p010(void **)
{
	create_and_destroy(p010);
}

// Clear (vkCmdClearAttachments in the plane's pass) and draws into both plane views, read back per plane through
// ordinary R8/RG8 (R16/RG16) stage surfaces and both planes through an NV12/P010 stage surface.
static void render_and_stage(const kind &k)
{
	fixture *f = open_fixture(k);
	gs_texture_t *y, *uv;
	create_planes(f, k, y, uv, GS_RENDER_TARGET);
	const pattern p = {0.25f, 0.75f, 0.5f, 0.125f, 0.875f, 0.375f};
	draw_pattern(f, y, uv, p);
	check_y(read_plane(f, y), k, p);
	check_uv(read_plane(f, uv), k, p);
	check_planar_stage(f, k, y, p);

	// The UV view is not what goes into an NV12 surface (D3D11 rejects its size): nothing is staged.
	gs_stagesurf_t *s = (k.p010 ? stagesurface_create_p010 : stagesurface_create_nv12)(f->d, W, H);
	stage_texture(f->d, s, uv);
	stagesurface_destroy(s);

	// Redraw after reading: the next frame's attachment writes follow the transfer reads.
	const pattern q = {0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f};
	draw_pattern(f, y, uv, q);
	check_planar_stage(f, k, y, q);
	flush(f->d);

	texture_destroy(y);
	texture_destroy(uv);
	close_fixture(f);
}

static void test_render_stage_nv12(void **)
{
	render_and_stage(nv12);
}

static void test_render_stage_p010(void **)
{
	render_and_stage(p010);
}

// The plane views sampled by a draw into an ordinary RGBA16 target (enough precision for P010's 10 bits).
static void sample(const kind &k)
{
	fixture *f = open_fixture(k);
	gs_texture_t *y, *uv;
	create_planes(f, k, y, uv, GS_RENDER_TARGET);
	const pattern p = {0.25f, 0.75f, 0.5f, 0.125f, 0.875f, 0.375f};
	draw_pattern(f, y, uv, p);

	for (gs_texture_t *plane : {y, uv}) {
		const uint32_t w = texture_width(plane), h = texture_height(plane);
		gs_texture_t *rt = texture_create(f->d, w, h, GS_RGBA16, 1, nullptr, GS_RENDER_TARGET);
		assert_non_null(rt);
		set_render_target(f->d, rt, nullptr);
		ortho(f->d, 0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		set_viewport(f->d, 0, 0, (int)w, (int)h);
		gs_shader_t *vs = shader(f, GS_SHADER_VERTEX, vs_text);
		gs_shader_t *ps = shader(f, GS_SHADER_PIXEL, ps_sample);
		set_texture(get_param_by_name(ps, "image"), plane);
		load_vertexshader(f->d, vs);
		load_pixelshader(f->d, ps);
		load_vertexbuffer(f->d, quad(f, 0, 0, (float)w, (float)h));
		draw(f->d, GS_TRISTRIP, 0, 0);

		gs_stagesurf_t *s = stagesurface_create(f->d, w, h, GS_RGBA16);
		stage_texture(f->d, s, rt);
		const std::vector<uint8_t> px = map_rows(s, w * 8, h);
		stagesurface_destroy(s);
		texture_destroy(rt);
		set_texture(get_param_by_name(ps, "image"), nullptr);

		const kind k16 = {"RGBA16", true, GS_RGBA16, GS_RGBA16, 8};
		for (uint32_t ty = 0; ty < h; ++ty) {
			for (uint32_t tx = 0; tx < w; ++tx) {
				if (plane == y) {
					expect_unorm(texel(px, k16, w, 4, tx, ty, 0), tx < W / 2 ? p.fill : p.base,
						     "sampled Y", tx, ty);
				} else {
					const bool in = tx < W / 4 && ty < H / 4;
					expect_unorm(texel(px, k16, w, 4, tx, ty, 0), in ? p.u1 : p.u0, "sampled U", tx,
						     ty);
					expect_unorm(texel(px, k16, w, 4, tx, ty, 1), in ? p.v1 : p.v0, "sampled V", tx,
						     ty);
				}
			}
		}
	}
	texture_destroy(y);
	texture_destroy(uv);
	close_fixture(f);
}

static void test_sample_nv12(void **)
{
	sample(nv12);
}

static void test_sample_p010(void **)
{
	sample(p010);
}

// gs_copy_texture semantics (see device_copy_texture_region): a whole copy between plane views of two images of
// the same format copies both planes, from either view, as D3D11's CopyResource; region copies copy one plane; a
// plane and an ordinary texture of its format copy into each other.
static void copy(const kind &k)
{
	fixture *f = open_fixture(k);
	gs_texture_t *sy, *suv, *dy, *duv;
	create_planes(f, k, sy, suv, GS_RENDER_TARGET);
	create_planes(f, k, dy, duv, GS_RENDER_TARGET);
	const pattern p = {0.25f, 0.75f, 0.5f, 0.125f, 0.875f, 0.375f};
	const pattern zero = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

	// Y view: both planes (what libobs's queue_frame relies on, Windows).
	draw_pattern(f, sy, suv, p);
	draw_pattern(f, dy, duv, zero);
	copy_texture(f->d, dy, sy);
	check_y(read_plane(f, dy), k, p);
	check_uv(read_plane(f, duv), k, p);

	// UV view: both planes as well (the same underlying copy); libobs elsewhere copies Y, then UV.
	draw_pattern(f, dy, duv, zero);
	copy_texture(f->d, duv, suv);
	check_planar_stage(f, k, dy, p);

	// A region copy takes only that plane: the Y plane changes, UV stays.
	draw_pattern(f, dy, duv, zero);
	copy_texture_region(f->d, dy, 0, 0, sy, 0, 0, W, H);
	check_y(read_plane(f, dy), k, p);
	check_uv(read_plane(f, duv), k, zero);

	// Plane <-> ordinary texture of the plane's format (as libobs's separate-texture fallback would hold).
	gs_texture_t *plain_uv = texture_create(f->d, W / 2, H / 2, k.uv, 1, nullptr, GS_RENDER_TARGET);
	assert_non_null(plain_uv);
	copy_texture(f->d, plain_uv, suv);
	check_uv(read_plane(f, plain_uv), k, p);
	draw_pattern(f, dy, duv, zero);
	copy_texture(f->d, duv, plain_uv);
	check_uv(read_plane(f, duv), k, p);
	check_y(read_plane(f, dy), k, zero); // the ordinary texture has one plane only
	texture_destroy(plain_uv);

	// Different kinds or planes do not copy (formats differ); nothing is recorded.
	copy_texture(f->d, duv, sy);

	texture_destroy(sy);
	texture_destroy(suv);
	texture_destroy(dy);
	texture_destroy(duv);
	close_fixture(f);
}

static void test_copy_nv12(void **)
{
	copy(nv12);
}

static void test_copy_p010(void **)
{
	copy(p010);
}

int main()
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_available),           cmocka_unit_test(test_create_destroy_nv12),
		cmocka_unit_test(test_create_destroy_p010), cmocka_unit_test(test_render_stage_nv12),
		cmocka_unit_test(test_render_stage_p010),   cmocka_unit_test(test_sample_nv12),
		cmocka_unit_test(test_sample_p010),         cmocka_unit_test(test_copy_nv12),
		cmocka_unit_test(test_copy_p010),
	};
	return cmocka_run_group_tests(tests, setup, teardown);
}
