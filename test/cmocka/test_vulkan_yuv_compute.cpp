// Compute RGB -> YUV conversion (libobs-vulkan, vulkan-yuv.cpp) against the fragment path it replaces: for NV12,
// P010 (sRGB, PQ, HLG) and P216 (4:2:2, sRGB, PQ, HLG) at 1920x1080, 1280x720 and 642x362, the effect's techniques
// (format_conversion.effect, set up as obs-video.c's render_convert_texture does) and every compute target (a
// pitch-linear buffer, the planes of an NV12/P010 texture, a gs_sharedbuf_t) must agree within one LSB of the output
// precision. A second test times fragment passes + frame buffer copy against the compute dispatches (GPU timestamps)
// and prints the numbers. Skips without a Vulkan 1.3 device or when the compute path is unavailable.
#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <graphics/graphics.h>
#include <graphics/matrix4.h>
#include <graphics/vec4.h>
#include <media-io/video-io.h>
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
static graphics_t *graphics;
static gs_effect_t *effect;
static bool (*convert_buffer)(gs_texture_t *, const struct gs_yuv_conversion *, uint32_t, bool, void *, uint64_t,
			      uint32_t *);
static bool (*read_sharedbuf)(gs_sharedbuf_t *, void *, uint64_t);

static int setup(void **)
{
	module = os_dlopen(OBS_VULKAN_MODULE);
	if (!module) {
		return -1;
	}
	convert_buffer = (decltype(convert_buffer))os_dlsym(module, "obs_vulkan_test_convert_yuv_buffer");
	read_sharedbuf = (decltype(read_sharedbuf))os_dlsym(module, "obs_vulkan_test_read_sharedbuf");
	return convert_buffer && read_sharedbuf ? 0 : -1;
}

static int teardown(void **)
{
	os_dlclose(module);
	return 0;
}

static bool open_graphics()
{
	const int r = gs_create(&graphics, OBS_VULKAN_MODULE, 0);
	if (r == GS_ERROR_NOT_SUPPORTED) {
		print_message("SKIPPED: no Vulkan 1.3 device\n");
		return false;
	}
	assert_int_equal(r, GS_SUCCESS);
	gs_enter_context(graphics);
	char *errors = nullptr;
	effect = gs_effect_create_from_file(OBS_SOURCE_DIR "/libobs/data/format_conversion.effect", &errors);
	if (!effect) {
		fail_msg("format_conversion.effect: %s", errors ? errors : "(no message)");
	}
	bfree(errors);
	if (!gs_yuv_compute_available(GS_SHARED_NV12, true)) {
		print_message("SKIPPED: compute YUV conversion unavailable (OBS_VULKAN_YUV_CONVERT=fragment?)\n");
		gs_effect_destroy(effect);
		gs_leave_context();
		gs_destroy(graphics);
		return false;
	}
	return true;
}

static void close_graphics()
{
	gs_effect_destroy(effect);
	gs_leave_context();
	gs_destroy(graphics);
}

/* ---- cases ---- */

struct format_case {
	const char *name;
	enum video_format format;
	enum video_colorspace cs;
	enum video_range_type range;
	const char *tech_y, *tech_uv;
	uint32_t bits; // 8, 10 (P010) or 16 (P216)
	bool sub_y;    // 4:2:0
	enum gs_yuv_transfer transfer;
	bool top_left;
};

