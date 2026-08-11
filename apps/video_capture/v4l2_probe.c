/*
 * v4l2_probe.c - VIDEO-R1: V4L2 capture-device diagnostic utility
 *
 * Target platform:
 *   - i.MX6ULL / Linux 4.1.15
 *   - mx6s-csi host driver + OV5640 sensor
 *   - BusyBox root filesystem without v4l2-ctl
 *
 * R1 is deliberately read-only: it queries the device but does not call
 * VIDIOC_S_FMT, request buffers, or start streaming.  This lets us verify the
 * existing kernel/video pipeline before the R2 MMAP capture code changes any
 * runtime state.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define DEFAULT_VIDEO_DEVICE "/dev/video0"
#define MAX_ENUM_ITEMS        64U

/*
 * ioctl() may be interrupted by a signal before the driver handles it.
 * Retrying EINTR in one place keeps all query functions simple and prevents a
 * harmless signal from being reported as a V4L2 failure.
 */
static int xioctl(int fd, unsigned long request, void *arg)
{
	int ret;

	do {
		ret = ioctl(fd, request, arg);
	} while (ret == -1 && errno == EINTR);

	return ret;
}

/* Convert a V4L2 little-endian fourcc value into a printable four-character string. */
static const char *fourcc_to_string(uint32_t fourcc, char text[5])
{
	unsigned int i;

	for (i = 0; i < 4; ++i) {
		unsigned char ch = (unsigned char)((fourcc >> (i * 8U)) & 0xffU);

		text[i] = isprint(ch) ? (char)ch : '.';
	}
	text[4] = '\0';
	return text;
}

static const char *buffer_type_name(uint32_t type)
{
	switch (type) {
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
		return "video capture (single-planar)";
	case V4L2_BUF_TYPE_VIDEO_OUTPUT:
		return "video output (single-planar)";
#ifdef V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		return "video capture (multi-planar)";
#endif
#ifdef V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		return "video output (multi-planar)";
#endif
	default:
		return "other";
	}
}

static const char *field_name(uint32_t field)
{
	switch (field) {
	case V4L2_FIELD_ANY:
		return "any";
	case V4L2_FIELD_NONE:
		return "none/progressive";
	case V4L2_FIELD_TOP:
		return "top";
	case V4L2_FIELD_BOTTOM:
		return "bottom";
	case V4L2_FIELD_INTERLACED:
		return "interlaced";
	case V4L2_FIELD_SEQ_TB:
		return "sequential top-bottom";
	case V4L2_FIELD_SEQ_BT:
		return "sequential bottom-top";
	case V4L2_FIELD_ALTERNATE:
		return "alternate";
	case V4L2_FIELD_INTERLACED_TB:
		return "interlaced top-bottom";
	case V4L2_FIELD_INTERLACED_BT:
		return "interlaced bottom-top";
	default:
		return "unknown";
	}
}

static const char *colorspace_name(uint32_t colorspace)
{
	switch (colorspace) {
	/* Linux 4.1.15 uses value 0 but has no V4L2_COLORSPACE_DEFAULT name. */
	case 0:
		return "default/unspecified";
	case V4L2_COLORSPACE_SMPTE170M:
		return "SMPTE170M";
	case V4L2_COLORSPACE_SMPTE240M:
		return "SMPTE240M";
	case V4L2_COLORSPACE_REC709:
		return "Rec.709";
	case V4L2_COLORSPACE_470_SYSTEM_M:
		return "470 System M";
	case V4L2_COLORSPACE_470_SYSTEM_BG:
		return "470 System BG";
	case V4L2_COLORSPACE_JPEG:
		return "JPEG/sRGB full range";
	case V4L2_COLORSPACE_SRGB:
		return "sRGB";
	default:
		return "unknown";
	}
}

static void print_capability_flag(uint32_t caps, uint32_t flag,
				  const char *name)
{
	if ((caps & flag) != 0U)
		printf("    - %s\n", name);
}

