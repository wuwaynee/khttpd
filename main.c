/* SPDX-License-Identifier: MIT
 * Derived from the NCKU/sysprog21 khttpd teaching project.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/inet.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/sockptr.h>
#include "http_server.h"

static unsigned short port = 8081;
module_param(port, ushort, 0444);
MODULE_PARM_DESC(port, "TCP port (1..65535)");
static char *bind_addr = "127.0.0.1";
module_param(bind_addr, charp, 0444);
MODULE_PARM_DESC(bind_addr, "IPv4 bind address; loopback by default");
static unsigned int workers = 64;
module_param(workers, uint, 0444);
MODULE_PARM_DESC(workers, "Fixed worker/active connection limit (1..512)");
static unsigned int shards = 1;
module_param(shards, uint, 0444);
MODULE_PARM_DESC(shards, "SO_REUSEPORT listeners (1..min(workers,64))");
static unsigned int backlog = 512;
module_param(backlog, uint, 0444);
MODULE_PARM_DESC(backlog, "Pending accept queue limit per listener (1..4096)");
static unsigned int timeout_ms = 5000;
module_param(timeout_ms, uint, 0444);
MODULE_PARM_DESC(timeout_ms, "Absolute header/idle and response deadline (100..60000 ms)");
static unsigned int max_requests = 1024;
module_param(max_requests, uint, 0444);
MODULE_PARM_DESC(max_requests, "Maximum responses per connection (1..1000000)");
static bool keep_alive = true;
module_param(keep_alive, bool, 0444);
static bool prealloc = true;
module_param(prealloc, bool, 0444);
MODULE_PARM_DESC(prealloc, "Preallocate one reusable header buffer per worker");
static int fail_start_after = -1;
module_param(fail_start_after, int, 0444);
MODULE_PARM_DESC(fail_start_after, "Test only: fail init after N workers, -1 disables");

struct kh_config kh_config;
bool kh_stopping;
atomic64_t kh_responses = ATOMIC64_INIT(0);
atomic64_t kh_ok = ATOMIC64_INIT(0);
atomic64_t kh_protocol_errors = ATOMIC64_INIT(0);
atomic64_t kh_io_errors = ATOMIC64_INIT(0);
atomic64_t kh_timeouts = ATOMIC64_INIT(0);
atomic64_t kh_alloc_errors = ATOMIC64_INIT(0);

static struct socket **listeners;
static struct kh_worker *pool;

static int stats_get(char *buffer, const struct kernel_param *param)
{
    (void)param;
    return scnprintf(buffer, PAGE_SIZE,
        "responses=%lld ok=%lld protocol_errors=%lld io_errors=%lld "
        "timeouts=%lld alloc_errors=%lld\n",
        (long long)atomic64_read(&kh_responses),
        (long long)atomic64_read(&kh_ok),
        (long long)atomic64_read(&kh_protocol_errors),
        (long long)atomic64_read(&kh_io_errors),
        (long long)atomic64_read(&kh_timeouts),
        (long long)atomic64_read(&kh_alloc_errors));
}
static const struct kernel_param_ops stats_ops = { .get = stats_get };
module_param_cb(stats, &stats_ops, NULL, 0444);

static int open_listener(__be32 address, struct socket **result)
{
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = address,
    };
    struct socket *socket;
    int value = 1, err;

    err = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP, &socket);
    if (err)
        return err;
    err = sock_setsockopt(socket, SOL_SOCKET, SO_REUSEADDR,
                         KERNEL_SOCKPTR(&value), sizeof(value));
    if (err)
        goto fail;
    if (shards > 1) {
        err = sock_setsockopt(socket, SOL_SOCKET, SO_REUSEPORT,
                             KERNEL_SOCKPTR(&value), sizeof(value));
        if (err)
            goto fail;
    }
    WRITE_ONCE(socket->sk->sk_rcvtimeo, msecs_to_jiffies(250));
    /* The explicit length describes this sockaddr_in. Older kernels take
     * sockaddr *, newer ones sockaddr_unsized *; both use this same storage.
     * A void pointer conversion avoids guessing a version/backport boundary.
     */
    err = kernel_bind(socket, (void *)&addr, sizeof(addr));
    if (err)
        goto fail;
    err = kernel_listen(socket, backlog);
    if (err)
        goto fail;
    *result = socket;
    return 0;
