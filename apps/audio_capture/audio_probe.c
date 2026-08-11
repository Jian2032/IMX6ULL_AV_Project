/*
 * audio_probe.c - AUDIO-R1: ALSA/ASoC read-only diagnostic program
 *
 * Target platform:
 *   NXP i.MX6ULL + SAI2 + WM8960, Linux 4.1.15, BusyBox root filesystem.
 *
 * Why this program does not use libasound:
 *   The current root filesystem has /dev/snd nodes but does not yet contain
 *   alsa-lib, arecord or amixer.  ALSA's public UAPI is nevertheless available
 *   through <sound/asound.h>.  Talking to that UAPI directly lets AUDIO-R1 run
 *   without adding a shared-library dependency and makes the kernel interface
 *   visible for learning.
 *
 * Safety boundary of AUDIO-R1:
 *   - Queries the control device, PCM list and mixer controls.
 *   - Opens the capture PCM and calls HW_REFINE to inspect constraints.
 *   - Never calls HW_PARAMS, SW_PARAMS, PREPARE, START or READI_FRAMES.
 *   - Never writes a mixer control.
 *
 * Therefore AUDIO-R1 does not start SAI DMA and does not record audio.
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sound/asound.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MAX_CONTROL_ELEMENTS 4096U
#define MAX_PRINT_VALUES 8U

struct value_name {
	int value;
	const char *name;
};

static const struct value_name access_names[] = {
	{ SNDRV_PCM_ACCESS_MMAP_INTERLEAVED,    "MMAP_INTERLEAVED" },
	{ SNDRV_PCM_ACCESS_MMAP_NONINTERLEAVED, "MMAP_NONINTERLEAVED" },
	{ SNDRV_PCM_ACCESS_MMAP_COMPLEX,        "MMAP_COMPLEX" },
	{ SNDRV_PCM_ACCESS_RW_INTERLEAVED,      "RW_INTERLEAVED" },
	{ SNDRV_PCM_ACCESS_RW_NONINTERLEAVED,   "RW_NONINTERLEAVED" },
};

/*
 * Keep the table explicit instead of indexing a sparse array: ALSA format
 * numbers are ABI values and contain gaps (for example SPECIAL is 31).
 */
static const struct value_name format_names[] = {
	{ SNDRV_PCM_FORMAT_S8,       "S8" },
	{ SNDRV_PCM_FORMAT_U8,       "U8" },
	{ SNDRV_PCM_FORMAT_S16_LE,   "S16_LE" },
	{ SNDRV_PCM_FORMAT_S16_BE,   "S16_BE" },
	{ SNDRV_PCM_FORMAT_U16_LE,   "U16_LE" },
	{ SNDRV_PCM_FORMAT_U16_BE,   "U16_BE" },
	{ SNDRV_PCM_FORMAT_S24_LE,   "S24_LE" },
	{ SNDRV_PCM_FORMAT_S24_BE,   "S24_BE" },
	{ SNDRV_PCM_FORMAT_U24_LE,   "U24_LE" },
	{ SNDRV_PCM_FORMAT_U24_BE,   "U24_BE" },
	{ SNDRV_PCM_FORMAT_S32_LE,   "S32_LE" },
	{ SNDRV_PCM_FORMAT_S32_BE,   "S32_BE" },
	{ SNDRV_PCM_FORMAT_U32_LE,   "U32_LE" },
	{ SNDRV_PCM_FORMAT_U32_BE,   "U32_BE" },
	{ SNDRV_PCM_FORMAT_MU_LAW,   "MU_LAW" },
	{ SNDRV_PCM_FORMAT_A_LAW,    "A_LAW" },
	{ SNDRV_PCM_FORMAT_S24_3LE,  "S24_3LE" },
	{ SNDRV_PCM_FORMAT_S24_3BE,  "S24_3BE" },
	{ SNDRV_PCM_FORMAT_S20_3LE,  "S20_3LE" },
	{ SNDRV_PCM_FORMAT_S20_3BE,  "S20_3BE" },
	{ SNDRV_PCM_FORMAT_S18_3LE,  "S18_3LE" },
	{ SNDRV_PCM_FORMAT_S18_3BE,  "S18_3BE" },
};

static int xioctl(int fd, unsigned long request, void *argument)
{
	int ret;

	/* EINTR means a signal interrupted the syscall; retry the same query. */
	do {
		ret = ioctl(fd, request, argument);
	} while (ret < 0 && errno == EINTR);

	return ret;
}

