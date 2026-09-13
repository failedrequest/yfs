#include "include/yfs_core.h"
#include <cstring>
#include <iostream>
#include <algorithm>
#include <ctime>
#include <errno.h>

YFSFileSystem::YFSFileSystem(const std::string &dev_path)
    : dev_path_(dev_path) {
    memset(&sb_, 0, sizeof(sb_));
}

YFSFileSystem::~YFSFileSystem() {
    unmount();
}

bool YFSFileSystem::mount() {
    std::lock_guard<std::mutex> lock(fs_lock_);
    dev_ = std::make_unique<BlockDev>();
    if (!dev_->open_device(dev_path_, false)) {
        return false;
    }

    cache_ = std::make_unique<BufferCache>(dev_.get(), YFS_DEFAULT_BSIZE, 2048);

    /* Read Superblock */
    if (!load_superblock()) {
        return false;
    }

    /* Initialize journal */
    journal_ = std::make_unique<Journal>(dev_.get(), sb_.s_journal_start_blk, sb_.s_journal_blocks, sb_.s_bsize);
    if (!journal_->load_journal()) {
        return false;
    }

    /* Check if recovery needed */
    if (sb_.s_clean_unmount == 0) {
        journal_->recover(dev_.get());
        cache_->invalidate();
        load_superblock();
    }

    tx_mgr_ = std::make_unique<TxManager>(journal_.get(), cache_.get());

    if (!load_cgroups()) {
        return false;
    }

    /* Mark dirty mount in superblock */
    sb_.s_clean_unmount = 0;
    auto tx = tx_mgr_->begin_transaction();
    sync_superblock(tx);
    tx->commit();
    cache_->checkpoint_tx(tx->txid());

    return true;
}

void YFSFileSystem::unmount() {
    std::lock_guard<std::mutex> lock(fs_lock_);
    if (!dev_) return;

    if (tx_mgr_) {
        sb_.s_clean_unmount = 1;
        auto tx = tx_mgr_->begin_transaction();
        sync_superblock(tx);
        tx->commit();
        tx_mgr_->checkpoint();
    }

    if (cache_) {
        cache_->sync_all();
    }

    if (journal_) {
        journal_->flush();
    }

    tx_mgr_.reset();
    journal_.reset();
    cache_.reset();
    dev_.reset();
}

bool YFSFileSystem::load_superblock() {
    auto buf = cache_->get_block(0, true);
    if (!buf) return false;
    memcpy(&sb_, buf->data.data(), sizeof(yfs_superblock));
    if (sb_.s_magic != YFS_MAGIC) {
        return false;
    }
    return true;
}

bool YFSFileSystem::sync_superblock(std::shared_ptr<Transaction> tx) {
    auto buf = cache_->get_block(0, true);
    if (!buf) return false;
    memcpy(buf->data.data(), &sb_, sizeof(yfs_superblock));
    if (tx) {
        tx->modify_block(buf);
    } else {
        cache_->mark_dirty(buf);
    }
    return true;
}

bool YFSFileSystem::load_cgroups() {
    cgroups_.resize(sb_.s_cgroup_count);
    size_t cg_per_block = sb_.s_bsize / sizeof(yfs_cgroup);

    for (uint32_t i = 0; i < sb_.s_cgroup_count; i += cg_per_block) {
        uint64_t blk = 1 + (i / cg_per_block);
        auto buf = cache_->get_block(blk, true);
        if (!buf) return false;

        yfs_cgroup *cg_arr = reinterpret_cast<yfs_cgroup *>(buf->data.data());
        for (size_t j = 0; j < cg_per_block && (i + j) < sb_.s_cgroup_count; ++j) {
            cgroups_[i + j] = cg_arr[j];
        }
    }
    return true;
}

bool YFSFileSystem::sync_cgroup(std::shared_ptr<Transaction> tx, uint32_t cg_idx) {
    if (cg_idx >= cgroups_.size()) return false;
    size_t cg_per_block = sb_.s_bsize / sizeof(yfs_cgroup);
    uint64_t blk = 1 + (cg_idx / cg_per_block);
    size_t offset_idx = cg_idx % cg_per_block;

    auto buf = cache_->get_block(blk, true);
    if (!buf) return false;

    yfs_cgroup *cg_arr = reinterpret_cast<yfs_cgroup *>(buf->data.data());
    cg_arr[offset_idx] = cgroups_[cg_idx];

    if (tx) {
        tx->modify_block(buf);
    } else {
        cache_->mark_dirty(buf);
    }
    return true;
}

