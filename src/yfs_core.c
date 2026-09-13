#include "yfs_core.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

void yfs_dir_list_init(yfs_dir_list_t *list) {
    if (!list) return;
    list->entries = nullptr;
    list->count = 0;
    list->capacity = 0;
}

void yfs_dir_list_free(yfs_dir_list_t *list) {
    if (!list) return;
    if (list->entries) {
        free(list->entries);
    }
    list->entries = nullptr;
    list->count = 0;
    list->capacity = 0;
}

bool yfs_dir_list_add(yfs_dir_list_t *list, uint32_t ino, uint8_t type, const char *name) {
    if (!list || !name) return false;
    if (list->count >= list->capacity) {
        size_t new_cap = list->capacity == 0 ? 16 : list->capacity * 2;
        yfs_dir_entry_t *new_entries = (yfs_dir_entry_t *)realloc(list->entries, new_cap * sizeof(yfs_dir_entry_t));
        if (!new_entries) return false;
        list->entries = new_entries;
        list->capacity = new_cap;
    }

    yfs_dir_entry_t *e = &list->entries[list->count++];
    e->ino = ino;
    e->type = type;
    strncpy(e->name, name, YFS_MAX_NAME_LEN);
    e->name[YFS_MAX_NAME_LEN] = '\0';
    return true;
}

static bool load_superblock(yfs_filesystem_t *fs) {
    block_buffer_t *buf = buffer_cache_get(fs->cache, 0, true);
    if (!buf) return false;
    memcpy(&fs->sb, buf->data, sizeof(yfs_superblock_t));
    if (fs->sb.s_magic != YFS_MAGIC) {
        return false;
    }
    return true;
}

static bool sync_superblock(yfs_filesystem_t *fs, transaction_t *tx) {
    block_buffer_t *buf = buffer_cache_get(fs->cache, 0, true);
    if (!buf) return false;
    memcpy(buf->data, &fs->sb, sizeof(yfs_superblock_t));
    if (tx) {
        tx_modify_block(tx, buf);
    } else {
        buffer_cache_mark_dirty(fs->cache, buf, 0);
    }
    return true;
}

static bool load_cgroups(yfs_filesystem_t *fs) {
    fs->cgroup_count = fs->sb.s_cgroup_count;
    fs->cgroups = (yfs_cgroup_t *)calloc(fs->cgroup_count, sizeof(yfs_cgroup_t));
    if (!fs->cgroups) return false;

    size_t cg_per_block = fs->sb.s_bsize / sizeof(yfs_cgroup_t);

    for (uint32_t i = 0; i < fs->sb.s_cgroup_count; i += cg_per_block) {
        uint64_t blk = 1 + (i / cg_per_block);
        block_buffer_t *buf = buffer_cache_get(fs->cache, blk, true);
        if (!buf) return false;

        yfs_cgroup_t *cg_arr = (yfs_cgroup_t *)buf->data;
        for (size_t j = 0; j < cg_per_block && (i + j) < fs->sb.s_cgroup_count; ++j) {
            fs->cgroups[i + j] = cg_arr[j];
        }
    }
    return true;
}

static bool sync_cgroup(yfs_filesystem_t *fs, transaction_t *tx, uint32_t cg_idx) {
    if (cg_idx >= fs->cgroup_count) return false;
    size_t cg_per_block = fs->sb.s_bsize / sizeof(yfs_cgroup_t);
    uint64_t blk = 1 + (cg_idx / cg_per_block);
    size_t offset_idx = cg_idx % cg_per_block;

    block_buffer_t *buf = buffer_cache_get(fs->cache, blk, true);
    if (!buf) return false;

    yfs_cgroup_t *cg_arr = (yfs_cgroup_t *)buf->data;
    cg_arr[offset_idx] = fs->cgroups[cg_idx];

    if (tx) {
        tx_modify_block(tx, buf);
    } else {
        buffer_cache_mark_dirty(fs->cache, buf, 0);
    }
    return true;
}

static uint64_t inode_to_block(yfs_filesystem_t *fs, uint32_t ino, uint32_t *offset_in_block) {
    if (ino == 0 || ino > fs->sb.s_inodes_count) return 0;
    uint32_t idx = ino - 1;
    uint32_t cg_idx = idx / fs->sb.s_inodes_per_cg;
    uint32_t ino_in_cg = idx % fs->sb.s_inodes_per_cg;

    size_t inodes_per_block = fs->sb.s_bsize / sizeof(yfs_dinode_t);
    uint64_t blk = fs->cgroups[cg_idx].cg_inode_table + (ino_in_cg / inodes_per_block);
    if (offset_in_block) {
        *offset_in_block = (ino_in_cg % inodes_per_block) * sizeof(yfs_dinode_t);
    }
    return blk;
}

yfs_filesystem_t *yfs_fs_create(const char *dev_path) {
    if (!dev_path) return nullptr;
    yfs_filesystem_t *fs = (yfs_filesystem_t *)calloc(1, sizeof(yfs_filesystem_t));
    if (!fs) return nullptr;

    strncpy(fs->dev_path, dev_path, sizeof(fs->dev_path) - 1);
    pthread_mutex_init(&fs->lock, nullptr);
    return fs;
}

void yfs_fs_destroy(yfs_filesystem_t *fs) {
    if (!fs) return;
    yfs_fs_unmount(fs);
    pthread_mutex_destroy(&fs->lock);
    free(fs);
}

bool yfs_fs_mount(yfs_filesystem_t *fs, size_t cache_max_blocks) {
    if (!fs) return false;
    pthread_mutex_lock(&fs->lock);

    fs->dev = block_dev_open(fs->dev_path, false, 0);
    if (!fs->dev) {
        pthread_mutex_unlock(&fs->lock);
        return false;
    }

    size_t c_blocks = cache_max_blocks > 0 ? cache_max_blocks : 65536;
    fs->cache = buffer_cache_create(fs->dev, YFS_DEFAULT_BSIZE, c_blocks);

    if (!load_superblock(fs)) {
        pthread_mutex_unlock(&fs->lock);
        return false;
    }

    fs->journal = journal_create(fs->dev, fs->sb.s_journal_start_blk, fs->sb.s_journal_blocks, fs->sb.s_bsize);
    if (!journal_load(fs->journal)) {
        pthread_mutex_unlock(&fs->lock);
        return false;
    }

    if (fs->sb.s_clean_unmount == 0) {
        journal_recover(fs->journal, fs->dev);
        buffer_cache_invalidate(fs->cache);
        load_superblock(fs);
    }

    fs->tx_mgr = tx_manager_create(fs->journal, fs->cache);

    if (!load_cgroups(fs)) {
        pthread_mutex_unlock(&fs->lock);
        return false;
    }

    fs->sb.s_clean_unmount = 0;
    transaction_t *tx = tx_begin(fs->tx_mgr);
    sync_superblock(fs, tx);
    tx_commit(tx);
    buffer_cache_checkpoint_tx(fs->cache, journal_last_txid(fs->journal));

    pthread_mutex_unlock(&fs->lock);
    return true;
}

