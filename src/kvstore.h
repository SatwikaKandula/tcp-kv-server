#ifndef KVSTORE_H
#define KVSTORE_H

#include <pthread.h>
#include <stddef.h>

/* One key-value pair in a bucket's singly linked list (separate chaining). */
typedef struct kv_entry {
    char            *key;
    char            *value;
    struct kv_entry *next;
} kv_entry_t;

/*
 * Thread-safe hash table using lock striping: buckets are split across
 * NUM_STRIPES reader-writer locks, so threads touching different stripes
 * never contend, and many readers can share one stripe at the same time.
 */
#define NUM_STRIPES 64

typedef struct kvstore {
    kv_entry_t     **buckets;
    size_t           num_buckets;
    pthread_rwlock_t stripes[NUM_STRIPES];
} kvstore_t;

kvstore_t *kv_create(size_t num_buckets);
void       kv_destroy(kvstore_t *kv);

/* Insert or overwrite. Returns 0 on success, -1 on allocation failure. */
int kv_set(kvstore_t *kv, const char *key, const char *value);

/* Copy the value into out (at most out_len bytes, NUL-terminated).
 * Returns 0 if found, -1 if the key does not exist. */
int kv_get(kvstore_t *kv, const char *key, char *out, size_t out_len);

/* Returns 0 if the key was deleted, -1 if it did not exist. */
int kv_del(kvstore_t *kv, const char *key);

#endif
