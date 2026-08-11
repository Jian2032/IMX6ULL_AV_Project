/* SPDX-License-Identifier: MIT */
/*
 * http_mjpeg.c - STREAM-R4 threaded latest-frame MJPEG server
 *
 *   capture pthread -> cacheable YUYV latest-frame pool
 *   encoder pthread -> complete JPEG latest-frame pool
 *   network pthread -> client-owned JPEG copy -> TCP
 *   main thread     -> accept and concurrent control endpoints
 *
 * Only one MJPEG stream client is active in R4, but /health and /status remain
 * responsive while it is connected.  A slow receiver may skip old JPEGs; it
 * cannot hold a V4L2 MMAP buffer or stop the capture/encoder producers.
 */

#define _POSIX_C_SOURCE 200809L

#include "av_mjpeg_pipeline.h"

#include <arpa/inet.h>
#include <errno.h>
#include <linux/videodev2.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define AV_HTTP_VERSION          "STREAM-R4.1"
#define AV_HTTP_DEFAULT_PORT     8080U
#define AV_HTTP_LISTEN_BACKLOG   4
#define AV_HTTP_REQUEST_SIZE     2048U
#define AV_HTTP_IO_TIMEOUT_SEC   3
#define AV_ACCEPT_POLL_MS        500U
#define AV_STREAM_BOUNDARY       "imx6ullframe"
#define AV_HTTP_HANDOFF          2

#define AV_VIDEO_WIDTH           640U
#define AV_VIDEO_HEIGHT          480U
#define AV_VIDEO_CAPTURE_MODE    0U
#define AV_VIDEO_BUFFER_COUNT    4U
#define AV_DEFAULT_FPS           30U
#define AV_DEFAULT_JPEG_QUALITY  80U

static volatile sig_atomic_t g_stop_requested;

struct av_http_stats {
	uint64_t accepted;
	uint64_t completed;
	uint64_t bad_requests;
	uint64_t rejected_streams;
	uint64_t send_errors;
	uint64_t bytes_sent;
};

struct av_stream_stats {
	uint64_t sessions;
	uint64_t disconnects;
	uint64_t frames;
	uint64_t client_frames_skipped;
	uint64_t send_us;
};

struct av_http_request {
	char method[16];
	char path[256];
	char version[16];
};

struct av_server;

struct av_network_worker {
	struct av_server *server;
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t client_ready;
	int lock_initialized;
	int cond_initialized;
	int thread_started;
	int stop_requested;
	int client_fd;
	char peer[64];
	unsigned char *jpeg_copy;
	size_t jpeg_capacity;
};

struct av_server {
	struct av_mjpeg_pipeline *pipeline;
	struct av_mjpeg_info info;
	struct av_network_worker network;
	pthread_mutex_t stats_lock;
	int stats_lock_initialized;
	struct av_http_stats http;
	struct av_stream_stats stream;
	const char *video_device;
	unsigned int fps;
	unsigned int quality;
	uint64_t started_ms;
};

static void av_signal_handler(int signal_number)
{
	(void)signal_number;
	g_stop_requested = 1;
}

static int av_install_signal_handlers(void)
{
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_handler = av_signal_handler;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGINT, &action, NULL) < 0)
		return -errno;
	if (sigaction(SIGTERM, &action, NULL) < 0)
		return -errno;
	return 0;
}

static uint64_t av_now_us(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return 0;
	return (uint64_t)now.tv_sec * UINT64_C(1000000) +
	       (uint64_t)now.tv_nsec / UINT64_C(1000);
}

static uint64_t av_now_ms(void)
{
	return av_now_us() / UINT64_C(1000);
}

static int av_parse_u32(const char *text, unsigned int minimum,
			unsigned int maximum, unsigned int *value)
{
	char *end = NULL;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0' ||
	    parsed < minimum || parsed > maximum)
		return -EINVAL;
	*value = (unsigned int)parsed;
	return 0;
}

static void av_stats_add_bytes(struct av_server *server, uint64_t bytes)
{
	pthread_mutex_lock(&server->stats_lock);
	server->http.bytes_sent += bytes;
	pthread_mutex_unlock(&server->stats_lock);
}

static void av_stats_snapshot(struct av_server *server,
			      struct av_http_stats *http,
			      struct av_stream_stats *stream)
{
	pthread_mutex_lock(&server->stats_lock);
	*http = server->http;
	*stream = server->stream;
	pthread_mutex_unlock(&server->stats_lock);
}

/* Preserve partial-send accounting without exposing statistics races. */
static int av_send_all(int fd, const void *data, size_t length,
		       struct av_server *server)
{
	const unsigned char *cursor = data;
	size_t remaining = length;
	uint64_t transferred = 0;
	int result = 0;

	while (remaining > 0) {
		ssize_t written = send(fd, cursor, remaining, MSG_NOSIGNAL);

		if (written > 0) {
			cursor += (size_t)written;
			remaining -= (size_t)written;
			transferred += (uint64_t)written;
			continue;
		}
		if (written < 0 && errno == EINTR) {
			if (g_stop_requested) {
				result = -EINTR;
				break;
			}
			continue;
		}
		result = written == 0 ? -EPIPE : -errno;
		break;
	}
	av_stats_add_bytes(server, transferred);
	return result;
}

