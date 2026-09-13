#define FUSE_USE_VERSION 31

#include <fuse_lowlevel.h>
#include "include/yfs_core.h"
#include <iostream>
#include <cstring>
#include <cerrno>
#include <unistd.h>

static YFSFileSystem *g_fs = nullptr;

static void dinode_to_stat(uint32_t ino, const yfs_dinode &dinode, struct stat &st) {
    memset(&st, 0, sizeof(st));
    st.st_ino = ino;
    st.st_mode = dinode.di_mode;
    st.st_nlink = dinode.di_nlink;
    st.st_uid = dinode.di_uid;
    st.st_gid = dinode.di_gid;
    st.st_size = dinode.di_size;
    st.st_atime = dinode.di_atime;
    st.st_mtime = dinode.di_mtime;
    st.st_ctime = dinode.di_ctime;
    st.st_blksize = YFS_DEFAULT_BSIZE;
    st.st_blocks = (dinode.di_size + 511) / 512;
}

static void yfs_ll_lookup(fuse_req_t req, fuse_ino_t parent, const char *name) {
    uint32_t out_ino = 0;
    yfs_dinode dinode;
    int ret = g_fs->lookup(parent, name, out_ino, dinode);
    if (ret != 0) {
        fuse_reply_err(req, -ret);
        return;
    }

    struct fuse_entry_param e;
    memset(&e, 0, sizeof(e));
    e.ino = out_ino;
    e.attr_timeout = 1.0;
    e.entry_timeout = 1.0;
    dinode_to_stat(out_ino, dinode, e.attr);
    fuse_reply_entry(req, &e);
}

static void yfs_ll_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
    (void)fi;
    yfs_dinode dinode;
    int ret = g_fs->getattr(ino, dinode);
    if (ret != 0) {
        fuse_reply_err(req, -ret);
        return;
    }

    struct stat st;
    dinode_to_stat(ino, dinode, st);
    fuse_reply_attr(req, &st, 1.0);
}

static void yfs_ll_setattr(fuse_req_t req, fuse_ino_t ino, struct stat *attr, int to_set, struct fuse_file_info *fi) {
    (void)fi;
    yfs_dinode dinode;
    memset(&dinode, 0, sizeof(dinode));
    int yfs_flags = 0;

    if (to_set & FUSE_SET_ATTR_MODE) {
        dinode.di_mode = attr->st_mode;
        yfs_flags |= 1;
    }
    if (to_set & FUSE_SET_ATTR_UID) {
        dinode.di_uid = attr->st_uid;
        yfs_flags |= 2;
    }
    if (to_set & FUSE_SET_ATTR_GID) {
        dinode.di_gid = attr->st_gid;
        yfs_flags |= 4;
    }
    if (to_set & FUSE_SET_ATTR_SIZE) {
        g_fs->truncate(ino, attr->st_size);
    }
    if (to_set & FUSE_SET_ATTR_ATIME) {
        dinode.di_atime = attr->st_atime;
        yfs_flags |= 16;
    }
    if (to_set & FUSE_SET_ATTR_MTIME) {
        dinode.di_mtime = attr->st_mtime;
        yfs_flags |= 32;
    }

    int ret = g_fs->setattr(ino, dinode, yfs_flags);
    if (ret != 0) {
        fuse_reply_err(req, -ret);
        return;
    }

    yfs_dinode out_dinode;
    g_fs->getattr(ino, out_dinode);
    struct stat st;
    dinode_to_stat(ino, out_dinode, st);
    fuse_reply_attr(req, &st, 1.0);
}

static void yfs_ll_create(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode, struct fuse_file_info *fi) {
    const struct fuse_ctx *ctx = fuse_req_ctx(req);
    uint32_t out_ino = 0;
    yfs_dinode dinode;
    int ret = g_fs->create(parent, name, mode, ctx->uid, ctx->gid, out_ino, dinode);
    if (ret != 0) {
        fuse_reply_err(req, -ret);
        return;
    }

    struct fuse_entry_param e;
    memset(&e, 0, sizeof(e));
    e.ino = out_ino;
    e.attr_timeout = 1.0;
    e.entry_timeout = 1.0;
    dinode_to_stat(out_ino, dinode, e.attr);

    fuse_reply_create(req, &e, fi);
}

