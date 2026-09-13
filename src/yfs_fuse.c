#define FUSE_USE_VERSION 31

#include <fuse_lowlevel.h>
#include "yfs_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

static yfs_filesystem_t *g_fs = nullptr;

static void dinode_to_stat(uint32_t ino, const yfs_dinode_t *dinode, struct stat *st) {
    memset(st, 0, sizeof(*st));
    st->st_ino = ino;
    st->st_mode = dinode->di_mode;
    st->st_nlink = dinode->di_nlink;
    st->st_uid = dinode->di_uid;
    st->st_gid = dinode->di_gid;
    st->st_size = (off_t)dinode->di_size;
    st->st_atime = (time_t)dinode->di_atime;
    st->st_mtime = (time_t)dinode->di_mtime;
    st->st_ctime = (time_t)dinode->di_ctime;
    st->st_blksize = YFS_DEFAULT_BSIZE;
    st->st_blocks = (blkcnt_t)((dinode->di_size + 511) / 512);
}

static void yfs_ll_lookup(fuse_req_t req, fuse_ino_t parent, const char *name) {
    uint32_t out_ino = 0;
    yfs_dinode_t dinode;
    int ret = yfs_lookup(g_fs, (uint32_t)parent, name, &out_ino, &dinode);
    if (ret != 0) {
        fuse_reply_err(req, -ret);
        return;
    }

    struct fuse_entry_param e;
    memset(&e, 0, sizeof(e));
    e.ino = out_ino;
    e.attr_timeout = 1.0;
    e.entry_timeout = 1.0;
    dinode_to_stat(out_ino, &dinode, &e.attr);
    fuse_reply_entry(req, &e);
}

static void yfs_ll_forget(fuse_req_t req, fuse_ino_t ino, uint64_t nlookup) {
    (void)ino;
    (void)nlookup;
    fuse_reply_none(req);
}

static void yfs_ll_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
    (void)fi;
    yfs_dinode_t dinode;
    int ret = yfs_getattr(g_fs, (uint32_t)ino, &dinode);
    if (ret != 0) {
        fuse_reply_err(req, -ret);
        return;
    }

    struct stat st;
    dinode_to_stat((uint32_t)ino, &dinode, &st);
    fuse_reply_attr(req, &st, 1.0);
}

static void yfs_ll_setattr(fuse_req_t req, fuse_ino_t ino, struct stat *attr, int to_set, struct fuse_file_info *fi) {
    (void)fi;
    yfs_dinode_t dinode;
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
        yfs_truncate(g_fs, (uint32_t)ino, attr->st_size);
    }
    if (to_set & FUSE_SET_ATTR_ATIME) {
        dinode.di_atime = (uint64_t)attr->st_atime;
        yfs_flags |= 16;
    }
    if (to_set & FUSE_SET_ATTR_MTIME) {
        dinode.di_mtime = (uint64_t)attr->st_mtime;
        yfs_flags |= 32;
    }

    int ret = yfs_setattr(g_fs, (uint32_t)ino, &dinode, yfs_flags);
    if (ret != 0) {
        fuse_reply_err(req, -ret);
        return;
    }

    yfs_dinode_t out_dinode;
    yfs_getattr(g_fs, (uint32_t)ino, &out_dinode);
    struct stat st;
    dinode_to_stat((uint32_t)ino, &out_dinode, &st);
    fuse_reply_attr(req, &st, 1.0);
}

static void yfs_ll_create(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode, struct fuse_file_info *fi) {
    const struct fuse_ctx *ctx = fuse_req_ctx(req);
    uint32_t out_ino = 0;
    yfs_dinode_t dinode;
    int ret = yfs_create(g_fs, (uint32_t)parent, name, mode, ctx->uid, ctx->gid, &out_ino, &dinode);
    if (ret != 0) {
        fuse_reply_err(req, -ret);
        return;
    }

    struct fuse_entry_param e;
    memset(&e, 0, sizeof(e));
    e.ino = out_ino;
    e.attr_timeout = 1.0;
    e.entry_timeout = 1.0;
    dinode_to_stat(out_ino, &dinode, &e.attr);

    fuse_reply_create(req, &e, fi);
}

static void yfs_ll_mkdir(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode) {
    const struct fuse_ctx *ctx = fuse_req_ctx(req);
    uint32_t out_ino = 0;
    yfs_dinode_t dinode;
    int ret = yfs_mkdir(g_fs, (uint32_t)parent, name, mode, ctx->uid, ctx->gid, &out_ino, &dinode);
    if (ret != 0) {
        fuse_reply_err(req, -ret);
        return;
    }

    struct fuse_entry_param e;
    memset(&e, 0, sizeof(e));
    e.ino = out_ino;
    e.attr_timeout = 1.0;
    e.entry_timeout = 1.0;
    dinode_to_stat(out_ino, &dinode, &e.attr);

    fuse_reply_entry(req, &e);
}

static void yfs_ll_unlink(fuse_req_t req, fuse_ino_t parent, const char *name) {
    int ret = yfs_unlink(g_fs, (uint32_t)parent, name);
    fuse_reply_err(req, ret == 0 ? 0 : -ret);
}