static int av_client_gone(int result)
{
	return result == -EPIPE || result == -ECONNRESET ||
	       result == -ETIMEDOUT || result == -EAGAIN;
}

static int av_set_client_timeouts(int fd)
{
	struct timeval timeout;

	timeout.tv_sec = AV_HTTP_IO_TIMEOUT_SEC;
	timeout.tv_usec = 0;
	if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
		       &timeout, sizeof(timeout)) < 0)
		return -errno;
	if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
		       &timeout, sizeof(timeout)) < 0)
		return -errno;
	return 0;
}

static int av_receive_request(int fd, char *buffer, size_t capacity,
			      size_t *request_length)
{
	size_t used = 0;

	if (capacity < 5U)
		return -EINVAL;
	while (used < capacity - 1U) {
		ssize_t received = recv(fd, buffer + used,
					capacity - 1U - used, 0);

		if (received > 0) {
			used += (size_t)received;
			buffer[used] = '\0';
			if (strstr(buffer, "\r\n\r\n")) {
				*request_length = used;
				return 0;
			}
			continue;
		}
		if (received == 0)
			return -ECONNRESET;
		if (errno == EINTR) {
			if (g_stop_requested)
				return -EINTR;
			continue;
		}
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return -ETIMEDOUT;
		return -errno;
	}
	return -EMSGSIZE;
}

static int av_parse_request_line(char *buffer, struct av_http_request *request)
{
	char *line_end = strstr(buffer, "\r\n");
	char *query;
	int fields;

	if (!line_end)
		return -EINVAL;
	*line_end = '\0';
	memset(request, 0, sizeof(*request));
	fields = sscanf(buffer, "%15s %255s %15s",
			request->method, request->path, request->version);
	if (fields != 3)
		return -EINVAL;
	if (strncmp(request->version, "HTTP/1.", 7) != 0)
		return -EPROTONOSUPPORT;
	query = strchr(request->path, '?');
	if (query)
		*query = '\0';
	return 0;
}

static const char *av_reason_phrase(unsigned int status)
{
	switch (status) {
	case 200: return "OK";
	case 400: return "Bad Request";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 503: return "Service Unavailable";
	default: return "Error";
	}
}

static int av_send_response(int fd, unsigned int status,
			    const char *content_type, const char *extra_headers,
			    const void *body, size_t body_length,
			    struct av_server *server)
{
	char header[768];
	int length;
	int result;

	length = snprintf(header, sizeof(header),
		"HTTP/1.1 %u %s\r\n"
		"Server: imx6ull-av/%s\r\n"
		"Connection: close\r\n"
		"Cache-Control: no-store, no-cache, must-revalidate\r\n"
		"Pragma: no-cache\r\n"
		"Content-Type: %s\r\n"
		"Content-Length: %lu\r\n"
		"%s"
		"\r\n",
		status, av_reason_phrase(status), AV_HTTP_VERSION,
		content_type, (unsigned long)body_length,
		extra_headers ? extra_headers : "");
	if (length < 0 || (size_t)length >= sizeof(header))
		return -EOVERFLOW;
	result = av_send_all(fd, header, (size_t)length, server);
	if (result < 0 || body_length == 0)
		return result;
	return av_send_all(fd, body, body_length, server);
}

static int av_send_text(int fd, unsigned int status, const char *content_type,
			const char *extra_headers, const char *text,
			struct av_server *server)
{
	return av_send_response(fd, status, content_type, extra_headers,
				text, strlen(text), server);
}

static int av_send_mjpeg_header(int fd, struct av_server *server)
{
	static const char header[] =
		"HTTP/1.1 200 OK\r\n"
		"Server: imx6ull-av/" AV_HTTP_VERSION "\r\n"
		"Connection: close\r\n"
		"Cache-Control: no-store, no-cache, must-revalidate\r\n"
		"Pragma: no-cache\r\n"
		"Content-Type: multipart/x-mixed-replace; boundary="
		AV_STREAM_BOUNDARY "\r\n"
		"\r\n";

	return av_send_all(fd, header, sizeof(header) - 1U, server);
}

static int av_send_mjpeg_frame(int fd, struct av_server *server,
			       const unsigned char *jpeg,
			       const struct av_mjpeg_frame *frame,
			       uint64_t *elapsed_us)
{
	char part_header[256];
	uint64_t begin = av_now_us();
	uint64_t end;
	int length;
	int result;

	length = snprintf(part_header, sizeof(part_header),
		"--" AV_STREAM_BOUNDARY "\r\n"
		"Content-Type: image/jpeg\r\n"
		"Content-Length: %lu\r\n"
		"X-Sequence: %u\r\n"
		"X-Timestamp-Us: %llu\r\n"
		"\r\n",
		(unsigned long)frame->size, frame->sequence,
		(unsigned long long)frame->timestamp_us);
	if (length < 0 || (size_t)length >= sizeof(part_header))
		return -EOVERFLOW;
	result = av_send_all(fd, part_header, (size_t)length, server);
	if (result == 0)
		result = av_send_all(fd, jpeg, frame->size, server);
	if (result == 0)
		result = av_send_all(fd, "\r\n", 2U, server);
	end = av_now_us();
	*elapsed_us = end >= begin ? end - begin : 0;
	return result;
}

