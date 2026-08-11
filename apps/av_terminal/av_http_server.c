/* SPDX-License-Identifier: MIT */
/*
 * av_http_server.c - AV-R3 threaded HTTP/MJPEG consumer
 *
 * Ownership is deliberately split into two socket threads:
 *
 *   accept thread  : accept, parse and finish short control requests
 *   network thread : own one handed-off /stream.mjpg socket until disconnect
 *
 * The network thread copies each complete JPEG into private storage before
 * send().  A slow TCP receiver can therefore skip JPEG serials, but it cannot
 * hold a shared JPEG slot or interfere with V4L2 DQBUF/QBUF circulation.
 */

#define _POSIX_C_SOURCE 200809L

#include "av_http_server.h"
#include "av_mjpeg_pipeline.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
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

#define AV_HTTP_LISTEN_BACKLOG    4
#define AV_HTTP_REQUEST_SIZE      2048U
#define AV_HTTP_STATUS_SIZE       4096U
#define AV_HTTP_IO_TIMEOUT_SEC    3
#define AV_HTTP_ACCEPT_POLL_MS    250U
#define AV_HTTP_JPEG_WAIT_MS      250U
#define AV_HTTP_HANDOFF           2
#define AV_STREAM_BOUNDARY        "imx6ullframe"

struct av_http_request {
	char method[16];
	char path[256];
	char version[16];
};

struct av_http_server;

struct av_network_worker {
	struct av_http_server *server;
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

struct av_http_server {
	struct av_mjpeg_pipeline *pipeline;
	struct av_mjpeg_info video;
	unsigned int port;
	char version[32];
	av_http_status_callback format_status;
	av_http_health_callback health_ok;
	void *callback_opaque;

	int listener;
	pthread_t accept_thread;
	int accept_thread_started;
	struct av_network_worker network;

	pthread_mutex_t state_lock;
	pthread_mutex_t stats_lock;
	int state_lock_initialized;
	int stats_lock_initialized;
	int started;
	int stop_requested;
	int failed;
	int error_number;
	uint64_t started_ms;
	struct av_http_server_stats stats;
};

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

static int av_server_should_stop(struct av_http_server *server)
{
	int stop;

	pthread_mutex_lock(&server->state_lock);
	stop = server->stop_requested;
	pthread_mutex_unlock(&server->state_lock);
	return stop;
}

static void av_server_fail(struct av_http_server *server, int error_number)
{
	pthread_mutex_lock(&server->state_lock);
	if (!server->failed) {
		server->failed = 1;
		server->error_number = error_number ? error_number : EIO;
	}
	pthread_mutex_unlock(&server->state_lock);
}

static void av_stats_add_bytes(struct av_http_server *server, uint64_t bytes)
{
	pthread_mutex_lock(&server->stats_lock);
	server->stats.bytes_sent += bytes;
	pthread_mutex_unlock(&server->stats_lock);
}

/* Keep partial-send accounting even when a peer closes mid-frame. */
static int av_send_all(int fd, const void *data, size_t length,
		       struct av_http_server *server)
{
	const unsigned char *cursor = data;
	size_t remaining = length;
	uint64_t transferred = 0;
	int result = 0;

	while (remaining > 0U) {
		ssize_t written = send(fd, cursor, remaining, MSG_NOSIGNAL);

		if (written > 0) {
			cursor += (size_t)written;
			remaining -= (size_t)written;
			transferred += (uint64_t)written;
			continue;
		}
		if (written < 0 && errno == EINTR) {
			if (av_server_should_stop(server)) {
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
			      size_t *request_length,
			      struct av_http_server *server)
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
			if (av_server_should_stop(server))
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
	case 500: return "Internal Server Error";
	case 503: return "Service Unavailable";
	default: return "Error";
	}
}

static int av_send_response(int fd, unsigned int status,
			    const char *content_type, const char *extra_headers,
			    const void *body, size_t body_length,
			    struct av_http_server *server)
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
		status, av_reason_phrase(status), server->version,
		content_type, (unsigned long)body_length,
		extra_headers ? extra_headers : "");
	if (length < 0 || (size_t)length >= sizeof(header))
		return -EOVERFLOW;
	result = av_send_all(fd, header, (size_t)length, server);
	if (result < 0 || body_length == 0U)
		return result;
	return av_send_all(fd, body, body_length, server);
}

