#include <obs-module.h>
#include <string.h>
#include "animation-controller.h"
#include "lol-api-client.h"

/* ----------------------------------------------------------------- */
/* Overlay data                                                       */
/* ----------------------------------------------------------------- */

struct mia_overlay_data {
	obs_source_t *source;

	/* Display settings */
	char *overlay_path;
	char *sound_path;
	uint32_t width;
	uint32_t height;
	int ping_count;
	float ping_interval;
	float ping_scale;

	/* Detection state */
	struct lol_api_client api_client;

	/* Escalate mode */
	bool escalate_mode;
	int death_count;

	/* Animation */
	struct animation_controller anim;

	/* Lifecycle */
	bool activated;
	bool visible;
	bool game_was_active; /* API game_active last check */
};

/* ----------------------------------------------------------------- */
/* Setting keys                                                      */
/* ----------------------------------------------------------------- */

#define SETTING_OVERLAY_PATH      "overlay_path"
#define SETTING_SOUND_PATH        "sound_path"
#define SETTING_WIDTH             "width"
#define SETTING_HEIGHT            "height"
#define SETTING_PING_COUNT        "ping_count"
#define SETTING_PING_INTERVAL     "ping_interval"
#define SETTING_PING_SCALE        "ping_scale"
#define SETTING_SE_VOLUME        "se_volume"
#define SETTING_ESCALATE_MODE    "escalate_mode"
#define SETTING_IMG_DURATION_MODE "img_duration_mode"
#define SETTING_IMG_DURATION      "img_duration"

#define IMG_DURATION_AUTO   0
#define IMG_DURATION_MANUAL 1

/* ----------------------------------------------------------------- */
/* Helpers                                                            */
/* ----------------------------------------------------------------- */

static bool path_is_video(const char *path)
{
	if (!path || !*path)
		return false;
	const char *ext = strrchr(path, '.');
	if (!ext)
		return false;
	return _stricmp(ext, ".mp4") == 0 ||
	       _stricmp(ext, ".webm") == 0 ||
	       _stricmp(ext, ".mov") == 0 ||
	       _stricmp(ext, ".mkv") == 0 ||
	       _stricmp(ext, ".avi") == 0 ||
	       _stricmp(ext, ".flv") == 0;
}

static void setup_audio_sources(struct mia_overlay_data *d)
{
	bool is_video = path_is_video(d->overlay_path);

	if (is_video) {
		/* Video overlay -- decode video audio + SE (mixed) */
		animation_controller_load_audio(&d->anim, d->overlay_path);
		animation_controller_load_se_audio(&d->anim, d->sound_path);
	} else {
		/* Image overlay -- SE only */
		animation_controller_load_audio(&d->anim, d->sound_path);
		animation_controller_load_se_audio(&d->anim, NULL);
	}

	/* Mute overlay sources -- audio goes through PCM pipeline */
	for (int i = 0; i < MAX_PINGS; i++) {
		if (d->anim.overlay_sources[i])
			obs_source_set_volume(d->anim.overlay_sources[i],
					      0.0f);
	}
}

static void update_image_duration(struct mia_overlay_data *d,
				  obs_data_t *settings)
{
	if (path_is_video(d->overlay_path))
		return;

	int mode = (int)obs_data_get_int(settings, SETTING_IMG_DURATION_MODE);
	if (mode == IMG_DURATION_MANUAL) {
		d->anim.image_duration =
			(float)obs_data_get_double(settings,
						   SETTING_IMG_DURATION);
	} else {
		/* Auto: derive from SE file length */
		if (d->anim.pcm_loaded && d->anim.pcm.sample_rate > 0)
			d->anim.image_duration =
				(float)d->anim.pcm.total_frames /
				(float)d->anim.pcm.sample_rate;
		else
			d->anim.image_duration = 1.0f;
	}
}

/* ----------------------------------------------------------------- */
/* Callbacks                                                         */
/* ----------------------------------------------------------------- */

static const char *mia_overlay_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("MIAOverlay");
}