static void yfs_ll_rmdir(fuse_req_t req, fuse_ino_t parent, const char *name) {
    int ret = yfs_rmdir(g_fs, (uint32_t)parent, name);
    fuse_reply_err(req, ret == 0 ? 0 : -ret);
}

static void yfs_ll_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
    (void)ino;
    fuse_reply_open(req, fi);
}

static void yfs_ll_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi) {
    (void)fi;
    char *buf = (char *)calloc(1, size);
    if (!buf) {
        fuse_reply_err(req, ENOMEM);
        return;
    }

    size_t bytes_read = 0;
    int ret = yfs_read(g_fs, (uint32_t)ino, buf, size, off, &bytes_read);
    if (ret != 0) {
        free(buf);
        fuse_reply_err(req, -ret);
        return;
    }
    fuse_reply_buf(req, buf, bytes_read);
    free(buf);
}

static void yfs_ll_write(fuse_req_t req, fuse_ino_t ino, const char *buf, size_t size, off_t off, struct fuse_file_info *fi) {
    (void)fi;
    size_t bytes_written = 0;
    int ret = yfs_write(g_fs, (uint32_t)ino, buf, size, off, &bytes_written);
    if (ret != 0) {
        fuse_reply_err(req, -ret);
        return;
    }
    fuse_reply_write(req, bytes_written);
}

static void yfs_ll_flush(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
    (void)ino;
    (void)fi;
    fuse_reply_err(req, 0);
}

static void yfs_ll_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
    (void)ino;
    (void)fi;
    fuse_reply_err(req, 0);
}

static void yfs_ll_fsync(fuse_req_t req, fuse_ino_t ino, int datasync, struct fuse_file_info *fi) {
    (void)ino;
    (void)datasync;
    (void)fi;
    if (g_fs && g_fs->tx_mgr) {
        tx_manager_checkpoint(g_fs->tx_mgr);
    }
    fuse_reply_err(req, 0);
}

static void yfs_ll_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
    (void)ino;
    fuse_reply_open(req, fi);
}

static void yfs_ll_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi) {
    (void)fi;
    yfs_dir_list_t list;
    yfs_dir_list_init(&list);

    int ret = yfs_readdir(g_fs, (uint32_t)ino, &list);
    if (ret != 0) {
        yfs_dir_list_free(&list);
        fuse_reply_err(req, -ret);
        return;
    }

    char *buf = (char *)calloc(1, size);
    if (!buf) {
        yfs_dir_list_free(&list);
        fuse_reply_err(req, ENOMEM);
        return;
    }

    size_t offset = 0;
    for (size_t i = (size_t)off; i < list.count; ++i) {
        struct stat st;
        memset(&st, 0, sizeof(st));
        st.st_ino = list.entries[i].ino;
        st.st_mode = (list.entries[i].type == 2) ? (S_IFDIR | 0755) : (S_IFREG | 0644);

        size_t entry_size = fuse_add_direntry(req, buf + offset, size - offset,
                                              list.entries[i].name, &st, (off_t)(i + 1));
        if (offset + entry_size > size) {
            break;
        }
        offset += entry_size;
    }

    yfs_dir_list_free(&list);
    fuse_reply_buf(req, buf, offset);
    free(buf);
}

static void yfs_ll_releasedir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
    (void)ino;
    (void)fi;
    fuse_reply_err(req, 0);
}

static void yfs_ll_fsyncdir(fuse_req_t req, fuse_ino_t ino, int datasync, struct fuse_file_info *fi) {
    (void)ino;
    (void)datasync;
    (void)fi;
    fuse_reply_err(req, 0);
}