static int av_send_text(int fd, unsigned int status, const char *content_type,
			const char *extra_headers, const char *text,
			struct av_http_server *server)
{
	return av_send_response(fd, status, content_type, extra_headers,
				text, strlen(text), server);
}

static int av_send_mjpeg_header(int fd, struct av_http_server *server)
{
	char header[512];
	int length;

	length = snprintf(header, sizeof(header),
		"HTTP/1.1 200 OK\r\n"
		"Server: imx6ull-av/%s\r\n"
		"Connection: close\r\n"
		"Cache-Control: no-store, no-cache, must-revalidate\r\n"
		"Pragma: no-cache\r\n"
		"Content-Type: multipart/x-mixed-replace; boundary=%s\r\n"
		"\r\n", server->version, AV_STREAM_BOUNDARY);
	if (length < 0 || (size_t)length >= sizeof(header))
		return -EOVERFLOW;
	return av_send_all(fd, header, (size_t)length, server);
}

static int av_send_mjpeg_frame(int fd, struct av_http_server *server,
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
		"X-Capture-Monotonic-Us: %llu\r\n"
		"\r\n",
		(unsigned long)frame->size, frame->sequence,
		(unsigned long long)frame->timestamp_us,
		(unsigned long long)frame->capture_time_us);
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

static int av_network_active(struct av_network_worker *worker)
{
	int active = 0;

	if (!worker->lock_initialized)
		return 0;
	pthread_mutex_lock(&worker->lock);
	active = worker->client_fd >= 0;
	pthread_mutex_unlock(&worker->lock);
	return active;
}

static void av_record_stream_start(struct av_http_server *server)
{
	pthread_mutex_lock(&server->stats_lock);
	server->stats.stream_sessions++;
	pthread_mutex_unlock(&server->stats_lock);
}

static void av_record_stream_frame(struct av_http_server *server,
				   uint64_t skipped, uint64_t send_us)
{
	pthread_mutex_lock(&server->stats_lock);
	server->stats.stream_frames++;
	server->stats.client_frames_skipped += skipped;
	server->stats.stream_send_us += send_us;
	pthread_mutex_unlock(&server->stats_lock);
}

static void av_record_stream_end(struct av_http_server *server,
				 int disconnected, int unexpected_error)
{
	pthread_mutex_lock(&server->stats_lock);
	if (disconnected)
		server->stats.stream_disconnects++;
	if (unexpected_error)
		server->stats.send_errors++;
	server->stats.completed++;
	pthread_mutex_unlock(&server->stats_lock);
}

static void av_serve_stream(struct av_network_worker *worker,
			    int fd, const char *peer)
{
	struct av_http_server *server = worker->server;
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

	printf("HTTP stream     : START peer=%s boundary=%s\n",
	       peer, AV_STREAM_BOUNDARY);
	while (!av_server_should_stop(server) &&
	       !av_network_should_stop(worker)) {
		struct av_mjpeg_frame frame;
		uint64_t skipped = 0;
		uint64_t send_us = 0;

		result = av_mjpeg_pipeline_copy_latest_timeout(
			server->pipeline, last_serial, worker->jpeg_copy,
			worker->jpeg_capacity, &frame, AV_HTTP_JPEG_WAIT_MS);
		if (result == AV_MJPEG_TIMEOUT)
			continue;
		if (result == AV_MJPEG_STOPPED)
			break;
		if (result < 0) {
			unexpected_error = 1;
			fprintf(stderr, "HTTP JPEG read failed: %s\n",
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
				printf("HTTP stream     : CLIENT_CLOSED peer=%s "
				       "reason=%s\n", peer, strerror(-result));
			} else {
				unexpected_error = 1;
				fprintf(stderr, "HTTP send to %s failed: %s\n",
					peer, strerror(-result));
			}
			break;
		}

		local_frames++;
		local_skipped += skipped;
		local_send_us += send_us;
		av_record_stream_frame(server, skipped, send_us);
		if (local_frames == 1U || local_frames % 30U == 0U) {
			printf("  HTTP frame    : count=%llu seq=%u serial=%llu "
			       "jpeg=%lu skipped=%llu send=%llu us\n",
			       (unsigned long long)local_frames, frame.sequence,
			       (unsigned long long)frame.serial,
			       (unsigned long)frame.size,
			       (unsigned long long)skipped,
			       (unsigned long long)send_us);
			fflush(stdout);
		}
	}

	printf("HTTP summary    : peer=%s frames=%llu skipped=%llu",
	       peer, (unsigned long long)local_frames,
	       (unsigned long long)local_skipped);
	if (local_frames > 0U)
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
			   struct av_http_server *server,
			   size_t jpeg_capacity)
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

