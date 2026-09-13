#ifndef YFS_CORE_H
#define YFS_CORE_H

#include "yfs_fs.h"
#include "block_dev.h"
#include "buffer_cache.h"
#include "journal.h"
#include "tx_manager.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>

typedef struct yfs_dir_entry {
    uint32_t ino;
    uint8_t type;
    char name[YFS_MAX_NAME_LEN + 1];
} yfs_dir_entry_t;

typedef struct yfs_dir_list {
    yfs_dir_entry_t *entries;
    size_t count;
    size_t capacity;
} yfs_dir_list_t;

typedef struct yfs_filesystem {
    char dev_path[1024];
    block_dev_t *dev;
    buffer_cache_t *cache;
    journal_t *journal;
    tx_manager_t *tx_mgr;
    yfs_superblock_t sb;
    yfs_cgroup_t *cgroups;
    uint32_t cgroup_count;
    pthread_mutex_t lock;
} yfs_filesystem_t;

yfs_filesystem_t *yfs_fs_create(const char *dev_path);
void yfs_fs_destroy(yfs_filesystem_t *fs);

bool yfs_fs_mount(yfs_filesystem_t *fs);
void yfs_fs_unmount(yfs_filesystem_t *fs);

/* Filesystem operations */
int yfs_lookup(yfs_filesystem_t *fs, uint32_t parent_ino, const char *name, uint32_t *out_ino, yfs_dinode_t *out_dinode);
int yfs_getattr(yfs_filesystem_t *fs, uint32_t ino, yfs_dinode_t *out_dinode);
int yfs_setattr(yfs_filesystem_t *fs, uint32_t ino, const yfs_dinode_t *dinode, int to_set);
int yfs_create(yfs_filesystem_t *fs, uint32_t parent_ino, const char *name, mode_t mode, uint32_t uid, uint32_t gid, uint32_t *out_ino, yfs_dinode_t *out_dinode);
int yfs_mkdir(yfs_filesystem_t *fs, uint32_t parent_ino, const char *name, mode_t mode, uint32_t uid, uint32_t gid, uint32_t *out_ino, yfs_dinode_t *out_dinode);
int yfs_unlink(yfs_filesystem_t *fs, uint32_t parent_ino, const char *name);
int yfs_rmdir(yfs_filesystem_t *fs, uint32_t parent_ino, const char *name);
int yfs_readdir(yfs_filesystem_t *fs, uint32_t ino, yfs_dir_list_t *list);
int yfs_read(yfs_filesystem_t *fs, uint32_t ino, void *buf, size_t size, off_t offset, size_t *bytes_read);
int yfs_write(yfs_filesystem_t *fs, uint32_t ino, const void *buf, size_t size, off_t offset, size_t *bytes_written);
int yfs_truncate(yfs_filesystem_t *fs, uint32_t ino, off_t new_size);
int yfs_statfs(yfs_filesystem_t *fs, uint64_t *total_blocks, uint64_t *free_blocks, uint64_t *total_inodes, uint64_t *free_inodes);

/* Allocation and mapping helpers */
uint32_t yfs_alloc_inode(yfs_filesystem_t *fs, transaction_t *tx, mode_t mode);
bool yfs_free_inode(yfs_filesystem_t *fs, transaction_t *tx, uint32_t ino);
uint32_t yfs_alloc_block(yfs_filesystem_t *fs, transaction_t *tx);
bool yfs_free_block(yfs_filesystem_t *fs, transaction_t *tx, uint32_t blk_no);

bool yfs_read_inode(yfs_filesystem_t *fs, uint32_t ino, yfs_dinode_t *dinode);
bool yfs_write_inode(yfs_filesystem_t *fs, transaction_t *tx, uint32_t ino, const yfs_dinode_t *dinode);
uint32_t yfs_bmap(yfs_filesystem_t *fs, transaction_t *tx, yfs_dinode_t *dinode, uint32_t ino, uint32_t logical_blk, bool create_if_missing);

void yfs_dir_list_init(yfs_dir_list_t *list);
void yfs_dir_list_free(yfs_dir_list_t *list);
bool yfs_dir_list_add(yfs_dir_list_t *list, uint32_t ino, uint8_t type, const char *name);

#endif /* YFS_CORE_H */
