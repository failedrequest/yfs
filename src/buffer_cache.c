#include "buffer_cache.h"
#include <stdlib.h>
#include <string.h>

static size_t hash_block(uint64_t blk_no) {
    return (size_t)(blk_no % BUF_HASH_SIZE);
}

buffer_cache_t *buffer_cache_create(block_dev_t *dev, size_t block_size, size_t max_blocks) {
    buffer_cache_t *cache = (buffer_cache_t *)calloc(1, sizeof(buffer_cache_t));
    if (!cache) return nullptr;

    cache->dev = dev;
    cache->block_size = block_size;
    cache->max_blocks = max_blocks;
    cache->num_blocks = 0;
    pthread_mutex_init(&cache->lock, nullptr);

    return cache;
}

static void lru_remove(buffer_cache_t *cache, block_buffer_t *buf) {
    if (buf->prev) {
        buf->prev->next = buf->next;
    } else {
        cache->lru_head = buf->next;
    }

    if (buf->next) {
        buf->next->prev = buf->prev;
    } else {
        cache->lru_tail = buf->prev;
    }

    buf->prev = nullptr;
    buf->next = nullptr;
}

static void lru_push_front(buffer_cache_t *cache, block_buffer_t *buf) {
    buf->prev = nullptr;
    buf->next = cache->lru_head;
    if (cache->lru_head) {
        cache->lru_head->prev = buf;
    }
    cache->lru_head = buf;
    if (!cache->lru_tail) {
        cache->lru_tail = buf;
    }
}

static void evict_lru_unlocked(buffer_cache_t *cache) {
    while (cache->num_blocks >= cache->max_blocks && cache->lru_tail) {
        block_buffer_t *victim = cache->lru_tail;

        if (victim->is_dirty && cache->dev) {
            block_dev_write(cache->dev, victim->blk_no, victim->data, cache->block_size);
            victim->is_dirty = false;
        }

        lru_remove(cache, victim);

        /* Remove from hash table */
        size_t h = hash_block(victim->blk_no);
        block_buffer_t **curr = &cache->hash_table[h];
        while (*curr) {
            if (*curr == victim) {
                *curr = victim->hash_next;
                break;
            }
            curr = &((*curr)->hash_next);
        }

        free(victim->data);
        free(victim);
        cache->num_blocks--;
    }
}

void buffer_cache_destroy(buffer_cache_t *cache) {
    if (!cache) return;
    buffer_cache_sync_all(cache);

    pthread_mutex_lock(&cache->lock);
    block_buffer_t *curr = cache->lru_head;
    while (curr) {
        block_buffer_t *next = curr->next;
        free(curr->data);
        free(curr);
        curr = next;
    }
    cache->lru_head = nullptr;
    cache->lru_tail = nullptr;
    cache->num_blocks = 0;
    pthread_mutex_unlock(&cache->lock);

    pthread_mutex_destroy(&cache->lock);
    free(cache);
}

block_buffer_t *buffer_cache_get(buffer_cache_t *cache, uint64_t blk_no, bool is_metadata) {
    if (!cache) return nullptr;
    pthread_mutex_lock(&cache->lock);

    size_t h = hash_block(blk_no);
    block_buffer_t *buf = cache->hash_table[h];
    while (buf) {
        if (buf->blk_no == blk_no) {
            lru_remove(cache, buf);
            lru_push_front(cache, buf);
            pthread_mutex_unlock(&cache->lock);
            return buf;
        }
        buf = buf->hash_next;
    }

    if (cache->num_blocks >= cache->max_blocks) {
        evict_lru_unlocked(cache);
    }

    buf = (block_buffer_t *)calloc(1, sizeof(block_buffer_t));
    if (!buf) {
        pthread_mutex_unlock(&cache->lock);
        return nullptr;
    }

    buf->blk_no = blk_no;
    buf->is_dirty = false;
    buf->is_metadata = is_metadata;
    buf->last_txid = 0;
    buf->data = (uint8_t *)calloc(1, cache->block_size);

    if (cache->dev) {
        block_dev_read(cache->dev, blk_no, buf->data, cache->block_size);
    }

    buf->hash_next = cache->hash_table[h];
    cache->hash_table[h] = buf;

    lru_push_front(cache, buf);
    cache->num_blocks++;

    pthread_mutex_unlock(&cache->lock);
    return buf;
}

void buffer_cache_mark_dirty(buffer_cache_t *cache, block_buffer_t *buf, uint64_t txid) {
    if (!cache || !buf) return;
    pthread_mutex_lock(&cache->lock);
    buf->is_dirty = true;
    if (txid > 0) {
        buf->last_txid = txid;
    }
    pthread_mutex_unlock(&cache->lock);
}

bool buffer_cache_flush_block(buffer_cache_t *cache, block_buffer_t *buf) {
    if (!cache || !buf || !cache->dev) return false;
    pthread_mutex_lock(&cache->lock);
    bool ok = true;
    if (buf->is_dirty) {
        ok = block_dev_write(cache->dev, buf->blk_no, buf->data, cache->block_size);
        if (ok) {
            buf->is_dirty = false;
        }
    }
    pthread_mutex_unlock(&cache->lock);
    return ok;
}

bool buffer_cache_checkpoint_tx(buffer_cache_t *cache, uint64_t up_to_txid) {
    if (!cache || !cache->dev) return false;
    pthread_mutex_lock(&cache->lock);

    bool all_ok = true;
    for (size_t i = 0; i < BUF_HASH_SIZE; ++i) {
        block_buffer_t *buf = cache->hash_table[i];
        while (buf) {
            if (buf->is_dirty && buf->last_txid <= up_to_txid) {
                if (!block_dev_write(cache->dev, buf->blk_no, buf->data, cache->block_size)) {
                    all_ok = false;
                } else {
                    buf->is_dirty = false;
                }
            }
            buf = buf->hash_next;
        }
    }

    block_dev_flush(cache->dev);
    pthread_mutex_unlock(&cache->lock);
    return all_ok;
}

bool buffer_cache_sync_all(buffer_cache_t *cache) {
    if (!cache || !cache->dev) return false;
    pthread_mutex_lock(&cache->lock);

    bool all_ok = true;
    for (size_t i = 0; i < BUF_HASH_SIZE; ++i) {
        block_buffer_t *buf = cache->hash_table[i];
        while (buf) {
            if (buf->is_dirty) {
                if (!block_dev_write(cache->dev, buf->blk_no, buf->data, cache->block_size)) {
                    all_ok = false;
                } else {
                    buf->is_dirty = false;
                }
            }
            buf = buf->hash_next;
        }
    }

    block_dev_flush(cache->dev);
    pthread_mutex_unlock(&cache->lock);
    return all_ok;
}

void buffer_cache_invalidate(buffer_cache_t *cache) {
    if (!cache) return;
    buffer_cache_sync_all(cache);

    pthread_mutex_lock(&cache->lock);
    block_buffer_t *curr = cache->lru_head;
    while (curr) {
        block_buffer_t *next = curr->next;
        free(curr->data);
        free(curr);
        curr = next;
    }
    cache->lru_head = nullptr;
    cache->lru_tail = nullptr;
    memset(cache->hash_table, 0, sizeof(cache->hash_table));
    cache->num_blocks = 0;
    pthread_mutex_unlock(&cache->lock);
}
