/* Video Device Capture: backend selection, device listing and
 * deactivate-when-hidden, with fake backends instead of hardware. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <obs.h>
#include <util/bmem.h>
#include <util/dstr.h>

#include <stdio.h>
#include <string.h>

#include "video-device.h"

/* The plugin's locale lookup, stubbed: tests compile the plugin sources
 * without the module loader. */
const char *obs_module_text(const char *val)
{
	return val;
}

/* ------------------------------------------------------------------------- */
/* Fake backends                                                             */

struct fake_state {
	int creates;
	int destroys;
	int updates;
	int hides;
	int shows;
	char last_device[64];
	bool available;
};

static struct fake_state fake_a_state;
static struct fake_state fake_b_state;

struct fake_instance {
	struct fake_state *state;
};

static void remember(struct fake_state *st, obs_data_t *settings)
{
	strncpy(st->last_device, obs_data_get_string(settings, VD_DEVICE_ID), sizeof(st->last_device) - 1);
}

#define FAKE_BACKEND(tag)                                                                      \
	static bool tag##_available(void)                                                      \
	{                                                                                      \
		return tag##_state.available;                                                  \
	}                                                                                      \
	static void tag##_enum(vd_device_cb cb, void *param)                                   \
	{                                                                                      \
		cb(param, #tag "-cam0", "Camera 0 (" #tag ")");                                \
		cb(param, #tag "-cam1", "Camera 1 (" #tag ")");                                \
	}                                                                                      \
	static void *tag##_create(obs_source_t *source, obs_data_t *settings)                  \
	{                                                                                      \
		UNUSED_PARAMETER(source);                                                      \
		struct fake_instance *f = bzalloc(sizeof(*f));                                 \
		f->state = &tag##_state;                                                       \
		f->state->creates++;                                                           \
		remember(f->state, settings);                                                  \
		return f;                                                                      \
	}                                                                                      \
	static void tag##_destroy(void *data)                                                  \
	{                                                                                      \
		struct fake_instance *f = data;                                                \
		f->state->destroys++;                                                          \
		bfree(f);                                                                      \
	}                                                                                      \
	static void tag##_update(void *data, obs_data_t *settings)                             \
	{                                                                                      \
		struct fake_instance *f = data;                                                \
		f->state->updates++;                                                           \
		remember(f->state, settings);                                                  \
	}                                                                                      \
	static const struct vd_backend tag##_backend = {                                       \
		.name = #tag,                                                                  \
		.display_name = "backend." #tag,                                               \
		.available = tag##_available,                                                  \
		.enum_devices = tag##_enum,                                                    \
		.create = tag##_create,                                                        \
		.destroy = tag##_destroy,                                                      \
		.update = tag##_update,                                                        \
	};

FAKE_BACKEND(fake_a)
FAKE_BACKEND(fake_b)

/* A backend that pauses on hide instead of being destroyed. It shares
 * fake_b's state. */
static void pausable_hide(void *data)
{
	struct fake_instance *f = data;
	f->state->hides++;
}

static void pausable_show(void *data)
{
	struct fake_instance *f = data;
	f->state->shows++;
}

static const struct vd_backend pausable_backend = {
	.name = "pausable",
	.display_name = "backend.pausable",
	.available = fake_b_available,
	.enum_devices = fake_b_enum,
	.create = fake_b_create,
	.destroy = fake_b_destroy,
	.update = fake_b_update,
	.show = pausable_show,
	.hide = pausable_hide,
};

/* The plugin's backend table for this test build. */
const struct vd_backend *const vd_backends[] = {&fake_a_backend, &fake_b_backend, &pausable_backend, NULL};
const size_t vd_num_backends = 3;

/* ------------------------------------------------------------------------- */
/* Fixture                                                                   */

static long allocs_before;

static int setup(void **state)
{
	UNUSED_PARAMETER(state);
	memset(&fake_a_state, 0, sizeof(fake_a_state));
	memset(&fake_b_state, 0, sizeof(fake_b_state));
	fake_a_state.available = true;
	fake_b_state.available = true;

	allocs_before = bnum_allocs();
	if (!obs_startup("en-US", NULL, NULL)) {
		return -1;
	}
	obs_register_source(&video_device_capture_info);
	return 0;
}

static int teardown(void **state)
{
	UNUSED_PARAMETER(state);
	obs_shutdown();
	if (bnum_allocs() != allocs_before) {
		fprintf(stderr, "leaked %ld allocations\n", bnum_allocs() - allocs_before);
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */

static void control_keys_and_colors(void **state)
{
	UNUSED_PARAMETER(state);
	struct dstr key = {0};
	vd_control_key(&key, "v4l2", "White Balance, Auto");
	assert_string_equal(key.array, "v4l2.control.white-balance-auto");
	vd_control_key(&key, "pipewire", "  Exposure (Absolute) ");
	assert_string_equal(key.array, "pipewire.control.exposure-absolute");
	dstr_free(&key);

	assert_int_equal(vd_parse_color_space("2100pq"), VIDEO_CS_2100_PQ);
	assert_int_equal(vd_parse_color_space("auto"), VIDEO_CS_DEFAULT);
	assert_int_equal(vd_parse_color_range("full"), VIDEO_RANGE_FULL);
	assert_true(vd_is_auto("auto"));
	assert_true(vd_is_auto(""));
	assert_false(vd_is_auto("709"));

	struct obs_source_frame f = {0};
	f.format = VIDEO_FORMAT_BGRA;
	vd_frame_set_color(&f, VIDEO_CS_2100_HLG, VIDEO_RANGE_FULL);
	assert_int_equal(f.trc, VIDEO_TRC_HLG);
	assert_true(f.full_range);
}

/* ------------------------------------------------------------------------- */
/* Source behaviour                                                          */

static void auto_prefers_device_owner_then_first(void **state)
{
	UNUSED_PARAMETER(state);

	/* No device saved: first available backend. */
	obs_source_t *src = obs_source_create(VIDEO_DEVICE_SOURCE_ID, "a", NULL, NULL);
	assert_int_equal(fake_a_state.creates, 1);
	obs_source_release(src);

	/* Saved device belongs to fake_b: Auto uses fake_b. */
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, VD_DEVICE_BACKEND, "fake_b");
	obs_data_set_string(s, VD_DEVICE_ID, "fake_b-cam1");
	src = obs_source_create(VIDEO_DEVICE_SOURCE_ID, "b", s, NULL);
	assert_int_equal(fake_b_state.creates, 1);
	obs_source_release(src);

	/* fake_a unavailable: the first available backend is fake_b. */
	fake_a_state.available = false;
	src = obs_source_create(VIDEO_DEVICE_SOURCE_ID, "c", NULL, NULL);
	assert_int_equal(fake_a_state.creates, 1);
	assert_int_equal(fake_b_state.creates, 2);
	obs_source_release(src);
	obs_data_release(s);
}

/* Video sources apply updates on the next video tick, and these tests run
 * without a video thread, so call the source's update directly. */
static void apply_update(obs_source_t *src, obs_data_t *s)
{
	obs_data_t *cur = obs_source_get_settings(src);
	obs_data_apply(cur, s);
	video_device_capture_info.update(obs_obj_get_data(src), cur);
	obs_data_release(cur);
}

static void explicit_backend_and_switching(void **state)
{
	UNUSED_PARAMETER(state);
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, VD_BACKEND, "fake_b");

	obs_source_t *src = obs_source_create(VIDEO_DEVICE_SOURCE_ID, "x", s, NULL);
	assert_int_equal(fake_b_state.creates, 1);

	/* Same backend: update in place. */
	obs_data_set_string(s, VD_DEVICE_ID, "fake_b-cam0");
	apply_update(src, s);
	assert_int_equal(fake_b_state.updates, 1);
	assert_string_equal(fake_b_state.last_device, "fake_b-cam0");

	/* Other backend: the old instance goes, a new one starts. */
	obs_data_set_string(s, VD_BACKEND, "fake_a");
	apply_update(src, s);
	assert_int_equal(fake_b_state.destroys, 1);
	assert_int_equal(fake_a_state.creates, 1);

	obs_source_release(src);
	obs_wait_for_destroy_queue();
	assert_int_equal(fake_a_state.destroys, 1);
	obs_data_release(s);
}

static void missing_device_stays_listed(void **state)
{
	UNUSED_PARAMETER(state);
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, VD_BACKEND, "fake_a");
	obs_data_set_string(s, VD_DEVICE_ID, "gone-device");
	obs_data_set_string(s, VD_DEVICE_NAME, "Old Camera");

	obs_source_t *src = obs_source_create(VIDEO_DEVICE_SOURCE_ID, "p", s, NULL);
	obs_properties_t *props = obs_source_properties(src);
	obs_property_t *list = obs_properties_get(props, VD_DEVICE_ID);
	assert_non_null(list);
	assert_int_equal(obs_property_list_item_count(list), 3);
	assert_string_equal(obs_property_list_item_string(list, 2), "gone-device");
	assert_true(obs_property_list_item_disabled(list, 2));
	assert_non_null(strstr(obs_property_list_item_name(list, 2), "Old Camera"));

	assert_non_null(obs_properties_get(props, VD_COLOR_SPACE));
	assert_null(obs_properties_get(props, VD_CAPTURE_AUDIO)); /* fake_a has no audio */

	obs_properties_destroy(props);
	obs_source_release(src);
	obs_data_release(s);
}

