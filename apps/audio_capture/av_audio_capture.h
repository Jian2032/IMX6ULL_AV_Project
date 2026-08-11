#ifndef AV_AUDIO_CAPTURE_H
#define AV_AUDIO_CAPTURE_H

#include <stddef.h>
#include <stdint.h>

/* Fixed stream profile shared by capture, encoder and A/V synchronization. */
#define AV_AUDIO_SAMPLE_RATE        48000U
#define AV_AUDIO_CHANNELS           2U
#define AV_AUDIO_SAMPLE_BITS        16U
#define AV_AUDIO_BYTES_PER_SAMPLE   (AV_AUDIO_SAMPLE_BITS / 8U)
#define AV_AUDIO_BYTES_PER_FRAME    \
	(AV_AUDIO_CHANNELS * AV_AUDIO_BYTES_PER_SAMPLE)
#define AV_AUDIO_PERIOD_FRAMES      1024U
#define AV_AUDIO_PERIOD_COUNT       4U
#define AV_AUDIO_DEFAULT_RING_SLOTS 16U

/* Non-error return values from av_audio_read(). */
#define AV_AUDIO_TIMEOUT 1
#define AV_AUDIO_STOPPED 2

struct av_audio_capture;

struct av_audio_config {
	const char *pcm_device;
	const char *control_device;
	unsigned int ring_slots;
};

/* Metadata belonging to the PCM copied by one av_audio_read() call. */
struct av_audio_packet_info {
	uint64_t sequence;
	uint64_t first_frame;
	uint64_t timestamp_us;
	unsigned int frames;
};

struct av_audio_stats {
	uint64_t captured_packets;
	uint64_t captured_frames;
	uint64_t consumed_packets;
	uint64_t consumed_frames;
	uint64_t dropped_packets;
	uint64_t dropped_frames;
	unsigned int xruns;
	unsigned int ring_high_watermark;
	unsigned int ring_slots;
	unsigned int ring_queued;
	int capture_error;
};

/*
 * open  : configure the main-MIC route and PREPARE the PCM.
 * start : create the producer; it explicitly STARTs SAI/SDMA before poll/read.
 * read  : copy one complete ring packet into caller-owned memory.
 * stop  : stop/join the producer and issue PCM DROP.
 * close : HW_FREE, restore the mixer and release every resource.
 */
int av_audio_open(struct av_audio_capture **capture_out,
		  const struct av_audio_config *config);
int av_audio_start(struct av_audio_capture *capture);
int av_audio_read(struct av_audio_capture *capture,
		  int16_t *destination,
		  unsigned int destination_capacity_frames,
		  struct av_audio_packet_info *packet,
		  unsigned int timeout_ms);
int av_audio_stop(struct av_audio_capture *capture);
void av_audio_close(struct av_audio_capture *capture);
void av_audio_get_stats(struct av_audio_capture *capture,
			struct av_audio_stats *stats);
unsigned int av_audio_period_frames(const struct av_audio_capture *capture);

#endif
