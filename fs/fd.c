#include <stdlib.h>
#include <string.h>
#include "debug.h"
#include "kernel/calls.h"
#include "kernel/resource.h"
#include "kernel/fs.h"
#include "fs/poll.h"
#include "fs/fd.h"
#include "fs/inode.h"

struct fd *fd_create(const struct fd_ops *ops) {
    struct fd *fd = malloc(sizeof(struct fd));
    if (fd == NULL)
        return NULL;
    *fd = (struct fd) {};
    fd->ops = ops;
    fd->refcount = 1;
    fd->flags = 0;
    fd->mount = NULL;
    fd->offset = 0;
    list_init(&fd->poll_fds);
    list_init(&fd->pidfd_links);
    lock_init(&fd->poll_lock);
    lock_init(&fd->lock);
    cond_init(&fd->cond);
    return fd;
}

struct fd *fd_retain(struct fd *fd) {
    fd->refcount++;
    return fd;
}

int fd_close(struct fd *fd) {
    int err = 0;
    if (--fd->refcount == 0) {
        poll_cleanup_fd(fd);
        if (fd->ops->close)
            err = fd->ops->close(fd);
        // see comment in close in kernel/fs.h
        if (fd->mount && fd->mount->fs->close && fd->mount->fs->close != fd->ops->close) {
            int new_err = fd->mount->fs->close(fd);
            if (new_err < 0)
                err = new_err;
        }

        if (fd->inode)
            inode_release(fd->inode);
        if (fd->mount)
            mount_release(fd->mount);
        free(fd);
    }
    return err;
}

static int fdtable_resize(struct fdtable *table, unsigned size);

struct fdtable *fdtable_new(int size) {
    struct fdtable *fdt = malloc(sizeof(struct fdtable));
    if (fdt == NULL)
        return ERR_PTR(_ENOMEM);
    fdt->refcount = 1;
    fdt->size = 0;
    fdt->files = NULL;
    fdt->cloexec = NULL;
    fdt->generation = NULL;
    lock_init(&fdt->lock);
    int err = fdtable_resize(fdt, size);
    if (err < 0) {
        free(fdt);
        return ERR_PTR(err);
    }
    return fdt;
}

static int fdtable_close(struct fdtable *table, fd_t f);

void fdtable_release(struct fdtable *table) {
    // Drop the reference before touching the lock: a non-final releaser must
    // never unlock a mutex that the final releaser can already have freed.
    if (atomic_fetch_sub(&table->refcount, 1) == 1) {
        lock(&table->lock);
        for (fd_t f = 0; (unsigned) f < table->size; f++)
            fdtable_close(table, f);
        free(table->files);
        free(table->cloexec);
        free(table->generation);
        unlock(&table->lock);
        pthread_mutex_destroy(&table->lock.m);
        free(table);
    }
}

static int fdtable_resize(struct fdtable *table, unsigned size) {
    // currently the only legitimate use of this is to expand the table
    assert(size > table->size);

    struct fd **files = malloc(sizeof(struct fd *) * size);
    if (files == NULL)
        return _ENOMEM;
    memset(files, 0, sizeof(struct fd *) * size);
    if (table->files)
        memcpy(files, table->files, sizeof(struct fd *) * table->size);

    bits_t *cloexec = malloc(BITS_SIZE(size));
    if (cloexec == NULL) {
        free(files);
        return _ENOMEM;
    }
    memset(cloexec, 0, BITS_SIZE(size));
    if (table->cloexec)
        memcpy(cloexec, table->cloexec, BITS_SIZE(table->size));

    uint64_t *generation = calloc(size, sizeof(*generation));
    if (generation == NULL) {
        free(files);
        free(cloexec);
        return _ENOMEM;
    }
    if (table->generation)
        memcpy(generation, table->generation, sizeof(*generation) * table->size);

    // Commit all three allocations together; failures leave the table intact.
    free(table->generation);
    table->generation = generation;
    free(table->files);
    table->files = files;
    free(table->cloexec);
    table->cloexec = cloexec;
    table->size = size;
    return 0;
}

