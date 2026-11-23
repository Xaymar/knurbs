#include <stdio.h>
#include <stdlib.h>

#include <chrono>
#include <future>
#include <thread>

#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <util/platform.h>

#include "vulkan-subsystem.hpp"

#ifdef _WIN32
#include <windows.h>
#else
// Linux presents to Wayland only: the test starts its own headless weston (when installed) and shows xdg toplevels.
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <png.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

#include <obs-nix-platform.h>
#include <string>
#include <vector>
extern char **environ;
#endif

// cmocka.h redefines `inline`, so it must come after the C++ standard headers.
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
extern "C" {
#include <cmocka.h>
}

// Set OBS_VULKAN_REQUIRE_VALIDATION=1 to make a missing validation layer fail the test.
static void require_validation()
{
	const char *rq = getenv("OBS_VULKAN_REQUIRE_VALIDATION");
	if (rq && !strcmp(rq, "1")) {
		fail_msg("validation layer required (OBS_VULKAN_REQUIRE_VALIDATION=1) but missing");
	}
}

static void *module;
static int (*create)(gs_device_t **, uint32_t);
static void (*destroy)(gs_device_t *);
static int (*messages)(void);
static gs_swapchain_t *(*swapchain_create)(gs_device_t *, const struct gs_init_data *);
static void (*swapchain_destroy)(gs_swapchain_t *);
static void (*resize)(gs_device_t *, uint32_t, uint32_t);
static void (*load_swapchain)(gs_device_t *, gs_swapchain_t *);
static bool (*present_ready)(gs_device_t *);
static void (*present)(gs_device_t *);
static enum gs_color_space (*get_color_space)(gs_device_t *);
static void (*update_color_space)(gs_device_t *);
static void (*clear)(gs_device_t *, uint32_t, const struct vec4 *, float, uint8_t);
static uint32_t (*get_width)(const gs_device_t *);

template<typename T> static bool load(T &fn, const char *name)
{
	fn = (T)os_dlsym(module, name);
	if (!fn) {
		print_message("missing export: %s\n", name);
	}
	return fn != nullptr;
}

#ifndef _WIN32
static bool start_weston();
static void stop_weston();
#endif

static int setup(void **)
{
#ifdef _WIN32
	_putenv_s("OBS_VULKAN_VALIDATION", "1");
#else
	setenv("OBS_VULKAN_VALIDATION", "1", 1);
	if (!start_weston()) {
		print_message("no headless weston (install weston): the Wayland cases skip\n");
	}
#endif
	module = os_dlopen(OBS_VULKAN_MODULE);
	if (!module) {
		return -1;
	}
	bool ok = load(create, "device_create") & load(destroy, "device_destroy") &
		  load(messages, "obs_vulkan_validation_messages") & load(swapchain_create, "device_swapchain_create") &
		  load(swapchain_destroy, "gs_swapchain_destroy") & load(resize, "device_resize") &
		  load(load_swapchain, "device_load_swapchain") & load(present_ready, "device_is_present_ready") &
		  load(present, "device_present") & load(get_color_space, "device_get_color_space") &
		  load(update_color_space, "device_update_color_space") & load(clear, "device_clear") &
		  load(get_width, "device_get_width");
	return ok ? 0 : -1;
}

static int teardown(void **)
{
	os_dlclose(module);
#ifndef _WIN32
	stop_weston();
#endif
	return 0;
}

static gs_device *create_or_skip()
{
	gs_device_t *device = nullptr;
	int ret = create(&device, 0);
	if (ret == GS_ERROR_NOT_SUPPORTED) {
		skip();
	}
	assert_int_equal(ret, GS_SUCCESS);
	return device;
}

static void destroy_and_check(gs_device *device)
{
	destroy(device);
	int n = messages();
	if (n < 0) {
		require_validation();
		print_message("validation layer missing, not checked\n");
	} else {
		assert_int_equal(n, 0);
	}
}

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

static const uint32_t size = 128;

#ifdef _WIN32
// Hidden (never shown) popup window, so the client area is exactly size x size.
static void *create_window()
{
	static ATOM cls = 0;
	if (!cls) {
		WNDCLASSW wc = {};
		wc.lpfnWndProc = DefWindowProcW;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.lpszClassName = L"obs_vulkan_test";
		cls = RegisterClassW(&wc);
	}
	HWND hwnd = CreateWindowExW(0, L"obs_vulkan_test", L"", WS_POPUP, 0, 0, size, size, nullptr, nullptr,
				    GetModuleHandleW(nullptr), nullptr);
	if (!hwnd) {
		skip();
	}
	return hwnd;
}

