// Windows capture on the Vulkan renderer, end to end: a producer process (this executable, "producer" mode) shows a
// window with a known pattern that alternates between two phases; win-capture captures it through the real OBS video
// pipeline, and the test reads the composited frames back (raw video callback, BGRA) and checks them.
//
//   test_vulkan_win_capture <graphics module> <win-capture module> <win-capture data dir> <case>
//
// The graphics module is libobs-vulkan; libobs-d3d11 runs the same cases on D3D11, as a reference for the existing
// paths (no validation there, and the producer uses the default adapter).
//
// Cases:
//   game_<format>        game capture, shared texture (hook's D3D11 texture opened by Vulkan), format one of
//                        bgra, rgba, rgb10a2, rgba16f (the producer's swap chain format)
//   game_shmem_bgra      game capture, memory capture (compatibility mode)
//   wgc_d3d11            window capture, Windows Graphics Capture of a D3D11 (BGRA) window
//   wgc_gdi              window capture, Windows Graphics Capture of a GDI window
//   bitblt_gdi           window capture, BitBlt of a GDI window
//
// The data dir holds the hook binaries (graphics-hook64.dll, inject-helper64.exe, get-graphics-offsets64.exe); point
// it at an OBS Studio installation's win-capture data dir to test against the official, signed hook.
//
// Every frame read back after the first captured one must show one of the two phases (8-bit and 10-bit: exact colours
// within a small tolerance, so a missing or doubled sRGB conversion fails; 16-bit float: the dominant channel), both
// phases must show up several times, and there may be no validation or synchronisation validation message. The
// methods log which path they took; the test requires the zero-copy / one-copy one (see check_path).
//
// Skips (ctest Skipped) without a Vulkan device or when Windows Graphics Capture is unavailable.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <tlhelp32.h>

#include <obs.h>
#include <obs-module.h>
#include <util/platform.h>

// cmocka.h redefines `inline`, so it must come after the C++ standard headers.
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
extern "C" {
#include <cmocka.h>
}

static const uint32_t W = 320, H = 240;
static const wchar_t *const CLASS_NAME = L"KnurbsCaptureTest";

/* ---- the pattern: four quadrants, two phases (the colours rotate) ---- */

struct rgb {
	float r, g, b;
};
static const rgb COLOURS[4] = {{0.8f, 0.1f, 0.1f}, {0.1f, 0.7f, 0.2f}, {0.15f, 0.2f, 0.8f}, {0.5f, 0.5f, 0.5f}};

static rgb colour_at(int phase, uint32_t x, uint32_t y)
{
	const int q = (y >= H / 2 ? 2 : 0) + (x >= W / 2 ? 1 : 0);
	return COLOURS[(q + phase) % 4];
}

static int phase_now()
{
	return (int)((GetTickCount64() / 200) % 2);
}

/* ---- producer process ---- */

static uint16_t to_half(float f)
{
	uint32_t x;
	memcpy(&x, &f, 4);
	const uint32_t sign = (x >> 16) & 0x8000;
	const int32_t exp = (int32_t)((x >> 23) & 0xff) - 127 + 15;
	const uint32_t mant = x & 0x7fffff;
	if (exp <= 0) {
		return (uint16_t)sign;
	}
	return (uint16_t)(sign | (exp << 10) | ((mant + 0x1000) >> 13));
}

