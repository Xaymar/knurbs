// Installing the game capture hook into the shared directory (C:\ProgramData\obs-studio-hook in win-capture): only a
// hook signed by OBS Project, LLC is ever copied, never over a newer one, and only a signed one is used from there.
//
// The decision is tested on its own; the signature check and the copying on real files in the working directory:
// this executable (unsigned) stands in for a self-built hook, and OBS_SIGNED_HOOK_DIR (the shipped, signed hook
// binaries) supplies a signed one.
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <windows.h>

#include <cmocka.h>

#include "game-capture-hook-install.h"

static struct hook_file_info info(bool exists, bool signed_by_obs, int major, int minor, int build)
{
	struct hook_file_info i = {0};
	i.exists = exists;
	i.signed_by_obs = exists && signed_by_obs;
	i.has_version = exists && major >= 0;
	if (i.has_version) {
		i.version.major = major;
		i.version.minor = minor;
		i.version.build = build;
	}
	return i;
}

static void decide_unsigned_source_is_never_copied(void **state)
{
	(void)state;
	const struct hook_file_info src = info(true, false, 1, 9, 0);
	struct hook_file_info dst = info(false, false, -1, 0, 0);
	assert_int_equal(hook_install_decide(&src, &dst), HOOK_INSTALL_NOT_TRUSTED);
	dst = info(true, true, 1, 8, 6); // older, signed
	assert_int_equal(hook_install_decide(&src, &dst), HOOK_INSTALL_NOT_TRUSTED);
	dst = info(true, false, 1, 8, 6); // older, unsigned
	assert_int_equal(hook_install_decide(&src, &dst), HOOK_INSTALL_NOT_TRUSTED);

	const struct hook_file_info missing = info(false, false, -1, 0, 0);
	assert_int_equal(hook_install_decide(&missing, &dst), HOOK_INSTALL_NOT_TRUSTED);
	const struct hook_file_info no_version = info(true, true, -1, 0, 0);
	assert_int_equal(hook_install_decide(&no_version, &dst), HOOK_INSTALL_NOT_TRUSTED);
}

static void decide_signed_newer_source_is_copied(void **state)
{
	(void)state;
	const struct hook_file_info src = info(true, true, 1, 8, 8);
	struct hook_file_info dst = info(false, false, -1, 0, 0);
	assert_int_equal(hook_install_decide(&src, &dst), HOOK_INSTALL_COPY);
	dst = info(true, true, 1, 8, 6);
	assert_int_equal(hook_install_decide(&src, &dst), HOOK_INSTALL_COPY);
	dst = info(true, true, 0, 9, 9);
	assert_int_equal(hook_install_decide(&src, &dst), HOOK_INSTALL_COPY);
}

static void decide_signed_older_source_is_not_copied(void **state)
{
	(void)state;
	const struct hook_file_info src = info(true, true, 1, 8, 6);
	struct hook_file_info dst = info(true, true, 1, 8, 8);
	assert_int_equal(hook_install_decide(&src, &dst), HOOK_INSTALL_KEEP);
	dst = info(true, true, 1, 8, 6); // the same version: nothing to do
	assert_int_equal(hook_install_decide(&src, &dst), HOOK_INSTALL_KEEP);
	dst = info(true, true, 2, 0, 0);
	assert_int_equal(hook_install_decide(&src, &dst), HOOK_INSTALL_KEEP);
}

static void decide_signed_source_replaces_unsigned_target_without_downgrade(void **state)
{
	(void)state;
	const struct hook_file_info src = info(true, true, 1, 8, 8);
	struct hook_file_info dst = info(true, false, 1, 8, 8);
	assert_int_equal(hook_install_decide(&src, &dst), HOOK_INSTALL_COPY);
	dst = info(true, false, 1, 8, 6);
	assert_int_equal(hook_install_decide(&src, &dst), HOOK_INSTALL_COPY);
	dst = info(true, false, -1, 0, 0);
	assert_int_equal(hook_install_decide(&src, &dst), HOOK_INSTALL_COPY);
	dst = info(true, false, 1, 9, 0); // newer, unsigned: not downgraded (and not used)
	assert_int_equal(hook_install_decide(&src, &dst), HOOK_INSTALL_KEEP);
	assert_false(hook_install_usable(&src, &dst));
}

