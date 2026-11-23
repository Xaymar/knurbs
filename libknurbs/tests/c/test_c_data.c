/* Exercises the C API for data and properties from C11. */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#include "knurbs/c/data.h"
#include "knurbs/c/object.h"
#include "knurbs/c/properties.h"
#include "knurbs/c/result.h"

static void test_c_data_roundtrip(void** state)
{
	(void)state;
	knurbs_data_t* data  = NULL;
	knurbs_data_t* inner = NULL;
	assert_int_equal(knurbs_data_create(&data), KNURBS_OK);
	assert_int_equal(knurbs_data_create(&inner), KNURBS_OK);
	assert_int_equal(knurbs_data_set_int(inner, "depth", 2), KNURBS_OK);

	assert_int_equal(knurbs_data_set_int(data, "count", INT64_C(-9007199254740993)), KNURBS_OK);
	assert_int_equal(knurbs_data_set_string(data, "name", "caf\xc3\xa9"), KNURBS_OK);
	assert_int_equal(knurbs_data_set_bool(data, "on", true), KNURBS_OK);
	assert_int_equal(knurbs_data_set_double(data, "ratio", 0.25), KNURBS_OK);
	assert_int_equal(knurbs_data_set_object(data, "inner", inner), KNURBS_OK);

	char* json = NULL;
	assert_int_equal(knurbs_data_to_json(data, false, &json), KNURBS_OK);
	assert_non_null(json);

	knurbs_data_t* parsed = NULL;
	assert_int_equal(knurbs_data_from_json(json, &parsed), KNURBS_OK);
	knurbs_free(json);

	int64_t count = 0;
	assert_int_equal(knurbs_data_get_int(parsed, "count", &count), KNURBS_OK);
	assert_true(count == INT64_C(-9007199254740993));
	char* name = NULL;
	assert_int_equal(knurbs_data_get_string(parsed, "name", &name), KNURBS_OK);
	assert_string_equal(name, "caf\xc3\xa9");
	knurbs_free(name);
	bool on = false;
	assert_int_equal(knurbs_data_get_bool(parsed, "on", &on), KNURBS_OK);
	assert_true(on);
	double ratio = 0;
	assert_int_equal(knurbs_data_get_double(parsed, "ratio", &ratio), KNURBS_OK);
	assert_true(ratio == 0.25);

	knurbs_data_t* parsed_inner = NULL;
	assert_int_equal(knurbs_data_get_object(parsed, "inner", &parsed_inner), KNURBS_OK);
	assert_non_null(parsed_inner);
	assert_int_equal(knurbs_data_get_int(parsed_inner, "depth", &count), KNURBS_OK);
	assert_int_equal(count, 2);

	/* Pretty output parses to the same tree; a clone is a separate tree with the same JSON */
	char* pretty  = NULL;
	char* compact = NULL;
	assert_int_equal(knurbs_data_to_json(parsed, true, &pretty), KNURBS_OK);
	assert_non_null(strchr(pretty, '\n'));
	knurbs_data_t* clone = NULL;
	assert_int_equal(knurbs_data_clone(parsed, &clone), KNURBS_OK);
	assert_true(clone != parsed);
	assert_int_equal(knurbs_data_to_json(clone, true, &compact), KNURBS_OK);
	assert_string_equal(pretty, compact);
	knurbs_free(pretty);
	knurbs_free(compact);

	knurbs_release((knurbs_object_t*)clone);
	knurbs_release((knurbs_object_t*)parsed_inner);
	knurbs_release((knurbs_object_t*)parsed);
	knurbs_release((knurbs_object_t*)inner);
	knurbs_release((knurbs_object_t*)data);
}