static std::vector<uint8_t> pattern_pixels(DXGI_FORMAT format, int phase, uint32_t &pitch)
{
	const uint32_t bpp = format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
	pitch = W * bpp;
	std::vector<uint8_t> data((size_t)pitch * H);
	for (uint32_t y = 0; y < H; ++y) {
		for (uint32_t x = 0; x < W; ++x) {
			const rgb c = colour_at(phase, x, y);
			uint8_t *p = data.data() + (size_t)y * pitch + (size_t)x * bpp;
			auto b8 = [](float v) {
				return (uint8_t)lrintf(v * 255.0f);
			};
			switch (format) {
			case DXGI_FORMAT_B8G8R8A8_UNORM:
				p[0] = b8(c.b), p[1] = b8(c.g), p[2] = b8(c.r), p[3] = 255;
				break;
			case DXGI_FORMAT_R8G8B8A8_UNORM:
				p[0] = b8(c.r), p[1] = b8(c.g), p[2] = b8(c.b), p[3] = 255;
				break;
			case DXGI_FORMAT_R10G10B10A2_UNORM: {
				const uint32_t v = (uint32_t)lrintf(c.r * 1023) | ((uint32_t)lrintf(c.g * 1023) << 10) |
						   ((uint32_t)lrintf(c.b * 1023) << 20) | (3u << 30);
				memcpy(p, &v, 4);
				break;
			}
			default: { // scRGB, linear: 1.0 is 80 nits
				const uint16_t h[4] = {to_half(c.r), to_half(c.g), to_half(c.b), to_half(1.0f)};
				memcpy(p, h, 8);
			}
			}
		}
	}
	return data;
}

static int gdi_phase = 0;
static bool gdi_resize = false; // "gdi_resize": the client area alternates between W x H and a larger size
static int gdi_size = 0;

static void set_client_size(HWND wnd, uint32_t w, uint32_t h)
{
	RECT r = {0, 0, (LONG)w, (LONG)h};
	AdjustWindowRect(&r, (DWORD)GetWindowLongW(wnd, GWL_STYLE), FALSE);
	SetWindowPos(wnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
		     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static LRESULT CALLBACK producer_wndproc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(wnd, &ps);
		for (int q = 0; q < 4; ++q) {
			const uint32_t x = (q & 1) ? W / 2 : 0, y = (q & 2) ? H / 2 : 0;
			const rgb c = colour_at(gdi_phase, x, y);
			HBRUSH br = CreateSolidBrush(RGB(lrintf(c.r * 255), lrintf(c.g * 255), lrintf(c.b * 255)));
			RECT r = {(LONG)x, (LONG)y, (LONG)(x + W / 2), (LONG)(y + H / 2)};
			FillRect(dc, &r, br);
			DeleteObject(br);
		}
		EndPaint(wnd, &ps);
		return 0;
	}
	case WM_TIMER:
		if (phase_now() != gdi_phase) {
			gdi_phase = phase_now();
			InvalidateRect(wnd, nullptr, FALSE);
		}
		if (gdi_resize && (int)((GetTickCount64() / 700) % 2) != gdi_size) {
			gdi_size = !gdi_size;
			set_client_size(wnd, gdi_size ? W + 80 : W, gdi_size ? H + 60 : H);
		}
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(wnd, msg, wp, lp);
}