void yfs_fs_unmount(yfs_filesystem_t *fs) {
    if (!fs) return;
    pthread_mutex_lock(&fs->lock);

    if (fs->tx_mgr) {
        fs->sb.s_clean_unmount = 1;
        transaction_t *tx = tx_begin(fs->tx_mgr);
        sync_superblock(fs, tx);
        tx_commit(tx);
        tx_manager_checkpoint(fs->tx_mgr);
        tx_manager_destroy(fs->tx_mgr);
        fs->tx_mgr = nullptr;
    }

    if (fs->cache) {
        buffer_cache_sync_all(fs->cache);
        buffer_cache_destroy(fs->cache);
        fs->cache = nullptr;
    }

    if (fs->journal) {
        journal_flush(fs->journal);
        journal_destroy(fs->journal);
        fs->journal = nullptr;
    }

    if (fs->cgroups) {
        free(fs->cgroups);
        fs->cgroups = nullptr;
    }

    if (fs->dev) {
        block_dev_close(fs->dev);
        fs->dev = nullptr;
    }

    pthread_mutex_unlock(&fs->lock);
}

bool yfs_read_inode(yfs_filesystem_t *fs, uint32_t ino, yfs_dinode_t *dinode) {
    if (!fs || !dinode) return false;
    uint32_t offset = 0;
    uint64_t blk = inode_to_block(fs, ino, &offset);
    if (blk == 0) return false;

    block_buffer_t *buf = buffer_cache_get(fs->cache, blk, true);
    if (!buf) return false;

    memcpy(dinode, buf->data + offset, sizeof(yfs_dinode_t));
    return true;
}

bool yfs_write_inode(yfs_filesystem_t *fs, transaction_t *tx, uint32_t ino, const yfs_dinode_t *dinode) {
    if (!fs || !dinode) return false;
    uint32_t offset = 0;
    uint64_t blk = inode_to_block(fs, ino, &offset);
    if (blk == 0) return false;

    block_buffer_t *buf = buffer_cache_get(fs->cache, blk, true);
    if (!buf) return false;

    memcpy(buf->data + offset, dinode, sizeof(yfs_dinode_t));
    if (tx) {
        tx_modify_block(tx, buf);
    } else {
        buffer_cache_mark_dirty(fs->cache, buf, 0);
    }
    return true;
}

uint32_t yfs_alloc_inode(yfs_filesystem_t *fs, transaction_t *tx, mode_t mode) {
    if (fs->sb.s_free_inodes == 0) return 0;

    for (uint32_t cg_idx = 0; cg_idx < fs->sb.s_cgroup_count; ++cg_idx) {
        if (fs->cgroups[cg_idx].cg_free_inodes == 0) continue;

        block_buffer_t *buf = buffer_cache_get(fs->cache, fs->cgroups[cg_idx].cg_inode_bitmap, true);
        if (!buf) continue;

        uint8_t *bitmap = buf->data;
        for (uint32_t i = 0; i < fs->sb.s_inodes_per_cg; ++i) {
            uint32_t byte_idx = i / 8;
            uint8_t bit_mask = (uint8_t)(1 << (i % 8));

            if ((bitmap[byte_idx] & bit_mask) == 0) {
                bitmap[byte_idx] |= bit_mask;
                tx_modify_block(tx, buf);

                fs->cgroups[cg_idx].cg_free_inodes--;
                sync_cgroup(fs, tx, cg_idx);

                fs->sb.s_free_inodes--;
                sync_superblock(fs, tx);

                uint32_t allocated_ino = cg_idx * fs->sb.s_inodes_per_cg + i + 1;

                yfs_dinode_t dinode;
                memset(&dinode, 0, sizeof(dinode));
                dinode.di_mode = mode;
                dinode.di_nlink = 1;
                time_t now = time(nullptr);
                dinode.di_atime = (uint64_t)now;
                dinode.di_mtime = (uint64_t)now;
                dinode.di_ctime = (uint64_t)now;
                yfs_write_inode(fs, tx, allocated_ino, &dinode);

                return allocated_ino;
            }
        }
    }
    return 0;
}

bool yfs_free_inode(yfs_filesystem_t *fs, transaction_t *tx, uint32_t ino) {
    if (ino == 0 || ino > fs->sb.s_inodes_count) return false;
    uint32_t idx = ino - 1;
    uint32_t cg_idx = idx / fs->sb.s_inodes_per_cg;
    uint32_t ino_in_cg = idx % fs->sb.s_inodes_per_cg;

    block_buffer_t *buf = buffer_cache_get(fs->cache, fs->cgroups[cg_idx].cg_inode_bitmap, true);
    if (!buf) return false;

    uint8_t *bitmap = buf->data;
    uint32_t byte_idx = ino_in_cg / 8;
    uint8_t bit_mask = (uint8_t)(1 << (ino_in_cg % 8));

    bitmap[byte_idx] &= ~bit_mask;
    tx_modify_block(tx, buf);

    fs->cgroups[cg_idx].cg_free_inodes++;
    sync_cgroup(fs, tx, cg_idx);

    fs->sb.s_free_inodes++;
    sync_superblock(fs, tx);

    yfs_dinode_t dinode;
    memset(&dinode, 0, sizeof(dinode));
    yfs_write_inode(fs, tx, ino, &dinode);

    return true;
}

uint32_t yfs_alloc_block(yfs_filesystem_t *fs, transaction_t *tx) {
    if (fs->sb.s_free_blocks == 0) return 0;

    for (uint32_t cg_idx = 0; cg_idx < fs->sb.s_cgroup_count; ++cg_idx) {
        if (fs->cgroups[cg_idx].cg_free_blocks == 0) continue;

        block_buffer_t *buf = buffer_cache_get(fs->cache, fs->cgroups[cg_idx].cg_block_bitmap, true);
        if (!buf) continue;

        uint8_t *bitmap = buf->data;
        for (uint32_t i = 0; i < fs->cgroups[cg_idx].cg_data_blocks; ++i) {
            uint32_t byte_idx = i / 8;
            uint8_t bit_mask = (uint8_t)(1 << (i % 8));

            if ((bitmap[byte_idx] & bit_mask) == 0) {
                bitmap[byte_idx] |= bit_mask;
                tx_modify_block(tx, buf);

                fs->cgroups[cg_idx].cg_free_blocks--;
                sync_cgroup(fs, tx, cg_idx);

                fs->sb.s_free_blocks--;
                sync_superblock(fs, tx);

                uint32_t blk = (uint32_t)(fs->cgroups[cg_idx].cg_data_start + i);
                block_buffer_t *data_buf = buffer_cache_get(fs->cache, blk, false);
                if (data_buf) {
                    memset(data_buf->data, 0, fs->sb.s_bsize);
                    tx_modify_block(tx, data_buf);
                }
                return blk;
            }
        }
    }
    return 0;
}

