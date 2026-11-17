/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #x, errno); \
    exit(1); } } while (0)
#define GET "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"
#define CLOSE "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"

struct connection {
    int fd;
    size_t used;
    char data[32768];
};
static unsigned short port;

static struct connection connect_server(void)
{
    struct sockaddr_in addr = {
        .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        .sin_port = htons(port),
    };
    struct timeval timeout = { .tv_sec = 10 };
    struct connection c = { .fd = socket(AF_INET, SOCK_STREAM, 0) };
    int one = 1;

    CHECK(c.fd >= 0);
    CHECK(!setsockopt(c.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    CHECK(!setsockopt(c.fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)));
    CHECK(!setsockopt(c.fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)));
    CHECK(!connect(c.fd, (struct sockaddr *)&addr, sizeof(addr)));
    return c;
}

static void write_all(int fd, const char *s, size_t length)
{
    while (length) {
        ssize_t n = send(fd, s, length, MSG_NOSIGNAL);

        if (n < 0 && errno == EINTR)
            continue;
        CHECK(n > 0);
        s += n;
        length -= (size_t)n;
    }
}

static void response(struct connection *c, int status, int head)
{
    size_t needed = 0;

    for (;;) {
        char *end, *cl;

        c->data[c->used] = 0;
        end = strstr(c->data, "\r\n\r\n");
        if (end) {
            unsigned int length;
            int got;

            CHECK(sscanf(c->data, "HTTP/1.1 %d", &got) == 1 && got == status);
            cl = strstr(c->data, "\r\nContent-Length: ");
            CHECK(cl && cl < end);
            CHECK(sscanf(cl, "\r\nContent-Length: %u", &length) == 1);
            CHECK(length == (status == 200 ? 12U : 0U));
            needed = (size_t)(end + 4 - c->data) + (head ? 0 : length);
            if (c->used >= needed) {
                if (!head && length)
                    CHECK(!memcmp(end + 4, "Hello World!", 12));
                memmove(c->data, c->data + needed, c->used - needed);
                c->used -= needed;
                return;
            }
        }
        CHECK(c->used < sizeof(c->data) - 1);
        {
            ssize_t n = recv(c->fd, c->data + c->used,
                             sizeof(c->data) - c->used - 1, 0);

            if (n < 0 && errno == EINTR)
                continue;
            CHECK(n > 0);
            c->used += (size_t)n;
        }
    }
}

static void eof(struct connection *c)
{
    char byte;
    ssize_t n;

    CHECK(c->used == 0);
    do {
        n = recv(c->fd, &byte, 1, 0);
    } while (n < 0 && errno == EINTR);
    CHECK(n == 0 || (n < 0 && errno == ECONNRESET));
    close(c->fd);
}

static void single(const char *request, int status, int head)
{
    struct connection c = connect_server();

    write_all(c.fd, request, strlen(request));
    response(&c, status, head);
    eof(&c);
}

static void *concurrent(void *unused)
{
    struct connection c = connect_server();
    unsigned int i;

    (void)unused;
    for (i = 0; i < 20; ++i) {
        write_all(c.fd, i == 19 ? CLOSE : GET, i == 19 ? strlen(CLOSE) : strlen(GET));
        response(&c, 200, 0);
    }
    eof(&c);
    return NULL;
}

