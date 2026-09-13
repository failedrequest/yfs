#include "include/yfs_fs.h"
#include "include/yfs_core.h"
#include <cassert>
#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <unistd.h>

void test_basic_fs_ops() {
    std::string test_img = "/tmp/yfs_test.img";
    unlink(test_img.c_str());

    /* Format */
    std::string cmd = "./newfs_yfs -s 16 " + test_img;
    int res = system(cmd.c_str());
    assert(res == 0);

    /* Mount filesystem */
    YFSFileSystem fs(test_img);
    assert(fs.mount());

    /* Create file /hello.txt */
    uint32_t ino = 0;
    yfs_dinode dinode;
    int ret = fs.create(YFS_ROOT_INO, "hello.txt", 0644, 1000, 1000, ino, dinode);
    assert(ret == 0);
    assert(ino > 1);

    /* Write data */
    std::string content = "Hello, FreeBSD yFS journaling filesystem with FUSE v3!";
    size_t written = 0;
    ret = fs.write(ino, content.data(), content.size(), 0, written);
    assert(ret == 0);
    assert(written == content.size());

    /* Read back data */
    std::vector<char> read_buf(128, 0);
    size_t bytes_read = 0;
    ret = fs.read(ino, read_buf.data(), content.size(), 0, bytes_read);
    assert(ret == 0);
    assert(bytes_read == content.size());
    assert(std::string(read_buf.data(), bytes_read) == content);

    /* Test readdir on root */
    std::vector<DirEntry> entries;
    ret = fs.readdir(YFS_ROOT_INO, entries);
    assert(ret == 0);
    assert(entries.size() == 3); /* ., .., hello.txt */

    /* Create directory /testdir */
    uint32_t dir_ino = 0;
    yfs_dinode dir_dinode;
    ret = fs.mkdir(YFS_ROOT_INO, "testdir", 0755, 1000, 1000, dir_ino, dir_dinode);
    assert(ret == 0);

    /* Lookup inside root */
    uint32_t found_ino = 0;
    yfs_dinode found_dinode;
    ret = fs.lookup(YFS_ROOT_INO, "testdir", found_ino, found_dinode);
    assert(ret == 0);
    assert(found_ino == dir_ino);

    /* Unlink file */
    ret = fs.unlink(YFS_ROOT_INO, "hello.txt");
    assert(ret == 0);

    /* Rmdir */
    ret = fs.rmdir(YFS_ROOT_INO, "testdir");
    assert(ret == 0);

    fs.unmount();
    std::cout << "[PASS] Basic FS operations test passed!" << std::endl;
}

void test_journal_recovery() {
    std::string test_img = "/tmp/yfs_recovery_test.img";
    unlink(test_img.c_str());

    std::string cmd = "./newfs_yfs -s 16 " + test_img;
    assert(system(cmd.c_str()) == 0);

    /* Simulate dirty write with committed transaction */
    {
        YFSFileSystem fs(test_img);
        assert(fs.mount());

        uint32_t file_ino = 0;
        yfs_dinode dinode;
        fs.create(YFS_ROOT_INO, "crash_file.txt", 0644, 1000, 1000, file_ino, dinode);

        std::string payload = "Journal WAL recovery payload test.";
        size_t written = 0;
        fs.write(file_ino, payload.data(), payload.size(), 0, written);

        /* Simulating crash by NOT calling clean unmount - journal has records committed */
    }

    /* Reopen and mount -> triggers journal->recover() */
    {
        YFSFileSystem fs(test_img);
        assert(fs.mount());

        uint32_t found_ino = 0;
        yfs_dinode found_dinode;
        int ret = fs.lookup(YFS_ROOT_INO, "crash_file.txt", found_ino, found_dinode);
        assert(ret == 0);

        std::vector<char> buf(64, 0);
        size_t bytes_read = 0;
        fs.read(found_ino, buf.data(), 34, 0, bytes_read);
        assert(std::string(buf.data(), bytes_read) == "Journal WAL recovery payload test.");

        fs.unmount();
    }

    std::cout << "[PASS] Journal WAL crash recovery test passed!" << std::endl;
}

int main() {
    test_basic_fs_ops();
    test_journal_recovery();
    std::cout << "All YFS tests passed successfully!" << std::endl;
    return 0;
}