uint64_t YFSFileSystem::inode_to_block(uint32_t ino, uint32_t &offset_in_block) {
    if (ino == 0 || ino > sb_.s_inodes_count) return 0;
    uint32_t idx = ino - 1;
    uint32_t cg_idx = idx / sb_.s_inodes_per_cg;
    uint32_t ino_in_cg = idx % sb_.s_inodes_per_cg;

    size_t inodes_per_block = sb_.s_bsize / sizeof(yfs_dinode);
    uint64_t blk = cgroups_[cg_idx].cg_inode_table + (ino_in_cg / inodes_per_block);
    offset_in_block = (ino_in_cg % inodes_per_block) * sizeof(yfs_dinode);
    return blk;
}

bool YFSFileSystem::read_inode(uint32_t ino, yfs_dinode &dinode) {
    uint32_t offset = 0;
    uint64_t blk = inode_to_block(ino, offset);
    if (blk == 0) return false;

    auto buf = cache_->get_block(blk, true);
    if (!buf) return false;

    memcpy(&dinode, buf->data.data() + offset, sizeof(yfs_dinode));
    return true;
}

bool YFSFileSystem::write_inode(std::shared_ptr<Transaction> tx, uint32_t ino, const yfs_dinode &dinode) {
    uint32_t offset = 0;
    uint64_t blk = inode_to_block(ino, offset);
    if (blk == 0) return false;

    auto buf = cache_->get_block(blk, true);
    if (!buf) return false;

    memcpy(buf->data.data() + offset, &dinode, sizeof(yfs_dinode));
    if (tx) {
        tx->modify_block(buf);
    } else {
        cache_->mark_dirty(buf);
    }
    return true;
}

uint32_t YFSFileSystem::alloc_inode(std::shared_ptr<Transaction> tx, mode_t mode) {
    if (sb_.s_free_inodes == 0) return 0;

    for (uint32_t cg_idx = 0; cg_idx < sb_.s_cgroup_count; ++cg_idx) {
        if (cgroups_[cg_idx].cg_free_inodes == 0) continue;

        auto buf = cache_->get_block(cgroups_[cg_idx].cg_inode_bitmap, true);
        if (!buf) continue;

        uint8_t *bitmap = buf->data.data();
        for (uint32_t i = 0; i < sb_.s_inodes_per_cg; ++i) {
            uint32_t byte_idx = i / 8;
            uint8_t bit_mask = 1 << (i % 8);

            if ((bitmap[byte_idx] & bit_mask) == 0) {
                /* Found free inode */
                bitmap[byte_idx] |= bit_mask;
                tx->modify_block(buf);

                cgroups_[cg_idx].cg_free_inodes--;
                sync_cgroup(tx, cg_idx);

                sb_.s_free_inodes--;
                sync_superblock(tx);

                uint32_t allocated_ino = cg_idx * sb_.s_inodes_per_cg + i + 1;

                /* Initialize inode on disk */
                yfs_dinode dinode;
                memset(&dinode, 0, sizeof(dinode));
                dinode.di_mode = mode;
                dinode.di_nlink = 1;
                time_t now = time(nullptr);
                dinode.di_atime = now;
                dinode.di_mtime = now;
                dinode.di_ctime = now;
                write_inode(tx, allocated_ino, dinode);

                return allocated_ino;
            }
        }
    }
    return 0;
}

bool YFSFileSystem::free_inode(std::shared_ptr<Transaction> tx, uint32_t ino) {
    if (ino == 0 || ino > sb_.s_inodes_count) return false;
    uint32_t idx = ino - 1;
    uint32_t cg_idx = idx / sb_.s_inodes_per_cg;
    uint32_t ino_in_cg = idx % sb_.s_inodes_per_cg;

    auto buf = cache_->get_block(cgroups_[cg_idx].cg_inode_bitmap, true);
    if (!buf) return false;

    uint8_t *bitmap = buf->data.data();
    uint32_t byte_idx = ino_in_cg / 8;
    uint8_t bit_mask = 1 << (ino_in_cg % 8);

    bitmap[byte_idx] &= ~bit_mask;
    tx->modify_block(buf);

    cgroups_[cg_idx].cg_free_inodes++;
    sync_cgroup(tx, cg_idx);

    sb_.s_free_inodes++;
    sync_superblock(tx);

    /* Zero out inode contents */
    yfs_dinode dinode;
    memset(&dinode, 0, sizeof(dinode));
    write_inode(tx, ino, dinode);

    return true;
}

