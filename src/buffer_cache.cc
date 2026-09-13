#include "include/buffer_cache.h"
#include <algorithm>
#include <iostream>

BufferCache::BufferCache(BlockDev *dev, size_t block_size, size_t max_blocks)
    : dev_(dev), block_size_(block_size), max_blocks_(max_blocks) {}

BufferCache::~BufferCache() {
    sync_all();
}

std::shared_ptr<BlockBuffer> BufferCache::get_block(uint64_t blk_no, bool is_metadata) {
    std::lock_guard<std::mutex> lock(cache_lock_);

    auto it = cache_map_.find(blk_no);
    if (it != cache_map_.end()) {
        /* Move to front of LRU */
        lru_list_.remove(blk_no);
        lru_list_.push_front(blk_no);
        return it->second;
    }

    /* Allocate and read from device */
    auto buf = std::make_shared<BlockBuffer>(blk_no, block_size_);
    buf->is_metadata = is_metadata;

    if (dev_) {
        dev_->read_block(blk_no, buf->data.data(), block_size_);
    }

    if (cache_map_.size() >= max_blocks_) {
        evict_lru_unlocked();
    }

    cache_map_[blk_no] = buf;
    lru_list_.push_front(blk_no);
    return buf;
}

void BufferCache::mark_dirty(std::shared_ptr<BlockBuffer> buf, uint64_t txid) {
    std::lock_guard<std::mutex> lock(cache_lock_);
    if (!buf) return;
    buf->is_dirty = true;
    if (txid > 0) {
        buf->last_txid = txid;
    }
}

bool BufferCache::flush_block(std::shared_ptr<BlockBuffer> buf) {
    if (!buf || !dev_) return false;
    if (buf->is_dirty) {
        bool ok = dev_->write_block(buf->blk_no, buf->data.data(), block_size_);
        if (ok) {
            buf->is_dirty = false;
        }
        return ok;
    }
    return true;
}

bool BufferCache::checkpoint_tx(uint64_t up_to_txid) {
    std::lock_guard<std::mutex> lock(cache_lock_);
    if (!dev_) return false;

    bool all_ok = true;
    for (auto &kv : cache_map_) {
        auto &buf = kv.second;
        if (buf->is_dirty && buf->last_txid <= up_to_txid) {
            if (!dev_->write_block(buf->blk_no, buf->data.data(), block_size_)) {
                all_ok = false;
            } else {
                buf->is_dirty = false;
            }
        }
    }
    dev_->flush();
    return all_ok;
}

bool BufferCache::sync_all() {
    std::lock_guard<std::mutex> lock(cache_lock_);
    if (!dev_) return false;

    bool all_ok = true;
    for (auto &kv : cache_map_) {
        auto &buf = kv.second;
        if (buf->is_dirty) {
            if (!dev_->write_block(buf->blk_no, buf->data.data(), block_size_)) {
                all_ok = false;
            } else {
                buf->is_dirty = false;
            }
        }
    }
    dev_->flush();
    return all_ok;
}

void BufferCache::invalidate() {
    std::lock_guard<std::mutex> lock(cache_lock_);
    sync_all();
    cache_map_.clear();
    lru_list_.clear();
}

void BufferCache::evict_lru_unlocked() {
    while (!lru_list_.empty() && cache_map_.size() >= max_blocks_) {
        uint64_t victim_blk = lru_list_.back();
        auto it = cache_map_.find(victim_blk);
        if (it != cache_map_.end()) {
            if (it->second->is_dirty && dev_) {
                dev_->write_block(victim_blk, it->second->data.data(), block_size_);
            }
            cache_map_.erase(it);
        }
        lru_list_.pop_back();
    }
}
