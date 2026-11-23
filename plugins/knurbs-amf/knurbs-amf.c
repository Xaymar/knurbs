#include <obs-module.h>

#include "amf-ids.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("knurbs-amf", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "AMD AMF hardware encoders";
}

extern void amf_load(void);
extern void amf_unload(void);

bool obs_module_load(void)
{
	amf_load();
	return true;
}

void obs_module_unload(void)
{
	amf_unload();
}
