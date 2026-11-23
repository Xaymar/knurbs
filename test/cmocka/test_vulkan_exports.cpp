#include <fstream>
#include <string>
#include <vector>

#include <graphics/graphics.h>
#include <util/platform.h>

#ifndef _WIN32
#include <dlfcn.h>
#endif

// cmocka.h redefines `inline`, so it must come after the C++ standard headers.
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
extern "C" {
#include <cmocka.h>
}

// Required (non-optional) imports of the Windows section of graphics-imports.c.
#define EXPECTED_REQUIRED_WIN 142

static void *open_module()
{
	return os_dlopen(OBS_VULKAN_MODULE);
}

// What libobs's graphics_dlsym (graphics-imports.c) sees: on Linux dlsym on a module handle also searches its
// dependencies and finds libobs's own gs_* functions, which do not count as exports of the module.
static void *module_symbol(void *module, const char *name)
{
	void *sym = os_dlsym(module, name);
#ifndef _WIN32
	Dl_info in_module, in_libobs;
	if (sym && dladdr(sym, &in_module) && dladdr((void *)gs_get_device_type, &in_libobs) &&
	    in_module.dli_fbase == in_libobs.dli_fbase) {
		return nullptr;
	}
#endif
	return sym;
}

static void test_module_loads(void **)
{
	void *module = open_module();
	assert_non_null(module);
	os_dlclose(module);
}

// Names of the imports of the current platform from graphics-imports.c, for the given macro prefix.
static std::vector<std::string> imports_for_platform(const std::string &key)
{
	std::ifstream file(std::string(OBS_SOURCE_DIR) + "/libobs/graphics/graphics-imports.c");
	assert_true(file.is_open());

	enum { ANY, APPLE, WIN, LINUX } section = ANY;
#if defined(__APPLE__)
	const auto here = APPLE;
#elif defined(_WIN32)
	const auto here = WIN;
#else
	const auto here = LINUX;
#endif

	std::vector<std::string> names;
	std::string line;
	while (std::getline(file, line)) {
		if (line.rfind("#ifdef __APPLE__", 0) == 0) {
			section = APPLE;
		} else if (line.rfind("#elif _WIN32", 0) == 0) {
			section = WIN;
		} else if (line.rfind("#elif defined(__linux__)", 0) == 0) {
			section = LINUX;
		} else if (line.rfind("#endif", 0) == 0) {
			section = ANY;
		}

		size_t pos = line.find(key);
		if (pos == std::string::npos || line.find_first_not_of(" 	") != pos) {
			continue;
		}
		if (section != ANY && section != here) {
			continue;
		}
		std::string name = line.substr(pos + key.size());
		names.push_back(name.substr(0, name.find(')')));
	}
	return names;
}

// Every GRAPHICS_IMPORT( (not _OPTIONAL) of the current platform must be exported.
static void test_required_exports(void **)
{
	void *module = open_module();
	assert_non_null(module);

	auto names = imports_for_platform("GRAPHICS_IMPORT(");
#ifdef _WIN32
	assert_int_equal(names.size(), EXPECTED_REQUIRED_WIN);
#else
	assert_true(names.size() > 100);
#endif
	size_t missing = 0;
	for (auto &name : names) {
		if (!module_symbol(module, name.c_str())) {
			print_message("missing export: %s\n", name.c_str());
			++missing;
		}
	}
	assert_int_equal(missing, 0);
	os_dlclose(module);
}

