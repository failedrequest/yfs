#ifndef BLOCK_DEV_H
#define BLOCK_DEV_H

#include <string>
#include <cstdint>
#include <cstddef>
#include <mutex>

class BlockDev {
public:
    BlockDev();
    ~BlockDev();

    bool open_device(const std::string &path, bool create_if_missing = false, uint64_t size_bytes = 0);
    void close_device();

    bool read_block(uint64_t blk_no, void *buf, size_t blk_size = 4096);
    bool write_block(uint64_t blk_no, const void *buf, size_t blk_size = 4096);
    bool flush();

    uint64_t total_blocks(size_t blk_size = 4096) const;
    uint64_t size_in_bytes() const { return size_bytes_; }
    int fd() const { return fd_; }

private:
    int fd_;
    std::string path_;
    uint64_t size_bytes_;
    std::mutex dev_lock_;
};

#endif /* BLOCK_DEV_H */
