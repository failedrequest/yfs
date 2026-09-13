#include "include/yfs_fs.h"
#include "include/block_dev.h"
#include "include/journal.h"
#include <iostream>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <ctime>

void usage(const char *prog) {
    std::cerr << "Usage: " << prog << " [-s size_in_mb] [-b block_size] [-j journal_blocks] <device_or_image_path>" << std::endl;
    std::cerr << "Example: " << prog << " -s 64 /tmp/yfs.img" << std::endl;
}

int main(int argc, char **argv) {
    uint64_t size_mb = 32;
    uint32_t bsize = YFS_DEFAULT_BSIZE;
    uint64_t journal_blocks = 256;

    int opt;
    while ((opt = getopt(argc, argv, "s:b:j:")) != -1) {
        switch (opt) {
            case 's':
                size_mb = std::strtoull(optarg, nullptr, 10);
                break;
            case 'b':
                bsize = std::strtoul(optarg, nullptr, 10);
                break;
            case 'j':
                journal_blocks = std::strtoull(optarg, nullptr, 10);
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

    std::string path = argv[optind];
    uint64_t size_bytes = size_mb * 1024 * 1024;
    uint64_t total_blocks = size_bytes / bsize;

    std::cout << "Formatting YFS filesystem on " << path << " (" << size_mb << " MB, "
              << total_blocks << " blocks, bsize=" << bsize << ")..." << std::endl;

    BlockDev dev;
    if (!dev.open_device(path, true, size_bytes)) {
        std::cerr << "Failed to open or create image: " << path << std::endl;
        return 1;
    }

    /* Clean all blocks */
    std::vector<uint8_t> zero_buf(bsize, 0);

    /* Layout calculations */
    /* Block 0: Superblock */
    /* Blocks 1..CG_DESC_BLOCKS: Cylinder Group Descriptors */
    /* Journal starts after CG descriptors */

    uint32_t inodes_per_cg = 1024;
    uint32_t cgroups_count = 1; /* For small images, single cylinder group */
    size_t inodes_per_block = bsize / sizeof(yfs_dinode);
    uint32_t inode_table_blocks = (inodes_per_cg + inodes_per_block - 1) / inodes_per_block;

    uint64_t journal_start = 2;
    uint64_t cg_block_bitmap_blk = journal_start + journal_blocks;
    uint64_t cg_inode_bitmap_blk = cg_block_bitmap_blk + 1;
    uint64_t cg_inode_table_blk = cg_inode_bitmap_blk + 1;
    uint64_t cg_data_start_blk = cg_inode_table_blk + inode_table_blocks;

    if (cg_data_start_blk >= total_blocks) {
        std::cerr << "Disk image too small for requested filesystem structure." << std::endl;
        return 1;
    }

    uint32_t data_blocks = total_blocks - cg_data_start_blk;

    /* Initialize Superblock */
    yfs_superblock sb;
    memset(&sb, 0, sizeof(sb));
    sb.s_magic = YFS_MAGIC;
    sb.s_bsize = bsize;
    sb.s_blocks_count = total_blocks;
    sb.s_free_blocks = data_blocks - 1; /* Block 0 of data used by root dir */
    sb.s_inodes_count = inodes_per_cg;
    sb.s_free_inodes = inodes_per_cg - 1; /* Inode 1 used by root dir */
    sb.s_cgroup_count = cgroups_count;
    sb.s_blocks_per_cg = total_blocks;
    sb.s_inodes_per_cg = inodes_per_cg;
    sb.s_journal_start_blk = journal_start;
    sb.s_journal_blocks = journal_blocks;
    sb.s_last_checkpoint_tx = 0;
    sb.s_clean_unmount = 1;

    std::vector<uint8_t> sb_buf(bsize, 0);
    memcpy(sb_buf.data(), &sb, sizeof(sb));
    dev.write_block(0, sb_buf.data(), bsize);

    /* Initialize Cylinder Group */
    yfs_cgroup cg;
    memset(&cg, 0, sizeof(cg));
    cg.cg_cgx = 0;
    cg.cg_free_blocks = sb.s_free_blocks;
    cg.cg_free_inodes = sb.s_free_inodes;
    cg.cg_block_bitmap = cg_block_bitmap_blk;
    cg.cg_inode_bitmap = cg_inode_bitmap_blk;
    cg.cg_inode_table = cg_inode_table_blk;
    cg.cg_data_start = cg_data_start_blk;
    cg.cg_data_blocks = data_blocks;

    std::vector<uint8_t> cg_buf(bsize, 0);
    memcpy(cg_buf.data(), &cg, sizeof(cg));
    dev.write_block(1, cg_buf.data(), bsize);

    /* Initialize Journal */
    Journal journal(&dev, journal_start, journal_blocks, bsize);
    journal.init_journal();

    /* Initialize Block Bitmap (Mark block 0 as allocated for root dir) */
    std::vector<uint8_t> blk_bitmap(bsize, 0);
    blk_bitmap[0] = 0x01;
    dev.write_block(cg_block_bitmap_blk, blk_bitmap.data(), bsize);

    /* Initialize Inode Bitmap (Mark Inode 1 as allocated) */
    std::vector<uint8_t> ino_bitmap(bsize, 0);
    ino_bitmap[0] = 0x01;
    dev.write_block(cg_inode_bitmap_blk, ino_bitmap.data(), bsize);

    /* Zero out inode table */
    for (uint32_t i = 0; i < inode_table_blocks; ++i) {
        dev.write_block(cg_inode_table_blk + i, zero_buf.data(), bsize);
    }

    /* Create Root Inode (Inode 1) */
    yfs_dinode root_dinode;
    memset(&root_dinode, 0, sizeof(root_dinode));
    root_dinode.di_mode = 0755 | YFS_IFDIR;
    root_dinode.di_nlink = 2; /* . and .. */
    root_dinode.di_uid = 0;
    root_dinode.di_gid = 0;
    root_dinode.di_size = 2 * sizeof(yfs_dirent);
    time_t now = time(nullptr);
    root_dinode.di_atime = now;
    root_dinode.di_mtime = now;
    root_dinode.di_ctime = now;
    root_dinode.di_direct[0] = cg_data_start_blk;

    std::vector<uint8_t> first_ino_blk(bsize, 0);
    memcpy(first_ino_blk.data(), &root_dinode, sizeof(root_dinode));
    dev.write_block(cg_inode_table_blk, first_ino_blk.data(), bsize);

    /* Populate Root Directory Data Block with "." and ".." */
    std::vector<uint8_t> root_dir_blk(bsize, 0);
    yfs_dirent *entries = reinterpret_cast<yfs_dirent *>(root_dir_blk.data());

    /* "." */
    entries[0].d_ino = YFS_ROOT_INO;
    entries[0].d_type = 2; /* DT_DIR */
    entries[0].d_namlen = 1;
    strcpy(entries[0].d_name, ".");
    entries[0].d_reclen = sizeof(yfs_dirent);

    /* ".." */
    entries[1].d_ino = YFS_ROOT_INO;
    entries[1].d_type = 2; /* DT_DIR */
    entries[1].d_namlen = 2;
    strcpy(entries[1].d_name, "..");
    entries[1].d_reclen = sizeof(yfs_dirent);

    dev.write_block(cg_data_start_blk, root_dir_blk.data(), bsize);
    dev.flush();

    std::cout << "yFS initialized successfully with " << data_blocks << " data blocks." << std::endl;
    return 0;
}