// Optional imports are feature probes: exporting a stub makes libobs believe the feature works.
static void test_optional_not_exported(void **)
{
	void *module = open_module();
	assert_non_null(module);

	auto names = imports_for_platform("GRAPHICS_IMPORT_OPTIONAL(");
	assert_true(names.size() > 10);
	size_t exported = 0;
	for (auto &name : names) {
		// Implemented for real (Task 2; NV12/P010 and the Windows interop in phase 2; device loss and fast clear in
		// phase 2 step 3), so exporting them is
		// correct. The sync_file pair is Linux-only.
		static const char *implemented[] = {
			"device_enum_adapters",
			"gpu_get_driver_version",
			"gpu_get_renderer",
			"device_nv12_available",
			"device_p010_available",
			"device_texture_create_nv12",
			"device_texture_create_p010",
			"device_stagesurface_create_nv12",
			"device_stagesurface_create_p010",
			"device_get_adapter_identity",
			"device_shared_texture_supported",
			"device_texture_create_exportable",
			"device_texture_export",
			"device_texture_import",
			"device_sharedbuf_create",
			"gs_sharedbuf_destroy",
			"device_sharedbuf_copy_texture",
			"device_sharedbuf_export",
			"device_yuv_compute_available",
			"device_texture_convert_yuv",
			"device_sharedbuf_convert",
			"device_timeline_create",
			"device_timeline_import",
			"device_timeline_export",
			"gs_timeline_destroy",
			"device_timeline_signal",
			"device_timeline_wait",
			"device_timeline_get_value",
			"device_timeline_wait_cpu",
			"device_timeline_signal_cpu",
			"device_register_loss_callbacks",
			"device_unregister_loss_callbacks",
			"device_can_adapter_fast_clear",
#ifndef _WIN32
			"device_sync_file_export",
			"device_sync_file_wait",
#endif
#ifdef _WIN32
			"device_texture_open_shared",
			"device_texture_open_nt_shared",
			"device_texture_get_shared_handle",
			"device_texture_acquire_sync",
			"device_texture_release_sync",
#endif
		};
		bool real = false;
		for (const char *n : implemented) {
			real |= name == n;
		}
		if (real) {
			continue;
		}
		if (module_symbol(module, name.c_str())) {
			print_message("optional export must be absent: %s\n", name.c_str());
			++exported;
		}
	}
	assert_int_equal(exported, 0);
	os_dlclose(module);
}

// Exports beyond the graphics imports, on purpose: the host API (documented in vulkan-subsystem.hpp) and the
// test hooks. Checks each listed one exists and is no graphics import name (so libobs probes are unchanged); it does
// not reject unlisted exports.
static void test_listed_extra_exports_exist_and_are_not_imports(void **)
{
	void *module = open_module();
	assert_non_null(module);
	const char *extra[] = {"obs_vulkan_set_global_priority",
			       "obs_vulkan_validation_messages",
			       "obs_vulkan_test_stage_mip",
			       "obs_vulkan_glslang_refs",
			       "obs_vulkan_test_read_sharedbuf",
			       "obs_vulkan_test_convert_yuv_buffer",
			       "obs_vulkan_test_monitor_info",
			       "obs_vulkan_test_choose_surface_format"};
	auto required = imports_for_platform("GRAPHICS_IMPORT(");
	auto optional = imports_for_platform("GRAPHICS_IMPORT_OPTIONAL(");
	for (const char *name : extra) {
		assert_non_null(os_dlsym(module, name));
		for (auto *list : {&required, &optional}) {
			for (auto &import : *list) {
				assert_string_not_equal(import.c_str(), name);
			}
		}
	}
	os_dlclose(module);
}

#ifdef NULL_GRAPHICS_MODULE
// The null graphics module (test/null-graphics) stands in for a backend in the libobs tests, so it must export every
// import of this platform, required and optional, or libobs's paths behind the optional ones go untested.
static void test_null_module_exports_every_import(void **)
{
	void *module = os_dlopen(NULL_GRAPHICS_MODULE);
	assert_non_null(module);
	size_t missing = 0, total = 0;
	for (const char *key : {"GRAPHICS_IMPORT(", "GRAPHICS_IMPORT_OPTIONAL("}) {
		for (auto &name : imports_for_platform(key)) {
			++total;
			if (!module_symbol(module, name.c_str())) {
				print_message("libobs-null lacks: %s\n", name.c_str());
				++missing;
			}
		}
	}
	assert_true(total > EXPECTED_REQUIRED_WIN);
	assert_int_equal(missing, 0);
	os_dlclose(module);
}
#endif

static void test_identity(void **)
{
	void *module = open_module();
	assert_non_null(module);

	auto get_type = (int (*)(void))os_dlsym(module, "device_get_type");
	auto get_name = (const char *(*)(void))os_dlsym(module, "device_get_name");
	assert_non_null(get_type);
	assert_non_null(get_name);
	assert_int_equal(get_type(), GS_DEVICE_VULKAN);
	assert_string_equal(get_name(), "Vulkan");
	os_dlclose(module);
}

int main()
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_module_loads),
		cmocka_unit_test(test_required_exports),
		cmocka_unit_test(test_optional_not_exported),
		cmocka_unit_test(test_listed_extra_exports_exist_and_are_not_imports),
		cmocka_unit_test(test_identity),
#ifdef NULL_GRAPHICS_MODULE
		cmocka_unit_test(test_null_module_exports_every_import),
#endif
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