static void print_capabilities(uint32_t caps)
{
	print_capability_flag(caps, V4L2_CAP_VIDEO_CAPTURE,
			      "V4L2_CAP_VIDEO_CAPTURE");
	print_capability_flag(caps, V4L2_CAP_VIDEO_OUTPUT,
			      "V4L2_CAP_VIDEO_OUTPUT");
	print_capability_flag(caps, V4L2_CAP_VIDEO_OVERLAY,
			      "V4L2_CAP_VIDEO_OVERLAY");
	print_capability_flag(caps, V4L2_CAP_VBI_CAPTURE,
			      "V4L2_CAP_VBI_CAPTURE");
	print_capability_flag(caps, V4L2_CAP_VBI_OUTPUT,
			      "V4L2_CAP_VBI_OUTPUT");
#ifdef V4L2_CAP_VIDEO_CAPTURE_MPLANE
	print_capability_flag(caps, V4L2_CAP_VIDEO_CAPTURE_MPLANE,
			      "V4L2_CAP_VIDEO_CAPTURE_MPLANE");
#endif
#ifdef V4L2_CAP_VIDEO_OUTPUT_MPLANE
	print_capability_flag(caps, V4L2_CAP_VIDEO_OUTPUT_MPLANE,
			      "V4L2_CAP_VIDEO_OUTPUT_MPLANE");
#endif
#ifdef V4L2_CAP_VIDEO_M2M_MPLANE
	print_capability_flag(caps, V4L2_CAP_VIDEO_M2M_MPLANE,
			      "V4L2_CAP_VIDEO_M2M_MPLANE");
#endif
#ifdef V4L2_CAP_VIDEO_M2M
	print_capability_flag(caps, V4L2_CAP_VIDEO_M2M,
			      "V4L2_CAP_VIDEO_M2M");
#endif
	print_capability_flag(caps, V4L2_CAP_READWRITE,
			      "V4L2_CAP_READWRITE");
	print_capability_flag(caps, V4L2_CAP_ASYNCIO,
			      "V4L2_CAP_ASYNCIO");
	print_capability_flag(caps, V4L2_CAP_STREAMING,
			      "V4L2_CAP_STREAMING");
}

static int query_capability(int fd, uint32_t *active_caps)
{
	struct v4l2_capability cap;
	unsigned int major;
	unsigned int minor;
	unsigned int patch;

	memset(&cap, 0, sizeof(cap));
	if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == -1) {
		fprintf(stderr, "VIDIOC_QUERYCAP failed: %s\n", strerror(errno));
		return -1;
	}

	major = (cap.version >> 16) & 0xffU;
	minor = (cap.version >> 8) & 0xffU;
	patch = cap.version & 0xffU;

	printf("\n[1] Device capability\n");
	printf("  driver       : %.*s\n", (int)sizeof(cap.driver),
	       (const char *)cap.driver);
	printf("  card         : %.*s\n", (int)sizeof(cap.card),
	       (const char *)cap.card);
	printf("  bus_info     : %.*s\n", (int)sizeof(cap.bus_info),
	       (const char *)cap.bus_info);
	printf("  driver version: %u.%u.%u (0x%08x)\n",
	       major, minor, patch, cap.version);
	printf("  capabilities: 0x%08x\n", cap.capabilities);

	/*
	 * Since V4L2_CAP_DEVICE_CAPS was introduced, cap.capabilities describes
	 * the whole physical device while cap.device_caps describes this node.
	 * Linux 4.1.15 mx6s-csi sets the flag, so validate device_caps when it is
	 * present and retain compatibility with older drivers when it is absent.
	 */
	if ((cap.capabilities & V4L2_CAP_DEVICE_CAPS) != 0U) {
		*active_caps = cap.device_caps;
		printf("  device_caps  : 0x%08x (used for validation)\n",
		       cap.device_caps);
	} else {
		*active_caps = cap.capabilities;
		printf("  device_caps  : not separately reported\n");
	}

	printf("  active flags :\n");
	print_capabilities(*active_caps);

	if ((*active_caps & V4L2_CAP_VIDEO_CAPTURE) == 0U) {
		fprintf(stderr,
			"ERROR: node is not a single-planar video-capture device.\n");
		return -1;
	}

	if ((*active_caps & V4L2_CAP_STREAMING) == 0U)
		printf("  WARNING      : streaming I/O is not advertised\n");

	return 0;
}