static void test_c_data_errors(void** state)
{
	(void)state;
	knurbs_data_t* data = NULL;
	assert_int_equal(knurbs_data_create(&data), KNURBS_OK);

	/* A missing key, and the out parameter cleared on every failure */
	int64_t number = 7;
	assert_int_equal(knurbs_data_get_int(data, "missing", &number), KNURBS_ERROR_NOT_FOUND);
	assert_int_equal(number, 0);
	assert_true(strlen(knurbs_last_error()) > 0);

	char* text = (char*)&number;
	assert_int_equal(knurbs_data_get_string(data, "missing", &text), KNURBS_ERROR_NOT_FOUND);
	assert_null(text);

	knurbs_data_t* object = (knurbs_data_t*)(void*)&number;
	assert_int_equal(knurbs_data_get_object(data, "missing", &object), KNURBS_ERROR_NOT_FOUND);
	assert_null(object);

	/* A bad key */
	assert_int_equal(knurbs_data_set_int(data, "Bad Key", 1), KNURBS_ERROR_INVALID_ARGUMENT);
	assert_int_equal(knurbs_data_set_bool(data, "", true), KNURBS_ERROR_INVALID_ARGUMENT);
	/* Getters do not check keys, they only find nothing */
	number = 7;
	assert_int_equal(knurbs_data_get_int(data, "Bad Key", &number), KNURBS_ERROR_NOT_FOUND);
	assert_int_equal(number, 0);

	/* A value of another type */
	assert_int_equal(knurbs_data_set_string(data, "name", "x"), KNURBS_OK);
	assert_int_equal(knurbs_data_get_int(data, "name", &number), KNURBS_ERROR_INVALID_ARGUMENT);

	/* NULL arguments */
	bool flag = true;
	assert_int_equal(knurbs_data_has(NULL, "name", &flag), KNURBS_ERROR_INVALID_ARGUMENT);
	assert_false(flag);
	assert_int_equal(knurbs_data_has(data, NULL, &flag), KNURBS_ERROR_INVALID_ARGUMENT);
	assert_int_equal(knurbs_data_has(data, "name", NULL), KNURBS_ERROR_INVALID_ARGUMENT);
	assert_int_equal(knurbs_data_set_string(data, "name", NULL), KNURBS_ERROR_INVALID_ARGUMENT);
	assert_int_equal(knurbs_data_set_object(data, "inner", NULL), KNURBS_ERROR_INVALID_ARGUMENT);
	assert_int_equal(knurbs_data_create(NULL), KNURBS_ERROR_INVALID_ARGUMENT);
	assert_int_equal(knurbs_data_erase(NULL, "name"), KNURBS_ERROR_INVALID_ARGUMENT);

	/* Bad JSON */
	knurbs_data_t* parsed = (knurbs_data_t*)(void*)&number;
	assert_int_equal(knurbs_data_from_json("{not json", &parsed), KNURBS_ERROR_INVALID_ARGUMENT);
	assert_null(parsed);
	assert_int_equal(knurbs_data_from_json("[]", &parsed), KNURBS_ERROR_INVALID_ARGUMENT);
	assert_int_equal(knurbs_data_from_json(NULL, &parsed), KNURBS_ERROR_INVALID_ARGUMENT);

	/* A tree cannot contain itself */
	assert_int_equal(knurbs_data_set_object(data, "self", data), KNURBS_ERROR_INVALID_ARGUMENT);

	knurbs_free(NULL);
	knurbs_release((knurbs_object_t*)data);
}

static void test_c_data_defaults_and_keys(void** state)
{
	(void)state;
	knurbs_data_t* data = NULL;
	assert_int_equal(knurbs_data_create(&data), KNURBS_OK);

	char** keys  = (char**)(void*)&state;
	size_t count = 5;
	assert_int_equal(knurbs_data_keys(data, &keys, &count), KNURBS_OK);
	assert_null(keys);
	assert_int_equal(count, 0);

	assert_int_equal(knurbs_data_set_default_int(data, "volume", 10), KNURBS_OK);
	assert_int_equal(knurbs_data_set_string(data, "alpha", "a"), KNURBS_OK);
	assert_int_equal(knurbs_data_set_default_string(data, "beta", "b"), KNURBS_OK);

	bool flag = false;
	assert_int_equal(knurbs_data_has(data, "volume", &flag), KNURBS_OK);
	assert_true(flag);
	assert_int_equal(knurbs_data_has_user_value(data, "volume", &flag), KNURBS_OK);
	assert_false(flag);

	assert_int_equal(knurbs_data_keys(data, &keys, &count), KNURBS_OK);
	assert_int_equal(count, 3);
	assert_string_equal(keys[0], "alpha");
	assert_string_equal(keys[1], "beta");
	assert_string_equal(keys[2], "volume");
	knurbs_free(keys);

	int64_t number = 0;
	assert_int_equal(knurbs_data_set_int(data, "volume", 3), KNURBS_OK);
	assert_int_equal(knurbs_data_get_int(data, "volume", &number), KNURBS_OK);
	assert_int_equal(number, 3);
	assert_int_equal(knurbs_data_erase(data, "volume"), KNURBS_OK);
	assert_int_equal(knurbs_data_get_int(data, "volume", &number), KNURBS_OK);
	assert_int_equal(number, 10);
	assert_int_equal(knurbs_data_erase(data, "unknown"), KNURBS_OK);

	knurbs_release((knurbs_object_t*)data);
}