uint32_t YFSFileSystem::alloc_block(std::shared_ptr<Transaction> tx) {
    if (sb_.s_free_blocks == 0) return 0;

    for (uint32_t cg_idx = 0; cg_idx < sb_.s_cgroup_count; ++cg_idx) {
        if (cgroups_[cg_idx].cg_free_blocks == 0) continue;

        auto buf = cache_->get_block(cgroups_[cg_idx].cg_block_bitmap, true);
        if (!buf) continue;

        uint8_t *bitmap = buf->data.data();
        for (uint32_t i = 0; i < cgroups_[cg_idx].cg_data_blocks; ++i) {
            uint32_t byte_idx = i / 8;
            uint8_t bit_mask = 1 << (i % 8);

            if ((bitmap[byte_idx] & bit_mask) == 0) {
                bitmap[byte_idx] |= bit_mask;
                tx->modify_block(buf);

                cgroups_[cg_idx].cg_free_blocks--;
                sync_cgroup(tx, cg_idx);

                sb_.s_free_blocks--;
                sync_superblock(tx);

                uint32_t blk = cgroups_[cg_idx].cg_data_start + i;
                /* Zero out allocated block */
                auto data_buf = cache_->get_block(blk, false);
                if (data_buf) {
                    std::fill(data_buf->data.begin(), data_buf->data.end(), 0);
                    tx->modify_block(data_buf);
                }
                return blk;
            }
        }
    }
    return 0;
}

bool YFSFileSystem::free_block(std::shared_ptr<Transaction> tx, uint32_t blk_no) {
    if (blk_no == 0 || blk_no >= sb_.s_blocks_count) return false;

    for (uint32_t cg_idx = 0; cg_idx < sb_.s_cgroup_count; ++cg_idx) {
        if (blk_no >= cgroups_[cg_idx].cg_data_start &&
            blk_no < cgroups_[cg_idx].cg_data_start + cgroups_[cg_idx].cg_data_blocks) {

            uint32_t offset = blk_no - cgroups_[cg_idx].cg_data_start;
            auto buf = cache_->get_block(cgroups_[cg_idx].cg_block_bitmap, true);
            if (!buf) return false;

            uint8_t *bitmap = buf->data.data();
            uint32_t byte_idx = offset / 8;
            uint8_t bit_mask = 1 << (offset % 8);

            bitmap[byte_idx] &= ~bit_mask;
            tx->modify_block(buf);

            cgroups_[cg_idx].cg_free_blocks++;
            sync_cgroup(tx, cg_idx);

            sb_.s_free_blocks++;
            sync_superblock(tx);

            return true;
        }
    }
    return false;
}

