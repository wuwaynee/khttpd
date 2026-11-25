/* SPDX-License-Identifier: MIT */
#ifndef KHTTPD_PROTOCOL_H
#define KHTTPD_PROTOCOL_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#endif

#define KH_HEADER_CAP 8192
#define KH_TARGET_CAP 2048
#define KH_HEADER_COUNT 64

struct kh_request {
    int status;
    int keep_alive;
    int head;
};

struct kh_response {
    const char *data;
    size_t length;
};

/* scan is retained between reads, so a byte-at-a-time sender costs O(n). */
size_t kh_header_end(const char *buf, size_t length, size_t *scan);
struct kh_request kh_parse(const char *buf, size_t length);
struct kh_response kh_response(int status, int keep_alive, int head);

#endif