bool yfs_free_block(yfs_filesystem_t *fs, transaction_t *tx, uint32_t blk_no) {
    if (blk_no == 0 || blk_no >= fs->sb.s_blocks_count) return false;

    for (uint32_t cg_idx = 0; cg_idx < fs->sb.s_cgroup_count; ++cg_idx) {
        if (blk_no >= fs->cgroups[cg_idx].cg_data_start &&
            blk_no < fs->cgroups[cg_idx].cg_data_start + fs->cgroups[cg_idx].cg_data_blocks) {

            uint32_t offset = (uint32_t)(blk_no - fs->cgroups[cg_idx].cg_data_start);
            block_buffer_t *buf = buffer_cache_get(fs->cache, fs->cgroups[cg_idx].cg_block_bitmap, true);
            if (!buf) return false;

            uint8_t *bitmap = buf->data;
            uint32_t byte_idx = offset / 8;
            uint8_t bit_mask = (uint8_t)(1 << (offset % 8));

            bitmap[byte_idx] &= ~bit_mask;
            tx_modify_block(tx, buf);

            fs->cgroups[cg_idx].cg_free_blocks++;
            sync_cgroup(fs, tx, cg_idx);

            fs->sb.s_free_blocks++;
            sync_superblock(fs, tx);

            return true;
        }
    }
    return false;
}

uint32_t yfs_bmap(yfs_filesystem_t *fs, transaction_t *tx, yfs_dinode_t *dinode, uint32_t ino, uint32_t logical_blk, bool create_if_missing) {
    if (logical_blk < YFS_DIRECT_BLOCKS) {
        if (dinode->di_direct[logical_blk] == 0 && create_if_missing) {
            uint32_t blk = yfs_alloc_block(fs, tx);
            if (blk == 0) return 0;
            dinode->di_direct[logical_blk] = blk;
            yfs_write_inode(fs, tx, ino, dinode);
        }
        return dinode->di_direct[logical_blk];
    }

    logical_blk -= YFS_DIRECT_BLOCKS;
    if (logical_blk < YFS_INDIR_PER_BLOCK) {
        if (dinode->di_indirect == 0) {
            if (!create_if_missing) return 0;
            uint32_t blk = yfs_alloc_block(fs, tx);
            if (blk == 0) return 0;
            dinode->di_indirect = blk;
            yfs_write_inode(fs, tx, ino, dinode);
        }

        block_buffer_t *buf = buffer_cache_get(fs->cache, dinode->di_indirect, true);
        if (!buf) return 0;
        uint32_t *table = (uint32_t *)buf->data;

        if (table[logical_blk] == 0 && create_if_missing) {
            uint32_t blk = yfs_alloc_block(fs, tx);
            if (blk == 0) return 0;
            table[logical_blk] = blk;
            tx_modify_block(tx, buf);
        }
        return table[logical_blk];
    }

    logical_blk -= YFS_INDIR_PER_BLOCK;
    uint32_t d1 = logical_blk / YFS_INDIR_PER_BLOCK;
    uint32_t d2 = logical_blk % YFS_INDIR_PER_BLOCK;

    if (d1 >= YFS_INDIR_PER_BLOCK) return 0;

    if (dinode->di_double_indirect == 0) {
        if (!create_if_missing) return 0;
        uint32_t blk = yfs_alloc_block(fs, tx);
        if (blk == 0) return 0;
        dinode->di_double_indirect = blk;
        yfs_write_inode(fs, tx, ino, dinode);
    }

    block_buffer_t *buf1 = buffer_cache_get(fs->cache, dinode->di_double_indirect, true);
    if (!buf1) return 0;
    uint32_t *table1 = (uint32_t *)buf1->data;

    if (table1[d1] == 0) {
        if (!create_if_missing) return 0;
        uint32_t blk = yfs_alloc_block(fs, tx);
        if (blk == 0) return 0;
        table1[d1] = blk;
        tx_modify_block(tx, buf1);
    }

    block_buffer_t *buf2 = buffer_cache_get(fs->cache, table1[d1], true);
    if (!buf2) return 0;
    uint32_t *table2 = (uint32_t *)buf2->data;

    if (table2[d2] == 0 && create_if_missing) {
        uint32_t blk = yfs_alloc_block(fs, tx);
        if (blk == 0) return 0;
        table2[d2] = blk;
        tx_modify_block(tx, buf2);
    }

    return table2[d2];
}

int yfs_lookup(yfs_filesystem_t *fs, uint32_t parent_ino, const char *name, uint32_t *out_ino, yfs_dinode_t *out_dinode) {
    if (!fs || !name) return -EINVAL;
    pthread_mutex_lock(&fs->lock);

    yfs_dinode_t pinode;
    if (!yfs_read_inode(fs, parent_ino, &pinode)) {
        pthread_mutex_unlock(&fs->lock);
        return -ENOENT;
    }
    if ((pinode.di_mode & YFS_IFDIR) == 0) {
        pthread_mutex_unlock(&fs->lock);
        return -ENOTDIR;
    }

    uint64_t offset = 0;
    size_t dirents_per_block = fs->sb.s_bsize / sizeof(yfs_dirent_t);

    for (uint32_t lblk = 0; offset < pinode.di_size; ++lblk) {
        uint32_t pblk = yfs_bmap(fs, nullptr, &pinode, parent_ino, lblk, false);
        if (pblk == 0) break;

        block_buffer_t *buf = buffer_cache_get(fs->cache, pblk, false);
        if (!buf) break;

        yfs_dirent_t *darr = (yfs_dirent_t *)buf->data;
        for (size_t i = 0; i < dirents_per_block && offset < pinode.di_size; ++i) {
            if (darr[i].d_ino != 0) {
                if (strcmp(name, darr[i].d_name) == 0) {
                    uint32_t found_ino = darr[i].d_ino;
                    if (out_ino) *out_ino = found_ino;
                    if (out_dinode) yfs_read_inode(fs, found_ino, out_dinode);
                    pthread_mutex_unlock(&fs->lock);
                    return 0;
                }
            }
            offset += sizeof(yfs_dirent_t);
        }
    }

    pthread_mutex_unlock(&fs->lock);
    return -ENOENT;
}