static void test_c_data_array(void** state)
{
	(void)state;
	knurbs_data_t*       data  = NULL;
	knurbs_data_array_t* array = NULL;
	assert_int_equal(knurbs_data_create(&data), KNURBS_OK);
	assert_int_equal(knurbs_data_array_create(&array), KNURBS_OK);

	for (int64_t i = 0; i < 3; i++) {
		knurbs_data_t* item = NULL;
		assert_int_equal(knurbs_data_create(&item), KNURBS_OK);
		assert_int_equal(knurbs_data_set_int(item, "index", i), KNURBS_OK);
		assert_int_equal(knurbs_data_array_push_back(array, item), KNURBS_OK);
		knurbs_release((knurbs_object_t*)item);
	}
	assert_int_equal(knurbs_data_set_array(data, "items", array), KNURBS_OK);

	size_t size = 0;
	assert_int_equal(knurbs_data_array_size(array, &size), KNURBS_OK);
	assert_int_equal(size, 3);
	assert_int_equal(knurbs_data_array_erase(array, 0), KNURBS_OK);

	knurbs_data_array_t* fetched = NULL;
	assert_int_equal(knurbs_data_get_array(data, "items", &fetched), KNURBS_OK);
	assert_ptr_equal(fetched, array);
	assert_int_equal(knurbs_data_array_size(fetched, &size), KNURBS_OK);
	assert_int_equal(size, 2);

	knurbs_data_t* item  = NULL;
	int64_t        index = 0;
	assert_int_equal(knurbs_data_array_at(fetched, 1, &item), KNURBS_OK);
	assert_int_equal(knurbs_data_get_int(item, "index", &index), KNURBS_OK);
	assert_int_equal(index, 2);
	knurbs_release((knurbs_object_t*)item);

	item = (knurbs_data_t*)(void*)&index;
	assert_int_equal(knurbs_data_array_at(fetched, 2, &item), KNURBS_ERROR_OUT_OF_RANGE);
	assert_null(item);
	assert_int_equal(knurbs_data_array_erase(fetched, 2), KNURBS_ERROR_OUT_OF_RANGE);
	assert_int_equal(knurbs_data_array_push_back(fetched, NULL), KNURBS_ERROR_INVALID_ARGUMENT);

	knurbs_release((knurbs_object_t*)fetched);
	knurbs_release((knurbs_object_t*)array);
	knurbs_release((knurbs_object_t*)data);
}

static int g_calls;
static int g_frees;
static int g_expected_param;
/* Checked by the tests after the call: an assertion that fails inside a callback would longjmp through C++ frames */
static bool g_callback_ok;

static bool on_click(knurbs_properties_t* properties, knurbs_property_t* property, void* param)
{
	g_calls++;
	char* id      = NULL;
	g_callback_ok = param == &g_expected_param && properties != NULL && knurbs_property_get_id(property, &id) == KNURBS_OK && id != NULL && strcmp(id, "reset") == 0;
	knurbs_free(id);
	return true;
}

static bool on_modified(knurbs_properties_t* properties, knurbs_property_t* property, knurbs_data_t* settings, void* param)
{
	g_calls++;
	int64_t value = 0;
	g_callback_ok = param == &g_expected_param && properties != NULL && property != NULL && knurbs_data_get_int(settings, "gain", &value) == KNURBS_OK;
	return value > 5;
}

static bool g_free_param_ok = true;

static void count_free(void* param)
{
	g_free_param_ok = g_free_param_ok && param == &g_expected_param;
	g_frees++;
}

