#include <sys/stat.h>
#include <unistd.h>
#include "kernel/calls.h"
#include "fs/fd.h"
#include "fs/real.h"
#include "debug.h"

// Consumes pipe_fd on every path. Keep an owner until publication completes.
static struct fd *pipe_f_alloc(int pipe_fd) {
    struct fd *fd = adhoc_fd_create(&realfs_fdops);
    if (fd == NULL) {
        close(pipe_fd);
        return ERR_PTR(_ENOMEM);
    }
    fd->real_fd = pipe_fd;
    fd->stat.mode = S_IFIFO | 0660;
    fd->stat.uid = current->uid;
    fd->stat.gid = current->gid;
    return fd;
}

int_t sys_pipe2(addr_t pipe_addr, int_t flags) {
    STRACE("pipe2(%#x, %#x)", pipe_addr, flags);
    if (flags & ~(O_CLOEXEC_|O_NONBLOCK_)) {
        FIXME("unsupported pipe2 flags");
        return _EINVAL;
    }

    int p[2];
    int err = pipe(p);
    if (err < 0)
        return errno_map();

    struct fd *ends[2];
    ends[0] = pipe_f_alloc(p[0]);
    if (IS_ERR(ends[0])) { close(p[1]); return PTR_ERR(ends[0]); }
    ends[1] = pipe_f_alloc(p[1]);
    if (IS_ERR(ends[1])) { fd_close(ends[0]); return PTR_ERR(ends[1]); }
    int fp[2];
    struct fd_install_token tokens[2] = {};
    err = fp[0] = f_install_owned(fd_retain(ends[0]), flags, &tokens[0]);
    if (fp[0] < 0) goto out_refs;
    err = fp[1] = f_install_owned(fd_retain(ends[1]), flags, &tokens[1]);
    if (fp[1] < 0) goto close_fake_0;
    err = _EFAULT;
    if (user_put(pipe_addr, fp)) goto close_fake_1;
    STRACE(" [%d %d]", fp[0], fp[1]);
    err = 0;
    goto out_refs;
close_fake_1:
    f_uninstall(&tokens[1]);
close_fake_0:
    f_uninstall(&tokens[0]);
out_refs:
    f_install_release(&tokens[0]);
    f_install_release(&tokens[1]);
    fd_close(ends[0]); fd_close(ends[1]);
    return err;
}

int_t sys_pipe(addr_t pipe_addr) {
    return sys_pipe2(pipe_addr, 0);
}
