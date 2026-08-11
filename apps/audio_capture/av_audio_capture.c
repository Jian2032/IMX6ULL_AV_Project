/*
 * av_audio_capture.c - AUDIO-R3 reusable threaded ALSA capture layer
 *
 * The BusyBox RootFS has no alsa-lib, so this module talks to the Linux ALSA
 * UAPI directly.  The producer thread performs only bounded PCM work; file,
 * codec and network operations belong to consumers outside this module.
 */

#include "av_audio_capture.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <sound/asound.h>

#define AV_MAX_CONTROL_IDS    4096U
#define AV_MAX_SAVED_CONTROLS 8U
#define AV_MIN_RING_SLOTS     2U
#define AV_MAX_RING_SLOTS     128U
#define AV_PCM_POLL_MS        200

#define CAPTURE_SWITCH_NAME "Capture Switch"
#define LINPUT1_SWITCH_NAME "Left Boost Mixer LINPUT1 Switch"
#define LINPUT2_SWITCH_NAME "Left Boost Mixer LINPUT2 Switch"
#define LINPUT3_SWITCH_NAME "Left Boost Mixer LINPUT3 Switch"
#define LEFT_BOOST_NAME     "Left Input Mixer Boost Switch"
#define LINPUT2_VOLUME_NAME "Left Input Boost Mixer LINPUT2 Volume"
#define LINPUT3_VOLUME_NAME "Left Input Boost Mixer LINPUT3 Volume"

struct saved_control {
	char name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN + 1];
	unsigned int value_count;
	int changed;
	struct snd_ctl_elem_value original;
};

struct mixer_transaction {
	int fd;
	unsigned int saved_count;
	struct saved_control saved[AV_MAX_SAVED_CONTROLS];
};

struct audio_slot {
	int16_t *samples;
	struct av_audio_packet_info info;
};

struct av_audio_capture {
	int pcm_fd;
	int hw_configured;
	int prepared;
	unsigned int period_frames;
	unsigned int period_count;
	unsigned int buffer_frames;

	struct mixer_transaction mixer;
	struct audio_slot *slots;
	int16_t *ring_storage;
	int16_t *period_buffer;
	unsigned int ring_capacity;
	unsigned int read_index;
	unsigned int write_index;
	unsigned int queued;

	pthread_mutex_t lock;
	pthread_cond_t data_ready;
	int lock_initialized;
	int cond_initialized;
	pthread_t thread;
	int thread_started;
	int stop_requested;
	int producer_finished;
	int capture_error;

	uint64_t next_sequence;
	uint64_t next_first_frame;
	struct av_audio_stats stats;
};

static int xioctl(int fd, unsigned long request, void *argument)
{
	int ret;

	do {
		ret = ioctl(fd, request, argument);
	} while (ret < 0 && errno == EINTR);
	return ret;
}

static int xioctl_noarg(int fd, unsigned long request)
{
	int ret;

	do {
		ret = ioctl(fd, request);
	} while (ret < 0 && errno == EINTR);
	return ret;
}

static uint64_t monotonic_us(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return 0;
	return (uint64_t)now.tv_sec * 1000000ULL +
	       (uint64_t)now.tv_nsec / 1000ULL;
}

static struct snd_mask *param_mask(struct snd_pcm_hw_params *params,
				   unsigned int parameter)
{
	return &params->masks[parameter - SNDRV_PCM_HW_PARAM_FIRST_MASK];
}