static int av_network_should_stop(struct av_network_worker *worker)
{
	int stop;

	pthread_mutex_lock(&worker->lock);
	stop = worker->stop_requested;
	pthread_mutex_unlock(&worker->lock);
	return stop;
}

static void av_record_stream_start(struct av_server *server)
{
	pthread_mutex_lock(&server->stats_lock);
	server->stream.sessions++;
	pthread_mutex_unlock(&server->stats_lock);
}

static void av_record_stream_frame(struct av_server *server,
				   uint64_t skipped, uint64_t send_us)
{
	pthread_mutex_lock(&server->stats_lock);
	server->stream.frames++;
	server->stream.client_frames_skipped += skipped;
	server->stream.send_us += send_us;
	pthread_mutex_unlock(&server->stats_lock);
}

static void av_record_stream_end(struct av_server *server, int disconnected,
				 int unexpected_error)
{
	pthread_mutex_lock(&server->stats_lock);
	if (disconnected)
		server->stream.disconnects++;
	if (unexpected_error)
		server->http.send_errors++;
	server->http.completed++;
	pthread_mutex_unlock(&server->stats_lock);
}

static void av_serve_stream(struct av_network_worker *worker,
			    int fd, const char *peer)
{
	struct av_server *server = worker->server;
	uint64_t last_serial = 0;
	uint64_t local_frames = 0;
	uint64_t local_skipped = 0;
	uint64_t local_send_us = 0;
	int have_last = 0;
	int disconnected = 0;
	int unexpected_error = 0;
	int result;

	av_record_stream_start(server);
	result = av_send_mjpeg_header(fd, server);
	if (result < 0) {
		disconnected = av_client_gone(result);
		unexpected_error = !disconnected && result != -EINTR;
		av_record_stream_end(server, disconnected, unexpected_error);
		return;
	}

	printf("stream state    : START peer=%s boundary=%s\n",
	       peer, AV_STREAM_BOUNDARY);
	while (!g_stop_requested && !av_network_should_stop(worker)) {
		struct av_mjpeg_frame frame;
		uint64_t skipped = 0;
		uint64_t send_us = 0;

		result = av_mjpeg_pipeline_copy_latest(server->pipeline,
				last_serial, worker->jpeg_copy,
				worker->jpeg_capacity, &frame);
		if (result == AV_MJPEG_STOPPED)
			break;
		if (result < 0) {
			unexpected_error = 1;
			fprintf(stderr, "JPEG latest-frame read failed: %s\n",
				strerror(-result));
			break;
		}
		if (have_last && frame.serial > last_serial + 1U)
			skipped = frame.serial - last_serial - 1U;
		last_serial = frame.serial;
		have_last = 1;

		result = av_send_mjpeg_frame(fd, server, worker->jpeg_copy,
					      &frame, &send_us);
		if (result < 0) {
			if (av_client_gone(result) || result == -EINTR) {
				disconnected = 1;
				printf("stream state    : CLIENT_CLOSED peer=%s "
				       "reason=%s\n", peer, strerror(-result));
			} else {
				unexpected_error = 1;
				fprintf(stderr, "stream send to %s failed: %s\n",
					peer, strerror(-result));
			}
			break;
		}

		local_frames++;
		local_skipped += skipped;
		local_send_us += send_us;
		av_record_stream_frame(server, skipped, send_us);
		if (local_frames == 1U || local_frames % 30U == 0U) {
			printf("  stream frame  : count=%llu seq=%u serial=%llu "
			       "jpeg=%lu skipped=%llu send=%llu us\n",
			       (unsigned long long)local_frames, frame.sequence,
			       (unsigned long long)frame.serial,
			       (unsigned long)frame.size,
			       (unsigned long long)skipped,
			       (unsigned long long)send_us);
			fflush(stdout);
		}
	}

	if (g_stop_requested || av_network_should_stop(worker)) {
		static const char closing[] =
			"--" AV_STREAM_BOUNDARY "--\r\n";

		(void)av_send_all(fd, closing, sizeof(closing) - 1U, server);
	}
	printf("stream summary  : peer=%s frames=%llu skipped=%llu",
	       peer, (unsigned long long)local_frames,
	       (unsigned long long)local_skipped);
	if (local_frames > 0)
		printf(" send=%.3f ms",
		       (double)local_send_us / (double)local_frames / 1000.0);
	printf("\n");
	av_record_stream_end(server, disconnected, unexpected_error);
}

static void *av_network_thread_main(void *argument)
{
	struct av_network_worker *worker = argument;

	for (;;) {
		char peer[sizeof(worker->peer)];
		int fd;

		pthread_mutex_lock(&worker->lock);
		while (worker->client_fd < 0 && !worker->stop_requested)
			pthread_cond_wait(&worker->client_ready, &worker->lock);
		if (worker->stop_requested && worker->client_fd < 0) {
			pthread_mutex_unlock(&worker->lock);
			break;
		}
		fd = worker->client_fd;
		snprintf(peer, sizeof(peer), "%s", worker->peer);
		pthread_mutex_unlock(&worker->lock);

		av_serve_stream(worker, fd, peer);
		(void)shutdown(fd, SHUT_RDWR);
		(void)close(fd);

		pthread_mutex_lock(&worker->lock);
		worker->client_fd = -1;
		worker->peer[0] = '\0';
		pthread_cond_broadcast(&worker->client_ready);
		pthread_mutex_unlock(&worker->lock);
	}
	return NULL;
}