static void deactivate_when_hidden_releases_backend(void **state)
{
	UNUSED_PARAMETER(state);
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, VD_BACKEND, "fake_a");

	obs_source_t *src = obs_source_create(VIDEO_DEVICE_SOURCE_ID, "h", s, NULL);
	void *data = obs_obj_get_data(src);
	assert_int_equal(fake_a_state.creates, 1);

	/* Off: hiding keeps the backend. */
	video_device_capture_info.hide(data);
	video_device_capture_info.show(data);
	assert_int_equal(fake_a_state.destroys, 0);

	/* On: fake_a has no hide of its own, so hiding destroys it, an update
	 * while hidden waits, and showing creates it again. */
	obs_data_set_bool(s, VD_DEACTIVATE_WHEN_HIDDEN, true);
	apply_update(src, s);
	video_device_capture_info.hide(data);
	assert_int_equal(fake_a_state.destroys, 1);
	apply_update(src, s);
	assert_int_equal(fake_a_state.creates, 1);
	video_device_capture_info.show(data);
	assert_int_equal(fake_a_state.creates, 2);

	obs_source_release(src);
	obs_wait_for_destroy_queue();
	assert_int_equal(fake_a_state.destroys, 2);
	obs_data_release(s);
}

/* Sources start out hidden. With the option on, the device isn't opened
 * until the source shows. */
