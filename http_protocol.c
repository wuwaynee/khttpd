/* SPDX-License-Identifier: MIT */
#include "http_protocol.h"

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

static int token(unsigned char c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9'))
        return 1;
    return c && strchr("!#$%&'*+-.^_~|", c) != NULL ? 1 : c == 0x60;
}

static unsigned char lower(unsigned char c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static int equal(const char *s, size_t n, const char *literal)
{
    size_t i;

    if (n != strlen(literal))
        return 0;
    for (i = 0; i < n; ++i)
        if (lower((unsigned char)s[i]) != (unsigned char)literal[i])
            return 0;
    return 1;
}

static size_t line_end(const char *s, size_t start, size_t n)
{
    size_t i;

    for (i = start; i + 1 < n; ++i)
        if (s[i] == '\r' && s[i + 1] == '\n')
            return i;
    return n;
}

size_t kh_header_end(const char *buf, size_t length, size_t *scan)
{
    size_t i = *scan;

    for (; i + 3 < length; ++i) {
        if (!memcmp(buf + i, "\r\n\r\n", 4)) {
            *scan = i + 4;
            return i + 4;
        }
    }
    *scan = i;
    return 0;
}

/* Deliberately restrictive: no whitespace, list, userinfo or URI in Host.
 * The value is not used for routing. This is not a general authority parser.
 */
static int valid_host(const char *s, size_t n)
{
    size_t i;

    if (!n)
        return 0;
    for (i = 0; i < n; ++i) {
        unsigned char c = s[i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-' ||
              c == ':' || c == '[' || c == ']'))
            return 0;
    }
    return 1;
}

static int connection_tokens(const char *s, size_t n, int *close)
{
    size_t pos = 0;

    if (!n)
        return 0;
    while (pos < n) {
        size_t start, end;

        while (pos < n && (s[pos] == ' ' || s[pos] == '\t'))
            ++pos;
        start = pos;
        while (pos < n && token((unsigned char)s[pos]))
            ++pos;
        end = pos;
        if (end == start)
            return 0;
        if (equal(s + start, end - start, "close"))
            *close = 1;
        /* Do not accept hop-by-hop nominations of framing fields. */
        if (equal(s + start, end - start, "host") ||
            equal(s + start, end - start, "content-length") ||
            equal(s + start, end - start, "transfer-encoding"))
            return 0;
        while (pos < n && (s[pos] == ' ' || s[pos] == '\t'))
            ++pos;
        if (pos == n)
            return 1;
        if (s[pos++] != ',' || pos == n)
            return 0;
    }
    return 1;
}

struct kh_request kh_parse(const char *buf, size_t length)
{
    struct kh_request r = { .status = 400 };
    size_t end, a, b, i, pos, count = 0;
    int http11, get, host = 0, cl = 0, body = 0;
    int te = 0, expect = 0, upgrade = 0, close = 0, route;

    if (length > KH_HEADER_CAP) {
        r.status = 431;
        return r;
    }
    if (length < 4 || memcmp(buf + length - 4, "\r\n\r\n", 4))
        return r;
    end = line_end(buf, 0, length);
    for (a = 0; a < end && token((unsigned char)buf[a]); ++a)
        ;
    if (!a || a == end || buf[a] != ' ')
        return r;
    for (b = a + 1; b < end && buf[b] != ' '; ++b) {
        unsigned char c = buf[b];

        if (c < 0x21 || c > 0x7e || c == '#' || c == '\\')
            return r;
    }
    if (b == a + 1 || b == end || buf[a + 1] != '/')
        return r;
    if (b - a - 1 > KH_TARGET_CAP) {
        r.status = 414;
        return r;
    }
    if (end - b - 1 != 8)
        return r;
    http11 = !memcmp(buf + b + 1, "HTTP/1.1", 8);
    if (!http11 && memcmp(buf + b + 1, "HTTP/1.0", 8)) {
        r.status = !memcmp(buf + b + 1, "HTTP/", 5) ? 505 : 400;
        return r;
    }
    get = a == 3 && !memcmp(buf, "GET", 3);
    r.head = a == 4 && !memcmp(buf, "HEAD", 4);
    route = b - a - 1 == 1;

