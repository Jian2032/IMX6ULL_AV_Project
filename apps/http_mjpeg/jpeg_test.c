/* SPDX-License-Identifier: MIT */
/*
 * jpeg_test.c - STREAM-R2 single-frame JPEG correctness/performance test
 *
 * This program intentionally reads a frame captured by VIDEO-R2 rather than
 * opening /dev/video0.  It isolates JPEG correctness and speed from V4L2,
 * threads, LCD preview and networking.  STREAM-R3 will connect the proven
 * encoder to the live capture path.
 */

#define _POSIX_C_SOURCE 200809L

#include "av_jpeg_encoder.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define DEFAULT_INPUT "frame_640x480_yuyv.raw"
#define DEFAULT_OUTPUT "frame_640x480_q80.jpg"
#define DEFAULT_WIDTH 640U
#define DEFAULT_HEIGHT 480U
#define DEFAULT_QUALITY 80
#define DEFAULT_ITERATIONS 300U

struct timing_stats {
	uint64_t total_us;
	uint64_t min_us;
	uint64_t max_us;
};

static uint64_t monotonic_us(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	return (uint64_t)now.tv_sec * UINT64_C(1000000) +
	       (uint64_t)now.tv_nsec / UINT64_C(1000);
}

static void timing_add(struct timing_stats *stats, uint64_t elapsed)
{
	stats->total_us += elapsed;
	if (elapsed < stats->min_us)
		stats->min_us = elapsed;
	if (elapsed > stats->max_us)
		stats->max_us = elapsed;
}

static int parse_unsigned(const char *text, unsigned int minimum,
			  unsigned int maximum, unsigned int *value)
{
	char *end = NULL;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0' ||
	    parsed < minimum || parsed > maximum)
		return -1;
	*value = (unsigned int)parsed;
	return 0;
}

static int calculate_yuyv_size(unsigned int width, unsigned int height,
			       size_t *bytes)
{
	if (width == 0 || height == 0 || (width & 1U) != 0)
		return -1;
	if ((size_t)width > SIZE_MAX / (size_t)height)
		return -1;
	if ((size_t)width * (size_t)height > SIZE_MAX / 2U)
		return -1;
	*bytes = (size_t)width * (size_t)height * 2U;
	return 0;
}

static int read_exact_file(const char *path, unsigned char **data,
			   size_t expected)
{
	struct stat status;
	unsigned char *buffer;
	FILE *file;
	size_t got;

	if (stat(path, &status) != 0) {
		fprintf(stderr, "Cannot stat %s: %s\n", path, strerror(errno));
		return -1;
	}
	if (status.st_size < 0 || (uint64_t)status.st_size != (uint64_t)expected) {
		fprintf(stderr, "%s has %jd bytes; expected exactly %zu\n",
			path, (intmax_t)status.st_size, expected);
		return -1;
	}

	buffer = malloc(expected);
	if (!buffer) {
		fprintf(stderr, "Cannot allocate %zu input bytes\n", expected);
		return -1;
	}

	file = fopen(path, "rb");
	if (!file) {
		fprintf(stderr, "Cannot open %s: %s\n", path, strerror(errno));
		free(buffer);
		return -1;
	}
	got = fread(buffer, 1, expected, file);
	if (got != expected || ferror(file)) {
		fprintf(stderr, "Short read from %s: %zu/%zu bytes\n",
			path, got, expected);
		fclose(file);
		free(buffer);
		return -1;
	}
	fclose(file);
	*data = buffer;
	return 0;
}

static int write_exact_file(const char *path, const unsigned char *data,
			    size_t bytes)
{
	FILE *file = fopen(path, "wb");
	size_t written;
	int failed = 0;

	if (!file) {
		fprintf(stderr, "Cannot create %s: %s\n", path, strerror(errno));
		return -1;
	}
	written = fwrite(data, 1, bytes, file);
	if (written != bytes || fflush(file) != 0)
		failed = 1;
	if (fclose(file) != 0)
		failed = 1;
	if (failed) {
		fprintf(stderr, "Cannot write complete JPEG %s\n", path);
		return -1;
	}
	return 0;
}

static uint32_t fnv1a(const unsigned char *data, size_t bytes)
{
	uint32_t hash = UINT32_C(2166136261);
	size_t index;

	for (index = 0; index < bytes; ++index) {
		hash ^= data[index];
		hash *= UINT32_C(16777619);
	}
	return hash;
}

