#include "pcm-decode.h"

#include <obs-module.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>

#include <stdlib.h>
#include <string.h>

/* Dynamic array for accumulating decoded frames */
struct frame_buf {
	float **data;     /* data[ch][...] */
	size_t channels;
	size_t capacity;  /* allocated frames per channel */
	size_t count;     /* written frames per channel */
};

static bool frame_buf_init(struct frame_buf *fb, size_t channels)
{
	fb->channels = channels;
	fb->capacity = 48000; /* ~1 second initial */
	fb->count = 0;
	fb->data = calloc(channels, sizeof(float *));
	if (!fb->data)
		return false;
	for (size_t ch = 0; ch < channels; ch++) {
		fb->data[ch] = malloc(fb->capacity * sizeof(float));
		if (!fb->data[ch])
			return false;
	}
	return true;
}

static bool frame_buf_ensure(struct frame_buf *fb, size_t additional)
{
	size_t needed = fb->count + additional;
	if (needed <= fb->capacity)
		return true;

	size_t new_cap = fb->capacity;
	while (new_cap < needed)
		new_cap *= 2;

	for (size_t ch = 0; ch < fb->channels; ch++) {
		float *tmp = realloc(fb->data[ch], new_cap * sizeof(float));
		if (!tmp)
			return false;
		fb->data[ch] = tmp;
	}
	fb->capacity = new_cap;
	return true;
}

static void frame_buf_free(struct frame_buf *fb)
{
	if (!fb->data)
		return;
	for (size_t ch = 0; ch < fb->channels; ch++)
		free(fb->data[ch]);
	free(fb->data);
	fb->data = NULL;
}

