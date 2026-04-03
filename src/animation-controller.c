#include "animation-controller.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* ----------------------------------------------------------------- */
/* Internal helpers                                                    */
/* ----------------------------------------------------------------- */

static void generate_positions(struct animation_controller *ac,
			       int count,
			       uint32_t cx, uint32_t cy)
{
	for (int i = 0; i < count && i < MAX_PINGS; i++) {
		ac->positions[i].x =
			((float)rand() / (float)RAND_MAX) * (float)cx;
		ac->positions[i].y =
			((float)rand() / (float)RAND_MAX) * (float)cy;
	}
}

/* ---- Source creation / release --------------------------------- */

static obs_source_t *create_media_source(const char *name, const char *path)
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "local_file", path);
	obs_data_set_bool(settings, "is_local_file", true);
	obs_data_set_bool(settings, "looping", false);
	obs_data_set_bool(settings, "restart_on_activate", false);

	obs_source_t *src =
		obs_source_create_private("ffmpeg_source", name, settings);
	obs_data_release(settings);

	if (src)
		obs_source_media_stop(src);

	return src;
}

static obs_source_t *create_image_source(const char *name, const char *path)
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "file", path);

	obs_source_t *src =
		obs_source_create_private("image_source", name, settings);
	obs_data_release(settings);

	return src;
}

static void release_source(obs_source_t **src)
{
	if (!*src)
		return;
	obs_source_media_stop(*src);
	obs_source_release(*src);
	*src = NULL;
}

/* ---- Audio output via obs_source_output_audio ------------------- */

/* Mix one PCM source into audio_buf for a single ping */
static void mix_pcm_for_ping(struct animation_controller *ac,
			     struct pcm_audio *pcm,
			     int64_t local_start, size_t frames,
			     size_t out_ch, bool *has_data)
{
	size_t src_ch = pcm->channels;
	if (src_ch > out_ch)
		src_ch = out_ch;

	for (size_t f = 0; f < frames; f++) {
		int64_t pos = local_start + (int64_t)f;
		if (pos < 0)
			continue;
		if (pos >= (int64_t)pcm->total_frames)
			break;

		*has_data = true;
		for (size_t ch = 0; ch < src_ch; ch++)
			ac->audio_buf[ch][f] +=
				pcm->data[ch][pos] * ac->se_volume;
	}
}

static void output_audio_chunk(struct animation_controller *ac,
			       size_t frames, uint64_t timestamp)
{
	if (!ac->parent_source || !ac->playback_active || frames == 0)
		return;
	if (!ac->pcm_loaded && !ac->pcm_se_loaded)
		return;

	/* Output channel count = max of both sources */
	size_t pcm_ch = 0;
	uint32_t sample_rate = 0;
	if (ac->pcm_loaded) {
		pcm_ch = ac->pcm.channels;
		sample_rate = ac->pcm.sample_rate;
	}
	if (ac->pcm_se_loaded) {
		if (ac->pcm_se.channels > pcm_ch)
			pcm_ch = ac->pcm_se.channels;
		if (sample_rate == 0)
			sample_rate = ac->pcm_se.sample_rate;
	}
	if (pcm_ch > MAX_AV_PLANES)
		pcm_ch = MAX_AV_PLANES;

	/* Zero scratch buffer */
	for (size_t ch = 0; ch < pcm_ch; ch++)
		memset(ac->audio_buf[ch], 0, frames * sizeof(float));

	/* Additive mixing from each ping */
	bool any_playing = false;
	for (int i = 0; i < ac->total_pings && i < MAX_PINGS; i++) {
		if (!ac->ping_audio[i].playing)
			continue;

		int64_t local_start =
			ac->playback_cursor - ac->ping_audio[i].start_sample;
		bool ping_has_data = false;

		if (ac->pcm_loaded)
			mix_pcm_for_ping(ac, &ac->pcm, local_start,
					 frames, pcm_ch, &ping_has_data);
		if (ac->pcm_se_loaded)
			mix_pcm_for_ping(ac, &ac->pcm_se, local_start,
					 frames, pcm_ch, &ping_has_data);

		/* Mark ping finished when both sources are done */
		int64_t end_pos = local_start + (int64_t)frames;
		bool done = true;
		if (ac->pcm_loaded &&
		    end_pos < (int64_t)ac->pcm.total_frames)
			done = false;
		if (ac->pcm_se_loaded &&
		    end_pos < (int64_t)ac->pcm_se.total_frames)
			done = false;
		if (done)
			ac->ping_audio[i].playing = false;

		/* Keep active if ping produced data OR still has future data */
		if (ping_has_data || ac->ping_audio[i].playing)
			any_playing = true;
	}

	if (!any_playing) {
		ac->playback_active = false;
		return;
	}

	/* Send to OBS via obs_source_output_audio */
	struct obs_source_audio out = {0};
	out.frames = (uint32_t)frames;
	out.format = AUDIO_FORMAT_FLOAT_PLANAR;
	out.samples_per_sec = sample_rate;

	/* Match speakers to actual data channels */
	out.speakers = (pcm_ch >= 2) ? SPEAKERS_STEREO : SPEAKERS_MONO;

	out.timestamp = timestamp;

	for (size_t ch = 0; ch < pcm_ch; ch++)
		out.data[ch] = (const uint8_t *)ac->audio_buf[ch];

	obs_source_output_audio(ac->parent_source, &out);

	ac->playback_cursor += (int64_t)frames;
}