static void *mia_overlay_create(obs_data_t *settings, obs_source_t *source)
{
	struct mia_overlay_data *d =
		bzalloc(sizeof(struct mia_overlay_data));
	d->source = source;

	/* Display settings */
	d->overlay_path = bstrdup(
		obs_data_get_string(settings, SETTING_OVERLAY_PATH));
	d->sound_path = bstrdup(
		obs_data_get_string(settings, SETTING_SOUND_PATH));
	d->width = (uint32_t)obs_data_get_int(settings, SETTING_WIDTH);
	d->height = (uint32_t)obs_data_get_int(settings, SETTING_HEIGHT);
	d->ping_count = (int)obs_data_get_int(settings, SETTING_PING_COUNT);
	d->ping_interval =
		(float)obs_data_get_double(settings, SETTING_PING_INTERVAL);
	d->ping_scale =
		(float)obs_data_get_double(settings, SETTING_PING_SCALE);
	d->escalate_mode =
		obs_data_get_bool(settings, SETTING_ESCALATE_MODE);

	/* Detection init */
	lol_api_client_init(&d->api_client);
	lol_api_client_start(&d->api_client);

	/* Animation init */
	animation_controller_init(&d->anim, source);
	animation_controller_load_overlay(&d->anim, d->overlay_path,
					 path_is_video(d->overlay_path));
	setup_audio_sources(d);
	animation_controller_set_volume(
		&d->anim,
		(float)obs_data_get_double(settings, SETTING_SE_VOLUME)
			/ 100.0f);
	update_image_duration(d, settings);

	return d;
}

static void mia_overlay_destroy(void *data)
{
	struct mia_overlay_data *d = data;

	if (d->activated)
		animation_controller_dec_active(&d->anim);
	if (d->visible)
		animation_controller_dec_showing(&d->anim);

	animation_controller_free(&d->anim);
	lol_api_client_stop(&d->api_client);
	lol_api_client_free(&d->api_client);
	bfree(d->overlay_path);
	bfree(d->sound_path);
	bfree(d);
}

static void mia_overlay_update(void *data, obs_data_t *settings)
{
	struct mia_overlay_data *d = data;

	/* Check if media paths changed */
	const char *new_overlay =
		obs_data_get_string(settings, SETTING_OVERLAY_PATH);
	const char *new_sound =
		obs_data_get_string(settings, SETTING_SOUND_PATH);

	bool overlay_changed = !d->overlay_path ||
		strcmp(d->overlay_path, new_overlay) != 0;
	bool sound_changed = !d->sound_path ||
		strcmp(d->sound_path, new_sound) != 0;

	/* Bracket dec/inc once for all media source reloads */
	if (overlay_changed || sound_changed) {
		if (d->activated)
			animation_controller_dec_active(&d->anim);
		if (d->visible)
			animation_controller_dec_showing(&d->anim);

		if (overlay_changed) {
			bfree(d->overlay_path);
			d->overlay_path = bstrdup(new_overlay);
			animation_controller_load_overlay(
				&d->anim, d->overlay_path,
				path_is_video(d->overlay_path));
		}
		if (sound_changed) {
			bfree(d->sound_path);
			d->sound_path = bstrdup(new_sound);
		}

		/* Reconfigure SE based on overlay type */
		setup_audio_sources(d);

		if (d->activated)
			animation_controller_inc_active(&d->anim);
		if (d->visible)
			animation_controller_inc_showing(&d->anim);
	}

	d->width = (uint32_t)obs_data_get_int(settings, SETTING_WIDTH);
	d->height = (uint32_t)obs_data_get_int(settings, SETTING_HEIGHT);
	d->ping_count = (int)obs_data_get_int(settings, SETTING_PING_COUNT);
	d->ping_interval =
		(float)obs_data_get_double(settings, SETTING_PING_INTERVAL);
	d->ping_scale =
		(float)obs_data_get_double(settings, SETTING_PING_SCALE);
	d->escalate_mode =
		obs_data_get_bool(settings, SETTING_ESCALATE_MODE);

	animation_controller_set_volume(
		&d->anim,
		(float)obs_data_get_double(settings, SETTING_SE_VOLUME)
			/ 100.0f);
	update_image_duration(d, settings);
}

static void mia_overlay_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, SETTING_WIDTH, 400);
	obs_data_set_default_int(settings, SETTING_HEIGHT, 400);
	obs_data_set_default_int(settings, SETTING_PING_COUNT, 3);
	obs_data_set_default_double(settings, SETTING_PING_INTERVAL, 0.5);
	obs_data_set_default_double(settings, SETTING_PING_SCALE, 35.0);
	obs_data_set_default_double(settings, SETTING_SE_VOLUME, 100.0);
	obs_data_set_default_bool(settings, SETTING_ESCALATE_MODE, false);
	obs_data_set_default_int(settings, SETTING_IMG_DURATION_MODE,
				 IMG_DURATION_AUTO);
	obs_data_set_default_double(settings, SETTING_IMG_DURATION, 1.0);
}

