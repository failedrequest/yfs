#ifndef BLOCK_DEV_H
#define BLOCK_DEV_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>

typedef struct block_dev {
    int fd;
    char path[1024];
    uint64_t size_bytes;
    pthread_mutex_t lock;
} block_dev_t;

block_dev_t *block_dev_open(const char *path, bool create_if_missing, uint64_t size_bytes);
void block_dev_close(block_dev_t *dev);

bool block_dev_read(block_dev_t *dev, uint64_t blk_no, void *buf, size_t blk_size);
bool block_dev_write(block_dev_t *dev, uint64_t blk_no, const void *buf, size_t blk_size);
bool block_dev_flush(block_dev_t *dev);

uint64_t block_dev_total_blocks(const block_dev_t *dev, size_t blk_size);

#endif /* BLOCK_DEV_H */