uint32_t YFSFileSystem::bmap(std::shared_ptr<Transaction> tx, yfs_dinode &dinode, uint32_t ino, uint32_t logical_blk, bool create_if_missing) {
    /* Direct blocks */
    if (logical_blk < YFS_DIRECT_BLOCKS) {
        if (dinode.di_direct[logical_blk] == 0 && create_if_missing) {
            uint32_t blk = alloc_block(tx);
            if (blk == 0) return 0;
            dinode.di_direct[logical_blk] = blk;
            write_inode(tx, ino, dinode);
        }
        return dinode.di_direct[logical_blk];
    }

    /* Single indirect blocks */
    logical_blk -= YFS_DIRECT_BLOCKS;
    if (logical_blk < YFS_INDIR_PER_BLOCK) {
        if (dinode.di_indirect == 0) {
            if (!create_if_missing) return 0;
            uint32_t blk = alloc_block(tx);
            if (blk == 0) return 0;
            dinode.di_indirect = blk;
            write_inode(tx, ino, dinode);
        }

        auto buf = cache_->get_block(dinode.di_indirect, true);
        if (!buf) return 0;
        uint32_t *table = reinterpret_cast<uint32_t *>(buf->data.data());

        if (table[logical_blk] == 0 && create_if_missing) {
            uint32_t blk = alloc_block(tx);
            if (blk == 0) return 0;
            table[logical_blk] = blk;
            tx->modify_block(buf);
        }
        return table[logical_blk];
    }

    /* Double indirect blocks */
    logical_blk -= YFS_INDIR_PER_BLOCK;
    uint32_t d1 = logical_blk / YFS_INDIR_PER_BLOCK;
    uint32_t d2 = logical_blk % YFS_INDIR_PER_BLOCK;

    if (d1 >= YFS_INDIR_PER_BLOCK) return 0; /* Exceeds max file size */

    if (dinode.di_double_indirect == 0) {
        if (!create_if_missing) return 0;
        uint32_t blk = alloc_block(tx);
        if (blk == 0) return 0;
        dinode.di_double_indirect = blk;
        write_inode(tx, ino, dinode);
    }

    auto buf1 = cache_->get_block(dinode.di_double_indirect, true);
    if (!buf1) return 0;
    uint32_t *table1 = reinterpret_cast<uint32_t *>(buf1->data.data());

    if (table1[d1] == 0) {
        if (!create_if_missing) return 0;
        uint32_t blk = alloc_block(tx);
        if (blk == 0) return 0;
        table1[d1] = blk;
        tx->modify_block(buf1);
    }

    auto buf2 = cache_->get_block(table1[d1], true);
    if (!buf2) return 0;
    uint32_t *table2 = reinterpret_cast<uint32_t *>(buf2->data.data());

    if (table2[d2] == 0 && create_if_missing) {
        uint32_t blk = alloc_block(tx);
        if (blk == 0) return 0;
        table2[d2] = blk;
        tx->modify_block(buf2);
    }

    return table2[d2];
}

int YFSFileSystem::lookup(uint32_t parent_ino, const std::string &name, uint32_t &out_ino, yfs_dinode &out_dinode) {
    std::lock_guard<std::mutex> lock(fs_lock_);
    yfs_dinode pinode;
    if (!read_inode(parent_ino, pinode)) return -ENOENT;
    if ((pinode.di_mode & YFS_IFDIR) == 0) return -ENOTDIR;

    std::vector<DirEntry> entries;
    uint64_t offset = 0;
    size_t dirents_per_block = sb_.s_bsize / sizeof(yfs_dirent);

    for (uint32_t lblk = 0; offset < pinode.di_size; ++lblk) {
        uint32_t pblk = bmap(nullptr, pinode, parent_ino, lblk, false);
        if (pblk == 0) break;

        auto buf = cache_->get_block(pblk, false);
        if (!buf) break;

        yfs_dirent *darr = reinterpret_cast<yfs_dirent *>(buf->data.data());
        for (size_t i = 0; i < dirents_per_block && offset < pinode.di_size; ++i) {
            if (darr[i].d_ino != 0) {
                if (name == darr[i].d_name) {
                    out_ino = darr[i].d_ino;
                    read_inode(out_ino, out_dinode);
                    return 0;
                }
            }
            offset += sizeof(yfs_dirent);
        }
    }
    return -ENOENT;
}

int YFSFileSystem::getattr(uint32_t ino, yfs_dinode &out_dinode) {
    std::lock_guard<std::mutex> lock(fs_lock_);
    if (!read_inode(ino, out_dinode)) return -ENOENT;
    return 0;
}

int YFSFileSystem::setattr(uint32_t ino, const yfs_dinode &dinode, int to_set) {
    std::lock_guard<std::mutex> lock(fs_lock_);
    yfs_dinode cur;
    if (!read_inode(ino, cur)) return -ENOENT;

    auto tx = tx_mgr_->begin_transaction();

    if (to_set & 1) cur.di_mode = dinode.di_mode;
    if (to_set & 2) cur.di_uid = dinode.di_uid;
    if (to_set & 4) cur.di_gid = dinode.di_gid;
    if (to_set & 8) cur.di_size = dinode.di_size;
    if (to_set & 16) cur.di_atime = dinode.di_atime;
    if (to_set & 32) cur.di_mtime = dinode.di_mtime;

    write_inode(tx, ino, cur);
    tx->commit();
    return 0;
}