/* Every read-only function takes a const handle, as a properties callback gets one (knurbs_type_info.properties). */
static void test_c_data_const_handles(void** state)
{
	(void)state;
	knurbs_data_t*       data  = NULL;
	knurbs_data_t*       inner = NULL;
	knurbs_data_array_t* array = NULL;
	assert_int_equal(knurbs_data_create(&data), KNURBS_OK);
	assert_int_equal(knurbs_data_create(&inner), KNURBS_OK);
	assert_int_equal(knurbs_data_array_create(&array), KNURBS_OK);
	assert_int_equal(knurbs_data_array_push_back(array, inner), KNURBS_OK);
	assert_int_equal(knurbs_data_set_bool(data, "on", true), KNURBS_OK);
	assert_int_equal(knurbs_data_set_int(data, "count", 3), KNURBS_OK);
	assert_int_equal(knurbs_data_set_double(data, "ratio", 0.5), KNURBS_OK);
	assert_int_equal(knurbs_data_set_string(data, "name", "x"), KNURBS_OK);
	assert_int_equal(knurbs_data_set_object(data, "inner", inner), KNURBS_OK);
	assert_int_equal(knurbs_data_set_array(data, "list", array), KNURBS_OK);

	const knurbs_data_t*       ro       = data;
	const knurbs_data_array_t* ro_array = array;
	bool                       flag     = false;
	int64_t                    count    = 0;
	double                     ratio    = 0;
	char*                      text     = NULL;
	knurbs_data_t*             object   = NULL;
	knurbs_data_array_t*       list     = NULL;
	char**                     keys     = NULL;
	size_t                     n        = 0;
	assert_int_equal(knurbs_data_has(ro, "on", &flag), KNURBS_OK);
	assert_true(flag);
	assert_int_equal(knurbs_data_has_user_value(ro, "on", &flag), KNURBS_OK);
	assert_true(flag);
	assert_int_equal(knurbs_data_get_bool(ro, "on", &flag), KNURBS_OK);
	assert_true(flag);
	assert_int_equal(knurbs_data_get_int(ro, "count", &count), KNURBS_OK);
	assert_int_equal(count, 3);
	assert_int_equal(knurbs_data_get_double(ro, "ratio", &ratio), KNURBS_OK);
	assert_true(ratio == 0.5);
	assert_int_equal(knurbs_data_get_string(ro, "name", &text), KNURBS_OK);
	assert_string_equal(text, "x");
	knurbs_free(text);
	assert_int_equal(knurbs_data_to_json(ro, false, &text), KNURBS_OK);
	assert_non_null(text);
	knurbs_free(text);
	assert_int_equal(knurbs_data_keys(ro, &keys, &n), KNURBS_OK);
	assert_int_equal(n, 6);
	knurbs_free(keys);
	assert_int_equal(knurbs_data_get_object(ro, "inner", &object), KNURBS_OK);
	assert_ptr_equal(object, inner);
	knurbs_release((knurbs_object_t*)object);
	assert_int_equal(knurbs_data_get_array(ro, "list", &list), KNURBS_OK);
	assert_ptr_equal(list, array);
	knurbs_release((knurbs_object_t*)list);
	assert_int_equal(knurbs_data_clone(ro, &object), KNURBS_OK);
	assert_non_null(object);
	knurbs_release((knurbs_object_t*)object);
	assert_int_equal(knurbs_data_array_size(ro_array, &n), KNURBS_OK);
	assert_int_equal(n, 1);
	assert_int_equal(knurbs_data_array_at(ro_array, 0, &object), KNURBS_OK);
	assert_ptr_equal(object, inner);
	knurbs_release((knurbs_object_t*)object);

	knurbs_release((knurbs_object_t*)array);
	knurbs_release((knurbs_object_t*)inner);
	knurbs_release((knurbs_object_t*)data);
}

