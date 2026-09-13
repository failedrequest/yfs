#ifndef TX_MANAGER_H
#define TX_MANAGER_H

#include "journal.h"
#include "buffer_cache.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>

#define MAX_TX_MODIFIED_BLOCKS 256

typedef struct transaction {
    uint64_t txid;
    journal_t *journal;
    buffer_cache_t *cache;
    block_buffer_t *modified_blocks[MAX_TX_MODIFIED_BLOCKS];
    size_t num_modified;
    bool committed;
    bool active;
} transaction_t;

typedef struct tx_manager {
    journal_t *journal;
    buffer_cache_t *cache;
    pthread_mutex_t lock;
    uint64_t last_checkpoint_tx;
} tx_manager_t;

tx_manager_t *tx_manager_create(journal_t *journal, buffer_cache_t *cache);
void tx_manager_destroy(tx_manager_t *mgr);

transaction_t *tx_begin(tx_manager_t *mgr);
void tx_modify_block(transaction_t *tx, block_buffer_t *buf);
bool tx_commit(transaction_t *tx);
void tx_abort(transaction_t *tx);

bool tx_manager_checkpoint(tx_manager_t *mgr);

#endif /* TX_MANAGER_H */