int YFSFileSystem::create(uint32_t parent_ino, const std::string &name, mode_t mode, uint32_t uid, uint32_t gid, uint32_t &out_ino, yfs_dinode &out_dinode) {
    std::lock_guard<std::mutex> lock(fs_lock_);
    yfs_dinode pinode;
    if (!read_inode(parent_ino, pinode)) return -ENOENT;
    if ((pinode.di_mode & YFS_IFDIR) == 0) return -ENOTDIR;

    auto tx = tx_mgr_->begin_transaction();
    uint32_t new_ino = alloc_inode(tx, (mode & 07777) | YFS_IFREG);
    if (new_ino == 0) {
        tx->abort();
        return -ENOSPC;
    }

    read_inode(new_ino, out_dinode);
    out_dinode.di_uid = uid;
    out_dinode.di_gid = gid;
    write_inode(tx, new_ino, out_dinode);

    /* Append to parent directory */
    uint32_t lblk = pinode.di_size / sb_.s_bsize;
    size_t offset_in_blk = pinode.di_size % sb_.s_bsize;
    size_t entry_idx = offset_in_blk / sizeof(yfs_dirent);

    uint32_t pblk = bmap(tx, pinode, parent_ino, lblk, true);
    if (pblk == 0) {
        tx->abort();
        return -ENOSPC;
    }

    auto buf = cache_->get_block(pblk, false);
    yfs_dirent *darr = reinterpret_cast<yfs_dirent *>(buf->data.data());

    darr[entry_idx].d_ino = new_ino;
    darr[entry_idx].d_type = 1; /* DT_REG */
    darr[entry_idx].d_namlen = std::min<uint8_t>(name.size(), YFS_MAX_NAME_LEN);
    strncpy(darr[entry_idx].d_name, name.c_str(), YFS_MAX_NAME_LEN);
    darr[entry_idx].d_name[YFS_MAX_NAME_LEN] = '\0';
    darr[entry_idx].d_reclen = sizeof(yfs_dirent);

    tx->modify_block(buf);

    pinode.di_size += sizeof(yfs_dirent);
    pinode.di_mtime = time(nullptr);
    write_inode(tx, parent_ino, pinode);

    tx->commit();
    out_ino = new_ino;
    return 0;
}

int YFSFileSystem::mkdir(uint32_t parent_ino, const std::string &name, mode_t mode, uint32_t uid, uint32_t gid, uint32_t &out_ino, yfs_dinode &out_dinode) {
    std::lock_guard<std::mutex> lock(fs_lock_);
    yfs_dinode pinode;
    if (!read_inode(parent_ino, pinode)) return -ENOENT;
    if ((pinode.di_mode & YFS_IFDIR) == 0) return -ENOTDIR;

    auto tx = tx_mgr_->begin_transaction();
    uint32_t new_ino = alloc_inode(tx, (mode & 07777) | YFS_IFDIR);
    if (new_ino == 0) {
        tx->abort();
        return -ENOSPC;
    }

    read_inode(new_ino, out_dinode);
    out_dinode.di_uid = uid;
    out_dinode.di_gid = gid;
    out_dinode.di_nlink = 2; /* . and parent */

    /* Add . and .. */
    uint32_t child_pblk = bmap(tx, out_dinode, new_ino, 0, true);
    if (child_pblk == 0) {
        tx->abort();
        return -ENOSPC;
    }

    auto cbuf = cache_->get_block(child_pblk, false);
    yfs_dirent *cdarr = reinterpret_cast<yfs_dirent *>(cbuf->data.data());

    /* . */
    cdarr[0].d_ino = new_ino;
    cdarr[0].d_type = 2; /* DT_DIR */
    cdarr[0].d_namlen = 1;
    strcpy(cdarr[0].d_name, ".");
    cdarr[0].d_reclen = sizeof(yfs_dirent);

    /* .. */
    cdarr[1].d_ino = parent_ino;
    cdarr[1].d_type = 2; /* DT_DIR */
    cdarr[1].d_namlen = 2;
    strcpy(cdarr[1].d_name, "..");
    cdarr[1].d_reclen = sizeof(yfs_dirent);

    tx->modify_block(cbuf);
    out_dinode.di_size = 2 * sizeof(yfs_dirent);
    write_inode(tx, new_ino, out_dinode);

    /* Add entry to parent dir */
    uint32_t lblk = pinode.di_size / sb_.s_bsize;
    size_t offset_in_blk = pinode.di_size % sb_.s_bsize;
    size_t entry_idx = offset_in_blk / sizeof(yfs_dirent);

    uint32_t pblk = bmap(tx, pinode, parent_ino, lblk, true);
    if (pblk == 0) {
        tx->abort();
        return -ENOSPC;
    }

    auto buf = cache_->get_block(pblk, false);
    yfs_dirent *darr = reinterpret_cast<yfs_dirent *>(buf->data.data());

    darr[entry_idx].d_ino = new_ino;
    darr[entry_idx].d_type = 2; /* DT_DIR */
    darr[entry_idx].d_namlen = std::min<uint8_t>(name.size(), YFS_MAX_NAME_LEN);
    strncpy(darr[entry_idx].d_name, name.c_str(), YFS_MAX_NAME_LEN);
    darr[entry_idx].d_name[YFS_MAX_NAME_LEN] = '\0';
    darr[entry_idx].d_reclen = sizeof(yfs_dirent);

    tx->modify_block(buf);

    pinode.di_size += sizeof(yfs_dirent);
    pinode.di_nlink++;
    pinode.di_mtime = time(nullptr);
    write_inode(tx, parent_ino, pinode);

    tx->commit();
    out_ino = new_ino;
    return 0;
}