static void destroy_window(void *w)
{
	DestroyWindow((HWND)w);
}
#else
// A private headless weston (GL renderer, so it imports the WSI's dmabufs) with its own runtime directory and
// socket; --debug enables the screenshooter that test_frames_reach_the_compositor reads back.
static struct {
	pid_t pid = -1;
	std::string runtime;
	struct wl_display *display = nullptr;
	struct wl_compositor *compositor = nullptr;
	struct xdg_wm_base *wm = nullptr;
} wl;

static const uint32_t output_w = 800, output_h = 600;

static void wm_ping(void *, struct xdg_wm_base *wm, uint32_t serial)
{
	xdg_wm_base_pong(wm, serial);
}
static const struct xdg_wm_base_listener wm_listener = {wm_ping};

static void global_add(void *, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version)
{
	if (!strcmp(iface, wl_compositor_interface.name)) {
		wl.compositor = (struct wl_compositor *)wl_registry_bind(reg, name, &wl_compositor_interface,
									 std::min(version, 4u));
	} else if (!strcmp(iface, xdg_wm_base_interface.name)) {
		wl.wm = (struct xdg_wm_base *)wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(wl.wm, &wm_listener, nullptr);
	}
}
static void global_remove(void *, struct wl_registry *, uint32_t) {}
static const struct wl_registry_listener registry_listener = {global_add, global_remove};

