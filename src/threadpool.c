#include "threadpool.h"

#include <stdlib.h>
#include <unistd.h>

static void *worker_loop(void *arg)
{
    threadpool_t *pool = arg;

    for (;;) {
        pthread_mutex_lock(&pool->lock);

        /* Wait in a loop: condition variables can wake spuriously. */
        while (pool->count == 0 && !pool->shutting_down)
            pthread_cond_wait(&pool->not_empty, &pool->lock);

        /* Exit only once the queue is drained, so no accepted client is lost. */
        if (pool->count == 0 && pool->shutting_down) {
            pthread_mutex_unlock(&pool->lock);
            return NULL;
        }

        int fd = pool->queue[pool->head];
        pool->head = (pool->head + 1) % pool->capacity;
        pool->count--;

        pthread_cond_signal(&pool->not_full);
        pthread_mutex_unlock(&pool->lock);

        /* Serve the client outside the lock so workers run in parallel. */
        pool->fn(fd, pool->ctx);
    }
}

threadpool_t *threadpool_create(size_t num_workers, size_t queue_capacity,
                                job_fn fn, void *ctx)
{
    if (num_workers == 0 || queue_capacity == 0 || fn == NULL)
        return NULL;

    threadpool_t *pool = calloc(1, sizeof(*pool));
    if (!pool)
        return NULL;

    pool->queue   = calloc(queue_capacity, sizeof(int));
    pool->workers = calloc(num_workers, sizeof(pthread_t));
    if (!pool->queue || !pool->workers) {
        free(pool->queue);
        free(pool->workers);
        free(pool);
        return NULL;
    }

    pool->capacity = queue_capacity;
    pool->fn       = fn;
    pool->ctx      = ctx;
    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->not_empty, NULL);
    pthread_cond_init(&pool->not_full, NULL);

    for (size_t i = 0; i < num_workers; i++) {
        if (pthread_create(&pool->workers[i], NULL, worker_loop, pool) != 0) {
            /* Start-up failed part way: shut down the threads we did create. */
            pool->num_workers = i;
            threadpool_destroy(pool);
            return NULL;
        }
    }
    pool->num_workers = num_workers;
    return pool;
}

int threadpool_submit(threadpool_t *pool, int client_fd)
{
    pthread_mutex_lock(&pool->lock);

    while (pool->count == pool->capacity && !pool->shutting_down)
        pthread_cond_wait(&pool->not_full, &pool->lock);

    if (pool->shutting_down) {
        pthread_mutex_unlock(&pool->lock);
        return -1;
    }

    pool->queue[pool->tail] = client_fd;
    pool->tail = (pool->tail + 1) % pool->capacity;
    pool->count++;

    pthread_cond_signal(&pool->not_empty);
    pthread_mutex_unlock(&pool->lock);
    return 0;
}

void threadpool_destroy(threadpool_t *pool)
{
    if (!pool)
        return;

    pthread_mutex_lock(&pool->lock);
    pool->shutting_down = true;
    pthread_cond_broadcast(&pool->not_empty);
    pthread_cond_broadcast(&pool->not_full);
    pthread_mutex_unlock(&pool->lock);

    for (size_t i = 0; i < pool->num_workers; i++)
        pthread_join(pool->workers[i], NULL);

    pthread_mutex_destroy(&pool->lock);
    pthread_cond_destroy(&pool->not_empty);
    pthread_cond_destroy(&pool->not_full);
    free(pool->queue);
    free(pool->workers);
    free(pool);
}