int YFSFileSystem::unlink(uint32_t parent_ino, const std::string &name) {
    std::lock_guard<std::mutex> lock(fs_lock_);
    yfs_dinode pinode;
    if (!read_inode(parent_ino, pinode)) return -ENOENT;

    auto tx = tx_mgr_->begin_transaction();
    size_t dirents_per_block = sb_.s_bsize / sizeof(yfs_dirent);
    uint64_t offset = 0;

    for (uint32_t lblk = 0; offset < pinode.di_size; ++lblk) {
        uint32_t pblk = bmap(tx, pinode, parent_ino, lblk, false);
        if (pblk == 0) break;

        auto buf = cache_->get_block(pblk, false);
        if (!buf) break;

        yfs_dirent *darr = reinterpret_cast<yfs_dirent *>(buf->data.data());
        for (size_t i = 0; i < dirents_per_block && offset < pinode.di_size; ++i) {
            if (darr[i].d_ino != 0 && name == darr[i].d_name) {
                uint32_t target_ino = darr[i].d_ino;
                yfs_dinode tinode;
                read_inode(target_ino, tinode);

                /* Remove entry */
                darr[i].d_ino = 0;
                tx->modify_block(buf);

                tinode.di_nlink--;
                if (tinode.di_nlink == 0) {
                    /* Free data blocks */
                    for (int b = 0; b < YFS_DIRECT_BLOCKS; ++b) {
                        if (tinode.di_direct[b] != 0) {
                            free_block(tx, tinode.di_direct[b]);
                        }
                    }
                    free_inode(tx, target_ino);
                } else {
                    write_inode(tx, target_ino, tinode);
                }

                pinode.di_mtime = time(nullptr);
                write_inode(tx, parent_ino, pinode);

                tx->commit();
                return 0;
            }
            offset += sizeof(yfs_dirent);
        }
    }

    tx->abort();
    return -ENOENT;
}

int YFSFileSystem::rmdir(uint32_t parent_ino, const std::string &name) {
    std::vector<DirEntry> entries;
    uint32_t target_ino = 0;
    yfs_dinode target_dinode;
    int ret = lookup(parent_ino, name, target_ino, target_dinode);
    if (ret != 0) return ret;

    if ((target_dinode.di_mode & YFS_IFDIR) == 0) return -ENOTDIR;

    readdir(target_ino, entries);
    for (const auto &e : entries) {
        if (e.name != "." && e.name != "..") {
            return -ENOTEMPTY;
        }
    }

    return unlink(parent_ino, name);
}

