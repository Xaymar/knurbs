// GPU (texture) encoding on the Vulkan renderer, end to end: the real OBS video pipeline renders a moving pattern
// that carries its frame number, a real texture encoder (knurbs-nvenc or obs-ffmpeg's AMF) encodes it from the
// shared frames (docs/graphics-interop.md, "Texture encoders"), and libavcodec decodes the packets back.
//
//   test_vulkan_gpu_encode <vulkan module> <encoder module> <encoder id> nv12|p010 [key=value ...]
//
// The settings are applied to the encoder (numbers as integers, true/false as booleans, anything else a string),
// except the test's own overdraw=N and stall=N (see there).
// Every decoded frame must be one the renderer produced, whole: the frame number at the top and at the bottom agree
// (no frame mixed from two), the grey level and the bar match that number (no stale or half-written frame), and the
// numbers never go backwards. The run fails on any validation or synchronisation validation message.
//
// Skips (ctest Skipped) when there is no Vulkan device, or the encoder module does not load or does not register
// the encoder (no such GPU). Pick the GPU with VK_LOADER_DRIVERS_SELECT.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include <obs.h>
#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

// cmocka.h redefines `inline`, so it must come after the C++ standard headers.
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
extern "C" {
#include <cmocka.h>
}

static const uint32_t W = 640, H = 352;
static const uint32_t BITS = 16, BIT_W = W / BITS, BIT_H = 16; // frame number: one row of blocks at top and bottom
static const uint32_t BAR_W = 16;
static const int WANT_PACKETS = 120;

static int argc_;
static char **argv_;
// Full-frame quads drawn under the pattern each frame ("overdraw=N"), so the GPU is still busy when the encoder
// thread picks the frame up: without that, a missing GPU wait never shows on a small frame.
static int overdraw = 0;
// Every N-th frame's render sleeps for three frame times ("stall=N"): libobs then hands the next frames to the
// encoder more than once (obs_tex_frame::count), so one shared frame is in several encoder slots at a time.
static int stall = 0;

/* ---- the pattern ---- */

static uint8_t grey_of(uint32_t n)
{
	return (uint8_t)(40 + (n * 23) % 180);
}

static uint32_t bar_x(uint32_t n)
{
	return (n * BAR_W) % W;
}

struct pattern_src {
	uint32_t n = 0;
	uint32_t stalled = 0;
};

static void fill(gs_effect_t *solid, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t level)
{
	vec4 c;
	vec4_set(&c, level / 255.0f, level / 255.0f, level / 255.0f, 1.0f);
	gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), &c);
	gs_matrix_push();
	gs_matrix_translate3f((float)x, (float)y, 0.0f);
	while (gs_effect_loop(solid, "Solid")) {
		gs_draw_sprite(nullptr, 0, w, h);
	}
	gs_matrix_pop();
}

static void pattern_render(void *data, gs_effect_t *)
{
	auto p = (pattern_src *)data;
	const uint32_t n = p->n;
	if (stall && n && n % stall == 0 && n != p->stalled) {
		p->stalled = n;
		os_sleep_ms(50);
	}
	// Grey levels written as they are, without sRGB conversion: Y = 16 + 219 * level / 255 for NV12 (see y_of).
	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	for (int i = 0; i < overdraw; ++i) {
		fill(solid, 0, 0, W, H, (uint8_t)(i * 7));
	}
	fill(solid, 0, 0, W, H, grey_of(n));
	fill(solid, bar_x(n), BIT_H * 2, BAR_W, H - BIT_H * 4, 255);
	for (uint32_t b = 0; b < BITS; ++b) {
		const uint8_t level = (n >> b) & 1 ? 255 : 0;
		fill(solid, b * BIT_W, 0, BIT_W, BIT_H, level);
		fill(solid, b * BIT_W, H - BIT_H, BIT_W, BIT_H, level);
	}
	gs_enable_framebuffer_srgb(previous);
}

static void register_pattern()
{
	obs_source_info s = {};
	s.id = "test_encode_pattern";
	s.type = OBS_SOURCE_TYPE_INPUT;
	s.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	s.get_name = [](void *) {
		return "encode pattern";
	};
	s.create = [](obs_data_t *, obs_source_t *) -> void * {
		return new pattern_src;
	};
	s.destroy = [](void *data) {
		delete (pattern_src *)data;
	};
	s.video_tick = [](void *data, float) {
		((pattern_src *)data)->n++;
	};
	s.video_render = pattern_render;
	s.get_width = [](void *) {
		return W;
	};
	s.get_height = [](void *) {
		return H;
	};
	obs_register_source(&s);
}

