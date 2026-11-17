/* SPDX-License-Identifier: MIT
 * Linux userspace control: same parser, connection engine and worker model.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "../http_connection.h"

struct worker {
    pthread_t thread;
    pthread_mutex_t lock;
    int listener;
    int client;
    char *buffer;
};

static atomic_bool stop = false;
static unsigned int timeout_ms = 5000, max_requests = 1024;
static int keep_alive = 1, prealloc = 1;

static unsigned long long clock_ms(void *context)
{
    struct timespec now;

    (void)context;
    if (clock_gettime(CLOCK_MONOTONIC, &now))
        abort();
    return (unsigned long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int stopping(void *context)
{
    (void)context;
    return atomic_load(&stop);
}

static int result(int n)
{
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        return KH_IO_RETRY;
    return n < 0 ? KH_IO_ERROR : n;
}

static int receive(void *context, char *buffer, size_t length)
{
    return result((int)recv(*(int *)context, buffer, length, 0));
}

static int transmit(void *context, const char *buffer, size_t length)
{
    return result((int)send(*(int *)context, buffer, length, MSG_NOSIGNAL));
}

static void *run(void *arg)
{
    struct worker *worker = arg;

    while (!atomic_load(&stop)) {
        struct timeval timeout = { .tv_usec = 250000 };
        struct kh_io io;
        char *buffer;
        int one = 1, fd = accept4(worker->listener, NULL, NULL, SOCK_CLOEXEC);

        if (fd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR &&
                !atomic_load(&stop))
                perror("accept");
            continue;
        }
        pthread_mutex_lock(&worker->lock);
        if (atomic_load(&stop)) {
            pthread_mutex_unlock(&worker->lock);
            close(fd);
            break;
        }
        worker->client = fd;
        pthread_mutex_unlock(&worker->lock);
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) ||
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one))) {
            perror("client setsockopt");
        } else {
            buffer = prealloc ? worker->buffer : malloc(KH_HEADER_CAP);
            if (buffer) {
                io = (struct kh_io){
                    .context = &fd, .recv = receive, .send = transmit,
                    .now_ms = clock_ms, .stopped = stopping,
                    .timeout_ms = timeout_ms, .max_requests = max_requests,
                    .keep_alive = keep_alive,
                };
                (void)kh_serve(&io, buffer);
                if (!prealloc)
                    free(buffer);
            }
        }
        pthread_mutex_lock(&worker->lock);
        worker->client = -1;
        pthread_mutex_unlock(&worker->lock);
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
    return NULL;
}

static unsigned int number(const char *s, unsigned int min, unsigned int max)
{
    char *end;
    unsigned long n;

    errno = 0;
    n = strtoul(s, &end, 10);
    if (!*s || *end || errno || n < min || n > max) {
        fprintf(stderr, "invalid number: %s\n", s);
        exit(2);
    }
    return (unsigned int)n;
}

int main(int argc, char **argv)
{
    struct sockaddr_in addr = { .sin_family = AF_INET };
    struct timeval timeout = { .tv_usec = 250000 };
    struct worker *pool;
    sigset_t signals;
    unsigned int workers, shards, i, started = 0;
    int *listeners, signal_number, one = 1, rc = 1;

    if (argc != 8) {
        fprintf(stderr, "usage: %s PORT WORKERS SHARDS KEEPALIVE PREALLOC "
                "TIMEOUT_MS MAX_REQUESTS\n", argv[0]);
        return 2;
    }
    addr.sin_port = htons(number(argv[1], 1, 65535));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    workers = number(argv[2], 1, 512);
    shards = number(argv[3], 1, workers < 64 ? workers : 64);
    keep_alive = number(argv[4], 0, 1);
    prealloc = number(argv[5], 0, 1);
    timeout_ms = number(argv[6], 100, 60000);
    max_requests = number(argv[7], 1, 1000000);
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &signals, NULL))
        return 1;
    listeners = malloc(shards * sizeof(*listeners));
    pool = calloc(workers, sizeof(*pool));
    if (!listeners || !pool) {
        free(listeners);
        free(pool);
        return 1;
    }
    for (i = 0; i < shards; ++i)
        listeners[i] = -1;
    for (i = 0; i < workers; ++i) {
        if (pthread_mutex_init(&pool[i].lock, NULL))
            abort();
        pool[i].client = -1;
    }
    for (i = 0; i < shards; ++i) {
        listeners[i] = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listeners[i] < 0 ||
            setsockopt(listeners[i], SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) ||
            (shards > 1 && setsockopt(listeners[i], SOL_SOCKET, SO_REUSEPORT,
                                     &one, sizeof(one))) ||
            setsockopt(listeners[i], SOL_SOCKET, SO_RCVTIMEO,
                       &timeout, sizeof(timeout)) ||
            bind(listeners[i], (struct sockaddr *)&addr, sizeof(addr)) ||
            listen(listeners[i], 512)) {
            perror("listener");
            goto out;
        }
    }
    for (i = 0; i < workers; ++i) {
        int err;

        pool[i].listener = listeners[i % shards];
        if (prealloc) {
            pool[i].buffer = malloc(KH_HEADER_CAP);
            if (!pool[i].buffer)
                goto out;
        }
        err = pthread_create(&pool[i].thread, NULL, run, &pool[i]);
        if (err) {
            fprintf(stderr, "pthread_create: %s\n", strerror(err));
            goto out;
        }
        ++started;
    }
    printf("ready port=%s workers=%u shards=%u\n", argv[1], workers, shards);
    fflush(stdout);
    if (!sigwait(&signals, &signal_number))
        rc = 0;
out:
    atomic_store(&stop, true);
    for (i = 0; i < shards; ++i)
        if (listeners[i] >= 0)
            shutdown(listeners[i], SHUT_RDWR);
    for (i = 0; i < workers; ++i) {
        pthread_mutex_lock(&pool[i].lock);
        if (pool[i].client >= 0)
            shutdown(pool[i].client, SHUT_RDWR);
        pthread_mutex_unlock(&pool[i].lock);
    }
    for (i = 0; i < started; ++i)
        pthread_join(pool[i].thread, NULL);
    for (i = 0; i < workers; ++i) {
        free(pool[i].buffer);
        pthread_mutex_destroy(&pool[i].lock);
    }
    for (i = 0; i < shards; ++i)
        if (listeners[i] >= 0)
            close(listeners[i]);
    free(pool);
    free(listeners);
    return rc;
}