    pos = end + 2;
    while (pos < length - 2) {
        size_t colon, start, stop;

        if (++count > KH_HEADER_COUNT) {
            r.status = 431;
            return r;
        }
        end = line_end(buf, pos, length);
        if (end == length)
            return r;
        for (colon = pos; colon < end &&
             token((unsigned char)buf[colon]); ++colon)
            ;
        if (colon == pos || colon == end || buf[colon] != ':')
            return r;
        start = colon + 1;
        stop = end;
        for (i = start; i < stop; ++i) {
            unsigned char c = buf[i];

            if ((c < 0x20 && c != '\t') || c > 0x7e)
                return r;
        }
        while (start < stop && (buf[start] == ' ' || buf[start] == '\t'))
            ++start;
        while (stop > start && (buf[stop - 1] == ' ' || buf[stop - 1] == '\t'))
            --stop;

        if (equal(buf + pos, colon - pos, "host")) {
            if (host++ || !valid_host(buf + start, stop - start))
                return r;
        } else if (equal(buf + pos, colon - pos, "content-length")) {
            if (cl++ || start == stop || stop - start > 20)
                return r;
            /* No integer conversion: any nonzero decimal body is rejected. */
            for (i = start; i < stop; ++i) {
                if (buf[i] < '0' || buf[i] > '9')
                    return r;
                body |= buf[i] != '0';
            }
        } else if (equal(buf + pos, colon - pos, "transfer-encoding")) {
            te = 1;
        } else if (equal(buf + pos, colon - pos, "expect")) {
            expect = 1;
        } else if (equal(buf + pos, colon - pos, "upgrade")) {
            upgrade = 1;
        } else if (equal(buf + pos, colon - pos, "connection")) {
            if (!connection_tokens(buf + start, stop - start, &close))
                return r;
        }
        pos = end + 2;
    }
    if (pos != length - 2 || (http11 && !host) || (te && cl))
        return r;
    if (te || upgrade)
        r.status = 501;
    else if (body)
        r.status = 413;
    else if (expect)
        r.status = 417;
    else if (!get && !r.head)
        r.status = 405;
    else if (!route)
        r.status = 404;
    else {
        r.status = 200;
        r.keep_alive = http11 && !close;
    }
    return r;
}

#define OK_PREFIX "HTTP/1.1 200 OK\r\nServer: khttpd\r\n" \
    "Content-Type: text/plain\r\nContent-Length: 12\r\n"
#define KEEP OK_PREFIX "Connection: keep-alive\r\n\r\n"
#define CLOSE OK_PREFIX "Connection: close\r\n\r\n"
#define ERROR(code, text) "HTTP/1.1 " #code " " text \
    "\r\nServer: khttpd\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"

struct kh_response kh_response(int status, int keep_alive, int head)
{
    const char *p;

    switch (status) {
    case 200:
        if (head)
            p = keep_alive ? KEEP : CLOSE;
        else
            p = keep_alive ? KEEP "Hello World!" : CLOSE "Hello World!";
        break;
    case 404: p = ERROR(404, "Not Found"); break;
    case 405:
        p = "HTTP/1.1 405 Method Not Allowed\r\nServer: khttpd\r\n"
            "Allow: GET, HEAD\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        break;
    case 408: p = ERROR(408, "Request Timeout"); break;
    case 413: p = ERROR(413, "Content Too Large"); break;
    case 414: p = ERROR(414, "URI Too Long"); break;
    case 417: p = ERROR(417, "Expectation Failed"); break;
    case 431: p = ERROR(431, "Request Header Fields Too Large"); break;
    case 501: p = ERROR(501, "Not Implemented"); break;
    case 505: p = ERROR(505, "HTTP Version Not Supported"); break;
    default: p = ERROR(400, "Bad Request"); break;
    }
    return (struct kh_response){ .data = p, .length = strlen(p) };
}