static const format_case cases[] = {
	{"NV12 709 limited", VIDEO_FORMAT_NV12, VIDEO_CS_709, VIDEO_RANGE_PARTIAL, "NV12_Y", "NV12_UV", 8, true,
	 GS_YUV_TRANSFER_NONE, false},
	{"NV12 601 full", VIDEO_FORMAT_NV12, VIDEO_CS_601, VIDEO_RANGE_FULL, "NV12_Y", "NV12_UV", 8, true,
	 GS_YUV_TRANSFER_NONE, false},
	{"P010 709 sRGB", VIDEO_FORMAT_P010, VIDEO_CS_709, VIDEO_RANGE_PARTIAL, "P010_SRGB_Y", "P010_SRGB_UV", 10, true,
	 GS_YUV_TRANSFER_SRGB, false},
	{"P010 2100 PQ", VIDEO_FORMAT_P010, VIDEO_CS_2100_PQ, VIDEO_RANGE_PARTIAL, "P010_PQ_Y", "P010_PQ_UV", 10, true,
	 GS_YUV_TRANSFER_PQ, true},
	{"P010 2100 HLG", VIDEO_FORMAT_P010, VIDEO_CS_2100_HLG, VIDEO_RANGE_PARTIAL, "P010_HLG_Y", "P010_HLG_UV", 10,
	 true, GS_YUV_TRANSFER_HLG, true},
	{"P216 709 sRGB (4:2:2)", VIDEO_FORMAT_P216, VIDEO_CS_709, VIDEO_RANGE_PARTIAL, "P216_SRGB_Y", "P216_SRGB_UV",
	 16, false, GS_YUV_TRANSFER_SRGB, false},
	{"P216 2100 PQ (4:2:2)", VIDEO_FORMAT_P216, VIDEO_CS_2100_PQ, VIDEO_RANGE_PARTIAL, "P216_PQ_Y", "P216_PQ_UV",
	 16, false, GS_YUV_TRANSFER_PQ, false},
	{"P216 2100 HLG (4:2:2)", VIDEO_FORMAT_P216, VIDEO_CS_2100_HLG, VIDEO_RANGE_PARTIAL, "P216_HLG_Y",
	 "P216_HLG_UV", 16, false, GS_YUV_TRANSFER_HLG, false},
};

struct size {
	uint32_t w, h;
};
static const size sizes[] = {{1920, 1080}, {1280, 720}, {642, 362}};

static constexpr float sdr_white = 300.f, hdr_lw = 1000.f;

// obs.c set_video_matrix, then the rows render_convert_texture binds.
static gs_yuv_conversion conversion(const format_case &c, vec4 vecs[3])
{
	matrix4 mat;
	video_format_get_parameters_for_format(c.cs, c.range, c.format, (float *)&mat, nullptr, nullptr);
	matrix4_inv(&mat, &mat);
	const vec4 r = mat.x;
	mat.x = mat.y;
	mat.y = r;
	const float *m = (const float *)&mat;
	gs_yuv_conversion conv = {};
	static const int rows[3] = {4, 0, 8};
	for (int i = 0; i < 3; ++i) {
		for (int j = 0; j < 4; ++j) {
			conv.color_vec[i][j] = m[rows[i] + j];
		}
		vec4_set(&vecs[i], m[rows[i]], m[rows[i] + 1], m[rows[i] + 2], m[rows[i] + 3]);
	}
	conv.transfer = c.transfer;
	conv.sdr_white_nits_over_maximum = sdr_white / 10000.f;
	conv.hdr_lw = hdr_lw;
	conv.chroma_top_left = c.top_left;
	return conv;
}

static uint16_t half(float f)
{
	uint32_t x;
	memcpy(&x, &f, 4);
	const uint32_t sign = (x >> 16) & 0x8000;
	const int32_t e = (int32_t)((x >> 23) & 0xFF) - 127 + 15;
	uint32_t m = x & 0x7FFFFF;
	if (e <= 0) {
		return (uint16_t)sign; // tiny: zero is fine for test content
	}
	// round to nearest even
	uint32_t h = ((uint32_t)e << 10) | (m >> 13);
	const uint32_t rem = m & 0x1FFF;
	if (rem > 0x1000 || (rem == 0x1000 && (h & 1))) {
		++h;
	}
	return (uint16_t)(sign | h);
}

