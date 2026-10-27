#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

/* A job is one accepted client socket plus the function that serves it. */
typedef void (*job_fn)(int client_fd, void *ctx);

typedef struct threadpool {
    pthread_t      *workers;
    size_t          num_workers;

    /* Bounded circular queue of client file descriptors. */
    int            *queue;
    size_t          capacity;
    size_t          head;      /* next slot to dequeue */
    size_t          tail;      /* next slot to enqueue */
    size_t          count;

    pthread_mutex_t lock;
    pthread_cond_t  not_empty; /* signalled when a job is added */
    pthread_cond_t  not_full;  /* signalled when a job is removed */
    bool            shutting_down;

    job_fn          fn;
    void           *ctx;
} threadpool_t;

/* Create a pool of num_workers threads with a queue of queue_capacity jobs. */
threadpool_t *threadpool_create(size_t num_workers, size_t queue_capacity,
                                job_fn fn, void *ctx);

/* Add a client socket. Blocks while the queue is full (backpressure).
 * Returns 0 on success, -1 if the pool is shutting down. */
int threadpool_submit(threadpool_t *pool, int client_fd);

/* Stop accepting jobs, let workers drain the queue, join them, free memory. */
void threadpool_destroy(threadpool_t *pool);

#endif
