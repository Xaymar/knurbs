// External memory and synchronisation in libobs-vulkan (docs/graphics-interop.md), Windows: D3D11 textures imported
// through the legacy exports (KMT and NT handles, keyed mutex hand-off both ways) and gs_texture_import (BGRA; NV12
// where the driver allows it), export and import on a second VkDevice synchronised by a shared timeline, D3D11
// fences as timelines, the pitch-linear NV12/P010 frame buffer, and a child process that receives the handles over a
// pipe (DuplicateHandle), writes the texture and signals back.
//
// Every test runs under the validation layer with synchronization validation and expects no messages; each skips
// without a Vulkan 1.3 device. Pick the GPU with VK_LOADER_DRIVERS_SELECT (e.g. *nv-vk64* or *amd-vulkan64*).
#include <stdio.h>
#include <stdlib.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_4.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <graphics/graphics.h>
#include <graphics/vec4.h>
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
// interop
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
// legacy
static bool (*shared_available)(void);
static gs_texture_t *(*open_shared)(gs_device_t *, uint32_t);
static gs_texture_t *(*open_nt_shared)(gs_device_t *, uint32_t);
static uint32_t (*get_shared_handle)(gs_texture_t *);
static int (*acquire_sync)(gs_texture_t *, uint64_t, uint32_t);
static int (*release_sync)(gs_texture_t *, uint64_t);

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
	       load(shared_available, "device_shared_texture_available") &
	       load(open_shared, "device_texture_open_shared") & load(open_nt_shared, "device_texture_open_nt_shared") &
	       load(get_shared_handle, "device_texture_get_shared_handle") &
	       load(acquire_sync, "device_texture_acquire_sync") & load(release_sync, "device_texture_release_sync");
}

static int setup(void **)
{
	_putenv_s("OBS_VULKAN_VALIDATION", "1");
	module = os_dlopen(OBS_VULKAN_MODULE);
	return module && load_all() ? 0 : -1;
}

static int teardown(void **)
{
	os_dlclose(module);
	return 0;
}

// Runs fn; ends the process when it takes longer than 30 s (a lost hand-off would hang ctest). The watchdog is
// detached and watches a generation number, because a failed cmocka assertion leaves fn by longjmp: a joinable
// std::thread left behind would std::terminate the process and hide the assertion's message.
static std::atomic<int> watch_generation{0};
template<typename F> static void within_30s(F fn)
{
	const int gen = ++watch_generation;
	std::thread([gen] {
		for (int i = 0; i < 600; ++i) {
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			if (watch_generation != gen) {
				return; // finished, or the next test began
			}
		}
		fprintf(stderr, "watchdog: timed out after 30 s\n");
		fflush(stderr);
		_Exit(2);
	}).detach();
	fn();
	++watch_generation;
}

// Validation messages of the devices a test creates: the module counter restarts with every device_create, so a
// test creating a second device banks the first one's count first.
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

static bool is_nvidia(gs_device_t *d)
{
	struct gs_adapter_identity id;
	return get_identity(d, &id) && id.vendor_id == 0x10DE;
}

/* ---- D3D11 producer ----------------------------------------------------------------------------------------- */

struct d3d {
	ID3D11Device *dev = nullptr;
	ID3D11DeviceContext *ctx = nullptr;