static bool escalate_mode_changed(obs_properties_t *props,
				  obs_property_t *prop,
				  obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);
	bool on = obs_data_get_bool(settings, SETTING_ESCALATE_MODE);
	obs_property_set_visible(
		obs_properties_get(props, SETTING_PING_COUNT), !on);
	obs_property_set_visible(
		obs_properties_get(props, SETTING_PING_INTERVAL), !on);
	return true;
}

static bool img_duration_mode_changed(obs_properties_t *props,
				      obs_property_t *prop,
				      obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);
	int mode = (int)obs_data_get_int(settings, SETTING_IMG_DURATION_MODE);
	obs_property_set_visible(
		obs_properties_get(props, SETTING_IMG_DURATION),
		mode == IMG_DURATION_MANUAL);
	return true;
}

static bool overlay_path_changed(obs_properties_t *props,
				 obs_property_t *prop,
				 obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);
	const char *path = obs_data_get_string(settings,
					       SETTING_OVERLAY_PATH);
	bool is_image = path && *path && !path_is_video(path);
	int mode = (int)obs_data_get_int(settings, SETTING_IMG_DURATION_MODE);

	obs_property_set_visible(
		obs_properties_get(props, SETTING_IMG_DURATION_MODE),
		is_image);
	obs_property_set_visible(
		obs_properties_get(props, SETTING_IMG_DURATION),
		is_image && mode == IMG_DURATION_MANUAL);
	return true;
}

static bool clear_overlay_clicked(obs_properties_t *props,
				  obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);
	struct mia_overlay_data *d = data;
	obs_data_t *settings = obs_source_get_settings(d->source);
	obs_data_set_string(settings, SETTING_OVERLAY_PATH, "");
	obs_source_update(d->source, settings);
	obs_data_release(settings);
	return true;
}

static bool clear_sound_clicked(obs_properties_t *props,
				obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);
	struct mia_overlay_data *d = data;
	obs_data_t *settings = obs_source_get_settings(d->source);
	obs_data_set_string(settings, SETTING_SOUND_PATH, "");
	obs_source_update(d->source, settings);
	obs_data_release(settings);
	return true;
}

