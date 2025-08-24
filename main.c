#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cpu.h>
#include <linux/kthread.h>
#include <linux/mempool.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/tcp.h>
#include <linux/version.h>
#include <net/sock.h>

#include "http_server.h"

#define DEFAULT_PORT    8081
#define DEFAULT_BACKLOG 512
#define POOL_MIN_NR     4

mempool_t *http_buf_pool;

static ushort port = DEFAULT_PORT;
module_param(port, ushort, 0444);
static ushort backlog = DEFAULT_BACKLOG;
module_param(backlog, ushort, 0444);

static int workers = 0;
module_param(workers, int, 0444);

static struct socket        **listen_sockets;
static struct http_server_param *params;
static struct task_struct   **http_servers;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)

static int set_sock_opt(struct socket *sock, int level, int optname,
                        char *optval, unsigned int optlen)
{
    int v;

    if (optlen < sizeof(int))
        return -EINVAL;

    v = *(int *)optval;

    switch (optname) {
    case SO_REUSEADDR:
        sock_set_reuseaddr(sock->sk);
        break;
    case SO_REUSEPORT:
        sock_set_reuseport(sock->sk, v);
        break;
    case SO_RCVBUF:
        sock_set_rcvbuf(sock->sk, v);
        break;
    case SO_SNDBUF:
        sock_set_sndbuf(sock->sk, v);
        break;
    default:
        return -ENOPROTOOPT;
    }
    return 0;
}

static int set_tcp_opt(struct socket *sock, int level, int optname,
                       char *optval, unsigned int optlen)
{
    if (optlen < sizeof(int))
        return -EINVAL;

    switch (optname) {
    case TCP_NODELAY:
        tcp_sock_set_nodelay(sock->sk);
        break;
    case TCP_CORK:
        tcp_sock_set_cork(sock->sk, *(bool *)optval);
        break;
    default:
        return -ENOPROTOOPT;
    }
    return 0;
}

static int kernel_setsockopt(struct socket *sock, int level, int optname,
                             char *optval, unsigned int optlen)
{
    if (level == SOL_SOCKET)
        return set_sock_opt(sock, level, optname, optval, optlen);
    else if (level == SOL_TCP)
        return set_tcp_opt(sock, level, optname, optval, optlen);
    return -EINVAL;
}
#endif /* >= 5.8 */

static inline int setsockopt_int(struct socket *sock, int level,
                                 int optname, int optval)
{
    int opt = optval;
    return kernel_setsockopt(sock, level, optname,
                             (char *)&opt, sizeof(opt));
}

static int open_listen_socket(ushort portnr, ushort bklg, struct socket **res)
{
    struct socket *sock;
    struct sockaddr_in s;
    int err;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 2, 0)
    err = sock_create_kern(&init_net, PF_INET, SOCK_STREAM, IPPROTO_TCP, &sock);
#else
    err = sock_create(PF_INET, SOCK_STREAM, IPPROTO_TCP, &sock);
#endif
    if (err < 0) {
        pr_err("sock_create() failure, err=%d\n", err);
        return err;
    }

    if ((err = setsockopt_int(sock, SOL_SOCKET, SO_REUSEADDR, 1)) < 0)
        goto bail_setsockopt;
    if ((err = setsockopt_int(sock, SOL_SOCKET, SO_REUSEPORT, 1)) < 0)
        goto bail_setsockopt;

    if ((err = setsockopt_int(sock, SOL_TCP, TCP_NODELAY, 1)) < 0)
        goto bail_setsockopt;
    if ((err = setsockopt_int(sock, SOL_TCP, TCP_CORK, 0)) < 0)
        goto bail_setsockopt;

    if ((err = setsockopt_int(sock, SOL_SOCKET, SO_RCVBUF, 1024 * 1024)) < 0)
        goto bail_setsockopt;
    if ((err = setsockopt_int(sock, SOL_SOCKET, SO_SNDBUF, 1024 * 1024)) < 0)
        goto bail_setsockopt;

    memset(&s, 0, sizeof(s));
    s.sin_family      = AF_INET;
    s.sin_addr.s_addr = htonl(INADDR_ANY);
    s.sin_port        = htons(portnr);

    err = kernel_bind(sock, (struct sockaddr *)&s, sizeof(s));
    if (err < 0) {
        pr_err("kernel_bind() failure, err=%d\n", err);
        goto bail_sock;
    }

    err = kernel_listen(sock, bklg);
    if (err < 0) {
        pr_err("kernel_listen() failure, err=%d\n", err);
        goto bail_sock;
    }

    *res = sock;
    return 0;