	bool create(gs_device_t *vk)
	{
		struct gs_adapter_identity id;
		if (!get_identity(vk, &id) || !id.luid_valid) {
			return false;
		}
		IDXGIFactory4 *f = nullptr;
		if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void **)&f))) {
			return false;
		}
		LUID luid;
		memcpy(&luid, id.luid, sizeof(luid));
		IDXGIAdapter *a = nullptr;
		HRESULT hr = f->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter), (void **)&a);
		f->Release();
		if (FAILED(hr)) {
			return false;
		}
		const D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
		hr = D3D11CreateDevice(a, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, fl, 2, D3D11_SDK_VERSION, &dev, nullptr,
				       &ctx);
		a->Release();
		return SUCCEEDED(hr);
	}

	~d3d()
	{
		if (ctx) {
			ctx->Release();
		}
		if (dev) {
			dev->Release();
		}
	}

	void finish()
	{
		D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
		ID3D11Query *q = nullptr;
		dev->CreateQuery(&qd, &q);
		ctx->End(q);
		ctx->Flush();
		BOOL done = FALSE;
		while (ctx->GetData(q, &done, sizeof(done), 0) != S_OK || !done) {
			Sleep(0);
		}
		q->Release();
	}

	ID3D11Texture2D *texture(uint32_t w, uint32_t h, DXGI_FORMAT fmt, UINT misc, const void *data, UINT pitch)
	{
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = w;
		td.Height = h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = fmt;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (fmt == DXGI_FORMAT_NV12 ? 0 : D3D11_BIND_RENDER_TARGET);
		td.MiscFlags = misc;
		D3D11_SUBRESOURCE_DATA sd = {data, pitch, 0};
		ID3D11Texture2D *t = nullptr;
		assert_true(SUCCEEDED(dev->CreateTexture2D(&td, data ? &sd : nullptr, &t)));
		return t;
	}

	// Copies the texture's bytes (all planes, D3D11's mapping order) out through a staging texture.
	std::vector<uint8_t> read(ID3D11Texture2D *t, uint32_t row_bytes, uint32_t rows)
	{
		D3D11_TEXTURE2D_DESC td;
		t->GetDesc(&td);
		td.Usage = D3D11_USAGE_STAGING;
		td.BindFlags = 0;
		td.MiscFlags = 0;
		td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ID3D11Texture2D *s = nullptr;
		assert_true(SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &s)));
		ctx->CopyResource(s, t);
		D3D11_MAPPED_SUBRESOURCE m;
		assert_true(SUCCEEDED(ctx->Map(s, 0, D3D11_MAP_READ, 0, &m)));
		std::vector<uint8_t> out((size_t)row_bytes * rows);
		for (uint32_t y = 0; y < rows; ++y) {
			memcpy(out.data() + (size_t)y * row_bytes, (const uint8_t *)m.pData + (size_t)y * m.RowPitch,
			       row_bytes);
		}
		ctx->Unmap(s, 0);
		s->Release();
		return out;
	}
};

static HANDLE kmt_handle(ID3D11Texture2D *t)
{
	IDXGIResource *r = nullptr;
	assert_true(SUCCEEDED(t->QueryInterface(__uuidof(IDXGIResource), (void **)&r)));
	HANDLE h = nullptr;
	assert_true(SUCCEEDED(r->GetSharedHandle(&h)));
	r->Release();
	return h;
}

static HANDLE nt_handle(ID3D11Texture2D *t)
{
	IDXGIResource1 *r = nullptr;
	assert_true(SUCCEEDED(t->QueryInterface(__uuidof(IDXGIResource1), (void **)&r)));
	HANDLE h = nullptr;
	assert_true(SUCCEEDED(
		r->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h)));
	r->Release();
	return h;
}

