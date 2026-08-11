/* SPDX-License-Identifier: MIT */
/*
 * av_jpeg_encoder.h - reusable packed-YUYV to JPEG encoder
 *
 * STREAM-R2 deliberately exposes a small interface.  The HTTP server will
 * reuse this same object in STREAM-R3, so the test program does not own any
 * encoder implementation details.
 */

#ifndef AV_JPEG_ENCODER_H
#define AV_JPEG_ENCODER_H

#include <stddef.h>
#include <stdint.h>

struct av_jpeg_encoder {
	void *handle;
	unsigned char *plane_storage;
	unsigned char *planes[3];
	int strides[3];
	unsigned char *jpeg_buffer;
	unsigned long jpeg_capacity;
	unsigned long jpeg_size;
	unsigned int width;
	unsigned int height;
	int quality;
	char error[160];
};

/* Allocate all working buffers once.  width must be even for packed YUYV. */
int av_jpeg_encoder_init(struct av_jpeg_encoder *encoder,
			 unsigned int width, unsigned int height,
			 int quality);

/*
 * Convert one YUYV frame to planar 4:2:2.  This operation is kept separate so
 * the benchmark can show packing cost independently of JPEG compression.
 */
int av_jpeg_encoder_unpack_yuyv(struct av_jpeg_encoder *encoder,
				const void *yuyv, size_t bytes,
				unsigned int source_stride);

/* Unpack while rotating the packed source by 180 degrees. */
int av_jpeg_encoder_unpack_yuyv_rotate_180(
				struct av_jpeg_encoder *encoder,
				const void *yuyv, size_t bytes,
				unsigned int source_stride);

/* Compress the planes prepared by either YUYV unpack entry point. */
int av_jpeg_encoder_compress(struct av_jpeg_encoder *encoder);

const unsigned char *av_jpeg_encoder_data(const struct av_jpeg_encoder *encoder);
unsigned long av_jpeg_encoder_size(const struct av_jpeg_encoder *encoder);
const char *av_jpeg_encoder_error(const struct av_jpeg_encoder *encoder);
void av_jpeg_encoder_destroy(struct av_jpeg_encoder *encoder);

#endif /* AV_JPEG_ENCODER_H */
