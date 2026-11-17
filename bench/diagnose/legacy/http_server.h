/* SPDX-License-Identifier: MIT */
#ifndef KH_LEGACY_SERVER_H
#define KH_LEGACY_SERVER_H
#include <linux/module.h>
#include <linux/mempool.h>
#include <linux/slab.h>
#include <linux/percpu.h>
#include <net/sock.h>

#define RECV_BUFFER_SIZE 4096
extern mempool_t *http_buf_pool;
extern bool keep_alive, log_requests, clear_buffer, use_pool, extra_crlf;
struct legacy_counts {
    u64 accepted, spawned, responses, send_bytes, buffer_alloc_calls, send_errors;
};
DECLARE_PER_CPU(struct legacy_counts, legacy_counts);
struct http_server_param { struct socket *listen_socket; };
int http_server_daemon(void *arg);
static inline void *http_buf_alloc(gfp_t mask, void *data)
{
    (void)data;
    return kzalloc(RECV_BUFFER_SIZE, mask);
}
static inline void http_buf_free(void *buffer, void *data)
{
    (void)data;
    kfree(buffer);
}
#endif
