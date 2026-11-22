#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>

#include <obs.h>

/* Every encoder's settings start with the notice that Knurbs grants no patent
 * licenses, whatever the encoder's own properties are. */

static const char *encoder_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "test";
}

static void *encoder_create(obs_data_t *settings, obs_encoder_t *encoder)
{
	UNUSED_PARAMETER(settings);
	UNUSED_PARAMETER(encoder);
	return (void *)1;
}

static void encoder_destroy(void *data)
{
	UNUSED_PARAMETER(data);
}

static bool encoder_encode(void *data, struct encoder_frame *frame, struct encoder_packet *packet,
			   bool *received_packet)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(frame);
	UNUSED_PARAMETER(packet);
	*received_packet = false;
	return true;
}

static obs_properties_t *with_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_int(props, "bitrate", "Bitrate", 0, 100000, 1);
	obs_properties_add_bool(props, "lossless", "Lossless");
	return props;
}

static struct obs_encoder_info with_props_info = {
	.id = "com.example.test.with-properties",
	.type = OBS_ENCODER_VIDEO,
	.codec = "hevc",
	.get_name = encoder_name,
	.create = encoder_create,
	.destroy = encoder_destroy,
	.encode = encoder_encode,
	.get_properties = with_properties,
};

static struct obs_encoder_info without_props_info = {
	.id = "com.example.test.without-properties",
	.type = OBS_ENCODER_VIDEO,
	.codec = "av1",
	.get_name = encoder_name,
	.create = encoder_create,
	.destroy = encoder_destroy,
	.encode = encoder_encode,
};

static int setup(void **state)
{
	UNUSED_PARAMETER(state);
	if (!obs_startup("en-US", NULL, NULL))
		return -1;
	obs_register_encoder(&with_props_info);
	obs_register_encoder(&without_props_info);
	return 0;
}

static int teardown(void **state)
{
	UNUSED_PARAMETER(state);
	obs_shutdown();
	return 0;
}

static void assert_notice_first(obs_properties_t *props)
{
	assert_non_null(props);
	obs_property_t *p = obs_properties_first(props);
	assert_non_null(p);
	assert_string_equal(obs_property_name(p), OBS_ENCODER_PATENT_NOTICE);
	assert_int_equal(obs_property_get_type(p), OBS_PROPERTY_TEXT);
	assert_int_equal(obs_property_text_type(p), OBS_TEXT_INFO);
	assert_int_equal(obs_property_text_info_type(p), OBS_TEXT_INFO_WARNING);
	assert_string_equal(obs_property_description(p), OBS_ENCODER_PATENT_NOTICE_TEXT);
}

static void type_properties_start_with_notice(void **state)
{
	UNUSED_PARAMETER(state);
	obs_properties_t *props = obs_get_encoder_properties(with_props_info.id);
	assert_notice_first(props);

	/* The encoder's own properties follow, in their order. */
	obs_property_t *p = obs_properties_first(props);
	assert_true(obs_property_next(&p));
	assert_string_equal(obs_property_name(p), "bitrate");
	assert_true(obs_property_next(&p));
	assert_string_equal(obs_property_name(p), "lossless");
	assert_false(obs_property_next(&p));
	obs_properties_destroy(props);
}

static void encoder_without_properties_gets_notice(void **state)
{
	UNUSED_PARAMETER(state);
	obs_properties_t *props = obs_get_encoder_properties(without_props_info.id);
	assert_notice_first(props);
	obs_property_t *p = obs_properties_first(props);
	assert_false(obs_property_next(&p));
	obs_properties_destroy(props);
}

static void instance_properties_start_with_notice(void **state)
{
	UNUSED_PARAMETER(state);
	obs_encoder_t *with = obs_video_encoder_create(with_props_info.id, "with", NULL, NULL);
	obs_encoder_t *without = obs_video_encoder_create(without_props_info.id, "without", NULL, NULL);
	assert_non_null(with);
	assert_non_null(without);

	obs_properties_t *props = obs_encoder_properties(with);
	assert_notice_first(props);
	assert_non_null(obs_properties_get(props, "bitrate"));
	obs_properties_destroy(props);

	props = obs_encoder_properties(without);
	assert_notice_first(props);
	obs_properties_destroy(props);

	obs_encoder_release(with);
	obs_encoder_release(without);
}

static void unknown_encoder_has_no_properties(void **state)
{
	UNUSED_PARAMETER(state);
	assert_null(obs_get_encoder_properties("com.example.test.missing"));
	assert_null(obs_encoder_properties(NULL));
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(type_properties_start_with_notice),
		cmocka_unit_test(encoder_without_properties_gets_notice),
		cmocka_unit_test(instance_properties_start_with_notice),
		cmocka_unit_test(unknown_encoder_has_no_properties),
	};

	return cmocka_run_group_tests(tests, setup, teardown);
}