/* On success the worker owns fd; otherwise the accept thread retains it. */
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
	if (worker->cond_initialized) {
		(void)pthread_cond_destroy(&worker->client_ready);
		worker->cond_initialized = 0;
	}
	if (worker->lock_initialized) {
		(void)pthread_mutex_destroy(&worker->lock);
		worker->lock_initialized = 0;
	}
	free(worker->jpeg_copy);
	worker->jpeg_copy = NULL;
}

void av_http_server_get_stats(struct av_http_server *server,
			      struct av_http_server_stats *stats)
{
	uint64_t started_ms;
	int failed;
	int error_number;

	if (!server || !stats)
		return;
	pthread_mutex_lock(&server->stats_lock);
	*stats = server->stats;
	pthread_mutex_unlock(&server->stats_lock);
	pthread_mutex_lock(&server->state_lock);
	started_ms = server->started_ms;
	failed = server->failed;
	error_number = server->error_number;
	pthread_mutex_unlock(&server->state_lock);
	stats->uptime_ms = started_ms ? av_now_ms() - started_ms : 0;
	stats->stream_client_active = av_network_active(&server->network);
	stats->failed = failed;
	stats->error_number = error_number;
}

static int av_send_status(int fd, struct av_http_server *server)
{
	char json[AV_HTTP_STATUS_SIZE];
	struct av_http_server_stats snapshot;
	int length;

	av_http_server_get_stats(server, &snapshot);
	length = server->format_status(server->callback_opaque, &snapshot,
				       json, sizeof(json));
	if (length < 0 || (size_t)length >= sizeof(json)) {
		static const char error_json[] =
			"{\"error\":\"status snapshot failed\"}\n";

		return av_send_text(fd, 500,
			"application/json; charset=utf-8", NULL,
			error_json, server);
	}
	return av_send_response(fd, 200, "application/json; charset=utf-8",
				NULL, json, (size_t)length, server);
}

static int av_route_request(int fd, const char *peer,
			    const struct av_http_request *request,
			    struct av_http_server *server)
{
	char index_page[768];
	static const char method_error[] =
		"{\"error\":\"only GET is supported\"}\n";
	static const char busy_error[] =
		"{\"error\":\"one MJPEG stream client is already active\"}\n";
	static const char not_found[] =
		"{\"error\":\"resource not found\"}\n";
	int length;
	int result;

	if (strcmp(request->method, "GET") != 0)
		return av_send_text(fd, 405, "application/json; charset=utf-8",
				    "Allow: GET\r\n", method_error, server);
	if (strcmp(request->path, "/") == 0 ||
	    strcmp(request->path, "/index.html") == 0) {
		length = snprintf(index_page, sizeof(index_page),
			"<!doctype html>\n"
			"<html><head><meta charset=\"utf-8\">"
			"<title>i.MX6ULL AV terminal</title></head>\n"
			"<body style=\"background:#111;color:#eee;"
			"font-family:sans-serif\">"
			"<h1>i.MX6ULL AV terminal</h1>\n"
			"<p>%s: one camera feeds LCD and HTTP while ALSA captures.</p>\n"
			"<img src=\"/stream.mjpg\" width=\"%u\" height=\"%u\" "
			"style=\"border:1px solid #555\" alt=\"live camera\">\n"
			"<p><a href=\"/status\" style=\"color:#8cf\">"
			"Integrated JSON status</a></p>\n"
			"</body></html>\n",
			server->version, server->video.width, server->video.height);
		if (length < 0 || (size_t)length >= sizeof(index_page))
			return -EOVERFLOW;
		return av_send_response(fd, 200, "text/html; charset=utf-8",
					NULL, index_page, (size_t)length, server);
	}
	if (strcmp(request->path, "/health") == 0) {
		int healthy = !server->health_ok ||
			server->health_ok(server->callback_opaque);

		return av_send_text(fd, healthy ? 200U : 503U,
			"text/plain; charset=utf-8",
			healthy ? NULL : "Retry-After: 1\r\n",
			healthy ? "ok\n" : "degraded\n", server);
	}
	if (strcmp(request->path, "/status") == 0)
		return av_send_status(fd, server);
	if (strcmp(request->path, "/stream.mjpg") == 0) {
		result = av_network_assign(&server->network, fd, peer);
		if (result == 0)
			return AV_HTTP_HANDOFF;
		pthread_mutex_lock(&server->stats_lock);
		server->stats.rejected_streams++;
		pthread_mutex_unlock(&server->stats_lock);
		return av_send_text(fd, 503, "application/json; charset=utf-8",
				    "Retry-After: 1\r\n", busy_error, server);
	}
	return av_send_text(fd, 404, "application/json; charset=utf-8",
				NULL, not_found, server);
}

