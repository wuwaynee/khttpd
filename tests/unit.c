/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../http_connection.h"

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); \
} } while (0)

static void parse_case(const char *input, int status, int keep, int head)
{
    struct kh_request r = kh_parse(input, strlen(input));

    CHECK(r.status == status);
    CHECK(r.keep_alive == keep);
    CHECK(r.head == head);
}

struct fake {
    const char *input;
    size_t length, pos, chunk, send_chunk;
    char output[32768];
    size_t written;
    unsigned long long now;
    unsigned int drip;
    int send_mode, idle, stopped;
};

static int receive(void *arg, char *buffer, size_t size)
{
    struct fake *f = arg;
    size_t n = f->length - f->pos;

    f->now += f->drip;
    if (!n) {
        f->now += 100;
        return f->idle ? KH_IO_RETRY : 0;
    }
    if (n > size)
        n = size;
    if (n > f->chunk)
        n = f->chunk;
    memcpy(buffer, f->input + f->pos, n);
    f->pos += n;
    return (int)n;
}

static int transmit(void *arg, const char *buffer, size_t size)
{
    struct fake *f = arg;
    size_t n = size;

    if (f->send_mode == 1)
        return 0;
    if (f->send_mode == 2)
        return KH_IO_ERROR;
    if (f->send_mode == 3) {
        f->now += 100;
        return KH_IO_RETRY;
    }
    if (n > f->send_chunk)
        n = f->send_chunk;
    CHECK(f->written + n < sizeof(f->output));
    memcpy(f->output + f->written, buffer, n);
    f->written += n;
    f->output[f->written] = 0;
    return (int)n;
}

static unsigned long long now_ms(void *arg)
{
    return ((struct fake *)arg)->now;
}

static int stopped(void *arg)
{
    return ((struct fake *)arg)->stopped;
}

static struct kh_stats run(struct fake *f, unsigned int max, int keep)
{
    char buffer[KH_HEADER_CAP];
    struct kh_io io = {
        .context = f, .recv = receive, .send = transmit,
        .now_ms = now_ms, .stopped = stopped,
        .timeout_ms = 500, .max_requests = max, .keep_alive = keep,
    };

    return kh_serve(&io, buffer);
}

static struct fake input(const char *s)
{
    return (struct fake){
        .input = s, .length = strlen(s), .chunk = 8192, .send_chunk = 8192,
    };
}

static size_t occurrences(const char *s, const char *needle)
{
    size_t n = 0;
    const char *p = s;

    while ((p = strstr(p, needle))) {
        ++n;
        p += strlen(needle);
    }
    return n;
}