static uint32_t bgra_px(uint32_t x, uint32_t y, int gen)
{
	const uint8_t b = (uint8_t)(x * 2 + gen * 7), g = (uint8_t)(y * 3 + gen * 13),
		      r = (uint8_t)((x ^ y) + gen * 31);
	return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static std::vector<uint32_t> bgra_pattern(uint32_t w, uint32_t h, int gen)
{
	std::vector<uint32_t> p(w * h);
	for (uint32_t y = 0; y < h; ++y) {
		for (uint32_t x = 0; x < w; ++x) {
			p[y * w + x] = bgra_px(x, y, gen);
		}
	}
	return p;
}

// Stages a BGRA texture of this device and returns its texels.
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

/* ---- Tests -------------------------------------------------------------------------------------------------- */

static void test_identity(void **)
{
	gs_device_t *d = device_or_skip();
	struct gs_adapter_identity id;
	assert_true(get_identity(d, &id));
	static const uint8_t zero[16] = {};
	assert_true(memcmp(id.device_uuid, zero, 16) != 0);
	assert_true(id.luid_valid);
	assert_true(shared_available());
	d3d dx;
	assert_true(dx.create(d)); // a DXGI adapter has this LUID
	assert_true(supported(d, GS_HANDLE_WIN32, GS_SHARED_SINGLE, GS_BGRA, false));
	assert_true(supported(d, GS_HANDLE_WIN32, GS_SHARED_NV12, GS_UNKNOWN, false));
	assert_true(supported(d, GS_HANDLE_WIN32_D3D11_KMT, GS_SHARED_SINGLE, GS_BGRA, true));
	assert_true(supported(d, GS_HANDLE_WIN32_D3D11, GS_SHARED_SINGLE, GS_BGRA, true));
	assert_false(
		supported(d, GS_HANDLE_WIN32_D3D11, GS_SHARED_SINGLE, GS_BGRA, false)); // Vulkan cannot export those
	assert_false(supported(d, GS_HANDLE_DMABUF, GS_SHARED_SINGLE, GS_BGRA, true));  // not on Windows
	// The NVIDIA D3D11 NV12 import bug: refused there, offered elsewhere.
	assert_true(supported(d, GS_HANDLE_WIN32_D3D11, GS_SHARED_NV12, GS_UNKNOWN, true) == !is_nvidia(d));
	destroy(d);
	expect_no_messages();
}

// Game capture: a legacy MISC_SHARED texture opened by its KMT handle. Also writes it from Vulkan and reads that back
// in D3D11 (the release to the external owner at the end of each submission).
static void test_d3d11_kmt_bgra(void **)
{
	gs_device_t *d = device_or_skip();
	within_30s([&] {
		d3d dx;
		assert_true(dx.create(d));
		const uint32_t w = 96, h = 64;
		auto init = bgra_pattern(w, h, 0);
		ID3D11Texture2D *t =
			dx.texture(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_RESOURCE_MISC_SHARED, init.data(), w * 4);
		auto upd = bgra_pattern(w, h, 1); // written after creation: later D3D11 writes must show
		dx.ctx->UpdateSubresource(t, 0, nullptr, upd.data(), w * 4, 0);
		dx.finish();
		const uint32_t handle = (uint32_t)(uintptr_t)kmt_handle(t);

		gs_texture_t *tex = open_shared(d, handle);
		assert_non_null(tex);
		assert_int_equal(texture_width(tex), w);
		assert_int_equal(texture_height(tex), h);
		assert_int_equal(texture_format(tex), GS_BGRA);
		assert_int_equal(get_shared_handle(tex), handle);
		assert_int_equal(acquire_sync(tex, 0, 0), -1); // no keyed mutex: as before for current callers
		assert_true(vk_read_bgra(d, tex) == upd);

		vk_clear(d, tex, 1.0f, 0.0f, 0.0f);
		vk_read_bgra(d, tex); // sync point: the clear has finished
		auto back = dx.read(t, w * 4, h);
		assert_true(all_equal(std::vector<uint32_t>((uint32_t *)back.data(), (uint32_t *)back.data() + w * h),
				      red));

		// The same through gs_texture_import with an explicit description.
		struct gs_shared_texture_desc desc = {};
		desc.type = GS_HANDLE_WIN32_D3D11_KMT;
		desc.win32 = (void *)(uintptr_t)handle;
		for (int &fd : desc.fds) {
			fd = -1;
		}
		desc.width = w;
		desc.height = h;
		desc.format = GS_BGRA;
		desc.flags = GS_RENDER_TARGET;
		gs_texture_t *tex2 = nullptr;
		assert_true(texture_import(d, &desc, &tex2, nullptr));
		assert_true(all_equal(vk_read_bgra(d, tex2), red));
		texture_destroy(tex2);
		texture_destroy(tex);
		t->Release();
	});
	destroy(d);
	expect_no_messages();
}

static void test_d3d11_nt_bgra(void **)
{
	gs_device_t *d = device_or_skip();
	within_30s([&] {
		d3d dx;
		assert_true(dx.create(d));
		const uint32_t w = 64, h = 48;
		auto init = bgra_pattern(w, h, 2);
		ID3D11Texture2D *t = dx.texture(w, h, DXGI_FORMAT_B8G8R8A8_UNORM,
						D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE,
						init.data(), w * 4);
		dx.finish();
		HANDLE h_nt = nt_handle(t);
		gs_texture_t *tex = open_nt_shared(d, (uint32_t)(uintptr_t)h_nt);
		assert_non_null(tex);
		assert_int_equal(get_shared_handle(tex), (uint32_t)(uintptr_t)h_nt);
		assert_true(vk_read_bgra(d, tex) == init);

		struct gs_shared_texture_desc desc = {};
		desc.type = GS_HANDLE_WIN32_D3D11;
		desc.win32 = h_nt;
		for (int &fd : desc.fds) {
			fd = -1;
		}
		desc.width = w;
		desc.height = h;
		desc.format = GS_BGRA;
		desc.flags = GS_RENDER_TARGET;
		gs_texture_t *tex2 = nullptr;
		assert_true(texture_import(d, &desc, &tex2, nullptr));
		vk_clear(d, tex2, 0.0f, 0.0f, 1.0f);
		assert_true(all_equal(vk_read_bgra(d, tex), blue)); // the same memory
		texture_destroy(tex2);
		texture_destroy(tex);
		CloseHandle(h_nt); // the import never took the handle
		t->Release();
	});
	destroy(d);
	expect_no_messages();
}

// Keyed mutex both ways: D3D11 writes and releases key 1, Vulkan acquires 1, reads, writes and releases 2, D3D11
// acquires 2 and sees the write. Then a held key times out. For NT (keyed mutex + NTHANDLE) and legacy KMT.
static void keyed_mutex_case(bool nt)
{
	gs_device_t *d = device_or_skip();
	within_30s([&] {
		d3d dx;
		assert_true(dx.create(d));
		const uint32_t w = 64, h = 64;
		const UINT misc = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX |
				  (nt ? D3D11_RESOURCE_MISC_SHARED_NTHANDLE : 0);
		ID3D11Texture2D *t = dx.texture(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, misc, nullptr, 0);
		IDXGIKeyedMutex *km = nullptr;
		assert_true(SUCCEEDED(t->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&km)));
		assert_int_equal(km->AcquireSync(0, 1000), S_OK);
		auto pattern = bgra_pattern(w, h, 3);
		dx.ctx->UpdateSubresource(t, 0, nullptr, pattern.data(), w * 4, 0);
		assert_int_equal(km->ReleaseSync(1), S_OK);
		HANDLE handle = nt ? nt_handle(t) : kmt_handle(t);

		gs_texture_t *tex = nt ? open_nt_shared(d, (uint32_t)(uintptr_t)handle)
				       : open_shared(d, (uint32_t)(uintptr_t)handle);
		assert_non_null(tex);
		assert_int_equal(acquire_sync(tex, 1, GS_WAIT_INFINITE), 0);
		assert_true(vk_read_bgra(d, tex) == pattern);
		vk_clear(d, tex, 0.0f, 1.0f, 0.0f);
		assert_int_equal(release_sync(tex, 2), 0);

		assert_int_equal(km->AcquireSync(2, 5000), S_OK);
		auto back = dx.read(t, w * 4, h);
		assert_true(all_equal(std::vector<uint32_t>((uint32_t *)back.data(), (uint32_t *)back.data() + w * h),
				      green));
		// D3D11 holds the mutex now: an acquire of any key times out.
		assert_int_equal(acquire_sync(tex, 3, 10), ETIMEDOUT);
		assert_int_equal(km->ReleaseSync(3), S_OK);
		assert_int_equal(acquire_sync(tex, 3, 1000), 0);
		vk_clear(d, tex, 1.0f, 0.0f, 0.0f);
		assert_int_equal(release_sync(tex, 4), 0);
		assert_int_equal(km->AcquireSync(4, 5000), S_OK);
		back = dx.read(t, w * 4, h);
		assert_true(all_equal(std::vector<uint32_t>((uint32_t *)back.data(), (uint32_t *)back.data() + w * h),
				      red));
		assert_int_equal(km->ReleaseSync(0), S_OK);

		texture_destroy(tex);
		if (nt) {
			CloseHandle(handle);
		}
		km->Release();
		t->Release();
	});
	destroy(d);
	expect_no_messages();
}