static void test_c_properties_callback(void** state)
{
	(void)state;
	g_calls = 0;
	g_frees = 0;

	knurbs_properties_t* props = NULL;
	knurbs_property_t*   gain  = NULL;
	assert_int_equal(knurbs_properties_create(&props), KNURBS_OK);
	assert_int_equal(knurbs_properties_add_int(props, "gain", "Gain", -10, 30, 0.5, &gain), KNURBS_OK);

	assert_int_equal(knurbs_properties_set_modified(props, "gain", on_modified, &g_expected_param, count_free), KNURBS_OK);

	knurbs_data_t* settings = NULL;
	assert_int_equal(knurbs_data_create(&settings), KNURBS_OK);
	assert_int_equal(knurbs_data_set_int(settings, "gain", 9), KNURBS_OK);

	bool result = false;
	assert_int_equal(knurbs_properties_notify_modified(props, "gain", settings, &result), KNURBS_OK);
	assert_true(result);
	assert_int_equal(g_calls, 1);
	assert_true(g_callback_ok);

	assert_int_equal(knurbs_data_set_int(settings, "gain", 1), KNURBS_OK);
	assert_int_equal(knurbs_properties_notify_modified(props, "gain", settings, &result), KNURBS_OK);
	assert_false(result);
	assert_int_equal(g_calls, 2);

	/* No callback for this id: false, and not an error */
	result = true;
	assert_int_equal(knurbs_properties_notify_modified(props, "nothing", settings, &result), KNURBS_OK);
	assert_false(result);
	assert_int_equal(g_calls, 2);

	/* Replacing the callback frees the parameter of the first; destroying the properties frees the second */
	assert_int_equal(g_frees, 0);
	assert_int_equal(knurbs_properties_set_modified(props, "gain", on_modified, &g_expected_param, count_free), KNURBS_OK);
	assert_int_equal(g_frees, 1);

	/* A failing call frees the parameter at once; a NULL free_param is allowed */
	assert_int_equal(knurbs_properties_set_modified(props, "Bad Id", on_modified, &g_expected_param, count_free), KNURBS_ERROR_INVALID_ARGUMENT);
	assert_int_equal(g_frees, 2);
	/* A NULL callback clears it and frees the parameter at once */
	assert_int_equal(knurbs_properties_set_modified(props, "gain", on_modified, &g_expected_param, count_free), KNURBS_OK);
	assert_int_equal(knurbs_properties_set_modified(props, "gain", NULL, &g_expected_param, count_free), KNURBS_OK);
	assert_int_equal(g_frees, 5);
	result = true;
	assert_int_equal(knurbs_properties_notify_modified(props, "gain", settings, &result), KNURBS_OK);
	assert_false(result);
	assert_int_equal(g_calls, 2);
	assert_int_equal(knurbs_properties_set_modified(props, "other", on_modified, &g_expected_param, NULL), KNURBS_OK);

	knurbs_release((knurbs_object_t*)settings);
	knurbs_release((knurbs_object_t*)gain);
	knurbs_release((knurbs_object_t*)props);
	assert_int_equal(g_frees, 5);
	assert_true(g_free_param_ok);
}