int main(int argc, char **argv)
{
    struct connection c;
    pthread_t threads[12];
    char large[9000];
    size_t i;

    CHECK(argc == 2 || argc == 3);
    {
        char *end;
        long value = strtol(argv[1], &end, 10);

        CHECK(*argv[1] && !*end && value > 0 && value <= 65535);
        port = (unsigned short)value;
    }
    signal(SIGPIPE, SIG_IGN);
    if (argc == 3 && !strcmp(argv[2], "hold")) {
        struct connection held[8];

        for (i = 0; i < 8; ++i) {
            held[i] = connect_server();
            write_all(held[i].fd, "GET", 3);
        }
        puts("HOLD_READY");
        fflush(stdout);
        for (i = 0; i < 8; ++i)
            eof(&held[i]);
        puts("hold: all sockets closed");
        return 0;
    }
    if (argc == 3 && !strcmp(argv[2], "deadline")) {
        struct timespec pause = { .tv_nsec = 100000000 };
        int observed = 0;

        /* Used with a 2s server deadline by the automated adapters. */
        c = connect_server();
        write_all(c.fd, "GET", 3);
        response(&c, 408, 0);
        eof(&c);
        c = connect_server();
        eof(&c); /* An idle connection also expires. */
        c = connect_server();
        for (i = 0; i < 40; ++i) {
            char byte;
            ssize_t n = recv(c.fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);

            if (n > 0) {
                observed = 1;
                break;
            }
            CHECK(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
            write_all(c.fd, i == 0 ? "G" : " ", 1);
            nanosleep(&pause, NULL);
        }
        CHECK(observed); /* Dripping bytes must not renew the header deadline. */
        response(&c, 408, 0);
        eof(&c);
        puts("deadline: partial, idle, slow-drip PASS");
        return 0;
    }
    if (argc == 3 && !strcmp(argv[2], "limit")) {
        c = connect_server();
        write_all(c.fd, GET GET GET, strlen(GET GET GET));
        response(&c, 200, 0);
        response(&c, 200, 0);
        eof(&c);
        puts("limit: PASS");
        return 0;
    }
    CHECK(argc == 2);
    single(CLOSE, 200, 0);
    single("HEAD / HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n", 200, 1);
    single("GET / HTTP/1.0\r\n\r\n", 200, 0);
    single("GET /bad HTTP/1.1\r\nHost: a\r\n\r\n", 404, 0);
    single("POST / HTTP/1.1\r\nHost: a\r\n\r\n", 405, 0);
    single("GET / HTTP/2.0\r\nHost: a\r\n\r\n", 505, 0);
    single("GET / HTTP/1.1\r\n\r\n", 400, 0);
    single("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", 400, 0);
    single("GET / HTTP/1.1\r\nHost : a\r\n\r\n", 400, 0);
    single("GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 0\r\n"
           "Content-Length: 0\r\n\r\n", 400, 0);
    single("GET / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n"
           "Content-Length: 1\r\n\r\nX" GET, 400, 0);
    single("GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 1\r\n\r\nX" GET, 413, 0);
    single("GET / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n", 501, 0);
    single("GET / HTTP/1.1\r\nHost: a\r\nExpect: 100-continue\r\n\r\n", 417, 0);
    memset(large, 'a', 8192);
    large[8192] = 0;
    single(large, 431, 0);
    memcpy(large, "GET /", 5);
    memset(large + 5, 'a', 2500);
    strcpy(large + 2505, " HTTP/1.1\r\nHost: a\r\n\r\n");
    single(large, 414, 0);

    c = connect_server();
    for (i = 0; i < strlen(CLOSE); ++i)
        write_all(c.fd, CLOSE + i, 1);
    response(&c, 200, 0);
    eof(&c);
    c = connect_server();
    write_all(c.fd, GET "HEAD / HTTP/1.1\r\nHost: a\r\n\r\n" CLOSE,
              strlen(GET "HEAD / HTTP/1.1\r\nHost: a\r\n\r\n" CLOSE));
    response(&c, 200, 0);
    response(&c, 200, 1);
    response(&c, 200, 0);
    eof(&c);
    c = connect_server();
    write_all(c.fd, CLOSE GET, strlen(CLOSE GET));
    response(&c, 200, 0);
    eof(&c);
    c = connect_server();
    write_all(c.fd, GET, strlen(GET));
    CHECK(!shutdown(c.fd, SHUT_WR));
    response(&c, 200, 0);
    eof(&c);
    for (i = 0; i < 12; ++i)
        CHECK(!pthread_create(&threads[i], NULL, concurrent, NULL));
    for (i = 0; i < 12; ++i)
        CHECK(!pthread_join(threads[i], NULL));
    puts("integration: framing, stream, errors, half-close, concurrency PASS");
    return 0;
}