static void print_protocol_version(const char *label, int version)
{
	printf("  %-16s: %u.%u.%u (0x%08x)\n",
	       label,
	       ((unsigned int)version >> 16) & 0xffffU,
	       ((unsigned int)version >> 8) & 0xffU,
	       (unsigned int)version & 0xffU,
	       (unsigned int)version);
}

static int validate_character_device(const char *path)
{
	struct stat st;

	if (stat(path, &st) < 0) {
		fprintf(stderr, "Cannot stat %s: %s\n", path, strerror(errno));
		return -1;
	}

	if (!S_ISCHR(st.st_mode)) {
		fprintf(stderr, "%s exists but is not a character device\n", path);
		return -1;
	}

	printf("  %-16s: character device, major=%u minor=%u\n",
	       path,
	       (unsigned int)((st.st_rdev >> 8) & 0xfffU),
	       (unsigned int)((st.st_rdev & 0xffU) |
	                      ((st.st_rdev >> 12) & 0xfff00U)));
	return 0;
}

static int ascii_contains_case_insensitive(const char *text, const char *word)
{
	size_t text_len;
	size_t word_len;
	size_t i;
	size_t j;

	text_len = strlen(text);
	word_len = strlen(word);
	if (word_len == 0 || word_len > text_len)
		return 0;

	for (i = 0; i + word_len <= text_len; ++i) {
		for (j = 0; j < word_len; ++j) {
			if (tolower((unsigned char)text[i + j]) !=
			    tolower((unsigned char)word[j]))
				break;
		}
		if (j == word_len)
			return 1;
	}

	return 0;
}

static int is_capture_related_control(const char *name)
{
	static const char *const words[] = {
		"capture", "adc", "input", "mic", "boost", "alc", "noise"
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(words); ++i) {
		if (ascii_contains_case_insensitive(name, words[i]))
			return 1;
	}
	return 0;
}

static const char *control_type_name(int type)
{
	switch (type) {
	case SNDRV_CTL_ELEM_TYPE_BOOLEAN:
		return "BOOLEAN";
	case SNDRV_CTL_ELEM_TYPE_INTEGER:
		return "INTEGER";
	case SNDRV_CTL_ELEM_TYPE_ENUMERATED:
		return "ENUM";
	case SNDRV_CTL_ELEM_TYPE_BYTES:
		return "BYTES";
	case SNDRV_CTL_ELEM_TYPE_IEC958:
		return "IEC958";
	case SNDRV_CTL_ELEM_TYPE_INTEGER64:
		return "INTEGER64";
	default:
		return "UNKNOWN";
	}
}

static void print_control_access(unsigned int access)
{
	printf("%c%c%c%c",
	       (access & SNDRV_CTL_ELEM_ACCESS_READ) ? 'R' : '-',
	       (access & SNDRV_CTL_ELEM_ACCESS_WRITE) ? 'W' : '-',
	       (access & SNDRV_CTL_ELEM_ACCESS_VOLATILE) ? 'V' : '-',
	       (access & SNDRV_CTL_ELEM_ACCESS_INACTIVE) ? 'I' : '-');
}

static void print_enumerated_value(int fd,
				   const struct snd_ctl_elem_info *base,
				   unsigned int item)
{
	struct snd_ctl_elem_info item_info;

	memset(&item_info, 0, sizeof(item_info));
	item_info.id = base->id;
	item_info.value.enumerated.item = item;

	if (xioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &item_info) == 0)
		printf("%u(%.*s)", item, 64, item_info.value.enumerated.name);
	else
		printf("%u", item);
}

