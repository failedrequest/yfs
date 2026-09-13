#ifndef JOURNAL_H
#define JOURNAL_H

#include "yfs_fs.h"
#include "block_dev.h"
#include <vector>
#include <mutex>
#include <memory>

class Journal {
public:
    Journal(BlockDev *dev, uint64_t start_blk, uint64_t total_blocks, uint32_t bsize = YFS_DEFAULT_BSIZE);
    ~Journal();

    bool init_journal();
    bool load_journal();

    /* Write log record for transaction */
    bool write_tx_begin(uint64_t txid);
    bool write_block_redo(uint64_t txid, uint64_t target_blk, const void *data, uint32_t len);
    bool write_tx_commit(uint64_t txid);

    /* Flush journal writes to disk */
    bool flush();

    /* Checkpoint: advance tail to reclaimed tx */
    bool update_tail(uint64_t last_checkpointed_tx);

    /* Crash recovery: read and replay uncheckpointed committed transactions */
    bool recover(BlockDev *dev);

    uint64_t next_txid();
    uint64_t last_txid() const { return sb_.j_last_txid; }

private:
    BlockDev *dev_;
    uint64_t start_blk_;
    uint64_t total_blocks_;
    uint32_t bsize_;
    yfs_journal_sb sb_;
    std::mutex journal_lock_;

    bool write_journal_block(uint64_t log_offset, const void *buf);
    bool read_journal_block(uint64_t log_offset, void *buf);
    bool sync_sb();
    uint32_t compute_checksum(const void *buf, size_t len);
};

#endif /* JOURNAL_H */
