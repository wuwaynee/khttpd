/* SPDX-License-Identifier: MIT
 * Hot path recovered from c745737c59476688755ca171da084a9670c5d9db.
 * Parameters restore the 3fbe63b historical close/log/clear behavior.
 * Common fixes: bounded URL, zero-send handling, module refs, accounting.
 */
#define pr_fmt(fmt) "khttpd: " fmt

#include <linux/kthread.h>
#include <linux/jiffies.h>
#include <linux/sched/signal.h>
#include <linux/tcp.h>

#include "http_parser.h"
#include "http_server.h"

#define CRLF "\r\n"

#define HTTP_RESPONSE_200_CLOSE \
    "HTTP/1.1 200 OK" CRLF \
    "Server: khttpd" CRLF \
    "Content-Type: text/plain" CRLF \
    "Content-Length: 12" CRLF \
    "Connection: close" CRLF \
    CRLF \
    "Hello World!"

#define HTTP_RESPONSE_200_KEEPALIVE \
    "HTTP/1.1 200 OK" CRLF \
    "Server: khttpd" CRLF \
    "Content-Type: text/plain" CRLF \
    "Content-Length: 12" CRLF \
    "Connection: keep-alive" CRLF \
    CRLF \
    "Hello World!"

#define HTTP_RESPONSE_501_CLOSE \
    "HTTP/1.1 501 Not Implemented" CRLF \
    "Server: khttpd" CRLF \
    "Content-Type: text/plain" CRLF \
    "Content-Length: 21" CRLF \
    "Connection: close" CRLF \
    CRLF \
    "501 Not Implemented" CRLF

#define HTTP_RESPONSE_501_KEEPALIVE \
    "HTTP/1.1 501 Not Implemented" CRLF \
    "Server: khttpd" CRLF \
    "Content-Type: text/plain" CRLF \
    "Content-Length: 21" CRLF \
    "Connection: keep-alive" CRLF \
    CRLF \
    "501 Not Implemented" CRLF


struct http_request {
    struct socket *socket;
    enum http_method method;
    char request_url[128];
    int complete;
};

static int http_server_recv(struct socket *sock, char *buf, size_t size)
{
    struct kvec iov = {.iov_base = (void *) buf, .iov_len = size};
    struct msghdr msg = {
        .msg_name = 0,
        .msg_namelen = 0,
        .msg_control = NULL,
        .msg_controllen = 0,
        .msg_flags = 0,
    };
    return kernel_recvmsg(sock, &msg, &iov, 1, size, msg.msg_flags);
}

static int http_server_send(struct socket *sock, const char *buf, size_t size)
{
    struct msghdr msg = {
        .msg_name = NULL,
        .msg_namelen = 0,
        .msg_control = NULL,
        .msg_controllen = 0,
        .msg_flags = 0,
    };
    int done = 0;
    while (done < size) {
        struct kvec iov = {
            .iov_base = (void *) ((char *) buf + done),
            .iov_len = size - done,
        };
        int length = kernel_sendmsg(sock, &msg, &iov, 1, iov.iov_len);
        if (length <= 0) {
            pr_err("write error: %d\n", length);
            break;
        }
        done += length;
    }
    return done;
}

static int http_server_response(struct http_request *request, int keep_alive)
{
    const char *response;

    if (request->method != HTTP_GET)
        response = keep_alive ? HTTP_RESPONSE_501_KEEPALIVE
                              : HTTP_RESPONSE_501_CLOSE;
    else
        response = keep_alive ?
            (extra_crlf ? HTTP_RESPONSE_200_KEEPALIVE "\r\n" : HTTP_RESPONSE_200_KEEPALIVE) :
            (extra_crlf ? HTTP_RESPONSE_200_CLOSE "\r\n" : HTTP_RESPONSE_200_CLOSE);

    if (log_requests)
        pr_info("requested_url = %s\n", request->request_url);
    {
        int sent = http_server_send(request->socket, response, strlen(response));
        if (sent == strlen(response)) {
            this_cpu_inc(legacy_counts.responses);
            this_cpu_add(legacy_counts.send_bytes, sent);
        } else {
            this_cpu_inc(legacy_counts.send_errors);
        }
    }
    return 0;
}

static int http_parser_callback_message_begin(http_parser *parser)
{
    struct http_request *request = parser->data;
    struct socket *socket = request->socket;
    memset(request, 0x00, sizeof(struct http_request));
    request->socket = socket;
    return 0;
}

