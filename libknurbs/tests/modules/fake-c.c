/* A well-behaved module written in C: registers com.example.fake.c (with the alias fake_c_old) in load, and leaves a
 * marker file in the working directory for each entry point that ran and when its type's param is freed, so tests
 * can tell what ran. */
#include <stdio.h>
#include <stdlib.h>

#include "knurbs/c/core.h"
#include "knurbs/c/data.h"
#include "knurbs/c/log.h"
#include "knurbs/c/module.h"
#include "knurbs/c/object.h"
#include "knurbs/c/properties.h"
#include "knurbs/c/registry.h"

#ifdef FAKE_C_API_MAJOR
/* Built a second time, as fake-c-other-major, for another API major version: the loader must reject it before load
 * runs. */
KNURBS_MODULE_EXPORT const struct knurbs_module_header *knurbs_module_header(void);
KNURBS_MODULE_EXPORT const struct knurbs_module_header *knurbs_module_header(void)
{
	static const struct knurbs_module_header header = {
		sizeof(struct knurbs_module_header),
		KNURBS_MODULE_API_C,
		FAKE_C_API_MAJOR,
		0,
	};
	return &header;
}
#else
KNURBS_MODULE_C()
#endif

static void write_marker(const char *entry)
{
	char name[64];
	snprintf(name, sizeof(name), "fake-c.%s", entry);
	FILE *f = fopen(name, "w");
	if (f) {
		fputs("ran\n", f);
		fclose(f);
	}
}

struct fake_c_param {
	int64_t gain;
};

static void fake_c_defaults(void *param, knurbs_data_t *settings)
{
	const struct fake_c_param *p = param;
	knurbs_data_set_default_int(settings, "gain", p->gain);
}

static knurbs_properties_t *fake_c_properties(void *param, const knurbs_data_t *settings)
{
	(void)param;
	(void)settings;
	knurbs_properties_t *props = NULL;
	if (knurbs_properties_create(&props) != KNURBS_OK) {
		return NULL;
	}
	knurbs_property_t *gain = NULL;
	if (knurbs_properties_add_int(props, "gain", "Gain", 0, 10, 1, &gain) == KNURBS_OK) {
		knurbs_release((knurbs_object_t *)gain);
	}
	return props; /* the library adopts this reference */
}

static void fake_c_free_param(void *param)
{
	free(param);
	write_marker("free_param");
}

KNURBS_MODULE_EXPORT void knurbs_module_load(knurbs_module_t *m);
KNURBS_MODULE_EXPORT void knurbs_module_post_load(knurbs_module_t *m);
KNURBS_MODULE_EXPORT void knurbs_module_unload(knurbs_module_t *m);

KNURBS_MODULE_EXPORT void knurbs_module_load(knurbs_module_t *m)
{
	write_marker("load");
	struct fake_c_param *param = malloc(sizeof(*param));
	if (!param) {
		return;
	}
	param->gain = 5;

	struct knurbs_type_info info = {
		sizeof(struct knurbs_type_info),
		"com.example.fake.c",
		KNURBS_TYPE_SOURCE,
		2,
		fake_c_defaults,
		fake_c_properties,
		param,
		fake_c_free_param,
	};
	/* From here on the library owns param, whether or not the call succeeds. */
	if (knurbs_registry_add(m, &info) == KNURBS_OK) {
		knurbs_registry_add_alias(m, "fake_c_old", "com.example.fake.c");
	}
}

KNURBS_MODULE_EXPORT void knurbs_module_post_load(knurbs_module_t *m)
{
	/* Marks post_load only if the type registered in load can be found through the module's core. */
	knurbs_core_t *core = knurbs_module_core(m);
	knurbs_type_t *type = knurbs_registry_find(core, "com.example.fake.c");
	if (type) {
		write_marker("post_load");
	}
	knurbs_release((knurbs_object_t *)type);
	/* The core is borrowed: not released. */
}

KNURBS_MODULE_EXPORT void knurbs_module_unload(knurbs_module_t *m)
{
	/* Uses the core during unload, which may be the shutdown that the core's last release started. Marks unload
	 * only if that worked. */
	knurbs_core_t *core = knurbs_module_core(m);
	knurbs_logger_t *logger = knurbs_core_logger(core);
	if (logger && knurbs_log_write(logger, KNURBS_LOG_DEBUG, "fake-c: unload") == KNURBS_OK) {
		write_marker("unload");
	}
	knurbs_release((knurbs_object_t *)logger);
}
