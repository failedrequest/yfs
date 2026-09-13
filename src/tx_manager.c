#include "tx_manager.h"
#include <stdlib.h>

tx_manager_t *tx_manager_create(journal_t *journal, buffer_cache_t *cache) {
    tx_manager_t *mgr = (tx_manager_t *)calloc(1, sizeof(tx_manager_t));
    if (!mgr) return nullptr;

    mgr->journal = journal;
    mgr->cache = cache;
    mgr->last_checkpoint_tx = 0;
    pthread_mutex_init(&mgr->lock, nullptr);

    return mgr;
}

void tx_manager_destroy(tx_manager_t *mgr) {
    if (!mgr) return;
    tx_manager_checkpoint(mgr);
    pthread_mutex_destroy(&mgr->lock);
    free(mgr);
}

transaction_t *tx_begin(tx_manager_t *mgr) {
    if (!mgr) return nullptr;
    pthread_mutex_lock(&mgr->lock);

    transaction_t *tx = (transaction_t *)calloc(1, sizeof(transaction_t));
    if (!tx) {
        pthread_mutex_unlock(&mgr->lock);
        return nullptr;
    }

    tx->txid = mgr->journal ? journal_next_txid(mgr->journal) : 1;
    tx->journal = mgr->journal;
    tx->cache = mgr->cache;
    tx->num_modified = 0;
    tx->committed = false;
    tx->active = true;

    if (tx->journal) {
        journal_write_tx_begin(tx->journal, tx->txid);
    }

    pthread_mutex_unlock(&mgr->lock);
    return tx;
}

void tx_modify_block(transaction_t *tx, block_buffer_t *buf) {
    if (!tx || !buf || !tx->active) return;
    buffer_cache_mark_dirty(tx->cache, buf, tx->txid);

    for (size_t i = 0; i < tx->num_modified; ++i) {
        if (tx->modified_blocks[i] == buf) return;
    }

    if (tx->num_modified < MAX_TX_MODIFIED_BLOCKS) {
        tx->modified_blocks[tx->num_modified++] = buf;
    }
}

bool tx_commit(transaction_t *tx) {
    if (!tx || !tx->active || tx->committed) return false;

    if (tx->journal) {
        for (size_t i = 0; i < tx->num_modified; ++i) {
            block_buffer_t *buf = tx->modified_blocks[i];
            journal_write_block_redo(tx->journal, tx->txid, buf->blk_no, buf->data, tx->cache->block_size);
        }
        journal_write_tx_commit(tx->journal, tx->txid);
    }

    tx->committed = true;
    tx->active = false;
    free(tx);
    return true;
}

void tx_abort(transaction_t *tx) {
    if (!tx) return;
    tx->active = false;
    tx->committed = false;
    free(tx);
}

bool tx_manager_checkpoint(tx_manager_t *mgr) {
    if (!mgr || !mgr->cache) return false;
    pthread_mutex_lock(&mgr->lock);

    uint64_t last_tx = mgr->journal ? journal_last_txid(mgr->journal) : 0;
    bool ok = buffer_cache_checkpoint_tx(mgr->cache, last_tx);
    if (ok && mgr->journal) {
        journal_update_tail(mgr->journal, last_tx);
        mgr->last_checkpoint_tx = last_tx;
    }

    pthread_mutex_unlock(&mgr->lock);
    return ok;
}
