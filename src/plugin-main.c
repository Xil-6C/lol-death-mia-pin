#include <obs-module.h>
#include "mia-overlay.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("lol-death-mia-pin", "en-US")

bool obs_module_load(void)
{
	obs_register_source(&mia_overlay_info);

	blog(LOG_INFO, "[lol-death-mia-pin] plugin loaded (version %s)",
	     PROJECT_VERSION);
	return true;
}

void obs_module_unload(void)
{
	blog(LOG_INFO, "[lol-death-mia-pin] plugin unloaded");
}
