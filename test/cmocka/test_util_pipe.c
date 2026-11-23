#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-test.h"

#include <string.h>

#include <util/pipe.h>

static void args_test(void **state)
{
	UNUSED_PARAMETER(state);

	os_process_args_t *args = os_process_args_create("prog");
	os_process_args_add_arg(args, "plain");
	os_process_args_add_argf(args, "num=%d", 5);
	/* Regression: an empty formatted argument used to become the argv terminator */
	os_process_args_add_argf(args, "%s", "");
	os_process_args_add_arg(args, "last");

	assert_int_equal(os_process_args_get_argc(args), 5);
	char **argv = os_process_args_get_argv(args);
	assert_string_equal(argv[0], "prog");
	assert_string_equal(argv[1], "plain");
	assert_string_equal(argv[2], "num=5");
	assert_string_equal(argv[3], "");
	assert_string_equal(argv[4], "last");
	assert_null(argv[5]);

	os_process_args_destroy(args);
	os_process_args_destroy(NULL);
}

static size_t read_all(os_process_pipe_t *pp, char *buf, size_t size, bool err)
{
	size_t total = 0, got;
	while (total < size - 1 && (got = err ? os_process_pipe_read_err(pp, (uint8_t *)buf + total, size - 1 - total)
					      : os_process_pipe_read(pp, (uint8_t *)buf + total, size - 1 - total)) > 0)
		total += got;
	buf[total] = 0;
	return total;
}

static void read_pipe_test(void **state)
{
	UNUSED_PARAMETER(state);

#ifdef _WIN32
	os_process_pipe_t *pp = os_process_pipe_create("cmd.exe /c echo out& echo err 1>&2& exit 3", "r");
#else
	os_process_pipe_t *pp = os_process_pipe_create("echo out; echo err 1>&2; exit 3", "r");
#endif
	assert_non_null(pp);
	char buf[256];
	read_all(pp, buf, sizeof(buf), false);
	assert_non_null(strstr(buf, "out"));
	read_all(pp, buf, sizeof(buf), true);
	assert_non_null(strstr(buf, "err"));
	/* a read pipe cannot be written to */
	assert_int_equal(os_process_pipe_write(pp, (const uint8_t *)"x", 1), 0);
	assert_int_equal(os_process_pipe_destroy(pp), 3);
}

static void write_pipe_test(void **state)
{
	UNUSED_PARAMETER(state);

	/* the filter exits with 0 only if it read the line written to its stdin */
#ifdef _WIN32
	os_process_args_t *args = os_process_args_create("cmd.exe");
	os_process_args_add_arg(args, "/c");
	os_process_args_add_arg(args, "findstr x >NUL");
	const char *line = "x\r\n";
#else
	os_process_args_t *args = os_process_args_create("/bin/sh");
	os_process_args_add_arg(args, "-c");
	os_process_args_add_arg(args, "grep x >/dev/null");
	const char *line = "x\n";
#endif
	os_process_pipe_t *pp = os_process_pipe_create2(args, "w");
	os_process_args_destroy(args);
	assert_non_null(pp);

	assert_int_equal(os_process_pipe_write(pp, (const uint8_t *)line, strlen(line)), strlen(line));
	uint8_t byte;
	assert_int_equal(os_process_pipe_read(pp, &byte, 1), 0);
	assert_int_equal(os_process_pipe_destroy(pp), 0);
}

static void quoting_test(void **state)
{
	UNUSED_PARAMETER(state);

	/* arguments with spaces, quotes, tabs and trailing backslashes survive the command line */
#ifdef _WIN32
	os_process_args_t *args = os_process_args_create("cmd.exe");
	os_process_args_add_arg(args, "/c");
	os_process_args_add_arg(args, "echo");
#else
	/* argv is passed to the program as it is; the shell prints each argument in brackets */
	os_process_args_t *args = os_process_args_create("/bin/sh");
	os_process_args_add_arg(args, "-c");
	os_process_args_add_arg(args, "printf '[%s]' \"$@\"");
	os_process_args_add_arg(args, "sh");
#endif
	os_process_args_add_arg(args, "a b");
	os_process_args_add_arg(args, "q\"uote");
	os_process_args_add_arg(args, "back\\\\\"slash");
	os_process_args_add_arg(args, "tab\there\\");
	os_process_args_add_arg(args, "");
	os_process_pipe_t *pp = os_process_pipe_create2(args, "r");
	os_process_args_destroy(args);
	assert_non_null(pp);

	char buf[256];
	read_all(pp, buf, sizeof(buf), false);
#ifdef _WIN32
	/* cmd's echo prints the raw command line */
	assert_non_null(strstr(buf, "\"a b\""));
	assert_non_null(strstr(buf, "q\\\"uote"));
#else
	assert_string_equal(buf, "[a b][q\"uote][back\\\\\"slash][tab\there\\][]");
#endif
	assert_int_equal(os_process_pipe_destroy(pp), 0);
}

static void error_test(void **state)
{
	UNUSED_PARAMETER(state);

#ifdef _WIN32
	const char *shell = "cmd.exe";
	const char *missing = "no-such-program-for-obs-tests.exe";
#else
	const char *shell = "/bin/sh";
	const char *missing = "/no-such-dir/no-such-program-for-obs-tests";
#endif
	assert_null(os_process_pipe_create(NULL, "r"));
	assert_null(os_process_pipe_create(shell, NULL));
	assert_null(os_process_pipe_create(shell, "x"));
#ifdef _WIN32
	assert_null(os_process_pipe_create(missing, "r"));
#else
	/* the command line runs through /bin/sh, which starts and reports a missing program as exit code 127 */
	os_process_pipe_t *pp = os_process_pipe_create(missing, "r");
	assert_non_null(pp);
	/* read the shell's complaint first: destroy closes stderr, and a shell writing to it then dies of SIGPIPE */
	char buf[256];
	read_all(pp, buf, sizeof(buf), true);
	assert_non_null(strstr(buf, "no-such-program-for-obs-tests"));
	assert_int_equal(os_process_pipe_destroy(pp), 127);

	/* Regression: a process killed by a signal has no exit code; destroy returned its pid */
	pp = os_process_pipe_create("kill -9 $$", "r");
	assert_non_null(pp);
	assert_int_equal(os_process_pipe_destroy(pp), -1);
#endif
	os_process_args_t *args = os_process_args_create(missing);
	assert_null(os_process_pipe_create2(args, "r"));
	assert_null(os_process_pipe_create2(args, "x"));
	os_process_args_destroy(args);

	uint8_t byte;
	assert_int_equal(os_process_pipe_read(NULL, &byte, 1), 0);
	assert_int_equal(os_process_pipe_read_err(NULL, &byte, 1), 0);
	assert_int_equal(os_process_pipe_write(NULL, &byte, 1), 0);
	assert_int_equal(os_process_pipe_destroy(NULL), 0);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		obs_unit_test(args_test),    obs_unit_test(read_pipe_test), obs_unit_test(write_pipe_test),
		obs_unit_test(quoting_test), obs_unit_test(error_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