static void print_control_value(int fd, const struct snd_ctl_elem_info *info)
{
	struct snd_ctl_elem_value value;
	unsigned int count;
	unsigned int i;

	if (!(info->access & SNDRV_CTL_ELEM_ACCESS_READ)) {
		printf(" value=<not readable>");
		return;
	}

	memset(&value, 0, sizeof(value));
	value.id = info->id;
	if (xioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &value) < 0) {
		printf(" value=<read failed: %s>", strerror(errno));
		return;
	}

	count = info->count;
	if (count > MAX_PRINT_VALUES)
		count = MAX_PRINT_VALUES;

	printf(" value=");
	switch (info->type) {
	case SNDRV_CTL_ELEM_TYPE_BOOLEAN:
	case SNDRV_CTL_ELEM_TYPE_INTEGER:
		for (i = 0; i < count; ++i)
			printf("%s%ld", i ? "," : "", value.value.integer.value[i]);
		break;
	case SNDRV_CTL_ELEM_TYPE_INTEGER64:
		for (i = 0; i < count; ++i)
			printf("%s%lld", i ? "," : "",
			       (long long)value.value.integer64.value[i]);
		break;
	case SNDRV_CTL_ELEM_TYPE_ENUMERATED:
		for (i = 0; i < count; ++i) {
			if (i)
				printf(",");
			print_enumerated_value(fd, info,
					       value.value.enumerated.item[i]);
		}
		break;
	case SNDRV_CTL_ELEM_TYPE_BYTES:
		for (i = 0; i < count; ++i)
			printf("%s0x%02x", i ? "," : "",
			       value.value.bytes.data[i]);
		break;
	default:
		printf("<type not decoded>");
		break;
	}

	if (info->count > MAX_PRINT_VALUES)
		printf(",...(%u values)", info->count);
}

static void print_control_range(const struct snd_ctl_elem_info *info)
{
	switch (info->type) {
	case SNDRV_CTL_ELEM_TYPE_INTEGER:
		printf(" range=%ld..%ld step=%ld",
		       info->value.integer.min,
		       info->value.integer.max,
		       info->value.integer.step);
		break;
	case SNDRV_CTL_ELEM_TYPE_INTEGER64:
		printf(" range=%lld..%lld step=%lld",
		       (long long)info->value.integer64.min,
		       (long long)info->value.integer64.max,
		       (long long)info->value.integer64.step);
		break;
	case SNDRV_CTL_ELEM_TYPE_ENUMERATED:
		printf(" items=%u", info->value.enumerated.items);
		break;
	default:
		break;
	}
}