static void test_d3d11_keyed_mutex_nt(void **)
{
	keyed_mutex_case(true);
}

static void test_d3d11_keyed_mutex_kmt(void **)
{
	keyed_mutex_case(false);
}

static uint8_t nv12_y(uint32_t x, uint32_t y)
{
	return (uint8_t)(16 + ((x + y * 2) % 220));
}

static uint8_t nv12_uv(uint32_t x, uint32_t y, int c)
{
	return (uint8_t)(c ? 255 - x * 3 - y : 30 + x * 2 + y * 3);
}

// D3D11 NV12 into Vulkan plane views. On NVIDIA the API refuses it (the driver bug), elsewhere the planes must
// arrive exactly. The legacy single-texture exports refuse NV12 handles everywhere.
static void test_d3d11_nv12(void **)
{
	gs_device_t *d = device_or_skip();
	within_30s([&] {
		d3d dx;
		assert_true(dx.create(d));
		for (uint32_t size : {0u, 1u}) {
			const uint32_t w = size ? 1920 : 128, h = size ? 1080 : 64;
			std::vector<uint8_t> init(w * h * 3 / 2);
			for (uint32_t y = 0; y < h; ++y) {
				for (uint32_t x = 0; x < w; ++x) {
					init[y * w + x] = nv12_y(x, y);
				}
			}
			for (uint32_t y = 0; y < h / 2; ++y) {
				for (uint32_t x = 0; x < w / 2; ++x) {
					init[w * h + y * w + x * 2] = nv12_uv(x, y, 0);
					init[w * h + y * w + x * 2 + 1] = nv12_uv(x, y, 1);
				}
			}
			for (bool nt : {true, false}) {
				const UINT misc = D3D11_RESOURCE_MISC_SHARED |
						  (nt ? D3D11_RESOURCE_MISC_SHARED_NTHANDLE : 0);
				ID3D11Texture2D *t = dx.texture(w, h, DXGI_FORMAT_NV12, misc, init.data(), w);
				dx.finish();
				HANDLE handle = nt ? nt_handle(t) : kmt_handle(t);
				assert_null(nt ? open_nt_shared(d, (uint32_t)(uintptr_t)handle)
					       : open_shared(d, (uint32_t)(uintptr_t)handle));

				struct gs_shared_texture_desc desc = {};
				desc.type = nt ? GS_HANDLE_WIN32_D3D11 : GS_HANDLE_WIN32_D3D11_KMT;
				desc.win32 = handle;
				for (int &fd : desc.fds) {
					fd = -1;
				}
				desc.width = w;
				desc.height = h;
				desc.layout = GS_SHARED_NV12;
				gs_texture_t *y = nullptr, *uv = nullptr;
				const bool ok = texture_import(d, &desc, &y, &uv);
				if (is_nvidia(d)) {
					assert_false(ok);
					assert_null(y);
				} else {
					assert_true(ok);
					assert_int_equal(texture_width(uv), w / 2);
					gs_stagesurf_t *s = stagesurface_create_nv12(d, w, h);
					assert_non_null(s);
					stage_texture(d, s, y);
					uint8_t *data;
					uint32_t linesize;
					assert_true(stagesurface_map(s, &data, &linesize));
					size_t bad = 0;
					for (uint32_t r = 0; r < h * 3 / 2; ++r) {
						bad += memcmp(data + (size_t)r * linesize, &init[(size_t)r * w], w) !=
						       0;
					}
					stagesurface_unmap(s);
					stagesurface_destroy(s);
					if (bad) {
						print_message("%ux%u %s: %zu rows differ\n", w, h, nt ? "NT" : "KMT",
							      bad);
					}
					assert_int_equal(bad, 0);
					texture_destroy(uv);
					texture_destroy(y);
				}
				if (nt) {
					CloseHandle(handle);
				}
				t->Release();
			}
		}
	});
	destroy(d);
	expect_no_messages();
}