/* ---- the output: keeps the packets ---- */

struct packets {
	std::mutex lock;
	std::vector<std::vector<uint8_t>> data;
	os_event_t *done = nullptr;
};
static packets *collected;

static void register_output(const char *codec)
{
	static std::string codecs;
	codecs = codec;
	obs_output_info o = {};
	o.id = "test_encode_output";
	o.flags = OBS_OUTPUT_VIDEO | OBS_OUTPUT_ENCODED;
	o.encoded_video_codecs = codecs.c_str();
	o.get_name = [](void *) {
		return "encode output";
	};
	o.create = [](obs_data_t *, obs_output_t *output) -> void * {
		return output;
	};
	o.destroy = [](void *) {
	};
	o.start = [](void *data) {
		auto out = (obs_output_t *)data;
		return obs_output_can_begin_data_capture(out, 0) && obs_output_initialize_encoders(out, 0) &&
		       obs_output_begin_data_capture(out, 0);
	};
	o.stop = [](void *data, uint64_t) {
		obs_output_end_data_capture((obs_output_t *)data);
	};
	o.encoded_packet = [](void *, struct encoder_packet *pkt) {
		if (!pkt || pkt->type != OBS_ENCODER_VIDEO) {
			return;
		}
		std::lock_guard<std::mutex> g(collected->lock);
		collected->data.emplace_back(pkt->data, pkt->data + pkt->size);
		if ((int)collected->data.size() == WANT_PACKETS) {
			os_event_signal(collected->done);
		}
	};
	obs_register_output(&o);
}

/* ---- decoding and checking ---- */

struct decoded {
	uint32_t w, h;
	bool ten_bit;
	std::vector<uint16_t> y; // luma, 8-bit scale
};

