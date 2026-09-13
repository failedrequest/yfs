#ifndef YFS_FS_H
#define YFS_FS_H

#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>

#define YFS_MAGIC           0x59465331  /* "YFS1" */
#define YFS_DEFAULT_BSIZE   4096        /* 4KB block size */
#define YFS_DIRECT_BLOCKS   12
#define YFS_INDIR_PER_BLOCK (YFS_DEFAULT_BSIZE / sizeof(uint32_t))
#define YFS_MAX_NAME_LEN    255
#define YFS_ROOT_INO        1           /* Root directory inode */

/* Inode type flags */
#define YFS_IFREG           0100000
#define YFS_IFDIR           0040000
#define YFS_IFLNK           0120000

/* On-disk Superblock */
typedef struct yfs_superblock {
    uint32_t s_magic;           /* Magic number YFS_MAGIC */
    uint32_t s_bsize;           /* Block size in bytes (4096) */
    uint64_t s_blocks_count;    /* Total blocks in filesystem */
    uint64_t s_free_blocks;     /* Free data blocks count */
    uint32_t s_inodes_count;    /* Total inodes in filesystem */
    uint32_t s_free_inodes;     /* Free inodes count */
    uint32_t s_cgroup_count;    /* Number of cylinder groups */
    uint32_t s_blocks_per_cg;   /* Number of blocks per cylinder group */
    uint32_t s_inodes_per_cg;   /* Inodes per cylinder group */

    /* Journal metadata */
    uint64_t s_journal_start_blk; /* Starting block number of journal area */
    uint64_t s_journal_blocks;    /* Number of blocks allocated for journal */

    /* Checkpoint and recovery pointers */
    uint64_t s_last_checkpoint_tx;
    uint32_t s_clean_unmount;   /* 1 if unmounted cleanly, 0 otherwise */
    uint32_t s_pad[30];         /* Padding to fit 256 bytes / align */
} __attribute__((packed)) yfs_superblock_t;

/* On-disk Cylinder Group Descriptor */
typedef struct yfs_cgroup {
    uint32_t cg_cgx;            /* Cylinder group index */
    uint32_t cg_free_blocks;    /* Free blocks count in this CG */
    uint32_t cg_free_inodes;    /* Free inodes count in this CG */
    uint64_t cg_block_bitmap;   /* Block number of data block allocation bitmap */
    uint64_t cg_inode_bitmap;   /* Block number of inode allocation bitmap */
    uint64_t cg_inode_table;    /* Start block number of inode table */
    uint64_t cg_data_start;     /* Start block number of data blocks in this CG */
    uint32_t cg_data_blocks;    /* Count of data blocks in this CG */
    uint32_t cg_pad[6];
} __attribute__((packed)) yfs_cgroup_t;

/* On-disk Inode (128 bytes) */
typedef struct yfs_dinode {
    uint16_t di_mode;           /* File mode and type */
    uint16_t di_nlink;          /* Number of hard links */
    uint32_t di_uid;            /* Owner UID */
    uint32_t di_gid;            /* Owner GID */
    uint64_t di_size;           /* File size in bytes */
    uint64_t di_atime;          /* Access time (seconds) */
    uint64_t di_mtime;          /* Modification time (seconds) */
    uint64_t di_ctime;          /* Inode change time (seconds) */
    uint32_t di_direct[YFS_DIRECT_BLOCKS]; /* Direct data block pointers */
    uint32_t di_indirect;       /* Single indirect block pointer */
    uint32_t di_double_indirect;/* Double indirect block pointer */
    uint32_t di_flags;          /* File flags */
    uint32_t di_pad[10];        /* Pad to 128 bytes */
} __attribute__((packed)) yfs_dinode_t;

/* Directory Entry Record */
typedef struct yfs_dirent {
    uint32_t d_ino;             /* Inode number */
    uint16_t d_reclen;          /* Record length (aligned to 4 bytes) */
    uint8_t  d_type;            /* File type (DT_DIR, DT_REG, etc.) */
    uint8_t  d_namlen;          /* Name length */
    char     d_name[YFS_MAX_NAME_LEN + 1]; /* Null-terminated name */
} __attribute__((packed)) yfs_dirent_t;

/* Journal Header and Record Types */
#define YFS_JOURNAL_MAGIC       0x4A4F5552  /* "JOUR" */
#define YFS_LOG_TX_BEGIN        1
#define YFS_LOG_BLOCK_REDO      2
#define YFS_LOG_TX_COMMIT       3
#define YFS_LOG_CHECKPOINT      4

typedef struct yfs_journal_sb {
    uint32_t j_magic;           /* YFS_JOURNAL_MAGIC */
    uint32_t j_bsize;           /* Journal block size */
    uint64_t j_start_blk;       /* Head block index in circular log */
    uint64_t j_head;            /* Next block to write (relative offset in journal) */
    uint64_t j_tail;            /* Oldest active transaction block */
    uint64_t j_last_txid;       /* Latest committed transaction ID */
    uint64_t j_total_blocks;    /* Total blocks in circular journal */
    uint32_t j_pad[10];
} __attribute__((packed)) yfs_journal_sb_t;

/* Record descriptor stored in journal block */
typedef struct yfs_log_header {
    uint32_t h_magic;           /* Magic marker */
    uint32_t h_type;            /* YFS_LOG_TX_BEGIN, YFS_LOG_BLOCK_REDO, YFS_LOG_TX_COMMIT */
    uint64_t h_txid;            /* Transaction sequence number */
    uint64_t h_target_blk;      /* Target home block number for redo record */
    uint32_t h_data_len;        /* Length of payload (typically block size or chunk) */
    uint32_t h_checksum;        /* CRC32 / Adler checksum */
} __attribute__((packed)) yfs_log_header_t;

#endif /* YFS_FS_H */