static bool start_weston()
{
	char tmpl[] = "/tmp/obs-vulkan-wayland-XXXXXX";
	if (!mkdtemp(tmpl)) {
		return false;
	}
	wl.runtime = tmpl;
	setenv("XDG_RUNTIME_DIR", tmpl, 1);
	const std::string width = "--width=" + std::to_string(output_w),
			  height = "--height=" + std::to_string(output_h);
	const char *argv[] = {"weston",  "--backend=headless", "--renderer=gl", "--socket=obs-vulkan-test",
			      "--debug", width.c_str(),        height.c_str(),  "--idle-time=0",
			      nullptr};
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	const std::string log = wl.runtime + "/weston.log";
	posix_spawn_file_actions_addopen(&fa, 1, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
	posix_spawn_file_actions_adddup2(&fa, 1, 2);
	const int err = posix_spawnp(&wl.pid, "weston", &fa, nullptr, (char *const *)argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	if (err) {
		wl.pid = -1;
		return false;
	}
	for (int i = 0; i < 100 && !wl.display; ++i) {
		os_sleep_ms(50);
		wl.display = wl_display_connect("obs-vulkan-test");
	}
	if (!wl.display) {
		print_message("weston did not start, see %s\n", log.c_str());
		return false;
	}
	struct wl_registry *reg = wl_display_get_registry(wl.display);
	wl_registry_add_listener(reg, &registry_listener, nullptr);
	wl_display_roundtrip(wl.display);
	if (!wl.compositor || !wl.wm) {
		return false;
	}
	obs_set_nix_platform(OBS_NIX_PLATFORM_WAYLAND);
	obs_set_nix_platform_display(wl.display);
	return true;
}

static void stop_weston()
{
	if (wl.display) {
		wl_display_disconnect(wl.display);
		wl.display = nullptr;
	}
	if (wl.pid > 0) {
		kill(wl.pid, SIGTERM);
		waitpid(wl.pid, nullptr, 0);
		wl.pid = -1;
	}
	if (!wl.runtime.empty()) { // the log, screenshots, socket and lock
		if (DIR *dir = opendir(wl.runtime.c_str())) {
			while (struct dirent *e = readdir(dir)) {
				if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
					unlink((wl.runtime + "/" + e->d_name).c_str());
				}
			}
			closedir(dir);
		}
		rmdir(wl.runtime.c_str());
	}
}

struct wl_window {
	struct wl_surface *surface;
	struct xdg_surface *xdg;
	struct xdg_toplevel *top;
	bool configured = false;
};

static void xdg_configure(void *data, struct xdg_surface *xdg, uint32_t serial)
{
	xdg_surface_ack_configure(xdg, serial);
	((wl_window *)data)->configured = true;
}
static const struct xdg_surface_listener xdg_listener = {xdg_configure};

// An xdg toplevel (so the compositor maps it and sends frame callbacks); the swapchain sizes its buffers.
static void *create_window()
{
	if (!wl.display) {
		skip();
	}
	auto w = new wl_window;
	w->surface = wl_compositor_create_surface(wl.compositor);
	w->xdg = xdg_wm_base_get_xdg_surface(wl.wm, w->surface);
	xdg_surface_add_listener(w->xdg, &xdg_listener, w);
	w->top = xdg_surface_get_toplevel(w->xdg);
	xdg_toplevel_set_title(w->top, "obs vulkan test");
	wl_surface_commit(w->surface);
	while (!w->configured && wl_display_roundtrip(wl.display) >= 0) {
	}
	return w;
}

static void destroy_window(void *p)
{
	auto w = (wl_window *)p;
	xdg_toplevel_destroy(w->top);
	xdg_surface_destroy(w->xdg);
	wl_surface_destroy(w->surface);
	wl_display_roundtrip(wl.display);
	delete w;
}

// The compositor's output as RGB rows, through weston-screenshooter (a PNG in the runtime directory).
static bool screenshot(std::vector<uint8_t> &rgb, uint32_t &w, uint32_t &h)
{
	wl_display_roundtrip(wl.display); // our commits are in
	os_sleep_ms(100);                 // and repainted
	const std::string cmd =
		"cd '" + wl.runtime +
		"' && rm -f wayland-screenshot-*.png && WAYLAND_DISPLAY=obs-vulkan-test weston-screenshooter >/dev/null 2>&1";
	if (system(cmd.c_str()) != 0) {
		return false;
	}
	std::string path;
	if (DIR *dir = opendir(wl.runtime.c_str())) {
		while (struct dirent *e = readdir(dir)) {
			if (!strncmp(e->d_name, "wayland-screenshot-", 19)) {
				path = wl.runtime + "/" + e->d_name;
			}
		}
		closedir(dir);
	}
	png_image img = {};
	img.version = PNG_IMAGE_VERSION;
	if (path.empty() || !png_image_begin_read_from_file(&img, path.c_str())) {
		return false;
	}
	img.format = PNG_FORMAT_RGB;
	w = img.width;
	h = img.height;
	rgb.resize(PNG_IMAGE_SIZE(img));
	return png_image_finish_read(&img, nullptr, rgb.data(), 0, nullptr) != 0;
}

// Pixels of the output within `tol` of the colour.
static size_t count_color(const std::vector<uint8_t> &rgb, int r, int g, int b, int tol)
{
	size_t n = 0;
	for (size_t i = 0; i + 2 < rgb.size(); i += 3) {
		n += abs(rgb[i] - r) <= tol && abs(rgb[i + 1] - g) <= tol && abs(rgb[i + 2] - b) <= tol;
	}
	return n;
}
#endif

static gs_swapchain_t *create_swap(gs_device *device, void *window)
{
	gs_init_data data = {};
#ifdef _WIN32
	data.window.hwnd = window;
#else
	data.window.display = ((wl_window *)window)->surface; // as the frontend on Wayland: the wl_surface
#endif
	data.cx = size;
	data.cy = size;
	data.num_backbuffers = 2;
	data.format = GS_BGRA;
	data.zsformat = GS_ZS_NONE;
	gs_swapchain_t *swap = swapchain_create(device, &data);
	assert_non_null(swap);
	return swap;
}

// Presents `n` cleared frames on the loaded swapchain; not-ready frames are retried (FIFO may hold every image).
static void present_frames(gs_device *device, uint32_t n)
{
	within_5s([&] {
		for (uint32_t done = 0; done < n;) {
			if (!present_ready(device)) {
				os_sleep_ms(1);
				continue;
			}
			struct vec4 color;
			vec4_set(&color, 0.0f, 0.5f, 1.0f, 1.0f);
			clear(device, GS_CLEAR_COLOR, &color, 1.0f, 0);
			const uint64_t before = device->graphics.next_value;
			present(device);
			assert_true(device->graphics.next_value > before); // the frame was submitted
			assert_int_equal(device->swap->image, UINT32_MAX); // and the image handed back
			++done;
		}
	});
}

static void test_present_after_clear_succeeds(void **)
{
	void *window = create_window();
	gs_device *device = create_or_skip();
	gs_swapchain_t *swap = create_swap(device, window);
	load_swapchain(device, swap);
	assert_int_equal(get_width(device), size);
	assert_int_equal(get_color_space(device), GS_CS_SRGB);
	update_color_space(device);
	assert_int_equal(get_color_space(device), GS_CS_SRGB);

	// More frames than images: every image and semaphore is reused.
	present_frames(device, 10);

	swapchain_destroy(swap);
	destroy_and_check(device);
	destroy_window(window);
}

// Review Focus 2: a 0x0 (minimized) size has no swapchain; present is a no-op and nothing is submitted.
static void test_resize_to_zero_then_present_is_noop(void **)
{
	void *window = create_window();
	gs_device *device = create_or_skip();
	gs_swapchain_t *swap = create_swap(device, window);
	load_swapchain(device, swap);
	present_frames(device, 1);

	resize(device, 0, 0);
	within_5s([&] {
		for (int i = 0; i < 3; ++i) {
			assert_false(present_ready(device));
			struct vec4 color;
			vec4_set(&color, 1.0f, 0.0f, 0.0f, 1.0f);
			clear(device, GS_CLEAR_COLOR, &color, 1.0f, 0);
			const uint64_t before = device->graphics.next_value;
			present(device);
			assert_int_equal(device->graphics.next_value, before);
		}
	});

	swapchain_destroy(swap);
	destroy_and_check(device);
	destroy_window(window);
}

static void test_resize_back_recovers(void **)
{
	void *window = create_window();
	gs_device *device = create_or_skip();
	gs_swapchain_t *swap = create_swap(device, window);
	load_swapchain(device, swap);

	resize(device, 0, 0);
	assert_false(present_ready(device));
	resize(device, size, size);
	present_frames(device, 4);
	assert_int_equal(get_width(device), size);

	swapchain_destroy(swap);
	destroy_and_check(device);
	destroy_window(window);
}

// Review Focus 5: a swapchain that is not ready is skipped without stalling the others.
static void test_two_swapchains_present_independently(void **)
{
	void *wa = create_window();
	void *wb = create_window();
	gs_device *device = create_or_skip();
	gs_swapchain_t *a = create_swap(device, wa);
	gs_swapchain_t *b = create_swap(device, wb);
	a->test_not_ready = true;

	within_5s([&] {
		for (int frame = 0; frame < 4; ++frame) {
			load_swapchain(device, a);
			assert_false(present_ready(device));
			const uint64_t before = device->graphics.next_value;
			present(device);
			assert_int_equal(device->graphics.next_value, before);

			load_swapchain(device, b);
			present_frames(device, 1);
		}
	});

	// Ready again: it presents too.
	a->test_not_ready = false;
	load_swapchain(device, a);
	present_frames(device, 2);

	swapchain_destroy(a);
	swapchain_destroy(b);
	destroy_and_check(device);
	destroy_window(wa);
	destroy_window(wb);
}

// Test hook OBS_VULKAN_TEST_FAIL_SUBMIT, read once at device creation; toggled here on the live device.
static void set_fail_submit(gs_device *device, bool on)
{
	device->frames.test_fail_submit = on;
}

// Carried finding 1: a failed render submit never signals the present semaphore, so present must be skipped
// (never wait on it), the acquired image must not leak, and later frames must present normally.
static void test_present_after_failed_submit_skips_and_recovers(void **)
{
	void *window = create_window();
	gs_device *device = create_or_skip();
	gs_swapchain_t *swap = create_swap(device, window);
	load_swapchain(device, swap);

	for (int i = 0; i < 3; ++i) {
		within_5s([&] {
			while (!present_ready(device)) {
				os_sleep_ms(1);
			}
		});
		struct vec4 color;
		vec4_set(&color, 1.0f, 1.0f, 0.0f, 1.0f);
		clear(device, GS_CLEAR_COLOR, &color, 1.0f, 0);
		set_fail_submit(device, true);
		within_5s([&] { present(device); });
		set_fail_submit(device, false);
		assert_int_equal(swap->image, UINT32_MAX);
		assert_null(device->acquire_wait);
	}
	present_frames(device, 4);

	swapchain_destroy(swap);
	destroy_and_check(device);
	destroy_window(window);
}

// A swapchain that keeps reporting VK_SUBOPTIMAL_KHR (test hook) at an unchanged size is not rebuilt every frame (a
// rebuild idles the queue); once the window's size differs from the swapchain's, it is rebuilt to the new size.
static void test_persistent_suboptimal_rebuilds_only_on_size_change(void **)
{
	void *window = create_window();
	gs_device *device = create_or_skip();
	gs_swapchain_t *swap = create_swap(device, window);
	load_swapchain(device, swap);
	present_frames(device, 1);

	swap->test_suboptimal = true;
	const uint32_t builds = swap->builds;
	present_frames(device, 5);
	assert_int_equal(swap->builds, builds);
	assert_false(swap->recreate);

#ifdef _WIN32
	SetWindowPos((HWND)window, nullptr, 0, 0, size / 2, size / 2, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
	present_frames(device, 2);
	assert_true(swap->builds > builds);
	assert_int_equal(get_width(device), size / 2);
#endif

	swapchain_destroy(swap);
	destroy_and_check(device);
	destroy_window(window);
}

#ifndef _WIN32
// What libobs presents reaches the compositor: two colours in turn (0.5 must stay 128: no sRGB conversion on the way),
// then a resize, each checked in weston's output.
static void present_color(gs_device *device, float r, float g, float b, int frames)
{
	within_5s([&] {
		for (int done = 0; done < frames;) {
			if (!present_ready(device)) {
				os_sleep_ms(1);
				continue;
			}
			struct vec4 color;
			vec4_set(&color, r, g, b, 1.0f);
			clear(device, GS_CLEAR_COLOR, &color, 1.0f, 0);
			present(device);
			++done;
		}
	});
}

static void test_frames_reach_the_compositor(void **)
{
	void *window = create_window();
	gs_device *device = create_or_skip();
	gs_swapchain_t *swap = create_swap(device, window);
	load_swapchain(device, swap);
	std::vector<uint8_t> rgb;
	uint32_t w = 0, h = 0;

	present_color(device, 1.0f, 0.0f, 0.0f, 3);
	assert_true(screenshot(rgb, w, h));
	assert_int_equal(w, output_w);
	assert_int_equal(h, output_h);
	assert_int_equal(count_color(rgb, 255, 0, 0, 0), size * size);

	present_color(device, 0.0f, 0.5f, 1.0f, 3);
	assert_true(screenshot(rgb, w, h));
	assert_int_equal(count_color(rgb, 255, 0, 0, 0), 0);
	assert_int_equal(count_color(rgb, 0, 128, 255, 1), size * size);

	resize(device, size / 2, size * 3 / 4);
	present_color(device, 0.0f, 1.0f, 0.0f, 3);
	assert_true(screenshot(rgb, w, h));
	assert_int_equal(get_width(device), size / 2);
	assert_int_equal(count_color(rgb, 0, 255, 0, 0), (size / 2) * (size * 3 / 4));

	swapchain_destroy(swap);
	destroy_and_check(device);
	destroy_window(window);
}

// Linux presents to Wayland only: without libobs's Wayland display (an X11 session) there is no swapchain.
static void test_x11_is_refused(void **)
{
	void *window = create_window();
	gs_device *device = create_or_skip();
	obs_set_nix_platform(OBS_NIX_PLATFORM_X11_EGL);
	gs_init_data data = {};
	data.window.display = ((wl_window *)window)->surface;
	data.cx = data.cy = size;
	data.num_backbuffers = 2;
	data.format = GS_BGRA;
	assert_null(swapchain_create(device, &data));
	obs_set_nix_platform(OBS_NIX_PLATFORM_WAYLAND);
	destroy_and_check(device);
	destroy_window(window);
}
#endif

int main()
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_present_after_clear_succeeds),
		cmocka_unit_test(test_resize_to_zero_then_present_is_noop),
		cmocka_unit_test(test_resize_back_recovers),
		cmocka_unit_test(test_two_swapchains_present_independently),
		cmocka_unit_test(test_present_after_failed_submit_skips_and_recovers),
		cmocka_unit_test(test_persistent_suboptimal_rebuilds_only_on_size_change),
#ifndef _WIN32
		cmocka_unit_test(test_frames_reach_the_compositor),
		cmocka_unit_test(test_x11_is_refused),
#endif
	};
	return cmocka_run_group_tests(tests, setup, teardown);
}
