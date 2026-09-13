#include "journal.h"
#include <stdlib.h>
#include <string.h>

static uint32_t compute_adler32(const void *buf, size_t len) {
    const uint8_t *data = (const uint8_t *)buf;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; ++i) {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

journal_t *journal_create(block_dev_t *dev, uint64_t start_blk, uint64_t total_blocks, uint32_t bsize) {
    journal_t *j = (journal_t *)calloc(1, sizeof(journal_t));
    if (!j) return nullptr;

    j->dev = dev;
    j->start_blk = start_blk;
    j->total_blocks = total_blocks;
    j->bsize = bsize;
    pthread_mutex_init(&j->lock, nullptr);

    return j;
}

void journal_destroy(journal_t *j) {
    if (!j) return;
    journal_flush(j);
    pthread_mutex_destroy(&j->lock);
    free(j);
}

static bool write_journal_block(journal_t *j, uint64_t log_offset, const void *buf) {
    uint64_t physical_blk = j->start_blk + (log_offset % j->total_blocks);
    return block_dev_write(j->dev, physical_blk, buf, j->bsize);
}

static bool read_journal_block(journal_t *j, uint64_t log_offset, void *buf) {
    uint64_t physical_blk = j->start_blk + (log_offset % j->total_blocks);
    return block_dev_read(j->dev, physical_blk, buf, j->bsize);
}

static bool sync_sb(journal_t *j) {
    if (!j->dev) return false;
    uint8_t *block = (uint8_t *)calloc(1, j->bsize);
    if (!block) return false;

    memcpy(block, &j->sb, sizeof(j->sb));
    bool ok = block_dev_write(j->dev, j->start_blk, block, j->bsize);
    if (ok) {
        block_dev_flush(j->dev);
    }
    free(block);
    return ok;
}

bool journal_init(journal_t *j) {
    if (!j) return false;
    pthread_mutex_lock(&j->lock);
    memset(&j->sb, 0, sizeof(j->sb));
    j->sb.j_magic = YFS_JOURNAL_MAGIC;
    j->sb.j_bsize = j->bsize;
    j->sb.j_start_blk = j->start_blk;
    j->sb.j_head = 1;
    j->sb.j_tail = 1;
    j->sb.j_last_txid = 0;
    j->sb.j_total_blocks = j->total_blocks;

    bool ok = sync_sb(j);
    pthread_mutex_unlock(&j->lock);
    return ok;
}

bool journal_load(journal_t *j) {
    if (!j || !j->dev) return false;
    pthread_mutex_lock(&j->lock);

    uint8_t *block = (uint8_t *)calloc(1, j->bsize);
    if (!block) {
        pthread_mutex_unlock(&j->lock);
        return false;
    }

    if (!block_dev_read(j->dev, j->start_blk, block, j->bsize)) {
        free(block);
        pthread_mutex_unlock(&j->lock);
        return false;
    }

    memcpy(&j->sb, block, sizeof(j->sb));
    free(block);

    if (j->sb.j_magic != YFS_JOURNAL_MAGIC) {
        pthread_mutex_unlock(&j->lock);
        return false;
    }

    pthread_mutex_unlock(&j->lock);
    return true;
}

uint64_t journal_next_txid(journal_t *j) {
    if (!j) return 0;
    pthread_mutex_lock(&j->lock);
    uint64_t txid = ++j->sb.j_last_txid;
    pthread_mutex_unlock(&j->lock);
    return txid;
}

uint64_t journal_last_txid(journal_t *j) {
    if (!j) return 0;
    pthread_mutex_lock(&j->lock);
    uint64_t txid = j->sb.j_last_txid;
    pthread_mutex_unlock(&j->lock);
    return txid;
}

bool journal_write_tx_begin(journal_t *j, uint64_t txid) {
    if (!j) return false;
    pthread_mutex_lock(&j->lock);

    uint8_t *block = (uint8_t *)calloc(1, j->bsize);
    if (!block) {
        pthread_mutex_unlock(&j->lock);
        return false;
    }

    yfs_log_header_t *hdr = (yfs_log_header_t *)block;
    hdr->h_magic = YFS_JOURNAL_MAGIC;
    hdr->h_type = YFS_LOG_TX_BEGIN;
    hdr->h_txid = txid;
    hdr->h_target_blk = 0;
    hdr->h_data_len = 0;
    hdr->h_checksum = compute_adler32(block + sizeof(yfs_log_header_t), j->bsize - sizeof(yfs_log_header_t));

    uint64_t pos = j->sb.j_head;
    if (pos == 0) pos = 1;
    if (!write_journal_block(j, pos, block)) {
        free(block);
        pthread_mutex_unlock(&j->lock);
        return false;
    }

    j->sb.j_head = (pos + 1) % j->total_blocks;
    if (j->sb.j_head == 0) j->sb.j_head = 1;

    free(block);
    pthread_mutex_unlock(&j->lock);
    return true;
}

bool journal_write_block_redo(journal_t *j, uint64_t txid, uint64_t target_blk, const void *data, uint32_t len) {
    if (!j) return false;
    pthread_mutex_lock(&j->lock);

    uint8_t *block = (uint8_t *)calloc(1, j->bsize);
    if (!block) {
        pthread_mutex_unlock(&j->lock);
        return false;
    }

    yfs_log_header_t *hdr = (yfs_log_header_t *)block;
    hdr->h_magic = YFS_JOURNAL_MAGIC;
    hdr->h_type = YFS_LOG_BLOCK_REDO;
    hdr->h_txid = txid;
    hdr->h_target_blk = target_blk;
    hdr->h_data_len = len;

    size_t payload_space = j->bsize - sizeof(yfs_log_header_t);
    size_t to_copy = (len < payload_space) ? len : payload_space;
    if (data && to_copy > 0) {
        memcpy(block + sizeof(yfs_log_header_t), data, to_copy);
    }
    hdr->h_checksum = compute_adler32(block + sizeof(yfs_log_header_t), payload_space);

    uint64_t pos = j->sb.j_head;
    if (pos == 0) pos = 1;
    if (!write_journal_block(j, pos, block)) {
        free(block);
        pthread_mutex_unlock(&j->lock);
        return false;
    }

    j->sb.j_head = (pos + 1) % j->total_blocks;
    if (j->sb.j_head == 0) j->sb.j_head = 1;

    free(block);
    pthread_mutex_unlock(&j->lock);
    return true;
}

bool journal_write_tx_commit(journal_t *j, uint64_t txid) {
    if (!j) return false;
    pthread_mutex_lock(&j->lock);

    uint8_t *block = (uint8_t *)calloc(1, j->bsize);
    if (!block) {
        pthread_mutex_unlock(&j->lock);
        return false;
    }

    yfs_log_header_t *hdr = (yfs_log_header_t *)block;
    hdr->h_magic = YFS_JOURNAL_MAGIC;
    hdr->h_type = YFS_LOG_TX_COMMIT;
    hdr->h_txid = txid;
    hdr->h_target_blk = 0;
    hdr->h_data_len = 0;
    hdr->h_checksum = compute_adler32(block + sizeof(yfs_log_header_t), j->bsize - sizeof(yfs_log_header_t));

    uint64_t pos = j->sb.j_head;
    if (pos == 0) pos = 1;
    if (!write_journal_block(j, pos, block)) {
        free(block);
        pthread_mutex_unlock(&j->lock);
        return false;
    }

    j->sb.j_head = (pos + 1) % j->total_blocks;
    if (j->sb.j_head == 0) j->sb.j_head = 1;
    j->sb.j_last_txid = txid;

    free(block);
    pthread_mutex_unlock(&j->lock);
    return true;
}

bool journal_flush(journal_t *j) {
    if (!j || !j->dev) return false;
    pthread_mutex_lock(&j->lock);
    sync_sb(j);
    bool ok = block_dev_flush(j->dev);
    pthread_mutex_unlock(&j->lock);
    return ok;
}

bool journal_update_tail(journal_t *j, uint64_t last_checkpointed_tx) {
    (void)last_checkpointed_tx;
    if (!j) return false;
    pthread_mutex_lock(&j->lock);
    bool ok = sync_sb(j);
    pthread_mutex_unlock(&j->lock);
    return ok;
}

typedef struct redo_entry {
    uint64_t txid;
    uint64_t target_blk;
    uint8_t *data;
    struct redo_entry *next;
} redo_entry_t;

typedef struct tx_record {
    uint64_t txid;
    bool committed;
    redo_entry_t *redos;
    struct tx_record *next;
} tx_record_t;

bool journal_recover(journal_t *j, block_dev_t *dev) {
    if (!j || !dev) return false;
    pthread_mutex_lock(&j->lock);

    uint8_t *block = (uint8_t *)calloc(1, j->bsize);
    if (!block) {
        pthread_mutex_unlock(&j->lock);
        return false;
    }

    tx_record_t *tx_list = nullptr;

    uint64_t curr = j->sb.j_tail;
    if (curr == 0) curr = 1;
    uint64_t end = j->sb.j_head;
    if (end == 0) end = 1;

    size_t scanned = 0;
    while (curr != end && scanned < j->total_blocks) {
        if (!read_journal_block(j, curr, block)) break;

        const yfs_log_header_t *hdr = (const yfs_log_header_t *)block;
        if (hdr->h_magic == YFS_JOURNAL_MAGIC) {
            /* Find or create tx record */
            tx_record_t *rec = tx_list;
            while (rec && rec->txid != hdr->h_txid) {
                rec = rec->next;
            }
            if (!rec) {
                rec = (tx_record_t *)calloc(1, sizeof(tx_record_t));
                rec->txid = hdr->h_txid;
                rec->committed = false;
                rec->redos = nullptr;
                rec->next = tx_list;
                tx_list = rec;
            }

            if (hdr->h_type == YFS_LOG_BLOCK_REDO) {
                redo_entry_t *redo = (redo_entry_t *)calloc(1, sizeof(redo_entry_t));
                redo->txid = hdr->h_txid;
                redo->target_blk = hdr->h_target_blk;
                redo->data = (uint8_t *)malloc(j->bsize);
                memcpy(redo->data, block + sizeof(yfs_log_header_t), j->bsize - sizeof(yfs_log_header_t));
                redo->next = rec->redos;
                rec->redos = redo;
            } else if (hdr->h_type == YFS_LOG_TX_COMMIT) {
                rec->committed = true;
            }
        }

        curr = (curr + 1) % j->total_blocks;
        if (curr == 0) curr = 1;
        scanned++;
    }

    /* Replay committed transactions in order */
    tx_record_t *rec = tx_list;
    while (rec) {
        if (rec->committed) {
            redo_entry_t *redo = rec->redos;
            while (redo) {
                block_dev_write(dev, redo->target_blk, redo->data, j->bsize);
                redo = redo->next;
            }
        }
        rec = rec->next;
    }

    block_dev_flush(dev);

    /* Free memory */
    while (tx_list) {
        tx_record_t *next_rec = tx_list->next;
        redo_entry_t *redo = tx_list->redos;
        while (redo) {
            redo_entry_t *next_redo = redo->next;
            free(redo->data);
            free(redo);
            redo = next_redo;
        }
        free(tx_list);
        tx_list = next_rec;
    }

    j->sb.j_tail = j->sb.j_head;
    sync_sb(j);

    free(block);
    pthread_mutex_unlock(&j->lock);
    return true;
}