int yfs_getattr(yfs_filesystem_t *fs, uint32_t ino, yfs_dinode_t *out_dinode) {
    if (!fs || !out_dinode) return -EINVAL;
    pthread_mutex_lock(&fs->lock);
    if (!yfs_read_inode(fs, ino, out_dinode)) {
        pthread_mutex_unlock(&fs->lock);
        return -ENOENT;
    }
    pthread_mutex_unlock(&fs->lock);
    return 0;
}

int yfs_setattr(yfs_filesystem_t *fs, uint32_t ino, const yfs_dinode_t *dinode, int to_set) {
    if (!fs || !dinode) return -EINVAL;
    pthread_mutex_lock(&fs->lock);

    yfs_dinode_t cur;
    if (!yfs_read_inode(fs, ino, &cur)) {
        pthread_mutex_unlock(&fs->lock);
        return -ENOENT;
    }

    transaction_t *tx = tx_begin(fs->tx_mgr);

    if (to_set & 1) cur.di_mode = dinode->di_mode;
    if (to_set & 2) cur.di_uid = dinode->di_uid;
    if (to_set & 4) cur.di_gid = dinode->di_gid;
    if (to_set & 8) cur.di_size = dinode->di_size;
    if (to_set & 16) cur.di_atime = dinode->di_atime;
    if (to_set & 32) cur.di_mtime = dinode->di_mtime;

    yfs_write_inode(fs, tx, ino, &cur);
    tx_commit(tx);

    pthread_mutex_unlock(&fs->lock);
    return 0;
}

int yfs_create(yfs_filesystem_t *fs, uint32_t parent_ino, const char *name, mode_t mode, uint32_t uid, uint32_t gid, uint32_t *out_ino, yfs_dinode_t *out_dinode) {
    if (!fs || !name) return -EINVAL;
    pthread_mutex_lock(&fs->lock);

    yfs_dinode_t pinode;
    if (!yfs_read_inode(fs, parent_ino, &pinode)) {
        pthread_mutex_unlock(&fs->lock);
        return -ENOENT;
    }
    if ((pinode.di_mode & YFS_IFDIR) == 0) {
        pthread_mutex_unlock(&fs->lock);
        return -ENOTDIR;
    }

    transaction_t *tx = tx_begin(fs->tx_mgr);
    uint32_t new_ino = yfs_alloc_inode(fs, tx, (mode & 07777) | YFS_IFREG);
    if (new_ino == 0) {
        tx_abort(tx);
        pthread_mutex_unlock(&fs->lock);
        return -ENOSPC;
    }

    yfs_dinode_t new_dinode;
    yfs_read_inode(fs, new_ino, &new_dinode);
    new_dinode.di_uid = uid;
    new_dinode.di_gid = gid;
    yfs_write_inode(fs, tx, new_ino, &new_dinode);

    /* Look for an empty directory slot before appending */
    size_t dirents_per_block = fs->sb.s_bsize / sizeof(yfs_dirent_t);
    uint64_t cur_off = 0;
    bool slot_found = false;

    for (uint32_t lblk = 0; cur_off < pinode.di_size; ++lblk) {
        uint32_t pblk = yfs_bmap(fs, tx, &pinode, parent_ino, lblk, false);
        if (pblk == 0) break;

        block_buffer_t *buf = buffer_cache_get(fs->cache, pblk, false);
        if (!buf) break;

        yfs_dirent_t *darr = (yfs_dirent_t *)buf->data;
        for (size_t i = 0; i < dirents_per_block && cur_off < pinode.di_size; ++i) {
            if (darr[i].d_ino == 0) {
                darr[i].d_ino = new_ino;
                darr[i].d_type = 1; /* DT_REG */
                size_t name_len = strlen(name);
                darr[i].d_namlen = (uint8_t)(name_len < YFS_MAX_NAME_LEN ? name_len : YFS_MAX_NAME_LEN);
                strncpy(darr[i].d_name, name, YFS_MAX_NAME_LEN);
                darr[i].d_name[YFS_MAX_NAME_LEN] = '\0';
                darr[i].d_reclen = sizeof(yfs_dirent_t);

                tx_modify_block(tx, buf);
                pinode.di_mtime = (uint64_t)time(nullptr);
                yfs_write_inode(fs, tx, parent_ino, &pinode);
                slot_found = true;
                break;
            }
            cur_off += sizeof(yfs_dirent_t);
        }
        if (slot_found) break;
    }

    if (!slot_found) {
        uint32_t lblk = (uint32_t)(pinode.di_size / fs->sb.s_bsize);
        size_t offset_in_blk = pinode.di_size % fs->sb.s_bsize;
        size_t entry_idx = offset_in_blk / sizeof(yfs_dirent_t);

        uint32_t pblk = yfs_bmap(fs, tx, &pinode, parent_ino, lblk, true);
        if (pblk == 0) {
            tx_abort(tx);
            pthread_mutex_unlock(&fs->lock);
            return -ENOSPC;
        }

        block_buffer_t *buf = buffer_cache_get(fs->cache, pblk, false);
        yfs_dirent_t *darr = (yfs_dirent_t *)buf->data;

        darr[entry_idx].d_ino = new_ino;
        darr[entry_idx].d_type = 1; /* DT_REG */
        size_t name_len = strlen(name);
        darr[entry_idx].d_namlen = (uint8_t)(name_len < YFS_MAX_NAME_LEN ? name_len : YFS_MAX_NAME_LEN);
        strncpy(darr[entry_idx].d_name, name, YFS_MAX_NAME_LEN);
        darr[entry_idx].d_name[YFS_MAX_NAME_LEN] = '\0';
        darr[entry_idx].d_reclen = sizeof(yfs_dirent_t);

        tx_modify_block(tx, buf);

        pinode.di_size += sizeof(yfs_dirent_t);
        pinode.di_mtime = (uint64_t)time(nullptr);
        yfs_write_inode(fs, tx, parent_ino, &pinode);
    }

    tx_commit(tx);

    if (out_ino) *out_ino = new_ino;
    if (out_dinode) *out_dinode = new_dinode;

    pthread_mutex_unlock(&fs->lock);
    return 0;
}

int yfs_mknod(yfs_filesystem_t *fs, uint32_t parent_ino, const char *name, mode_t mode, dev_t rdev, uint32_t uid, uint32_t gid, uint32_t *out_ino, yfs_dinode_t *out_dinode) {
    (void)rdev;
    return yfs_create(fs, parent_ino, name, mode, uid, gid, out_ino, out_dinode);
}