static void yfs_ll_statfs(fuse_req_t req, fuse_ino_t ino) {
    (void)ino;
    uint64_t total_blocks = 0, free_blocks = 0, total_inodes = 0, free_inodes = 0;
    yfs_statfs(g_fs, &total_blocks, &free_blocks, &total_inodes, &free_inodes);

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

static void yfs_ll_readlink(fuse_req_t req, fuse_ino_t ino) {
    char buf[1024] = {0};
    int ret = yfs_readlink(g_fs, (uint32_t)ino, buf, sizeof(buf));
    if (ret != 0) {
        fuse_reply_err(req, -ret);
        return;
    }
    fuse_reply_readlink(req, buf);
}

static void yfs_ll_mknod(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode, dev_t rdev) {
    const struct fuse_ctx *ctx = fuse_req_ctx(req);
    uint32_t out_ino = 0;
    yfs_dinode_t dinode;
    int ret = yfs_mknod(g_fs, (uint32_t)parent, name, mode, rdev, ctx->uid, ctx->gid, &out_ino, &dinode);
    if (ret != 0) {
        fuse_reply_err(req, -ret);
        return;
    }
    struct fuse_entry_param e;
    memset(&e, 0, sizeof(e));
    e.ino = out_ino;
    e.attr_timeout = 1.0;
    e.entry_timeout = 1.0;
    dinode_to_stat(out_ino, &dinode, &e.attr);
    fuse_reply_entry(req, &e);
}

static void yfs_ll_symlink(fuse_req_t req, const char *link, fuse_ino_t parent, const char *name) {
    const struct fuse_ctx *ctx = fuse_req_ctx(req);
    uint32_t out_ino = 0;
    yfs_dinode_t dinode;
    int ret = yfs_symlink(g_fs, link, (uint32_t)parent, name, ctx->uid, ctx->gid, &out_ino, &dinode);
    if (ret != 0) {
        fuse_reply_err(req, -ret);
        return;
    }
    struct fuse_entry_param e;
    memset(&e, 0, sizeof(e));
    e.ino = out_ino;
    e.attr_timeout = 1.0;
    e.entry_timeout = 1.0;
    dinode_to_stat(out_ino, &dinode, &e.attr);
    fuse_reply_entry(req, &e);
}

static void yfs_ll_rename(fuse_req_t req, fuse_ino_t parent, const char *name, fuse_ino_t newparent, const char *newname, unsigned int flags) {
    (void)flags;
    int ret = yfs_rename(g_fs, (uint32_t)parent, name, (uint32_t)newparent, newname);
    fuse_reply_err(req, ret == 0 ? 0 : -ret);
}

static void yfs_ll_link(fuse_req_t req, fuse_ino_t ino, fuse_ino_t newparent, const char *newname) {
    (void)ino;
    (void)newparent;
    (void)newname;
    fuse_reply_err(req, ENOSYS);
}

static const struct fuse_lowlevel_ops yfs_oper = {
    .init = nullptr,
    .destroy = nullptr,
    .lookup = yfs_ll_lookup,
    .forget = yfs_ll_forget,
    .getattr = yfs_ll_getattr,
    .setattr = yfs_ll_setattr,
    .readlink = yfs_ll_readlink,
    .mknod = yfs_ll_mknod,
    .mkdir = yfs_ll_mkdir,
    .unlink = yfs_ll_unlink,
    .rmdir = yfs_ll_rmdir,
    .symlink = yfs_ll_symlink,
    .rename = yfs_ll_rename,
    .link = yfs_ll_link,
    .open = yfs_ll_open,
    .read = yfs_ll_read,
    .write = yfs_ll_write,
    .flush = yfs_ll_flush,
    .release = yfs_ll_release,
    .fsync = yfs_ll_fsync,
    .opendir = yfs_ll_opendir,
    .readdir = yfs_ll_readdir,
    .releasedir = yfs_ll_releasedir,
    .fsyncdir = yfs_ll_fsyncdir,
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
        fprintf(stderr, "Usage: %s <image_or_device_path> <mount_point> [FUSE options...]\n", argv[0]);
        return 1;
    }

    const char *dev_path = argv[1];

    g_fs = yfs_fs_create(dev_path);
    if (!g_fs || !yfs_fs_mount(g_fs, 65536)) {
        fprintf(stderr, "Failed to mount yFS filesystem from %s\n", dev_path);
        if (g_fs) yfs_fs_destroy(g_fs);
        return 1;
    }

    int fuse_argc = argc - 1;
    char **fuse_argv = (char **)calloc(fuse_argc + 1, sizeof(char *));
    fuse_argv[0] = argv[0];
    fuse_argv[1] = argv[2];
    for (int i = 3; i < argc; ++i) {
        fuse_argv[i - 1] = argv[i];
    }
    fuse_argv[fuse_argc] = nullptr;

    struct fuse_args args = FUSE_ARGS_INIT(fuse_argc, fuse_argv);
    struct fuse_cmdline_opts opts;
    if (fuse_parse_cmdline(&args, &opts) != 0) {
        free(fuse_argv);
        return 1;
    }

    struct fuse_session *se = fuse_session_new(&args, &yfs_oper, sizeof(yfs_oper), nullptr);
    if (!se) {
        fprintf(stderr, "Failed to create fuse session\n");
        free(fuse_argv);
        return 1;
    }

    if (fuse_set_signal_handlers(se) != 0) {
        fprintf(stderr, "Failed to set signal handlers\n");
        fuse_session_destroy(se);
        free(fuse_argv);
        return 1;
    }

    if (fuse_session_mount(se, opts.mountpoint) != 0) {
        fprintf(stderr, "Failed to mount fuse session at %s\n", opts.mountpoint);
        fuse_remove_signal_handlers(se);
        fuse_session_destroy(se);
        free(fuse_argv);
        return 1;
    }

    if (fuse_daemonize(opts.foreground) != 0) {
        fuse_session_unmount(se);
        fuse_remove_signal_handlers(se);
        fuse_session_destroy(se);
        free(fuse_argv);
        return 1;
    }

    int ret = fuse_session_loop(se);

    fuse_session_unmount(se);
    fuse_remove_signal_handlers(se);
    fuse_session_destroy(se);
    free(opts.mountpoint);
    fuse_opt_free_args(&args);
    free(fuse_argv);

    yfs_fs_destroy(g_fs);
    g_fs = nullptr;

    return ret ? 1 : 0;
}
