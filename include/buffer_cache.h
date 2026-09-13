#ifndef BUFFER_CACHE_H
#define BUFFER_CACHE_H

#include "block_dev.h"
#include <unordered_map>
#include <list>
#include <vector>
#include <mutex>
#include <memory>
#include <cstring>

struct BlockBuffer {
    uint64_t blk_no;
    bool is_dirty;
    bool is_metadata;
    uint64_t last_txid;     /* Transaction ID that modified this block */
    std::vector<uint8_t> data;

    BlockBuffer(uint64_t b, size_t size)
        : blk_no(b), is_dirty(false), is_metadata(false), last_txid(0), data(size, 0) {}
};

class BufferCache {
public:
    BufferCache(BlockDev *dev, size_t block_size = 4096, size_t max_blocks = 1024);
    ~BufferCache();

    /* Read a block from cache or disk */
    std::shared_ptr<BlockBuffer> get_block(uint64_t blk_no, bool is_metadata = false);

    /* Mark block as dirty and associate with current transaction ID */
    void mark_dirty(std::shared_ptr<BlockBuffer> buf, uint64_t txid = 0);

    /* Flush a specific buffer to disk */
    bool flush_block(std::shared_ptr<BlockBuffer> buf);

    /* Flush all dirty blocks up to a given transaction ID (checkpoint) */
    bool checkpoint_tx(uint64_t up_to_txid);

    /* Flush all dirty blocks in cache */
    bool sync_all();

    /* Invalidate cached blocks */
    void invalidate();

    size_t block_size() const { return block_size_; }

private:
    BlockDev *dev_;
    size_t block_size_;
    size_t max_blocks_;
    std::mutex cache_lock_;

    std::unordered_map<uint64_t, std::shared_ptr<BlockBuffer>> cache_map_;
    std::list<uint64_t> lru_list_;

    void evict_lru_unlocked();
};

#endif /* BUFFER_CACHE_H */