/* Verify JPEG framing and read the dimensions from a Start Of Frame marker. */
static int inspect_jpeg(const unsigned char *jpeg, size_t bytes,
			unsigned int *width, unsigned int *height)
{
	size_t position = 2;

	if (bytes < 4 || jpeg[0] != 0xff || jpeg[1] != 0xd8 ||
	    jpeg[bytes - 2] != 0xff || jpeg[bytes - 1] != 0xd9)
		return -1;

	while (position + 3 < bytes) {
		unsigned int marker;
		unsigned int length;

		while (position < bytes && jpeg[position] != 0xff)
			++position;
		while (position < bytes && jpeg[position] == 0xff)
			++position;
		if (position >= bytes)
			break;
		marker = jpeg[position++];
		if (marker == 0xd9 || marker == 0xda)
			break;
		if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7))
			continue;
		if (position + 1 >= bytes)
			return -1;
		length = ((unsigned int)jpeg[position] << 8) |
			 (unsigned int)jpeg[position + 1];
		if (length < 2 || position + length > bytes)
			return -1;

		/* Baseline/progressive and extended sequential SOF markers. */
		if ((marker >= 0xc0 && marker <= 0xc3) ||
		    (marker >= 0xc5 && marker <= 0xc7) ||
		    (marker >= 0xc9 && marker <= 0xcb) ||
		    (marker >= 0xcd && marker <= 0xcf)) {
			if (length < 8)
				return -1;
			*height = ((unsigned int)jpeg[position + 3] << 8) |
				  (unsigned int)jpeg[position + 4];
			*width = ((unsigned int)jpeg[position + 5] << 8) |
				 (unsigned int)jpeg[position + 6];
			return 0;
		}
		position += length;
	}
	return -1;
}

static void usage(const char *program)
{
	printf("Usage: %s [input.yuyv] [width] [height] [quality] [iterations] [output.jpg]\n",
	       program);
	printf("Defaults: %s %u %u %d %u %s\n",
	       DEFAULT_INPUT, DEFAULT_WIDTH, DEFAULT_HEIGHT,
	       DEFAULT_QUALITY, DEFAULT_ITERATIONS, DEFAULT_OUTPUT);
	printf("Input must be one tightly packed YUYV frame (width * height * 2 bytes).\n");
}