// Two R8 / R8G8 patterns copied into the planes of an NV12 texture.
static void fill_nv12(gs_device_t *d, gs_texture_t *y, gs_texture_t *uv, uint32_t w, uint32_t h,
		      std::vector<uint8_t> &expect)
{
	std::vector<uint8_t> py(w * h), puv(w * h / 2);
	for (uint32_t j = 0; j < h; ++j) {
		for (uint32_t i = 0; i < w; ++i) {
			py[j * w + i] = nv12_y(i, j);
		}
	}
	for (uint32_t j = 0; j < h / 2; ++j) {
		for (uint32_t i = 0; i < w / 2; ++i) {
			puv[j * w + i * 2] = nv12_uv(i, j, 0);
			puv[j * w + i * 2 + 1] = nv12_uv(i, j, 1);
		}
	}
	const uint8_t *dy = py.data(), *duv = puv.data();
	gs_texture_t *ty = texture_create(d, w, h, GS_R8, 1, &dy, 0);
	gs_texture_t *tuv = texture_create(d, w / 2, h / 2, GS_R8G8, 1, &duv, 0);
	assert_non_null(ty);
	assert_non_null(tuv);
	copy_texture(d, y, ty);
	copy_texture(d, uv, tuv);
	texture_destroy(ty);
	texture_destroy(tuv);
	expect = py;
	expect.insert(expect.end(), puv.begin(), puv.end());
}