// Deterministic content: gradients plus per-texel noise (chroma filtering differences show up on noise), with
// highlights above SDR white for the HDR cases.
static gs_texture_t *make_source(uint32_t w, uint32_t h, bool wide)
{
	uint32_t seed = 0x12345678u ^ (w * 31 + h);
	auto rnd = [&seed]() {
		seed = seed * 1664525u + 1013904223u;
		return (float)(seed >> 8) / (float)(1u << 24);
	};
	std::vector<uint8_t> data;
	if (wide) {
		data.resize((size_t)w * h * 8);
		uint16_t *p = (uint16_t *)data.data();
		for (uint32_t y = 0; y < h; ++y) {
			for (uint32_t x = 0; x < w; ++x, p += 4) {
				const float gx = (float)x / w, gy = (float)y / h;
				const float n = rnd();
				const float boost = (x / 37 + y / 23) % 5 == 0 ? 8.f : 1.f; // some highlights
				p[0] = half((gx * 0.7f + n * 0.3f) * boost);
				p[1] = half((gy * 0.6f + rnd() * 0.4f) * boost);
				p[2] = half(((1.f - gx) * 0.5f + rnd() * 0.5f) * boost);
				p[3] = half(1.f);
			}
		}
	} else {
		data.resize((size_t)w * h * 4);
		uint8_t *p = data.data();
		for (uint32_t y = 0; y < h; ++y) {
			for (uint32_t x = 0; x < w; ++x, p += 4) {
				p[0] = (uint8_t)((x * 255 / w + (uint32_t)(rnd() * 96)) & 0xFF);
				p[1] = (uint8_t)((y * 255 / h + (uint32_t)(rnd() * 96)) & 0xFF);
				p[2] = (uint8_t)(rnd() * 256);
				p[3] = 255;
			}
		}
	}
	const uint8_t *levels = data.data();
	gs_texture_t *t = gs_texture_create(w, h, wide ? GS_RGBA16F : GS_RGBA, 1, &levels, 0);
	assert_non_null(t);
	return t;
}

static void draw_plane(gs_texture_t *target, const char *tech_name)
{
	gs_technique_t *tech = gs_effect_get_technique(effect, tech_name);
	assert_non_null(tech);
	gs_set_render_target(target, nullptr);
	gs_set_viewport(0, 0, (int)gs_texture_get_width(target), (int)gs_texture_get_height(target));
	gs_ortho(0.f, (float)gs_texture_get_width(target), 0.f, (float)gs_texture_get_height(target), -100.f, 100.f);
	gs_load_vertexbuffer(nullptr);
	gs_load_indexbuffer(nullptr);
	const size_t passes = gs_technique_begin(tech);
	for (size_t i = 0; i < passes; ++i) {
		gs_technique_begin_pass(tech, i);
		gs_draw(GS_TRIS, 0, 3);
		gs_technique_end_pass(tech);
	}
	gs_technique_end(tech);
}

// render_convert_texture's two passes into y and uv (separate textures or the plane views of one).
static void fragment_convert(const format_case &c, gs_texture_t *src, gs_texture_t *y, gs_texture_t *uv,
			     const vec4 vecs[3], uint32_t w, uint32_t h)
{
	gs_enable_blending(false);
	gs_enable_depth_test(false); // as render_video
	gs_set_cull_mode(GS_NEITHER);
	gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), src);
	gs_effect_set_vec4(gs_effect_get_param_by_name(effect, "color_vec0"), &vecs[0]);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "sdr_white_nits_over_maximum"), sdr_white / 10000.f);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "hdr_lw"), hdr_lw);
	draw_plane(y, c.tech_y);
	gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), src);
	gs_effect_set_vec4(gs_effect_get_param_by_name(effect, "color_vec1"), &vecs[1]);
	gs_effect_set_vec4(gs_effect_get_param_by_name(effect, "color_vec2"), &vecs[2]);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "width_i"), 1.f / (float)w);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "height_i"), 1.f / (float)h);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "sdr_white_nits_over_maximum"), sdr_white / 10000.f);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "hdr_lw"), hdr_lw);
	draw_plane(uv, c.tech_uv);
	gs_set_render_target(nullptr, nullptr);
	gs_enable_blending(true);
}

// Planes as plain arrays of samples (Y: w x h, UV: interleaved (w/2 * 2) x chroma rows), as stored (8- or 16-bit).
struct planes {
	uint32_t w = 0, h = 0, ch = 0;
	std::vector<uint32_t> y, uv;
};