static void yfs_ll_mkdir(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode) {
    const struct fuse_ctx *ctx = fuse_req_ctx(req);
    uint32_t out_ino = 0;
    yfs_dinode dinode;
    int ret = g_fs->mkdir(parent, name, mode, ctx->uid, ctx->gid, out_ino, dinode);
    if (ret != 0) {
        fuse_reply_err(req, -ret);
        return;
    }

    struct fuse_entry_param e;
    memset(&e, 0, sizeof(e));
    e.ino = out_ino;
    e.attr_timeout = 1.0;
    e.entry_timeout = 1.0;
    dinode_to_stat(out_ino, dinode, e.attr);

    fuse_reply_entry(req, &e);
}

static void yfs_ll_unlink(fuse_req_t req, fuse_ino_t parent, const char *name) {
    int ret = g_fs->unlink(parent, name);
    fuse_reply_err(req, ret == 0 ? 0 : -ret);
}

static void yfs_ll_rmdir(fuse_req_t req, fuse_ino_t parent, const char *name) {
    int ret = g_fs->rmdir(parent, name);
    fuse_reply_err(req, ret == 0 ? 0 : -ret);
}

static void yfs_ll_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi) {
    (void)fi;
    std::vector<char> buf(size, 0);
    size_t bytes_read = 0;
    int ret = g_fs->read(ino, buf.data(), size, off, bytes_read);
    if (ret != 0) {
        fuse_reply_err(req, -ret);
        return;
    }
    fuse_reply_buf(req, buf.data(), bytes_read);
}

static void yfs_ll_write(fuse_req_t req, fuse_ino_t ino, const char *buf, size_t size, off_t off, struct fuse_file_info *fi) {
    (void)fi;
    size_t bytes_written = 0;
    int ret = g_fs->write(ino, buf, size, off, bytes_written);
    if (ret != 0) {
        fuse_reply_err(req, -ret);
        return;
    }
    fuse_reply_write(req, bytes_written);
}

static void yfs_ll_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi) {
    (void)fi;
    std::vector<DirEntry> entries;
    int ret = g_fs->readdir(ino, entries);
    if (ret != 0) {
        fuse_reply_err(req, -ret);
        return;
    }

    std::vector<char> buf(size, 0);
    size_t offset = 0;

    for (size_t i = off; i < entries.size(); ++i) {
        struct stat st;
        memset(&st, 0, sizeof(st));
        st.st_ino = entries[i].ino;
        st.st_mode = (entries[i].type == 2) ? (S_IFDIR | 0755) : (S_IFREG | 0644);

        size_t entry_size = fuse_add_direntry(req, buf.data() + offset, size - offset,
                                              entries[i].name.c_str(), &st, i + 1);
        if (offset + entry_size > size) {
            break;
        }
        offset += entry_size;
    }

    fuse_reply_buf(req, buf.data(), offset);
}

static void yfs_ll_statfs(fuse_req_t req, fuse_ino_t ino) {
    (void)ino;
    uint64_t total_blocks = 0, free_blocks = 0, total_inodes = 0, free_inodes = 0;
    g_fs->statfs(total_blocks, free_blocks, total_inodes, free_inodes);

    struct statvfs stbuf;
    memset(&stbuf, 0, sizeof(stbuf));
    stbuf.f_bsize = YFS_DEFAULT_BSIZE;
    stbuf.f_frsize = YFS_DEFAULT_BSIZE;
    stbuf.f_blocks = total_blocks;
    stbuf.f_bfree = free_blocks;
    stbuf.f_bavail = free_blocks;
    stbuf.f_files = total_inodes;
    stbuf.f_ffree = free_inodes;
    stbuf.f_namemax = YFS_MAX_NAME_LEN;

    fuse_reply_statfs(req, &stbuf);
}

