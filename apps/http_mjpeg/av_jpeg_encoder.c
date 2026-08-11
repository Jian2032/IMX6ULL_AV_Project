/* SPDX-License-Identifier: MIT */
/*
 * av_jpeg_encoder.c - packed YUYV 4:2:2 to JPEG using TurboJPEG
 *
 * YUYV stores two pixels in four bytes:
 *
 *     Y0 U0 Y1 V0
 *
 * The two pixels have independent luminance but share chroma.  TurboJPEG can
 * consume planar 4:2:2 directly, avoiding an unnecessary YUYV -> RGB -> YCbCr
 * round trip.  All storage is allocated in init and reused for every frame.
 */

#include "av_jpeg_encoder.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <turbojpeg.h>

static void set_error(struct av_jpeg_encoder *encoder, const char *message)
{
	if (!encoder)
		return;

	snprintf(encoder->error, sizeof(encoder->error), "%s", message);
}

static void release_resources(struct av_jpeg_encoder *encoder)
{
	if (encoder->handle)
		tjDestroy((tjhandle)encoder->handle);
	if (encoder->jpeg_buffer)
		tjFree(encoder->jpeg_buffer);
	free(encoder->plane_storage);

	encoder->handle = NULL;
	encoder->jpeg_buffer = NULL;
	encoder->plane_storage = NULL;
}

static int checked_plane_size(unsigned int width, unsigned int height,
			      size_t *total)
{
	size_t pixels;

	if (width == 0 || height == 0 || (width & 1U) != 0)
		return -1;
	if ((size_t)width > SIZE_MAX / (size_t)height)
		return -1;

	pixels = (size_t)width * (size_t)height;
	/* Y uses one byte/pixel and U/V together use one byte/pixel. */
	if (pixels > SIZE_MAX / 2U)
		return -1;

	*total = pixels * 2U;
	return 0;
}

int av_jpeg_encoder_init(struct av_jpeg_encoder *encoder,
			 unsigned int width, unsigned int height,
			 int quality)
{
	size_t pixels;
	size_t plane_bytes;
	tjhandle handle;
	unsigned long capacity;

	if (!encoder)
		return -EINVAL;

	memset(encoder, 0, sizeof(*encoder));
	if (quality < 1 || quality > 100) {
		set_error(encoder, "JPEG quality must be in the range 1..100");
		return -EINVAL;
	}
	if (width > INT_MAX || height > INT_MAX ||
	    checked_plane_size(width, height, &plane_bytes) != 0) {
		set_error(encoder, "invalid or overflowing YUYV dimensions");
		return -EINVAL;
	}

	pixels = (size_t)width * (size_t)height;
	encoder->plane_storage = malloc(plane_bytes);
	if (!encoder->plane_storage) {
		set_error(encoder, "cannot allocate planar YUV working memory");
		return -ENOMEM;
	}

	encoder->planes[0] = encoder->plane_storage;
	encoder->planes[1] = encoder->planes[0] + pixels;
	encoder->planes[2] = encoder->planes[1] + pixels / 2U;
	encoder->strides[0] = (int)width;
	encoder->strides[1] = (int)(width / 2U);
	encoder->strides[2] = (int)(width / 2U);

	handle = tjInitCompress();
	if (!handle) {
		set_error(encoder, tjGetErrorStr());
		release_resources(encoder);
		return -EIO;
	}
	encoder->handle = handle;

	capacity = tjBufSize((int)width, (int)height, TJSAMP_422);
	if (capacity == (unsigned long)-1) {
		set_error(encoder, tjGetErrorStr2(handle));
		release_resources(encoder);
		return -EIO;
	}
	if (capacity > INT_MAX) {
		set_error(encoder, "worst-case JPEG buffer exceeds tjAlloc limit");
		release_resources(encoder);
		return -EOVERFLOW;
	}

	encoder->jpeg_buffer = tjAlloc((int)capacity);
	if (!encoder->jpeg_buffer) {
		set_error(encoder, "cannot allocate worst-case JPEG output buffer");
		release_resources(encoder);
		return -ENOMEM;
	}

	encoder->jpeg_capacity = capacity;
	encoder->width = width;
	encoder->height = height;
	encoder->quality = quality;
	encoder->error[0] = '\0';
	return 0;
}

