/* SPDX-License-Identifier: MIT */
#ifndef KHTTPD_HTTP_SERVER_H
#define KHTTPD_HTTP_SERVER_H
#include <linux/atomic.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <net/sock.h>
#include "http_connection.h"

struct kh_config {
    unsigned int timeout_ms;
    unsigned int max_requests;
    bool keep_alive;
    bool prealloc;
};

struct kh_worker {
    struct task_struct *task; /* Explicit reference until joined. */
    struct socket *listener; /* Owned by main.c, released after every join. */
    struct socket *client;   /* Protected by client_lock, released by worker. */
    struct mutex client_lock;
    char *buffer;
};

extern struct kh_config kh_config;
extern bool kh_stopping;
extern atomic64_t kh_responses, kh_ok, kh_protocol_errors;
extern atomic64_t kh_io_errors, kh_timeouts, kh_alloc_errors;
int http_server_daemon(void *arg);
#endif