int YFSFileSystem::readdir(uint32_t ino, std::vector<DirEntry> &entries) {
    std::lock_guard<std::mutex> lock(fs_lock_);
    yfs_dinode dinode;
    if (!read_inode(ino, dinode)) return -ENOENT;
    if ((dinode.di_mode & YFS_IFDIR) == 0) return -ENOTDIR;

    entries.clear();
    uint64_t offset = 0;
    size_t dirents_per_block = sb_.s_bsize / sizeof(yfs_dirent);

    for (uint32_t lblk = 0; offset < dinode.di_size; ++lblk) {
        uint32_t pblk = bmap(nullptr, dinode, ino, lblk, false);
        if (pblk == 0) break;

        auto buf = cache_->get_block(pblk, false);
        if (!buf) break;

        yfs_dirent *darr = reinterpret_cast<yfs_dirent *>(buf->data.data());
        for (size_t i = 0; i < dirents_per_block && offset < dinode.di_size; ++i) {
            if (darr[i].d_ino != 0) {
                DirEntry e;
                e.ino = darr[i].d_ino;
                e.type = darr[i].d_type;
                e.name = darr[i].d_name;
                entries.push_back(e);
            }
            offset += sizeof(yfs_dirent);
        }
    }
    return 0;
}

int YFSFileSystem::read(uint32_t ino, void *buf, size_t size, off_t offset, size_t &bytes_read) {
    std::lock_guard<std::mutex> lock(fs_lock_);
    yfs_dinode dinode;
    if (!read_inode(ino, dinode)) return -ENOENT;

    bytes_read = 0;
    if (offset >= static_cast<off_t>(dinode.di_size)) return 0;
    if (offset + size > dinode.di_size) {
        size = dinode.di_size - offset;
    }

    uint8_t *dst = static_cast<uint8_t *>(buf);
    size_t remaining = size;
    off_t cur_offset = offset;

    while (remaining > 0) {
        uint32_t lblk = cur_offset / sb_.s_bsize;
        size_t offset_in_blk = cur_offset % sb_.s_bsize;
        size_t chunk = std::min(remaining, sb_.s_bsize - offset_in_blk);

        uint32_t pblk = bmap(nullptr, dinode, ino, lblk, false);
        if (pblk != 0) {
            auto blk_buf = cache_->get_block(pblk, false);
            if (blk_buf) {
                memcpy(dst + bytes_read, blk_buf->data.data() + offset_in_blk, chunk);
            } else {
                memset(dst + bytes_read, 0, chunk);
            }
        } else {
            memset(dst + bytes_read, 0, chunk);
        }

        bytes_read += chunk;
        remaining -= chunk;
        cur_offset += chunk;
    }

    return 0;
}

int YFSFileSystem::write(uint32_t ino, const void *buf, size_t size, off_t offset, size_t &bytes_written) {
    std::lock_guard<std::mutex> lock(fs_lock_);
    yfs_dinode dinode;
    if (!read_inode(ino, dinode)) return -ENOENT;

    auto tx = tx_mgr_->begin_transaction();
    bytes_written = 0;
    const uint8_t *src = static_cast<const uint8_t *>(buf);
    size_t remaining = size;
    off_t cur_offset = offset;

    /* If writing beyond current EOF (creating a hole), zero out the gap */
    if (static_cast<uint64_t>(offset) > dinode.di_size) {
        uint64_t gap_start = dinode.di_size;
        uint64_t gap_len = offset - dinode.di_size;
        while (gap_len > 0) {
            uint32_t g_lblk = gap_start / sb_.s_bsize;
            size_t g_offset_in_blk = gap_start % sb_.s_bsize;
            size_t g_chunk = std::min<uint64_t>(gap_len, sb_.s_bsize - g_offset_in_blk);

            uint32_t pblk = bmap(tx, dinode, ino, g_lblk, false);
            if (pblk != 0) {
                auto blk_buf = cache_->get_block(pblk, false);
                if (blk_buf) {
                    memset(blk_buf->data.data() + g_offset_in_blk, 0, g_chunk);
                    cache_->mark_dirty(blk_buf, tx->txid());
                }
            }
            gap_start += g_chunk;
            gap_len -= g_chunk;
        }
    }

    while (remaining > 0) {
        uint32_t lblk = cur_offset / sb_.s_bsize;
        size_t offset_in_blk = cur_offset % sb_.s_bsize;
        size_t chunk = std::min(remaining, sb_.s_bsize - offset_in_blk);

        uint32_t pblk = bmap(tx, dinode, ino, lblk, true);
        if (pblk == 0) {
            tx->abort();
            return -ENOSPC;
        }

        auto blk_buf = cache_->get_block(pblk, false);
        memcpy(blk_buf->data.data() + offset_in_blk, src + bytes_written, chunk);
        cache_->mark_dirty(blk_buf, tx->txid());

        bytes_written += chunk;
        remaining -= chunk;
        cur_offset += chunk;
    }

    if (offset + size > dinode.di_size) {
        dinode.di_size = offset + size;
    }
    dinode.di_mtime = time(nullptr);
    write_inode(tx, ino, dinode);

    tx->commit();
    return 0;
}