static struct snd_interval *param_interval(struct snd_pcm_hw_params *params,
					   unsigned int parameter)
{
	return &params->intervals[parameter - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
}

static const struct snd_interval *param_interval_const(
				const struct snd_pcm_hw_params *params,
				unsigned int parameter)
{
	return &params->intervals[parameter - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
}

/* Build the unconstrained parameter set expected by HW_PARAMS. */
static void hw_params_any(struct snd_pcm_hw_params *params)
{
	unsigned int parameter;

	memset(params, 0, sizeof(*params));
	for (parameter = SNDRV_PCM_HW_PARAM_FIRST_MASK;
	     parameter <= SNDRV_PCM_HW_PARAM_LAST_MASK; ++parameter)
		memset(param_mask(params, parameter)->bits, 0xff,
		       sizeof(param_mask(params, parameter)->bits));

	for (parameter = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL;
	     parameter <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; ++parameter) {
		struct snd_interval *interval = param_interval(params, parameter);

		interval->min = 0;
		interval->max = UINT_MAX;
	}
	params->rmask = ~0U;
	params->info = ~0U;
}

static void set_mask_value(struct snd_pcm_hw_params *params,
			   unsigned int parameter, unsigned int value)
{
	struct snd_mask *mask = param_mask(params, parameter);

	memset(mask->bits, 0, sizeof(mask->bits));
	mask->bits[value / 32U] = 1U << (value % 32U);
}

static void set_interval_value(struct snd_pcm_hw_params *params,
			       unsigned int parameter, unsigned int value)
{
	struct snd_interval *interval = param_interval(params, parameter);

	memset(interval, 0, sizeof(*interval));
	interval->min = value;
	interval->max = value;
	interval->integer = 1;
}

static unsigned int get_interval_value(const struct snd_pcm_hw_params *params,
				       unsigned int parameter)
{
	return param_interval_const(params, parameter)->min;
}

static int configure_hardware(struct av_audio_capture *capture)
{
	struct snd_pcm_hw_params params;
	unsigned int rate;
	unsigned int channels;

	hw_params_any(&params);
	set_mask_value(&params, SNDRV_PCM_HW_PARAM_ACCESS,
		       (unsigned int)SNDRV_PCM_ACCESS_RW_INTERLEAVED);
	set_mask_value(&params, SNDRV_PCM_HW_PARAM_FORMAT,
		       (unsigned int)SNDRV_PCM_FORMAT_S16_LE);
	set_mask_value(&params, SNDRV_PCM_HW_PARAM_SUBFORMAT,
		       (unsigned int)SNDRV_PCM_SUBFORMAT_STD);
	set_interval_value(&params, SNDRV_PCM_HW_PARAM_CHANNELS,
			   AV_AUDIO_CHANNELS);
	set_interval_value(&params, SNDRV_PCM_HW_PARAM_RATE,
			   AV_AUDIO_SAMPLE_RATE);
	set_interval_value(&params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
			   AV_AUDIO_PERIOD_FRAMES);
	set_interval_value(&params, SNDRV_PCM_HW_PARAM_PERIODS,
			   AV_AUDIO_PERIOD_COUNT);

	if (xioctl(capture->pcm_fd, SNDRV_PCM_IOCTL_HW_PARAMS, &params) < 0) {
		fprintf(stderr, "SNDRV_PCM_IOCTL_HW_PARAMS failed: %s\n",
			strerror(errno));
		return -1;
	}
	capture->hw_configured = 1;

	rate = get_interval_value(&params, SNDRV_PCM_HW_PARAM_RATE);
	channels = get_interval_value(&params, SNDRV_PCM_HW_PARAM_CHANNELS);
	capture->period_frames = get_interval_value(
		&params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
	capture->period_count = get_interval_value(
		&params, SNDRV_PCM_HW_PARAM_PERIODS);
	capture->buffer_frames = get_interval_value(
		&params, SNDRV_PCM_HW_PARAM_BUFFER_SIZE);
	if (rate != AV_AUDIO_SAMPLE_RATE || channels != AV_AUDIO_CHANNELS ||
	    capture->period_frames == 0 || capture->period_count < 2 ||
	    capture->buffer_frames == 0) {
		fprintf(stderr, "Driver returned an unexpected PCM configuration\n");
		errno = EINVAL;
		return -1;
	}

	printf("audio hardware : %u Hz, %u ch, S16_LE\n", rate, channels);
	printf("  period       : %u frames, %.3f ms, %u bytes\n",
	       capture->period_frames,
	       (double)capture->period_frames * 1000.0 / rate,
	       capture->period_frames * AV_AUDIO_BYTES_PER_FRAME);
	printf("  kernel ring  : %u periods, %u frames, %.3f ms\n",
	       capture->period_count, capture->buffer_frames,
	       (double)capture->buffer_frames * 1000.0 / rate);
	return 0;
}

static int configure_software(struct av_audio_capture *capture)
{
	struct snd_pcm_sw_params params;

	memset(&params, 0, sizeof(params));
	params.tstamp_mode = SNDRV_PCM_TSTAMP_ENABLE;
	params.period_step = 1;
	params.avail_min = capture->period_frames;
	params.xfer_align = 1;
	params.start_threshold = 1;
	params.stop_threshold = capture->buffer_frames;
	params.proto = SNDRV_PCM_VERSION;
	params.tstamp_type = SNDRV_PCM_TSTAMP_TYPE_GETTIMEOFDAY;
	if (xioctl(capture->pcm_fd, SNDRV_PCM_IOCTL_SW_PARAMS, &params) < 0) {
		fprintf(stderr, "SNDRV_PCM_IOCTL_SW_PARAMS failed: %s\n",
			strerror(errno));
		return -1;
	}
	return 0;
}

static int find_control_id(int fd, const char *wanted,
			   struct snd_ctl_elem_id *found)
{
	struct snd_ctl_elem_list list;
	struct snd_ctl_elem_id *ids = NULL;
	unsigned int i;
	int ret = -1;

	memset(&list, 0, sizeof(list));
	if (xioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0)
		return -1;
	if (list.count == 0 || list.count > AV_MAX_CONTROL_IDS) {
		errno = ENOENT;
		return -1;
	}
	ids = calloc(list.count, sizeof(*ids));
	if (!ids)
		return -1;
	list.space = list.count;
	list.pids = ids;
	if (xioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0)
		goto out;

	for (i = 0; i < list.used; ++i) {
		char name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN + 1];

		memcpy(name, ids[i].name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
		name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN] = '\0';
		if (ids[i].iface == SNDRV_CTL_ELEM_IFACE_MIXER &&
		    strcmp(name, wanted) == 0) {
			*found = ids[i];
			ret = 0;
			break;
		}
	}
	if (ret < 0)
		errno = ENOENT;
out:
	free(ids);
	return ret;
}

static void print_values(const long *values, unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; ++i)
		printf("%s%ld", i ? "," : "", values[i]);
}

/* Save before writing so setup is fully reversible on every error path. */
static int set_control(struct mixer_transaction *transaction,
		       const char *name, const long *values,
		       unsigned int value_count)
{
	struct snd_ctl_elem_info info;
	struct snd_ctl_elem_value desired;
	struct snd_ctl_elem_value verify;
	struct snd_ctl_elem_id id;
	struct saved_control *saved;
	unsigned int i;

	if (transaction->saved_count >= AV_MAX_SAVED_CONTROLS) {
		errno = ENOSPC;
		return -1;
	}
	memset(&id, 0, sizeof(id));
	if (find_control_id(transaction->fd, name, &id) < 0) {
		fprintf(stderr, "Cannot find mixer control '%s'\n", name);
		return -1;
	}
	memset(&info, 0, sizeof(info));
	info.id = id;
	if (xioctl(transaction->fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0)
		return -1;
	if ((info.type != SNDRV_CTL_ELEM_TYPE_BOOLEAN &&
	     info.type != SNDRV_CTL_ELEM_TYPE_INTEGER) ||
	    info.count != value_count ||
	    !(info.access & SNDRV_CTL_ELEM_ACCESS_WRITE)) {
		fprintf(stderr, "Mixer control '%s' has unexpected metadata\n", name);
		errno = EINVAL;
		return -1;
	}
	for (i = 0; i < value_count; ++i) {
		if (values[i] < info.value.integer.min ||
		    values[i] > info.value.integer.max) {
			errno = ERANGE;
			return -1;
		}
	}

	saved = &transaction->saved[transaction->saved_count];
	memset(saved, 0, sizeof(*saved));
	strncpy(saved->name, name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
	saved->name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN] = '\0';
	saved->value_count = value_count;
	saved->original.id = id;
	if (xioctl(transaction->fd, SNDRV_CTL_IOCTL_ELEM_READ,
	           &saved->original) < 0)
		return -1;

	desired = saved->original;
	for (i = 0; i < value_count; ++i) {
		if (desired.value.integer.value[i] != values[i])
			saved->changed = 1;
		desired.value.integer.value[i] = values[i];
	}
	++transaction->saved_count;
	printf("audio mixer    : %s ", name);
	print_values(saved->original.value.integer.value, value_count);
	printf(" -> ");
	print_values(values, value_count);
	printf("%s\n", saved->changed ? "" : " (unchanged)");
	if (!saved->changed)
		return 0;

	if (xioctl(transaction->fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &desired) < 0)
		return -1;
	memset(&verify, 0, sizeof(verify));
	verify.id = id;
	if (xioctl(transaction->fd, SNDRV_CTL_IOCTL_ELEM_READ, &verify) < 0)
		return -1;
	for (i = 0; i < value_count; ++i) {
		if (verify.value.integer.value[i] != values[i]) {
			fprintf(stderr, "Mixer readback mismatch for '%s'\n", name);
			errno = EIO;
			return -1;
		}
	}
	return 0;
}

static int configure_main_mic(struct mixer_transaction *transaction,
			      const char *control_device)
{
	static const long zero[] = { 0 };
	static const long one[] = { 1 };
	static const long stereo_on[] = { 1, 1 };

	transaction->fd = open(control_device, O_RDWR | O_CLOEXEC);
	if (transaction->fd < 0) {
		fprintf(stderr, "Cannot open %s: %s\n", control_device,
			strerror(errno));
		return -1;
	}
	/* ALPHA V2.4: MIC- is LINPUT1 and MIC+ is LINPUT2. */
	if (set_control(transaction, LINPUT2_VOLUME_NAME, zero, 1) < 0 ||
	    set_control(transaction, LINPUT3_VOLUME_NAME, zero, 1) < 0 ||
	    set_control(transaction, LINPUT1_SWITCH_NAME, one, 1) < 0 ||
	    set_control(transaction, LINPUT2_SWITCH_NAME, one, 1) < 0 ||
	    set_control(transaction, LINPUT3_SWITCH_NAME, zero, 1) < 0 ||
	    set_control(transaction, LEFT_BOOST_NAME, one, 1) < 0 ||
	    /* Unmute last, after the complete analog path is stable. */
	    set_control(transaction, CAPTURE_SWITCH_NAME, stereo_on, 2) < 0)
		return -1;
	return 0;
}

static void restore_mixer(struct mixer_transaction *transaction)
{
	while (transaction->fd >= 0 && transaction->saved_count > 0) {
		struct saved_control *saved =
			&transaction->saved[--transaction->saved_count];

		if (!saved->changed)
			continue;
		if (xioctl(transaction->fd, SNDRV_CTL_IOCTL_ELEM_WRITE,
		           &saved->original) < 0)
			fprintf(stderr, "Warning: cannot restore '%s': %s\n",
				saved->name, strerror(errno));
	}
	if (transaction->fd >= 0)
		close(transaction->fd);
	transaction->fd = -1;
}

static int allocate_ring(struct av_audio_capture *capture,
			 unsigned int ring_slots)
{
	size_t samples_per_slot;
	size_t total_samples;
	unsigned int i;

	if (ring_slots < AV_MIN_RING_SLOTS || ring_slots > AV_MAX_RING_SLOTS) {
		fprintf(stderr, "Audio ring slots must be %u..%u\n",
			AV_MIN_RING_SLOTS, AV_MAX_RING_SLOTS);
		errno = EINVAL;
		return -1;
	}
	samples_per_slot = (size_t)capture->period_frames * AV_AUDIO_CHANNELS;
	if (ring_slots > SIZE_MAX / samples_per_slot ||
	    ring_slots * samples_per_slot > SIZE_MAX / sizeof(int16_t)) {
		errno = EOVERFLOW;
		return -1;
	}
	total_samples = ring_slots * samples_per_slot;
	capture->slots = calloc(ring_slots, sizeof(*capture->slots));
	capture->ring_storage = calloc(total_samples, sizeof(int16_t));
	capture->period_buffer = calloc(samples_per_slot, sizeof(int16_t));
	if (!capture->slots || !capture->ring_storage || !capture->period_buffer) {
		fprintf(stderr, "Cannot allocate audio ring: %s\n", strerror(errno));
		return -1;
	}
	for (i = 0; i < ring_slots; ++i)
		capture->slots[i].samples =
			capture->ring_storage + i * samples_per_slot;
	capture->ring_capacity = ring_slots;
	capture->stats.ring_slots = ring_slots;
	printf("user ring       : %u slots, %.3f ms, %zu bytes\n",
	       ring_slots,
	       (double)ring_slots * capture->period_frames * 1000.0 /
	       AV_AUDIO_SAMPLE_RATE,
	       total_samples * sizeof(int16_t));
	return 0;
}

static int should_stop(struct av_audio_capture *capture)
{
	int stop;

	pthread_mutex_lock(&capture->lock);
	stop = capture->stop_requested;
	pthread_mutex_unlock(&capture->lock);
	return stop;
}

/* Drop the oldest packet rather than blocking the real-time producer. */
static void publish_period(struct av_audio_capture *capture,
			   const int16_t *samples, unsigned int frames)
{
	struct audio_slot *slot;

	pthread_mutex_lock(&capture->lock);
	if (capture->queued == capture->ring_capacity) {
		struct audio_slot *oldest = &capture->slots[capture->read_index];

		capture->stats.dropped_packets++;
		capture->stats.dropped_frames += oldest->info.frames;
		capture->read_index =
			(capture->read_index + 1U) % capture->ring_capacity;
		capture->queued--;
	}
	slot = &capture->slots[capture->write_index];
	memcpy(slot->samples, samples,
	       (size_t)frames * AV_AUDIO_BYTES_PER_FRAME);
	slot->info.sequence = capture->next_sequence++;
	slot->info.first_frame = capture->next_first_frame;
	slot->info.timestamp_us = monotonic_us();
	slot->info.frames = frames;
	capture->next_first_frame += frames;
	capture->write_index =
		(capture->write_index + 1U) % capture->ring_capacity;
	capture->queued++;
	capture->stats.captured_packets++;
	capture->stats.captured_frames += frames;
	if (capture->queued > capture->stats.ring_high_watermark)
		capture->stats.ring_high_watermark = capture->queued;
	pthread_cond_signal(&capture->data_ready);
	pthread_mutex_unlock(&capture->lock);
}

static int recover_pcm(struct av_audio_capture *capture, int error_number)
{
	int ret;

	if (error_number == EPIPE) {
		pthread_mutex_lock(&capture->lock);
		capture->stats.xruns++;
		pthread_mutex_unlock(&capture->lock);
		fprintf(stderr, "Audio XRUN: PREPARE and START capture again\n");
		if (xioctl_noarg(capture->pcm_fd,
				   SNDRV_PCM_IOCTL_PREPARE) < 0)
			return -1;
		/*
		 * This PCM descriptor is nonblocking.  After PREPARE, waiting in
		 * poll() cannot restart capture by itself: the DMA engine has not
		 * started and therefore cannot make the descriptor readable.  Start
		 * it explicitly before returning to the poll/read loop.
		 */
		return xioctl_noarg(capture->pcm_fd, SNDRV_PCM_IOCTL_START);
	}
	if (error_number != ESTRPIPE) {
		errno = error_number;
		return -1;
	}
	do {
		ret = ioctl(capture->pcm_fd, SNDRV_PCM_IOCTL_RESUME);
		if (ret < 0 && errno == EAGAIN)
			usleep(10000);
	} while (ret < 0 && errno == EAGAIN && !should_stop(capture));
	if (ret < 0) {
		ret = xioctl_noarg(capture->pcm_fd, SNDRV_PCM_IOCTL_PREPARE);
		if (ret == 0)
			ret = xioctl_noarg(capture->pcm_fd,
					     SNDRV_PCM_IOCTL_START);
	}
	return ret;
}

static int read_one_period(struct av_audio_capture *capture)
{
	for (;;) {
		struct snd_xferi transfer;
		struct pollfd poll_fd;
		int ready;
		int error_number;

		if (should_stop(capture))
			return 0;
		poll_fd.fd = capture->pcm_fd;
		poll_fd.events = POLLIN | POLLERR;
		poll_fd.revents = 0;
		ready = poll(&poll_fd, 1, AV_PCM_POLL_MS);
		if (ready < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (ready == 0)
			continue;

		memset(&transfer, 0, sizeof(transfer));
		transfer.buf = capture->period_buffer;
		transfer.frames = capture->period_frames;
		if (ioctl(capture->pcm_fd, SNDRV_PCM_IOCTL_READI_FRAMES,
		          &transfer) == 0) {
			if (transfer.result > 0 &&
			    (unsigned long)transfer.result <= capture->period_frames) {
				publish_period(capture, capture->period_buffer,
					       (unsigned int)transfer.result);
				return 1;
			}
			if (transfer.result == 0)
				continue;
			if (transfer.result > 0) {
				errno = EIO;
				return -1;
			}
			error_number = (int)-transfer.result;
		} else {
			error_number = errno;
		}
		if (error_number == EINTR || error_number == EAGAIN)
			continue;
		if (recover_pcm(capture, error_number) < 0)
			return -1;
	}
}

static void finish_producer(struct av_audio_capture *capture, int error_number)
{
	pthread_mutex_lock(&capture->lock);
	capture->capture_error = error_number;
	capture->stats.capture_error = error_number;
	capture->producer_finished = 1;
	pthread_cond_broadcast(&capture->data_ready);
	pthread_mutex_unlock(&capture->lock);
}

static void *capture_thread_main(void *argument)
{
	struct av_audio_capture *capture = argument;
	int error_number = 0;

	/*
	 * configure_hardware() leaves ALSA in PREPARED state.  AUDIO-R2 used a
	 * blocking READI_FRAMES call, which implicitly started capture.  R3 uses
	 * O_NONBLOCK and waits with poll(), so relying on that implicit start
	 * creates a deadlock: poll waits for PCM data while SAI/SDMA waits for a
	 * START request.  Start inside the producer thread before its first poll.
	 */
	if (xioctl_noarg(capture->pcm_fd, SNDRV_PCM_IOCTL_START) < 0) {
		error_number = errno ? errno : EIO;
		fprintf(stderr, "Audio producer START failed: %s\n",
			strerror(error_number));
		finish_producer(capture, error_number);
		return NULL;
	}
	printf("audio stream    : STARTED (SAI/SDMA running)\n");

	while (!should_stop(capture)) {
		if (read_one_period(capture) < 0) {
			error_number = errno ? errno : EIO;
			fprintf(stderr, "Audio producer failed: %s\n",
				strerror(error_number));
			break;
		}
	}
	finish_producer(capture, error_number);
	return NULL;
}

static int make_timeout(struct timespec *deadline, unsigned int timeout_ms)
{
	if (clock_gettime(CLOCK_REALTIME, deadline) < 0)
		return -1;
	deadline->tv_sec += timeout_ms / 1000U;
	deadline->tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
	if (deadline->tv_nsec >= 1000000000L) {
		deadline->tv_sec++;
		deadline->tv_nsec -= 1000000000L;
	}
	return 0;
}

int av_audio_open(struct av_audio_capture **capture_out,
		  const struct av_audio_config *config)
{
	struct av_audio_capture *capture;
	struct stat st;
	unsigned int ring_slots;
	int error;

	if (!capture_out || !config || !config->pcm_device ||
	    !config->control_device) {
		errno = EINVAL;
		return -1;
	}
	*capture_out = NULL;
	ring_slots = config->ring_slots ? config->ring_slots :
		AV_AUDIO_DEFAULT_RING_SLOTS;
	capture = calloc(1, sizeof(*capture));
	if (!capture)
		return -1;
	capture->pcm_fd = -1;
	capture->mixer.fd = -1;

	error = pthread_mutex_init(&capture->lock, NULL);
	if (error) {
		errno = error;
		goto fail;
	}
	capture->lock_initialized = 1;
	error = pthread_cond_init(&capture->data_ready, NULL);
	if (error) {
		errno = error;
		goto fail;
	}
	capture->cond_initialized = 1;

	if (stat(config->pcm_device, &st) < 0 || !S_ISCHR(st.st_mode)) {
		fprintf(stderr, "%s is not a PCM character device\n",
			config->pcm_device);
		errno = ENODEV;
		goto fail;
	}
	if (configure_main_mic(&capture->mixer, config->control_device) < 0)
		goto fail;
	capture->pcm_fd = open(config->pcm_device,
		O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (capture->pcm_fd < 0) {
		fprintf(stderr, "Cannot open %s: %s\n", config->pcm_device,
			strerror(errno));
		goto fail;
	}
	if (configure_hardware(capture) < 0 ||
	    configure_software(capture) < 0 ||
	    xioctl_noarg(capture->pcm_fd, SNDRV_PCM_IOCTL_PREPARE) < 0)
		goto fail;
	capture->prepared = 1;
	if (allocate_ring(capture, ring_slots) < 0)
		goto fail;

	*capture_out = capture;
	return 0;
fail:
	av_audio_close(capture);
	return -1;
}

int av_audio_start(struct av_audio_capture *capture)
{
	int error;

	if (!capture || capture->thread_started || !capture->prepared) {
		errno = EINVAL;
		return -1;
	}
	error = pthread_create(&capture->thread, NULL,
			       capture_thread_main, capture);
	if (error) {
		errno = error;
		fprintf(stderr, "pthread_create failed: %s\n", strerror(error));
		return -1;
	}
	capture->thread_started = 1;
	return 0;
}

int av_audio_read(struct av_audio_capture *capture,
		  int16_t *destination,
		  unsigned int destination_capacity_frames,
		  struct av_audio_packet_info *packet,
		  unsigned int timeout_ms)
{
	struct timespec deadline;
	int wait_error = 0;

	if (!capture || !destination || !packet ||
	    destination_capacity_frames < capture->period_frames) {
		errno = EINVAL;
		return -1;
	}
	if (timeout_ms && make_timeout(&deadline, timeout_ms) < 0)
		return -1;

	pthread_mutex_lock(&capture->lock);
	while (capture->queued == 0 && !capture->producer_finished) {
		if (timeout_ms == 0)
			wait_error = pthread_cond_wait(&capture->data_ready,
					       &capture->lock);
		else
			wait_error = pthread_cond_timedwait(&capture->data_ready,
						    &capture->lock, &deadline);
		if (wait_error == ETIMEDOUT) {
			pthread_mutex_unlock(&capture->lock);
			return AV_AUDIO_TIMEOUT;
		}
		if (wait_error) {
			pthread_mutex_unlock(&capture->lock);
			errno = wait_error;
			return -1;
		}
	}
	if (capture->queued == 0) {
		int capture_error = capture->capture_error;

		pthread_mutex_unlock(&capture->lock);
		if (capture_error) {
			errno = capture_error;
			return -1;
		}
		return AV_AUDIO_STOPPED;
	}

	{
		struct audio_slot *slot = &capture->slots[capture->read_index];

		memcpy(destination, slot->samples,
		       (size_t)slot->info.frames * AV_AUDIO_BYTES_PER_FRAME);
		*packet = slot->info;
		capture->read_index =
			(capture->read_index + 1U) % capture->ring_capacity;
		capture->queued--;
		capture->stats.consumed_packets++;
		capture->stats.consumed_frames += packet->frames;
	}
	pthread_mutex_unlock(&capture->lock);
	return 0;
}

int av_audio_stop(struct av_audio_capture *capture)
{
	int failed = 0;

	if (!capture)
		return 0;
	if (capture->lock_initialized) {
		pthread_mutex_lock(&capture->lock);
		capture->stop_requested = 1;
		if (capture->cond_initialized)
			pthread_cond_broadcast(&capture->data_ready);
		pthread_mutex_unlock(&capture->lock);
	}
	if (capture->thread_started) {
		(void)pthread_join(capture->thread, NULL);
		capture->thread_started = 0;
	}
	if (capture->prepared && capture->pcm_fd >= 0) {
		if (xioctl_noarg(capture->pcm_fd, SNDRV_PCM_IOCTL_DROP) < 0 &&
		    errno != EBADFD) {
			fprintf(stderr, "SNDRV_PCM_IOCTL_DROP failed: %s\n",
				strerror(errno));
			failed = 1;
		}
		capture->prepared = 0;
	}
	return failed ? -1 : 0;
}

void av_audio_close(struct av_audio_capture *capture)
{
	if (!capture)
		return;
	(void)av_audio_stop(capture);
	if (capture->hw_configured && capture->pcm_fd >= 0)
		(void)xioctl_noarg(capture->pcm_fd, SNDRV_PCM_IOCTL_HW_FREE);
	if (capture->pcm_fd >= 0)
		close(capture->pcm_fd);
	restore_mixer(&capture->mixer);
	free(capture->period_buffer);
	free(capture->ring_storage);
	free(capture->slots);
	if (capture->cond_initialized)
		(void)pthread_cond_destroy(&capture->data_ready);
	if (capture->lock_initialized)
		(void)pthread_mutex_destroy(&capture->lock);
	free(capture);
}

void av_audio_get_stats(struct av_audio_capture *capture,
			struct av_audio_stats *stats)
{
	if (!capture || !stats)
		return;
	pthread_mutex_lock(&capture->lock);
	*stats = capture->stats;
	stats->ring_queued = capture->queued;
	pthread_mutex_unlock(&capture->lock);
}

unsigned int av_audio_period_frames(const struct av_audio_capture *capture)
{
	return capture ? capture->period_frames : 0;
}
