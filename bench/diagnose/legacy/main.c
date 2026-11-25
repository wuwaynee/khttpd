/* SPDX-License-Identifier: MIT
 * Historical experiment adapter, derived from NCKU/sysprog21 and Wayne Wu.
 * Listener model follows c745737; defaults reproduce 3fbe63b close/log/clear.
 * sock_setsockopt is a compatibility repair, not a workload delay.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/module.h>
#include <linux/inet.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/sockptr.h>
#include <net/tcp.h>
#include "http_server.h"

static ushort port = 8081;
module_param(port, ushort, 0444);
static uint shards = 1, backlog = 100, sndbuf = 0;
module_param(shards, uint, 0444);
module_param(backlog, uint, 0444);
module_param(sndbuf, uint, 0444);
static int fail_start_after = -1;
module_param(fail_start_after, int, 0444);
MODULE_PARM_DESC(fail_start_after, "Test only: fail after N prepared acceptors, -1 disables");
bool keep_alive, log_requests = true, clear_buffer = true, use_pool = true, extra_crlf;
module_param(keep_alive, bool, 0444);
module_param(log_requests, bool, 0444);
module_param(clear_buffer, bool, 0444);
module_param(use_pool, bool, 0444);
module_param(extra_crlf, bool, 0444);

mempool_t *http_buf_pool;
DEFINE_PER_CPU(struct legacy_counts, legacy_counts);
static struct socket **listeners;
static struct task_struct **tasks;
static struct http_server_param *params;

static int stats_get(char *buffer, const struct kernel_param *param)
{
    struct legacy_counts sum = { 0 };
    int cpu;

    (void)param;
    for_each_possible_cpu(cpu) {
        struct legacy_counts *p = &per_cpu(legacy_counts, cpu);
        sum.accepted += READ_ONCE(p->accepted);
        sum.spawned += READ_ONCE(p->spawned);
        sum.responses += READ_ONCE(p->responses);
        sum.send_bytes += READ_ONCE(p->send_bytes);
        sum.buffer_alloc_calls += READ_ONCE(p->buffer_alloc_calls);
        sum.send_errors += READ_ONCE(p->send_errors);
    }
    return scnprintf(buffer, PAGE_SIZE,
        "accepted=%llu spawned=%llu responses=%llu send_bytes=%llu "
        "buffer_alloc_calls=%llu send_errors=%llu\n",
        sum.accepted, sum.spawned, sum.responses, sum.send_bytes,
        sum.buffer_alloc_calls, sum.send_errors);
}
static const struct kernel_param_ops stats_ops = { .get = stats_get };
module_param_cb(stats, &stats_ops, NULL, 0444);

static int option(struct socket *s, int name, int value)
{
    return sock_setsockopt(s, SOL_SOCKET, name, KERNEL_SOCKPTR(&value), sizeof(value));
}

static int open_listener(struct socket **out)
{
    struct sockaddr_in address = {
        .sin_family = AF_INET, .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    struct socket *s;
    int err = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP, &s);

    if (err)
        return err;
    err = option(s, SO_REUSEADDR, 1);
    if (err)
        goto fail;
    if (shards > 1) {
        err = option(s, SO_REUSEPORT, 1);
        if (err)
            goto fail;
    }
    err = option(s, SO_RCVBUF, 1024 * 1024);
    if (err)
        goto fail;
    /* The historical wrapper ignored SO_SNDBUF. Preserve that with 0. */
    if (sndbuf) {
        err = option(s, SO_SNDBUF, sndbuf);
        if (err)
            goto fail;
    }
    tcp_sock_set_nodelay(s->sk);
    tcp_sock_set_cork(s->sk, false);
    err = kernel_bind(s, (void *)&address, sizeof(address));
    if (!err)
        err = kernel_listen(s, backlog);
    if (err)
        goto fail;
    *out = s;
    return 0;
fail:
    sock_release(s);
    return err;
}

static void stop(void)
{
    unsigned int i;

    if (listeners)
        for (i = 0; i < shards; ++i)
            if (listeners[i])
                kernel_sock_shutdown(listeners[i], SHUT_RDWR);
    if (tasks)
        for (i = 0; i < shards; ++i)
            if (tasks[i]) {
                send_sig(SIGTERM, tasks[i], 1);
                kthread_stop(tasks[i]);
                put_task_struct(tasks[i]);
            }
    if (listeners)
        for (i = 0; i < shards; ++i)
            if (listeners[i])
                sock_release(listeners[i]);
    kfree(params);
    kfree(tasks);
    kfree(listeners);
    if (http_buf_pool)
        mempool_destroy(http_buf_pool);
}

static int __init start(void)
{
    unsigned int i;
    int err;

    if (!port || !shards || shards > 16 || !backlog || backlog > 4096 ||
        fail_start_after < -1 || fail_start_after > (int)shards)
        return -EINVAL;
    if (use_pool) {
        http_buf_pool = mempool_create(4, http_buf_alloc, http_buf_free, NULL);
        if (!http_buf_pool)
            return -ENOMEM;
    }
    listeners = kcalloc(shards, sizeof(*listeners), GFP_KERNEL);
    tasks = kcalloc(shards, sizeof(*tasks), GFP_KERNEL);
    params = kcalloc(shards, sizeof(*params), GFP_KERNEL);
    if (!listeners || !tasks || !params) {
        err = -ENOMEM;
        goto fail;
    }
    for (i = 0; i < shards; ++i) {
        if (fail_start_after == (int)i) {
            err = -EIO;
            goto fail;
        }
        err = open_listener(&listeners[i]);
        if (err)
            goto fail;
        params[i].listen_socket = listeners[i];
        tasks[i] = kthread_create(http_server_daemon, &params[i], "kh_legacy_accept");
        if (IS_ERR(tasks[i])) {
            err = PTR_ERR(tasks[i]);
            tasks[i] = NULL;
            goto fail;
        }
        get_task_struct(tasks[i]);
    }
    if (fail_start_after == (int)shards) {
        err = -EIO;
        goto fail;
    }
    /* No connection worker may run while initialization can still fail:
     * failed init must be able to destroy the pool without live borrowers.
     */
    for (i = 0; i < shards; ++i)
        wake_up_process(tasks[i]);
    pr_info("ready keep_alive=%d log=%d clear=%d shards=%u backlog=%u pool=%d\n",
            keep_alive, log_requests, clear_buffer, shards, backlog, use_pool);
    return 0;
fail:
    stop();
    return err;
}
static void __exit finish(void)
{
    /* Every connection owns a module reference and exits via the kernel's
     * module_put_and_kthread_exit helper. rmmod refuses while one is active.
     */
    stop();
}
module_init(start);
module_exit(finish);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("Historical khttpd performance reconstruction, loopback only");
MODULE_AUTHOR("NCKU/sysprog21 contributors; Wayne Wu");
