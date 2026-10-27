/*
 * tcp-kv-server: a multithreaded in-memory key-value server over TCP.
 *
 * Protocol (one command per line, '\n'-terminated):
 *   SET <key> <value>   -> OK
 *   GET <key>           -> VALUE <value> | NOT_FOUND
 *   DEL <key>           -> DELETED | NOT_FOUND
 *   PING                -> PONG
 *   QUIT                -> BYE (server closes the connection)
 *   anything else       -> ERR <reason>
 */
#define _GNU_SOURCE
#include "kvstore.h"
#include "threadpool.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define MAX_LINE   8192
#define MAX_KEY    256
#define MAX_VALUE  4096

/*
 * Shutdown flag, written by the signal handler and read by every worker.
 * `volatile sig_atomic_t` is only safe within a single thread, so sharing it
 * across threads is a data race (ThreadSanitizer flagged it). A lock-free C11
 * atomic is both signal-safe and thread-safe.
 */
static atomic_int running = 1;

static void on_signal(int sig)
{
    (void)sig;
    int saved_errno = errno; /* handlers must not clobber errno for the code they interrupt */
    atomic_store(&running, 0);
    errno = saved_errno;
}

/* send() may write fewer bytes than asked, so loop until everything is out. */
static int send_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

static int reply(int fd, const char *msg)
{
    return send_all(fd, msg, strlen(msg));
}

/* Handle one command line. Returns 1 to keep the connection, 0 to close it. */
static int handle_command(kvstore_t *kv, int fd, char *line)
{
    char *save = NULL;
    char *cmd  = strtok_r(line, " ", &save);
    if (!cmd)
        return reply(fd, "ERR empty command\n") == 0;

    if (strcmp(cmd, "SET") == 0) {
        char *key   = strtok_r(NULL, " ", &save);
        char *value = save; /* the rest of the line, so values may contain spaces */
        if (!key || !value || *value == '\0')
            return reply(fd, "ERR usage: SET <key> <value>\n") == 0;
        if (strlen(key) > MAX_KEY || strlen(value) > MAX_VALUE)
            return reply(fd, "ERR key or value too long\n") == 0;
        if (kv_set(kv, key, value) != 0)
            return reply(fd, "ERR out of memory\n") == 0;
        return reply(fd, "OK\n") == 0;
    }

    if (strcmp(cmd, "GET") == 0) {
        char *key = strtok_r(NULL, " ", &save);
        if (!key)
            return reply(fd, "ERR usage: GET <key>\n") == 0;
        char value[MAX_VALUE + 1];
        if (kv_get(kv, key, value, sizeof(value)) != 0)
            return reply(fd, "NOT_FOUND\n") == 0;
        char out[MAX_VALUE + 16];
        int n = snprintf(out, sizeof(out), "VALUE %s\n", value);
        return send_all(fd, out, (size_t)n) == 0;
    }

    if (strcmp(cmd, "DEL") == 0) {
        char *key = strtok_r(NULL, " ", &save);
        if (!key)
            return reply(fd, "ERR usage: DEL <key>\n") == 0;
        return reply(fd, kv_del(kv, key) == 0 ? "DELETED\n" : "NOT_FOUND\n") == 0;
    }

    if (strcmp(cmd, "PING") == 0)
        return reply(fd, "PONG\n") == 0;

    if (strcmp(cmd, "QUIT") == 0) {
        reply(fd, "BYE\n");
        return 0;
    }

    return reply(fd, "ERR unknown command\n") == 0;
}

/* Worker job: serve one client until it disconnects or the server stops. */
static void serve_client(int fd, void *ctx)
{
    kvstore_t *kv = ctx;
    char buf[MAX_LINE];
    size_t used = 0;

    /* A receive timeout lets idle connections notice shutdown. */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    while (running) {
        ssize_t n = recv(fd, buf + used, sizeof(buf) - used - 1, 0);
        if (n == 0)
            break; /* client closed */
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            break;
        }
        used += (size_t)n;
        buf[used] = '\0';

        /* TCP is a byte stream: one recv may hold several lines, or half of one. */
        char *start = buf;
        char *nl;
        int keep = 1;
        while (keep && (nl = memchr(start, '\n', used - (size_t)(start - buf)))) {
            *nl = '\0';
            if (nl > start && nl[-1] == '\r')
                nl[-1] = '\0'; /* accept telnet-style \r\n */
            keep = handle_command(kv, fd, start);
            start = nl + 1;
        }
        if (!keep)
            break;

        /* Shift any partial line to the front of the buffer. */
        size_t leftover = used - (size_t)(start - buf);
        memmove(buf, start, leftover);
        used = leftover;

        if (used >= sizeof(buf) - 1) {
            reply(fd, "ERR line too long\n");
            break;
        }
    }
    close(fd);
}

static int listen_on(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, 128) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char **argv)
{
    int port = 7070;
    int threads = 8;
    int opt;
    while ((opt = getopt(argc, argv, "p:t:h")) != -1) {
        switch (opt) {
        case 'p': port = atoi(optarg); break;
        case 't': threads = atoi(optarg); break;
        default:
            fprintf(stderr, "usage: %s [-p port] [-t worker_threads]\n", argv[0]);
            return opt == 'h' ? 0 : 1;
        }
    }
    if (port <= 0 || port > 65535 || threads <= 0) {
        fprintf(stderr, "invalid port or thread count\n");
        return 1;
    }

    /* sigaction without SA_RESTART, so blocking calls return EINTR on Ctrl+C. */
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    kvstore_t *kv = kv_create(4096);
    if (!kv) {
        fprintf(stderr, "failed to create store\n");
        return 1;
    }
    threadpool_t *pool = threadpool_create((size_t)threads, 256, serve_client, kv);
    if (!pool) {
        fprintf(stderr, "failed to create thread pool\n");
        kv_destroy(kv);
        return 1;
    }
    int lfd = listen_on(port);
    if (lfd < 0) {
        threadpool_destroy(pool);
        kv_destroy(kv);
        return 1;
    }
    printf("listening on port %d with %d worker threads\n", port, threads);
    fflush(stdout);

    /* poll() with a timeout so the accept loop re-checks the running flag. */
    struct pollfd pfd = { .fd = lfd, .events = POLLIN };
    while (running) {
        int ready = poll(&pfd, 1, 500);
        if (ready <= 0)
            continue; /* timeout or EINTR */
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0)
            continue;
        if (threadpool_submit(pool, cfd) != 0)
            close(cfd);
    }

    printf("shutting down...\n");
    close(lfd);
    threadpool_destroy(pool); /* joins workers after they finish their clients */
    kv_destroy(kv);
    printf("clean exit\n");
    return 0;
}