// The producer renders on the renderer's adapter (`luid`, hex), as a game on the same GPU as OBS would.
static int producer_main(const char *mode, const char *title_utf8, const char *luid_hex)
{
	wchar_t title[256];
	MultiByteToWideChar(CP_UTF8, 0, title_utf8, -1, title, 256);
	WNDCLASSW wc = {};
	wc.lpfnWndProc = producer_wndproc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = CLASS_NAME;
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
	RegisterClassW(&wc);
	RECT r = {0, 0, (LONG)W, (LONG)H};
	const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
	AdjustWindowRect(&r, style, FALSE);
	HWND wnd = CreateWindowExW(WS_EX_TOPMOST, CLASS_NAME, title, style, 64, 64, r.right - r.left, r.bottom - r.top,
				   nullptr, nullptr, wc.hInstance, nullptr);
	if (!wnd) {
		return 1;
	}
	ShowWindow(wnd, SW_SHOWNOACTIVATE);
	UpdateWindow(wnd);

	gdi_resize = !strcmp(mode, "gdi_resize");
	if (!strcmp(mode, "gdi") || gdi_resize) {
		SetTimer(wnd, 1, 15, nullptr);
		MSG msg;
		while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		return 0;
	}

	DXGI_FORMAT format = !strcmp(mode, "rgba")      ? DXGI_FORMAT_R8G8B8A8_UNORM
			     : !strcmp(mode, "rgb10a2") ? DXGI_FORMAT_R10G10B10A2_UNORM
			     : !strcmp(mode, "rgba16f") ? DXGI_FORMAT_R16G16B16A16_FLOAT
							: DXGI_FORMAT_B8G8R8A8_UNORM;
	const unsigned long long l = strtoull(luid_hex, nullptr, 16);
	LUID luid = {(DWORD)(l & 0xffffffff), (LONG)(l >> 32)};
	IDXGIFactory4 *f4 = nullptr;
	IDXGIAdapter *want = nullptr; // luid 0: the default adapter
	if (l && (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void **)&f4)) ||
		  FAILED(f4->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter), (void **)&want)))) {
		return 5;
	}
	ID3D11Device *dev = nullptr;
	ID3D11DeviceContext *ctx = nullptr;
	if (FAILED(D3D11CreateDevice(want, want ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
				     D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr,
				     &ctx))) {
		return 2;
	}
	IDXGIDevice *dxgi_dev = nullptr;
	IDXGIAdapter *adapter = nullptr;
	IDXGIFactory2 *factory = nullptr;
	dev->QueryInterface(&dxgi_dev);
	dxgi_dev->GetAdapter(&adapter);
	adapter->GetParent(__uuidof(IDXGIFactory2), (void **)&factory);
	DXGI_SWAP_CHAIN_DESC1 sd = {};
	sd.Width = W;
	sd.Height = H;
	sd.Format = format;
	sd.SampleDesc.Count = 1;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = 2;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	IDXGISwapChain1 *swap = nullptr;
	if (FAILED(factory->CreateSwapChainForHwnd(dev, wnd, &sd, nullptr, nullptr, &swap))) {
		return 3;
	}
	ID3D11Texture2D *phases[2] = {};
	for (int p = 0; p < 2; ++p) {
		uint32_t pitch;
		std::vector<uint8_t> px = pattern_pixels(format, p, pitch);
		D3D11_TEXTURE2D_DESC td = {
			W, H, 1, 1, format, {1, 0}, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0};
		D3D11_SUBRESOURCE_DATA init = {px.data(), pitch, 0};
		if (FAILED(dev->CreateTexture2D(&td, &init, &phases[p]))) {
			return 4;
		}
	}
	for (;;) {
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
			if (msg.message == WM_QUIT) {
				return 0;
			}
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		ID3D11Texture2D *back = nullptr;
		swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back);
		ctx->CopyResource(back, phases[phase_now()]);
		back->Release();
		swap->Present(1, 0);
	}
}

/* ---- the test process ---- */

static int argc_;
static char **argv_;
static std::mutex log_lock;
static std::atomic<bool> rebuilt{false}; // libobs-vulkan rebuilt the device (the "_loss" cases)
static std::string log_text;

static void log_handler(int level, const char *format, va_list args, void *)
{
	char line[4096];
	vsnprintf(line, sizeof(line), format, args);
	if (strstr(line, "[libobs-vulkan] device rebuilt (")) {
		rebuilt = true;
	}
	{
		std::lock_guard<std::mutex> g(log_lock);
		log_text += line;
		log_text += "\n";
	}
	const char *names[] = {"error", "warning", "info", "debug"};
	const int i = level <= LOG_ERROR ? 0 : level <= LOG_WARNING ? 1 : level <= LOG_INFO ? 2 : 3;
	printf("%s: %s\n", names[i], line);
	fflush(stdout);
}

static bool logged(const char *s)
{
	std::lock_guard<std::mutex> g(log_lock);
	return log_text.find(s) != std::string::npos;
}

static int times_logged(const char *s)
{
	std::lock_guard<std::mutex> g(log_lock);
	int n = 0;
	for (size_t at = log_text.find(s); at != std::string::npos; at = log_text.find(s, at + 1)) {
		++n;
	}
	return n;
}

struct frame_stats {
	std::mutex lock;
	int black = 0, phase[2] = {0, 0}, bad = 0, transitions = 0, last = -1;
	int after_rebuild[2] = {0, 0}; // frames of each phase after a device rebuild
	std::string first_bad;
	bool float_format = false;
};
static frame_stats *stats;