int YFSFileSystem::truncate(uint32_t ino, off_t new_size) {
    std::lock_guard<std::mutex> lock(fs_lock_);
    yfs_dinode dinode;
    if (!read_inode(ino, dinode)) return -ENOENT;

    auto tx = tx_mgr_->begin_transaction();

    /* If expanding, zero out the extension gap */
    if (static_cast<uint64_t>(new_size) > dinode.di_size) {
        uint64_t gap_start = dinode.di_size;
        uint64_t gap_len = new_size - dinode.di_size;
        while (gap_len > 0) {
            uint32_t g_lblk = gap_start / sb_.s_bsize;
            size_t g_offset_in_blk = gap_start % sb_.s_bsize;
            size_t g_chunk = std::min<uint64_t>(gap_len, sb_.s_bsize - g_offset_in_blk);

            uint32_t pblk = bmap(tx, dinode, ino, g_lblk, false);
            if (pblk != 0) {
                auto blk_buf = cache_->get_block(pblk, false);
                if (blk_buf) {
                    memset(blk_buf->data.data() + g_offset_in_blk, 0, g_chunk);
                    cache_->mark_dirty(blk_buf, tx->txid());
                }
            }
            gap_start += g_chunk;
            gap_len -= g_chunk;
        }
    } else if (static_cast<uint64_t>(new_size) < dinode.di_size) {
        /* If shrinking, zero out partial tail block and free blocks beyond new_size */
        uint32_t old_last_lblk = (dinode.di_size > 0) ? (dinode.di_size - 1) / sb_.s_bsize : 0;
        uint32_t new_last_lblk = (new_size > 0) ? (new_size - 1) / sb_.s_bsize : 0;
        size_t new_offset_in_blk = new_size % sb_.s_bsize;

        /* Zero remainder of the last block */
        if (new_size > 0 && new_offset_in_blk > 0) {
            uint32_t pblk = bmap(tx, dinode, ino, new_last_lblk, false);
            if (pblk != 0) {
                auto blk_buf = cache_->get_block(pblk, false);
                if (blk_buf) {
                    memset(blk_buf->data.data() + new_offset_in_blk, 0, sb_.s_bsize - new_offset_in_blk);
                    cache_->mark_dirty(blk_buf, tx->txid());
                }
            }
        }

        /* Free truncated direct blocks */
        uint32_t start_free_lblk = (new_size == 0) ? 0 : (new_last_lblk + 1);
        for (uint32_t lblk = start_free_lblk; lblk <= old_last_lblk && lblk < YFS_DIRECT_BLOCKS; ++lblk) {
            if (dinode.di_direct[lblk] != 0) {
                free_block(tx, dinode.di_direct[lblk]);
                dinode.di_direct[lblk] = 0;
            }
        }
    }

    dinode.di_size = new_size;
    dinode.di_mtime = time(nullptr);
    write_inode(tx, ino, dinode);
    tx->commit();
    return 0;
}

int YFSFileSystem::statfs(uint64_t &total_blocks, uint64_t &free_blocks, uint64_t &total_inodes, uint64_t &free_inodes) {
    std::lock_guard<std::mutex> lock(fs_lock_);
    total_blocks = sb_.s_blocks_count;
    free_blocks = sb_.s_free_blocks;
    total_inodes = sb_.s_inodes_count;
    free_inodes = sb_.s_free_inodes;
    return 0;
}