static void print_input_status(uint32_t status)
{
	if (status == 0U) {
		printf("ok");
		return;
	}

	printf("0x%08x", status);
#ifdef V4L2_IN_ST_NO_POWER
	if ((status & V4L2_IN_ST_NO_POWER) != 0U)
		printf(" no-power");
#endif
#ifdef V4L2_IN_ST_NO_SIGNAL
	if ((status & V4L2_IN_ST_NO_SIGNAL) != 0U)
		printf(" no-signal");
#endif
#ifdef V4L2_IN_ST_NO_SYNC
	if ((status & V4L2_IN_ST_NO_SYNC) != 0U)
		printf(" no-sync");
#endif
}

static void query_inputs(int fd)
{
	struct v4l2_input input;
	unsigned int current = 0;
	unsigned int index;
	int have_input = 0;

	printf("\n[2] Video inputs\n");
	if (xioctl(fd, VIDIOC_G_INPUT, &current) == -1)
		printf("  current input: unavailable (%s)\n", strerror(errno));
	else
		printf("  current input: %u\n", current);

	for (index = 0; index < MAX_ENUM_ITEMS; ++index) {
		memset(&input, 0, sizeof(input));
		input.index = index;
		if (xioctl(fd, VIDIOC_ENUMINPUT, &input) == -1) {
			if (errno != EINVAL)
				printf("  VIDIOC_ENUMINPUT[%u] failed: %s\n",
				       index, strerror(errno));
			break;
		}

		have_input = 1;
		printf("  input[%u]     : %.*s%s\n", index,
		       (int)sizeof(input.name), (const char *)input.name,
		       index == current ? " [selected]" : "");
		printf("    type       : %u (%s)\n", input.type,
		       input.type == V4L2_INPUT_TYPE_CAMERA ? "camera" : "other");
		printf("    status     : ");
		print_input_status(input.status);
		printf("\n");
		printf("    std mask   : 0x%016llx\n",
		       (unsigned long long)input.std);
	}

	if (!have_input)
		printf("  no input entries exposed by the driver\n");
}

static void print_fraction(const struct v4l2_fract *value)
{
	if (value->numerator == 0U || value->denominator == 0U) {
		printf("%u/%u (invalid or not initialized)",
		       value->numerator, value->denominator);
		return;
	}

	printf("%u/%u s (%.2f fps)", value->numerator, value->denominator,
	       (double)value->denominator / (double)value->numerator);
}

static void enumerate_intervals(int fd, uint32_t pixel_format,
				uint32_t width, uint32_t height)
{
	struct v4l2_frmivalenum interval;
	unsigned int index;
	int found = 0;

	for (index = 0; index < MAX_ENUM_ITEMS; ++index) {
		memset(&interval, 0, sizeof(interval));
		interval.index = index;
		interval.pixel_format = pixel_format;
		interval.width = width;
		interval.height = height;

		if (xioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &interval) == -1) {
			if (errno != EINVAL)
				printf("        frame intervals: query failed (%s)\n",
				       strerror(errno));
			break;
		}

		found = 1;
		printf("        interval[%u] : ", index);
		switch (interval.type) {
		case V4L2_FRMIVAL_TYPE_DISCRETE:
			print_fraction(&interval.discrete);
			break;
		case V4L2_FRMIVAL_TYPE_CONTINUOUS:
		case V4L2_FRMIVAL_TYPE_STEPWISE:
			printf("min ");
			print_fraction(&interval.stepwise.min);
			printf(", max ");
			print_fraction(&interval.stepwise.max);
			printf(", step ");
			print_fraction(&interval.stepwise.step);
			break;
		default:
			printf("unknown type %u", interval.type);
			break;
		}
		printf("\n");
	}

	if (!found)
		printf("        frame intervals: not exposed for this size\n");
}