int main(void)
{
    const char *get = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    char large[KH_HEADER_CAP + 2], binary[100];
    struct fake f;
    struct kh_stats stats;
    size_t i, scan = 0;
    unsigned int random = 12345;

    parse_case(get, 200, 1, 0);
    parse_case("HEAD / HTTP/1.1\r\nhOsT: localhost:8081\r\n\r\n", 200, 1, 1);
    parse_case("GET / HTTP/1.0\r\n\r\n", 200, 0, 0);
    parse_case("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n", 200, 0, 0);
    parse_case("GET / HTTP/1.1\r\nHost: [::1]\r\n"
               "Connection: keep-alive, CLOSE\r\n\r\n", 200, 0, 0);
    parse_case("GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 000\r\n\r\n", 200, 1, 0);
    parse_case("POST / HTTP/1.1\r\nHost: a\r\n\r\n", 405, 0, 0);
    parse_case("get / HTTP/1.1\r\nHost: a\r\n\r\n", 405, 0, 0);
    parse_case("GET /other HTTP/1.1\r\nHost: a\r\n\r\n", 404, 0, 0);
    parse_case("GET /?x=1 HTTP/1.1\r\nHost: a\r\n\r\n", 404, 0, 0);
    parse_case("GET / HTTP/2.0\r\nHost: a\r\n\r\n", 505, 0, 0);
    parse_case("GET / HTTP/1.1\r\n\r\n", 400, 0, 0);
    parse_case("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", 400, 0, 0);
    parse_case("GET / HTTP/1.1\r\nHost: a,b\r\n\r\n", 400, 0, 0);
    parse_case("GET / HTTP/1.1\r\nHost : a\r\n\r\n", 400, 0, 0);
    parse_case("GET / HTTP/1.1\r\nHost: a\r\n folded\r\n\r\n", 400, 0, 0);
    parse_case("GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 0\r\n"
               "Content-Length: 0\r\n\r\n", 400, 0, 0);
    parse_case("GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 0,0\r\n\r\n", 400, 0, 0);
    parse_case("GET / HTTP/1.1\r\nHost: a\r\nContent-Length: +0\r\n\r\n", 400, 0, 0);
    parse_case("GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 1\r\n\r\n", 413, 0, 0);
    parse_case("GET / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n", 501, 0, 0);
    parse_case("GET / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n"
               "Content-Length: 0\r\n\r\n", 400, 0, 0);
    parse_case("GET / HTTP/1.1\r\nHost: a\r\nExpect: 100-continue\r\n\r\n", 417, 0, 0);
    parse_case("GET / HTTP/1.1\r\nHost: a\r\nUpgrade: websocket\r\n\r\n", 501, 0, 0);
    parse_case("GET / HTTP/1.1\r\nHost: a\r\nConnection: close,\r\n\r\n", 400, 0, 0);
    parse_case("GET / HTTP/1.1\r\nHost: a\r\nConnection: host\r\n\r\n", 400, 0, 0);
    parse_case("GET  / HTTP/1.1\r\nHost: a\r\n\r\n", 400, 0, 0);
    parse_case("GET /#fragment HTTP/1.1\r\nHost: a\r\n\r\n", 400, 0, 0);
    parse_case("GET / HTTP/1.1\nHost: a\r\n\r\n", 400, 0, 0);
    memcpy(binary, get, strlen(get) + 1);
    binary[20] = 0;
    CHECK(kh_parse(binary, strlen(get)).status == 400);

    memset(large, 'a', sizeof(large));
    memcpy(large, "GET /", 5);
    memcpy(large + 5 + KH_TARGET_CAP, " HTTP/1.1\r\nHost: a\r\n\r\n", 22);
    CHECK(kh_parse(large, 5 + KH_TARGET_CAP + 22).status == 414);
    CHECK(kh_parse(large, KH_HEADER_CAP + 1).status == 431);
    strcpy(large, "GET / HTTP/1.1\r\nHost: a\r\n");
    for (i = 0; i < KH_HEADER_COUNT; ++i)
        strcat(large, "X: a\r\n");
    strcat(large, "\r\n");
    CHECK(kh_parse(large, strlen(large)).status == 431);

    for (i = 0; i < strlen(get); ++i)
        CHECK(kh_header_end(get, i, &scan) == 0);
    CHECK(kh_header_end(get, strlen(get), &scan) == strlen(get));
    for (i = 1; i <= strlen(get); ++i) {
        f = input(get);
        f.chunk = i;
        f.send_chunk = 1; /* Every possible read split, byte-at-a-time writes. */
        stats = run(&f, 1024, 1);
        CHECK(stats.ok == 1 && stats.io_errors == 0);
        CHECK(strstr(f.output, "Content-Length: 12\r\n"));
        CHECK(!strcmp(f.output + f.written - 12, "Hello World!"));
    }
    f = input("GET / HTTP/1.1\r\nHost: a\r\n\r\n"
              "HEAD / HTTP/1.1\r\nHost: a\r\n\r\n"
              "GET / HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n");
    stats = run(&f, 1024, 1);
    CHECK(stats.ok == 3 && occurrences(f.output, "HTTP/1.1 200") == 3);
    CHECK(occurrences(f.output, "Hello World!") == 2);
    f = input("GET / HTTP/1.1\r\nHost: a\r\n\r\n"
              "GET / HTTP/1.1\r\nHost: a\r\n\r\n");
    stats = run(&f, 1, 1);
    CHECK(stats.ok == 1 && occurrences(f.output, "HTTP/1.1 200") == 1);
    CHECK(strstr(f.output, "Connection: close"));
    f = input(get);
    stats = run(&f, 1024, 0);
    CHECK(stats.ok == 1 && strstr(f.output, "Connection: close"));
    for (i = 1; i <= 3; ++i) {
        f = input(get);
        f.send_mode = (int)i;
        stats = run(&f, 1024, 1);
        CHECK(stats.io_errors == 1 && stats.ok == 0);
    }
    f = input("GET / HTTP/1.1\r\n");
    f.idle = 1;
    stats = run(&f, 1024, 1);
    CHECK(stats.timeouts == 1 && strstr(f.output, "408 Request Timeout"));
    f = input(get);
    f.chunk = 1;
    f.drip = 100;
    stats = run(&f, 1024, 1);
    CHECK(stats.timeouts == 1 && f.pos < f.length);
    f = input(get);
    f.stopped = 1;
    stats = run(&f, 1024, 1);
    CHECK(stats.responses == 0 && f.pos == 0);
    memset(large, 'a', KH_HEADER_CAP);
    large[KH_HEADER_CAP] = 0;
    f = input(large);
    stats = run(&f, 1024, 1);
    CHECK(stats.protocol_errors == 1 && strstr(f.output, "431 "));
    /* Deterministic invalid-input exploration under ASan/UBSan. */
    for (i = 0; i < 20000; ++i) {
        size_t j, n = i % sizeof(binary);

        for (j = 0; j < n; ++j) {
            random = random * 1664525U + 1013904223U;
            binary[j] = (char)(random >> 24);
        }
        (void)kh_parse(binary, n);
        scan = 0;
        (void)kh_header_end(binary, n, &scan);
    }
    puts("unit: protocol, framing, stream, partial I/O, deadlines PASS");
    return 0;
}
