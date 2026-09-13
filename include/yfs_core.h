#ifndef YFS_CORE_H
#define YFS_CORE_H

#include "yfs_fs.h"
#include "block_dev.h"
#include "buffer_cache.h"
#include "journal.h"
#include "tx_manager.h"
#include <string>
#include <vector>
#include <memory>
#include <mutex>

struct DirEntry {
    uint32_t ino;
    uint8_t type;
    std::string name;
};

class YFSFileSystem {
public:
    YFSFileSystem(const std::string &dev_path);
    ~YFSFileSystem();

    bool mount();
    void unmount();

    /* Filesystem-level operations */
    int lookup(uint32_t parent_ino, const std::string &name, uint32_t &out_ino, yfs_dinode &out_dinode);
    int getattr(uint32_t ino, yfs_dinode &out_dinode);
    int setattr(uint32_t ino, const yfs_dinode &dinode, int to_set);
    int create(uint32_t parent_ino, const std::string &name, mode_t mode, uint32_t uid, uint32_t gid, uint32_t &out_ino, yfs_dinode &out_dinode);
    int mkdir(uint32_t parent_ino, const std::string &name, mode_t mode, uint32_t uid, uint32_t gid, uint32_t &out_ino, yfs_dinode &out_dinode);
    int unlink(uint32_t parent_ino, const std::string &name);
    int rmdir(uint32_t parent_ino, const std::string &name);
    int readdir(uint32_t ino, std::vector<DirEntry> &entries);
    int read(uint32_t ino, void *buf, size_t size, off_t offset, size_t &bytes_read);
    int write(uint32_t ino, const void *buf, size_t size, off_t offset, size_t &bytes_written);
    int truncate(uint32_t ino, off_t new_size);
    int statfs(uint64_t &total_blocks, uint64_t &free_blocks, uint64_t &total_inodes, uint64_t &free_inodes);

    /* Inode and Block Allocations (Metadata updates) */
    uint32_t alloc_inode(std::shared_ptr<Transaction> tx, mode_t mode);
    bool free_inode(std::shared_ptr<Transaction> tx, uint32_t ino);
    uint32_t alloc_block(std::shared_ptr<Transaction> tx);
    bool free_block(std::shared_ptr<Transaction> tx, uint32_t blk_no);

    bool read_inode(uint32_t ino, yfs_dinode &dinode);
    bool write_inode(std::shared_ptr<Transaction> tx, uint32_t ino, const yfs_dinode &dinode);

    /* File block mapping */
    uint32_t bmap(std::shared_ptr<Transaction> tx, yfs_dinode &dinode, uint32_t ino, uint32_t logical_blk, bool create_if_missing);

    /* Block device & buffer cache access */
    BufferCache *cache() { return cache_.get(); }
    TxManager *tx_mgr() { return tx_mgr_.get(); }

private:
    std::string dev_path_;
    std::unique_ptr<BlockDev> dev_;
    std::unique_ptr<BufferCache> cache_;
    std::unique_ptr<Journal> journal_;
    std::unique_ptr<TxManager> tx_mgr_;
    yfs_superblock sb_;
    std::vector<yfs_cgroup> cgroups_;
    std::mutex fs_lock_;

    bool load_superblock();
    bool sync_superblock(std::shared_ptr<Transaction> tx);
    bool load_cgroups();
    bool sync_cgroup(std::shared_ptr<Transaction> tx, uint32_t cg_idx);
    uint64_t inode_to_block(uint32_t ino, uint32_t &offset_in_block);
};

#endif /* YFS_CORE_H */