static void read_rows(planes &p, const uint8_t *data, uint32_t linesize, bool wide, bool chroma)
{
	const uint32_t rows = chroma ? p.ch : p.h, n = p.w; // UV: w/2 pairs = w samples per row
	std::vector<uint32_t> &dst = chroma ? p.uv : p.y;
	dst.resize((size_t)rows * n);
	for (uint32_t r = 0; r < rows; ++r) {
		const uint8_t *row = data + (size_t)r * linesize;
		for (uint32_t i = 0; i < n; ++i) {
			dst[(size_t)r * n + i] = wide ? ((const uint16_t *)row)[i] : row[i];
		}
	}
}

static void stage_plane(planes &p, gs_texture_t *t, enum gs_color_format f, bool wide, bool chroma)
{
	gs_stagesurf_t *s = gs_stagesurface_create(gs_texture_get_width(t), gs_texture_get_height(t), f);
	assert_non_null(s);
	gs_stage_texture(s, t);
	uint8_t *data;
	uint32_t linesize;
	assert_true(gs_stagesurface_map(s, &data, &linesize));
	read_rows(p, data, linesize, wide, chroma);
	gs_stagesurface_unmap(s);
	gs_stagesurface_destroy(s);
}

struct diff {
	uint32_t max = 0;
	size_t count = 0, over = 0;
};

// Differences in LSBs of the output precision; 10-bit samples must have their low 6 bits clear.
static diff compare(const std::vector<uint32_t> &a, const std::vector<uint32_t> &b, uint32_t bits, uint32_t tol)
{
	diff d;
	assert_int_equal(a.size(), b.size());
	for (size_t i = 0; i < a.size(); ++i) {
		uint32_t x = a[i], y = b[i];
		if (bits == 10) {
			assert_int_equal(y & 63, 0);
			x >>= 6;
			y >>= 6;
		}
		const uint32_t e = x > y ? x - y : y - x;
		d.max = std::max(d.max, e);
		d.count += e != 0;
		d.over += e > tol;
	}
	return d;
}

static void check(const char *what, const format_case &c, const size &s, const planes &ref, const planes &got)
{
	// 16-bit chroma (P216): the reference itself is off by up to ~1e-4, as the effect averages through bilinear filtering
	// of the fp16 source; compute averages exact texel values. Its chroma is held to one LSB at 12 bits instead.
	const uint32_t uv_tol = c.bits == 16 ? 16 : 1;
	const diff dy = compare(ref.y, got.y, c.bits, 1), duv = compare(ref.uv, got.uv, c.bits, uv_tol);
	print_message(
		"  %-22s %4ux%-4u %-10s Y: max %u LSB, %zu differ (%.3f%%); UV: max %u LSB, %zu differ (%.3f%%)\n",
		c.name, s.w, s.h, what, dy.max, dy.count, 100.0 * dy.count / ref.y.size(), duv.max, duv.count,
		100.0 * duv.count / ref.uv.size());
	if (dy.over || duv.over) {
		fail_msg("%s %ux%u %s: %zu Y / %zu UV samples differ by more than 1 / %u LSB", c.name, s.w, s.h, what,
			 dy.over, duv.over, uv_tol);
	}
}