static int av_handle_client(int fd, const char *peer,
			    struct av_http_server *server)
{
	char buffer[AV_HTTP_REQUEST_SIZE];
	struct av_http_request request;
	size_t request_length = 0;
	int result;

	result = av_set_client_timeouts(fd);
	if (result < 0)
		return result;
	result = av_receive_request(fd, buffer, sizeof(buffer),
				    &request_length, server);
	if (result < 0) {
		if (result != -EINTR) {
			pthread_mutex_lock(&server->stats_lock);
			server->stats.bad_requests++;
			pthread_mutex_unlock(&server->stats_lock);
		}
		return result;
	}
	result = av_parse_request_line(buffer, &request);
	if (result < 0) {
		static const char bad_request[] =
			"{\"error\":\"malformed HTTP request\"}\n";

		pthread_mutex_lock(&server->stats_lock);
		server->stats.bad_requests++;
		pthread_mutex_unlock(&server->stats_lock);
		return av_send_text(fd, 400, "application/json; charset=utf-8",
				    NULL, bad_request, server);
	}

	printf("HTTP request    : peer=%s method=%s path=%s bytes=%lu\n",
	       peer, request.method, request.path,
	       (unsigned long)request_length);
	result = av_route_request(fd, peer, &request, server);
	if (result != AV_HTTP_HANDOFF) {
		pthread_mutex_lock(&server->stats_lock);
		if (result < 0 && result != -EINTR && !av_client_gone(result))
			server->stats.send_errors++;
		else if (result == 0)
			server->stats.completed++;
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
	timeout.tv_sec = AV_HTTP_ACCEPT_POLL_MS / 1000U;
	timeout.tv_usec =
		(AV_HTTP_ACCEPT_POLL_MS % 1000U) * 1000U;
	result = select(listener + 1, &read_set, NULL, NULL, &timeout);
	if (result < 0)
		return -errno;
	return result;
}

static void *av_accept_thread_main(void *argument)
{
	struct av_http_server *server = argument;

	while (!av_server_should_stop(server)) {
		struct sockaddr_in peer_address;
		socklen_t peer_length = sizeof(peer_address);
		char peer_ip[INET_ADDRSTRLEN];
		char peer[64];
		int wait_result;
		int client_fd;
		int result;

		if (av_mjpeg_pipeline_failed(server->pipeline)) {
			av_server_fail(server, EIO);
			break;
		}
		wait_result = av_wait_listener(server->listener);
		if (wait_result < 0) {
			if (wait_result == -EINTR || av_server_should_stop(server))
				continue;
			fprintf(stderr, "HTTP listener select failed: %s\n",
				strerror(-wait_result));
			av_server_fail(server, -wait_result);
			break;
		}
		if (wait_result == 0)
			continue;

		client_fd = accept(server->listener,
				   (struct sockaddr *)&peer_address,
				   &peer_length);
		if (client_fd < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "HTTP accept failed: %s\n", strerror(errno));
			av_server_fail(server, errno);
			break;
		}
		pthread_mutex_lock(&server->stats_lock);
		server->stats.accepted++;
		pthread_mutex_unlock(&server->stats_lock);

		if (!inet_ntop(AF_INET, &peer_address.sin_addr,
			       peer_ip, sizeof(peer_ip)))
			strcpy(peer_ip, "unknown");
		snprintf(peer, sizeof(peer), "%s:%u", peer_ip,
			 (unsigned int)ntohs(peer_address.sin_port));

		result = av_handle_client(client_fd, peer, server);
		if (result != AV_HTTP_HANDOFF) {
			if (result < 0 && result != -EINTR &&
			    !av_client_gone(result))
				fprintf(stderr, "HTTP client %s failed: %s\n",
					peer, strerror(-result));
			(void)shutdown(client_fd, SHUT_RDWR);
			(void)close(client_fd);
		}
	}
	return NULL;
}