// Export on device A, import on device B, ordered by a shared timeline both ways (BGRA), plus an NV12 texture
// written on A and read on B.
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
		assert_true(texture_export(a, ta, GS_HANDLE_WIN32, &desc));
		assert_int_equal(desc.width, w);
		assert_int_equal(desc.format, texture_format(ta));
		assert_true(desc.size > 0);
		CloseHandle(desc.win32);
		assert_false(texture_export(a, ta, GS_HANDLE_WIN32_D3D11, &desc)); // import-only type
		assert_true(texture_export(a, ta, GS_HANDLE_WIN32, &desc));
		struct gs_shared_handle th;
		assert_true(timeline_export(a, tla, GS_HANDLE_WIN32, &th));

		gs_timeline_t *tlb = timeline_import(b, &th);
		assert_non_null(tlb);
		CloseHandle(th.win32);
		gs_texture_t *tb = nullptr;
		const bool imported = texture_import(b, &desc, &tb, nullptr);
		CloseHandle(desc.win32);
		if (is_nvidia(a)) {
			// Known NVIDIA (621.24) issue: memory exported by this process does not import into it again
			// (VK_ERROR_OUT_OF_DEVICE_MEMORY); test_cross_process covers NVIDIA. Fails here once fixed.
			assert_false(imported);
			print_message("NVIDIA: in-process import refused by the driver (known), timeline part only\n");
			vk_clear(a, ta, 0.0f, 0.0f, 1.0f);
			assert_true(timeline_signal(a, tla, 1));
			assert_true(timeline_wait(b, tlb, 1));
			assert_true(timeline_signal(b, tlb, 2));
			assert_int_equal(timeline_wait_cpu(a, tla, 2, 5000000000ull), 0);
			texture_destroy(ta);
			timeline_destroy(tlb);
			timeline_destroy(tla);
			return;
		}
		assert_true(imported);

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
		assert_true(texture_export(a, ta, GS_HANDLE_WIN32, &bad));
		bad.device_uuid[0] ^= 0xFF;
		gs_texture_t *none = nullptr;
		assert_false(texture_import(b, &bad, &none, nullptr));
		CloseHandle(bad.win32);

		// NV12: A fills both planes, B stages them.
		const uint32_t nw = 256, nh = 128;
		gs_texture_t *ya = nullptr, *uva = nullptr;
		assert_true(create_exportable(a, nw, nh, GS_UNKNOWN, GS_SHARED_NV12, GS_RENDER_TARGET, &ya, &uva));
		std::vector<uint8_t> expect;
		fill_nv12(a, ya, uva, nw, nh, expect);
		assert_true(texture_export(a, uva, GS_HANDLE_WIN32, &desc)); // either plane view exports the image
		assert_int_equal(desc.width, nw);
		assert_int_equal(desc.layout, GS_SHARED_NV12);
		gs_texture_t *yb = nullptr, *uvb = nullptr;
		assert_true(texture_import(b, &desc, &yb, &uvb));
		CloseHandle(desc.win32);
		assert_true(timeline_signal(a, tla, 3));
		assert_true(timeline_wait(b, tlb, 3));
		gs_stagesurf_t *s = stagesurface_create_nv12(b, nw, nh);
		stage_texture(b, s, yb);
		uint8_t *data;
		uint32_t linesize;
		assert_true(stagesurface_map(s, &data, &linesize));
		size_t bad_rows = 0;
		for (uint32_t r = 0; r < nh * 3 / 2; ++r) {
			bad_rows += memcmp(data + (size_t)r * linesize, &expect[(size_t)r * nw], nw) != 0;
		}
		stagesurface_unmap(s);
		stagesurface_destroy(s);
		assert_int_equal(bad_rows, 0);
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
		// A GPU wait on a value the CPU signals later, then a GPU signal after it.
		assert_true(timeline_wait(d, t, 9));
		assert_true(timeline_signal(d, t, 10));
		assert_int_equal(timeline_wait_cpu(d, t, 10, 1000000), ETIMEDOUT);
		assert_true(timeline_signal_cpu(d, t, 9));
		assert_int_equal(timeline_wait_cpu(d, t, 10, 5000000000ull), 0);
		timeline_destroy(t);
	});
	destroy(d);
	expect_no_messages();
}

