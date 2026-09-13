#include "yfs_fs.h"
#include "yfs_core.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void test_basic_fs_ops(void) {
    const char *test_img = "/tmp/yfs_test.img";
    unlink(test_img);

    /* Format */
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "./newfs_yfs -s 16 %s", test_img);
    int res = system(cmd);
    assert(res == 0);

    /* Mount filesystem */
    yfs_filesystem_t *fs = yfs_fs_create(test_img);
    assert(fs != nullptr);
    assert(yfs_fs_mount(fs));

    /* Create file /hello.txt */
    uint32_t ino = 0;
    yfs_dinode_t dinode;
    int ret = yfs_create(fs, YFS_ROOT_INO, "hello.txt", 0644, 1000, 1000, &ino, &dinode);
    assert(ret == 0);
    assert(ino > 1);

    /* Write data */
    const char *content = "Hello, FreeBSD yFS journaling filesystem with FUSE v3 (C23 standard)!";
    size_t content_len = strlen(content);
    size_t written = 0;
    ret = yfs_write(fs, ino, content, content_len, 0, &written);
    assert(ret == 0);
    assert(written == content_len);

    /* Read back data */
    char read_buf[128] = {0};
    size_t bytes_read = 0;
    ret = yfs_read(fs, ino, read_buf, content_len, 0, &bytes_read);
    assert(ret == 0);
    assert(bytes_read == content_len);
    assert(memcmp(read_buf, content, content_len) == 0);

    /* Test readdir on root */
    yfs_dir_list_t list;
    yfs_dir_list_init(&list);
    ret = yfs_readdir(fs, YFS_ROOT_INO, &list);
    assert(ret == 0);
    assert(list.count == 3); /* ., .., hello.txt */
    yfs_dir_list_free(&list);

    /* Create directory /testdir */
    uint32_t dir_ino = 0;
    yfs_dinode_t dir_dinode;
    ret = yfs_mkdir(fs, YFS_ROOT_INO, "testdir", 0755, 1000, 1000, &dir_ino, &dir_dinode);
    assert(ret == 0);

    /* Lookup inside root */
    uint32_t found_ino = 0;
    yfs_dinode_t found_dinode;
    ret = yfs_lookup(fs, YFS_ROOT_INO, "testdir", &found_ino, &found_dinode);
    assert(ret == 0);
    assert(found_ino == dir_ino);

    /* Unlink file */
    ret = yfs_unlink(fs, YFS_ROOT_INO, "hello.txt");
    assert(ret == 0);

    /* Rmdir */
    ret = yfs_rmdir(fs, YFS_ROOT_INO, "testdir");
    assert(ret == 0);

    yfs_fs_destroy(fs);
    printf("[PASS] Basic FS operations test passed!\n");
}

void test_journal_recovery(void) {
    const char *test_img = "/tmp/yfs_recovery_test.img";
    unlink(test_img);

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "./newfs_yfs -s 16 %s", test_img);
    assert(system(cmd) == 0);

    /* Simulate dirty write with committed transaction without clean unmount */
    {
        yfs_filesystem_t *fs = yfs_fs_create(test_img);
        assert(fs != nullptr);
        assert(yfs_fs_mount(fs));

        uint32_t file_ino = 0;
        yfs_dinode_t dinode;
        yfs_create(fs, YFS_ROOT_INO, "crash_file.txt", 0644, 1000, 1000, &file_ino, &dinode);

        const char *payload = "Journal WAL recovery payload test.";
        size_t written = 0;
        yfs_write(fs, file_ino, payload, strlen(payload), 0, &written);

        /* Crash without calling yfs_fs_unmount (leave sb.s_clean_unmount = 0) */
        buffer_cache_sync_all(fs->cache);
        block_dev_flush(fs->dev);
        block_dev_close(fs->dev);
        fs->dev = nullptr;
        free(fs);
    }

    /* Reopen and mount -> triggers journal_recover() */
    {
        yfs_filesystem_t *fs = yfs_fs_create(test_img);
        assert(fs != nullptr);
        assert(yfs_fs_mount(fs));

        uint32_t found_ino = 0;
        yfs_dinode_t found_dinode;
        int ret = yfs_lookup(fs, YFS_ROOT_INO, "crash_file.txt", &found_ino, &found_dinode);
        assert(ret == 0);

        char buf[64] = {0};
        size_t bytes_read = 0;
        yfs_read(fs, found_ino, buf, 34, 0, &bytes_read);
        assert(strcmp(buf, "Journal WAL recovery payload test.") == 0);

        yfs_fs_destroy(fs);
    }

    printf("[PASS] Journal WAL crash recovery test passed!\n");
}

int main(void) {
    test_basic_fs_ops();
    test_journal_recovery();
    printf("All YFS C23 tests passed successfully!\n");
    return 0;
}