int av_jpeg_encoder_unpack_yuyv(struct av_jpeg_encoder *encoder,
				const void *yuyv, size_t bytes,
				unsigned int source_stride)
{
	const unsigned char *source = yuyv;
	unsigned int row;
	size_t required;

	if (!encoder || !encoder->plane_storage || !source)
		return -EINVAL;
	if (source_stride < encoder->width * 2U) {
		set_error(encoder, "YUYV source stride is smaller than width * 2");
		return -EINVAL;
	}
	if ((size_t)source_stride > SIZE_MAX / encoder->height) {
		set_error(encoder, "YUYV source size calculation overflowed");
		return -EOVERFLOW;
	}
	required = (size_t)source_stride * encoder->height;
	if (bytes < required) {
		set_error(encoder, "YUYV input buffer is shorter than stride * height");
		return -EMSGSIZE;
	}

	for (row = 0; row < encoder->height; ++row) {
		const unsigned char *src = source + (size_t)row * source_stride;
		unsigned char *dst_y = encoder->planes[0] +
			(size_t)row * (size_t)encoder->strides[0];
		unsigned char *dst_u = encoder->planes[1] +
			(size_t)row * (size_t)encoder->strides[1];
		unsigned char *dst_v = encoder->planes[2] +
			(size_t)row * (size_t)encoder->strides[2];
		unsigned int column;

		for (column = 0; column < encoder->width; column += 2U) {
			unsigned int chroma = column / 2U;

			dst_y[column] = src[0];
			dst_u[chroma] = src[1];
			dst_y[column + 1U] = src[2];
			dst_v[chroma] = src[3];
			src += 4;
		}
	}

	encoder->error[0] = '\0';
	return 0;
}

int av_jpeg_encoder_unpack_yuyv_rotate_180(
				struct av_jpeg_encoder *encoder,
				const void *yuyv, size_t bytes,
				unsigned int source_stride)
{
	const unsigned char *source = yuyv;
	unsigned int row;
	size_t required;

	if (!encoder || !encoder->plane_storage || !source)
		return -EINVAL;
	if (source_stride < encoder->width * 2U) {
		set_error(encoder, "YUYV source stride is smaller than width * 2");
		return -EINVAL;
	}
	if ((size_t)source_stride > SIZE_MAX / encoder->height) {
		set_error(encoder, "YUYV source size calculation overflowed");
		return -EOVERFLOW;
	}
	required = (size_t)source_stride * encoder->height;
	if (bytes < required) {
		set_error(encoder, "YUYV input buffer is shorter than stride * height");
		return -EMSGSIZE;
	}

	for (row = 0; row < encoder->height; ++row) {
		const unsigned char *source_row = source +
			(size_t)(encoder->height - 1U - row) * source_stride;
		unsigned char *dst_y = encoder->planes[0] +
			(size_t)row * (size_t)encoder->strides[0];
		unsigned char *dst_u = encoder->planes[1] +
			(size_t)row * (size_t)encoder->strides[1];
		unsigned char *dst_v = encoder->planes[2] +
			(size_t)row * (size_t)encoder->strides[2];
		unsigned int column;

		for (column = 0; column < encoder->width; column += 2U) {
			unsigned int chroma = column / 2U;
			unsigned int source_column = encoder->width - 2U - column;
			const unsigned char *src =
				source_row + (size_t)source_column * 2U;

			/* Reverse the pair's pixels but retain their shared U and V. */
			dst_y[column] = src[2];
			dst_u[chroma] = src[1];
			dst_y[column + 1U] = src[0];
			dst_v[chroma] = src[3];
		}
	}

	encoder->error[0] = '\0';
	return 0;
}

int av_jpeg_encoder_compress(struct av_jpeg_encoder *encoder)
{
	const unsigned char *planes[3];
	unsigned char *output;
	unsigned long output_size;
	int result;

	if (!encoder || !encoder->handle || !encoder->jpeg_buffer)
		return -EINVAL;

	planes[0] = encoder->planes[0];
	planes[1] = encoder->planes[1];
	planes[2] = encoder->planes[2];
	output = encoder->jpeg_buffer;
	output_size = encoder->jpeg_capacity;

	result = tjCompressFromYUVPlanes((tjhandle)encoder->handle,
					 planes, (int)encoder->width,
					 encoder->strides, (int)encoder->height,
					 TJSAMP_422, &output, &output_size,
					 encoder->quality,
					 TJFLAG_FASTDCT | TJFLAG_NOREALLOC);
	if (result != 0) {
		set_error(encoder,
			  tjGetErrorStr2((tjhandle)encoder->handle));
		return -EIO;
	}
	if (output != encoder->jpeg_buffer ||
	    output_size > encoder->jpeg_capacity) {
		set_error(encoder, "TurboJPEG violated the preallocated buffer contract");
		return -EIO;
	}

	encoder->jpeg_size = output_size;
	encoder->error[0] = '\0';
	return 0;
}

const unsigned char *av_jpeg_encoder_data(const struct av_jpeg_encoder *encoder)
{
	return encoder ? encoder->jpeg_buffer : NULL;
}

unsigned long av_jpeg_encoder_size(const struct av_jpeg_encoder *encoder)
{
	return encoder ? encoder->jpeg_size : 0;
}

const char *av_jpeg_encoder_error(const struct av_jpeg_encoder *encoder)
{
	if (!encoder)
		return "invalid encoder object";
	return encoder->error[0] ? encoder->error : "no error detail";
}

void av_jpeg_encoder_destroy(struct av_jpeg_encoder *encoder)
{
	if (!encoder)
		return;

	release_resources(encoder);
	memset(encoder, 0, sizeof(*encoder));
}