fail:
    sock_release(socket);
    return err;
}

static void stop_server(void)
{
    unsigned int i;

    WRITE_ONCE(kh_stopping, true);
    /* Wake accepts, then receive/send calls, before joining any worker. */
    if (listeners)
        for (i = 0; i < shards; ++i)
            if (listeners[i])
                kernel_sock_shutdown(listeners[i], SHUT_RDWR);
    if (pool) {
        for (i = 0; i < workers; ++i) {
            mutex_lock(&pool[i].client_lock);
            if (pool[i].client)
                kernel_sock_shutdown(pool[i].client, SHUT_RDWR);
            mutex_unlock(&pool[i].client_lock);
        }
        for (i = 0; i < workers; ++i) {
            if (pool[i].task) {
                kthread_stop(pool[i].task);
                put_task_struct(pool[i].task);
            }
            kfree(pool[i].buffer);
        }
    }
    /* No worker can reach a listener or buffer after the joins. */
    if (listeners)
        for (i = 0; i < shards; ++i)
            if (listeners[i])
                sock_release(listeners[i]);
    kfree(pool);
    kfree(listeners);
}

static int __init khttpd_init(void)
{
    __be32 address;
    const char *end;
    unsigned int i;
    int err;

    if (!port || !workers || workers > 512 || !shards ||
        shards > workers || shards > 64 || !backlog || backlog > 4096 ||
        timeout_ms < 100 || timeout_ms > 60000 ||
        !max_requests || max_requests > 1000000 ||
        fail_start_after < -1 || fail_start_after > (int)workers ||
        !in4_pton(bind_addr, -1, (u8 *)&address, -1, &end) || *end)
        return -EINVAL;

    kh_config = (struct kh_config) {
        .timeout_ms = timeout_ms, .max_requests = max_requests,
        .keep_alive = keep_alive, .prealloc = prealloc,
    };
    listeners = kcalloc(shards, sizeof(*listeners), GFP_KERNEL);
    pool = kcalloc(workers, sizeof(*pool), GFP_KERNEL);
    /* Initialize all locks even if a later allocation fails. */
    if (pool)
        for (i = 0; i < workers; ++i)
            mutex_init(&pool[i].client_lock);
    if (!listeners || !pool) {
        err = -ENOMEM;
        goto fail;
    }
    for (i = 0; i < shards; ++i) {
        err = open_listener(address, &listeners[i]);
        if (err)
            goto fail;
    }
    for (i = 0; i < workers; ++i) {
        if (fail_start_after == (int)i) {
            err = -EIO;
            goto fail;
        }
        pool[i].listener = listeners[i % shards];
        if (prealloc) {
            pool[i].buffer = kmalloc(KH_HEADER_CAP, GFP_KERNEL);
            if (!pool[i].buffer) {
                err = -ENOMEM;
                goto fail;
            }
        }
        pool[i].task = kthread_create(http_server_daemon, &pool[i], "khttpd");
        if (IS_ERR(pool[i].task)) {
            err = PTR_ERR(pool[i].task);
            pool[i].task = NULL;
            goto fail;
        }
        /* A task may exit before stop_server reaches it: own a reference. */
        get_task_struct(pool[i].task);
        wake_up_process(pool[i].task);
    }
    if (fail_start_after == (int)workers) {
        err = -EIO;
        goto fail;
    }
    pr_info("ready bind=%s port=%hu workers=%u shards=%u prealloc=%d keep_alive=%d\n",
            bind_addr, port, workers, shards, prealloc, keep_alive);
    return 0;
fail:
    stop_server();
    return err;
}

static void __exit khttpd_exit(void)
{
    stop_server();
    pr_info("stopped after joining all workers\n");
}
module_init(khttpd_init);
module_exit(khttpd_exit);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("NCKU/sysprog21 contributors; Wayne Wu");
MODULE_DESCRIPTION("Bounded experimental kernel HTTP microserver");
MODULE_VERSION("0.2");