struct fdtable *fdtable_copy(struct fdtable *table) {
    lock(&table->lock);
    int size = table->size;
    struct fdtable *new_table = fdtable_new(size);
    if (IS_ERR(new_table)) {
        unlock(&table->lock);
        return new_table;
    }
    memcpy(new_table->files, table->files, sizeof(struct fd *) * size);
    for (fd_t f = 0; f < size; f++)
        if (new_table->files[f])
            new_table->files[f]->refcount++;
    memcpy(new_table->cloexec, table->cloexec, BITS_SIZE(size));
    memcpy(new_table->generation, table->generation, sizeof(*table->generation) * size);
    unlock(&table->lock);
    return new_table;
}

static int fdtable_expand(struct fdtable *table, fd_t max) {
    unsigned size = max + 1;
    if (size > rlimit(RLIMIT_NOFILE_))
        return _EMFILE;
    if (table->size >= size)
        return 0;
    return fdtable_resize(table, max + 1);
}

struct fd *fdtable_get(struct fdtable *table, fd_t f) {
    if (f < 0 || (unsigned) f >= table->size)
        return NULL;
    return table->files[f];
}

struct fd *f_get(fd_t f) {
    lock(&current->files->lock);
    struct fd *fd = fdtable_get(current->files, f);
    unlock(&current->files->lock);
    return fd;
}

// Acquire while the table still owns the object; retaining after f_get races close.
struct fd *f_get_retained(fd_t f) {
    struct fdtable *table = current->files;
    if (!table)
        return NULL;
    lock(&table->lock);
    struct fd *fd = fdtable_get(table, f);
    if (fd)
        fd_retain(fd);
    unlock(&table->lock);
    return fd;
}

void f_install_release(struct fd_install_token *token) {
    struct fdtable *table = token->table;
    *token = (struct fd_install_token) {};
    if (table)
        fdtable_release(table); // Always called without the table lock.
}

bool f_uninstall(struct fd_install_token *token) {
    struct fdtable *table = token->table;
    if (!table)
        return false;
    fd_t f = token->slot;
    struct fd *expected = token->expected;
    lock(&table->lock);
    bool installed = fdtable_get(table, f) == expected &&
        table->generation[f] == token->generation;
    if (installed) {
        if (expected->inode)
            file_lock_remove_owned_by(expected, table);
        table->files[f] = NULL;
        bit_clear(f, table->cloexec);
    }
    unlock(&table->lock);
    if (installed)
        fd_close(expected);
    f_install_release(token);
    return installed;
}

// Caller holds table->lock; consumes the reference only on success.
static fd_t f_install_start(struct fdtable *table, struct fd *fd, fd_t start) {
    assert(start >= 0);
    unsigned size = rlimit(RLIMIT_NOFILE_);
    if (size > table->size)
        size = table->size;

    fd_t f;
    for (f = start; (unsigned) f < size; f++)
        if (table->files[f] == NULL)
            break;
    if ((unsigned) f >= size) {
        int err = fdtable_expand(table, f);
        if (err < 0)
            f = err;
    }

    if (f >= 0) {
        if (table->generation[f] == UINT64_MAX)
            return _EOVERFLOW;
        ++table->generation[f];
        table->files[f] = fd;
        bit_clear(f, table->cloexec);
    }
    return f;
}

static fd_t f_install_common(struct fd *fd, int flags, struct fd_install_token *token) {
    struct fdtable *table = current->files;
    if (token)
        *token = (struct fd_install_token) {};
    lock(&table->lock);
    fd_t f = f_install_start(table, fd, 0);
    if (f >= 0) {
        if (flags & O_CLOEXEC_)
            bit_set(f, table->cloexec);
        if (flags & O_NONBLOCK_)
            fd_setflags(fd, O_NONBLOCK_);
        if (token) {
            // Capture and pin DURING publication, before any unlock/rebind.
            table->refcount++;
            *token = (struct fd_install_token) {
                .table = table, .slot = f,
                .generation = table->generation[f], .expected = fd,
            };
        }
    }
    unlock(&table->lock);
    if (f < 0)
        fd_close(fd);
    return f;
}

fd_t f_install(struct fd *fd, int flags) {
    return f_install_common(fd, flags, NULL);
}

fd_t f_install_owned(struct fd *fd, int flags, struct fd_install_token *token) {
    assert(token);
    return f_install_common(fd, flags, token);
}

static int fdtable_close(struct fdtable *table, fd_t f) {
    struct fd *fd = fdtable_get(table, f);
    if (fd == NULL)
        return _EBADF;
    if (fd->inode != NULL) // temporary hack for files like sockets that right now don't have inodes but will eventually
        file_lock_remove_owned_by(fd, table);
    int err = fd_close(fd);
    table->files[f] = NULL;
    bit_clear(f, table->cloexec);
    return err;
}