static int av_network_init(struct av_network_worker *worker,
			   struct av_server *server, size_t jpeg_capacity)
{
	int error;

	memset(worker, 0, sizeof(*worker));
	worker->server = server;
	worker->client_fd = -1;
	worker->jpeg_capacity = jpeg_capacity;
	worker->jpeg_copy = malloc(jpeg_capacity);
	if (!worker->jpeg_copy)
		return -ENOMEM;
	error = pthread_mutex_init(&worker->lock, NULL);
	if (error) {
		free(worker->jpeg_copy);
		worker->jpeg_copy = NULL;
		return -error;
	}
	worker->lock_initialized = 1;
	error = pthread_cond_init(&worker->client_ready, NULL);
	if (error) {
		(void)pthread_mutex_destroy(&worker->lock);
		worker->lock_initialized = 0;
		free(worker->jpeg_copy);
		worker->jpeg_copy = NULL;
		return -error;
	}
	worker->cond_initialized = 1;
	error = pthread_create(&worker->thread, NULL,
			       av_network_thread_main, worker);
	if (error) {
		(void)pthread_cond_destroy(&worker->client_ready);
		(void)pthread_mutex_destroy(&worker->lock);
		worker->cond_initialized = 0;
		worker->lock_initialized = 0;
		free(worker->jpeg_copy);
		worker->jpeg_copy = NULL;
		return -error;
	}
	worker->thread_started = 1;
	return 0;
}

/* On success the worker owns fd; on failure the caller must send/close it. */
static int av_network_assign(struct av_network_worker *worker,
			     int fd, const char *peer)
{
	int result = 0;

	pthread_mutex_lock(&worker->lock);
	if (worker->stop_requested)
		result = -ECANCELED;
	else if (worker->client_fd >= 0)
		result = -EBUSY;
	else {
		worker->client_fd = fd;
		snprintf(worker->peer, sizeof(worker->peer), "%s", peer);
		pthread_cond_signal(&worker->client_ready);
	}
	pthread_mutex_unlock(&worker->lock);
	return result;
}

static int av_network_active(struct av_network_worker *worker)
{
	int active;

	pthread_mutex_lock(&worker->lock);
	active = worker->client_fd >= 0;
	pthread_mutex_unlock(&worker->lock);
	return active;
}

static void av_network_request_stop(struct av_network_worker *worker)
{
	if (!worker->lock_initialized)
		return;
	pthread_mutex_lock(&worker->lock);
	worker->stop_requested = 1;
	if (worker->client_fd >= 0)
		(void)shutdown(worker->client_fd, SHUT_RDWR);
	if (worker->cond_initialized)
		pthread_cond_broadcast(&worker->client_ready);
	pthread_mutex_unlock(&worker->lock);
}

static void av_network_destroy(struct av_network_worker *worker)
{
	av_network_request_stop(worker);
	if (worker->thread_started) {
		(void)pthread_join(worker->thread, NULL);
		worker->thread_started = 0;
	}
	if (worker->cond_initialized)
		(void)pthread_cond_destroy(&worker->client_ready);
	if (worker->lock_initialized)
		(void)pthread_mutex_destroy(&worker->lock);
	free(worker->jpeg_copy);
	worker->jpeg_copy = NULL;
}

