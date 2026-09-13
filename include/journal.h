#ifndef JOURNAL_H
#define JOURNAL_H

#include "yfs_fs.h"
#include "block_dev.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>

typedef struct journal {
    block_dev_t *dev;
    uint64_t start_blk;
    uint64_t total_blocks;
    uint32_t bsize;
    yfs_journal_sb_t sb;
    pthread_mutex_t lock;
} journal_t;

journal_t *journal_create(block_dev_t *dev, uint64_t start_blk, uint64_t total_blocks, uint32_t bsize);
void journal_destroy(journal_t *j);

bool journal_init(journal_t *j);
bool journal_load(journal_t *j);

/* Write log record for transaction */
bool journal_write_tx_begin(journal_t *j, uint64_t txid);
bool journal_write_block_redo(journal_t *j, uint64_t txid, uint64_t target_blk, const void *data, uint32_t len);
bool journal_write_tx_commit(journal_t *j, uint64_t txid);

/* Flush journal writes to disk */
bool journal_flush(journal_t *j);

/* Checkpoint: advance tail to reclaimed tx */
bool journal_update_tail(journal_t *j, uint64_t last_checkpointed_tx);

/* Crash recovery: read and replay uncheckpointed committed transactions */
bool journal_recover(journal_t *j, block_dev_t *dev);

uint64_t journal_next_txid(journal_t *j);
uint64_t journal_last_txid(journal_t *j);

#endif /* JOURNAL_H */