static int enumerate_mixer_controls(int control_fd)
{
	struct snd_ctl_elem_list list;
	struct snd_ctl_elem_id *ids = NULL;
	unsigned int capacity;
	unsigned int mixer_count = 0;
	unsigned int capture_count = 0;
	unsigned int i;
	int ret = -1;

	/* First call with space=0 asks ALSA only for the total element count. */
	memset(&list, 0, sizeof(list));
	if (xioctl(control_fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0) {
		fprintf(stderr, "SNDRV_CTL_IOCTL_ELEM_LIST(count) failed: %s\n",
			strerror(errno));
		return -1;
	}

	printf("  control elements: %u total\n", list.count);
	if (list.count == 0) {
		fprintf(stderr, "No ALSA control elements were registered\n");
		return -1;
	}
	if (list.count > MAX_CONTROL_ELEMENTS) {
		fprintf(stderr, "Refusing unreasonable control count %u\n", list.count);
		return -1;
	}
	capacity = list.count;

	ids = calloc(capacity, sizeof(*ids));
	if (!ids) {
		fprintf(stderr, "Cannot allocate control ID list: %s\n", strerror(errno));
		return -1;
	}

	/* Second call supplies storage into which ALSA copies the element IDs. */
	memset(&list, 0, sizeof(list));
	list.offset = 0;
	list.space = capacity;
	list.pids = ids;
	if (xioctl(control_fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0) {
		fprintf(stderr, "SNDRV_CTL_IOCTL_ELEM_LIST(data) failed: %s\n",
			strerror(errno));
		goto out;
	}

	for (i = 0; i < list.used; ++i) {
		struct snd_ctl_elem_info info;
		char name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN + 1];
		int capture_related;

		if (ids[i].iface != SNDRV_CTL_ELEM_IFACE_MIXER)
			continue;

		memset(&info, 0, sizeof(info));
		info.id = ids[i];
		if (xioctl(control_fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0) {
			fprintf(stderr, "  numid=%u ELEM_INFO failed: %s\n",
				ids[i].numid, strerror(errno));
			continue;
		}

		memcpy(name, info.id.name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
		name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN] = '\0';
		capture_related = is_capture_related_control(name);
		if (capture_related)
			++capture_count;
		++mixer_count;

		printf("  %c numid=%-3u %-43s type=%-7s count=%-2u access=",
		       capture_related ? '*' : ' ',
		       info.id.numid,
		       name,
		       control_type_name(info.type),
		       info.count);
		print_control_access(info.access);
		print_control_range(&info);
		print_control_value(control_fd, &info);
		printf("\n");
	}

	printf("  mixer summary   : %u mixer controls, %u capture-path candidates\n",
	       mixer_count, capture_count);
	printf("  legend          : '*' capture-related, access=Read/Write/Volatile/Inactive\n");
	if (mixer_count == 0) {
		fprintf(stderr, "No mixer-interface controls were registered\n");
		ret = -1;
	} else {
		ret = 0;
	}

out:
	free(ids);
	return ret;
}

static int enumerate_pcm_devices(int control_fd)
{
	int device = -1;
	unsigned int capture_count = 0;
	unsigned int playback_count = 0;

	for (;;) {
		if (xioctl(control_fd, SNDRV_CTL_IOCTL_PCM_NEXT_DEVICE, &device) < 0) {
			fprintf(stderr, "SNDRV_CTL_IOCTL_PCM_NEXT_DEVICE failed: %s\n",
				strerror(errno));
			return -1;
		}
		if (device < 0)
			break;

		{
			int stream;

			for (stream = SNDRV_PCM_STREAM_PLAYBACK;
			     stream <= SNDRV_PCM_STREAM_CAPTURE;
			     ++stream) {
				struct snd_pcm_info info;

				memset(&info, 0, sizeof(info));
				info.device = (unsigned int)device;
				info.subdevice = 0;
				info.stream = stream;
				if (xioctl(control_fd, SNDRV_CTL_IOCTL_PCM_INFO, &info) < 0) {
					if (errno == ENXIO || errno == ENODEV || errno == EINVAL)
						continue;
					fprintf(stderr, "PCM_INFO device=%d stream=%d failed: %s\n",
						device, stream, strerror(errno));
					return -1;
				}

				printf("  pcmC%dD%u%c: id=%.*s name=%.*s subdevices=%u available=%u\n",
				       info.card,
				       info.device,
				       stream == SNDRV_PCM_STREAM_CAPTURE ? 'c' : 'p',
				       64, (const char *)info.id,
				       80, (const char *)info.name,
				       info.subdevices_count,
				       info.subdevices_avail);

				if (stream == SNDRV_PCM_STREAM_CAPTURE)
					++capture_count;
				else
					++playback_count;
			}
		}
	}

	printf("  PCM summary     : %u capture endpoint(s), %u playback endpoint(s)\n",
	       capture_count, playback_count);
	return capture_count ? 0 : -1;
}

static struct snd_mask *param_mask(struct snd_pcm_hw_params *params,
					   unsigned int parameter)
{
	return &params->masks[parameter - SNDRV_PCM_HW_PARAM_FIRST_MASK];
}

static const struct snd_mask *param_mask_const(const struct snd_pcm_hw_params *params,
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

static void hw_params_any(struct snd_pcm_hw_params *params)
{
	unsigned int parameter;

	memset(params, 0, sizeof(*params));

	for (parameter = SNDRV_PCM_HW_PARAM_FIRST_MASK;
	     parameter <= SNDRV_PCM_HW_PARAM_LAST_MASK;
	     ++parameter) {
		memset(param_mask(params, parameter)->bits, 0xff,
		       sizeof(param_mask(params, parameter)->bits));
		params->cmask |= 1U << parameter;
		params->rmask |= 1U << parameter;
	}

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
		params->cmask |= 1U << parameter;
		params->rmask |= 1U << parameter;
	}

	params->info = ~0U;
}

static void set_mask_value(struct snd_pcm_hw_params *params,
			   unsigned int parameter,
			   unsigned int value)
{
	struct snd_mask *mask = param_mask(params, parameter);

	memset(mask->bits, 0, sizeof(mask->bits));
	if (value < SNDRV_MASK_MAX)
		mask->bits[value / 32U] |= 1U << (value % 32U);
	params->rmask |= 1U << parameter;
	params->cmask |= 1U << parameter;
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
	params->rmask |= 1U << parameter;
	params->cmask |= 1U << parameter;
}

static int mask_has_value(const struct snd_mask *mask, unsigned int value)
{
	if (value >= SNDRV_MASK_MAX)
		return 0;
	return !!(mask->bits[value / 32U] & (1U << (value % 32U)));
}

static void print_named_mask(const char *label,
			     const struct snd_mask *mask,
			     const struct value_name *names,
			     size_t name_count)
{
	size_t i;
	int printed = 0;

	printf("  %-16s:", label);
	for (i = 0; i < name_count; ++i) {
		if (mask_has_value(mask, (unsigned int)names[i].value)) {
			printf(" %s", names[i].name);
			printed = 1;
		}
	}
	if (!printed)
		printf(" <none from known table>");
	printf("\n");
}

static void print_interval(const char *label,
			   const struct snd_interval *interval,
			   const char *unit)
{
	printf("  %-16s: %c%u..%u%c%s%s\n",
	       label,
	       interval->openmin ? '(' : '[',
	       interval->min,
	       interval->max,
	       interval->openmax ? ')' : ']',
	       unit,
	       interval->integer ? " integer" : "");
}

static int refine_profile(int pcm_fd, int access, const char *access_name)
{
	struct snd_pcm_hw_params params;

	hw_params_any(&params);
	set_mask_value(&params, SNDRV_PCM_HW_PARAM_ACCESS, (unsigned int)access);
	set_mask_value(&params, SNDRV_PCM_HW_PARAM_FORMAT,
		       (unsigned int)SNDRV_PCM_FORMAT_S16_LE);
	set_mask_value(&params, SNDRV_PCM_HW_PARAM_SUBFORMAT,
		       (unsigned int)SNDRV_PCM_SUBFORMAT_STD);
	set_interval_value(&params, SNDRV_PCM_HW_PARAM_CHANNELS, 2);
	set_interval_value(&params, SNDRV_PCM_HW_PARAM_RATE, 48000);

	if (xioctl(pcm_fd, SNDRV_PCM_IOCTL_HW_REFINE, &params) < 0) {
		printf("  48k/S16_LE/2ch %-18s: unsupported (%s)\n",
		       access_name, strerror(errno));
		return -1;
	}

	printf("  48k/S16_LE/2ch %-18s: supported\n", access_name);
	return 0;
}

static int probe_capture_pcm(const char *pcm_path)
{
	struct snd_pcm_hw_params params;
	struct snd_pcm_info info;
	int version;
	int fd;
	int rw_profile_ok;

	fd = open(pcm_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "Cannot open capture PCM %s: %s\n",
			pcm_path, strerror(errno));
		return -1;
	}

	version = 0;
	if (xioctl(fd, SNDRV_PCM_IOCTL_PVERSION, &version) < 0) {
		fprintf(stderr, "SNDRV_PCM_IOCTL_PVERSION failed: %s\n", strerror(errno));
		close(fd);
		return -1;
	}
	print_protocol_version("PCM protocol", version);

	memset(&info, 0, sizeof(info));
	if (xioctl(fd, SNDRV_PCM_IOCTL_INFO, &info) < 0) {
		fprintf(stderr, "SNDRV_PCM_IOCTL_INFO failed: %s\n", strerror(errno));
		close(fd);
		return -1;
	}

	printf("  PCM identity    : card=%d device=%u subdevice=%u stream=%s\n",
	       info.card, info.device, info.subdevice,
	       info.stream == SNDRV_PCM_STREAM_CAPTURE ? "capture" : "playback");
	printf("  PCM id/name     : %.*s / %.*s\n",
	       64, (const char *)info.id, 80, (const char *)info.name);
	printf("  PCM subdevice   : %.*s\n", 32, (const char *)info.subname);

	hw_params_any(&params);
	if (xioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, &params) < 0) {
		fprintf(stderr, "SNDRV_PCM_IOCTL_HW_REFINE(any) failed: %s\n",
			strerror(errno));
		close(fd);
		return -1;
	}

	print_named_mask("access modes",
			 param_mask_const(&params, SNDRV_PCM_HW_PARAM_ACCESS),
			 access_names, ARRAY_SIZE(access_names));
	print_named_mask("sample formats",
			 param_mask_const(&params, SNDRV_PCM_HW_PARAM_FORMAT),
			 format_names, ARRAY_SIZE(format_names));
	print_interval("channels",
		       param_interval_const(&params, SNDRV_PCM_HW_PARAM_CHANNELS), "");
	print_interval("sample rates",
		       param_interval_const(&params, SNDRV_PCM_HW_PARAM_RATE), " Hz");
	print_interval("period size",
		       param_interval_const(&params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE),
		       " frames");
	print_interval("period time",
		       param_interval_const(&params, SNDRV_PCM_HW_PARAM_PERIOD_TIME),
		       " us");
	print_interval("periods",
		       param_interval_const(&params, SNDRV_PCM_HW_PARAM_PERIODS), "");
	print_interval("buffer size",
		       param_interval_const(&params, SNDRV_PCM_HW_PARAM_BUFFER_SIZE),
		       " frames");
	print_interval("buffer time",
		       param_interval_const(&params, SNDRV_PCM_HW_PARAM_BUFFER_TIME),
		       " us");

	printf("\n[6] Target-profile combination tests (HW_REFINE only)\n");
	rw_profile_ok = refine_profile(fd, SNDRV_PCM_ACCESS_RW_INTERLEAVED,
				       "RW_INTERLEAVED");
	(void)refine_profile(fd, SNDRV_PCM_ACCESS_MMAP_INTERLEAVED,
			     "MMAP_INTERLEAVED");

	close(fd);
	return rw_profile_ok;
}

