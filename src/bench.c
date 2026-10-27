/*
 * bench: concurrent load generator for tcp-kv-server.
 *
 * Spawns C client threads; each opens its own TCP connection and runs N
 * operations (a mix of SET and GET on its own keys), checking every reply.
 * Reports total throughput and p50/p99 per-request latency.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int     id;
    int     ops;
    int     read_pct;
    double *latencies_us; /* one entry per request */
    int     errors;
} client_args_t;

static const char *host = "127.0.0.1";
static int port = 7070;

static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

/* Read one '\n'-terminated reply. Returns its length, or -1 on error. */
static int read_line(int fd, char *buf, size_t len)
{
    size_t used = 0;
    while (used < len - 1) {
        ssize_t n = recv(fd, buf + used, 1, 0);
        if (n <= 0)
            return -1;
        if (buf[used] == '\n') {
            buf[used] = '\0';
            return (int)used;
        }
        used++;
    }
    return -1;
}

static int connect_to_server(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    inet_pton(AF_INET, host, &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

static void *client_thread(void *arg)
{
    client_args_t *a = arg;
    int fd = connect_to_server();
    if (fd < 0) {
        a->errors = a->ops;
        return NULL;
    }

    unsigned seed = (unsigned)a->id * 2654435761u;
    char req[512], resp[512], expect[512];

    for (int i = 0; i < a->ops; i++) {
        int key = rand_r(&seed) % 1000;
        /* Always SET first so later GETs have something to read. */
        int is_get = i > 0 && (rand_r(&seed) % 100) < a->read_pct;

        int n;
        if (is_get)
            n = snprintf(req, sizeof(req), "GET c%d-k%d\n", a->id, key);
        else
            n = snprintf(req, sizeof(req), "SET c%d-k%d value-%d-%d\n",
                         a->id, key, a->id, key);

        double t0 = now_us();
        if (send(fd, req, (size_t)n, MSG_NOSIGNAL) != n ||
            read_line(fd, resp, sizeof(resp)) < 0) {
            a->errors++;
            break;
        }
        a->latencies_us[i] = now_us() - t0;

        /* Verify correctness, not just speed. Keys are private to this thread,
         * so a GET must return exactly what this thread SET, or NOT_FOUND. */
        if (is_get) {
            snprintf(expect, sizeof(expect), "VALUE value-%d-%d", a->id, key);
            if (strcmp(resp, expect) != 0 && strcmp(resp, "NOT_FOUND") != 0)
                a->errors++;
        } else if (strcmp(resp, "OK") != 0) {
            a->errors++;
        }
    }
    close(fd);
    return NULL;
}

static int cmp_double(const void *x, const void *y)
{
    double a = *(const double *)x, b = *(const double *)y;
    return (a > b) - (a < b);
}

int main(int argc, char **argv)
{
    int clients = 16, ops = 10000, read_pct = 80, opt;
    while ((opt = getopt(argc, argv, "H:p:c:n:r:")) != -1) {
        switch (opt) {
        case 'H': host = optarg; break;
        case 'p': port = atoi(optarg); break;
        case 'c': clients = atoi(optarg); break;
        case 'n': ops = atoi(optarg); break;
        case 'r': read_pct = atoi(optarg); break;
        default:
            fprintf(stderr, "usage: %s [-H host] [-p port] [-c clients] "
                            "[-n ops_per_client] [-r read_percent]\n", argv[0]);
            return 1;
        }
    }

    pthread_t     *tids = calloc((size_t)clients, sizeof(*tids));
    client_args_t *args = calloc((size_t)clients, sizeof(*args));
    double        *all  = calloc((size_t)clients * (size_t)ops, sizeof(double));
    if (!tids || !args || !all) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    double start = now_us();
    for (int i = 0; i < clients; i++) {
        args[i] = (client_args_t){ .id = i, .ops = ops, .read_pct = read_pct,
                                   .latencies_us = all + (size_t)i * (size_t)ops };
        pthread_create(&tids[i], NULL, client_thread, &args[i]);
    }
    int errors = 0;
    for (int i = 0; i < clients; i++) {
        pthread_join(tids[i], NULL);
        errors += args[i].errors;
    }
    double elapsed_s = (now_us() - start) / 1e6;

    size_t total = (size_t)clients * (size_t)ops;
    qsort(all, total, sizeof(double), cmp_double);

    printf("clients:      %d\n", clients);
    printf("requests:     %zu (%d%% reads)\n", total, read_pct);
    printf("errors:       %d\n", errors);
    printf("elapsed:      %.2f s\n", elapsed_s);
    printf("throughput:   %.0f req/s\n", total / elapsed_s);
    printf("latency p50:  %.1f us\n", all[total / 2]);
    printf("latency p99:  %.1f us\n", all[(size_t)(total * 0.99)]);

    free(tids);
    free(args);
    free(all);
    return errors == 0 ? 0 : 1;
}