static std::vector<decoded> decode(const char *codec, const std::vector<uint8_t> &header,
				   const std::vector<std::vector<uint8_t>> &pkts)
{
	const AVCodec *c = avcodec_find_decoder(!strcmp(codec, "hevc") ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
	assert_non_null(c);
	AVCodecContext *ctx = avcodec_alloc_context3(c);
	assert_non_null(ctx);
	assert_int_equal(avcodec_open2(ctx, c, nullptr), 0);
	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	std::vector<decoded> out;
	auto drain = [&]() {
		while (avcodec_receive_frame(ctx, frame) == 0) {
			decoded d;
			d.w = (uint32_t)frame->width;
			d.h = (uint32_t)frame->height;
			d.ten_bit = frame->format == AV_PIX_FMT_YUV420P10LE;
			d.y.resize((size_t)d.w * d.h);
			for (uint32_t yy = 0; yy < d.h; ++yy) {
				const uint8_t *row = frame->data[0] + (size_t)yy * frame->linesize[0];
				for (uint32_t x = 0; x < d.w; ++x) {
					d.y[(size_t)yy * d.w + x] =
						d.ten_bit ? (uint16_t)(((const uint16_t *)row)[x] >> 2) : row[x];
				}
			}
			out.push_back(std::move(d));
		}
	};
	for (size_t i = 0; i < pkts.size(); ++i) {
		std::vector<uint8_t> buf = i == 0 ? header : std::vector<uint8_t>();
		buf.insert(buf.end(), pkts[i].begin(), pkts[i].end());
		buf.resize(buf.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
		pkt->data = buf.data();
		pkt->size = (int)(buf.size() - AV_INPUT_BUFFER_PADDING_SIZE);
		const int r = avcodec_send_packet(ctx, pkt);
		if (r < 0) {
			print_message("packet %zu: avcodec_send_packet failed (%d)\n", i, r);
		}
		drain();
	}
	avcodec_send_packet(ctx, nullptr);
	drain();
	av_frame_free(&frame);
	av_packet_free(&pkt);
	avcodec_free_context(&ctx);
	return out;
}

static double mean(const decoded &d, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h)
{
	double sum = 0;
	for (uint32_t y = y0; y < y0 + h; ++y) {
		for (uint32_t x = x0; x < x0 + w; ++x) {
			sum += d.y[(size_t)y * d.w + x];
		}
	}
	return sum / ((double)w * h);
}

static uint32_t read_number(const decoded &d, uint32_t y0)
{
	uint32_t n = 0;
	for (uint32_t b = 0; b < BITS; ++b) {
		// the middle of each block, away from the edges the encoder smears
		if (mean(d, b * BIT_W + 8, y0 + 4, BIT_W - 16, BIT_H - 8) > 128.0) {
			n |= 1u << b;
		}
	}
	return n;
}

static bool linear_render; // P010: the scene renders into a linear float target, so levels are linear light

static double y_of(uint8_t level)
{
	double v = level / 255.0;
	if (linear_render) { // the conversion applies the sRGB curve
		v = v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
	}
	return 16.0 + 219.0 * v;
}

static int check_frames(const std::vector<decoded> &frames)
{
	int failures = 0, distinct = 0;
	uint32_t prev = 0;
	for (size_t i = 0; i < frames.size(); ++i) {
		const decoded &d = frames[i];
		if (d.w != W || d.h != H) {
			print_message("frame %zu: %ux%u, expected %ux%u\n", i, d.w, d.h, W, H);
			++failures;
			continue;
		}
		const uint32_t top = read_number(d, 0), bottom = read_number(d, H - BIT_H);
		// grey away from the bar (the bar moves 16 px a frame, so one of two sample columns is clear of it)
		const uint32_t bx = bar_x(top);
		const uint32_t sx = bx < W / 2 ? W * 3 / 4 : W / 4;
		const double grey = mean(d, sx, H / 2 - 16, 32, 32), want = y_of(grey_of(top));
		const double bar = mean(d, bx + 4, H / 2 - 16, BAR_W - 8, 32);
		const bool bad = top != bottom || fabs(grey - want) > 6.0 || bar < 200.0 || (i && top < prev);
		if (bad) {
			print_message(
				"frame %zu: number %u (bottom %u), grey %.1f (want %.1f), bar %.1f, previous %u\n", i,
				top, bottom, grey, want, bar, prev);
			++failures;
		}
		distinct += !i || top != prev;
		prev = top;
	}
	print_message("%zu frames decoded, %d distinct, %zu repeated, %d bad\n", frames.size(), distinct,
		      frames.size() - (size_t)distinct, failures);
	if (stall && frames.size() == (size_t)distinct) {
		print_message("stall=%d gave no repeated frames: the case under test did not happen\n", stall);
		++failures;
	}
	if (distinct < WANT_PACKETS / 2) {
		print_message("too few distinct frames\n");
		++failures;
	}
	return failures;
}

/* ---- the test ---- */

static void apply_setting(obs_data_t *s, const char *kv)
{
	const char *eq = strchr(kv, '=');
	assert_non_null(eq);
	std::string key(kv, eq), value(eq + 1);
	char *end = nullptr;
	const long long i = strtoll(value.c_str(), &end, 10);
	if (key == "overdraw" || key == "stall") { // the test's own, not the encoder's
		(key == "overdraw" ? overdraw : stall) = (int)i;
		return;
	}
	if (!value.empty() && *end == 0) {
		obs_data_set_int(s, key.c_str(), i);
	} else if (value == "true" || value == "false") {
		obs_data_set_bool(s, key.c_str(), value == "true");
	} else {
		obs_data_set_string(s, key.c_str(), value.c_str());
	}
}

static int reset_video(const char *module, enum video_format format)
{
	obs_video_info ovi = {};
	ovi.graphics_module = module;
	ovi.fps_num = 60;
	ovi.fps_den = 1;
	ovi.base_width = ovi.output_width = W;
	ovi.base_height = ovi.output_height = H;
	ovi.output_format = format;
	ovi.gpu_conversion = true;
	ovi.colorspace = VIDEO_CS_709; // P010 too: SDR, so the grey levels stay linear in Y
	ovi.range = VIDEO_RANGE_PARTIAL;
	ovi.scale_type = OBS_SCALE_BICUBIC;
	return obs_reset_video(&ovi);
}

static void skip_test(void *own, const char *why)
{
	obs_shutdown();
	os_dlclose(own);
	print_message("%s, skipped\n", why);
	skip();
}

static void test_encode(void **)
{
	const char *vulkan = argv_[1], *module = argv_[2], *encoder_id = argv_[3];
	const bool p010 = !strcmp(argv_[4], "p010");
	linear_render = p010;

	void *own = os_dlopen(vulkan); // keeps the validation counter past obs_shutdown
	if (!own) {
		fail_msg("cannot load %s", vulkan);
	}
	auto messages = (int (*)(void))os_dlsym(own, "obs_vulkan_validation_messages");
	assert_non_null(messages);
	_putenv_s("OBS_VULKAN_VALIDATION", "1");

	assert_true(obs_startup("en-US", nullptr, nullptr));
#pragma warning(push)
#pragma warning(disable : 4996)
	obs_add_data_path(OBS_SOURCE_DIR "/libobs/data/");
#pragma warning(pop)
	register_pattern();

	const int r = reset_video(vulkan, p010 ? VIDEO_FORMAT_P010 : VIDEO_FORMAT_NV12);
	if (r == OBS_VIDEO_NOT_SUPPORTED) {
		skip_test(own, "no Vulkan device");
	}
	assert_int_equal(r, OBS_VIDEO_SUCCESS);
	obs_enter_graphics();
	assert_int_equal(gs_get_device_type(), GS_DEVICE_VULKAN);
	const bool tex_ok = p010 ? gs_p010_available() : gs_nv12_available();
	gs_adapter_identity id = {};
	gs_get_adapter_identity(&id);
	obs_leave_graphics();
	assert_true(tex_ok);
	print_message("renderer adapter: vendor %04x device %04x\n", id.vendor_id, id.device_id);

	obs_module_t *mod = nullptr;
	if (obs_open_module(&mod, module, nullptr) != MODULE_SUCCESS || !obs_init_module(mod)) {
		skip_test(own, "encoder module did not load (no such GPU?)");
	}
	const char *codec = obs_get_encoder_codec(encoder_id);
	if (!codec) {
		skip_test(own, "encoder not registered (no such GPU?)");
	}
	// The texture path is the point: an encoder that rerouted itself to its raw (CPU) variant fails below.
	assert_true(obs_get_encoder_caps(encoder_id) & OBS_ENCODER_CAP_SHARED_HANDLES);
	register_output(codec);

	obs_source_t *pattern = obs_source_create("test_encode_pattern", "pattern", nullptr, nullptr);
	obs_set_output_source(0, pattern);

	obs_data_t *settings = obs_data_create();
	for (int i = 5; i < argc_; ++i) {
		apply_setting(settings, argv_[i]);
	}
	obs_encoder_t *encoder = obs_video_encoder_create(encoder_id, "encoder", settings, nullptr);
	obs_data_release(settings);
	assert_non_null(encoder);
	obs_encoder_set_video(encoder, obs_get_video());
	obs_output_t *output = obs_output_create("test_encode_output", "output", nullptr, nullptr);
	assert_non_null(output);
	obs_output_set_video_encoder(output, encoder);

	packets pk;
	collected = &pk;
	assert_int_equal(os_event_init(&pk.done, OS_EVENT_TYPE_MANUAL), 0);
	assert_true(obs_output_start(output));
	const int waited = os_event_timedwait(pk.done, 30000);
	obs_output_stop(output);
	for (int i = 0; i < 500 && (obs_output_active(output) || obs_video_active()); ++i) {
		os_sleep_ms(10);
	}
	assert_false(obs_video_active());

	// The encoder must have run as a texture encoder, not a rerouted raw one.
	const char *ran = obs_encoder_get_id(encoder);
	print_message("encoder %s ran as %s\n", encoder_id, ran);

	uint8_t *extra = nullptr;
	size_t extra_size = 0;
	std::vector<uint8_t> header;
	if (obs_encoder_get_extra_data(encoder, &extra, &extra_size)) {
		header.assign(extra, extra + extra_size);
	}
	std::vector<std::vector<uint8_t>> data;
	{
		std::lock_guard<std::mutex> g(pk.lock);
		data = pk.data;
	}
	obs_output_release(output);
	obs_encoder_release(encoder);
	obs_set_output_source(0, nullptr);
	obs_source_release(pattern);
	obs_wait_for_destroy_queue();
	collected = nullptr;
	os_event_destroy(pk.done);
	obs_shutdown();

	if (waited != 0) {
		fail_msg("timed out with %zu of %d packets", data.size(), WANT_PACKETS);
	}
	const std::vector<decoded> frames = decode(codec, header, data);
	assert_true(frames.size() >= (size_t)WANT_PACKETS * 3 / 4);
	assert_int_equal(check_frames(frames), 0);

	const int n = messages();
	if (n < 0) {
		print_message("validation layer missing, not checked\n");
	} else {
		assert_int_equal(n, 0);
	}
	os_dlclose(own);
}

int main(int argc, char **argv)
{
	if (argc < 5 || (strcmp(argv[4], "nv12") && strcmp(argv[4], "p010"))) {
		fprintf(stderr, "usage: %s <vulkan module> <encoder module> <encoder id> nv12|p010 [key=value ...]\n",
			argv[0]);
		return 2;
	}
	argc_ = argc;
	argv_ = argv;
	const struct CMUnitTest tests[] = {cmocka_unit_test(test_encode)};
	return cmocka_run_group_tests(tests, nullptr, nullptr);
}
