/*
 * audio_capture.c - AUDIO-R2.1: WM8960 PCM capture and main-MIC A/B test
 *
 * Data path:
 *
 *   WM8960 ADC -> SAI2 -> SDMA -> ALSA kernel ring buffer
 *              -> SNDRV_PCM_IOCTL_READI_FRAMES -> application RAM
 *              -> stop DMA -> WAV file
 *
 * This implementation deliberately uses the Linux ALSA UAPI instead of
 * libasound because the current BusyBox root filesystem has no libasound.so.
 * It demonstrates the same essential PCM state machine explicitly:
 *
 *   open -> HW_PARAMS -> SW_PARAMS -> PREPARE -> READI_FRAMES
 *        -> DROP -> HW_FREE -> close
 *
 * The program records to RAM first.  Only after DROP/HW_FREE/close does it
 * write the WAV file, so slow NFS or flash writes cannot delay PCM reads and
 * create an artificial capture overrun.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#include <sound/asound.h>

#define AV_SAMPLE_RATE       48000U
#define AV_CHANNELS          2U
#define AV_SAMPLE_BITS       16U
#define AV_BYTES_PER_SAMPLE  (AV_SAMPLE_BITS / 8U)
#define AV_BYTES_PER_FRAME   (AV_CHANNELS * AV_BYTES_PER_SAMPLE)

#define AV_PERIOD_FRAMES     1024U
#define AV_PERIOD_COUNT      4U
#define AV_DEFAULT_SECONDS   5U
#define AV_MAX_SECONDS       60U

#define AV_CAPTURE_SWITCH_NAME "Capture Switch"
#define AV_LEFT_LINPUT1_SWITCH "Left Boost Mixer LINPUT1 Switch"
#define AV_LEFT_LINPUT2_SWITCH "Left Boost Mixer LINPUT2 Switch"
#define AV_LEFT_LINPUT3_SWITCH "Left Boost Mixer LINPUT3 Switch"
#define AV_LEFT_BOOST_SWITCH   "Left Input Mixer Boost Switch"
#define AV_LEFT_LINPUT1_GAIN   "Left Input Boost Mixer LINPUT1 Volume"
#define AV_LEFT_LINPUT2_GAIN   "Left Input Boost Mixer LINPUT2 Volume"
#define AV_LEFT_LINPUT3_GAIN   "Left Input Boost Mixer LINPUT3 Volume"

#define AV_MAX_CONTROLS        4096U
#define AV_MAX_SAVED_CONTROLS  10U

static volatile sig_atomic_t stop_requested;

enum mixer_profile {
	MIXER_PROFILE_BASELINE = 0,
	MIXER_PROFILE_MAINMIC_ROUTE,
	MIXER_PROFILE_MAINMIC_GAIN,
};

struct saved_mixer_control {
	char name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN + 1];
	int changed;
	unsigned int value_count;
	struct snd_ctl_elem_value original;
};

struct mixer_transaction {
	int fd;
	unsigned int saved_count;
	struct saved_mixer_control saved[AV_MAX_SAVED_CONTROLS];
};

struct capture_stats {
	int32_t peak[AV_CHANNELS];
	int64_t sum[AV_CHANNELS];
	uint64_t square_sum[AV_CHANNELS];
	uint64_t clipped[AV_CHANNELS];
	uint64_t frames;
};

static void signal_handler(int signal_number)
{
	(void)signal_number;
	stop_requested = 1;
}

static int install_signal_handlers(void)
{
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_handler = signal_handler;
	sigemptyset(&action.sa_mask);
	/* No SA_RESTART: a blocking PCM ioctl must return EINTR on Ctrl+C. */
	if (sigaction(SIGINT, &action, NULL) < 0 ||
	    sigaction(SIGTERM, &action, NULL) < 0) {
		fprintf(stderr, "Cannot install signal handlers: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

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

/* Build the unconstrained parameter set expected by the ALSA HW_PARAMS ABI. */
static void hw_params_any(struct snd_pcm_hw_params *params)
{
	unsigned int parameter;

	memset(params, 0, sizeof(*params));
	for (parameter = SNDRV_PCM_HW_PARAM_FIRST_MASK;
	     parameter <= SNDRV_PCM_HW_PARAM_LAST_MASK;
	     ++parameter)
		memset(param_mask(params, parameter)->bits, 0xff,
		       sizeof(param_mask(params, parameter)->bits));

	for (parameter = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL;
	     parameter <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL;
	     ++parameter) {
		struct snd_interval *interval = param_interval(params, parameter);

		interval->min = 0;
		interval->max = UINT_MAX;
		interval->openmin = 0;
		interval->openmax = 0;
		interval->integer = 0;
		interval->empty = 0;
	}

	params->rmask = ~0U;
	params->cmask = 0;
	params->info = ~0U;
}

static void set_mask_value(struct snd_pcm_hw_params *params,
			   unsigned int parameter,
			   unsigned int value)
{
	struct snd_mask *mask = param_mask(params, parameter);

	memset(mask->bits, 0, sizeof(mask->bits));
	mask->bits[value / 32U] = 1U << (value % 32U);
}

static void set_interval_value(struct snd_pcm_hw_params *params,
			       unsigned int parameter,
			       unsigned int value)
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
	const struct snd_interval *interval =
		param_interval_const(params, parameter);

	/* Exact parameters have equal minimum and maximum after HW_PARAMS. */
	return interval->min;
}

static int configure_hardware(int pcm_fd,
			      unsigned int *period_frames,
			      unsigned int *period_count,
			      unsigned int *buffer_frames,
			      int *parameters_committed)
{
	struct snd_pcm_hw_params params;
	unsigned int actual_rate;
	unsigned int actual_channels;
	unsigned int actual_format;

	*parameters_committed = 0;
	hw_params_any(&params);
	set_mask_value(&params, SNDRV_PCM_HW_PARAM_ACCESS,
		       (unsigned int)SNDRV_PCM_ACCESS_RW_INTERLEAVED);
	set_mask_value(&params, SNDRV_PCM_HW_PARAM_FORMAT,
		       (unsigned int)SNDRV_PCM_FORMAT_S16_LE);
	set_mask_value(&params, SNDRV_PCM_HW_PARAM_SUBFORMAT,
		       (unsigned int)SNDRV_PCM_SUBFORMAT_STD);
	set_interval_value(&params, SNDRV_PCM_HW_PARAM_CHANNELS, AV_CHANNELS);
	set_interval_value(&params, SNDRV_PCM_HW_PARAM_RATE, AV_SAMPLE_RATE);
	set_interval_value(&params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
			   AV_PERIOD_FRAMES);
	set_interval_value(&params, SNDRV_PCM_HW_PARAM_PERIODS, AV_PERIOD_COUNT);

	if (xioctl(pcm_fd, SNDRV_PCM_IOCTL_HW_PARAMS, &params) < 0) {
		fprintf(stderr, "SNDRV_PCM_IOCTL_HW_PARAMS failed: %s\n",
			strerror(errno));
		return -1;
	}
	/* From this point the PCM owns a committed hardware configuration. */
	*parameters_committed = 1;

	actual_rate = get_interval_value(&params, SNDRV_PCM_HW_PARAM_RATE);
	actual_channels = get_interval_value(&params, SNDRV_PCM_HW_PARAM_CHANNELS);
	actual_format = (unsigned int)SNDRV_PCM_FORMAT_S16_LE;
	*period_frames = get_interval_value(&params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
	*period_count = get_interval_value(&params, SNDRV_PCM_HW_PARAM_PERIODS);
	*buffer_frames = get_interval_value(&params, SNDRV_PCM_HW_PARAM_BUFFER_SIZE);

	if (actual_rate != AV_SAMPLE_RATE || actual_channels != AV_CHANNELS ||
	    *period_frames == 0 || *period_count < 2 || *buffer_frames == 0) {
		fprintf(stderr, "Driver returned an invalid or unexpected PCM setup\n");
		return -1;
	}

	printf("hardware params : rate=%u Hz channels=%u format=%s(%u)\n",
	       actual_rate, actual_channels, "S16_LE", actual_format);
	printf("  period        : %u frames, %.3f ms, %u bytes\n",
	       *period_frames,
	       (double)*period_frames * 1000.0 / (double)actual_rate,
	       *period_frames * AV_BYTES_PER_FRAME);
	printf("  periods       : %u\n", *period_count);
	printf("  buffer        : %u frames, %.3f ms, %u bytes\n",
	       *buffer_frames,
	       (double)*buffer_frames * 1000.0 / (double)actual_rate,
	       *buffer_frames * AV_BYTES_PER_FRAME);
	printf("  msbits/fifo   : %u / %lu frames\n",
	       params.msbits, (unsigned long)params.fifo_size);
	return 0;
}

static int configure_software(int pcm_fd,
			      unsigned int period_frames,
			      unsigned int buffer_frames)
{
	struct snd_pcm_sw_params params;

	memset(&params, 0, sizeof(params));
	params.tstamp_mode = SNDRV_PCM_TSTAMP_ENABLE;
	params.period_step = 1;
	params.sleep_min = 0;
	params.avail_min = period_frames;
	params.xfer_align = 1;

	/* For capture, the first READI_FRAMES request starts a prepared stream. */
	params.start_threshold = 1;

	/* Reaching a full unread ring buffer is a real capture overrun (XRUN). */
	params.stop_threshold = buffer_frames;
	params.silence_threshold = 0;
	params.silence_size = 0;
	params.proto = SNDRV_PCM_VERSION;
	params.tstamp_type = SNDRV_PCM_TSTAMP_TYPE_GETTIMEOFDAY;

	if (xioctl(pcm_fd, SNDRV_PCM_IOCTL_SW_PARAMS, &params) < 0) {
		fprintf(stderr, "SNDRV_PCM_IOCTL_SW_PARAMS failed: %s\n",
			strerror(errno));
		return -1;
	}

	printf("software params : avail_min=%lu start=%lu stop=%lu boundary=%lu\n",
	       (unsigned long)params.avail_min,
	       (unsigned long)params.start_threshold,
	       (unsigned long)params.stop_threshold,
	       (unsigned long)params.boundary);
	return 0;
}

static int find_control_id(int control_fd,
			   const char *wanted_name,
			   struct snd_ctl_elem_id *found)
{
	struct snd_ctl_elem_list list;
	struct snd_ctl_elem_id *ids = NULL;
	unsigned int capacity;
	unsigned int i;
	int ret = -1;

	memset(&list, 0, sizeof(list));
	if (xioctl(control_fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0)
		return -1;
	if (list.count == 0 || list.count > AV_MAX_CONTROLS) {
		errno = ENOENT;
		return -1;
	}

	capacity = list.count;
	ids = calloc(capacity, sizeof(*ids));
	if (!ids)
		return -1;

	memset(&list, 0, sizeof(list));
	list.space = capacity;
	list.pids = ids;
	if (xioctl(control_fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0)
		goto out;

	for (i = 0; i < list.used; ++i) {
		char name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN + 1];

		memcpy(name, ids[i].name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
		name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN] = '\0';
		if (ids[i].iface == SNDRV_CTL_ELEM_IFACE_MIXER &&
		    strcmp(name, wanted_name) == 0) {
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

static const char *mixer_profile_name(enum mixer_profile profile)
{
	switch (profile) {
	case MIXER_PROFILE_BASELINE:
		return "baseline";
	case MIXER_PROFILE_MAINMIC_ROUTE:
		return "mainmic-route";
	case MIXER_PROFILE_MAINMIC_GAIN:
		return "mainmic-gain";
	default:
		return "unknown";
	}
}

static int parse_mixer_profile(const char *text, enum mixer_profile *profile)
{
	if (!strcmp(text, "baseline"))
		*profile = MIXER_PROFILE_BASELINE;
	else if (!strcmp(text, "mainmic-route"))
		*profile = MIXER_PROFILE_MAINMIC_ROUTE;
	else if (!strcmp(text, "mainmic-gain"))
		*profile = MIXER_PROFILE_MAINMIC_GAIN;
	else {
		fprintf(stderr,
			"Mixer profile must be baseline, mainmic-route or mainmic-gain\n");
		return -1;
	}
	return 0;
}

static void print_mixer_values(const long *values, unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; ++i)
		printf("%s%ld", i ? "," : "", values[i]);
}

/*
 * Save one control before changing it.  Every saved item is restored in
 * reverse order, including partial setup failures and Ctrl+C cleanup.
 */
static int set_mixer_control(struct mixer_transaction *transaction,
			     const char *name,
			     const long *desired_values,
			     unsigned int desired_count)
{
	struct saved_mixer_control *saved;
	struct snd_ctl_elem_info info;
	struct snd_ctl_elem_value desired;
	struct snd_ctl_elem_value verify;
	struct snd_ctl_elem_id id;
	unsigned int i;

	if (transaction->saved_count >= AV_MAX_SAVED_CONTROLS) {
		errno = ENOSPC;
		return -1;
	}

	memset(&id, 0, sizeof(id));
	if (find_control_id(transaction->fd, name, &id) < 0) {
		fprintf(stderr, "Cannot find mixer control '%s': %s\n",
			name, strerror(errno));
		return -1;
	}

	memset(&info, 0, sizeof(info));
	info.id = id;
	if (xioctl(transaction->fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0) {
		fprintf(stderr, "ELEM_INFO '%s' failed: %s\n",
			name, strerror(errno));
		return -1;
	}
	if ((info.type != SNDRV_CTL_ELEM_TYPE_BOOLEAN &&
	     info.type != SNDRV_CTL_ELEM_TYPE_INTEGER) ||
	    info.count != desired_count || info.count == 0 || info.count > 128 ||
	    !(info.access & SNDRV_CTL_ELEM_ACCESS_WRITE)) {
		fprintf(stderr, "Mixer control '%s' has unexpected type/access/count\n",
			name);
		errno = EINVAL;
		return -1;
	}
	for (i = 0; i < desired_count; ++i) {
		if (desired_values[i] < info.value.integer.min ||
		    desired_values[i] > info.value.integer.max) {
			fprintf(stderr, "Mixer value %ld is outside '%s' range %ld..%ld\n",
				desired_values[i], name,
				info.value.integer.min, info.value.integer.max);
			errno = ERANGE;
			return -1;
		}
	}

	saved = &transaction->saved[transaction->saved_count];
	memset(saved, 0, sizeof(*saved));
	strncpy(saved->name, name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
	saved->name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN] = '\0';
	saved->value_count = info.count;
	saved->original.id = id;
	if (xioctl(transaction->fd, SNDRV_CTL_IOCTL_ELEM_READ,
	           &saved->original) < 0) {
		fprintf(stderr, "ELEM_READ '%s' failed: %s\n",
			name, strerror(errno));
		return -1;
	}

	desired = saved->original;
	for (i = 0; i < desired_count; ++i) {
		if (desired.value.integer.value[i] != desired_values[i])
			saved->changed = 1;
		desired.value.integer.value[i] = desired_values[i];
	}
	/* The original value is now owned by the rollback transaction. */
	++transaction->saved_count;

	printf("mixer setup     : %s ", name);
	print_mixer_values(saved->original.value.integer.value, desired_count);
	printf(" -> ");
	print_mixer_values(desired_values, desired_count);
	if (!saved->changed) {
		printf(" (unchanged)\n");
		return 0;
	}
	printf("\n");

	if (xioctl(transaction->fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &desired) < 0) {
		fprintf(stderr, "ELEM_WRITE '%s' failed: %s\n",
			name, strerror(errno));
		return -1;
	}

	memset(&verify, 0, sizeof(verify));
	verify.id = id;
	if (xioctl(transaction->fd, SNDRV_CTL_IOCTL_ELEM_READ, &verify) < 0) {
		fprintf(stderr, "ELEM_READBACK '%s' failed: %s\n",
			name, strerror(errno));
		return -1;
	}
	for (i = 0; i < desired_count; ++i) {
		if (verify.value.integer.value[i] != desired_values[i]) {
			fprintf(stderr, "Mixer control '%s' readback mismatch\n", name);
			errno = EIO;
			return -1;
		}
	}
	return 0;
}

static int configure_mixer(const char *control_path,
			   enum mixer_profile profile,
			   struct mixer_transaction *transaction)
{
	static const long zero[] = { 0 };
	static const long one[] = { 1 };
	static const long stereo_on[] = { 1, 1 };

	memset(transaction, 0, sizeof(*transaction));
	transaction->fd = -1;
	transaction->fd = open(control_path, O_RDWR | O_CLOEXEC);
	if (transaction->fd < 0) {
		fprintf(stderr, "Cannot open mixer %s: %s\n",
			control_path, strerror(errno));
		return -1;
	}

	printf("mixer profile   : %s\n", mixer_profile_name(profile));
	if (profile != MIXER_PROFILE_BASELINE) {
		/*
		 * ALPHA V2.4 main MIC is pseudo-differential:
		 *   LINPUT2 = non-inverting MIC+, LINPUT1 = inverting MIC-.
		 * Keep the independent LINPUT2/3 line-boost paths muted, select both
		 * PGA terminals, and connect the left PGA output to the ADC boost.
		 */
		if (set_mixer_control(transaction, AV_LEFT_LINPUT2_GAIN,
				      zero, 1) < 0 ||
		    set_mixer_control(transaction, AV_LEFT_LINPUT3_GAIN,
				      zero, 1) < 0 ||
		    set_mixer_control(transaction, AV_LEFT_LINPUT1_SWITCH,
				      one, 1) < 0 ||
		    set_mixer_control(transaction, AV_LEFT_LINPUT2_SWITCH,
				      one, 1) < 0 ||
		    set_mixer_control(transaction, AV_LEFT_LINPUT3_SWITCH,
				      zero, 1) < 0 ||
		    set_mixer_control(transaction, AV_LEFT_BOOST_SWITCH,
				      one, 1) < 0)
			return -1;

		/* +13dB is the first non-zero WM8960 MIC-PGA boost step. */
		if (profile == MIXER_PROFILE_MAINMIC_GAIN &&
		    set_mixer_control(transaction, AV_LEFT_LINPUT1_GAIN,
				      one, 1) < 0)
			return -1;
	}

	/* Unmute last, after every route and gain control is stable. */
	return set_mixer_control(transaction, AV_CAPTURE_SWITCH_NAME,
				 stereo_on, 2);
}

static void restore_mixer(struct mixer_transaction *transaction)
{
	if (transaction->fd < 0)
		return;

	/* Reverse order mutes Capture Switch before restoring routes and gains. */
	while (transaction->saved_count > 0) {
		struct saved_mixer_control *saved =
			&transaction->saved[--transaction->saved_count];
		unsigned int i;

		if (!saved->changed)
			continue;
		if (xioctl(transaction->fd, SNDRV_CTL_IOCTL_ELEM_WRITE,
		           &saved->original) < 0) {
			fprintf(stderr, "Warning: failed to restore '%s': %s\n",
				saved->name, strerror(errno));
			continue;
		}
		printf("mixer restore   : %s=", saved->name);
		for (i = 0; i < saved->value_count; ++i)
			printf("%s%ld", i ? "," : "",
			       saved->original.value.integer.value[i]);
		printf("\n");
	}
	close(transaction->fd);
	transaction->fd = -1;
}

/* Return positive captured frames, 0 for no progress, -2 for clean signal. */
static int read_pcm_frames(int pcm_fd,
			   int16_t *destination,
			   unsigned int requested_frames,
			   unsigned int *xruns)
{
	for (;;) {
		struct snd_xferi transfer;
		int ret;
		int saved_errno;

		memset(&transfer, 0, sizeof(transfer));
		transfer.buf = destination;
		transfer.frames = requested_frames;
		ret = ioctl(pcm_fd, SNDRV_PCM_IOCTL_READI_FRAMES, &transfer);
		if (ret == 0) {
			if (transfer.result < 0) {
				errno = (int)-transfer.result;
				ret = -1;
			} else if ((unsigned long)transfer.result > requested_frames) {
				fprintf(stderr, "Driver returned too many PCM frames\n");
				errno = EIO;
				return -1;
			} else {
				return (int)transfer.result;
			}
		}

		saved_errno = errno;
		if (saved_errno == EINTR) {
			if (stop_requested)
				return -2;
			continue;
		}
		if (saved_errno == EAGAIN)
			continue;

		if (saved_errno == EPIPE) {
			++(*xruns);
			fprintf(stderr, "XRUN #%u: capture overrun, preparing PCM again\n",
				*xruns);
			if (xioctl_noarg(pcm_fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
				fprintf(stderr, "XRUN recovery PREPARE failed: %s\n",
					strerror(errno));
				return -1;
			}
			continue;
		}

		if (saved_errno == ESTRPIPE) {
			/* Resume after system suspend; fall back to PREPARE if unsupported. */
			do {
				ret = ioctl(pcm_fd, SNDRV_PCM_IOCTL_RESUME);
				if (ret < 0 && errno == EAGAIN)
					usleep(10000);
			} while (ret < 0 && errno == EAGAIN && !stop_requested);
			if (ret < 0 &&
			    xioctl_noarg(pcm_fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
				fprintf(stderr, "Suspend recovery failed: %s\n",
					strerror(errno));
				return -1;
			}
			continue;
		}

		errno = saved_errno;
		fprintf(stderr, "SNDRV_PCM_IOCTL_READI_FRAMES failed: %s\n",
			strerror(errno));
		return -1;
	}
}

static void update_capture_stats(struct capture_stats *stats,
				 const int16_t *samples,
				 unsigned int frames)
{
	unsigned int frame;
	unsigned int channel;

	for (frame = 0; frame < frames; ++frame) {
		for (channel = 0; channel < AV_CHANNELS; ++channel) {
			int32_t sample = samples[frame * AV_CHANNELS + channel];
			int32_t magnitude = sample < 0 ? -sample : sample;

			if (magnitude > stats->peak[channel])
				stats->peak[channel] = magnitude;
			stats->sum[channel] += sample;
			stats->square_sum[channel] += (uint64_t)((int64_t)sample * sample);
			if (sample == INT16_MIN || sample == INT16_MAX)
				++stats->clipped[channel];
		}
	}
	stats->frames += frames;
}

static double positive_square_root(double value)
{
	double estimate;
	unsigned int i;

	if (value <= 0.0)
		return 0.0;

	/* PCM RMS never exceeds 32768, which is a useful Newton start point. */
	estimate = 32768.0;
	for (i = 0; i < 24; ++i)
		estimate = 0.5 * (estimate + value / estimate);
	return estimate;
}

static void print_capture_stats(const struct capture_stats *stats)
{
	unsigned int channel;

	if (stats->frames == 0)
		return;

	for (channel = 0; channel < AV_CHANNELS; ++channel) {
		double mean = (double)stats->sum[channel] / (double)stats->frames;
		double mean_square =
			(double)stats->square_sum[channel] / (double)stats->frames;
		double rms = positive_square_root(mean_square);

		printf("  channel %c     : peak=%d (%.2f%%), rms=%.2f (%.2f%%), ",
		       channel == 0 ? 'L' : 'R',
		       stats->peak[channel],
		       (double)stats->peak[channel] * 100.0 / 32768.0,
		       rms, rms * 100.0 / 32768.0);
		printf("mean=%.2f clipped=%llu\n",
		       mean, (unsigned long long)stats->clipped[channel]);
	}
}

static void put_le16(uint8_t *destination, uint16_t value)
{
	destination[0] = (uint8_t)(value & 0xffU);
	destination[1] = (uint8_t)((value >> 8) & 0xffU);
}

static void put_le32(uint8_t *destination, uint32_t value)
{
	destination[0] = (uint8_t)(value & 0xffU);
	destination[1] = (uint8_t)((value >> 8) & 0xffU);
	destination[2] = (uint8_t)((value >> 16) & 0xffU);
	destination[3] = (uint8_t)((value >> 24) & 0xffU);
}

static void build_wav_header(uint8_t header[44], uint32_t data_bytes)
{
	uint32_t byte_rate = AV_SAMPLE_RATE * AV_BYTES_PER_FRAME;
	uint16_t block_align = (uint16_t)AV_BYTES_PER_FRAME;

	memset(header, 0, 44);
	memcpy(header + 0, "RIFF", 4);
	put_le32(header + 4, 36U + data_bytes);
	memcpy(header + 8, "WAVE", 4);
	memcpy(header + 12, "fmt ", 4);
	put_le32(header + 16, 16U);                 /* PCM fmt chunk size */
	put_le16(header + 20, 1U);                  /* WAVE_FORMAT_PCM */
	put_le16(header + 22, (uint16_t)AV_CHANNELS);
	put_le32(header + 24, AV_SAMPLE_RATE);
	put_le32(header + 28, byte_rate);
	put_le16(header + 32, block_align);
	put_le16(header + 34, (uint16_t)AV_SAMPLE_BITS);
	memcpy(header + 36, "data", 4);
	put_le32(header + 40, data_bytes);
}

static int write_all(int fd, const void *data, size_t bytes)
{
	const uint8_t *position = data;

	while (bytes > 0) {
		ssize_t written = write(fd, position, bytes);

		if (written < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (written == 0) {
			errno = EIO;
			return -1;
		}
		position += written;
		bytes -= (size_t)written;
	}
	return 0;
}

static int save_wav_file(const char *path,
			 const int16_t *samples,
			 uint64_t frames)
{
	uint64_t data_bytes_64 = frames * AV_BYTES_PER_FRAME;
	uint8_t header[44];
	uint32_t data_bytes;
	int fd;
	int ret = -1;

	if (data_bytes_64 > UINT32_MAX - 36U) {
		fprintf(stderr, "Captured PCM is too large for a classic RIFF/WAV file\n");
		errno = EFBIG;
		return -1;
	}
	data_bytes = (uint32_t)data_bytes_64;
	build_wav_header(header, data_bytes);

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0) {
		fprintf(stderr, "Cannot create %s: %s\n", path, strerror(errno));
		return -1;
	}

	if (write_all(fd, header, sizeof(header)) < 0 ||
	    write_all(fd, samples, data_bytes) < 0) {
		fprintf(stderr, "Cannot write WAV %s: %s\n", path, strerror(errno));
		goto out;
	}
	if (close(fd) < 0) {
		fd = -1;
		fprintf(stderr, "Cannot close WAV %s: %s\n", path, strerror(errno));
		return -1;
	}
	fd = -1;
	ret = 0;

out:
	if (fd >= 0)
		close(fd);
	return ret;
}

static double elapsed_seconds(const struct timeval *start,
			      const struct timeval *end)
{
	return (double)(end->tv_sec - start->tv_sec) +
	       (double)(end->tv_usec - start->tv_usec) / 1000000.0;
}

static int parse_seconds(const char *text, unsigned int *seconds)
{
	char *end = NULL;
	unsigned long value;

	errno = 0;
	value = strtoul(text, &end, 10);
	if (errno || !end || *end != '\0' || value == 0 ||
	    value > AV_MAX_SECONDS) {
		fprintf(stderr, "Duration must be 1..%u seconds\n", AV_MAX_SECONDS);
		return -1;
	}
	*seconds = (unsigned int)value;
	return 0;
}

static void print_usage(const char *program)
{
	printf("Usage: %s [pcm-device] [seconds] [output-wav] [control-device] ",
	       program);
	printf("[mixer-profile]\n");
	printf("Defaults: /dev/snd/pcmC0D0c %u capture_48k_stereo_s16.wav ",
	       AV_DEFAULT_SECONDS);
	printf("/dev/snd/controlC0 baseline\n");
	printf("Profiles: baseline, mainmic-route, mainmic-gain.\n");
	printf("Format is fixed by AUDIO-R2.1: 48000 Hz, stereo, signed 16-bit LE.\n");
}

int main(int argc, char **argv)
{
	const char *pcm_path = "/dev/snd/pcmC0D0c";
	const char *output_path = "capture_48k_stereo_s16.wav";
	const char *control_path = "/dev/snd/controlC0";
	enum mixer_profile profile = MIXER_PROFILE_BASELINE;
	unsigned int seconds = AV_DEFAULT_SECONDS;
	unsigned int period_frames = 0;
	unsigned int period_count = 0;
	unsigned int buffer_frames = 0;
	unsigned int xruns = 0;
	unsigned int zero_reads = 0;
	uint64_t target_frames;
	uint64_t target_bytes;
	uint64_t captured_frames = 0;
	int16_t *pcm_data = NULL;
	struct mixer_transaction mixer;
	struct capture_stats stats;
	struct timeval start_time;
	struct timeval end_time;
	double elapsed = 0.0;
	int pcm_fd = -1;
	int hw_parameters_committed = 0;
	int hw_configured = 0;
	int stream_prepared = 0;
	int capture_failed = 0;
	int exit_code = EXIT_FAILURE;

	memset(&mixer, 0, sizeof(mixer));
	mixer.fd = -1;
	memset(&stats, 0, sizeof(stats));

	if (argc > 1 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
		print_usage(argv[0]);
		return EXIT_SUCCESS;
	}
	if (argc > 6) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (argc >= 2)
		pcm_path = argv[1];
	if (argc >= 3 && parse_seconds(argv[2], &seconds) < 0)
		return EXIT_FAILURE;
	if (argc >= 4)
		output_path = argv[3];
	if (argc >= 5)
		control_path = argv[4];
	if (argc >= 6 && parse_mixer_profile(argv[5], &profile) < 0)
		return EXIT_FAILURE;

	if (install_signal_handlers() < 0)
		return EXIT_FAILURE;

	target_frames = (uint64_t)AV_SAMPLE_RATE * seconds;
	if (target_frames > SIZE_MAX / AV_BYTES_PER_FRAME) {
		fprintf(stderr, "Requested capture buffer would overflow size_t\n");
		return EXIT_FAILURE;
	}
	target_bytes = target_frames * AV_BYTES_PER_FRAME;
	pcm_data = malloc((size_t)target_bytes);
	if (!pcm_data) {
		fprintf(stderr, "Cannot allocate %llu-byte capture buffer: %s\n",
			(unsigned long long)target_bytes, strerror(errno));
		return EXIT_FAILURE;
	}

	printf("AUDIO-R2.1 WM8960 PCM capture and main-MIC A/B test\n");
	printf("PCM device      : %s\n", pcm_path);
	printf("control device  : %s\n", control_path);
	printf("target          : %u Hz, %u channels, S16_LE, %u seconds\n",
	       AV_SAMPLE_RATE, AV_CHANNELS, seconds);
	printf("target storage  : %llu frames, %llu bytes in RAM\n",
	       (unsigned long long)target_frames,
	       (unsigned long long)target_bytes);
	printf("output WAV      : %s\n", output_path);

	if (configure_mixer(control_path, profile, &mixer) < 0)
		goto cleanup;

	pcm_fd = open(pcm_path, O_RDONLY | O_CLOEXEC);
	if (pcm_fd < 0) {
		fprintf(stderr, "Cannot open capture PCM %s: %s\n",
			pcm_path, strerror(errno));
		goto cleanup;
	}

	if (configure_hardware(pcm_fd, &period_frames, &period_count,
			       &buffer_frames,
			       &hw_parameters_committed) < 0) {
		hw_configured = hw_parameters_committed;
		goto cleanup;
	}
	hw_configured = 1;

	if (configure_software(pcm_fd, period_frames, buffer_frames) < 0)
		goto cleanup;
	if (xioctl_noarg(pcm_fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
		fprintf(stderr, "SNDRV_PCM_IOCTL_PREPARE failed: %s\n",
			strerror(errno));
		goto cleanup;
	}
	stream_prepared = 1;

	printf("stream state    : PREPARED; first READI_FRAMES starts capture\n");
	if (gettimeofday(&start_time, NULL) < 0) {
		fprintf(stderr, "gettimeofday(start) failed: %s\n", strerror(errno));
		goto cleanup;
	}

	while (captured_frames < target_frames && !stop_requested) {
		uint64_t remaining = target_frames - captured_frames;
		unsigned int request = period_frames;
		int got;

		if (remaining < request)
			request = (unsigned int)remaining;
		got = read_pcm_frames(pcm_fd,
				      pcm_data + captured_frames * AV_CHANNELS,
				      request, &xruns);
		if (got == -2)
			break;
		if (got < 0) {
			capture_failed = 1;
			break;
		}
		if (got == 0) {
			if (++zero_reads > 10) {
				fprintf(stderr, "Too many zero-length PCM reads\n");
				capture_failed = 1;
				break;
			}
			continue;
		}
		zero_reads = 0;
		update_capture_stats(&stats,
				     pcm_data + captured_frames * AV_CHANNELS,
				     (unsigned int)got);
		captured_frames += (unsigned int)got;

		if (captured_frames == (unsigned int)got ||
		    captured_frames == target_frames ||
		    captured_frames % (AV_SAMPLE_RATE) < (unsigned int)got) {
			printf("  captured      : %llu/%llu frames (%.1f%%)\n",
			       (unsigned long long)captured_frames,
			       (unsigned long long)target_frames,
			       (double)captured_frames * 100.0 / (double)target_frames);
		}
	}

	if (gettimeofday(&end_time, NULL) == 0)
		elapsed = elapsed_seconds(&start_time, &end_time);

cleanup:
	/* DROP transitions RUNNING/XRUN/PREPARED back to SETUP and stops DMA. */
	if (pcm_fd >= 0 && stream_prepared) {
		if (xioctl_noarg(pcm_fd, SNDRV_PCM_IOCTL_DROP) < 0)
			fprintf(stderr, "Warning: SNDRV_PCM_IOCTL_DROP failed: %s\n",
				strerror(errno));
		else
			printf("stream state    : DROPPED (SAI/SDMA stopped)\n");
	}
	if (pcm_fd >= 0 && hw_configured) {
		if (xioctl_noarg(pcm_fd, SNDRV_PCM_IOCTL_HW_FREE) < 0)
			fprintf(stderr, "Warning: SNDRV_PCM_IOCTL_HW_FREE failed: %s\n",
				strerror(errno));
		else
			printf("hardware state  : HW_FREE\n");
	}
	if (pcm_fd >= 0) {
		close(pcm_fd);
		pcm_fd = -1;
	}
	restore_mixer(&mixer);

	if (captured_frames > 0) {
		printf("capture stats   : frames=%llu bytes=%llu xruns=%u\n",
		       (unsigned long long)captured_frames,
		       (unsigned long long)(captured_frames * AV_BYTES_PER_FRAME),
		       xruns);
		if (elapsed > 0.0) {
			printf("  elapsed       : %.3f s\n", elapsed);
			printf("  measured rate : %.2f frames/s\n",
			       (double)captured_frames / elapsed);
		}
		print_capture_stats(&stats);

		/* File I/O starts only after PCM DMA has been stopped and closed. */
		if (save_wav_file(output_path, pcm_data, captured_frames) < 0)
			goto done;
		printf("WAV saved       : %s (%llu bytes including 44-byte header)\n",
		       output_path,
		       (unsigned long long)(captured_frames * AV_BYTES_PER_FRAME + 44U));

		if (capture_failed) {
			printf("[FAIL] Capture ended because of an unrecovered PCM error; ");
			printf("partial WAV was preserved.\n");
		} else if (stop_requested) {
			printf("[STOP] Signal requested a clean stop; partial WAV is valid.\n");
			exit_code = EXIT_SUCCESS;
		} else if (captured_frames == target_frames && xruns == 0) {
			printf("[PASS] Captured the requested PCM with zero XRUN and wrote WAV.\n");
			exit_code = EXIT_SUCCESS;
		} else {
			printf("[FAIL] Capture length or XRUN acceptance criterion was not met.\n");
		}
	} else {
		printf("[FAIL] No PCM frames were captured; no WAV file was written.\n");
	}

done:
	free(pcm_data);
	return exit_code;
}