static obs_properties_t *mia_overlay_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	/* --- Display --- */
	obs_property_t *overlay_prop = obs_properties_add_path(
		props, SETTING_OVERLAY_PATH,
		obs_module_text("MIAPinImage"),
		OBS_PATH_FILE,
		"Media (*.png *.jpg *.bmp *.gif "
		"*.webm *.mp4 *.mov)",
		NULL);
	obs_property_set_modified_callback(overlay_prop, overlay_path_changed);

	obs_properties_add_button(props, "clear_overlay",
				  obs_module_text("ClearOverlay"),
				  clear_overlay_clicked);

	obs_properties_add_path(props, SETTING_SOUND_PATH,
				obs_module_text("SoundEffect"),
				OBS_PATH_FILE,
				"Audio (*.wav *.mp3 *.ogg *.flac)",
				NULL);
	obs_properties_add_button(props, "clear_sound",
				  obs_module_text("ClearSound"),
				  clear_sound_clicked);

	obs_properties_add_float_slider(props, SETTING_SE_VOLUME,
					obs_module_text("SEVolume"),
					0.0, 100.0, 1.0);

	/* --- Image duration (only visible for image overlays) --- */
	obs_property_t *dur_mode = obs_properties_add_list(
		props, SETTING_IMG_DURATION_MODE,
		obs_module_text("ImageDurationMode"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(dur_mode,
				  obs_module_text("ImageDurationAuto"),
				  IMG_DURATION_AUTO);
	obs_property_list_add_int(dur_mode,
				  obs_module_text("ImageDurationManual"),
				  IMG_DURATION_MANUAL);
	obs_property_set_modified_callback(dur_mode,
					   img_duration_mode_changed);

	obs_properties_add_float_slider(props, SETTING_IMG_DURATION,
					obs_module_text("ImageDuration"),
					0.1, 10.0, 0.1);

	obs_properties_add_int(props, SETTING_WIDTH,
			       obs_module_text("OverlayWidth"),
			       100, 3840, 10);

	obs_properties_add_int(props, SETTING_HEIGHT,
			       obs_module_text("OverlayHeight"),
			       100, 2160, 10);

	obs_property_t *esc = obs_properties_add_bool(
		props, SETTING_ESCALATE_MODE,
		obs_module_text("EscalateMode"));
	obs_property_set_modified_callback(esc, escalate_mode_changed);

	obs_properties_add_int(props, SETTING_PING_COUNT,
			       obs_module_text("PingCount"),
			       1, 10, 1);

	obs_properties_add_float_slider(props, SETTING_PING_INTERVAL,
					obs_module_text("PingInterval"),
					0.1, 2.0, 0.1);

	obs_properties_add_float_slider(props, SETTING_PING_SCALE,
					obs_module_text("PingScale"),
					2.0, 50.0, 1.0);

	return props;
}

/* ----------------------------------------------------------------- */
/* Lifecycle                                                          */
/* ----------------------------------------------------------------- */

static void mia_overlay_activate(void *data)
{
	struct mia_overlay_data *d = data;
	d->activated = true;
	animation_controller_inc_active(&d->anim);
}

static void mia_overlay_deactivate(void *data)
{
	struct mia_overlay_data *d = data;
	d->activated = false;
	animation_controller_dec_active(&d->anim);
}

static void mia_overlay_show(void *data)
{
	struct mia_overlay_data *d = data;
	d->visible = true;
	animation_controller_inc_showing(&d->anim);
}

static void mia_overlay_hide(void *data)
{
	struct mia_overlay_data *d = data;
	d->visible = false;
	animation_controller_dec_showing(&d->anim);
}

/* ----------------------------------------------------------------- */
/* Size -- auto-detect from canvas resolution                          */
/* ----------------------------------------------------------------- */

static uint32_t mia_overlay_get_width(void *data)
{
	return ((struct mia_overlay_data *)data)->width;
}

static uint32_t mia_overlay_get_height(void *data)
{
	return ((struct mia_overlay_data *)data)->height;
}

/* ----------------------------------------------------------------- */
/* video_tick -- detection timing + animation (main thread)            */
/* ----------------------------------------------------------------- */

static void mia_overlay_video_tick(void *data, float seconds)
{
	struct mia_overlay_data *d = data;

	/* Check if LoL game is active via API */
	bool game_active = lol_api_client_is_game_active(&d->api_client);

	/* Reset escalate count when game starts or ends */
	if (game_active && !d->game_was_active)
		d->death_count = 0;
	if (!game_active && d->game_was_active) {
		d->death_count = 0;
		d->anim.stagger_interval = 1.5f;
	}
	d->game_was_active = game_active;

	/* Check for death (always consume to clear flag, but only
	 * start animation if not already playing) */
	int api_deaths = 0;
	if (game_active && lol_api_client_consume_death(&d->api_client,
							&api_deaths) &&
	    !d->anim.active) {
		blog(LOG_INFO, "[mia-pin] DEATH CONSUMED: api_deaths=%d, "
		     "anim_active=%d", api_deaths, d->anim.active);
		int count;
		float interval;
		if (d->escalate_mode) {
			d->death_count = api_deaths;
			count = d->death_count;
			if (count < 1)
				count = 1;
			if (count > MAX_PINGS)
				count = MAX_PINGS;
			interval = 1.5f / (float)count;
		} else {
			count = d->ping_count;
			interval = d->ping_interval;
		}
		d->anim.stagger_interval = interval;
		d->anim.ping_scale = d->ping_scale / 100.0f;
		animation_controller_start(&d->anim, count,
					   d->width, d->height);
	}

	if (d->anim.active)
		animation_controller_tick(&d->anim, seconds);
}

/* ----------------------------------------------------------------- */
/* video_render -- detection capture + overlay (graphics thread)       */
/* ----------------------------------------------------------------- */

static void mia_overlay_video_render(void *data, gs_effect_t *effect)
{
	struct mia_overlay_data *d = data;
	UNUSED_PARAMETER(effect);

	/* Render active pings */
	if (d->anim.active && animation_controller_any_visible(&d->anim))
		animation_controller_render(&d->anim);
}

/* ----------------------------------------------------------------- */
/* Source info                                                        */
/* ----------------------------------------------------------------- */

struct obs_source_info mia_overlay_info = {
	.id = "lol_death_mia_overlay",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW |
			OBS_SOURCE_AUDIO,
	.get_name = mia_overlay_get_name,
	.create = mia_overlay_create,
	.destroy = mia_overlay_destroy,
	.update = mia_overlay_update,
	.get_defaults = mia_overlay_defaults,
	.get_properties = mia_overlay_properties,
	.get_width = mia_overlay_get_width,
	.get_height = mia_overlay_get_height,
	.video_tick = mia_overlay_video_tick,
	.video_render = mia_overlay_video_render,
	.activate = mia_overlay_activate,
	.deactivate = mia_overlay_deactivate,
	.show = mia_overlay_show,
	.hide = mia_overlay_hide,
};
