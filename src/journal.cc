#include "include/journal.h"
#include <cstring>
#include <iostream>
#include <map>
#include <vector>

Journal::Journal(BlockDev *dev, uint64_t start_blk, uint64_t total_blocks, uint32_t bsize)
    : dev_(dev), start_blk_(start_blk), total_blocks_(total_blocks), bsize_(bsize) {
    memset(&sb_, 0, sizeof(sb_));
}

Journal::~Journal() {
    flush();
}

uint32_t Journal::compute_checksum(const void *buf, size_t len) {
    /* Simple Adler-32 */
    const uint8_t *data = static_cast<const uint8_t *>(buf);
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; ++i) {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

bool Journal::init_journal() {
    std::lock_guard<std::mutex> lock(journal_lock_);
    memset(&sb_, 0, sizeof(sb_));
    sb_.j_magic = YFS_JOURNAL_MAGIC;
    sb_.j_bsize = bsize_;
    sb_.j_start_blk = start_blk_;
    sb_.j_head = 1; /* Block 0 is the journal superblock */
    sb_.j_tail = 1;
    sb_.j_last_txid = 0;
    sb_.j_total_blocks = total_blocks_;

    return sync_sb();
}

bool Journal::load_journal() {
    std::lock_guard<std::mutex> lock(journal_lock_);
    if (!dev_) return false;
    std::vector<uint8_t> block(bsize_, 0);
    if (!dev_->read_block(start_blk_, block.data(), bsize_)) {
        return false;
    }
    memcpy(&sb_, block.data(), sizeof(sb_));
    if (sb_.j_magic != YFS_JOURNAL_MAGIC) {
        return false;
    }
    return true;
}

bool Journal::sync_sb() {
    if (!dev_) return false;
    std::vector<uint8_t> block(bsize_, 0);
    memcpy(block.data(), &sb_, sizeof(sb_));
    bool ok = dev_->write_block(start_blk_, block.data(), bsize_);
    if (ok) {
        dev_->flush();
    }
    return ok;
}

bool Journal::write_journal_block(uint64_t log_offset, const void *buf) {
    uint64_t physical_blk = start_blk_ + (log_offset % total_blocks_);
    return dev_->write_block(physical_blk, buf, bsize_);
}

bool Journal::read_journal_block(uint64_t log_offset, void *buf) {
    uint64_t physical_blk = start_blk_ + (log_offset % total_blocks_);
    return dev_->read_block(physical_blk, buf, bsize_);
}

uint64_t Journal::next_txid() {
    std::lock_guard<std::mutex> lock(journal_lock_);
    return ++sb_.j_last_txid;
}

bool Journal::write_tx_begin(uint64_t txid) {
    std::lock_guard<std::mutex> lock(journal_lock_);
    std::vector<uint8_t> block(bsize_, 0);
    yfs_log_header *hdr = reinterpret_cast<yfs_log_header *>(block.data());
    hdr->h_magic = YFS_JOURNAL_MAGIC;
    hdr->h_type = YFS_LOG_TX_BEGIN;
    hdr->h_txid = txid;
    hdr->h_target_blk = 0;
    hdr->h_data_len = 0;
    hdr->h_checksum = compute_checksum(block.data() + sizeof(yfs_log_header), bsize_ - sizeof(yfs_log_header));

    uint64_t pos = sb_.j_head;
    if (pos == 0) pos = 1;
    if (!write_journal_block(pos, block.data())) return false;

    sb_.j_head = (pos + 1) % total_blocks_;
    if (sb_.j_head == 0) sb_.j_head = 1;
    return true;
}

bool Journal::write_block_redo(uint64_t txid, uint64_t target_blk, const void *data, uint32_t len) {
    std::lock_guard<std::mutex> lock(journal_lock_);
    std::vector<uint8_t> block(bsize_, 0);
    yfs_log_header *hdr = reinterpret_cast<yfs_log_header *>(block.data());
    hdr->h_magic = YFS_JOURNAL_MAGIC;
    hdr->h_type = YFS_LOG_BLOCK_REDO;
    hdr->h_txid = txid;
    hdr->h_target_blk = target_blk;
    hdr->h_data_len = len;

    size_t payload_space = bsize_ - sizeof(yfs_log_header);
    size_t to_copy = (len < payload_space) ? len : payload_space;
    if (data && to_copy > 0) {
        memcpy(block.data() + sizeof(yfs_log_header), data, to_copy);
    }
    hdr->h_checksum = compute_checksum(block.data() + sizeof(yfs_log_header), payload_space);

    uint64_t pos = sb_.j_head;
    if (pos == 0) pos = 1;
    if (!write_journal_block(pos, block.data())) return false;

    sb_.j_head = (pos + 1) % total_blocks_;
    if (sb_.j_head == 0) sb_.j_head = 1;
    return true;
}

bool Journal::write_tx_commit(uint64_t txid) {
    std::lock_guard<std::mutex> lock(journal_lock_);
    std::vector<uint8_t> block(bsize_, 0);
    yfs_log_header *hdr = reinterpret_cast<yfs_log_header *>(block.data());
    hdr->h_magic = YFS_JOURNAL_MAGIC;
    hdr->h_type = YFS_LOG_TX_COMMIT;
    hdr->h_txid = txid;
    hdr->h_target_blk = 0;
    hdr->h_data_len = 0;
    hdr->h_checksum = compute_checksum(block.data() + sizeof(yfs_log_header), bsize_ - sizeof(yfs_log_header));

    uint64_t pos = sb_.j_head;
    if (pos == 0) pos = 1;
    if (!write_journal_block(pos, block.data())) return false;

    sb_.j_head = (pos + 1) % total_blocks_;
    if (sb_.j_head == 0) sb_.j_head = 1;

    sb_.j_last_txid = txid;
    return true;
}

bool Journal::flush() {
    std::lock_guard<std::mutex> lock(journal_lock_);
    if (!dev_) return false;
    sync_sb();
    return dev_->flush();
}

bool Journal::update_tail(uint64_t last_checkpointed_tx) {
    std::lock_guard<std::mutex> lock(journal_lock_);
    (void)last_checkpointed_tx;
    return sync_sb();
}

bool Journal::recover(BlockDev *dev) {
    std::lock_guard<std::mutex> lock(journal_lock_);
    if (!dev) return false;

    std::vector<uint8_t> block(bsize_, 0);
    struct RedoRecord {
        uint64_t target_blk;
        std::vector<uint8_t> data;
    };

    std::map<uint64_t, std::vector<RedoRecord>> uncommitted_tx;
    std::vector<uint64_t> committed_tx_order;

    /* Scan circular log from j_tail to j_head */
    uint64_t curr = sb_.j_tail;
    if (curr == 0) curr = 1;
    uint64_t end = sb_.j_head;
    if (end == 0) end = 1;

    size_t scanned = 0;
    while (curr != end && scanned < total_blocks_) {
        if (!read_journal_block(curr, block.data())) break;

        const yfs_log_header *hdr = reinterpret_cast<const yfs_log_header *>(block.data());
        if (hdr->h_magic == YFS_JOURNAL_MAGIC) {
            if (hdr->h_type == YFS_LOG_TX_BEGIN) {
                uncommitted_tx[hdr->h_txid].clear();
            } else if (hdr->h_type == YFS_LOG_BLOCK_REDO) {
                RedoRecord rec;
                rec.target_blk = hdr->h_target_blk;
                rec.data.assign(block.begin() + sizeof(yfs_log_header), block.end());
                uncommitted_tx[hdr->h_txid].push_back(rec);
            } else if (hdr->h_type == YFS_LOG_TX_COMMIT) {
                committed_tx_order.push_back(hdr->h_txid);
            }
        }

        curr = (curr + 1) % total_blocks_;
        if (curr == 0) curr = 1;
        scanned++;
    }

    /* Replay committed transactions to home block locations */
    for (uint64_t txid : committed_tx_order) {
        auto it = uncommitted_tx.find(txid);
        if (it != uncommitted_tx.end()) {
            for (const auto &rec : it->second) {
                dev->write_block(rec.target_blk, rec.data.data(), bsize_);
            }
        }
    }

    dev->flush();

    /* Reset journal to clean state */
    sb_.j_tail = sb_.j_head;
    sync_sb();

    return true;
}