// A D3D11 fence (ID3D11Fence, shared as a D3D12 fence handle) imported as a timeline, signalled by each API.
static void test_d3d11_fence(void **)
{
	gs_device_t *d = device_or_skip();
	within_30s([&] {
		d3d dx;
		assert_true(dx.create(d));
		ID3D11Device5 *dev5 = nullptr;
		ID3D11DeviceContext4 *ctx4 = nullptr;
		if (FAILED(dx.dev->QueryInterface(__uuidof(ID3D11Device5), (void **)&dev5)) ||
		    FAILED(dx.ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&ctx4))) {
			print_message("no ID3D11Device5, not checked\n");
			return;
		}
		ID3D11Fence *fence = nullptr;
		assert_true(SUCCEEDED(
			dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), (void **)&fence)));
		HANDLE h = nullptr;
		assert_true(SUCCEEDED(fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h)));
		struct gs_shared_handle sh = {GS_HANDLE_WIN32_D3D12_FENCE, h, -1};
		gs_timeline_t *t = timeline_import(d, &sh);
		assert_non_null(t);
		CloseHandle(h);
		assert_true(SUCCEEDED(ctx4->Signal(fence, 3)));
		dx.ctx->Flush();
		assert_int_equal(timeline_wait_cpu(d, t, 3, 5000000000ull), 0);
		assert_true(timeline_signal(d, t, 4));
		HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		assert_true(SUCCEEDED(fence->SetEventOnCompletion(4, ev)));
		assert_int_equal(WaitForSingleObject(ev, 5000), WAIT_OBJECT_0);
		CloseHandle(ev);
		timeline_destroy(t);
		fence->Release();
		ctx4->Release();
		dev5->Release();
	});
	destroy(d);
	expect_no_messages();
}