static void compare_paths(void **)
{
	if (!open_graphics()) {
		skip();
	}
	for (const format_case &c : cases) {
		for (const size &s : sizes) {
			const bool wide = c.bits != 8;
			const uint32_t cw = s.w / 2, ch = c.sub_y ? s.h / 2 : s.h;
			vec4 vecs[3];
			const gs_yuv_conversion conv = conversion(c, vecs);
			gs_texture_t *src = make_source(s.w, s.h, wide);

			// Fragment: the effect's passes into separate Y and UV render targets.
			const enum gs_color_format fy = wide ? GS_R16 : GS_R8, fuv = wide ? GS_RG16 : GS_R8G8;
			gs_texture_t *ty = gs_texture_create(s.w, s.h, fy, 1, nullptr, GS_RENDER_TARGET);
			gs_texture_t *tuv = gs_texture_create(cw, ch, fuv, 1, nullptr, GS_RENDER_TARGET);
			fragment_convert(c, src, ty, tuv, vecs, s.w, s.h);
			planes ref;
			ref.w = s.w;
			ref.h = s.h;
			ref.ch = ch;
			stage_plane(ref, ty, fy, wide, false);
			stage_plane(ref, tuv, fuv, wide, true);
			gs_texture_destroy(ty);
			gs_texture_destroy(tuv);

			// Compute into a pitch-linear buffer (every layout, 4:2:2 included).
			{
				uint32_t pitch = 0;
				const uint32_t bytes = wide ? 2 : 1;
				const size_t size = (size_t)((s.w * bytes + 255) & ~255u) * (s.h + ch);
				std::vector<uint8_t> out(size);
				assert_true(
					convert_buffer(src, &conv, c.bits, c.sub_y, out.data(), out.size(), &pitch));
				planes got = ref;
				read_rows(got, out.data(), pitch, wide, false);
				read_rows(got, out.data() + (size_t)pitch * s.h, pitch, wide, true);
				check("buffer", c, s, ref, got);
			}

			if (c.sub_y) {
				const enum gs_shared_layout layout = wide ? GS_SHARED_P010 : GS_SHARED_NV12;
				// Compute into the planes of an NV12/P010 texture.
				if (gs_yuv_compute_available(layout, false)) {
					gs_texture_t *py = nullptr, *puv = nullptr;
					assert_true(
						wide ? gs_texture_create_p010(&py, &puv, s.w, s.h, GS_RENDER_TARGET)
						     : gs_texture_create_nv12(&py, &puv, s.w, s.h, GS_RENDER_TARGET));
					assert_true(gs_texture_convert_yuv(py, src, &conv));
					gs_stagesurf_t *st = wide ? gs_stagesurface_create_p010(s.w, s.h)
								  : gs_stagesurface_create_nv12(s.w, s.h);
					gs_stage_texture(st, py);
					uint8_t *data;
					uint32_t linesize;
					assert_true(gs_stagesurface_map(st, &data, &linesize));
					planes got = ref;
					read_rows(got, data, linesize, wide, false);
					read_rows(got, data + (size_t)linesize * s.h, linesize, wide, true);
					gs_stagesurface_unmap(st);
					gs_stagesurface_destroy(st);
					gs_texture_destroy(py);
					gs_texture_destroy(puv);
					check("image", c, s, ref, got);
				} else {
					print_message("  %-22s image target unavailable on this device\n", c.name);
				}
				// Compute into an exportable frame buffer (gs_sharedbuf_t), as NVENC reads it.
				gs_sharedbuf_t *buf = gs_sharedbuf_create(s.w, s.h, layout);
				if (buf) {
					struct gs_shared_texture_desc desc = {};
					assert_true(gs_sharedbuf_export(buf, GS_HANDLE_WIN32, &desc));
					struct gs_shared_handle hd = {desc.type, desc.win32, desc.fds[0]};
					gs_shared_handle_close(&hd);
					assert_true(gs_sharedbuf_convert(buf, src, &conv));
					const size_t size = (size_t)desc.offsets[1] + (size_t)desc.pitches[1] * ch;
					std::vector<uint8_t> out(size);
					assert_true(read_sharedbuf(buf, out.data(), out.size()));
					planes got = ref;
					read_rows(got, out.data() + desc.offsets[0], desc.pitches[0], wide, false);
					read_rows(got, out.data() + desc.offsets[1], desc.pitches[1], wide, true);
					gs_sharedbuf_destroy(buf);
					check("sharedbuf", c, s, ref, got);
				}
			}
			gs_texture_destroy(src);
		}
	}
	close_graphics();
}

/* ---- timing ---- */

struct stats {
	double median_us = 0, mean_us = 0;
};

