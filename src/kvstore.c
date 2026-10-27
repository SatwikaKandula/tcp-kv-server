#include "kvstore.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* FNV-1a: a simple, fast hash with a good spread for short string keys. */
static uint64_t hash_str(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 1099511628211ULL;
    }
    return h;
}

static size_t bucket_of(const kvstore_t *kv, const char *key)
{
    return hash_str(key) % kv->num_buckets;
}

/* Every bucket maps to exactly one stripe lock. */
static pthread_rwlock_t *stripe_of(kvstore_t *kv, size_t bucket)
{
    return &kv->stripes[bucket % NUM_STRIPES];
}

kvstore_t *kv_create(size_t num_buckets)
{
    if (num_buckets == 0)
        return NULL;

    kvstore_t *kv = calloc(1, sizeof(*kv));
    if (!kv)
        return NULL;

    kv->buckets = calloc(num_buckets, sizeof(kv_entry_t *));
    if (!kv->buckets) {
        free(kv);
        return NULL;
    }
    kv->num_buckets = num_buckets;

    for (int i = 0; i < NUM_STRIPES; i++)
        pthread_rwlock_init(&kv->stripes[i], NULL);
    return kv;
}

void kv_destroy(kvstore_t *kv)
{
    if (!kv)
        return;

    /* Free every entry and the strings it owns, so the store never leaks. */
    for (size_t i = 0; i < kv->num_buckets; i++) {
        kv_entry_t *e = kv->buckets[i];
        while (e) {
            kv_entry_t *next = e->next;
            free(e->key);
            free(e->value);
            free(e);
            e = next;
        }
    }
    for (int i = 0; i < NUM_STRIPES; i++)
        pthread_rwlock_destroy(&kv->stripes[i]);
    free(kv->buckets);
    free(kv);
}

int kv_set(kvstore_t *kv, const char *key, const char *value)
{
    size_t b = bucket_of(kv, key);
    pthread_rwlock_t *lock = stripe_of(kv, b);

    /* Allocate before taking the lock to keep the critical section short. */
    char *new_value = strdup(value);
    if (!new_value)
        return -1;

    pthread_rwlock_wrlock(lock);

    for (kv_entry_t *e = kv->buckets[b]; e; e = e->next) {
        if (strcmp(e->key, key) == 0) {
            char *old = e->value;
            e->value = new_value;
            pthread_rwlock_unlock(lock);
            free(old);
            return 0;
        }
    }

    kv_entry_t *e = malloc(sizeof(*e));
    char *new_key = strdup(key);
    if (!e || !new_key) {
        pthread_rwlock_unlock(lock);
        free(e);
        free(new_key);
        free(new_value);
        return -1;
    }
    e->key   = new_key;
    e->value = new_value;
    e->next  = kv->buckets[b];
    kv->buckets[b] = e;

    pthread_rwlock_unlock(lock);
    return 0;
}

int kv_get(kvstore_t *kv, const char *key, char *out, size_t out_len)
{
    size_t b = bucket_of(kv, key);
    pthread_rwlock_t *lock = stripe_of(kv, b);
    int rc = -1;

    pthread_rwlock_rdlock(lock); /* shared: readers don't block each other */
    for (kv_entry_t *e = kv->buckets[b]; e; e = e->next) {
        if (strcmp(e->key, key) == 0) {
            /* Copy while holding the lock: the value may be freed after unlock. */
            strncpy(out, e->value, out_len - 1);
            out[out_len - 1] = '\0';
            rc = 0;
            break;
        }
    }
    pthread_rwlock_unlock(lock);
    return rc;
}

int kv_del(kvstore_t *kv, const char *key)
{
    size_t b = bucket_of(kv, key);
    pthread_rwlock_t *lock = stripe_of(kv, b);

    pthread_rwlock_wrlock(lock);
    kv_entry_t **link = &kv->buckets[b];
    while (*link) {
        kv_entry_t *e = *link;
        if (strcmp(e->key, key) == 0) {
            *link = e->next; /* unlink using a pointer-to-pointer */
            pthread_rwlock_unlock(lock);
            free(e->key);
            free(e->value);
            free(e);
            return 0;
        }
        link = &e->next;
    }
    pthread_rwlock_unlock(lock);
    return -1;
}