int yfs_symlink(yfs_filesystem_t *fs, const char *target, uint32_t parent_ino, const char *name, uint32_t uid, uint32_t gid, uint32_t *out_ino, yfs_dinode_t *out_dinode) {
    if (!fs || !target || !name) return -EINVAL;
    pthread_mutex_lock(&fs->lock);

    yfs_dinode_t pinode;
    if (!yfs_read_inode(fs, parent_ino, &pinode)) {
        pthread_mutex_unlock(&fs->lock);
        return -ENOENT;
    }
    if ((pinode.di_mode & YFS_IFDIR) == 0) {
        pthread_mutex_unlock(&fs->lock);
        return -ENOTDIR;
    }

    transaction_t *tx = tx_begin(fs->tx_mgr);
    uint32_t new_ino = yfs_alloc_inode(fs, tx, 0777 | YFS_IFLNK);
    if (new_ino == 0) {
        tx_abort(tx);
        pthread_mutex_unlock(&fs->lock);
        return -ENOSPC;
    }

    yfs_dinode_t new_dinode;
    yfs_read_inode(fs, new_ino, &new_dinode);
    new_dinode.di_uid = uid;
    new_dinode.di_gid = gid;

    size_t target_len = strlen(target);
    uint32_t pblk = yfs_bmap(fs, tx, &new_dinode, new_ino, 0, true);
    if (pblk == 0) {
        tx_abort(tx);
        pthread_mutex_unlock(&fs->lock);
        return -ENOSPC;
    }

    block_buffer_t *target_buf = buffer_cache_get(fs->cache, pblk, false);
    memcpy(target_buf->data, target, target_len);
    buffer_cache_mark_dirty(fs->cache, target_buf, tx->txid);

    new_dinode.di_size = target_len;
    new_dinode.di_mtime = (uint64_t)time(nullptr);
    yfs_write_inode(fs, tx, new_ino, &new_dinode);

    /* Append to parent dir */
    uint32_t lblk = (uint32_t)(pinode.di_size / fs->sb.s_bsize);
    size_t offset_in_blk = pinode.di_size % fs->sb.s_bsize;
    size_t entry_idx = offset_in_blk / sizeof(yfs_dirent_t);

    uint32_t dir_pblk = yfs_bmap(fs, tx, &pinode, parent_ino, lblk, true);
    if (dir_pblk == 0) {
        tx_abort(tx);
        pthread_mutex_unlock(&fs->lock);
        return -ENOSPC;
    }

    block_buffer_t *buf = buffer_cache_get(fs->cache, dir_pblk, false);
    yfs_dirent_t *darr = (yfs_dirent_t *)buf->data;

    darr[entry_idx].d_ino = new_ino;
    darr[entry_idx].d_type = 3; /* DT_LNK */
    size_t name_len = strlen(name);
    darr[entry_idx].d_namlen = (uint8_t)(name_len < YFS_MAX_NAME_LEN ? name_len : YFS_MAX_NAME_LEN);
    strncpy(darr[entry_idx].d_name, name, YFS_MAX_NAME_LEN);
    darr[entry_idx].d_name[YFS_MAX_NAME_LEN] = '\0';
    darr[entry_idx].d_reclen = sizeof(yfs_dirent_t);

    tx_modify_block(tx, buf);

    pinode.di_size += sizeof(yfs_dirent_t);
    pinode.di_mtime = (uint64_t)time(nullptr);
    yfs_write_inode(fs, tx, parent_ino, &pinode);

    tx_commit(tx);

    if (out_ino) *out_ino = new_ino;
    if (out_dinode) *out_dinode = new_dinode;

    pthread_mutex_unlock(&fs->lock);
    return 0;
}

int yfs_readlink(yfs_filesystem_t *fs, uint32_t ino, char *buf, size_t size) {
    if (!fs || !buf || size == 0) return -EINVAL;
    pthread_mutex_lock(&fs->lock);

    yfs_dinode_t dinode;
    if (!yfs_read_inode(fs, ino, &dinode)) {
        pthread_mutex_unlock(&fs->lock);
        return -ENOENT;
    }
    if ((dinode.di_mode & YFS_IFLNK) == 0) {
        pthread_mutex_unlock(&fs->lock);
        return -EINVAL;
    }

    uint32_t pblk = yfs_bmap(fs, nullptr, &dinode, ino, 0, false);
    if (pblk == 0) {
        pthread_mutex_unlock(&fs->lock);
        return -EIO;
    }

    block_buffer_t *b = buffer_cache_get(fs->cache, pblk, false);
    if (!b) {
        pthread_mutex_unlock(&fs->lock);
        return -EIO;
    }

    size_t to_copy = dinode.di_size < (size - 1) ? dinode.di_size : (size - 1);
    memcpy(buf, b->data, to_copy);
    buf[to_copy] = '\0';

    pthread_mutex_unlock(&fs->lock);
    return 0;
}