static int av_send_status(int fd, struct av_server *server)
{
	char json[2048];
	struct av_http_stats http;
	struct av_stream_stats stream;
	struct av_mjpeg_pipeline_stats pipeline;
	uint64_t uptime = av_now_ms() - server->started_ms;
	double capture_fps = 0.0;
	double encode_fps = 0.0;
	double average_jpeg = 0.0;
	double average_copy_ms = 0.0;
	double average_unpack_ms = 0.0;
	double average_encode_ms = 0.0;
	double average_publish_ms = 0.0;
	double average_send_ms = 0.0;
	int active = av_network_active(&server->network);
	int length;

	av_stats_snapshot(server, &http, &stream);
	av_mjpeg_pipeline_get_stats(server->pipeline, &pipeline);
	if (pipeline.captured_frames > 1U &&
	    pipeline.last_capture_timestamp_us >
	    pipeline.first_capture_timestamp_us) {
		capture_fps = (double)(pipeline.captured_frames - 1U) * 1000000.0 /
			(double)(pipeline.last_capture_timestamp_us -
				 pipeline.first_capture_timestamp_us);
	}
	if (pipeline.encoded_frames > 1U &&
	    pipeline.last_encode_time_us > pipeline.first_encode_time_us) {
		encode_fps = (double)(pipeline.encoded_frames - 1U) * 1000000.0 /
			(double)(pipeline.last_encode_time_us -
				 pipeline.first_encode_time_us);
	}
	if (pipeline.captured_frames > 0U)
		average_copy_ms = (double)pipeline.copy_us /
			(double)pipeline.captured_frames / 1000.0;
	if (pipeline.encoded_frames > 0U) {
		average_jpeg = (double)pipeline.jpeg_bytes /
			(double)pipeline.encoded_frames;
		average_unpack_ms = (double)pipeline.unpack_us /
			(double)pipeline.encoded_frames / 1000.0;
		average_encode_ms = (double)pipeline.encode_us /
			(double)pipeline.encoded_frames / 1000.0;
		average_publish_ms = (double)pipeline.jpeg_publish_us /
			(double)pipeline.encoded_frames / 1000.0;
	}
	if (stream.frames > 0U)
		average_send_ms = (double)stream.send_us /
			(double)stream.frames / 1000.0;

	length = snprintf(json, sizeof(json),
		"{\n"
		"  \"version\": \"%s\",\n"
		"  \"http\": \"ready\",\n"
		"  \"camera\": \"%s\",\n"
		"  \"jpeg\": \"quality-%u-422\",\n"
		"  \"video\": \"%ux%u-yuyv-%u-fps\",\n"
		"  \"orientation\": \"rotate-180\",\n"
		"  \"stream_client\": \"%s\",\n"
		"  \"queue_policy\": \"latest-frame-drop-oldest\",\n"
		"  \"uptime_ms\": %llu,\n"
		"  \"accepted\": %llu,\n"
		"  \"completed\": %llu,\n"
		"  \"bad_requests\": %llu,\n"
		"  \"rejected_streams\": %llu,\n"
		"  \"stream_sessions\": %llu,\n"
		"  \"stream_frames\": %llu,\n"
		"  \"client_frames_skipped\": %llu,\n"
		"  \"captured_frames\": %llu,\n"
		"  \"encoded_frames\": %llu,\n"
		"  \"driver_sequence_gaps\": %llu,\n"
		"  \"capture_timeouts\": %llu,\n"
		"  \"raw_frames_dropped\": %llu,\n"
		"  \"jpeg_frames_replaced\": %llu,\n"
		"  \"capture_fps\": %.3f,\n"
		"  \"encode_fps\": %.3f,\n"
		"  \"average_jpeg_bytes\": %.2f,\n"
		"  \"average_copy_ms\": %.3f,\n"
		"  \"average_unpack_ms\": %.3f,\n"
		"  \"average_encode_ms\": %.3f,\n"
		"  \"average_jpeg_publish_ms\": %.3f,\n"
		"  \"average_send_ms\": %.3f,\n"
		"  \"send_errors\": %llu\n"
		"}\n",
		AV_HTTP_VERSION, pipeline.failed ? "failed" : "streaming",
		server->quality, server->info.width, server->info.height,
		server->fps, active ? "active" : "idle",
		(unsigned long long)uptime,
		(unsigned long long)http.accepted,
		(unsigned long long)http.completed,
		(unsigned long long)http.bad_requests,
		(unsigned long long)http.rejected_streams,
		(unsigned long long)stream.sessions,
		(unsigned long long)stream.frames,
		(unsigned long long)stream.client_frames_skipped,
		(unsigned long long)pipeline.captured_frames,
		(unsigned long long)pipeline.encoded_frames,
		(unsigned long long)pipeline.driver_sequence_gaps,
		(unsigned long long)pipeline.capture_timeouts,
		(unsigned long long)pipeline.raw_frames_dropped,
		(unsigned long long)pipeline.jpeg_frames_replaced,
		capture_fps, encode_fps, average_jpeg, average_copy_ms,
		average_unpack_ms, average_encode_ms, average_publish_ms,
		average_send_ms, (unsigned long long)http.send_errors);
	if (length < 0 || (size_t)length >= sizeof(json))
		return -EOVERFLOW;
	return av_send_response(fd, 200, "application/json; charset=utf-8",
				NULL, json, (size_t)length, server);
}

static int av_route_request(int fd, const char *peer,
			    const struct av_http_request *request,
			    struct av_server *server)
{
	static const char index_page[] =
		"<!doctype html>\n"
		"<html><head><meta charset=\"utf-8\">"
		"<title>i.MX6ULL MJPEG</title></head>\n"
		"<body style=\"background:#111;color:#eee;font-family:sans-serif\">"
		"<h1>i.MX6ULL live camera</h1>\n"
		"<p>STREAM-R4.1: threaded capture, JPEG and network stages.</p>\n"
		"<img src=\"/stream.mjpg\" width=\"640\" height=\"480\" "
		"style=\"border:1px solid #555\" alt=\"live camera\">\n"
		"<p><a href=\"/status\" style=\"color:#8cf\">JSON status</a></p>\n"
		"</body></html>\n";
	static const char method_error[] =
		"{\"error\":\"only GET is supported in STREAM-R4.1\"}\n";
	static const char busy_error[] =
		"{\"error\":\"one MJPEG stream client is already active\"}\n";
	static const char not_found[] =
		"{\"error\":\"resource not found\"}\n";
	int result;

	if (strcmp(request->method, "GET") != 0)
		return av_send_text(fd, 405, "application/json; charset=utf-8",
				    "Allow: GET\r\n", method_error, server);
	if (strcmp(request->path, "/") == 0 ||
	    strcmp(request->path, "/index.html") == 0)
		return av_send_text(fd, 200, "text/html; charset=utf-8",
				    NULL, index_page, server);
	if (strcmp(request->path, "/health") == 0)
		return av_send_text(fd, 200, "text/plain; charset=utf-8",
				    NULL, "ok\n", server);
	if (strcmp(request->path, "/status") == 0)
		return av_send_status(fd, server);
	if (strcmp(request->path, "/stream.mjpg") == 0) {
		result = av_network_assign(&server->network, fd, peer);
		if (result == 0)
			return AV_HTTP_HANDOFF;
		pthread_mutex_lock(&server->stats_lock);
		server->http.rejected_streams++;
		pthread_mutex_unlock(&server->stats_lock);
		return av_send_text(fd, 503, "application/json; charset=utf-8",
				    "Retry-After: 1\r\n", busy_error, server);
	}
	return av_send_text(fd, 404, "application/json; charset=utf-8",
				NULL, not_found, server);
}