int av_http_server_create(struct av_http_server **server_out,
			  const struct av_http_server_config *config)
{
	struct av_http_server *server;
	int error;
	int result;

	if (!server_out || !config || !config->pipeline ||
	    !config->format_status || !config->version ||
	    config->port == 0U || config->port > 65535U ||
	    strlen(config->version) >= sizeof(server->version))
		return -EINVAL;
	*server_out = NULL;
	server = calloc(1, sizeof(*server));
	if (!server)
		return -ENOMEM;
	server->listener = -1;
	server->pipeline = config->pipeline;
	server->port = config->port;
	server->format_status = config->format_status;
	server->health_ok = config->health_ok;
	server->callback_opaque = config->callback_opaque;
	snprintf(server->version, sizeof(server->version), "%s",
		 config->version);
	av_mjpeg_pipeline_get_info(server->pipeline, &server->video);

	error = pthread_mutex_init(&server->state_lock, NULL);
	if (error) {
		result = -error;
		goto fail;
	}
	server->state_lock_initialized = 1;
	error = pthread_mutex_init(&server->stats_lock, NULL);
	if (error) {
		result = -error;
		goto fail;
	}
	server->stats_lock_initialized = 1;
	server->listener = av_create_listener(server->port);
	if (server->listener < 0) {
		result = server->listener;
		server->listener = -1;
		goto fail;
	}
	result = av_network_init(&server->network, server,
				 server->video.jpeg_capacity);
	if (result < 0)
		goto fail;

	*server_out = server;
	return 0;

fail:
	av_network_destroy(&server->network);
	if (server->listener >= 0)
		(void)close(server->listener);
	if (server->stats_lock_initialized)
		(void)pthread_mutex_destroy(&server->stats_lock);
	if (server->state_lock_initialized)
		(void)pthread_mutex_destroy(&server->state_lock);
	free(server);
	return result;
}

int av_http_server_start(struct av_http_server *server)
{
	int error;

	if (!server)
		return -EINVAL;
	pthread_mutex_lock(&server->state_lock);
	if (server->started || server->stop_requested) {
		pthread_mutex_unlock(&server->state_lock);
		return -EINVAL;
	}
	server->started_ms = av_now_ms();
	server->started = 1;
	pthread_mutex_unlock(&server->state_lock);
	error = pthread_create(&server->accept_thread, NULL,
			       av_accept_thread_main, server);
	if (error) {
		pthread_mutex_lock(&server->state_lock);
		server->started = 0;
		pthread_mutex_unlock(&server->state_lock);
		return -error;
	}
	server->accept_thread_started = 1;
	return 0;
}

int av_http_server_failed(struct av_http_server *server)
{
	int failed;

	if (!server)
		return 1;
	pthread_mutex_lock(&server->state_lock);
	failed = server->failed;
	pthread_mutex_unlock(&server->state_lock);
	return failed;
}

void av_http_server_stop(struct av_http_server *server)
{
	if (!server)
		return;
	if (server->state_lock_initialized) {
		pthread_mutex_lock(&server->state_lock);
		server->stop_requested = 1;
		pthread_mutex_unlock(&server->state_lock);
	}
	av_network_request_stop(&server->network);
	if (server->accept_thread_started) {
		(void)pthread_join(server->accept_thread, NULL);
		server->accept_thread_started = 0;
	}
	av_network_destroy(&server->network);
	if (server->listener >= 0) {
		(void)close(server->listener);
		server->listener = -1;
	}
	if (server->state_lock_initialized) {
		pthread_mutex_lock(&server->state_lock);
		server->started = 0;
		pthread_mutex_unlock(&server->state_lock);
	}
}

void av_http_server_destroy(struct av_http_server *server)
{
	if (!server)
		return;
	av_http_server_stop(server);
	if (server->stats_lock_initialized)
		(void)pthread_mutex_destroy(&server->stats_lock);
	if (server->state_lock_initialized)
		(void)pthread_mutex_destroy(&server->state_lock);
	free(server);
}