bail_setsockopt:
    pr_err("kernel_setsockopt() failure, err=%d\n", err);
bail_sock:
    sock_release(sock);
    return err;
}

static void close_listen_socket(struct socket *socket)
{
    if (!socket) return;
    kernel_sock_shutdown(socket, SHUT_RDWR);
    sock_release(socket);
}

static int __init khttpd_init(void)
{
    int i, err;

    if (!(http_buf_pool = mempool_create(POOL_MIN_NR, http_buf_alloc,
                                         http_buf_free, NULL))) {
        pr_err("failed to create mempool\n");
        return -ENOMEM;
    }

    if (workers <= 0)
        workers = max(2, num_online_cpus()); 

    listen_sockets = kcalloc(workers, sizeof(*listen_sockets), GFP_KERNEL);
    params         = kcalloc(workers, sizeof(*params), GFP_KERNEL);
    http_servers   = kcalloc(workers, sizeof(*http_servers), GFP_KERNEL);
    if (!listen_sockets || !params || !http_servers) {
        err = -ENOMEM;
        goto bail_alloc;
    }

    for (i = 0; i < workers; i++) {
        err = open_listen_socket(port, backlog, &listen_sockets[i]);
        if (err < 0) {
            pr_err("open_listen_socket[%d] failed: %d\n", i, err);
            goto bail_open;
        }

        params[i].listen_socket = listen_sockets[i];

        http_servers[i] = kthread_run(http_server_daemon, &params[i],
                                      KBUILD_MODNAME);
        if (IS_ERR(http_servers[i])) {
            err = PTR_ERR(http_servers[i]);
            pr_err("can't start http server daemon[%d]: %d\n", i, err);
            http_servers[i] = NULL;
            goto bail_open;
        }
    }

    pr_info("khttpd started: port=%u backlog=%u workers=%d\n",
            port, backlog, workers);
    return 0;

bail_open:
    for (i = 0; i < workers; i++) {
        if (http_servers && http_servers[i]) {
            send_sig(SIGTERM, http_servers[i], 1);
            kthread_stop(http_servers[i]);
        }
        if (listen_sockets && listen_sockets[i])
            close_listen_socket(listen_sockets[i]);
    }
bail_alloc:
    kfree(http_servers);
    kfree(params);
    kfree(listen_sockets);
    mempool_destroy(http_buf_pool);
    return err;
}

static void __exit khttpd_exit(void)
{
    int i;
    if (http_servers) {
        for (i = 0; i < workers; i++) {
            if (http_servers[i]) {
                send_sig(SIGTERM, http_servers[i], 1);
                kthread_stop(http_servers[i]);
            }
        }
    }
    if (listen_sockets) {
        for (i = 0; i < workers; i++)
            if (listen_sockets[i])
                close_listen_socket(listen_sockets[i]);
    }

    kfree(http_servers);
    kfree(params);
    kfree(listen_sockets);

    if (http_buf_pool)
        mempool_destroy(http_buf_pool);

    pr_info("module unloaded\n");
}

module_init(khttpd_init);
module_exit(khttpd_exit);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("National Cheng Kung University, Taiwan");
MODULE_DESCRIPTION("in-kernel HTTP daemon");
MODULE_VERSION("0.1");