int yfs_rename(yfs_filesystem_t *fs, uint32_t old_parent, const char *old_name, uint32_t new_parent, const char *new_name) {
    if (!fs || !old_name || !new_name) return -EINVAL;

    uint32_t target_ino = 0;
    yfs_dinode_t target_dinode;
    int ret = yfs_lookup(fs, old_parent, old_name, &target_ino, &target_dinode);
    if (ret != 0) return ret;

    /* If new target exists, unlink it first */
    uint32_t existing_ino = 0;
    yfs_dinode_t existing_dinode;
    if (yfs_lookup(fs, new_parent, new_name, &existing_ino, &existing_dinode) == 0) {
        if (existing_dinode.di_mode & YFS_IFDIR) {
            yfs_rmdir(fs, new_parent, new_name);
        } else {
            yfs_unlink(fs, new_parent, new_name);
        }
    }

    pthread_mutex_lock(&fs->lock);

    yfs_dinode_t old_pinode;
    if (!yfs_read_inode(fs, old_parent, &old_pinode)) {
        pthread_mutex_unlock(&fs->lock);
        return -ENOENT;
    }

    yfs_dinode_t new_pinode;
    if (!yfs_read_inode(fs, new_parent, &new_pinode)) {
        pthread_mutex_unlock(&fs->lock);
        return -ENOENT;
    }

    transaction_t *tx = tx_begin(fs->tx_mgr);

    /* 1. Add entry to new_parent */
    uint32_t lblk = (uint32_t)(new_pinode.di_size / fs->sb.s_bsize);
    size_t offset_in_blk = new_pinode.di_size % fs->sb.s_bsize;
    size_t entry_idx = offset_in_blk / sizeof(yfs_dirent_t);

    uint32_t pblk = yfs_bmap(fs, tx, &new_pinode, new_parent, lblk, true);
    if (pblk == 0) {
        tx_abort(tx);
        pthread_mutex_unlock(&fs->lock);
        return -ENOSPC;
    }

    block_buffer_t *buf = buffer_cache_get(fs->cache, pblk, false);
    yfs_dirent_t *darr = (yfs_dirent_t *)buf->data;

    darr[entry_idx].d_ino = target_ino;
    darr[entry_idx].d_type = (target_dinode.di_mode & YFS_IFDIR) ? 2 : ((target_dinode.di_mode & YFS_IFLNK) ? 3 : 1);
    size_t name_len = strlen(new_name);
    darr[entry_idx].d_namlen = (uint8_t)(name_len < YFS_MAX_NAME_LEN ? name_len : YFS_MAX_NAME_LEN);
    strncpy(darr[entry_idx].d_name, new_name, YFS_MAX_NAME_LEN);
    darr[entry_idx].d_name[YFS_MAX_NAME_LEN] = '\0';
    darr[entry_idx].d_reclen = sizeof(yfs_dirent_t);

    tx_modify_block(tx, buf);

    new_pinode.di_size += sizeof(yfs_dirent_t);
    new_pinode.di_mtime = (uint64_t)time(nullptr);
    yfs_write_inode(fs, tx, new_parent, &new_pinode);

    /* 2. Remove entry from old_parent */
    size_t dirents_per_block = fs->sb.s_bsize / sizeof(yfs_dirent_t);
    uint64_t offset = 0;

    for (uint32_t l = 0; offset < old_pinode.di_size; ++l) {
        uint32_t p = yfs_bmap(fs, tx, &old_pinode, old_parent, l, false);
        if (p == 0) break;

        block_buffer_t *old_buf = buffer_cache_get(fs->cache, p, false);
        if (!old_buf) break;

        yfs_dirent_t *old_darr = (yfs_dirent_t *)old_buf->data;
        for (size_t i = 0; i < dirents_per_block && offset < old_pinode.di_size; ++i) {
            if (old_darr[i].d_ino == target_ino && strcmp(old_name, old_darr[i].d_name) == 0) {
                old_darr[i].d_ino = 0;
                tx_modify_block(tx, old_buf);
                old_pinode.di_mtime = (uint64_t)time(nullptr);
                yfs_write_inode(fs, tx, old_parent, &old_pinode);
                break;
            }
            offset += sizeof(yfs_dirent_t);
        }
    }

    tx_commit(tx);
    pthread_mutex_unlock(&fs->lock);
    return 0;
}

int yfs_mkdir(yfs_filesystem_t *fs, uint32_t parent_ino, const char *name, mode_t mode, uint32_t uid, uint32_t gid, uint32_t *out_ino, yfs_dinode_t *out_dinode) {
    if (!fs || !name) return -EINVAL;
    pthread_mutex_lock(&fs->lock);

    yfs_dinode_t pinode;
    if (!yfs_read_inode(fs, parent_ino, &pinode)) {
        pthread_mutex_unlock(&fs->lock);
        return -ENOENT;
    }
    if ((pinode.di_mode & YFS_IFDIR) == 0) {
        pthread_mutex_unlock(&fs->lock);
        return -ENOTDIR;
    }

    transaction_t *tx = tx_begin(fs->tx_mgr);
    uint32_t new_ino = yfs_alloc_inode(fs, tx, (mode & 07777) | YFS_IFDIR);
    if (new_ino == 0) {
        tx_abort(tx);
        pthread_mutex_unlock(&fs->lock);
        return -ENOSPC;
    }

    yfs_dinode_t new_dinode;
    yfs_read_inode(fs, new_ino, &new_dinode);
    new_dinode.di_uid = uid;
    new_dinode.di_gid = gid;
    new_dinode.di_nlink = 2;

    uint32_t child_pblk = yfs_bmap(fs, tx, &new_dinode, new_ino, 0, true);
    if (child_pblk == 0) {
        tx_abort(tx);
        pthread_mutex_unlock(&fs->lock);
        return -ENOSPC;
    }

    block_buffer_t *cbuf = buffer_cache_get(fs->cache, child_pblk, false);
    yfs_dirent_t *cdarr = (yfs_dirent_t *)cbuf->data;

    /* . */
    cdarr[0].d_ino = new_ino;
    cdarr[0].d_type = 2; /* DT_DIR */
    cdarr[0].d_namlen = 1;
    strcpy(cdarr[0].d_name, ".");
    cdarr[0].d_reclen = sizeof(yfs_dirent_t);

    /* .. */
    cdarr[1].d_ino = parent_ino;
    cdarr[1].d_type = 2; /* DT_DIR */
    cdarr[1].d_namlen = 2;
    strcpy(cdarr[1].d_name, "..");
    cdarr[1].d_reclen = sizeof(yfs_dirent_t);

    tx_modify_block(tx, cbuf);
    new_dinode.di_size = 2 * sizeof(yfs_dirent_t);
    yfs_write_inode(fs, tx, new_ino, &new_dinode);

    /* Add entry to parent dir */
    uint32_t lblk = (uint32_t)(pinode.di_size / fs->sb.s_bsize);
    size_t offset_in_blk = pinode.di_size % fs->sb.s_bsize;
    size_t entry_idx = offset_in_blk / sizeof(yfs_dirent_t);

    uint32_t pblk = yfs_bmap(fs, tx, &pinode, parent_ino, lblk, true);
    if (pblk == 0) {
        tx_abort(tx);
        pthread_mutex_unlock(&fs->lock);
        return -ENOSPC;
    }

    block_buffer_t *buf = buffer_cache_get(fs->cache, pblk, false);
    yfs_dirent_t *darr = (yfs_dirent_t *)buf->data;

    darr[entry_idx].d_ino = new_ino;
    darr[entry_idx].d_type = 2; /* DT_DIR */
    size_t name_len = strlen(name);
    darr[entry_idx].d_namlen = (uint8_t)(name_len < YFS_MAX_NAME_LEN ? name_len : YFS_MAX_NAME_LEN);
    strncpy(darr[entry_idx].d_name, name, YFS_MAX_NAME_LEN);
    darr[entry_idx].d_name[YFS_MAX_NAME_LEN] = '\0';
    darr[entry_idx].d_reclen = sizeof(yfs_dirent_t);

    tx_modify_block(tx, buf);

    pinode.di_size += sizeof(yfs_dirent_t);
    pinode.di_nlink++;
    pinode.di_mtime = (uint64_t)time(nullptr);
    yfs_write_inode(fs, tx, parent_ino, &pinode);

    tx_commit(tx);

    if (out_ino) *out_ino = new_ino;
    if (out_dinode) *out_dinode = new_dinode;

    pthread_mutex_unlock(&fs->lock);
    return 0;
}