static void created_hidden_waits_for_show(void **state)
{
	UNUSED_PARAMETER(state);
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, VD_BACKEND, "fake_a");
	obs_data_set_bool(s, VD_DEACTIVATE_WHEN_HIDDEN, true);

	obs_source_t *src = obs_source_create(VIDEO_DEVICE_SOURCE_ID, "h", s, NULL);
	void *data = obs_obj_get_data(src);
	assert_int_equal(fake_a_state.creates, 0);

	video_device_capture_info.show(data);
	assert_int_equal(fake_a_state.creates, 1);
	video_device_capture_info.hide(data);
	assert_int_equal(fake_a_state.destroys, 1);

	obs_source_release(src);
	obs_wait_for_destroy_queue();
	assert_int_equal(fake_a_state.destroys, 1);
	obs_data_release(s);
}

static void created_hidden_pauses(void **state)
{
	UNUSED_PARAMETER(state);
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, VD_BACKEND, "pausable");
	obs_data_set_bool(s, VD_DEACTIVATE_WHEN_HIDDEN, true);

	obs_source_t *src = obs_source_create(VIDEO_DEVICE_SOURCE_ID, "h", s, NULL);
	void *data = obs_obj_get_data(src);
	assert_int_equal(fake_b_state.creates, 1);
	assert_int_equal(fake_b_state.hides, 1);

	/* Hiding again doesn't hide twice; showing resumes once. */
	video_device_capture_info.hide(data);
	assert_int_equal(fake_b_state.hides, 1);
	video_device_capture_info.show(data);
	video_device_capture_info.show(data);
	assert_int_equal(fake_b_state.shows, 1);
	assert_int_equal(fake_b_state.destroys, 0);

	obs_source_release(src);
	obs_data_release(s);
}

/* Turning the option on while hidden lets go of the device right away, and
 * turning it off again takes it back. */
static void toggling_while_hidden(void **state)
{
	UNUSED_PARAMETER(state);
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, VD_BACKEND, "fake_a");

	obs_source_t *src = obs_source_create(VIDEO_DEVICE_SOURCE_ID, "h", s, NULL);
	assert_int_equal(fake_a_state.creates, 1);

	obs_data_set_bool(s, VD_DEACTIVATE_WHEN_HIDDEN, true);
	apply_update(src, s);
	assert_int_equal(fake_a_state.destroys, 1);

	obs_data_set_bool(s, VD_DEACTIVATE_WHEN_HIDDEN, false);
	apply_update(src, s);
	assert_int_equal(fake_a_state.creates, 2);

	obs_source_release(src);
	obs_wait_for_destroy_queue();
	assert_int_equal(fake_a_state.destroys, 2);
	obs_data_release(s);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test_setup_teardown(control_keys_and_colors, setup, teardown),
		cmocka_unit_test_setup_teardown(auto_prefers_device_owner_then_first, setup, teardown),
		cmocka_unit_test_setup_teardown(explicit_backend_and_switching, setup, teardown),
		cmocka_unit_test_setup_teardown(missing_device_stays_listed, setup, teardown),
		cmocka_unit_test_setup_teardown(deactivate_when_hidden_releases_backend, setup, teardown),
		cmocka_unit_test_setup_teardown(created_hidden_waits_for_show, setup, teardown),
		cmocka_unit_test_setup_teardown(created_hidden_pauses, setup, teardown),
		cmocka_unit_test_setup_teardown(toggling_while_hidden, setup, teardown),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