int main(int argc, char **argv)
{
	const char *input_path = DEFAULT_INPUT;
	const char *output_path = DEFAULT_OUTPUT;
	unsigned int width = DEFAULT_WIDTH;
	unsigned int height = DEFAULT_HEIGHT;
	unsigned int quality = DEFAULT_QUALITY;
	unsigned int iterations = DEFAULT_ITERATIONS;
	unsigned char *input = NULL;
	struct av_jpeg_encoder encoder;
	struct timing_stats unpack_stats = { 0, UINT64_MAX, 0 };
	struct timing_stats encode_stats = { 0, UINT64_MAX, 0 };
	size_t input_bytes;
	unsigned int jpeg_width = 0;
	unsigned int jpeg_height = 0;
	unsigned int iteration;
	int result = 1;

	memset(&encoder, 0, sizeof(encoder));
	if (argc > 1 && strcmp(argv[1], "--help") == 0) {
		usage(argv[0]);
		return 0;
	}
	if (argc > 7) {
		usage(argv[0]);
		return 2;
	}
	if (argc > 1)
		input_path = argv[1];
	if (argc > 2 && parse_unsigned(argv[2], 2, 8192, &width) != 0) {
		fprintf(stderr, "Invalid even image width: %s\n", argv[2]);
		return 2;
	}
	if ((width & 1U) != 0) {
		fprintf(stderr, "YUYV image width must be even\n");
		return 2;
	}
	if (argc > 3 && parse_unsigned(argv[3], 1, 8192, &height) != 0) {
		fprintf(stderr, "Invalid image height: %s\n", argv[3]);
		return 2;
	}
	if (argc > 4 && parse_unsigned(argv[4], 1, 100, &quality) != 0) {
		fprintf(stderr, "Invalid JPEG quality: %s\n", argv[4]);
		return 2;
	}
	if (argc > 5 && parse_unsigned(argv[5], 1, 1000000,
					 &iterations) != 0) {
		fprintf(stderr, "Invalid iteration count: %s\n", argv[5]);
		return 2;
	}
	if (argc > 6)
		output_path = argv[6];

	if (calculate_yuyv_size(width, height, &input_bytes) != 0) {
		fprintf(stderr, "Image dimensions overflow the host size type\n");
		return 2;
	}

	printf("STREAM-R2 YUYV to JPEG acceptance test\n");
	printf("input           : %s\n", input_path);
	printf("format          : %ux%u packed YUYV 4:2:2, %zu bytes\n",
	       width, height, input_bytes);
	printf("encoder         : static libjpeg-turbo, planar 4:2:2, FASTDCT\n");
	printf("quality         : %u\n", quality);
	printf("iterations      : %u (one warm-up is excluded)\n", iterations);
	printf("output          : %s\n", output_path);

	if (read_exact_file(input_path, &input, input_bytes) != 0)
		goto out;
	if (av_jpeg_encoder_init(&encoder, width, height, (int)quality) != 0) {
		fprintf(stderr, "Encoder init failed: %s\n",
			av_jpeg_encoder_error(&encoder));
		goto out;
	}

	/* Warm up instruction/data caches before collecting timing evidence. */
	if (av_jpeg_encoder_unpack_yuyv(&encoder, input, input_bytes,
					 width * 2U) != 0 ||
	    av_jpeg_encoder_compress(&encoder) != 0) {
		fprintf(stderr, "Warm-up failed: %s\n",
			av_jpeg_encoder_error(&encoder));
		goto out;
	}

	for (iteration = 0; iteration < iterations; ++iteration) {
		uint64_t begin;
		uint64_t middle;
		uint64_t end;

		begin = monotonic_us();
		if (av_jpeg_encoder_unpack_yuyv(&encoder, input, input_bytes,
						 width * 2U) != 0) {
			fprintf(stderr, "YUYV unpack failed: %s\n",
				av_jpeg_encoder_error(&encoder));
			goto out;
		}
		middle = monotonic_us();
		if (av_jpeg_encoder_compress(&encoder) != 0) {
			fprintf(stderr, "JPEG encode failed: %s\n",
				av_jpeg_encoder_error(&encoder));
			goto out;
		}
		end = monotonic_us();
		if (begin == 0 || middle < begin || end < middle) {
			fprintf(stderr, "CLOCK_MONOTONIC returned an invalid sample\n");
			goto out;
		}
		timing_add(&unpack_stats, middle - begin);
		timing_add(&encode_stats, end - middle);
	}

	if (inspect_jpeg(av_jpeg_encoder_data(&encoder),
			 (size_t)av_jpeg_encoder_size(&encoder),
			 &jpeg_width, &jpeg_height) != 0) {
		fprintf(stderr, "Encoded output is not a structurally valid JPEG\n");
		goto out;
	}
	if (jpeg_width != width || jpeg_height != height) {
		fprintf(stderr, "JPEG SOF reports %ux%u instead of %ux%u\n",
			jpeg_width, jpeg_height, width, height);
		goto out;
	}
	if (write_exact_file(output_path, av_jpeg_encoder_data(&encoder),
			     (size_t)av_jpeg_encoder_size(&encoder)) != 0)
		goto out;

	{
		double unpack_average = (double)unpack_stats.total_us / iterations;
		double encode_average = (double)encode_stats.total_us / iterations;
		double pipeline_average = unpack_average + encode_average;
		unsigned long jpeg_bytes = av_jpeg_encoder_size(&encoder);

		printf("JPEG result     : %ux%u, %lu bytes, SOI/EOI and SOF valid\n",
		       jpeg_width, jpeg_height, jpeg_bytes);
		printf("  FNV-1a        : 0x%08" PRIx32 "\n",
		       fnv1a(av_jpeg_encoder_data(&encoder), (size_t)jpeg_bytes));
		printf("  raw/JPEG ratio: %.2f:1\n", (double)input_bytes / jpeg_bytes);
		printf("timing          : microseconds/frame\n");
		printf("  unpack YUYV   : avg=%.2f min=%" PRIu64 " max=%" PRIu64 "\n",
		       unpack_average, unpack_stats.min_us, unpack_stats.max_us);
		printf("  JPEG encode   : avg=%.2f min=%" PRIu64 " max=%" PRIu64 "\n",
		       encode_average, encode_stats.min_us, encode_stats.max_us);
		printf("  pipeline      : avg=%.2f theoretical=%.2f fps\n",
		       pipeline_average, UINT64_C(1000000) / pipeline_average);
		printf("output file     : %s\n", output_path);
	}

	printf("[PASS] Encoded and validated %u JPEG frames without per-frame allocation.\n",
	       iterations);
	result = 0;

out:
	av_jpeg_encoder_destroy(&encoder);
	free(input);
	return result;
}
