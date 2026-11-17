/* SPDX-License-Identifier: MIT */
#ifndef KHTTPD_CONNECTION_H
#define KHTTPD_CONNECTION_H
#include "http_protocol.h"

/* Adapters return bytes, 0 for EOF/zero send, or one of these values. */
#define KH_IO_RETRY (-1)
#define KH_IO_ERROR (-2)

struct kh_io {
    void *context;
    int (*recv)(void *context, char *buf, size_t size);
    int (*send)(void *context, const char *buf, size_t size);
    unsigned long long (*now_ms)(void *context);
    int (*stopped)(void *context);
    unsigned int timeout_ms;
    unsigned int max_requests;
    int keep_alive;
};

struct kh_stats {
    unsigned long long responses;
    unsigned long long ok;
    unsigned long long protocol_errors;
    unsigned long long io_errors;
    unsigned long long timeouts;
};

/* Caller owns buffer (KH_HEADER_CAP bytes), transport, and its lifetime. */
struct kh_stats kh_serve(struct kh_io *io, char *buffer);
#endif