static int probe_control(const char *control_path)
{
	struct snd_ctl_card_info card;
	int version;
	int fd;
	int ret = -1;

	fd = open(control_path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "Cannot open control device %s: %s\n",
			control_path, strerror(errno));
		return -1;
	}

	version = 0;
	if (xioctl(fd, SNDRV_CTL_IOCTL_PVERSION, &version) < 0) {
		fprintf(stderr, "SNDRV_CTL_IOCTL_PVERSION failed: %s\n", strerror(errno));
		goto out;
	}
	print_protocol_version("control protocol", version);

	memset(&card, 0, sizeof(card));
	if (xioctl(fd, SNDRV_CTL_IOCTL_CARD_INFO, &card) < 0) {
		fprintf(stderr, "SNDRV_CTL_IOCTL_CARD_INFO failed: %s\n", strerror(errno));
		goto out;
	}

	printf("  card number     : %d\n", card.card);
	printf("  id              : %.*s\n", 16, (const char *)card.id);
	printf("  driver          : %.*s\n", 16, (const char *)card.driver);
	printf("  name            : %.*s\n", 32, (const char *)card.name);
	printf("  long name       : %.*s\n", 80, (const char *)card.longname);
	printf("  mixer name      : %.*s\n", 80, (const char *)card.mixername);
	printf("  components      : %.*s\n", 128, (const char *)card.components);

	printf("\n[3] PCM endpoints registered on this card\n");
	if (enumerate_pcm_devices(fd) < 0)
		goto out;

	printf("\n[4] Mixer controls (read-only values)\n");
	if (enumerate_mixer_controls(fd) < 0)
		goto out;

	ret = 0;