static void usable_only_when_signed_and_compatible(void **state)
{
	(void)state;
	const struct hook_file_info src = info(true, true, 1, 8, 8);
	struct hook_file_info dst = info(true, true, 1, 9, 0);
	assert_true(hook_install_usable(&src, &dst));
	dst = info(true, false, 1, 8, 8);
	assert_false(hook_install_usable(&src, &dst));
	dst = info(true, true, 2, 0, 0); // newer major: the protocol may differ
	assert_false(hook_install_usable(&src, &dst));
	dst = info(false, false, -1, 0, 0);
	assert_false(hook_install_usable(&src, &dst));
	// an unsigned bundled hook still allows a signed installed one
	const struct hook_file_info unsigned_src = info(true, false, 1, 8, 8);
	dst = info(true, true, 1, 8, 6);
	assert_true(hook_install_usable(&unsigned_src, &dst));
}

/* ------------------------------------------------------------------------- */
/* real files                                                                */

static wchar_t work[MAX_PATH];

static void path_in(wchar_t *out, const wchar_t *dir, const wchar_t *name)
{
	_snwprintf(out, MAX_PATH, L"%ls\\%ls", dir, name);
	out[MAX_PATH - 1] = 0;
}

static void fresh_dir(const wchar_t *name, wchar_t *out)
{
	path_in(out, work, name);
	wchar_t pattern[MAX_PATH], file[MAX_PATH];
	path_in(pattern, out, L"*");
	WIN32_FIND_DATAW wfd;
	HANDLE h = FindFirstFileW(pattern, &wfd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			if (!(wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
				path_in(file, out, wfd.cFileName);
				SetFileAttributesW(file, FILE_ATTRIBUTE_NORMAL);
				DeleteFileW(file);
			}
		} while (FindNextFileW(h, &wfd));
		FindClose(h);
	}
	CreateDirectoryW(out, NULL);
}

static void write_file(const wchar_t *path, const char *text)
{
	FILE *f = _wfopen(path, L"wb");
	assert_non_null(f);
	fputs(text, f);
	fclose(f);
}

