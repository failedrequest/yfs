#include "yfs_fs.h"
#include "block_dev.h"
#include "journal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [-s size_in_mb] [-b block_size] [-j journal_blocks] <device_or_image_path>\n", prog);
    fprintf(stderr, "Example: %s -s 64 /tmp/yfs.img\n", prog);
}

int main(int argc, char **argv) {
    uint64_t size_mb = 32;
    uint32_t bsize = YFS_DEFAULT_BSIZE;
    uint64_t journal_blocks = 256;

    int opt;
    while ((opt = getopt(argc, argv, "s:b:j:")) != -1) {
        switch (opt) {
            case 's':
                size_mb = strtoull(optarg, nullptr, 10);
                break;
            case 'b':
                bsize = (uint32_t)strtoul(optarg, nullptr, 10);
                break;
            case 'j':
                journal_blocks = strtoull(optarg, nullptr, 10);
                break;
            default:
                usage(argv[0]);
                return 1;
        }
    }

    if (optind >= argc) {
        usage(argv[0]);
        return 1;
    }

    const char *path = argv[optind];
    uint64_t size_bytes = size_mb * 1024 * 1024;
    uint64_t total_blocks = size_bytes / bsize;

    printf("Formatting YFS filesystem on %s (%llu MB, %llu blocks, bsize=%u)...\n",
           path, (unsigned long long)size_mb, (unsigned long long)total_blocks, bsize);

    block_dev_t *dev = block_dev_open(path, true, size_bytes);
    if (!dev) {
        fprintf(stderr, "Failed to open or create image: %s\n", path);
        return 1;
    }

    uint8_t *zero_buf = (uint8_t *)calloc(1, bsize);

    uint32_t inodes_per_cg = 1024;
    uint32_t cgroups_count = 1;
    size_t inodes_per_block = bsize / sizeof(yfs_dinode_t);
    uint32_t inode_table_blocks = (uint32_t)((inodes_per_cg + inodes_per_block - 1) / inodes_per_block);

    uint64_t journal_start = 2;
    uint64_t cg_block_bitmap_blk = journal_start + journal_blocks;
    uint64_t cg_inode_bitmap_blk = cg_block_bitmap_blk + 1;
    uint64_t cg_inode_table_blk = cg_inode_bitmap_blk + 1;
    uint64_t cg_data_start_blk = cg_inode_table_blk + inode_table_blocks;

    if (cg_data_start_blk >= total_blocks) {
        fprintf(stderr, "Disk image too small for requested filesystem structure.\n");
        free(zero_buf);
        block_dev_close(dev);
        return 1;
    }

    uint32_t data_blocks = (uint32_t)(total_blocks - cg_data_start_blk);

    /* Initialize Superblock */
    yfs_superblock_t sb;
    memset(&sb, 0, sizeof(sb));
    sb.s_magic = YFS_MAGIC;
    sb.s_bsize = bsize;
    sb.s_blocks_count = total_blocks;
    sb.s_free_blocks = data_blocks - 1; /* Block 0 used by root dir */
    sb.s_inodes_count = inodes_per_cg;
    sb.s_free_inodes = inodes_per_cg - 1; /* Inode 1 used by root dir */
    sb.s_cgroup_count = cgroups_count;
    sb.s_blocks_per_cg = (uint32_t)total_blocks;
    sb.s_inodes_per_cg = inodes_per_cg;
    sb.s_journal_start_blk = journal_start;
    sb.s_journal_blocks = journal_blocks;
    sb.s_last_checkpoint_tx = 0;
    sb.s_clean_unmount = 1;

    uint8_t *sb_buf = (uint8_t *)calloc(1, bsize);
    memcpy(sb_buf, &sb, sizeof(sb));
    block_dev_write(dev, 0, sb_buf, bsize);
    free(sb_buf);

    /* Initialize Cylinder Group */
    yfs_cgroup_t cg;
    memset(&cg, 0, sizeof(cg));
    cg.cg_cgx = 0;
    cg.cg_free_blocks = (uint32_t)sb.s_free_blocks;
    cg.cg_free_inodes = sb.s_free_inodes;
    cg.cg_block_bitmap = cg_block_bitmap_blk;
    cg.cg_inode_bitmap = cg_inode_bitmap_blk;
    cg.cg_inode_table = cg_inode_table_blk;
    cg.cg_data_start = cg_data_start_blk;
    cg.cg_data_blocks = data_blocks;

    uint8_t *cg_buf = (uint8_t *)calloc(1, bsize);
    memcpy(cg_buf, &cg, sizeof(cg));
    block_dev_write(dev, 1, cg_buf, bsize);
    free(cg_buf);

    /* Initialize Journal */
    journal_t *journal = journal_create(dev, journal_start, journal_blocks, bsize);
    journal_init(journal);
    journal_destroy(journal);

    /* Initialize Block Bitmap (Mark block 0 as allocated) */
    uint8_t *blk_bitmap = (uint8_t *)calloc(1, bsize);
    blk_bitmap[0] = 0x01;
    block_dev_write(dev, cg_block_bitmap_blk, blk_bitmap, bsize);
    free(blk_bitmap);

    /* Initialize Inode Bitmap (Mark Inode 1 as allocated) */
    uint8_t *ino_bitmap = (uint8_t *)calloc(1, bsize);
    ino_bitmap[0] = 0x01;
    block_dev_write(dev, cg_inode_bitmap_blk, ino_bitmap, bsize);
    free(ino_bitmap);

    /* Zero out inode table */
    for (uint32_t i = 0; i < inode_table_blocks; ++i) {
        block_dev_write(dev, cg_inode_table_blk + i, zero_buf, bsize);
    }
    free(zero_buf);

    /* Create Root Inode (Inode 1) */
    yfs_dinode_t root_dinode;
    memset(&root_dinode, 0, sizeof(root_dinode));
    root_dinode.di_mode = 0755 | YFS_IFDIR;
    root_dinode.di_nlink = 2;
    root_dinode.di_uid = 0;
    root_dinode.di_gid = 0;
    root_dinode.di_size = 2 * sizeof(yfs_dirent_t);
    time_t now = time(nullptr);
    root_dinode.di_atime = (uint64_t)now;
    root_dinode.di_mtime = (uint64_t)now;
    root_dinode.di_ctime = (uint64_t)now;
    root_dinode.di_direct[0] = (uint32_t)cg_data_start_blk;

    uint8_t *first_ino_blk = (uint8_t *)calloc(1, bsize);
    memcpy(first_ino_blk, &root_dinode, sizeof(root_dinode));
    block_dev_write(dev, cg_inode_table_blk, first_ino_blk, bsize);
    free(first_ino_blk);

    /* Populate Root Directory Data Block with "." and ".." */
    uint8_t *root_dir_blk = (uint8_t *)calloc(1, bsize);
    yfs_dirent_t *entries = (yfs_dirent_t *)root_dir_blk;

    /* "." */
    entries[0].d_ino = YFS_ROOT_INO;
    entries[0].d_type = 2; /* DT_DIR */
    entries[0].d_namlen = 1;
    strcpy(entries[0].d_name, ".");
    entries[0].d_reclen = sizeof(yfs_dirent_t);

    /* ".." */
    entries[1].d_ino = YFS_ROOT_INO;
    entries[1].d_type = 2; /* DT_DIR */
    entries[1].d_namlen = 2;
    strcpy(entries[1].d_name, "..");
    entries[1].d_reclen = sizeof(yfs_dirent_t);

    block_dev_write(dev, cg_data_start_blk, root_dir_blk, bsize);
    free(root_dir_blk);

    block_dev_flush(dev);
    block_dev_close(dev);

    printf("yFS initialized successfully with %u data blocks.\n", data_blocks);
    return 0;
}
