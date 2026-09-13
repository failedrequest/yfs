#include "block_dev.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>

block_dev_t *block_dev_open(const char *path, bool create_if_missing, uint64_t size_bytes) {
    if (!path) return nullptr;

    block_dev_t *dev = (block_dev_t *)calloc(1, sizeof(block_dev_t));
    if (!dev) return nullptr;

    strncpy(dev->path, path, sizeof(dev->path) - 1);
    pthread_mutex_init(&dev->lock, nullptr);

    int flags = O_RDWR;
    if (create_if_missing) {
        flags |= O_CREAT;
    }

    dev->fd = open(path, flags, 0644);
    if (dev->fd < 0) {
        pthread_mutex_destroy(&dev->lock);
        free(dev);
        return nullptr;
    }

    struct stat st;
    if (fstat(dev->fd, &st) != 0) {
        close(dev->fd);
        pthread_mutex_destroy(&dev->lock);
        free(dev);
        return nullptr;
    }

    if (S_ISREG(st.st_mode)) {
        if (create_if_missing && size_bytes > 0) {
            if (ftruncate(dev->fd, (off_t)size_bytes) != 0) {
                close(dev->fd);
                pthread_mutex_destroy(&dev->lock);
                free(dev);
                return nullptr;
            }
            dev->size_bytes = size_bytes;
        } else {
            dev->size_bytes = (uint64_t)st.st_size;
        }
    } else {
        dev->size_bytes = (uint64_t)st.st_size;
    }

    return dev;
}

void block_dev_close(block_dev_t *dev) {
    if (!dev) return;
    pthread_mutex_lock(&dev->lock);
    if (dev->fd >= 0) {
        fsync(dev->fd);
        close(dev->fd);
        dev->fd = -1;
    }
    pthread_mutex_unlock(&dev->lock);
    pthread_mutex_destroy(&dev->lock);
    free(dev);
}

bool block_dev_read(block_dev_t *dev, uint64_t blk_no, void *buf, size_t blk_size) {
    if (!dev || dev->fd < 0 || !buf) return false;
    pthread_mutex_lock(&dev->lock);
    off_t offset = (off_t)(blk_no * blk_size);
    ssize_t ret = pread(dev->fd, buf, blk_size, offset);
    pthread_mutex_unlock(&dev->lock);
    return (ret == (ssize_t)blk_size);
}

bool block_dev_write(block_dev_t *dev, uint64_t blk_no, const void *buf, size_t blk_size) {
    if (!dev || dev->fd < 0 || !buf) return false;
    pthread_mutex_lock(&dev->lock);
    off_t offset = (off_t)(blk_no * blk_size);
    ssize_t ret = pwrite(dev->fd, buf, blk_size, offset);
    pthread_mutex_unlock(&dev->lock);
    return (ret == (ssize_t)blk_size);
}

bool block_dev_flush(block_dev_t *dev) {
    if (!dev || dev->fd < 0) return false;
    pthread_mutex_lock(&dev->lock);
    int ret = fsync(dev->fd);
    pthread_mutex_unlock(&dev->lock);
    return (ret == 0);
}

uint64_t block_dev_total_blocks(const block_dev_t *dev, size_t blk_size) {
    if (!dev || blk_size == 0) return 0;
    return dev->size_bytes / blk_size;
}
