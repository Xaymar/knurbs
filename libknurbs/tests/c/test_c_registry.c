/* Exercises the C API for the registry and C modules from C11, with the fake modules of tests/CMakeLists.txt. */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <cmocka.h>

#include "knurbs/c/core.h"
#include "knurbs/c/data.h"
#include "knurbs/c/module.h"
#include "knurbs/c/object.h"
#include "knurbs/c/registry.h"
#include "knurbs/c/result.h"

/* A core with the fake modules loaded, shut down and released by teardown. */
static int setup_core(void **state)
{
	knurbs_core_t *core = NULL;
	if (knurbs_core_create("config", "cache", &core) != KNURBS_OK) {
		return -1;
	}
	const char *dirs[] = {KNURBS_TEST_MODULES_DIR};
	if (knurbs_core_load_modules(core, dirs, 1) != KNURBS_OK) {
		knurbs_release((knurbs_object_t *)core);
		return -1;
	}
	*state = core;
	return 0;
}

static int teardown_core(void **state)
{
	knurbs_core_t *core = *state;
	knurbs_core_shutdown(core);
	knurbs_release((knurbs_object_t *)core);
	/* The marker files the fake modules leave in the working directory. */
	const char *const markers[] = {"fake-ok.load", "fake-ok.post_load", "fake-ok.unload", "fake-throws.load",
				       "fake-c.load",  "fake-c.post_load",  "fake-c.unload",  "fake-c.free_param"};
	for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); ++i) {
		remove(markers[i]);
	}
	return 0;
}

static void test_c_module_loads(void **state)
{
	knurbs_core_t *core = *state;
	knurbs_type_t *type = knurbs_registry_find(core, "com.example.fake.c");
	assert_non_null(type);

	knurbs_data_t *settings = NULL;
	assert_int_equal(knurbs_data_create(&settings), KNURBS_OK);
	assert_int_equal(knurbs_type_defaults(type, settings), KNURBS_OK);
	int64_t gain = 0;
	assert_int_equal(knurbs_data_get_int(settings, "gain", &gain), KNURBS_OK);
	assert_int_equal(gain, 5);

	/* A C++ module's type is found through the C API, too. */
	knurbs_type_t *cpp_type = knurbs_registry_find(core, "com.example.fake.ok");
	assert_non_null(cpp_type);

	knurbs_release((knurbs_object_t *)cpp_type);
	knurbs_release((knurbs_object_t *)settings);
	knurbs_release((knurbs_object_t *)type);
}

static void test_c_registry_find_missing(void **state)
{
	knurbs_core_t *core = *state;
	assert_null(knurbs_registry_find(core, "com.example.none"));
	assert_null(knurbs_registry_find(core, NULL));
	assert_null(knurbs_registry_find(NULL, "com.example.fake.c"));
}

static void test_c_alias(void **state)
{
	knurbs_core_t *core = *state;
	knurbs_type_t *by_alias = knurbs_registry_find(core, "fake_c_old");
	knurbs_type_t *by_id = knurbs_registry_find(core, "com.example.fake.c");
	assert_non_null(by_alias);
	assert_ptr_equal(by_alias, by_id);
	knurbs_release((knurbs_object_t *)by_id);
	knurbs_release((knurbs_object_t *)by_alias);
}

static void test_c_type_defaults_checks_arguments(void **state)
{
	knurbs_core_t *core = *state;
	knurbs_type_t *type = knurbs_registry_find(core, "com.example.fake.c");
	knurbs_data_t *settings = NULL;
	assert_int_equal(knurbs_data_create(&settings), KNURBS_OK);
	assert_int_equal(knurbs_type_defaults(NULL, settings), KNURBS_ERROR_INVALID_ARGUMENT);
	assert_int_equal(knurbs_type_defaults(type, NULL), KNURBS_ERROR_INVALID_ARGUMENT);
	knurbs_release((knurbs_object_t *)settings);
	knurbs_release((knurbs_object_t *)type);
}

static void test_c_load_modules_checks_arguments(void **state)
{
	knurbs_core_t *core = *state;
	const char *dirs[] = {NULL};
	assert_int_equal(knurbs_core_load_modules(NULL, NULL, 0), KNURBS_ERROR_INVALID_ARGUMENT);
	assert_int_equal(knurbs_core_load_modules(core, NULL, 1), KNURBS_ERROR_INVALID_ARGUMENT);
	assert_int_equal(knurbs_core_load_modules(core, dirs, 1), KNURBS_ERROR_INVALID_ARGUMENT);
	/* Nothing to search is fine. */
	assert_int_equal(knurbs_core_load_modules(core, NULL, 0), KNURBS_OK);
	assert_null(knurbs_module_core(NULL));
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test_setup_teardown(test_c_module_loads, setup_core, teardown_core),
		cmocka_unit_test_setup_teardown(test_c_registry_find_missing, setup_core, teardown_core),
		cmocka_unit_test_setup_teardown(test_c_alias, setup_core, teardown_core),
		cmocka_unit_test_setup_teardown(test_c_type_defaults_checks_arguments, setup_core, teardown_core),
		cmocka_unit_test_setup_teardown(test_c_load_modules_checks_arguments, setup_core, teardown_core),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
