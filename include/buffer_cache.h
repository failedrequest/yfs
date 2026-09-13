#ifndef BUFFER_CACHE_H
#define BUFFER_CACHE_H

#include "block_dev.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>

typedef struct block_buffer {
    uint64_t blk_no;
    bool is_dirty;
    bool is_metadata;
    uint64_t last_txid;     /* Transaction ID that modified this block */
    uint8_t *data;
    struct block_buffer *prev;
    struct block_buffer *next;
    struct block_buffer *hash_next;
} block_buffer_t;

typedef struct buffer_cache {
    block_dev_t *dev;
    size_t block_size;
    size_t max_blocks;
    size_t num_blocks;
    pthread_mutex_t lock;

    block_buffer_t *lru_head;
    block_buffer_t *lru_tail;

    #define BUF_HASH_SIZE 1024
    block_buffer_t *hash_table[BUF_HASH_SIZE];
} buffer_cache_t;

buffer_cache_t *buffer_cache_create(block_dev_t *dev, size_t block_size, size_t max_blocks);
void buffer_cache_destroy(buffer_cache_t *cache);

/* Read or get a block from cache/disk */
block_buffer_t *buffer_cache_get(buffer_cache_t *cache, uint64_t blk_no, bool is_metadata);

/* Mark block as dirty and associate with current transaction ID */
void buffer_cache_mark_dirty(buffer_cache_t *cache, block_buffer_t *buf, uint64_t txid);

/* Flush a specific buffer to disk */
bool buffer_cache_flush_block(buffer_cache_t *cache, block_buffer_t *buf);

/* Flush all dirty blocks up to a given transaction ID (checkpoint) */
bool buffer_cache_checkpoint_tx(buffer_cache_t *cache, uint64_t up_to_txid);

/* Flush all dirty blocks in cache */
bool buffer_cache_sync_all(buffer_cache_t *cache);

/* Invalidate cached blocks */
void buffer_cache_invalidate(buffer_cache_t *cache);

#endif /* BUFFER_CACHE_H */
