// Host-linked deterministic ownership probes against the actual kernel archives.
// Run with tests/regress/run_socket_ownership_0912.sh, adding
// -Wl,--wrap=f_install_owned -Wl,--wrap=malloc -Wl,--wrap=calloc
// to its link flags; no guest workload required.
#include <assert.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#include "kernel/init.h"
#include "kernel/calls.h"
#include "kernel/resource.h"
#include "fs/fd.h"
#include "fs/real.h"
#include "fs/sock.h"
#include "fs/netlink.h"

static bool tracking;
static int raw[2], closes[2], raw_count, fail_alloc, reuse_install, installs;
static struct fd *replacement;
static fd_t replacement_num;
static struct fd *second_replacement;
static fd_t second_replacement_num;
static struct fd *original[2];
static int accepted = -1;
static int fail_table_alloc;
static const addr_t output = 0x10000;

void *__real_malloc(size_t);
void *__wrap_malloc(size_t size) {
    if (fail_table_alloc && --fail_table_alloc == 0) return NULL;
    return __real_malloc(size);
}
void *__real_calloc(size_t, size_t);
void *__wrap_calloc(size_t count, size_t size) {
    if (fail_table_alloc && --fail_table_alloc == 0) return NULL;
    return __real_calloc(count, size);
}

int __real_close(int);
int __wrap_close(int f) {
    if (tracking)
        for (int i = 0; i < raw_count; i++)
            if (f == raw[i]) assert(++closes[i] == 1);
    return __real_close(f);
}
static void record(int f) {
    if (tracking) { assert(raw_count < 2); raw[raw_count++] = f; }
}
int __real_socket(int, int, int);
int __wrap_socket(int d, int t, int p) {
    int f = __real_socket(d, t, p); if (f >= 0) record(f); return f;
}
int __real_socketpair(int, int, int, int *);
int __wrap_socketpair(int d, int t, int p, int *sv) {
    int r = __real_socketpair(d, t, p, sv);
    if (!r) { record(sv[0]); record(sv[1]); } return r;
}
int __real_pipe(int *);
int __wrap_pipe(int *p) {
    int r = __real_pipe(p); if (!r) { record(p[0]); record(p[1]); } return r;
}
int __wrap_accept(int f, struct sockaddr *addr, socklen_t *len) {
    (void)f;
    assert(accepted >= 0);
    int result = accepted; accepted = -1; record(result);
    if (addr) {
        struct sockaddr_in in = {.sin_family = AF_INET};
        assert(*len >= sizeof(in)); memcpy(addr, &in, sizeof(in)); *len = sizeof(in);
    }
    return result;
}
struct fd *__real_adhoc_fd_create(const struct fd_ops *);
struct fd *__wrap_adhoc_fd_create(const struct fd_ops *ops) {
    if (fail_alloc && --fail_alloc == 0) return NULL;
    return __real_adhoc_fd_create(ops);
}
fd_t __real_f_install(struct fd *, int);
fd_t __wrap_f_install(struct fd *fd, int flags) {
    return __real_f_install(fd, flags);
}
fd_t __real_f_install_owned(struct fd *, int, struct fd_install_token *);
fd_t __wrap_f_install_owned(struct fd *fd, int flags, struct fd_install_token *token) {
    if (!reuse_install) return __real_f_install_owned(fd, flags, token);
    assert(installs < 2 && atomic_load(&fd->refcount) >= 2);
    original[installs] = fd;
    struct fdtable *table = current->files;
    unsigned refs = atomic_load(&table->refcount);
    fd_t f = __real_f_install_owned(fd, flags, token);
    installs++;
    if (f < 0) {
        assert(installs == 2 && f == _EMFILE && !token->table);
        return f;
    }
    assert(token->table == table && token->slot == f && token->expected == fd);
    assert(token->generation && token->generation == table->generation[f]);
    assert(atomic_load(&table->refcount) == refs + 1);
    if (installs == 1 && reuse_install == 4) {
        struct fdtable *copy = fdtable_copy(table); assert(!IS_ERR(copy));
        current->files = copy;
        fdtable_release(table); // Only the captured token now owns the old table.
        assert(atomic_load(&table->refcount) == 1);
        return f;
    }
    if (installs == 1 || reuse_install == 2) {
        assert(f_close(f) == 0);
        // The constructor still owns both ORIGINAL ends at this checkpoint.
        for (int i = 0; i < raw_count; i++) assert(closes[i] == 0);
        struct fd *r;
        if (reuse_install == 3) {
            r = fd_retain(fd); // SAME object, different publication and CLOEXEC.
        } else {
            r = __real_adhoc_fd_create(&realfs_fdops); assert(r);
            r->real_fd = open("/dev/null", O_RDONLY); assert(r->real_fd >= 0);
        }
        fd_t number = __real_f_install(r, O_CLOEXEC_); assert(number == f);
        assert(table->generation[f] > token->generation);
        if (installs == 1) { replacement = r; replacement_num = number; }
        else { second_replacement = r; second_replacement_num = number; }
    }
    return f;
}
static void limit(unsigned slots) { current->group->limits[RLIMIT_NOFILE_].cur = slots; }
static void begin(unsigned slots, int fail) {
    limit(slots); raw_count = 0; closes[0] = closes[1] = 0;
    fail_alloc = fail; tracking = true; installs = 0;
}
static void end(void) {
    for (int i = 0; i < raw_count; i++) assert(closes[i] == 1);
    tracking = false; fail_alloc = 0;
}
static void pair_errors(void) {
    for (unsigned slots = 0; slots <= 1; slots++) {
        begin(slots, 0); assert(sys_socketpair(AF_LOCAL_, SOCK_STREAM_, 0, output) == _EMFILE); end();
        begin(slots, 0); assert(sys_pipe2(output, 0) == _EMFILE); end();
    }
    for (int allocation = 1; allocation <= 2; allocation++) {
        begin(8, allocation); assert(sys_socketpair(AF_LOCAL_, SOCK_STREAM_, 0, output) == _ENOMEM); end();
        begin(8, allocation); assert(sys_pipe2(output, 0) == _ENOMEM); end();
    }
    begin(8, 0); assert(sys_socketpair(AF_LOCAL_, SOCK_STREAM_, 0, 1) == _EFAULT); end();
    begin(8, 0); assert(sys_pipe2(1, 0) == _EFAULT); end();
    begin(0, 0); assert(sys_socket(AF_LOCAL_, SOCK_STREAM_, 0) == _EMFILE); end();
    begin(8, 1); assert(sys_socket(AF_LOCAL_, SOCK_STREAM_, 0) == _ENOMEM); end();
    assert(!setenv("ISH_NETLINK_STUB", "1", 1));
    begin(0, 0); assert(sys_socket(AF_NETLINK_, SOCK_DGRAM_, 0) == _EMFILE); end();
    begin(8, 1); assert(sys_socket(AF_NETLINK_, SOCK_DGRAM_, 0) == _ENOMEM); end();
}
static void pair_reuse(bool fault) {
    begin(8, 0); reuse_install = 1;
    assert(sys_socketpair(AF_LOCAL_, SOCK_STREAM_, 0, fault ? 1 : output) == (fault ? _EFAULT : 0));
    reuse_install = 0;
    assert(f_get(replacement_num) == replacement && replacement->socket.unix_peer == NULL);
    if (!fault) {
        int sv[2]; assert(!user_get(output, sv));
        struct fd *survivor = f_get(sv[1]); assert(survivor == original[1]);
        assert(survivor->socket.unix_peer == NULL); // Original first end was closed, not replacement.
        assert(f_close(sv[1]) == 0);
    }
    end(); assert(f_close(replacement_num) == 0);
}
static void constructor_rollback(bool pipe_case, bool second_failure, bool replace_both) {
    begin(second_failure ? 1 : 8, 0); reuse_install = replace_both ? 2 : 1;
    int result = pipe_case ? sys_pipe2(1, O_NONBLOCK_) :
        sys_socketpair(AF_LOCAL_, SOCK_STREAM_, 0, 1);
    reuse_install = 0;
    assert(result == (second_failure ? _EMFILE : _EFAULT) && installs == 2 && raw_count == 2);
    end();
    for (int i = 0; i < 2; i++) assert(fcntl(raw[i], F_GETFD) == -1 && errno == EBADF);
    assert(f_get(replacement_num) == replacement && atomic_load(&replacement->refcount) == 1);
    assert(bit_test(replacement_num, current->files->cloexec));
    assert(fcntl(replacement->real_fd, F_GETFD) >= 0 && !(fd_getflags(replacement) & O_NONBLOCK_));
    if (replace_both) {
        assert(f_get(second_replacement_num) == second_replacement);
        assert(atomic_load(&second_replacement->refcount) == 1);
        assert(bit_test(second_replacement_num, current->files->cloexec));
        assert(fcntl(second_replacement->real_fd, F_GETFD) >= 0);
        assert(!f_close(second_replacement_num));
    }
    assert(!f_close(replacement_num));
}
static void same_object_rollback(bool pipe_case, bool second_failure) {
    begin(second_failure ? 1 : 8, 0); reuse_install = 3;
    assert((pipe_case ? sys_pipe2(1, 0) : sys_socketpair(AF_LOCAL_, SOCK_STREAM_, 0, 1)) ==
        (second_failure ? _EMFILE : _EFAULT));
    reuse_install = 0;
    assert(installs == 2 && raw_count == 2 && closes[0] == 0 && closes[1] == 1);
    assert(f_get(replacement_num) == original[0] && replacement == original[0]);
    assert(atomic_load(&replacement->refcount) == 1);
    assert(bit_test(replacement_num, current->files->cloexec));
    assert(atomic_load(&current->files->refcount) == 1);
    assert(!f_close(replacement_num)); end();
}
static void changed_table_rollback(bool pipe_case) {
    begin(8, 0); reuse_install = 4;
    assert((pipe_case ? sys_pipe2(1, 0) : sys_socketpair(AF_LOCAL_, SOCK_STREAM_, 0, 1)) == _EFAULT);
    reuse_install = 0;
    // The old-table publication was rolled back, not the copied publication.
    assert(f_get(0) == original[0] && atomic_load(&original[0]->refcount) == 1);
    assert(!f_get(1) && closes[0] == 0 && closes[1] == 1);
    assert(atomic_load(&current->files->refcount) == 1);
    assert(!f_close(0)); end();
}
static void token_and_dup(void) {
    limit(128);
    struct fd *fd = __real_adhoc_fd_create(&realfs_fdops); assert(fd);
    fd->real_fd = open("/dev/null", O_RDONLY); assert(fd->real_fd >= 0);
    struct fd_install_token token = {};
    assert(__real_f_install_owned(fd_retain(fd), O_CLOEXEC_, &token) == 0);
    struct fdtable *table = token.table;
    uint64_t generation = token.generation;
    assert(sys_dup2(0, 0) == 0 && table->generation[0] == generation);
    assert(bit_test(0, table->cloexec));
    assert((int_t) sys_dup3(0, 0, 0) == _EINVAL);
    assert((int_t) sys_dup3(0, -1, 0) == _EBADF);
    assert((int_t) sys_dup3(0, 1, O_NONBLOCK_) == _EINVAL);
    assert((int_t) sys_dup2(-1, -1) == _EBADF);
    assert((int_t) sys_dup2(0, 128) == _EBADF);
    unsigned size = table->size;
    struct fd **files = table->files;
    bits_t *cloexec = table->cloexec;
    uint64_t *generations = table->generation;
    // All three resize allocations and all four copy allocations fail cleanly.
    for (int allocation = 1; allocation <= 3; allocation++) {
        fail_table_alloc = allocation;
        assert((int_t) sys_dup3(0, 80, O_CLOEXEC_) == _ENOMEM);
        assert(!fail_table_alloc && table->size == size && table->files == files);
        assert(table->cloexec == cloexec && table->generation == generations);
        assert(f_get(0) == fd && table->generation[0] == generation);
        assert(bit_test(0, table->cloexec) && atomic_load(&fd->refcount) == 2);
    }
    for (int allocation = 1; allocation <= 4; allocation++) {
        fail_table_alloc = allocation;
        struct fdtable *failed = fdtable_copy(table);
        assert(IS_ERR(failed) && PTR_ERR(failed) == _ENOMEM && !fail_table_alloc);
        assert(table->files == files && table->cloexec == cloexec && table->generation == generations);
        assert(atomic_load(&fd->refcount) == 2 && atomic_load(&table->refcount) == 2);
    }
    limit(0);
    assert(sys_dup2(0, 0) == 0 && bit_test(0, table->cloexec));
    limit(128);
    assert(sys_dup3(0, 80, O_CLOEXEC_) == 80); // Resize preserves token identity.
    assert(table->generation[0] == generation && table->generation[80]);
    struct fdtable *copy = fdtable_copy(table); assert(!IS_ERR(copy));
    assert(copy->generation[0] == generation && copy->generation[80] == table->generation[80]);
    fdtable_release(copy);
    assert(sys_dup2(80, 0) == 0 && table->generation[0] > generation);
    assert(!bit_test(0, table->cloexec));
    assert(sys_dup3(80, 0, O_CLOEXEC_) == 0);
    assert(!f_uninstall(&token) && !token.table && f_get(0) == fd);
    assert(bit_test(0, table->cloexec));
    assert(sys_fcntl(0, 1030, 81) == 81 && table->generation[81]);
    assert(bit_test(81, table->cloexec));
    assert((int_t) sys_fcntl(0, 1030, (addr_t)-1) == _EINVAL);
    assert((int_t) sys_fcntl(0, 1030, 128) == _EINVAL);
    assert(sys_dup(0) == 1 && table->generation[1]);
    assert(!f_close(1) && !f_close(80) && !f_close(81) && !f_close(0));
    assert(__real_f_install_owned(fd_retain(fd), 0, &token) == 0);
    assert(f_uninstall(&token) && !token.table && !f_get(0));
    f_install_release(&token); // Safe after rollback and on an empty token.
    assert(atomic_load(&table->refcount) == 1 && atomic_load(&fd->refcount) == 1);
    table->generation[0] = UINT64_MAX;
    assert(__real_f_install_owned(fd_retain(fd), 0, &token) == _EOVERFLOW);
    assert(!token.table && !f_get(0) && atomic_load(&fd->refcount) == 1);
    table->generation[0] = generation + 10; // Restore this synthetic exhaustion fixture.
    assert(!fd_close(fd));
}
static void normal_pairs(void) {
    begin(8, 0);
    assert(!sys_socketpair(AF_LOCAL_, SOCK_STREAM_ | SOCK_NONBLOCK_ | SOCK_CLOEXEC_, 0, output));
    int sv[2]; assert(!user_get(output, sv));
    struct fd *a = f_get(sv[0]), *b = f_get(sv[1]); assert(a && b);
    assert(a->socket.unix_peer == b && b->socket.unix_peer == a);
    for (int i = 0; i < 2; i++) {
        assert(fd_getflags(f_get(sv[i])) & O_NONBLOCK_);
        assert(bit_test(sv[i], current->files->cloexec));
    }
    char byte;
    assert(write(a->real_fd, "x", 1) == 1 && read(b->real_fd, &byte, 1) == 1 && byte == 'x');
    assert(!f_close(sv[0]) && b->socket.unix_peer == NULL && !f_close(sv[1])); end();
    begin(8, 0);
    assert(!sys_pipe2(output, O_NONBLOCK_ | O_CLOEXEC_)); assert(!user_get(output, sv));
    a = f_get(sv[0]); b = f_get(sv[1]); assert(a && b);
    assert(fd_getflags(a) & O_NONBLOCK_); assert(fd_getflags(b) & O_NONBLOCK_);
    assert(bit_test(sv[0], current->files->cloexec) && bit_test(sv[1], current->files->cloexec));
    assert(write(b->real_fd, "p", 1) == 1 && read(a->real_fd, &byte, 1) == 1 && byte == 'p');
    assert(!f_close(sv[0]) && !f_close(sv[1])); end();
}
static void accept_errors(void) {
    limit(8);
    fd_t listener = sys_socket(AF_LOCAL_, SOCK_STREAM_, 0); assert(listener >= 0);
    struct fd *listen_fd = f_get(listener);
    int sv[2];
    for (int mode = 0; mode < 3; mode++) {
        assert(!__real_socketpair(AF_UNIX, SOCK_STREAM, 0, sv)); accepted = sv[0];
        begin(mode == 0 ? 1 : 8, mode == 1 ? 1 : 0);
        dword_t len = sizeof(struct sockaddr_in); assert(!user_put(output, len));
        // EFAULT before construction must close the still-unowned accepted host fd.
        assert(sys_accept(listener, mode == 2 ? 1 : 0, output) ==
            (mode == 0 ? _EMFILE : mode == 1 ? _ENOMEM : _EFAULT));
        end(); assert(!__real_close(sv[1]));
    }
    // A local accept uses its original object, never peer_lock -> fd-table lookup.
    assert(!__real_socketpair(AF_UNIX, SOCK_STREAM, 0, sv)); accepted = sv[0];
    struct fd *peer = __real_adhoc_fd_create(&realfs_fdops); assert(peer);
    peer->real_fd = sv[1]; cond_init(&peer->socket.unix_got_peer);
    assert(write(sv[1], &peer, sizeof(peer)) == sizeof(peer));
    begin(8, 0); reuse_install = 1;
    fd_t result = sys_accept(listener, 0, 0); assert(result >= 0);
    reuse_install = 0;
    assert(f_get(result) == replacement && peer->socket.unix_peer == NULL);
    assert(listen_fd == f_get(listener)); end();
    assert(!f_close(result)); assert(!fd_close(peer)); assert(!f_close(listener));
}
int main(void) {
    alarm(15);
    assert(!mount_root(&realfs, "/") && !become_first_process());
    write_wrlock(&current->mem->lock);
    assert(!pt_map_nothing(current->mem, output >> PAGE_BITS, 1, P_READ | P_WRITE));
    write_wrunlock(&current->mem->lock);
    pair_errors(); pair_reuse(false); pair_reuse(true);
    constructor_rollback(true, false, false); constructor_rollback(true, false, true);
    constructor_rollback(true, true, false); constructor_rollback(false, true, false);
    same_object_rollback(true, false); same_object_rollback(true, true);
    same_object_rollback(false, false); same_object_rollback(false, true);
    changed_table_rollback(true); changed_table_rollback(false);
    token_and_dup(); normal_pairs(); accept_errors();
    for (unsigned f = 0; f < current->files->size; f++) assert(!f_get(f));
    puts("socket-pipe-ownership-ok: allocation/EMFILE/EFAULT/netlink/publication-reuse/pipe-EFAULT-both-reused/second-install-failure/same-object-rebind/captured-table/dup-generation/resize-copy-allocation-failure/exhaustion/accept/flags/IO");
}