bool pcm_audio_load(struct pcm_audio *pcm, const char *path)
{
	memset(pcm, 0, sizeof(*pcm));

	if (!path || !*path)
		return false;

	/* Determine target sample rate from OBS */
	struct obs_audio_info oai;
	uint32_t target_sr = obs_get_audio_info(&oai)
				     ? oai.samples_per_sec
				     : 48000;

	AVFormatContext *fmt_ctx = NULL;
	int ret = avformat_open_input(&fmt_ctx, path, NULL, NULL);
	if (ret < 0) {
		blog(LOG_WARNING,
		     "[pcm-decode] avformat_open_input failed: %d", ret);
		return false;
	}

	ret = avformat_find_stream_info(fmt_ctx, NULL);
	if (ret < 0) {
		blog(LOG_WARNING,
		     "[pcm-decode] avformat_find_stream_info failed: %d", ret);
		avformat_close_input(&fmt_ctx);
		return false;
	}

	/* Find audio stream */
	int audio_idx = -1;
	for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
		if (fmt_ctx->streams[i]->codecpar->codec_type ==
		    AVMEDIA_TYPE_AUDIO) {
			audio_idx = (int)i;
			break;
		}
	}
	if (audio_idx < 0) {
		blog(LOG_WARNING, "[pcm-decode] no audio stream in: %s", path);
		avformat_close_input(&fmt_ctx);
		return false;
	}

	AVCodecParameters *codecpar = fmt_ctx->streams[audio_idx]->codecpar;
	const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
	if (!codec) {
		blog(LOG_WARNING, "[pcm-decode] decoder not found for codec %d",
		     codecpar->codec_id);
		avformat_close_input(&fmt_ctx);
		return false;
	}

	AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
	if (!codec_ctx) {
		avformat_close_input(&fmt_ctx);
		return false;
	}

	avcodec_parameters_to_context(codec_ctx, codecpar);
	ret = avcodec_open2(codec_ctx, codec, NULL);
	if (ret < 0) {
		blog(LOG_WARNING,
		     "[pcm-decode] avcodec_open2 failed: %d", ret);
		avcodec_free_context(&codec_ctx);
		avformat_close_input(&fmt_ctx);
		return false;
	}

	/* Determine source channel layout */
	AVChannelLayout src_ch_layout;
	if (codec_ctx->ch_layout.nb_channels > 0) {
		av_channel_layout_copy(&src_ch_layout, &codec_ctx->ch_layout);
	} else {
		av_channel_layout_default(&src_ch_layout, 2);
	}

	int out_channels = src_ch_layout.nb_channels;
	if (out_channels > 8)
		out_channels = 8;

	AVChannelLayout dst_ch_layout;
	av_channel_layout_default(&dst_ch_layout, out_channels);

	/* Setup resampler: convert to float planar at target sample rate */
	SwrContext *swr = NULL;
	ret = swr_alloc_set_opts2(&swr, &dst_ch_layout, AV_SAMPLE_FMT_FLTP,
				  (int)target_sr, &src_ch_layout,
				  codec_ctx->sample_fmt,
				  codec_ctx->sample_rate, 0, NULL);
	if (ret < 0 || !swr) {
		blog(LOG_WARNING,
		     "[pcm-decode] swr_alloc_set_opts2 failed: %d", ret);
		av_channel_layout_uninit(&src_ch_layout);
		av_channel_layout_uninit(&dst_ch_layout);
		avcodec_free_context(&codec_ctx);
		avformat_close_input(&fmt_ctx);
		return false;
	}

	ret = swr_init(swr);
	if (ret < 0) {
		blog(LOG_WARNING, "[pcm-decode] swr_init failed: %d", ret);
		swr_free(&swr);
		av_channel_layout_uninit(&src_ch_layout);
		av_channel_layout_uninit(&dst_ch_layout);
		avcodec_free_context(&codec_ctx);
		avformat_close_input(&fmt_ctx);
		return false;
	}

	/* Accumulation buffer */
	struct frame_buf fb;
	if (!frame_buf_init(&fb, (size_t)out_channels)) {
		swr_free(&swr);
		av_channel_layout_uninit(&src_ch_layout);
		av_channel_layout_uninit(&dst_ch_layout);
		avcodec_free_context(&codec_ctx);
		avformat_close_input(&fmt_ctx);
		return false;
	}

	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	bool success = true;

	while (av_read_frame(fmt_ctx, pkt) >= 0) {
		if (pkt->stream_index != audio_idx) {
			av_packet_unref(pkt);
			continue;
		}

		ret = avcodec_send_packet(codec_ctx, pkt);
		av_packet_unref(pkt);
		if (ret < 0)
			continue;

		while (avcodec_receive_frame(codec_ctx, frame) == 0) {
			/* Estimate output frames after resampling */
			int out_frames = swr_get_out_samples(swr,
							     frame->nb_samples);
			if (out_frames <= 0) {
				av_frame_unref(frame);
				continue;
			}

			if (!frame_buf_ensure(&fb, (size_t)out_frames)) {
				success = false;
				av_frame_unref(frame);
				goto decode_done;
			}

			/* Build output pointer array at current write pos */
			uint8_t *out_ptrs[8];
			for (int ch = 0; ch < out_channels; ch++)
				out_ptrs[ch] =
					(uint8_t *)&fb.data[ch][fb.count];

			int converted = swr_convert(
				swr, out_ptrs, out_frames,
				(const uint8_t **)frame->extended_data,
				frame->nb_samples);

			if (converted > 0)
				fb.count += (size_t)converted;

			av_frame_unref(frame);
		}
	}

	/* Flush decoder */
	avcodec_send_packet(codec_ctx, NULL);
	while (avcodec_receive_frame(codec_ctx, frame) == 0) {
		int out_frames = swr_get_out_samples(swr, frame->nb_samples);
		if (out_frames <= 0) {
			av_frame_unref(frame);
			continue;
		}
		if (!frame_buf_ensure(&fb, (size_t)out_frames)) {
			success = false;
			av_frame_unref(frame);
			goto decode_done;
		}
		uint8_t *out_ptrs[8];
		for (int ch = 0; ch < out_channels; ch++)
			out_ptrs[ch] = (uint8_t *)&fb.data[ch][fb.count];
		int converted =
			swr_convert(swr, out_ptrs, out_frames,
				    (const uint8_t **)frame->extended_data,
				    frame->nb_samples);
		if (converted > 0)
			fb.count += (size_t)converted;
		av_frame_unref(frame);
	}

	/* Flush resampler */
	{
		int delay = (int)swr_get_delay(swr, (int64_t)target_sr);
		if (delay > 0) {
			if (frame_buf_ensure(&fb, (size_t)delay)) {
				uint8_t *out_ptrs[8];
				for (int ch = 0; ch < out_channels; ch++)
					out_ptrs[ch] = (uint8_t *)&fb
								.data[ch][fb.count];
				int converted = swr_convert(swr, out_ptrs,
							    delay, NULL, 0);
				if (converted > 0)
					fb.count += (size_t)converted;
			}
		}
	}

decode_done:
	av_frame_free(&frame);
	av_packet_free(&pkt);
	swr_free(&swr);
	av_channel_layout_uninit(&src_ch_layout);
	av_channel_layout_uninit(&dst_ch_layout);
	avcodec_free_context(&codec_ctx);
	avformat_close_input(&fmt_ctx);

	if (!success || fb.count == 0) {
		frame_buf_free(&fb);
		return false;
	}

	/* Transfer ownership to pcm_audio */
	pcm->data = fb.data;
	pcm->channels = fb.channels;
	pcm->total_frames = fb.count;
	pcm->sample_rate = target_sr;
	return true;
}

void pcm_audio_free(struct pcm_audio *pcm)
{
	if (!pcm->data)
		return;
	for (size_t ch = 0; ch < pcm->channels; ch++)
		free(pcm->data[ch]);
	free(pcm->data);
	pcm->data = NULL;
	pcm->channels = 0;
	pcm->total_frames = 0;
}
