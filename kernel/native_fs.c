// Guest-thread-owned filesystem snapshots; no host CWD or raw backing paths.
#include <stdlib.h>
#include "kernel/native_offload.h"
#include "kernel/task.h"
#include "kernel/fs.h"
#include "kernel/errno.h"
#include "fs/fd.h"
struct native_fs_context {
    struct task *owner;
    pthread_t thread;
    struct fs_info *fs;
};
static bool context_owned(struct native_fs_context *context) {
    return context && current == context->owner && pthread_equal(pthread_self(), context->thread);
}
struct native_fs_context *native_fs_context_create(void) {
    if (!current || !current->fs) return ERR_PTR(_ESRCH);
    struct native_fs_context *context = malloc(sizeof(*context));
    if (!context) return ERR_PTR(_ENOMEM);
    struct fs_info *snapshot = fs_info_new();
    if (!snapshot) { free(context); return ERR_PTR(_ENOMEM); }
    lock(&current->fs->lock);
    snapshot->umask = current->fs->umask;
    snapshot->root = current->fs->root ? fd_retain(current->fs->root) : NULL;
    snapshot->pwd = current->fs->pwd ? fd_retain(current->fs->pwd) : NULL;
    unlock(&current->fs->lock);
    if (!snapshot->root || !snapshot->pwd) {
        if (snapshot->root) fd_close(snapshot->root);
        if (snapshot->pwd) fd_close(snapshot->pwd);
        free(snapshot); free(context); return ERR_PTR(_ENOENT);
    }
    *context = (struct native_fs_context) {
        .owner = current, .thread = pthread_self(), .fs = snapshot,
    };
    return context;
}
void native_fs_context_destroy(struct native_fs_context *context) {
    if (!context || IS_ERR(context)) return;
    assert(context_owned(context));
    fs_info_release(context->fs);
    free(context);
}
struct fd *native_fs_open(struct native_fs_context *context, const char *path,
        int flags, int mode) {
    if (!context || IS_ERR(context) || !context_owned(context)) return ERR_PTR(_EPERM);
    if (!path) return ERR_PTR(_EINVAL);
    if (flags & O_CREAT_) mode &= ~context->fs->umask;
    return generic_open_in_fs(context->fs, path, flags, mode);
}