static void test_c_properties_button_and_list(void** state)
{
	(void)state;
	g_calls = 0;
	g_frees = 0;

	knurbs_properties_t* props = NULL;
	assert_int_equal(knurbs_properties_create(&props), KNURBS_OK);

	knurbs_property_t* button = NULL;
	assert_int_equal(knurbs_properties_add_button(props, "reset", "Reset", on_click, &g_expected_param, count_free, &button), KNURBS_OK);
	bool result = false;
	assert_int_equal(knurbs_property_click(button, &result), KNURBS_OK);
	assert_true(result);
	assert_int_equal(g_calls, 1);
	assert_true(g_callback_ok);

	knurbs_property_kind kind = KNURBS_PROPERTY_NOTICE;
	assert_int_equal(knurbs_property_get_kind(button, &kind), KNURBS_OK);
	assert_int_equal(kind, KNURBS_PROPERTY_BUTTON);
	char* label = NULL;
	assert_int_equal(knurbs_property_get_label(button, &label), KNURBS_OK);
	assert_string_equal(label, "Reset");
	knurbs_free(label);

	/* A button with a NULL callback or a duplicate id is refused, and its parameter freed */
	knurbs_property_t* rejected = (knurbs_property_t*)(void*)&g_calls;
	assert_int_equal(knurbs_properties_add_button(props, "reset", "Again", on_click, &g_expected_param, count_free, &rejected), KNURBS_ERROR_ALREADY_EXISTS);
	assert_null(rejected);
	assert_int_equal(g_frees, 1);
	assert_int_equal(knurbs_properties_add_button(props, "other", "Other", NULL, &g_expected_param, count_free, &rejected), KNURBS_ERROR_INVALID_ARGUMENT);
	assert_int_equal(g_frees, 2);

	/* A list property, a group and a notice */
	knurbs_property_t* choice = NULL;
	assert_int_equal(knurbs_properties_add_list(props, "choice", "Choice", &choice), KNURBS_OK);
	assert_int_equal(knurbs_property_add_item(choice, "One", "one"), KNURBS_OK);
	assert_int_equal(knurbs_property_add_item(choice, "Two", "two"), KNURBS_OK);
	size_t items = 0;
	assert_int_equal(knurbs_property_item_count(choice, &items), KNURBS_OK);
	assert_int_equal(items, 2);
	/* Wrong kind */
	double bound = 1;
	assert_int_equal(knurbs_property_get_min(choice, &bound), KNURBS_ERROR_INVALID_ARGUMENT);
	assert_true(bound == 0);

	knurbs_properties_t* inner = NULL;
	assert_int_equal(knurbs_properties_create(&inner), KNURBS_OK);
	knurbs_property_t* gain = NULL;
	assert_int_equal(knurbs_properties_add_float(inner, "gain", "Gain", -1, 1, 0.1, &gain), KNURBS_OK);
	knurbs_property_t* group = NULL;
	assert_int_equal(knurbs_properties_add_group(props, "audio", "Audio", inner, &group), KNURBS_OK);
	assert_int_equal(knurbs_properties_add_group(props, "empty", "Empty", NULL, &rejected), KNURBS_ERROR_INVALID_ARGUMENT);
	knurbs_properties_t* fetched = NULL;
	assert_int_equal(knurbs_property_get_group(group, &fetched), KNURBS_OK);
	assert_ptr_equal(fetched, inner);
	knurbs_release((knurbs_object_t*)fetched);
	assert_int_equal(knurbs_property_get_group(choice, &fetched), KNURBS_ERROR_INVALID_ARGUMENT);
	assert_null(fetched);

	knurbs_property_t* notice = NULL;
	assert_int_equal(knurbs_properties_add_notice(props, "info", "Be careful", &notice), KNURBS_OK);

	/* find searches groups; a miss is not an error */
	knurbs_property_t* found = NULL;
	assert_int_equal(knurbs_properties_find(props, "gain", &found), KNURBS_OK);
	assert_ptr_equal(found, gain);
	knurbs_release((knurbs_object_t*)found);
	found = (knurbs_property_t*)(void*)&g_calls;
	assert_int_equal(knurbs_properties_find(props, "missing", &found), KNURBS_OK);
	assert_null(found);

	double min = 0, max = 0, step = 0;
	assert_int_equal(knurbs_property_get_min(gain, &min), KNURBS_OK);
	assert_int_equal(knurbs_property_get_max(gain, &max), KNURBS_OK);
	assert_int_equal(knurbs_property_get_step(gain, &step), KNURBS_OK);
	assert_true(min == -1 && max == 1 && step == 0.1);
	bool flag = false;
	assert_int_equal(knurbs_property_set_enabled(gain, false), KNURBS_OK);
	assert_int_equal(knurbs_property_get_enabled(gain, &flag), KNURBS_OK);
	assert_false(flag);
	assert_int_equal(knurbs_property_set_visible(gain, false), KNURBS_OK);
	assert_int_equal(knurbs_property_get_visible(gain, &flag), KNURBS_OK);
	assert_false(flag);

	/* list returns the properties of this level in insertion order */
	knurbs_property_t** list  = NULL;
	size_t              count = 0;
	assert_int_equal(knurbs_properties_list(props, &list, &count), KNURBS_OK);
	assert_int_equal(count, 4);
	const char* expected[] = {"reset", "choice", "audio", "info"};
	for (size_t i = 0; i < count; i++) {
		char* id = NULL;
		assert_int_equal(knurbs_property_get_id(list[i], &id), KNURBS_OK);
		assert_string_equal(id, expected[i]);
		knurbs_free(id);
		knurbs_release((knurbs_object_t*)list[i]);
	}
	knurbs_free(list);

	knurbs_properties_t* empty = NULL;
	assert_int_equal(knurbs_properties_create(&empty), KNURBS_OK);
	list  = (knurbs_property_t**)(void*)&g_calls;
	count = 9;
	assert_int_equal(knurbs_properties_list(empty, &list, &count), KNURBS_OK);
	assert_null(list);
	assert_int_equal(count, 0);
	knurbs_release((knurbs_object_t*)empty);

	assert_int_equal(knurbs_property_click(NULL, &result), KNURBS_ERROR_INVALID_ARGUMENT);
	assert_false(result);

	knurbs_release((knurbs_object_t*)notice);
	knurbs_release((knurbs_object_t*)group);
	knurbs_release((knurbs_object_t*)gain);
	knurbs_release((knurbs_object_t*)inner);
	knurbs_release((knurbs_object_t*)choice);
	knurbs_release((knurbs_object_t*)button);
	assert_int_equal(g_frees, 2);
	knurbs_release((knurbs_object_t*)props);
	assert_int_equal(g_frees, 3);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_c_data_roundtrip), cmocka_unit_test(test_c_data_errors), cmocka_unit_test(test_c_data_defaults_and_keys), cmocka_unit_test(test_c_data_array), cmocka_unit_test(test_c_data_const_handles), cmocka_unit_test(test_c_properties_callback), cmocka_unit_test(test_c_properties_button_and_list),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
