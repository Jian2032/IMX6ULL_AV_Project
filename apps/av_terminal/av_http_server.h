/* SPDX-License-Identifier: MIT */
/*
 * av_http_server.h - AV-R3 reusable HTTP/MJPEG consumer
 *
 * The server never opens /dev/video0.  It consumes complete JPEG snapshots
 * from the already-running av_mjpeg_pipeline owned by av_terminal.
 */

#ifndef AV_HTTP_SERVER_H
#define AV_HTTP_SERVER_H

#include <stddef.h>
#include <stdint.h>

struct av_mjpeg_pipeline;
struct av_http_server;

struct av_http_server_stats {
	uint64_t uptime_ms;
	uint64_t accepted;
	uint64_t completed;
	uint64_t bad_requests;
	uint64_t rejected_streams;
	uint64_t send_errors;
	uint64_t bytes_sent;
	uint64_t stream_sessions;
	uint64_t stream_disconnects;
	uint64_t stream_frames;
	uint64_t client_frames_skipped;
	uint64_t stream_send_us;
	int stream_client_active;
	int failed;
	int error_number;
};

/*
 * The status callback formats the complete JSON object for /status.  The
 * server passes a race-free snapshot of its own counters; the integration
 * layer can add video, LCD and audio state without coupling this module to
 * those implementations.
 */
typedef int (*av_http_status_callback)(
	void *opaque,
	const struct av_http_server_stats *http,
	char *destination,
	size_t capacity);

/* Return nonzero while /health should report HTTP 200. */
typedef int (*av_http_health_callback)(void *opaque);

struct av_http_server_config {
	struct av_mjpeg_pipeline *pipeline;
	unsigned int port;
	const char *version;
	av_http_status_callback format_status;
	av_http_health_callback health_ok;
	void *callback_opaque;
};

int av_http_server_create(struct av_http_server **server_out,
			  const struct av_http_server_config *config);
int av_http_server_start(struct av_http_server *server);
void av_http_server_get_stats(struct av_http_server *server,
			      struct av_http_server_stats *stats);
int av_http_server_failed(struct av_http_server *server);

/*
 * stop first closes the HTTP ownership boundary: the listener exits and an
 * active socket is shut down before the JPEG/video producer may be stopped.
 */
void av_http_server_stop(struct av_http_server *server);
void av_http_server_destroy(struct av_http_server *server);

#endif /* AV_HTTP_SERVER_H */