static const struct fuse_lowlevel_ops yfs_oper = {
    .init = nullptr,
    .destroy = nullptr,
    .lookup = yfs_ll_lookup,
    .forget = nullptr,
    .getattr = yfs_ll_getattr,
    .setattr = yfs_ll_setattr,
    .readlink = nullptr,
    .mknod = nullptr,
    .mkdir = yfs_ll_mkdir,
    .unlink = yfs_ll_unlink,
    .rmdir = yfs_ll_rmdir,
    .symlink = nullptr,
    .rename = nullptr,
    .link = nullptr,
    .open = [](fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
        (void)ino;
        fuse_reply_open(req, fi);
    },
    .read = yfs_ll_read,
    .write = yfs_ll_write,
    .flush = [](fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
        (void)ino;
        (void)fi;
        fuse_reply_err(req, 0);
    },
    .release = [](fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
        (void)ino;
        (void)fi;
        fuse_reply_err(req, 0);
    },
    .fsync = [](fuse_req_t req, fuse_ino_t ino, int datasync, struct fuse_file_info *fi) {
        (void)ino;
        (void)datasync;
        (void)fi;
        if (g_fs) {
            g_fs->tx_mgr()->checkpoint();
        }
        fuse_reply_err(req, 0);
    },
    .opendir = [](fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
        (void)ino;
        fuse_reply_open(req, fi);
    },
    .readdir = yfs_ll_readdir,
    .releasedir = [](fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
        (void)ino;
        (void)fi;
        fuse_reply_err(req, 0);
    },
    .fsyncdir = [](fuse_req_t req, fuse_ino_t ino, int datasync, struct fuse_file_info *fi) {
        (void)ino;
        (void)datasync;
        (void)fi;
        fuse_reply_err(req, 0);
    },
    .statfs = yfs_ll_statfs,
    .setxattr = nullptr,
    .getxattr = nullptr,
    .listxattr = nullptr,
    .removexattr = nullptr,
    .access = nullptr,
    .create = yfs_ll_create,
    .getlk = nullptr,
    .setlk = nullptr,
    .bmap = nullptr,
    .ioctl = nullptr,
    .poll = nullptr,
    .write_buf = nullptr,
    .retrieve_reply = nullptr,
    .forget_multi = nullptr,
    .flock = nullptr,
    .fallocate = nullptr,
    .readdirplus = nullptr,
    .copy_file_range = nullptr,
    .lseek = nullptr
};

int main(int argc, char **argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <image_or_device_path> <mount_point> [FUSE options...]" << std::endl;
        return 1;
    }

    std::string dev_path = argv[1];
    std::string mountpoint = argv[2];

    g_fs = new YFSFileSystem(dev_path);
    if (!g_fs->mount()) {
        std::cerr << "Failed to mount yFS filesystem from " << dev_path << std::endl;
        delete g_fs;
        return 1;
    }

    /* Shift arguments for FUSE */
    int fuse_argc = argc - 1;
    char **fuse_argv = new char *[fuse_argc + 1];
    fuse_argv[0] = argv[0];
    fuse_argv[1] = argv[2]; /* mountpoint */
    for (int i = 3; i < argc; ++i) {
        fuse_argv[i - 1] = argv[i];
    }
    fuse_argv[fuse_argc] = nullptr;

    struct fuse_args args = FUSE_ARGS_INIT(fuse_argc, fuse_argv);
    struct fuse_cmdline_opts opts;
    if (fuse_parse_cmdline(&args, &opts) != 0) {
        return 1;
    }

    struct fuse_session *se = fuse_session_new(&args, &yfs_oper, sizeof(yfs_oper), nullptr);
    if (!se) {
        std::cerr << "Failed to create fuse session" << std::endl;
        return 1;
    }

    if (fuse_set_signal_handlers(se) != 0) {
        std::cerr << "Failed to set signal handlers" << std::endl;
        return 1;
    }

    if (fuse_session_mount(se, opts.mountpoint) != 0) {
        std::cerr << "Failed to mount fuse session at " << opts.mountpoint << std::endl;
        fuse_remove_signal_handlers(se);
        fuse_session_destroy(se);
        return 1;
    }

    if (fuse_daemonize(opts.foreground) != 0) {
        fuse_session_unmount(se);
        fuse_remove_signal_handlers(se);
        fuse_session_destroy(se);
        return 1;
    }

    int ret = fuse_session_loop(se);

    fuse_session_unmount(se);
    fuse_remove_signal_handlers(se);
    fuse_session_destroy(se);
    free(opts.mountpoint);
    fuse_opt_free_args(&args);

    delete[] fuse_argv;
    g_fs->unmount();
    delete g_fs;

    return ret ? 1 : 0;
}
