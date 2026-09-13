#include "include/tx_manager.h"
#include <iostream>

Transaction::Transaction(uint64_t txid, Journal *journal, BufferCache *cache)
    : txid_(txid), journal_(journal), cache_(cache), committed_(false), active_(true) {
    if (journal_) {
        journal_->write_tx_begin(txid_);
    }
}

Transaction::~Transaction() {
    if (active_ && !committed_) {
        abort();
    }
}

void Transaction::modify_block(std::shared_ptr<BlockBuffer> buf) {
    if (!buf || !active_) return;
    cache_->mark_dirty(buf, txid_);
    modified_blocks_.push_back(buf);
}

bool Transaction::commit() {
    if (!active_ || committed_) return false;

    /* Write redo records for all modified blocks to journal */
    if (journal_) {
        for (const auto &buf : modified_blocks_) {
            journal_->write_block_redo(txid_, buf->blk_no, buf->data.data(), buf->data.size());
        }
        journal_->write_tx_commit(txid_);
    }

    committed_ = true;
    active_ = false;
    return true;
}

void Transaction::abort() {
    active_ = false;
    committed_ = false;
}

TxManager::TxManager(Journal *journal, BufferCache *cache)
    : journal_(journal), cache_(cache), last_checkpoint_tx_(0) {}

TxManager::~TxManager() {
    checkpoint();
}

std::shared_ptr<Transaction> TxManager::begin_transaction() {
    std::lock_guard<std::mutex> lock(tx_lock_);
    uint64_t txid = 1;
    if (journal_) {
        txid = journal_->next_txid();
    }
    return std::make_shared<Transaction>(txid, journal_, cache_);
}

bool TxManager::checkpoint() {
    std::lock_guard<std::mutex> lock(tx_lock_);
    if (!cache_) return false;

    uint64_t last_tx = journal_ ? journal_->last_txid() : 0;
    bool ok = cache_->checkpoint_tx(last_tx);
    if (ok && journal_) {
        journal_->update_tail(last_tx);
        last_checkpoint_tx_ = last_tx;
    }
    return ok;
}