static int http_parser_callback_request_url(http_parser *parser,
                                            const char *p,
                                            size_t len)
{
    struct http_request *request = parser->data;
    size_t used = strlen(request->request_url);
    if (len >= sizeof(request->request_url) - used)
        return -1;
    memcpy(request->request_url + used, p, len);
    request->request_url[used + len] = '\0';
    return 0;
}

static int http_parser_callback_header_field(http_parser *parser,
                                             const char *p,
                                             size_t len)
{
    return 0;
}

static int http_parser_callback_header_value(http_parser *parser,
                                             const char *p,
                                             size_t len)
{
    return 0;
}

static int http_parser_callback_headers_complete(http_parser *parser)
{
    struct http_request *request = parser->data;
    request->method = parser->method;
    return 0;
}

static int http_parser_callback_body(http_parser *parser,
                                     const char *p,
                                     size_t len)
{
    return 0;
}

static int http_parser_callback_message_complete(http_parser *parser)
{
    struct http_request *request = parser->data;
    http_server_response(request, keep_alive && http_should_keep_alive(parser));
    request->complete = 1;
    return 0;
}

static int http_server_worker(void *arg)
{
    char *buf;
    struct http_parser parser;
    struct http_parser_settings setting = {
        .on_message_begin = http_parser_callback_message_begin,
        .on_url = http_parser_callback_request_url,
        .on_header_field = http_parser_callback_header_field,
        .on_header_value = http_parser_callback_header_value,
        .on_headers_complete = http_parser_callback_headers_complete,
        .on_body = http_parser_callback_body,
        .on_message_complete = http_parser_callback_message_complete,
    };
    struct http_request request;
    struct socket *socket = (struct socket *) arg;
    int err = 0;

    allow_signal(SIGKILL);
    allow_signal(SIGTERM);

    this_cpu_inc(legacy_counts.buffer_alloc_calls);
    buf = use_pool ? mempool_alloc(http_buf_pool, GFP_KERNEL) :
        kzalloc(RECV_BUFFER_SIZE, GFP_KERNEL);
    if (!buf) {
        pr_err("can't allocate memory!\n");
        err = -ENOMEM;
        goto out;
    }

    request.socket = socket;
    http_parser_init(&parser, HTTP_REQUEST);
    parser.data = &request;

    while (!kthread_should_stop()) {
        int ret = http_server_recv(socket, buf, RECV_BUFFER_SIZE);
        if (ret <= 0) {
            if (ret) {
                pr_err("recv error: %d\n", ret);
                err = ret;
            }
            goto out_free_buf;
        }

        if (http_parser_execute(&parser, &setting, buf, ret) != ret)
            goto out_free_buf;

        if (request.complete && !http_should_keep_alive(&parser))
            goto out_free_buf;
        if (clear_buffer)
            memset(buf, 0, RECV_BUFFER_SIZE);
    }
out_free_buf:
    if (use_pool)
        mempool_free(buf, http_buf_pool);
    else
        kfree(buf);
out:
    kernel_sock_shutdown(socket, SHUT_RDWR);
    sock_release(socket);
    module_put_and_kthread_exit(err);
}

int http_server_daemon(void *arg)
{
    struct socket *socket;
    struct task_struct *worker;
    struct http_server_param *param = (struct http_server_param *) arg;

    allow_signal(SIGKILL);
    allow_signal(SIGTERM);

    while (!kthread_should_stop()) {
        int err = kernel_accept(param->listen_socket, &socket, 0);
        if (err < 0) {
            if (signal_pending(current))
                break;
            pr_err("kernel_accept() error: %d\n", err);
            continue;
        }
        /* Longer than wrk's 2s timeout: bounds orphaned idle sockets without
         * shortening the deadline of any measured successful request.
         */
        WRITE_ONCE(socket->sk->sk_rcvtimeo, msecs_to_jiffies(10000));
        WRITE_ONCE(socket->sk->sk_sndtimeo, msecs_to_jiffies(10000));
        this_cpu_inc(legacy_counts.accepted);
        if (!try_module_get(THIS_MODULE)) {
            kernel_sock_shutdown(socket, SHUT_RDWR);
            sock_release(socket);
            break;
        }
        worker = kthread_run(http_server_worker, socket, "kh_legacy_conn");
        if (IS_ERR(worker)) {
            module_put(THIS_MODULE);
            pr_err("can't create more worker process\n");
            kernel_sock_shutdown(socket, SHUT_RDWR);
            sock_release(socket);
            continue;
        }
        this_cpu_inc(legacy_counts.spawned);
    }
    return 0;
}