static void enumerate_frame_sizes(int fd, uint32_t pixel_format)
{
	struct v4l2_frmsizeenum size;
	unsigned int index;
	int found = 0;

	for (index = 0; index < MAX_ENUM_ITEMS; ++index) {
		memset(&size, 0, sizeof(size));
		size.index = index;
		size.pixel_format = pixel_format;

		if (xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) == -1) {
			if (errno != EINVAL)
				printf("      frame sizes: query failed (%s)\n",
				       strerror(errno));
			break;
		}

		found = 1;
		switch (size.type) {
		case V4L2_FRMSIZE_TYPE_DISCRETE:
			printf("      size[%u]     : %ux%u\n", index,
			       size.discrete.width, size.discrete.height);
			enumerate_intervals(fd, pixel_format,
					    size.discrete.width,
					    size.discrete.height);
			break;
		case V4L2_FRMSIZE_TYPE_CONTINUOUS:
		case V4L2_FRMSIZE_TYPE_STEPWISE:
			printf("      size[%u]     : %ux%u .. %ux%u, step %ux%u\n",
			       index,
			       size.stepwise.min_width,
			       size.stepwise.min_height,
			       size.stepwise.max_width,
			       size.stepwise.max_height,
			       size.stepwise.step_width,
			       size.stepwise.step_height);
			break;
		default:
			printf("      size[%u]     : unknown type %u\n",
			       index, size.type);
			break;
		}
	}

	if (!found)
		printf("      frame sizes  : not exposed for this format\n");
}

static int enumerate_formats(int fd)
{
	struct v4l2_fmtdesc format;
	unsigned int index;
	int found = 0;

	printf("\n[3] Capture formats, sizes and frame intervals\n");
	for (index = 0; index < MAX_ENUM_ITEMS; ++index) {
		char fourcc[5];

		memset(&format, 0, sizeof(format));
		format.index = index;
		format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

		if (xioctl(fd, VIDIOC_ENUM_FMT, &format) == -1) {
			if (errno != EINVAL)
				printf("  VIDIOC_ENUM_FMT[%u] failed: %s\n",
				       index, strerror(errno));
			break;
		}

		found = 1;
		printf("  format[%u]    : '%s' (0x%08x), %.*s\n",
		       index,
		       fourcc_to_string(format.pixelformat, fourcc),
		       format.pixelformat,
		       (int)sizeof(format.description),
		       (const char *)format.description);
		printf("    flags       : 0x%08x", format.flags);
		if ((format.flags & V4L2_FMT_FLAG_COMPRESSED) != 0U)
			printf(" compressed");
#ifdef V4L2_FMT_FLAG_EMULATED
		if ((format.flags & V4L2_FMT_FLAG_EMULATED) != 0U)
			printf(" emulated");
#endif
		if (format.flags == 0U)
			printf(" none");
		printf("\n");

		enumerate_frame_sizes(fd, format.pixelformat);
	}

	if (!found) {
		fprintf(stderr,
			"ERROR: no single-planar capture format was enumerated.\n");
		return -1;
	}

	return 0;
}

static void query_current_format(int fd)
{
	struct v4l2_format format;
	const struct v4l2_pix_format *pix;
	char fourcc[5];

	memset(&format, 0, sizeof(format));
	format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	printf("\n[4] Current capture format (read-only G_FMT)\n");
	if (xioctl(fd, VIDIOC_G_FMT, &format) == -1) {
		printf("  unavailable: %s\n", strerror(errno));
		return;
	}

	pix = &format.fmt.pix;
	printf("  buffer type  : %u (%s)\n", (unsigned int)format.type,
	       buffer_type_name(format.type));
	printf("  size         : %ux%u\n", pix->width, pix->height);
	printf("  pixel format : '%s' (0x%08x)\n",
	       fourcc_to_string(pix->pixelformat, fourcc), pix->pixelformat);
	printf("  field        : %u (%s)\n", pix->field, field_name(pix->field));
	printf("  bytesperline : %u\n", pix->bytesperline);
	printf("  sizeimage    : %u\n", pix->sizeimage);
	printf("  colorspace   : %u (%s)\n", pix->colorspace,
	       colorspace_name(pix->colorspace));

	/*
	 * NXP's 4.1.15 mx6s-csi probe zero-initializes csi_dev->pix and does not
	 * install a default format.  Therefore 0x0 here can simply mean that no
	 * application has called S_FMT since boot.  R2 will set 640x480 YUYV and
	 * verify the negotiated result.
	 */
	if (pix->width == 0U || pix->height == 0U || pix->pixelformat == 0U)
		printf("  NOTE         : format is not initialized yet; this is allowed in R1\n");
}