static int av_handle_client(int fd, const char *peer, struct av_server *server)
{
	char buffer[AV_HTTP_REQUEST_SIZE];
	struct av_http_request request;
	size_t request_length = 0;
	int result;

	result = av_set_client_timeouts(fd);
	if (result < 0)
		return result;
	result = av_receive_request(fd, buffer, sizeof(buffer), &request_length);
	if (result < 0) {
		if (result != -EINTR) {
			pthread_mutex_lock(&server->stats_lock);
			server->http.bad_requests++;
			pthread_mutex_unlock(&server->stats_lock);
		}
		return result;
	}
	result = av_parse_request_line(buffer, &request);
	if (result < 0) {
		static const char bad_request[] =
			"{\"error\":\"malformed HTTP request\"}\n";

		pthread_mutex_lock(&server->stats_lock);
		server->http.bad_requests++;
		pthread_mutex_unlock(&server->stats_lock);
		return av_send_text(fd, 400, "application/json; charset=utf-8",
				    NULL, bad_request, server);
	}

	printf("request         : peer=%s method=%s path=%s bytes=%lu\n",
	       peer, request.method, request.path,
	       (unsigned long)request_length);
	result = av_route_request(fd, peer, &request, server);
	if (result != AV_HTTP_HANDOFF) {
		pthread_mutex_lock(&server->stats_lock);
		if (result < 0 && result != -EINTR && !av_client_gone(result))
			server->http.send_errors++;
		else if (result == 0)
			server->http.completed++;
		pthread_mutex_unlock(&server->stats_lock);
	}
	return result;
}

static int av_create_listener(unsigned int port)
{
	struct sockaddr_in address;
	int listener;
	int reuse = 1;

	listener = socket(AF_INET, SOCK_STREAM, 0);
	if (listener < 0)
		return -errno;
	if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
		       &reuse, sizeof(reuse)) < 0)
		goto fail;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons((uint16_t)port);
	if (bind(listener, (struct sockaddr *)&address, sizeof(address)) < 0)
		goto fail;
	if (listen(listener, AV_HTTP_LISTEN_BACKLOG) < 0)
		goto fail;
	return listener;

fail:
	{
		int saved_errno = errno;

		(void)close(listener);
		return -saved_errno;
	}
}

static int av_wait_listener(int listener)
{
	fd_set read_set;
	struct timeval timeout;
	int result;

	FD_ZERO(&read_set);
	FD_SET(listener, &read_set);
	timeout.tv_sec = AV_ACCEPT_POLL_MS / 1000U;
	timeout.tv_usec = (AV_ACCEPT_POLL_MS % 1000U) * 1000U;
	result = select(listener + 1, &read_set, NULL, NULL, &timeout);
	if (result < 0)
		return -errno;
	return result;
}

static void av_print_usage(const char *program)
{
	printf("Usage: %s [port] [max-requests] [video-device] [fps] [quality]\n",
	       program);
	printf("Defaults: %u 0 /dev/video0 %u %u\n",
	       AV_HTTP_DEFAULT_PORT, AV_DEFAULT_FPS,
	       AV_DEFAULT_JPEG_QUALITY);
	printf("STREAM-R4.1 endpoints: / /health /status /stream.mjpg\n");
	printf("R4 keeps control endpoints responsive during one active stream.\n");
}