out:
	close(fd);
	return ret;
}

static void print_usage(const char *program)
{
	printf("Usage: %s [control-device] [capture-pcm-device]\n", program);
	printf("Defaults: /dev/snd/controlC0 /dev/snd/pcmC0D0c\n");
	printf("AUDIO-R1 only queries ALSA; it does not configure or record PCM.\n");
}

int main(int argc, char **argv)
{
	const char *control_path = "/dev/snd/controlC0";
	const char *pcm_path = "/dev/snd/pcmC0D0c";
	int control_ok;
	int pcm_ok;

	if (argc > 1 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
		print_usage(argv[0]);
		return EXIT_SUCCESS;
	}
	if (argc > 3) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (argc >= 2)
		control_path = argv[1];
	if (argc >= 3)
		pcm_path = argv[2];

	printf("AUDIO-R1 ALSA/WM8960 diagnostic\n");
	printf("control device  : %s\n", control_path);
	printf("capture PCM     : %s\n", pcm_path);
	printf("operation       : query only; no HW_PARAMS, START, read or mixer write\n");

	printf("\n[1] Device-node validation\n");
	if (validate_character_device(control_path) < 0 ||
	    validate_character_device(pcm_path) < 0)
		return EXIT_FAILURE;

	printf("\n[2] ALSA control protocol and sound-card identity\n");
	control_ok = probe_control(control_path);

	printf("\n[5] Selected capture PCM capabilities\n");
	pcm_ok = probe_capture_pcm(pcm_path);

	if (control_ok == 0 && pcm_ok == 0) {
		printf("\n[PASS] ALSA card, capture PCM, mixer controls and ");
		printf("48kHz/S16_LE/stereo RW profile are available.\n");
		printf("[NEXT] AUDIO-R2 can configure periods/buffer and capture a WAV file.\n");
		return EXIT_SUCCESS;
	}

	printf("\n[FAIL] AUDIO-R1 found an ALSA capability or access problem.\n");
	return EXIT_FAILURE;
}
