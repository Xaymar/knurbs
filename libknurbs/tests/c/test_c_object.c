/* Exercises the C API from C11. It includes every C header, so they all get compiled as C. */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#include "knurbs/c/core.h"
#include "knurbs/c/export.h"
#include "knurbs/c/log.h"
#include "knurbs/c/module.h"
#include "knurbs/c/object.h"
#include "knurbs/c/result.h"

static void test_c_core_lifecycle(void** state)
{
	(void)state;
	knurbs_core_t* core = NULL;
	assert_int_equal(knurbs_core_create("config", "cache", &core), KNURBS_OK);
	assert_non_null(core);

	knurbs_logger_t* logger = knurbs_core_logger(core);
	assert_non_null(logger);
	assert_int_equal(knurbs_log_write(logger, KNURBS_LOG_INFO, "hello from C"), KNURBS_OK);
	knurbs_release((knurbs_object_t*)logger);

	knurbs_core_shutdown(core);
	knurbs_core_shutdown(core);
	knurbs_release((knurbs_object_t*)core);
}

static void test_c_error_mapping(void** state)
{
	(void)state;
	knurbs_core_t* out = NULL;
	assert_int_equal(knurbs_core_create(NULL, NULL, &out), KNURBS_ERROR_INVALID_ARGUMENT);
	assert_null(out);
	assert_true(strlen(knurbs_last_error()) > 0);

	assert_int_equal(knurbs_core_create("config", "cache", NULL), KNURBS_ERROR_INVALID_ARGUMENT);
	/* Empty paths are rejected by the core itself, which throws invalid_argument */
	assert_int_equal(knurbs_core_create("", "", &out), KNURBS_ERROR_INVALID_ARGUMENT);
	assert_null(out);
	assert_true(strlen(knurbs_last_error()) > 0);
}

static void test_c_core_create_clears_out(void** state)
{
	(void)state;
	int            marker = 0;
	knurbs_core_t* out    = (knurbs_core_t*)(void*)&marker;
	assert_int_equal(knurbs_core_create("", "", &out), KNURBS_ERROR_INVALID_ARGUMENT);
	assert_null(out);

	out = (knurbs_core_t*)(void*)&marker;
	assert_int_equal(knurbs_core_create(NULL, "cache", &out), KNURBS_ERROR_INVALID_ARGUMENT);
	assert_null(out);
}

static void test_c_null_and_levels(void** state)
{
	(void)state;
	knurbs_ref(NULL);
	knurbs_release(NULL);
	knurbs_core_shutdown(NULL);
	assert_null(knurbs_core_logger(NULL));
	assert_null(knurbs_weak_get(NULL));
	assert_null(knurbs_weak_lock(NULL));
	knurbs_weak_release(NULL);
	assert_int_equal(knurbs_log_write(NULL, KNURBS_LOG_INFO, "x"), KNURBS_ERROR_INVALID_ARGUMENT);

	knurbs_core_t* core = NULL;
	assert_int_equal(knurbs_core_create("config", "cache", &core), KNURBS_OK);
	knurbs_logger_t* logger = knurbs_core_logger(core);
	assert_non_null(logger);
	assert_int_equal(knurbs_log_write(logger, KNURBS_LOG_INFO, NULL), KNURBS_ERROR_INVALID_ARGUMENT);
	assert_int_equal(knurbs_log_write(logger, -1, "x"), KNURBS_ERROR_INVALID_ARGUMENT);
	assert_int_equal(knurbs_log_write(logger, 4, "x"), KNURBS_ERROR_INVALID_ARGUMENT);
	assert_int_equal(knurbs_log_write(logger, KNURBS_LOG_ERROR, "ok"), KNURBS_OK);
	knurbs_release((knurbs_object_t*)logger);
	knurbs_core_shutdown(core);
	knurbs_release((knurbs_object_t*)core);
}

static void test_c_weak(void** state)
{
	(void)state;
	knurbs_core_t* core = NULL;
	assert_int_equal(knurbs_core_create("config", "cache", &core), KNURBS_OK);

	knurbs_weak_t* weak = knurbs_weak_get((knurbs_object_t*)core);
	assert_non_null(weak);

	knurbs_object_t* locked = knurbs_weak_lock(weak);
	assert_ptr_equal(locked, (knurbs_object_t*)core);
	knurbs_release(locked);

	knurbs_core_shutdown(core);
	knurbs_release((knurbs_object_t*)core);
	assert_null(knurbs_weak_lock(weak));
	knurbs_weak_release(weak);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_c_core_lifecycle), cmocka_unit_test(test_c_error_mapping), cmocka_unit_test(test_c_core_create_clears_out), cmocka_unit_test(test_c_null_and_levels), cmocka_unit_test(test_c_weak),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