int main(int argc, char **argv)
{
	struct av_server server;
	struct av_mjpeg_config config;
	struct av_mjpeg_pipeline_stats final_pipeline_stats;
	struct av_http_stats final_http;
	struct av_stream_stats final_stream;
	unsigned int port = AV_HTTP_DEFAULT_PORT;
	unsigned int max_requests = 0;
	unsigned int accepted_requests = 0;
	double final_capture_fps = 0.0;
	double final_encode_fps = 0.0;
	double final_copy_ms = 0.0;
	double final_unpack_ms = 0.0;
	double final_encode_ms = 0.0;
	double final_publish_ms = 0.0;
	double final_send_ms = 0.0;
	int listener = -1;
	int network_initialized = 0;
	int result;
	int exit_code = 1;

	memset(&server, 0, sizeof(server));
	memset(&final_pipeline_stats, 0, sizeof(final_pipeline_stats));
	server.video_device = "/dev/video0";
	server.fps = AV_DEFAULT_FPS;
	server.quality = AV_DEFAULT_JPEG_QUALITY;

	if (argc > 1 && strcmp(argv[1], "--help") == 0) {
		av_print_usage(argv[0]);
		return 0;
	}
	if (argc > 6) {
		av_print_usage(argv[0]);
		return 2;
	}
	if (argc > 1 && av_parse_u32(argv[1], 1, 65535, &port) < 0) {
		fprintf(stderr, "Invalid TCP port: %s\n", argv[1]);
		return 2;
	}
	if (argc > 2 &&
	    av_parse_u32(argv[2], 0, 1000000, &max_requests) < 0) {
		fprintf(stderr, "Invalid max-requests: %s\n", argv[2]);
		return 2;
	}
	if (argc > 3)
		server.video_device = argv[3];
	if (argc > 4 && av_parse_u32(argv[4], 15, 30, &server.fps) < 0) {
		fprintf(stderr, "FPS must be 15 or 30: %s\n", argv[4]);
		return 2;
	}
	if (server.fps != 15U && server.fps != 30U) {
		fprintf(stderr, "FPS must be exactly 15 or 30\n");
		return 2;
	}
	if (argc > 5 && av_parse_u32(argv[5], 1, 100,
					 &server.quality) < 0) {
		fprintf(stderr, "JPEG quality must be 1..100: %s\n", argv[5]);
		return 2;
	}

	result = av_install_signal_handlers();
	if (result < 0) {
		fprintf(stderr, "Cannot install signal handlers: %s\n",
			strerror(-result));
		return 1;
	}
	result = pthread_mutex_init(&server.stats_lock, NULL);
	if (result) {
		fprintf(stderr, "Cannot initialize statistics mutex: %s\n",
			strerror(result));
		return 1;
	}
	server.stats_lock_initialized = 1;

	listener = av_create_listener(port);
	if (listener < 0) {
		fprintf(stderr, "Cannot listen on 0.0.0.0:%u: %s\n",
			port, strerror(-listener));
		goto cleanup;
	}

	memset(&config, 0, sizeof(config));
	config.video_device = server.video_device;
	config.width = AV_VIDEO_WIDTH;
	config.height = AV_VIDEO_HEIGHT;
	config.fps = server.fps;
	config.capture_mode = AV_VIDEO_CAPTURE_MODE;
	config.video_buffer_count = AV_VIDEO_BUFFER_COUNT;
	config.pixel_format = V4L2_PIX_FMT_YUYV;
	config.jpeg_quality = (int)server.quality;
	result = av_mjpeg_pipeline_create(&server.pipeline, &config);
	if (result < 0) {
		fprintf(stderr, "Cannot create MJPEG pipeline: %s\n",
			strerror(-result));
		goto cleanup;
	}
	av_mjpeg_pipeline_get_info(server.pipeline, &server.info);
	result = av_network_init(&server.network, &server,
				 server.info.jpeg_capacity);
	if (result < 0) {
		fprintf(stderr, "Cannot create network worker: %s\n",
			strerror(-result));
		goto cleanup;
	}
	network_initialized = 1;
	result = av_mjpeg_pipeline_start(server.pipeline);
	if (result < 0) {
		fprintf(stderr, "Cannot start MJPEG pipeline: %s\n",
			strerror(-result));
		goto cleanup;
	}

	server.started_ms = av_now_ms();
	printf("STREAM-R4.1 threaded latest-frame MJPEG server\n");
	printf("listen address  : 0.0.0.0:%u\n", port);
	printf("index URL       : http://192.168.1.50:%u/\n", port);
	printf("stream URL      : http://192.168.1.50:%u/stream.mjpg\n", port);
	printf("request limit   : %u (0 means until signal)\n", max_requests);
	printf("camera          : %s, %ux%u YUYV at %u fps\n",
	       server.video_device, server.info.width,
	       server.info.height, server.fps);
	printf("raw pool        : 3 cacheable latest-frame slots, rotate 180 degrees\n");
	printf("JPEG pool       : 3 complete slots, capacity=%lu bytes each\n",
	       (unsigned long)server.info.jpeg_capacity);
	printf("network policy  : one client, private copy, drop old JPEGs\n");
	printf("server state    : LISTENING\n");
	fflush(stdout);

	while (!g_stop_requested &&
	       !av_mjpeg_pipeline_failed(server.pipeline) &&
	       (max_requests == 0U || accepted_requests < max_requests)) {
		struct sockaddr_in peer_address;
		socklen_t peer_length = sizeof(peer_address);
		char peer_ip[INET_ADDRSTRLEN];
		char peer[64];
		int wait_result;
		int client_fd;

		wait_result = av_wait_listener(listener);
		if (wait_result < 0) {
			if (g_stop_requested)
				break;
			fprintf(stderr, "listener select failed: %s\n",
				strerror(-wait_result));
			continue;
		}
		if (wait_result == 0)
			continue;

		client_fd = accept(listener, (struct sockaddr *)&peer_address,
				   &peer_length);
		if (client_fd < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "accept failed: %s\n", strerror(errno));
			continue;
		}
		accepted_requests++;
		pthread_mutex_lock(&server.stats_lock);
		server.http.accepted++;
		pthread_mutex_unlock(&server.stats_lock);

		if (!inet_ntop(AF_INET, &peer_address.sin_addr,
			       peer_ip, sizeof(peer_ip)))
			strcpy(peer_ip, "unknown");
		snprintf(peer, sizeof(peer), "%s:%u", peer_ip,
			 (unsigned int)ntohs(peer_address.sin_port));

		result = av_handle_client(client_fd, peer, &server);
		if (result != AV_HTTP_HANDOFF) {
			if (result < 0 && result != -EINTR &&
			    !av_client_gone(result))
				fprintf(stderr, "client %s failed: %s\n",
					peer, strerror(-result));
			(void)shutdown(client_fd, SHUT_RDWR);
			(void)close(client_fd);
		}
	}

	exit_code = av_mjpeg_pipeline_failed(server.pipeline) ? 1 : 0;