int f_close(fd_t f) {
    lock(&current->files->lock);
    int err = fdtable_close(current->files, f);
    unlock(&current->files->lock);
    return err;
}

dword_t sys_close(fd_t f) {
    STRACE("close(%d)", f);
    return f_close(f);
}

#define CLOSE_RANGE_UNSHARE_ (1u << 1)
#define CLOSE_RANGE_CLOEXEC_ (1u << 2)

dword_t sys_close_range(uint_t first, uint_t last, uint_t flags) {
    STRACE("close_range(%u, %u, %#x)", first, last, flags);

    if (flags & ~(CLOSE_RANGE_UNSHARE_ | CLOSE_RANGE_CLOEXEC_))
        return _EINVAL;
    if (first > last)
        return _EINVAL;

    struct fdtable *table = current->files;
    lock(&table->lock);

    if (table->size == 0) {
        unlock(&table->lock);
        return 0;
    }

    uint_t capped_last = last;
    if (capped_last >= table->size)
        capped_last = table->size - 1;
    if (first >= table->size || first > capped_last) {
        unlock(&table->lock);
        return 0;
    }

    for (uint_t f = first; f <= capped_last; f++) {
        if (flags & CLOSE_RANGE_CLOEXEC_) {
            if (table->files[f] != NULL)
                bit_set(f, table->cloexec);
            continue;
        }
        if (table->files[f] != NULL)
            fdtable_close(table, (fd_t)f);
    }

    unlock(&table->lock);
    return 0;
}

void fdtable_do_cloexec(struct fdtable *table) {
    lock(&table->lock);
    for (fd_t f = 0; (unsigned) f < table->size; f++)
        if (bit_test(f, table->cloexec))
            fdtable_close(table, f);
    unlock(&table->lock);
}

#define F_DUPFD_ 0
#define F_GETFD_ 1
#define F_SETFD_ 2
#define F_GETFL_ 3
#define F_SETFL_ 4

#define F_GETLK_ 5
#define F_SETLK_ 6
#define F_SETLKW_ 7
#define F_GETLK64_ 12
#define F_SETLK64_ 13
#define F_SETLKW64_ 14

#define F_DUPFD_CLOEXEC_ 1030

dword_t sys_dup(fd_t f) {
    STRACE("dup(%d)", f);
    struct fd *fd = f_get_retained(f);
    if (fd == NULL)
        return _EBADF;
    return f_install(fd, 0);
}

static dword_t dup_to(fd_t f, fd_t new_f, int_t flags, bool dup2) {
    if (flags & ~O_CLOEXEC_ || (!dup2 && f == new_f))
        return _EINVAL;
    struct fdtable *table = current->files;
    lock(&table->lock);
    struct fd *fd = fdtable_get(table, f);
    int err = _EBADF;
    if (!fd)
        goto out;
    if (f == new_f) { // Linux dup2: validate source, but preserve CLOEXEC/identity.
        err = new_f;
        goto out;
    }
    if (new_f < 0 || (unsigned) new_f >= rlimit(RLIMIT_NOFILE_))
        goto out;
    err = fdtable_expand(table, new_f);
    if (err < 0)
        goto out;
    if (table->generation[new_f] == UINT64_MAX) {
        err = _EOVERFLOW;
        goto out;
    }
    fd_retain(fd);
    fdtable_close(table, new_f);
    ++table->generation[new_f];
    table->files[new_f] = fd;
    bit_clear(new_f, table->cloexec);
    if (flags & O_CLOEXEC_)
        bit_set(new_f, table->cloexec);
    err = new_f;
out:
    unlock(&table->lock);
    return err;
}

dword_t sys_dup3(fd_t f, fd_t new_f, int_t flags) {
    STRACE("dup3(%d, %d, %d)", f, new_f, flags);
    return dup_to(f, new_f, flags, false);
}

dword_t sys_dup2(fd_t f, fd_t new_f) {
    return dup_to(f, new_f, 0, true);
}

int fd_getflags(struct fd *fd) {
    if (fd->ops->getflags)
        return fd->ops->getflags(fd);
    return fd->flags;
}