static const uint32_t PROBES[4][2] = {{W / 4, H / 4}, {W * 3 / 4, H / 4}, {W / 4, H * 3 / 4}, {W * 3 / 4, H * 3 / 4}};

// Whether a pixel read back (BGRA, sRGB) shows colour c.
static bool matches(const uint8_t *px, const rgb &c, bool float_format)
{
	const int r = px[2], g = px[1], b = px[0];
	if (!float_format) {
		const int tol = 4;
		return abs(r - (int)lrintf(c.r * 255)) <= tol && abs(g - (int)lrintf(c.g * 255)) <= tol &&
		       abs(b - (int)lrintf(c.b * 255)) <= tol;
	}
	// scRGB is scaled to the SDR white level and tonemapped: only the colour's character is checked.
	const bool grey = c.r == c.g && c.g == c.b;
	if (grey) {
		return abs(r - g) <= 6 && abs(g - b) <= 6 && r > 20;
	}
	const int want = c.r > c.g && c.r > c.b ? 0 : c.g > c.b ? 1 : 2;
	const int v[3] = {r, g, b};
	return v[want] > v[(want + 1) % 3] + 20 && v[want] > v[(want + 2) % 3] + 20;
}

static void on_frame(void *, struct video_data *frame)
{
	std::lock_guard<std::mutex> g(stats->lock);
	auto px = [&](uint32_t x, uint32_t y) {
		return frame->data[0] + (size_t)y * frame->linesize[0] + (size_t)x * 4;
	};
	bool black = true;
	for (auto &p : PROBES) {
		const uint8_t *v = px(p[0], p[1]);
		black = black && v[0] < 8 && v[1] < 8 && v[2] < 8;
	}
	if (black && stats->phase[0] + stats->phase[1] == 0) {
		++stats->black; // not captured yet
		return;
	}
	int found = -1;
	for (int phase = 0; phase < 2 && found < 0; ++phase) {
		bool all = true;
		for (auto &p : PROBES) {
			all = all && matches(px(p[0], p[1]), colour_at(phase, p[0], p[1]), stats->float_format);
		}
		if (all) {
			found = phase;
		}
	}
	if (found < 0) {
		if (stats->first_bad.empty()) {
			char buf[256];
			std::string s;
			for (auto &p : PROBES) {
				const uint8_t *v = px(p[0], p[1]);
				snprintf(buf, sizeof(buf), "(%u,%u)=rgb(%d,%d,%d) ", p[0], p[1], v[2], v[1], v[0]);
				s += buf;
			}
			stats->first_bad = s;
		}
		++stats->bad;
		return;
	}
	++stats->phase[found];
	if (rebuilt) {
		++stats->after_rebuild[found];
	}
	if (stats->last >= 0 && stats->last != found) {
		++stats->transitions;
	}
	stats->last = found;
}

static PROCESS_INFORMATION start_producer(const char *mode, const std::string &title, const char *luid, HANDLE job)
{
	char exe[MAX_PATH];
	GetModuleFileNameA(nullptr, exe, MAX_PATH);
	std::string cmd = std::string("\"") + exe + "\" producer " + mode + " \"" + title + "\" " + luid;
	STARTUPINFOA si = {sizeof(si)};
	PROCESS_INFORMATION pi = {};
	std::vector<char> buf(cmd.begin(), cmd.end());
	buf.push_back(0);
	if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &si,
			    &pi)) {
		fail_msg("cannot start the producer (%lu)", GetLastError());
	}
	AssignProcessToJobObject(job, pi.hProcess); // dies with the test
	ResumeThread(pi.hThread);
	return pi;
}

static HWND wait_for_window(const std::string &title)
{
	wchar_t wtitle[256];
	MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, wtitle, 256);
	for (int i = 0; i < 500; ++i) {
		HWND w = FindWindowW(CLASS_NAME, wtitle);
		if (w && IsWindowVisible(w)) {
			return w;
		}
		os_sleep_ms(10);
	}
	return nullptr;
}