static void query_stream_parameters(int fd)
{
	struct v4l2_streamparm parm;
	const struct v4l2_captureparm *capture;

	memset(&parm, 0, sizeof(parm));
	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	printf("\n[5] Current stream parameters (read-only G_PARM)\n");
	if (xioctl(fd, VIDIOC_G_PARM, &parm) == -1) {
		printf("  unavailable: %s\n", strerror(errno));
		return;
	}

	capture = &parm.parm.capture;
	printf("  capability   : 0x%08x", capture->capability);
	if ((capture->capability & V4L2_CAP_TIMEPERFRAME) != 0U)
		printf(" V4L2_CAP_TIMEPERFRAME");
	printf("\n");
	printf("  capture mode : %u\n", capture->capturemode);
	printf("  timeperframe : ");
	print_fraction(&capture->timeperframe);
	printf("\n");
	printf("  read buffers : %u\n", capture->readbuffers);
}

static void usage(const char *program)
{
	printf("Usage: %s [video-device]\n", program);
	printf("Default device: %s\n", DEFAULT_VIDEO_DEVICE);
	printf("VIDEO-R1 only queries the device; it does not capture or change format.\n");
}

int main(int argc, char *argv[])
{
	const char *device = DEFAULT_VIDEO_DEVICE;
	struct stat st;
	uint32_t active_caps = 0;
	int fd;
	int ret = EXIT_FAILURE;

	if (argc > 2) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (argc == 2) {
		if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
			usage(argv[0]);
			return EXIT_SUCCESS;
		}
		device = argv[1];
	}

	if (stat(device, &st) == -1) {
		fprintf(stderr, "Cannot stat %s: %s\n", device, strerror(errno));
		return EXIT_FAILURE;
	}
	if (!S_ISCHR(st.st_mode)) {
		fprintf(stderr, "%s is not a character device\n", device);
		return EXIT_FAILURE;
	}

	/* O_RDWR is the conventional V4L2 capture open mode; O_NONBLOCK is ready for R2. */
	fd = open(device, O_RDWR | O_NONBLOCK);
	if (fd == -1) {
		fprintf(stderr, "Cannot open %s: %s\n", device, strerror(errno));
		return EXIT_FAILURE;
	}

	printf("VIDEO-R1 V4L2 diagnostic\n");
	printf("device        : %s\n", device);
	printf("access mode   : O_RDWR | O_NONBLOCK\n");
	printf("operation     : read-only V4L2 queries\n");

	if (query_capability(fd, &active_caps) == -1)
		goto out_close;

	query_inputs(fd);
	if (enumerate_formats(fd) == -1)
		goto out_close;

	query_current_format(fd);
	query_stream_parameters(fd);

	printf("\n[PASS] %s is usable as a V4L2 single-planar capture node.\n", device);
	if ((active_caps & V4L2_CAP_STREAMING) != 0U)
		printf("[NEXT] Streaming capability is present; VIDEO-R2 can use MMAP.\n");
	else
		printf("[NEXT] R2 must use another I/O method or a different node.\n");
	ret = EXIT_SUCCESS;

out_close:
	if (close(fd) == -1) {
		fprintf(stderr, "close(%s) failed: %s\n", device, strerror(errno));
		ret = EXIT_FAILURE;
	}
	return ret;
}