int yfs_unlink(yfs_filesystem_t *fs, uint32_t parent_ino, const char *name) {
    if (!fs || !name) return -EINVAL;
    pthread_mutex_lock(&fs->lock);

    yfs_dinode_t pinode;
    if (!yfs_read_inode(fs, parent_ino, &pinode)) {
        pthread_mutex_unlock(&fs->lock);
        return -ENOENT;
    }

    transaction_t *tx = tx_begin(fs->tx_mgr);
    size_t dirents_per_block = fs->sb.s_bsize / sizeof(yfs_dirent_t);
    uint64_t offset = 0;

    for (uint32_t lblk = 0; offset < pinode.di_size; ++lblk) {
        uint32_t pblk = yfs_bmap(fs, tx, &pinode, parent_ino, lblk, false);
        if (pblk == 0) break;

        block_buffer_t *buf = buffer_cache_get(fs->cache, pblk, false);
        if (!buf) break;

        yfs_dirent_t *darr = (yfs_dirent_t *)buf->data;
        for (size_t i = 0; i < dirents_per_block && offset < pinode.di_size; ++i) {
            if (darr[i].d_ino != 0 && strcmp(name, darr[i].d_name) == 0) {
                uint32_t target_ino = darr[i].d_ino;
                yfs_dinode_t tinode;
                yfs_read_inode(fs, target_ino, &tinode);

                darr[i].d_ino = 0;
                tx_modify_block(tx, buf);

                tinode.di_nlink--;
                if (tinode.di_nlink == 0) {
                    for (int b = 0; b < YFS_DIRECT_BLOCKS; ++b) {
                        if (tinode.di_direct[b] != 0) {
                            yfs_free_block(fs, tx, tinode.di_direct[b]);
                        }
                    }
                    yfs_free_inode(fs, tx, target_ino);
                } else {
                    yfs_write_inode(fs, tx, target_ino, &tinode);
                }

                pinode.di_mtime = (uint64_t)time(nullptr);
                yfs_write_inode(fs, tx, parent_ino, &pinode);

                tx_commit(tx);
                pthread_mutex_unlock(&fs->lock);
                return 0;
            }
            offset += sizeof(yfs_dirent_t);
        }
    }

    tx_abort(tx);
    pthread_mutex_unlock(&fs->lock);
    return -ENOENT;
}

int yfs_rmdir(yfs_filesystem_t *fs, uint32_t parent_ino, const char *name) {
    if (!fs || !name) return -EINVAL;

    uint32_t target_ino = 0;
    yfs_dinode_t target_dinode;
    int ret = yfs_lookup(fs, parent_ino, name, &target_ino, &target_dinode);
    if (ret != 0) return ret;

    if ((target_dinode.di_mode & YFS_IFDIR) == 0) return -ENOTDIR;

    yfs_dir_list_t list;
    yfs_dir_list_init(&list);
    yfs_readdir(fs, target_ino, &list);

    for (size_t i = 0; i < list.count; ++i) {
        if (strcmp(list.entries[i].name, ".") != 0 && strcmp(list.entries[i].name, "..") != 0) {
            yfs_dir_list_free(&list);
            return -ENOTEMPTY;
        }
    }
    yfs_dir_list_free(&list);

    return yfs_unlink(fs, parent_ino, name);
}

int yfs_readdir(yfs_filesystem_t *fs, uint32_t ino, yfs_dir_list_t *list) {
    if (!fs || !list) return -EINVAL;
    pthread_mutex_lock(&fs->lock);

    yfs_dinode_t dinode;
    if (!yfs_read_inode(fs, ino, &dinode)) {
        pthread_mutex_unlock(&fs->lock);
        return -ENOENT;
    }
    if ((dinode.di_mode & YFS_IFDIR) == 0) {
        pthread_mutex_unlock(&fs->lock);
        return -ENOTDIR;
    }

    uint64_t offset = 0;
    size_t dirents_per_block = fs->sb.s_bsize / sizeof(yfs_dirent_t);

    for (uint32_t lblk = 0; offset < dinode.di_size; ++lblk) {
        uint32_t pblk = yfs_bmap(fs, nullptr, &dinode, ino, lblk, false);
        if (pblk == 0) break;

        block_buffer_t *buf = buffer_cache_get(fs->cache, pblk, false);
        if (!buf) break;

        yfs_dirent_t *darr = (yfs_dirent_t *)buf->data;
        for (size_t i = 0; i < dirents_per_block && offset < dinode.di_size; ++i) {
            if (darr[i].d_ino != 0) {
                yfs_dir_list_add(list, darr[i].d_ino, darr[i].d_type, darr[i].d_name);
            }
            offset += sizeof(yfs_dirent_t);
        }
    }

    pthread_mutex_unlock(&fs->lock);
    return 0;
}

int yfs_read(yfs_filesystem_t *fs, uint32_t ino, void *buf, size_t size, off_t offset, size_t *bytes_read) {
    if (!fs || !buf) return -EINVAL;
    pthread_mutex_lock(&fs->lock);

    yfs_dinode_t dinode;
    if (!yfs_read_inode(fs, ino, &dinode)) {
        pthread_mutex_unlock(&fs->lock);
        return -ENOENT;
    }

    if (bytes_read) *bytes_read = 0;
    if (offset >= (off_t)dinode.di_size) {
        pthread_mutex_unlock(&fs->lock);
        return 0;
    }
    if (offset + size > dinode.di_size) {
        size = dinode.di_size - offset;
    }

    uint8_t *dst = (uint8_t *)buf;
    size_t remaining = size;
    off_t cur_offset = offset;
    size_t total_read = 0;

    while (remaining > 0) {
        uint32_t lblk = (uint32_t)(cur_offset / fs->sb.s_bsize);
        size_t offset_in_blk = cur_offset % fs->sb.s_bsize;
        size_t chunk = remaining < (fs->sb.s_bsize - offset_in_blk) ? remaining : (fs->sb.s_bsize - offset_in_blk);

        uint32_t pblk = yfs_bmap(fs, nullptr, &dinode, ino, lblk, false);
        if (pblk != 0) {
            block_buffer_t *blk_buf = buffer_cache_get(fs->cache, pblk, false);
            if (blk_buf) {
                memcpy(dst + total_read, blk_buf->data + offset_in_blk, chunk);
            } else {
                memset(dst + total_read, 0, chunk);
            }
        } else {
            memset(dst + total_read, 0, chunk);
        }

        total_read += chunk;
        remaining -= chunk;
        cur_offset += chunk;
    }

    if (bytes_read) *bytes_read = total_read;
    pthread_mutex_unlock(&fs->lock);
    return 0;
}