// The graphics hook the producer has loaded (empty: none), so the test can show which hook it ran against.
static std::string loaded_hook(DWORD pid)
{
	std::string path;
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
	if (snap == INVALID_HANDLE_VALUE) {
		return path;
	}
	MODULEENTRY32W me = {sizeof(me)};
	for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
		if (!_wcsicmp(me.szModule, L"graphics-hook64.dll")) {
			char buf[MAX_PATH * 3];
			WideCharToMultiByte(CP_UTF8, 0, me.szExePath, -1, buf, sizeof(buf), nullptr, nullptr);
			path = buf;
			break;
		}
	}
	CloseHandle(snap);
	return path;
}

static int reset_video(const char *module)
{
	obs_video_info ovi = {};
	ovi.graphics_module = module;
	ovi.fps_num = 60;
	ovi.fps_den = 1;
	ovi.base_width = ovi.output_width = W;
	ovi.base_height = ovi.output_height = H;
	ovi.output_format = VIDEO_FORMAT_BGRA;
	ovi.gpu_conversion = true;
	ovi.colorspace = VIDEO_CS_SRGB;
	ovi.range = VIDEO_RANGE_FULL;
	ovi.scale_type = OBS_SCALE_POINT;
	const char *adapter = getenv("OBS_TEST_ADAPTER");
	ovi.adapter = adapter ? (uint32_t)atoi(adapter) : 0;
	return obs_reset_video(&ovi);
}

static void skip_test(void *own, const char *why)
{
	obs_shutdown();
	os_dlclose(own);
	print_message("%s, skipped\n", why);
	skip();
}

