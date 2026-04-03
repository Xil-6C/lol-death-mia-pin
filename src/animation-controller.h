#pragma once

#include <obs-module.h>
#include "pcm-decode.h"

/* ----------------------------------------------------------------- */
/* Animation controller -- MIA pin overlay + SE playback               */
/* ----------------------------------------------------------------- */

#define MAX_PINGS 10
#define AUDIO_CHUNK_FRAMES 1024

struct ping_position {
	float x;
	float y;
};

struct animation_controller {
	/* Parent source -- audio output target */
	obs_source_t *parent_source;

	/* Overlay media sources (one private ffmpeg_source per ping) */
	obs_source_t *overlay_sources[MAX_PINGS];

	/* Pre-decoded PCM audio (primary: video audio or SE) */
	struct pcm_audio pcm;
	bool pcm_loaded;

	/* Pre-decoded PCM audio (SE overlay: used when video + SE both set) */
	struct pcm_audio pcm_se;
	bool pcm_se_loaded;

	/* Per-ping audio playback state */
	struct {
		bool playing;
		int64_t start_sample; /* offset on global cursor */
	} ping_audio[MAX_PINGS];

	/* Global playback cursor (managed by tick) */
	int64_t playback_cursor;
	uint64_t playback_start_ts; /* os_gettime_ns() at start */
	bool playback_active;
	float se_volume;

	/* Audio output scratch buffer (reused each tick) */
	float audio_buf[MAX_AV_PLANES][AUDIO_CHUNK_FRAMES];

	/* Animation state */
	bool active;
	int total_pings;
	int pings_spawned;
	float spawn_elapsed;
	float stagger_interval;   /* seconds between ping spawns */

	/* Rendering */
	uint32_t render_cx;   /* source width -- for scale calculation */
	float ping_scale;     /* fraction of source width per ping */
	uint32_t overlay_cx;  /* cached overlay media dimensions */
	uint32_t overlay_cy;

	/* Pre-generated random positions */
	struct ping_position positions[MAX_PINGS];
};

void animation_controller_init(struct animation_controller *ac,
			       obs_source_t *parent);
void animation_controller_free(struct animation_controller *ac);

void animation_controller_load_overlay(struct animation_controller *ac,
				       const char *path);
void animation_controller_load_audio(struct animation_controller *ac,
				     const char *path);
void animation_controller_load_se_audio(struct animation_controller *ac,
					const char *path);

/* Caller-managed lifecycle for private child sources */
void animation_controller_inc_active(struct animation_controller *ac);
void animation_controller_dec_active(struct animation_controller *ac);
void animation_controller_inc_showing(struct animation_controller *ac);
void animation_controller_dec_showing(struct animation_controller *ac);

void animation_controller_set_volume(struct animation_controller *ac,
				     float volume);

void animation_controller_start(struct animation_controller *ac,
				int ping_count,
				uint32_t source_cx, uint32_t source_cy);

bool animation_controller_tick(struct animation_controller *ac, float seconds);
bool animation_controller_any_visible(struct animation_controller *ac);
void animation_controller_render(struct animation_controller *ac);
