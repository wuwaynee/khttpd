/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #x, errno); \
    exit(1); } } while (0)
#define REQUEST "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"

static atomic_bool stopping;
static atomic_ulong connections, replies;

static void pause_ms(long milliseconds)
{
    struct timespec delay = { .tv_sec = milliseconds / 1000,
                              .tv_nsec = milliseconds % 1000 * 1000000 };

    while (nanosleep(&delay, &delay) && errno == EINTR)
        ;
}

static int connect_server(void)
{
    struct sockaddr_in address = {
        .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        .sin_port = htons(8081),
    };
    struct timeval timeout = { .tv_usec = 20000 };
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    CHECK(fd >= 0);
    CHECK(!setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    CHECK(!setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)));
    if (connect(fd, (struct sockaddr *)&address, sizeof(address))) {
        close(fd);
        return -1;
    }
    return fd;
}

static int command(char *const argv[])
{
    int status;
    pid_t child = fork(), waited;

    CHECK(child >= 0);
    if (!child) {
        execv(argv[0], argv);
        _exit(127);
    }
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    CHECK(waited == child && WIFEXITED(status) && WEXITSTATUS(status) != 127);
    return WEXITSTATUS(status);
}

static int legacy_tasks(void)
{
    DIR *directory = opendir("/proc");
    struct dirent *entry;
    int found = 0;

    CHECK(directory);
    while ((entry = readdir(directory))) {
        char path[320], name[64];
        FILE *file;

        if (entry->d_name[0] < '0' || entry->d_name[0] > '9')
            continue;
        CHECK(snprintf(path, sizeof(path), "/proc/%s/comm", entry->d_name) > 0);
        file = fopen(path, "r");
        if (!file)
            continue;
        if (fgets(name, sizeof(name), file) && !strncmp(name, "kh_legacy_", 10))
            found = 1;
        fclose(file);
    }
    closedir(directory);
    return found;
}

static void assert_removed(void)
{
    unsigned int i;

    CHECK(access("/sys/module/khttpd_legacy", F_OK) < 0 && errno == ENOENT);
    for (i = 0; i < 100 && legacy_tasks(); ++i)
        pause_ms(10);
    CHECK(!legacy_tasks());
}

static void smoke(void)
{
    char *load[] = { "/bin/insmod", "/khttpd_legacy.ko", "shards=4",
                     "log_requests=0", "fail_start_after=-1", NULL };
    char *remove[] = { "/bin/rmmod", "khttpd_legacy", NULL };
    char response[512] = { 0 };
    struct timeval timeout = { .tv_sec = 2 };
    size_t used = 0;
    int fd;
    unsigned int attempt;

    CHECK(command(load) == 0);
    fd = connect_server();
    CHECK(fd >= 0);
    CHECK(!setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    CHECK(!setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)));
    CHECK(send(fd, REQUEST, sizeof(REQUEST) - 1, MSG_NOSIGNAL) == sizeof(REQUEST) - 1);
    while (!strstr(response, "Hello World!")) {
        ssize_t n;

        CHECK(used < sizeof(response) - 1);
        n = recv(fd, response + used, sizeof(response) - used - 1, 0);
        CHECK(n > 0);
        used += (size_t)n;
        response[used] = 0;
    }
    CHECK(!strncmp(response, "HTTP/1.1 200 OK\r\n", 17));
    close(fd);
    for (attempt = 0; attempt < 100; ++attempt) {
        if (command(remove) == 0)
            break;
        pause_ms(10);
    }
    CHECK(attempt < 100);
    assert_removed();
}

static void *traffic(void *unused)
{
    (void)unused;
    while (!atomic_load(&stopping)) {
        int fd = connect_server();

        if (fd >= 0) {
            char response[128];

            atomic_fetch_add(&connections, 1);
            if (send(fd, REQUEST, sizeof(REQUEST) - 1, MSG_NOSIGNAL) > 0 &&
                recv(fd, response, sizeof(response), 0) > 0)
                atomic_fetch_add(&replies, 1);
            close(fd);
        } else {
            pause_ms(1);
        }
    }
    return NULL;
}

int main(void)
{
    const unsigned int failures[] = { 0, 1, 8, 16 };
    pthread_t clients[4];
    unsigned int i, repetition;

    smoke();
    for (i = 0; i < 4; ++i)
        CHECK(!pthread_create(&clients[i], NULL, traffic, NULL));
    for (repetition = 0; repetition < 3; ++repetition) {
        for (i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
            char parameter[40];
            char *load[] = { "/bin/insmod", "/khttpd_legacy.ko", "shards=16",
                             "log_requests=0", parameter, NULL };

            CHECK(snprintf(parameter, sizeof(parameter), "fail_start_after=%u", failures[i]) > 0);
            CHECK(command(load) != 0);
            assert_removed();
        }
    }
    atomic_store(&stopping, 1);
    for (i = 0; i < 4; ++i)
        CHECK(!pthread_join(clients[i], NULL));
    CHECK(atomic_load(&connections) > 0);
    CHECK(atomic_load(&replies) == 0);
    assert_removed();
    printf("legacy startup: 12 injected failures, %lu connections, zero responses PASS\n",
           atomic_load(&connections));
    smoke();
    return 0;
}
