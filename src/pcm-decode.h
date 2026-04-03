#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct pcm_audio {
	float **data;        /* data[channel][sample] -- planar float */
	size_t channels;
	size_t total_frames; /* total samples per channel */
	uint32_t sample_rate;
};

/* Decode entire file to float planar PCM at OBS output sample rate.
 * Returns true on success. */
bool pcm_audio_load(struct pcm_audio *pcm, const char *path);

/* Free decoded PCM data. */
void pcm_audio_free(struct pcm_audio *pcm);