int yfs_write(yfs_filesystem_t *fs, uint32_t ino, const void *buf, size_t size, off_t offset, size_t *bytes_written) {
    if (!fs || !buf) return -EINVAL;
    pthread_mutex_lock(&fs->lock);

    yfs_dinode_t dinode;
    if (!yfs_read_inode(fs, ino, &dinode)) {
        pthread_mutex_unlock(&fs->lock);
        return -ENOENT;
    }

    transaction_t *tx = tx_begin(fs->tx_mgr);
    const uint8_t *src = (const uint8_t *)buf;
    size_t remaining = size;
    off_t cur_offset = offset;
    size_t total_written = 0;

    /* If writing beyond current EOF (creating a hole), zero out the gap */
    if ((uint64_t)offset > dinode.di_size) {
        uint64_t gap_start = dinode.di_size;
        uint64_t gap_len = offset - dinode.di_size;
        while (gap_len > 0) {
            uint32_t g_lblk = (uint32_t)(gap_start / fs->sb.s_bsize);
            size_t g_offset_in_blk = gap_start % fs->sb.s_bsize;
            size_t g_chunk = gap_len < (fs->sb.s_bsize - g_offset_in_blk) ? gap_len : (fs->sb.s_bsize - g_offset_in_blk);

            uint32_t pblk = yfs_bmap(fs, tx, &dinode, ino, g_lblk, false);
            if (pblk != 0) {
                block_buffer_t *blk_buf = buffer_cache_get(fs->cache, pblk, false);
                if (blk_buf) {
                    memset(blk_buf->data + g_offset_in_blk, 0, g_chunk);
                    buffer_cache_mark_dirty(fs->cache, blk_buf, tx->txid);
                }
            }
            gap_start += g_chunk;
            gap_len -= g_chunk;
        }
    }

    while (remaining > 0) {
        uint32_t lblk = (uint32_t)(cur_offset / fs->sb.s_bsize);
        size_t offset_in_blk = cur_offset % fs->sb.s_bsize;
        size_t chunk = remaining < (fs->sb.s_bsize - offset_in_blk) ? remaining : (fs->sb.s_bsize - offset_in_blk);

        uint32_t pblk = yfs_bmap(fs, tx, &dinode, ino, lblk, true);
        if (pblk == 0) {
            tx_abort(tx);
            pthread_mutex_unlock(&fs->lock);
            return -ENOSPC;
        }

        block_buffer_t *blk_buf = buffer_cache_get(fs->cache, pblk, false);
        memcpy(blk_buf->data + offset_in_blk, src + total_written, chunk);
        buffer_cache_mark_dirty(fs->cache, blk_buf, tx->txid);

        total_written += chunk;
        remaining -= chunk;
        cur_offset += chunk;
    }

    if (offset + size > dinode.di_size) {
        dinode.di_size = offset + size;
    }
    dinode.di_mtime = (uint64_t)time(nullptr);
    yfs_write_inode(fs, tx, ino, &dinode);

    tx_commit(tx);

    if (bytes_written) *bytes_written = total_written;
    pthread_mutex_unlock(&fs->lock);
    return 0;
}

int yfs_truncate(yfs_filesystem_t *fs, uint32_t ino, off_t new_size) {
    if (!fs) return -EINVAL;
    pthread_mutex_lock(&fs->lock);

    yfs_dinode_t dinode;
    if (!yfs_read_inode(fs, ino, &dinode)) {
        pthread_mutex_unlock(&fs->lock);
        return -ENOENT;
    }

    transaction_t *tx = tx_begin(fs->tx_mgr);

    if ((uint64_t)new_size > dinode.di_size) {
        uint64_t gap_start = dinode.di_size;
        uint64_t gap_len = new_size - dinode.di_size;
        while (gap_len > 0) {
            uint32_t g_lblk = (uint32_t)(gap_start / fs->sb.s_bsize);
            size_t g_offset_in_blk = gap_start % fs->sb.s_bsize;
            size_t g_chunk = gap_len < (fs->sb.s_bsize - g_offset_in_blk) ? gap_len : (fs->sb.s_bsize - g_offset_in_blk);

            uint32_t pblk = yfs_bmap(fs, tx, &dinode, ino, g_lblk, false);
            if (pblk != 0) {
                block_buffer_t *blk_buf = buffer_cache_get(fs->cache, pblk, false);
                if (blk_buf) {
                    memset(blk_buf->data + g_offset_in_blk, 0, g_chunk);
                    buffer_cache_mark_dirty(fs->cache, blk_buf, tx->txid);
                }
            }
            gap_start += g_chunk;
            gap_len -= g_chunk;
        }
    } else if ((uint64_t)new_size < dinode.di_size) {
        uint32_t old_last_lblk = (dinode.di_size > 0) ? (uint32_t)((dinode.di_size - 1) / fs->sb.s_bsize) : 0;
        uint32_t new_last_lblk = (new_size > 0) ? (uint32_t)((new_size - 1) / fs->sb.s_bsize) : 0;
        size_t new_offset_in_blk = new_size % fs->sb.s_bsize;

        if (new_size > 0 && new_offset_in_blk > 0) {
            uint32_t pblk = yfs_bmap(fs, tx, &dinode, ino, new_last_lblk, false);
            if (pblk != 0) {
                block_buffer_t *blk_buf = buffer_cache_get(fs->cache, pblk, false);
                if (blk_buf) {
                    memset(blk_buf->data + new_offset_in_blk, 0, fs->sb.s_bsize - new_offset_in_blk);
                    buffer_cache_mark_dirty(fs->cache, blk_buf, tx->txid);
                }
            }
        }

        uint32_t start_free_lblk = (new_size == 0) ? 0 : (new_last_lblk + 1);
        for (uint32_t lblk = start_free_lblk; lblk <= old_last_lblk && lblk < YFS_DIRECT_BLOCKS; ++lblk) {
            if (dinode.di_direct[lblk] != 0) {
                yfs_free_block(fs, tx, dinode.di_direct[lblk]);
                dinode.di_direct[lblk] = 0;
            }
        }
    }

    dinode.di_size = new_size;
    dinode.di_mtime = (uint64_t)time(nullptr);
    yfs_write_inode(fs, tx, ino, &dinode);
    tx_commit(tx);

    pthread_mutex_unlock(&fs->lock);
    return 0;
}

int yfs_statfs(yfs_filesystem_t *fs, uint64_t *total_blocks, uint64_t *free_blocks, uint64_t *total_inodes, uint64_t *free_inodes) {
    if (!fs) return -EINVAL;
    pthread_mutex_lock(&fs->lock);
    if (total_blocks) *total_blocks = fs->sb.s_blocks_count;
    if (free_blocks) *free_blocks = fs->sb.s_free_blocks;
    if (total_inodes) *total_inodes = fs->sb.s_inodes_count;
    if (free_inodes) *free_inodes = fs->sb.s_free_inodes;
    pthread_mutex_unlock(&fs->lock);
    return 0;
}