#define FD_ALLOWED_FLAGS (O_APPEND_ | O_NONBLOCK_)
int fd_setflags(struct fd *fd, int flags) {
    if (fd->ops->setflags)
        return fd->ops->setflags(fd, flags);
    fd->flags = (fd->flags & ~FD_ALLOWED_FLAGS) | (flags & FD_ALLOWED_FLAGS);
    return 0;
}

dword_t sys_fcntl(fd_t f, dword_t cmd, addr_t arg) {
    struct fdtable *table = current->files;
    if (cmd == F_DUPFD_ || cmd == F_DUPFD_CLOEXEC_) {
        lock(&table->lock);
        struct fd *source = fdtable_get(table, f);
        int result = _EBADF;
        if (!source)
            goto dup_out;
        if ((sdword_t) arg < 0 || arg >= rlimit(RLIMIT_NOFILE_)) {
            result = _EINVAL;
            goto dup_out;
        }
        fd_retain(source);
        result = f_install_start(table, source, arg);
        if (result >= 0 && cmd == F_DUPFD_CLOEXEC_)
            bit_set(result, table->cloexec);
        unlock(&table->lock);
        if (result < 0)
            fd_close(source);
        return result;
dup_out:
        unlock(&table->lock);
        return result;
    }
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    struct flock32_ flock32;
    struct flock_ flock;
    int err;
    switch (cmd) {
        case F_GETFD_:
            STRACE("fcntl(%d, F_GETFD)", f);
            return bit_test(f, table->cloexec);
        case F_SETFD_:
            STRACE("fcntl(%d, F_SETFD, 0x%x)", f, arg);
            if (arg & 1)
                bit_set(f, table->cloexec);
            else
                bit_clear(f, table->cloexec);
            return 0;

        case F_GETFL_:
            STRACE("fcntl(%d, F_GETFL)", f);
            return fd_getflags(fd);
        case F_SETFL_:
            STRACE("fcntl(%d, F_SETFL, %#x)", f, arg);
            return fd_setflags(fd, arg);

        case F_GETLK_:
            STRACE("fcntl(%d, F_GETLK, %#x)", f, arg);
            if (user_read(arg, &flock32, sizeof(flock32)))
                return _EFAULT;
            flock.type = flock32.type;
            flock.whence = flock32.whence;
            flock.start = flock32.start;
            flock.len = flock32.len;
            flock.pid = flock32.pid;
            err = fcntl_getlk(fd, &flock);
            if (err >= 0) {
                flock32.type = flock.type;
                flock32.whence = flock.whence;
                flock32.start = flock.start;
                flock32.len = flock.len;
                flock32.pid = flock.pid;
                if (user_write(arg, &flock32, sizeof(flock32)))
                    return _EFAULT;
            }
            return err;

        case F_GETLK64_:
            STRACE("fcntl(%d, F_GETLK64, %#x)", f, arg);
            if (user_read(arg, &flock, sizeof(flock)))
                return _EFAULT;
            err = fcntl_getlk(fd, &flock);
            if (err >= 0)
                if (user_write(arg, &flock, sizeof(flock)))
                    return _EFAULT;
            return err;

        case F_SETLK_:
        case F_SETLKW_:
            STRACE("fcntl(%d, F_SETLK%*s, %#x)", f, cmd == F_SETLKW_, "W", arg);
            if (user_read(arg, &flock32, sizeof(flock32)))
                return _EFAULT;
            flock.type = flock32.type;
            flock.whence = flock32.whence;
            flock.start = flock32.start;
            flock.len = flock32.len;
            flock.pid = flock32.pid;
            return fcntl_setlk(fd, &flock, cmd == F_SETLKW64_);

        case F_SETLK64_:
        case F_SETLKW64_:
            STRACE("fcntl(%d, F_SETLK%*s64, %#x)", f, cmd == F_SETLKW_, "W", arg);
            if (user_read(arg, &flock, sizeof(flock)))
                return _EFAULT;
            return fcntl_setlk(fd, &flock, cmd == F_SETLKW_);

        default:
            STRACE("fcntl(%d, %d)", f, cmd);
            return _EINVAL;
    }
}

dword_t sys_fcntl32(fd_t fd, dword_t cmd, addr_t arg) {
    switch (cmd) {
        case F_GETLK64_:
        case F_SETLK64_:
        case F_SETLKW64_:
            return _EINVAL;
    }
    return sys_fcntl(fd, cmd, arg);
}
