#include "include/block_dev.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>
#include <iostream>

BlockDev::BlockDev() : fd_(-1), size_bytes_(0) {}

BlockDev::~BlockDev() {
    close_device();
}

bool BlockDev::open_device(const std::string &path, bool create_if_missing, uint64_t size_bytes) {
    std::lock_guard<std::mutex> lock(dev_lock_);
    path_ = path;
    int flags = O_RDWR;
    if (create_if_missing) {
        flags |= O_CREAT;
    }

    fd_ = open(path.c_str(), flags, 0644);
    if (fd_ < 0) {
        return false;
    }

    struct stat st;
    if (fstat(fd_, &st) != 0) {
        close(fd_);
        fd_ = -1;
        return false;
    }

    if (S_ISREG(st.st_mode)) {
        if (create_if_missing && size_bytes > 0) {
            if (ftruncate(fd_, size_bytes) != 0) {
                close(fd_);
                fd_ = -1;
                return false;
            }
            size_bytes_ = size_bytes;
        } else {
            size_bytes_ = st.st_size;
        }
    } else {
        size_bytes_ = st.st_size;
    }

    return true;
}

void BlockDev::close_device() {
    std::lock_guard<std::mutex> lock(dev_lock_);
    if (fd_ >= 0) {
        fsync(fd_);
        close(fd_);
        fd_ = -1;
    }
}

bool BlockDev::read_block(uint64_t blk_no, void *buf, size_t blk_size) {
    std::lock_guard<std::mutex> lock(dev_lock_);
    if (fd_ < 0) return false;
    off_t offset = static_cast<off_t>(blk_no * blk_size);
    ssize_t ret = pread(fd_, buf, blk_size, offset);
    return (ret == static_cast<ssize_t>(blk_size));
}

bool BlockDev::write_block(uint64_t blk_no, const void *buf, size_t blk_size) {
    std::lock_guard<std::mutex> lock(dev_lock_);
    if (fd_ < 0) return false;
    off_t offset = static_cast<off_t>(blk_no * blk_size);
    ssize_t ret = pwrite(fd_, buf, blk_size, offset);
    return (ret == static_cast<ssize_t>(blk_size));
}

bool BlockDev::flush() {
    std::lock_guard<std::mutex> lock(dev_lock_);
    if (fd_ < 0) return false;
    return (fsync(fd_) == 0);
}

uint64_t BlockDev::total_blocks(size_t blk_size) const {
    if (blk_size == 0) return 0;
    return size_bytes_ / blk_size;
}