cleanup:
	if (listener >= 0)
		(void)close(listener);
	if (network_initialized)
		av_network_request_stop(&server.network);
	if (server.pipeline)
		av_mjpeg_pipeline_stop(server.pipeline);
	if (network_initialized)
		av_network_destroy(&server.network);
	if (server.pipeline)
		av_mjpeg_pipeline_get_stats(server.pipeline,
					     &final_pipeline_stats);
	av_stats_snapshot(&server, &final_http, &final_stream);
	if (final_pipeline_stats.captured_frames > 1U &&
	    final_pipeline_stats.last_capture_timestamp_us >
	    final_pipeline_stats.first_capture_timestamp_us)
		final_capture_fps =
			(double)(final_pipeline_stats.captured_frames - 1U) *
			1000000.0 /
			(double)(final_pipeline_stats.last_capture_timestamp_us -
				 final_pipeline_stats.first_capture_timestamp_us);
	if (final_pipeline_stats.encoded_frames > 1U &&
	    final_pipeline_stats.last_encode_time_us >
	    final_pipeline_stats.first_encode_time_us)
		final_encode_fps =
			(double)(final_pipeline_stats.encoded_frames - 1U) *
			1000000.0 /
			(double)(final_pipeline_stats.last_encode_time_us -
				 final_pipeline_stats.first_encode_time_us);
	if (final_pipeline_stats.captured_frames > 0U)
		final_copy_ms = (double)final_pipeline_stats.copy_us /
			(double)final_pipeline_stats.captured_frames / 1000.0;
	if (final_pipeline_stats.encoded_frames > 0U) {
		final_unpack_ms = (double)final_pipeline_stats.unpack_us /
			(double)final_pipeline_stats.encoded_frames / 1000.0;
		final_encode_ms = (double)final_pipeline_stats.encode_us /
			(double)final_pipeline_stats.encoded_frames / 1000.0;
		final_publish_ms =
			(double)final_pipeline_stats.jpeg_publish_us /
			(double)final_pipeline_stats.encoded_frames / 1000.0;
	}
	if (final_stream.frames > 0U)
		final_send_ms = (double)final_stream.send_us /
			(double)final_stream.frames / 1000.0;
	av_mjpeg_pipeline_destroy(server.pipeline);
	server.pipeline = NULL;

	printf("server state    : STOPPED\n");
	printf("HTTP stats      : accepted=%llu completed=%llu bad=%llu "
	       "rejected_streams=%llu send_errors=%llu bytes=%llu\n",
	       (unsigned long long)final_http.accepted,
	       (unsigned long long)final_http.completed,
	       (unsigned long long)final_http.bad_requests,
	       (unsigned long long)final_http.rejected_streams,
	       (unsigned long long)final_http.send_errors,
	       (unsigned long long)final_http.bytes_sent);
	printf("capture stats   : frames=%llu driver_gaps=%llu timeouts=%llu "
	       "raw_dropped=%llu\n",
	       (unsigned long long)final_pipeline_stats.captured_frames,
	       (unsigned long long)final_pipeline_stats.driver_sequence_gaps,
	       (unsigned long long)final_pipeline_stats.capture_timeouts,
	       (unsigned long long)final_pipeline_stats.raw_frames_dropped);
	printf("encode stats    : frames=%llu jpeg_replaced=%llu "
	       "jpeg_bytes=%llu\n",
	       (unsigned long long)final_pipeline_stats.encoded_frames,
	       (unsigned long long)final_pipeline_stats.jpeg_frames_replaced,
	       (unsigned long long)final_pipeline_stats.jpeg_bytes);
	printf("stream stats    : sessions=%llu frames=%llu disconnects=%llu "
	       "client_skipped=%llu\n",
	       (unsigned long long)final_stream.sessions,
	       (unsigned long long)final_stream.frames,
	       (unsigned long long)final_stream.disconnects,
	       (unsigned long long)final_stream.client_frames_skipped);
	printf("thread rates    : capture=%.3f fps encode=%.3f fps\n",
	       final_capture_fps, final_encode_fps);
	printf("stage averages  : copy=%.3f ms unpack=%.3f ms encode=%.3f ms "
	       "publish=%.3f ms send=%.3f ms\n",
	       final_copy_ms, final_unpack_ms, final_encode_ms,
	       final_publish_ms, final_send_ms);
	if (final_pipeline_stats.driver_sequence_gaps != 0U)
		printf("[WARN] Capture driver sequence gaps must be zero for R4 acceptance.\n");
	if (g_stop_requested)
		printf("[STOP] Signal requested a clean shutdown.\n");
	else if (exit_code == 0)
		printf("[PASS] STREAM-R4.1 stopped without a pipeline failure.\n");
	else
		printf("[FAIL] STREAM-R4.1 pipeline error=%d (%s).\n",
		       final_pipeline_stats.error_number,
		       strerror(final_pipeline_stats.error_number ?
			final_pipeline_stats.error_number : EIO));

	if (server.stats_lock_initialized)
		(void)pthread_mutex_destroy(&server.stats_lock);
	return exit_code;
}