static void test_capture(void **)
{
	const char *vulkan = argv_[1], *module = argv_[2], *data_dir = argv_[3], *name = argv_[4];
	const std::string c = name;
	auto suffix = [&](const char *s) {
		const size_t n = strlen(s);
		return c.size() > n && c.compare(c.size() - n, n, s) == 0;
	};
	const bool loss = suffix("_loss");     // the Vulkan device is lost (simulated) during the capture
	const bool hdr = suffix("_hdr");       // WGC delivers RGBA16F frames (OBS_WINRT_TEST_HDR)
	const bool resize = suffix("_resize"); // the window keeps changing size
	const std::string base = loss || hdr ? c.substr(0, c.rfind('_')) : c;
	const bool game = base.rfind("game_", 0) == 0;
	const bool shmem = base == "game_shmem_bgra";
	const bool wgc = base.rfind("wgc_", 0) == 0;
	const std::string mode = game                                    ? (shmem ? "bgra" : base.substr(5))
				 : resize                                ? "gdi_resize"
				 : base.find("gdi") != std::string::npos ? "gdi"
									 : "bgra";
	if (loss) {
		// a few seconds in: libobs-vulkan rebuilds the device and every importer imports again
		_putenv_s("OBS_VULKAN_TEST_DEVICE_LOST", "400");
	}
	if (hdr) {
		_putenv_s("OBS_WINRT_TEST_HDR", "1");
	}

	const bool d3d11 = strstr(vulkan, "d3d11") != nullptr;
	void *own = os_dlopen(vulkan); // keeps the validation counter past obs_shutdown
	if (!own) {
		fail_msg("cannot load %s", vulkan);
	}
	auto messages = (int (*)(void))os_dlsym(own, "obs_vulkan_validation_messages");
	assert_true(d3d11 || messages);
	_putenv_s("OBS_VULKAN_VALIDATION", "1");
	base_set_log_handler(log_handler, nullptr);

	// libobs loads modules with a search path that leaves out PATH, where they and their dependencies are outside an
	// install tree: load them the usual way first. libobs-winrt also gives the graphics thread its dispatcher queue,
	// through which WGC delivers frames.
	HMODULE winrt = LoadLibraryA("libobs-winrt.dll");
	HMODULE preloaded = LoadLibraryA(module);
	assert_non_null(winrt);
	assert_non_null(preloaded);

	assert_true(obs_startup("en-US", nullptr, nullptr));
#pragma warning(push)
#pragma warning(disable : 4996)
	obs_add_data_path(OBS_SOURCE_DIR "/libobs/data/");
#pragma warning(pop)
	const int r = reset_video(vulkan);
	if (r == OBS_VIDEO_NOT_SUPPORTED) {
		skip_test(own, "no Vulkan device");
	}
	assert_int_equal(r, OBS_VIDEO_SUCCESS);
	obs_enter_graphics();
	assert_int_equal(gs_get_device_type(), d3d11 ? GS_DEVICE_DIRECT3D_11 : GS_DEVICE_VULKAN);
	gs_adapter_identity id = {};
	const bool have_id = gs_get_adapter_identity(&id);
	obs_leave_graphics();
	assert_true(d3d11 || (have_id && id.luid_valid));
	uint64_t luid_value = 0;
	if (!d3d11) {
		memcpy(&luid_value, id.luid, 8);
	}
	char luid[32];
	snprintf(luid, sizeof(luid), "%llx", (unsigned long long)luid_value);
	print_message("renderer adapter: vendor %04x device %04x\n", id.vendor_id, id.device_id);

	obs_module_t *mod = nullptr;
	assert_int_equal(obs_open_module(&mod, module, data_dir), MODULE_SUCCESS);
	assert_true(obs_init_module(mod));

	HANDLE job = CreateJobObjectW(nullptr, nullptr);
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
	li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));
	const std::string title = "knurbs capture test " + c + " " + std::to_string(GetCurrentProcessId());
	PROCESS_INFORMATION pi = start_producer(mode.c_str(), title, luid, job);
	HWND wnd = wait_for_window(title);
	assert_non_null(wnd);

	char exe[MAX_PATH];
	GetModuleFileNameA(nullptr, exe, MAX_PATH);
	const char *exe_name = strrchr(exe, '\\') ? strrchr(exe, '\\') + 1 : exe;
	const std::string window = title + ":KnurbsCaptureTest:" + exe_name; // no ':' to encode in either

	obs_data_t *settings = obs_data_create();
	obs_source_t *source = nullptr;
	if (game) {
		obs_data_set_string(settings, "capture_mode", "window");
		obs_data_set_string(settings, "window", window.c_str());
		obs_data_set_int(settings, "priority", 1); // exact title
		obs_data_set_bool(settings, "capture_cursor", false);
		obs_data_set_bool(settings, "sli_compatibility", shmem);
		source = obs_source_create("game_capture", "capture", settings, nullptr);
	} else {
		obs_data_set_string(settings, "window", window.c_str());
		obs_data_set_int(settings, "priority", 1);
		obs_data_set_int(settings, "method", wgc ? 2 : 1);
		obs_data_set_bool(settings, "cursor", false);
		obs_data_set_bool(settings, "client_area", true);
		source = obs_source_create("window_capture", "capture", settings, nullptr);
	}
	obs_data_release(settings);
	assert_non_null(source);

	frame_stats st;
	st.float_format = mode == "rgba16f" || hdr;
	stats = &st;
	video_scale_info conv = {};
	conv.format = VIDEO_FORMAT_BGRA;
	conv.width = W;
	conv.height = H;
	conv.range = VIDEO_RANGE_FULL;
	conv.colorspace = VIDEO_CS_SRGB;
	obs_add_raw_video_callback(&conv, on_frame, nullptr);
	obs_set_output_source(0, source);

	// Up to 20 s for the hook or the capture to start, then 4 s of frames.
	const uint64_t start = os_gettime_ns();
	uint64_t first = 0;
	for (;;) {
		os_sleep_ms(50);
		const uint64_t now = os_gettime_ns();
		{
			std::lock_guard<std::mutex> g(st.lock);
			if (!first && st.phase[0] + st.phase[1] + st.bad > 0) {
				first = now;
			}
		}
		// the "_loss" cases run until 3 s after the rebuild (at most 40 s)
		static uint64_t rebuilt_at = 0;
		if (rebuilt && !rebuilt_at) {
			rebuilt_at = now;
		}
		if (loss    ? (rebuilt_at && now - rebuilt_at > 3000000000ull) || now - start > 40000000000ull
		    : first ? now - first > 4000000000ull
			    : now - start > 20000000000ull) {
			break;
		}
	}

	const std::string hook = game ? loaded_hook(pi.dwProcessId) : std::string();
	obs_set_output_source(0, nullptr);
	obs_remove_raw_video_callback(on_frame, nullptr);
	obs_source_release(source);
	obs_wait_for_destroy_queue();
	// window capture frees its capture in a graphics task (wc_destroy): let it run before the device goes
	obs_queue_task(OBS_TASK_GRAPHICS, [](void *) {}, nullptr, true);
	stats = nullptr;
	TerminateProcess(pi.hProcess, 0);
	WaitForSingleObject(pi.hProcess, 5000);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	CloseHandle(job);
	obs_shutdown();
	FreeLibrary(preloaded);
	FreeLibrary(winrt);

	print_message("%s: %d black (before capture), phase 0: %d, phase 1: %d, transitions %d, bad %d\n", name,
		      st.black, st.phase[0], st.phase[1], st.transitions, st.bad);
	if (loss) {
		print_message("after the device rebuild: phase 0: %d, phase 1: %d\n", st.after_rebuild[0],
			      st.after_rebuild[1]);
	}
	if (!st.first_bad.empty()) {
		print_message("first bad frame: %s\n", st.first_bad.c_str());
	}

	// The path each method must take.
	if (game) {
		// The hook from the data dir given (an OBS Studio installation's, to test against the official one).
		print_message("hook loaded by the producer: %s\n", hook.c_str());
		char want[MAX_PATH], got[MAX_PATH];
		const std::string want_path = std::string(data_dir) + "/graphics-hook64.dll";
		assert_non_null(_fullpath(want, want_path.c_str(), MAX_PATH));
		assert_non_null(_fullpath(got, hook.c_str(), MAX_PATH));
		assert_int_equal(_stricmp(want, got), 0);
		assert_true(logged(shmem ? "memory capture successful" : "shared texture capture successful"));
	} else if (wgc && !d3d11) {
		if (!logged("capturing through a D3D11 device")) {
			fail_msg("window capture did not use Windows Graphics Capture through the D3D11 bridge");
		}
	}

	assert_true(st.phase[0] >= 5 && st.phase[1] >= 5);
	assert_true(st.transitions >= 4);
	// Game capture has no synchronisation with the hook (as on D3D11), so a frame torn between phases is possible
	// in principle; every other path must be exact.
	const int total = st.phase[0] + st.phase[1] + st.bad;
	if (loss) {
		assert_true(rebuilt);
		assert_true(st.after_rebuild[0] >= 5 && st.after_rebuild[1] >= 5);
		assert_true(st.bad * 50 <= total); // frames rendered while the device was lost
	} else if (resize) {
		// new shared textures for every size; frames delivered while the window resizes may be partial
		const int sizes = times_logged("[winrt-capture] Vulkan: shared textures");
		print_message("shared texture sets created: %d\n", sizes);
		assert_true(d3d11 || sizes >= 4); // the Vulkan bridge's textures
		assert_true(st.bad * 10 <= total);
	} else if (game && !shmem) {
		assert_true(st.bad * 50 <= total);
	} else {
		assert_true(st.bad <= 1); // the frame on which the capture starts may be partial (BitBlt, resize)
	}

	const int n = messages ? messages() : -1;
	if (n < 0) {
		print_message("validation layer missing, not checked\n");
	} else {
		assert_int_equal(n, 0);
	}
	os_dlclose(own);
}

int main(int argc, char **argv)
{
	if (argc == 5 && !strcmp(argv[1], "producer")) {
		return producer_main(argv[2], argv[3], argv[4]);
	}
	if (argc < 5) {
		fprintf(stderr, "usage: %s <vulkan module> <win-capture module> <data dir> <case>\n", argv[0]);
		return 2;
	}
	argc_ = argc;
	argv_ = argv;
	const struct CMUnitTest tests[] = {cmocka_unit_test(test_capture)};
	return cmocka_run_group_tests(tests, nullptr, nullptr);
}
