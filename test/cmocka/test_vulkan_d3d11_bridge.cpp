// shared/d3d11-vulkan-bridge on the Vulkan renderer: a private D3D11 device writes frames into the keyed-mutex slot
// set, libobs-vulkan (through the generic gs_* API) draws them, and a simulated device loss (frames.test_lose_after)
// hits at chosen points. After the rebuild every slot must be writable by D3D11 again: a slot whose keyed mutex is
// still held by the lost device is never given back, so the set must notice it and be created again.
//
// Skips without a Vulkan device, without the validation layer (unless OBS_VULKAN_REQUIRE_VALIDATION=1), or when the
// device cannot import D3D11 keyed-mutex textures.
#include <stdio.h>
#include <stdlib.h>

#include <string>
#include <vector>

#include <d3d11-vulkan-bridge/d3d11-vulkan-bridge.hpp>

#include <graphics/graphics.h>
#include <graphics/graphics-internal.h>
#include <graphics/vec4.h>
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

namespace bridge = d3d11_vulkan_bridge;

static const uint32_t W = 64, H = 64;

static void *module;
static int (*messages)(void);

static int setup(void **)
{
	_putenv_s("OBS_VULKAN_VALIDATION", "1");
	module = os_dlopen(OBS_VULKAN_MODULE); // keeps the validation counter past gs_destroy
	if (!module) {
		return -1;
	}
	messages = (decltype(messages))os_dlsym(module, "obs_vulkan_validation_messages");
	return messages ? 0 : -1;
}

static int teardown(void **)
{
	os_dlclose(module);
	return 0;
}

struct fixture {
	graphics_t *graphics = nullptr;
	gs_device *d = nullptr;
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	ComPtr<ID3D11Texture2D> frame;
	gs_texture_t *rt = nullptr;
	bridge::slots slots{"[test]"};
	int reimported = 0, recreated = 0;
};

// As plugins/win-capture/dxgi-duplicator.cpp's device_loss_rebuild: import again, or create the set again.
static void on_release(void *) {}

static void on_rebuild(void *, void *data)
{
	auto f = (fixture *)data;
	if (f->slots.reimport()) {
		++f->reimported;
		return;
	}
	f->slots.destroy();
	assert_true(f->slots.create(f->device.Get(), W, H, DXGI_FORMAT_B8G8R8A8_UNORM));
	++f->recreated;
}

static bool open(fixture &f)
{
	const int r = gs_create(&f.graphics, OBS_VULKAN_MODULE, 0);
	if (r == GS_ERROR_NOT_SUPPORTED) {
		print_message("SKIPPED: no Vulkan 1.3 device\n");
		return false;
	}
	assert_int_equal(r, GS_SUCCESS);
	gs_enter_context(f.graphics);
	f.d = (gs_device *)f.graphics->device;
	if (messages() < 0) {
		const char *rq = getenv("OBS_VULKAN_REQUIRE_VALIDATION");
		if (rq && !strcmp(rq, "1")) {
			fail_msg("validation layer required (OBS_VULKAN_REQUIRE_VALIDATION=1) but missing");
		}
		print_message("SKIPPED: no validation layer\n");
		gs_leave_context();
		gs_destroy(f.graphics);
		return false;
	}
	LUID luid;
	if (!f.d->ext_keyed_mutex || !bridge::renderer_luid(luid) ||
	    !bridge::create_device(luid, f.device, f.context, nullptr, "[test]")) {
		print_message("SKIPPED: no D3D11 keyed-mutex import on this device\n");
		gs_leave_context();
		gs_destroy(f.graphics);
		return false;
	}
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = W;
	td.Height = H;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	assert_true(SUCCEEDED(f.device->CreateTexture2D(&td, nullptr, &f.frame)));
	assert_true(f.slots.create(f.device.Get(), W, H, DXGI_FORMAT_B8G8R8A8_UNORM));
	f.rt = gs_texture_create(W, H, GS_BGRA, 1, nullptr, GS_RENDER_TARGET);
	assert_non_null(f.rt);
	const gs_device_loss callbacks = {on_release, on_rebuild, &f};
	gs_register_loss_callbacks(&callbacks);
	return true;
}