/* ----------------------------------------------------------------- */
/* Public API                                                         */
/* ----------------------------------------------------------------- */

void animation_controller_init(struct animation_controller *ac,
			       obs_source_t *parent)
{
	memset(ac, 0, sizeof(*ac));
	ac->parent_source = parent;
	ac->stagger_interval = 0.5f;
	ac->ping_scale = 0.35f;
	ac->se_volume = 1.0f;

	srand((unsigned int)os_gettime_ns());
}

void animation_controller_free(struct animation_controller *ac)
{
	for (int i = 0; i < MAX_PINGS; i++)
		release_source(&ac->overlay_sources[i]);
	if (ac->pcm_loaded)
		pcm_audio_free(&ac->pcm);
	if (ac->pcm_se_loaded)
		pcm_audio_free(&ac->pcm_se);
}

void animation_controller_load_overlay(struct animation_controller *ac,
				       const char *path, bool is_video)
{
	for (int i = 0; i < MAX_PINGS; i++)
		release_source(&ac->overlay_sources[i]);

	if (!path || !*path)
		return;

	ac->is_video = is_video;

	for (int i = 0; i < MAX_PINGS; i++) {
		char name[64];
		snprintf(name, sizeof(name), "mia-pin-overlay-%d", i);
		if (is_video)
			ac->overlay_sources[i] =
				create_media_source(name, path);
		else
			ac->overlay_sources[i] =
				create_image_source(name, path);
	}

	if (!ac->overlay_sources[0])
		blog(LOG_WARNING,
		     "[lol-death-mia-pin] failed to create overlay: %s", path);
}

void animation_controller_load_audio(struct animation_controller *ac,
				     const char *path)
{
	if (ac->pcm_loaded) {
		pcm_audio_free(&ac->pcm);
		ac->pcm_loaded = false;
	}
	if (!path || !*path)
		return;

	if (pcm_audio_load(&ac->pcm, path)) {
		ac->pcm_loaded = true;
	} else {
		blog(LOG_WARNING,
		     "[lol-death-mia-pin] failed to decode audio: %s", path);
	}
}

void animation_controller_load_se_audio(struct animation_controller *ac,
					const char *path)
{
	if (ac->pcm_se_loaded) {
		pcm_audio_free(&ac->pcm_se);
		ac->pcm_se_loaded = false;
	}
	if (!path || !*path)
		return;

	if (pcm_audio_load(&ac->pcm_se, path)) {
		ac->pcm_se_loaded = true;
	} else {
		blog(LOG_WARNING,
		     "[lol-death-mia-pin] failed to decode SE: %s", path);
	}
}

/* ---- Lifecycle helpers ------------------------------------------ */

void animation_controller_inc_active(struct animation_controller *ac)
{
	for (int i = 0; i < MAX_PINGS; i++) {
		if (ac->overlay_sources[i])
			obs_source_inc_active(ac->overlay_sources[i]);
	}
}

void animation_controller_dec_active(struct animation_controller *ac)
{
	for (int i = 0; i < MAX_PINGS; i++) {
		if (ac->overlay_sources[i])
			obs_source_dec_active(ac->overlay_sources[i]);
	}
}

void animation_controller_inc_showing(struct animation_controller *ac)
{
	for (int i = 0; i < MAX_PINGS; i++) {
		if (ac->overlay_sources[i])
			obs_source_inc_showing(ac->overlay_sources[i]);
	}
}

void animation_controller_dec_showing(struct animation_controller *ac)
{
	for (int i = 0; i < MAX_PINGS; i++) {
		if (ac->overlay_sources[i])
			obs_source_dec_showing(ac->overlay_sources[i]);
	}
}

void animation_controller_set_volume(struct animation_controller *ac,
				     float volume)
{
	ac->se_volume = volume;
	/* Mute overlay sources -- we only use them for video */
	for (int i = 0; i < MAX_PINGS; i++) {
		if (ac->overlay_sources[i])
			obs_source_set_volume(ac->overlay_sources[i], 0.0f);
	}
}

/* ---- Animation ------------------------------------------------- */

