/* SPDX-License-Identifier: MIT */
#include "http_connection.h"

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

static int send_all(struct kh_io *io, struct kh_response response)
{
    size_t sent = 0;
    unsigned long long deadline = io->now_ms(io->context) + io->timeout_ms;

    while (sent < response.length) {
        int n;

        if (io->stopped(io->context) ||
            io->now_ms(io->context) >= deadline)
            return 0;
        n = io->send(io->context, response.data + sent, response.length - sent);
        if (n == KH_IO_RETRY)
            continue;
        if (n <= 0 || (size_t)n > response.length - sent)
            return 0;
        sent += n;
    }
    return 1;
}

struct kh_stats kh_serve(struct kh_io *io, char *buffer)
{
    struct kh_stats stats = { 0 };
    size_t used = 0, scan = 0;
    unsigned int requests = 0;
    unsigned long long deadline = io->now_ms(io->context) + io->timeout_ms;

    while (!io->stopped(io->context)) {
        struct kh_request request;
        size_t end = kh_header_end(buffer, used, &scan);
        int keep = 0, n;

        if (io->now_ms(io->context) >= deadline) {
            ++stats.timeouts;
            if (used && !send_all(io, kh_response(408, 0, 0)))
                ++stats.io_errors;
            break;
        }
        if (end || used == KH_HEADER_CAP) {
            request = end ? kh_parse(buffer, end) :
                (struct kh_request){ .status = 431 };
            ++requests;
            keep = request.keep_alive && io->keep_alive &&
                requests < io->max_requests;
            if (request.status != 200)
                ++stats.protocol_errors;
            if (!send_all(io, kh_response(request.status, keep, request.head))) {
                ++stats.io_errors;
                break;
            }
            ++stats.responses;
            stats.ok += request.status == 200;
            if (!keep)
                break;
            memmove(buffer, buffer + end, used - end);
            used -= end;
            scan = 0;
            deadline = io->now_ms(io->context) + io->timeout_ms;
            continue;
        }
        n = io->recv(io->context, buffer + used, KH_HEADER_CAP - used);
        if (n == KH_IO_RETRY)
            continue;
        if (n < 0 || (size_t)n > KH_HEADER_CAP - used) {
            ++stats.io_errors;
            break;
        }
        if (!n)
            break;
        used += n;
    }
    return stats;
}
