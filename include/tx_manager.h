#ifndef TX_MANAGER_H
#define TX_MANAGER_H

#include "journal.h"
#include "buffer_cache.h"
#include <vector>
#include <mutex>
#include <memory>

class Transaction {
public:
    Transaction(uint64_t txid, Journal *journal, BufferCache *cache);
    ~Transaction();

    uint64_t txid() const { return txid_; }

    /* Modify a block within this transaction */
    void modify_block(std::shared_ptr<BlockBuffer> buf);

    /* Commit transaction to journal and buffer cache */
    bool commit();

    /* Abort transaction */
    void abort();

private:
    uint64_t txid_;
    Journal *journal_;
    BufferCache *cache_;
    std::vector<std::shared_ptr<BlockBuffer>> modified_blocks_;
    bool committed_;
    bool active_;
};

class TxManager {
public:
    TxManager(Journal *journal, BufferCache *cache);
    ~TxManager();

    std::shared_ptr<Transaction> begin_transaction();
    bool checkpoint();

private:
    Journal *journal_;
    BufferCache *cache_;
    std::mutex tx_lock_;
    uint64_t last_checkpoint_tx_;
};

#endif /* TX_MANAGER_H */