void animation_controller_start(struct animation_controller *ac,
				int ping_count,
				uint32_t source_cx, uint32_t source_cy)
{
	if (!ac->overlay_sources[0]) {
		ac->active = false;
		return;
	}

	if (ping_count <= 0)
		ping_count = 1;
	if (ping_count > MAX_PINGS)
		ping_count = MAX_PINGS;

	ac->total_pings = ping_count;
	ac->active = true;
	ac->render_cx = source_cx;

	generate_positions(ac, ping_count, source_cx, source_cy);

	/* Stop all overlay sources to ensure clean state */
	for (int i = 0; i < MAX_PINGS; i++) {
		if (ac->overlay_sources[i])
			obs_source_media_stop(ac->overlay_sources[i]);
	}

	/* Initialize audio playback state */
	ac->playback_cursor = 0;
	ac->playback_start_ts = os_gettime_ns();
	ac->next_audio_ts = ac->playback_start_ts;
	ac->playback_active = ac->pcm_loaded || ac->pcm_se_loaded;

	struct obs_audio_info oai;
	uint32_t sr = obs_get_audio_info(&oai) ? oai.samples_per_sec : 48000;

	/* Send a silent primer chunk to reset OBS audio timestamp tracking.
	 * Without this, the second animation's timestamps can be rejected
	 * by OBS's internal smoothing logic after the first animation's
	 * audio buffer has been drained. */
	if (ac->playback_active && ac->parent_source) {
		size_t pcm_ch = 0;
		if (ac->pcm_loaded)
			pcm_ch = ac->pcm.channels;
		if (ac->pcm_se_loaded && ac->pcm_se.channels > pcm_ch)
			pcm_ch = ac->pcm_se.channels;
		if (pcm_ch == 0)
			pcm_ch = 2;

		/* Use the existing audio_buf (zeroed) as silence source
		 * to avoid large stack allocation */
		for (size_t ch = 0; ch < pcm_ch && ch < MAX_AV_PLANES; ch++)
			memset(ac->audio_buf[ch], 0,
			       AUDIO_CHUNK_FRAMES * sizeof(float));

		struct obs_source_audio prime = {0};
		prime.frames = AUDIO_CHUNK_FRAMES;
		prime.format = AUDIO_FORMAT_FLOAT_PLANAR;
		prime.samples_per_sec = sr;
		prime.speakers = (pcm_ch >= 2) ? SPEAKERS_STEREO
						: SPEAKERS_MONO;
		prime.timestamp = ac->playback_start_ts;
		for (size_t ch = 0; ch < pcm_ch && ch < MAX_AV_PLANES; ch++)
			prime.data[ch] =
				(const uint8_t *)ac->audio_buf[ch];
		obs_source_output_audio(ac->parent_source, &prime);
	}

	/* Set each ping's start sample based on stagger interval */
	for (int i = 0; i < ping_count && i < MAX_PINGS; i++) {
		ac->ping_audio[i].playing = true;
		ac->ping_audio[i].start_sample =
			(int64_t)((float)i * ac->stagger_interval * (float)sr);
	}
	for (int i = ping_count; i < MAX_PINGS; i++) {
		ac->ping_audio[i].playing = false;
		ac->ping_audio[i].start_sample = 0;
	}

	/* Spawn first overlay. Remaining spawn in tick. */
	ac->pings_spawned = 1;
	ac->spawn_elapsed = 0.0f;
	ac->overlay_cx = 0;
	ac->overlay_cy = 0;

	ac->anim_elapsed = 0.0f;
	memset(ac->ping_spawn_time, 0, sizeof(ac->ping_spawn_time));
	ac->ping_spawn_time[0] = 0.0f;

	if (ac->overlay_sources[0])
		obs_source_media_restart(ac->overlay_sources[0]);
}