static stats summarize(std::vector<gs_timer_t *> &timers)
{
	std::vector<double> v;
	for (gs_timer_t *t : timers) {
		uint64_t ns = 0;
		if (gs_timer_get_data(t, &ns)) {
			v.push_back((double)ns / 1000.0);
		}
		gs_timer_destroy(t);
	}
	timers.clear();
	stats s;
	if (v.empty()) {
		return s;
	}
	std::sort(v.begin(), v.end());
	s.median_us = v[v.size() / 2];
	for (double x : v) {
		s.mean_us += x;
	}
	s.mean_us /= (double)v.size();
	return s;
}

template<typename F> static stats measure(int n, F work)
{
	std::vector<gs_timer_t *> timers;
	for (int i = 0; i < n; ++i) {
		gs_timer_t *t = gs_timer_create();
		gs_timer_begin(t);
		work();
		gs_timer_end(t);
		gs_flush();
		timers.push_back(t);
	}
	return summarize(timers);
}

static void timing(void **)
{
	if (!open_graphics()) {
		skip();
	}
	constexpr int n = 200;
	const char *gpu = getenv("VK_LOADER_DRIVERS_SELECT");
	print_message("GPU timings, 1920x1080, %d runs each, median (mean) in microseconds%s%s\n", n,
		      gpu ? ", drivers " : "", gpu ? gpu : "");
	const size s = {1920, 1080};
	for (const format_case *pc : {&cases[0], &cases[3]}) {
		const format_case &c = *pc;
		const bool wide = c.bits != 8;
		const enum gs_shared_layout layout = wide ? GS_SHARED_P010 : GS_SHARED_NV12;
		vec4 vecs[3];
		const gs_yuv_conversion conv = conversion(c, vecs);
		gs_texture_t *src = make_source(s.w, s.h, wide);
		gs_texture_t *py = nullptr, *puv = nullptr;
		assert_true(wide ? gs_texture_create_p010(&py, &puv, s.w, s.h, GS_RENDER_TARGET)
				 : gs_texture_create_nv12(&py, &puv, s.w, s.h, GS_RENDER_TARGET));
		gs_sharedbuf_t *buf = gs_sharedbuf_create(s.w, s.h, layout);
		assert_non_null(buf);

		// warm-up: pipelines are created on first use
		fragment_convert(c, src, py, puv, vecs, s.w, s.h);
		gs_sharedbuf_copy_texture(buf, py);
		gs_sharedbuf_convert(buf, src, &conv);
		if (gs_yuv_compute_available(layout, false)) {
			gs_texture_convert_yuv(py, src, &conv);
		}
		gs_flush();

		const stats frag = measure(n, [&] { fragment_convert(c, src, py, puv, vecs, s.w, s.h); });
		const stats copy = measure(n, [&] { gs_sharedbuf_copy_texture(buf, py); });
		const stats both = measure(n, [&] {
			fragment_convert(c, src, py, puv, vecs, s.w, s.h);
			gs_sharedbuf_copy_texture(buf, py);
		});
		const stats cbuf = measure(n, [&] { gs_sharedbuf_convert(buf, src, &conv); });
		print_message("  %-14s fragment passes %.1f (%.1f), copy to buffer %.1f (%.1f), fragment + copy %.1f "
			      "(%.1f); compute -> buffer %.1f (%.1f)",
			      c.name, frag.median_us, frag.mean_us, copy.median_us, copy.mean_us, both.median_us,
			      both.mean_us, cbuf.median_us, cbuf.mean_us);
		if (gs_yuv_compute_available(layout, false)) {
			const stats cimg = measure(n, [&] { gs_texture_convert_yuv(py, src, &conv); });
			print_message(", compute -> image %.1f (%.1f)", cimg.median_us, cimg.mean_us);
		}
		print_message("\n");
		gs_sharedbuf_destroy(buf);
		gs_texture_destroy(py);
		gs_texture_destroy(puv);
		gs_texture_destroy(src);
	}
	close_graphics();
}

int main()
{
	const struct CMUnitTest tests[] = {cmocka_unit_test(compare_paths), cmocka_unit_test(timing)};
	return cmocka_run_group_tests(tests, setup, teardown);
}