static void close(fixture &f)
{
	gs_unregister_loss_callbacks(&f);
	f.slots.destroy();
	gs_texture_destroy(f.rt);
	gs_leave_context();
	gs_destroy(f.graphics);
	f.frame.Clear();
	f.context.Clear();
	f.device.Clear();
	assert_int_equal(messages(), 0);
}

// A submission of its own (a clear), which the armed test hook turns into the loss.
static void submit(fixture &f)
{
	gs_set_render_target(f.rt, nullptr);
	vec4 c;
	vec4_set(&c, 0, 0, 0, 1);
	gs_clear(GS_CLEAR_COLOR, &c, 1.0f, 0);
	gs_set_render_target(nullptr, nullptr);
	gs_flush();
}

// Every slot takes a frame (D3D11 gets each keyed mutex within slots::d3d11_acquire_ms) and Vulkan draws it.
static void check_every_slot_writable(fixture &f)
{
	for (int i = 0; i < 2 * bridge::slots::count; ++i) {
		assert_true(f.slots.write(f.context.Get(), f.frame.Get(), nullptr));
		assert_non_null(f.slots.draw_texture());
		gs_flush();
	}
}

// Lost while Vulkan holds the drawn slot: the lost device never releases it.
static void test_loss_while_drawing(void **)
{
	fixture f;
	if (!open(f)) {
		skip();
	}
	assert_true(f.slots.write(f.context.Get(), f.frame.Get(), nullptr));
	assert_non_null(f.slots.draw_texture());
	gs_flush();
	f.d->frames.test_lose_after = 1;
	submit(f);
	assert_true(vk_lost(f.d));
	gs_begin_frame();
	assert_false(vk_lost(f.d));
	assert_int_equal(f.recreated, 1); // slot 0 is held for good
	check_every_slot_writable(f);
	close(f);
}

// Lost between the release of the drawn slot and the acquire of the newer one (slots::draw_texture): the release never
// reaches the driver, so the old slot stays held by the lost device although the set no longer counts it as drawn.
static void test_loss_between_release_and_acquire(void **)
{
	fixture f;
	if (!open(f)) {
		skip();
	}
	assert_true(f.slots.write(f.context.Get(), f.frame.Get(), nullptr));
	assert_non_null(f.slots.draw_texture()); // Vulkan holds slot 0
	gs_flush();
	assert_true(f.slots.write(f.context.Get(), f.frame.Get(), nullptr)); // slot 1 is newer
	f.d->frames.test_lose_after = 1; // the release of slot 0
	assert_null(f.slots.draw_texture());
	assert_true(vk_lost(f.d));
	gs_begin_frame();
	assert_false(vk_lost(f.d));
	assert_int_equal(f.reimported + f.recreated, 1);
	assert_int_equal(f.recreated, 1); // slot 0 is held for good: importing again cannot work
	check_every_slot_writable(f);
	close(f);
}

// Lost with no slot held by Vulkan (frames written, none drawn yet): importing the same textures again is enough.
static void test_loss_with_no_slot_held(void **)
{
	fixture f;
	if (!open(f)) {
		skip();
	}
	assert_true(f.slots.write(f.context.Get(), f.frame.Get(), nullptr));
	f.d->frames.test_lose_after = 1;
	submit(f);
	assert_true(vk_lost(f.d));
	gs_begin_frame();
	assert_false(vk_lost(f.d));
	assert_int_equal(f.reimported, 1);
	check_every_slot_writable(f);
	close(f);
}

int main(void)
{
	setvbuf(stdout, nullptr, _IONBF, 0); // the log up to a hang reaches ctest
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_loss_while_drawing),
		cmocka_unit_test(test_loss_between_release_and_acquire),
		cmocka_unit_test(test_loss_with_no_slot_held),
	};
	return cmocka_run_group_tests(tests, setup, teardown);
}