static bool exists(const wchar_t *path)
{
	return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static FILETIME mtime(const wchar_t *path)
{
	WIN32_FILE_ATTRIBUTE_DATA a;
	assert_true(GetFileAttributesExW(path, GetFileExInfoStandard, &a));
	return a.ftLastWriteTime;
}

static void set_old_mtime(const wchar_t *path)
{
	HANDLE h = CreateFileW(path, FILE_WRITE_ATTRIBUTES, 0, NULL, OPEN_EXISTING, 0, NULL);
	assert_true(h != INVALID_HANDLE_VALUE);
	FILETIME old = {0x2a000000, 0x01d00000}; // 2014
	assert_true(SetFileTime(h, NULL, NULL, &old));
	CloseHandle(h);
}

static void self_path(wchar_t *out)
{
	assert_true(GetModuleFileNameW(NULL, out, MAX_PATH) > 0);
}

static const char *signed_dir = OBS_SIGNED_HOOK_DIR;

static bool signed_hook(wchar_t *dll, wchar_t *json)
{
	if (!signed_dir[0])
		return false;
	wchar_t dir[MAX_PATH];
	MultiByteToWideChar(CP_UTF8, 0, signed_dir, -1, dir, MAX_PATH);
	path_in(dll, dir, L"graphics-hook64.dll");
	path_in(json, dir, L"obs-vulkan64.json");
	return true;
}

static void unsigned_file_is_not_signed(void **state)
{
	(void)state;
	wchar_t self[MAX_PATH];
	self_path(self);
	assert_false(hook_signed_by_obs(self));
	assert_false(hook_signed_by_obs(L"Z:\\does\\not\\exist\\graphics-hook64.dll"));
	assert_false(hook_signed_by_obs(NULL));
}

static void unsigned_hook_is_not_installed(void **state)
{
	(void)state;
	wchar_t src_dir[MAX_PATH], dst_dir[MAX_PATH], self[MAX_PATH];
	wchar_t src_dll[MAX_PATH], src_json[MAX_PATH], dst_dll[MAX_PATH], dst_json[MAX_PATH];
	fresh_dir(L"unsigned-src", src_dir);
	fresh_dir(L"unsigned-dst", dst_dir);
	self_path(self);
	path_in(src_dll, src_dir, L"graphics-hook64.dll");
	path_in(src_json, src_dir, L"obs-vulkan64.json");
	path_in(dst_dll, dst_dir, L"graphics-hook64.dll");
	path_in(dst_json, dst_dir, L"obs-vulkan64.json");
	assert_true(CopyFileW(self, src_dll, FALSE));
	write_file(src_json, "{}");

	assert_false(hook_install_files(src_dll, src_json, dst_dll, dst_json));
	assert_false(exists(dst_dll));
	assert_false(exists(dst_json));
}

static void signed_hook_is_installed_once(void **state)
{
	wchar_t sdll[MAX_PATH], sjson[MAX_PATH];
	if (!signed_hook(sdll, sjson)) {
		print_message("no signed hook (OBS_SIGNED_HOOK_DIR), skipped\n");
		skip();
	}
	(void)state;
	assert_true(hook_signed_by_obs(sdll));

	wchar_t dst_dir[MAX_PATH], dst_dll[MAX_PATH], dst_json[MAX_PATH];
	fresh_dir(L"signed-dst", dst_dir);
	path_in(dst_dll, dst_dir, L"graphics-hook64.dll");
	path_in(dst_json, dst_dir, L"obs-vulkan64.json");

	assert_true(hook_install_files(sdll, sjson, dst_dll, dst_json));
	assert_true(hook_signed_by_obs(dst_dll));
	assert_true(exists(dst_json));

	// the same version again: left alone
	set_old_mtime(dst_dll);
	const FILETIME before = mtime(dst_dll);
	assert_true(hook_install_files(sdll, sjson, dst_dll, dst_json));
	const FILETIME after = mtime(dst_dll);
	assert_memory_equal(&before, &after, sizeof(before));
}

static void signed_hook_replaces_unsigned_one(void **state)
{
	wchar_t sdll[MAX_PATH], sjson[MAX_PATH];
	if (!signed_hook(sdll, sjson)) {
		print_message("no signed hook (OBS_SIGNED_HOOK_DIR), skipped\n");
		skip();
	}
	(void)state;
	wchar_t dst_dir[MAX_PATH], dst_dll[MAX_PATH], dst_json[MAX_PATH], self[MAX_PATH];
	fresh_dir(L"replace-dst", dst_dir);
	path_in(dst_dll, dst_dir, L"graphics-hook64.dll");
	path_in(dst_json, dst_dir, L"obs-vulkan64.json");
	self_path(self);
	assert_true(CopyFileW(self, dst_dll, FALSE)); // an unsigned hook without a version
	write_file(dst_json, "{}");

	assert_true(hook_install_files(sdll, sjson, dst_dll, dst_json));
	assert_true(hook_signed_by_obs(dst_dll));
}

static void tampered_signed_hook_is_not_installed(void **state)
{
	wchar_t sdll[MAX_PATH], sjson[MAX_PATH];
	if (!signed_hook(sdll, sjson)) {
		print_message("no signed hook (OBS_SIGNED_HOOK_DIR), skipped\n");
		skip();
	}
	(void)state;
	wchar_t src_dir[MAX_PATH], dst_dir[MAX_PATH];
	wchar_t src_dll[MAX_PATH], src_json[MAX_PATH], dst_dll[MAX_PATH], dst_json[MAX_PATH];
	fresh_dir(L"tampered-src", src_dir);
	fresh_dir(L"tampered-dst", dst_dir);
	path_in(src_dll, src_dir, L"graphics-hook64.dll");
	path_in(src_json, src_dir, L"obs-vulkan64.json");
	path_in(dst_dll, dst_dir, L"graphics-hook64.dll");
	path_in(dst_json, dst_dir, L"obs-vulkan64.json");
	assert_true(CopyFileW(sdll, src_dll, FALSE));
	assert_true(CopyFileW(sjson, src_json, FALSE));
	SetFileAttributesW(src_dll, FILE_ATTRIBUTE_NORMAL);

	// flip one byte in the code, well inside the signed part of the image
	FILE *f = _wfopen(src_dll, L"r+b");
	assert_non_null(f);
	assert_int_equal(fseek(f, 0x2000, SEEK_SET), 0);
	const int c = fgetc(f);
	assert_int_equal(fseek(f, 0x2000, SEEK_SET), 0);
	fputc(c ^ 0xff, f);
	fclose(f);

	assert_false(hook_signed_by_obs(src_dll));
	assert_false(hook_install_files(src_dll, src_json, dst_dll, dst_json));
	assert_false(exists(dst_dll));
}

int main(void)
{
	assert_true(GetCurrentDirectoryW(MAX_PATH, work) > 0);

	const struct CMUnitTest tests[] = {
		cmocka_unit_test(decide_unsigned_source_is_never_copied),
		cmocka_unit_test(decide_signed_newer_source_is_copied),
		cmocka_unit_test(decide_signed_older_source_is_not_copied),
		cmocka_unit_test(decide_signed_source_replaces_unsigned_target_without_downgrade),
		cmocka_unit_test(usable_only_when_signed_and_compatible),
		cmocka_unit_test(unsigned_file_is_not_signed),
		cmocka_unit_test(unsigned_hook_is_not_installed),
		cmocka_unit_test(signed_hook_is_installed_once),
		cmocka_unit_test(signed_hook_replaces_unsigned_one),
		cmocka_unit_test(tampered_signed_hook_is_not_installed),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
