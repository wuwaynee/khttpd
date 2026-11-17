/* SPDX-License-Identifier: MIT
 * Derived from the NCKU/sysprog21 khttpd teaching project.
 * Connection engine and bounded worker lifecycle revised in 2026.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/slab.h>
#include <linux/tcp.h>
#include <net/tcp.h>
#include "http_server.h"

struct kernel_io {
    struct socket *socket;
};

static int transport_result(int n)
{
    if (n == -EAGAIN || n == -EINTR || n == -ERESTARTSYS)
        return KH_IO_RETRY;
    return n < 0 ? KH_IO_ERROR : n;
}

static int kh_kernel_recv(void *context, char *buffer, size_t size)
{
    struct kernel_io *io = context;
    struct kvec iov = { .iov_base = buffer, .iov_len = size };
    struct msghdr msg = { 0 };

    return transport_result(kernel_recvmsg(io->socket, &msg, &iov, 1, size, 0));
}

static int kh_kernel_send(void *context, const char *buffer, size_t size)
{
    struct kernel_io *io = context;
    struct kvec iov = { .iov_base = (void *)buffer, .iov_len = size };
    struct msghdr msg = { .msg_flags = MSG_NOSIGNAL };

    return transport_result(kernel_sendmsg(io->socket, &msg, &iov, 1, size));
}

static unsigned long long clock_ms(void *context)
{
    (void)context;
    return ktime_get_ns() / 1000000;
}

static int stopping(void *context)
{
    (void)context;
    return READ_ONCE(kh_stopping) || kthread_should_stop();
}

static void serve(struct socket *socket, char *buffer)
{
    struct kernel_io context = { .socket = socket };
    struct kh_io io = {
        .context = &context,
        .recv = kh_kernel_recv,
        .send = kh_kernel_send,
        .now_ms = clock_ms,
        .stopped = stopping,
        .timeout_ms = kh_config.timeout_ms,
        .max_requests = kh_config.max_requests,
        .keep_alive = kh_config.keep_alive,
    };
    struct kh_stats stats = kh_serve(&io, buffer);

    /* Publish at connection close, avoiding a global atomic on every GET. */
    atomic64_add(stats.responses, &kh_responses);
    atomic64_add(stats.ok, &kh_ok);
    atomic64_add(stats.protocol_errors, &kh_protocol_errors);
    atomic64_add(stats.io_errors, &kh_io_errors);
    atomic64_add(stats.timeouts, &kh_timeouts);
}

int http_server_daemon(void *arg)
{
    struct kh_worker *worker = arg;

    while (!stopping(NULL)) {
        struct socket *socket = NULL;
        char *buffer;
        int err = kernel_accept(worker->listener, &socket, 0);

        if (err) {
            if (stopping(NULL))
                break;
            if (err != -EAGAIN && err != -EINTR && err != -ERESTARTSYS) {
                pr_warn_ratelimited("accept failed: %d\n", err);
                schedule_timeout_interruptible(msecs_to_jiffies(20));
            }
            continue;
        }

        /* Publication and removal serialize with module shutdown. */
        mutex_lock(&worker->client_lock);
        if (stopping(NULL)) {
            mutex_unlock(&worker->client_lock);
            kernel_sock_shutdown(socket, SHUT_RDWR);
            sock_release(socket);
            break;
        }
        worker->client = socket;
        mutex_unlock(&worker->client_lock);

        WRITE_ONCE(socket->sk->sk_rcvtimeo, msecs_to_jiffies(250));
        WRITE_ONCE(socket->sk->sk_sndtimeo, msecs_to_jiffies(250));
        tcp_sock_set_nodelay(socket->sk);
        buffer = kh_config.prealloc ? worker->buffer :
            kmalloc(KH_HEADER_CAP, GFP_KERNEL_ACCOUNT);
        if (buffer) {
            serve(socket, buffer);
            if (!kh_config.prealloc)
                kfree(buffer);
        } else {
            atomic64_inc(&kh_alloc_errors);
        }

        mutex_lock(&worker->client_lock);
        worker->client = NULL;
        mutex_unlock(&worker->client_lock);
        kernel_sock_shutdown(socket, SHUT_RDWR);
        sock_release(socket);
        cond_resched();
    }
    return 0;
}