// The pitch-linear frame buffer: both planes land at the reported offsets and pitches.
static void sharedbuf_case(bool p010)
{
	gs_device_t *d = device_or_skip();
	within_30s([&] {
		const uint32_t w = 200, h = 120; // pitch is padded: 200 (400) bytes -> 256 (512)
		const enum gs_shared_layout layout = p010 ? GS_SHARED_P010 : GS_SHARED_NV12;
		gs_sharedbuf_t *buf = sharedbuf_create(d, w, h, layout);
		assert_non_null(buf);
		struct gs_shared_texture_desc desc;
		assert_true(sharedbuf_export(d, buf, GS_HANDLE_WIN32, &desc));
		assert_non_null(desc.win32);
		assert_int_equal(desc.num_planes, 2);
		assert_int_equal(desc.pitches[0], p010 ? 512 : 256);
		assert_int_equal(desc.offsets[1], desc.pitches[0] * h);
		assert_true(desc.size >= (uint64_t)desc.pitches[0] * h * 3 / 2);
		CloseHandle(desc.win32);

		gs_texture_t *y = nullptr, *uv = nullptr;
		assert_true((p010 ? create_p010 : create_nv12)(d, &y, &uv, w, h, GS_RENDER_TARGET));
		std::vector<uint8_t> expect;
		if (p010) {
			vk_clear(d, y, 0.5f, 0.0f, 0.0f);
			vk_clear(d, uv, 0.25f, 0.75f, 0.0f);
		} else {
			fill_nv12(d, y, uv, w, h, expect);
		}
		assert_true(sharedbuf_copy(d, buf, y));
		assert_false(sharedbuf_copy(d, buf, uv)); // needs the Y plane
		std::vector<uint8_t> out(desc.offsets[1] + (size_t)desc.pitches[1] * h / 2);
		assert_true(read_sharedbuf(buf, out.data(), out.size()));
		size_t bad = 0;
		if (p010) {
			// 10-bit values of the clears (0.5, 0.25, 0.75), within one step of the UNORM16 rounding.
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

// The message the parent writes to the child's stdin: handles already duplicated into the child.
struct child_msg {
	uint64_t texture, timeline, nv12;
	struct gs_shared_texture_desc desc, nv12_desc;
};

static const uint32_t cross_nv12_w = 256, cross_nv12_h = 128;

// The NV12 pattern fill_nv12 writes, Y rows then UV rows.
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

// Child: imports the textures and timeline, waits for 1, checks the parent's red and NV12 pattern, clears green,
// signals 2.
static int child_main()
{
	HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
	child_msg msg;
	DWORD got = 0;
	if (!ReadFile(in, &msg, sizeof(msg), &got, nullptr) || got != sizeof(msg)) {
		fprintf(stderr, "child: no message\n");
		return 10;
	}
	_putenv_s("OBS_VULKAN_VALIDATION", "1");
	module = os_dlopen(OBS_VULKAN_MODULE);
	if (!module || !load_all()) {
		return 11;
	}
	gs_device_t *d = nullptr;
	if (create(&d, 0) != GS_SUCCESS) {
		return 12;
	}
	msg.desc.win32 = (void *)(uintptr_t)msg.texture;
	msg.nv12_desc.win32 = (void *)(uintptr_t)msg.nv12;
	gs_texture_t *tex = nullptr, *y = nullptr, *uv = nullptr;
	struct gs_shared_handle th = {GS_HANDLE_WIN32, (void *)(uintptr_t)msg.timeline, -1};
	gs_timeline_t *t = timeline_import(d, &th);
	if (!texture_import(d, &msg.desc, &tex, nullptr) || !texture_import(d, &msg.nv12_desc, &y, &uv) || !t) {
		return 13;
	}
	CloseHandle((HANDLE)(uintptr_t)msg.texture);
	CloseHandle((HANDLE)(uintptr_t)msg.nv12);
	CloseHandle((HANDLE)(uintptr_t)msg.timeline);
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
		std::vector<uint8_t> expect;
		fill_nv12(d, y, uv, cross_nv12_w, cross_nv12_h, expect);
		assert_true(expect == nv12_expected(cross_nv12_w, cross_nv12_h));
		child_msg msg = {};
		assert_true(texture_export(d, tex, GS_HANDLE_WIN32, &msg.desc));
		assert_true(texture_export(d, y, GS_HANDLE_WIN32, &msg.nv12_desc));
		struct gs_shared_handle th;
		assert_true(timeline_export(d, t, GS_HANDLE_WIN32, &th));

		// Child with a pipe as stdin; it inherits nothing else (the handles go by DuplicateHandle).
		SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
		HANDLE rd, wr;
		assert_true(CreatePipe(&rd, &wr, &sa, 0));
		SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);
		wchar_t exe[MAX_PATH];
		GetModuleFileNameW(nullptr, exe, MAX_PATH);
		std::wstring cmd = L"\"" + std::wstring(exe) + L"\" child";
		STARTUPINFOW si = {sizeof(si)};
		si.dwFlags = STARTF_USESTDHANDLES;
		si.hStdInput = rd;
		si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
		si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
		PROCESS_INFORMATION pi;
		assert_true(CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi));
		CloseHandle(rd);

		HANDLE ct = nullptr, cs = nullptr, cn = nullptr;
		assert_true(DuplicateHandle(GetCurrentProcess(), (HANDLE)msg.desc.win32, pi.hProcess, &ct, 0, FALSE,
					    DUPLICATE_SAME_ACCESS));
		assert_true(DuplicateHandle(GetCurrentProcess(), (HANDLE)msg.nv12_desc.win32, pi.hProcess, &cn, 0,
					    FALSE, DUPLICATE_SAME_ACCESS));
		CloseHandle((HANDLE)msg.nv12_desc.win32);
		msg.nv12 = (uint64_t)(uintptr_t)cn;
		msg.nv12_desc.win32 = nullptr;
		assert_true(DuplicateHandle(GetCurrentProcess(), (HANDLE)th.win32, pi.hProcess, &cs, 0, FALSE,
					    DUPLICATE_SAME_ACCESS));
		CloseHandle((HANDLE)msg.desc.win32);
		CloseHandle((HANDLE)th.win32);
		msg.texture = (uint64_t)(uintptr_t)ct;
		msg.timeline = (uint64_t)(uintptr_t)cs;
		msg.desc.win32 = nullptr;
		DWORD put = 0;
		assert_true(WriteFile(wr, &msg, sizeof(msg), &put, nullptr) && put == sizeof(msg));
		CloseHandle(wr);

		vk_clear(d, tex, 1.0f, 0.0f, 0.0f);
		assert_true(timeline_signal(d, t, 1));
		assert_true(timeline_wait(d, t, 2));
		assert_true(all_equal(vk_read_bgra(d, tex), green));

		assert_int_equal(WaitForSingleObject(pi.hProcess, 20000), WAIT_OBJECT_0);
		DWORD code = 1;
		GetExitCodeProcess(pi.hProcess, &code);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		assert_int_equal(code, 0);
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
		return child_main();
	}
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_identity),
		cmocka_unit_test(test_d3d11_kmt_bgra),
		cmocka_unit_test(test_d3d11_nt_bgra),
		cmocka_unit_test(test_d3d11_keyed_mutex_nt),
		cmocka_unit_test(test_d3d11_keyed_mutex_kmt),
		cmocka_unit_test(test_d3d11_nv12),
		cmocka_unit_test(test_export_import_second_device),
		cmocka_unit_test(test_timeline_cpu),
		cmocka_unit_test(test_d3d11_fence),
		cmocka_unit_test(test_sharedbuf_nv12),
		cmocka_unit_test(test_sharedbuf_p010),
		cmocka_unit_test(test_cross_process),
	};
	return cmocka_run_group_tests(tests, setup, teardown);
}