bool animation_controller_tick(struct animation_controller *ac, float seconds)
{
	if (!ac->active)
		return false;

	ac->anim_elapsed += seconds;

	/* Spawn additional pings */
	if (ac->pings_spawned < ac->total_pings) {
		ac->spawn_elapsed += seconds;
		while (ac->pings_spawned < ac->total_pings &&
		       ac->spawn_elapsed >= ac->stagger_interval) {
			ac->spawn_elapsed -= ac->stagger_interval;

			int idx = ac->pings_spawned;
			ac->pings_spawned++;
			ac->ping_spawn_time[idx] = ac->anim_elapsed;

			if (ac->overlay_sources[idx])
				obs_source_media_restart(
					ac->overlay_sources[idx]);
		}
	}

	/* Output audio via obs_source_output_audio */
	if (ac->playback_active && (ac->pcm_loaded || ac->pcm_se_loaded)) {
		uint32_t sr = ac->pcm_loaded
				      ? ac->pcm.sample_rate
				      : ac->pcm_se.sample_rate;

		if (ac->is_video) {
			/* Video mode: original logic (unchanged) */
			size_t frames = (size_t)((float)sr * seconds);
			if (frames < 1)
				frames = 1;
			if (frames > AUDIO_CHUNK_FRAMES)
				frames = AUDIO_CHUNK_FRAMES;

			uint64_t ts = ac->playback_start_ts +
				      (uint64_t)ac->playback_cursor *
					      1000000000ULL / (uint64_t)sr;

			output_audio_chunk(ac, frames, ts);
		} else {
			/* Image mode: wall-clock based sample count.
			 * Outputs every tick (no skips) and avoids
			 * float truncation drift of the seconds approach
			 * by using absolute elapsed time. */
			uint64_t now = os_gettime_ns();
			uint64_t elapsed = now - ac->playback_start_ts;
			int64_t expected = (int64_t)(
				(double)elapsed * (double)sr /
				1000000000.0);
			int64_t needed = expected - ac->playback_cursor;
			if (needed > 0) {
				if (needed > AUDIO_CHUNK_FRAMES)
					needed = AUDIO_CHUNK_FRAMES;
				uint64_t ts = ac->playback_start_ts +
					      (uint64_t)ac->playback_cursor *
						      1000000000ULL /
						      (uint64_t)sr;
				output_audio_chunk(ac, (size_t)needed, ts);
			}
		}
	}

	/* Check animation completion (grace period for media startup) */
	if (ac->pings_spawned >= ac->total_pings &&
	    ac->anim_elapsed > 0.5f &&
	    !animation_controller_any_visible(ac) &&
	    !ac->playback_active) {
		ac->active = false;
		for (int i = 0; i < ac->total_pings; i++)
			if (ac->overlay_sources[i])
				obs_source_media_stop(
					ac->overlay_sources[i]);
		return false;
	}

	return true;
}

static bool is_media_playing(obs_source_t *src)
{
	if (!src)
		return false;
	enum obs_media_state state = obs_source_media_get_state(src);
	return state == OBS_MEDIA_STATE_PLAYING ||
	       state == OBS_MEDIA_STATE_OPENING ||
	       state == OBS_MEDIA_STATE_BUFFERING;
}

/* Time-based visibility for image mode (uses accumulated seconds,
 * not os_gettime_ns, to avoid platform timer discontinuities) */
static bool is_image_ping_visible(struct animation_controller *ac, int i)
{
	if (i >= ac->pings_spawned)
		return false;
	float since_spawn = ac->anim_elapsed - ac->ping_spawn_time[i];
	return since_spawn >= 0.0f && since_spawn < ac->image_duration;
}

bool animation_controller_any_visible(struct animation_controller *ac)
{
	if (!ac->active)
		return false;
	for (int i = 0; i < ac->pings_spawned && i < MAX_PINGS; i++) {
		if (ac->is_video) {
			if (is_media_playing(ac->overlay_sources[i]))
				return true;
		} else {
			if (is_image_ping_visible(ac, i))
				return true;
		}
	}
	return false;
}

/* ---- Video render ---------------------------------------------- */

void animation_controller_render(struct animation_controller *ac)
{
	if (!ac->active)
		return;

	if (ac->overlay_cx == 0 || ac->overlay_cy == 0) {
		for (int i = 0; i < ac->pings_spawned && i < MAX_PINGS; i++) {
			if (!ac->overlay_sources[i])
				continue;
			uint32_t cx = obs_source_get_width(ac->overlay_sources[i]);
			uint32_t cy = obs_source_get_height(ac->overlay_sources[i]);
			if (cx > 0 && cy > 0) {
				ac->overlay_cx = cx;
				ac->overlay_cy = cy;
				break;
			}
		}
	}
	if (ac->overlay_cx == 0 || ac->overlay_cy == 0)
		return;

	uint32_t img_cx = ac->overlay_cx;
	uint32_t img_cy = ac->overlay_cy;

	float target_w = (float)ac->render_cx * ac->ping_scale;
	float scale = target_w / (float)img_cx;
	float scaled_cx = (float)img_cx * scale;
	float scaled_cy = (float)img_cy * scale;

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

	for (int i = 0; i < ac->pings_spawned && i < MAX_PINGS; i++) {
		if (ac->is_video) {
			if (!is_media_playing(ac->overlay_sources[i]))
				continue;
		} else {
			if (!is_image_ping_visible(ac, i))
				continue;
		}

		struct ping_position *pos = &ac->positions[i];
		float draw_x = pos->x - scaled_cx / 2.0f;
		float draw_y = pos->y - scaled_cy / 2.0f;

		gs_matrix_push();
		gs_matrix_translate3f(draw_x, draw_y, 0.0f);
		gs_matrix_scale3f(scale, scale, 1.0f);
		obs_source_video_render(ac->overlay_sources[i]);
		gs_matrix_pop();
	}

	gs_blend_state_pop();
}
