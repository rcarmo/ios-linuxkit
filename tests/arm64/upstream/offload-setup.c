// Actual portable offload scaffold, with deterministic setup failure injection.
#include <assert.h>
#include <dirent.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>
#include <spawn.h>
#include <sys/wait.h>
#include "kernel/init.h"
#include "kernel/calls.h"
#include "kernel/native_offload.h"
#include "fs/real.h"
#include "fs/fd.h"
static jmp_buf exited;
static unsigned calls, sequence, failure_at, injections;
static bool inject, committing;
static int expected_status;
static struct fd *outputs[2];
static const char *kind;
static void *allocations[128];
static unsigned live_allocations;
static pthread_mutex_t allocation_lock = PTHREAD_MUTEX_INITIALIZER;
static void *track_allocation(void *p) {
    if (!p || !inject) return p;
    pthread_mutex_lock(&allocation_lock);
    unsigned i;
    for(i=0;i<128;i++) if(!allocations[i]) { allocations[i]=p; live_allocations++; break; }
    assert(i<128);
    pthread_mutex_unlock(&allocation_lock);
    return p;
}
void __real_free(void *);
void __wrap_free(void *p) {
    pthread_mutex_lock(&allocation_lock);
    for(unsigned i=0;i<128;i++) if(p && allocations[i]==p) {
        allocations[i]=NULL; live_allocations--; break;
    }
    pthread_mutex_unlock(&allocation_lock);
    __real_free(p);
}
static bool should_fail(const char *operation) {
    if (!inject || strcmp(kind, operation)) return false;
    sequence++;
    if (sequence != failure_at) return false;
    injections++; errno = !strcmp(operation, "thread") ? EAGAIN :
        !strcmp(operation, "pipe") ? EMFILE : ENOMEM;
    return true;
}
void *__real_malloc(size_t);
void *__wrap_malloc(size_t n) { return should_fail("alloc") ? NULL : track_allocation(__real_malloc(n)); }
void *__real_calloc(size_t, size_t);
void *__wrap_calloc(size_t n, size_t s) { return should_fail("alloc") ? NULL : track_allocation(__real_calloc(n,s)); }
char *__real_strdup(const char *);
char *__wrap_strdup(const char *s) { return should_fail("alloc") ? NULL : track_allocation(__real_strdup(s)); }
int __real_pipe(int *);
int __wrap_pipe(int *p) { return should_fail("pipe") ? -1 : __real_pipe(p); }
int __real_chdir(const char *);
int __wrap_chdir(const char *p) { return should_fail("chdir") ? -1 : __real_chdir(p); }
int __real_open(const char *,int,...);
int __wrap_open(const char *p,int f,...) {
    mode_t m=0;
    if(f & O_CREAT) { va_list ap; va_start(ap,f); m=va_arg(ap,int); va_end(ap); }
    return should_fail("open") ? -1 : __real_open(p,f,m);
}
int __real_fcntl(int, int, ...);
int __wrap_fcntl(int fd, int cmd, ...) {
    va_list ap; va_start(ap,cmd); int arg=va_arg(ap,int); va_end(ap);
    return should_fail("fcntl") ? -1 : __real_fcntl(fd,cmd,arg);
}
int __real_posix_spawn_file_actions_init(posix_spawn_file_actions_t *);
int __wrap_posix_spawn_file_actions_init(posix_spawn_file_actions_t *a) {
    return should_fail("spawn") ? ENOMEM : __real_posix_spawn_file_actions_init(a);
}
int __real_posix_spawnattr_init(posix_spawnattr_t *);
int __wrap_posix_spawnattr_init(posix_spawnattr_t *a) {
    return should_fail("spawn") ? ENOMEM : __real_posix_spawnattr_init(a);
}
int __real_posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *,int,int);
int __wrap_posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *a,int f,int t) {
    return should_fail("spawn") ? ENOMEM : __real_posix_spawn_file_actions_adddup2(a,f,t);
}
int __real_posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *,int);
int __wrap_posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *a,int f) {
    return should_fail("spawn") ? ENOMEM : __real_posix_spawn_file_actions_addclose(a,f);
}
int __real_posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *,int,const char *,int,mode_t);
int __wrap_posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *a,int f,const char *p,int o,mode_t m) {
    return should_fail("spawn") ? ENOMEM : __real_posix_spawn_file_actions_addopen(a,f,p,o,m);
}
int __real_posix_spawn_file_actions_addchdir_np(posix_spawn_file_actions_t *,const char *);
int __wrap_posix_spawn_file_actions_addchdir_np(posix_spawn_file_actions_t *a,const char *p) {
    return should_fail("spawn") ? ENOMEM : __real_posix_spawn_file_actions_addchdir_np(a,p);
}
int __real_posix_spawn(pid_t *,const char *,const posix_spawn_file_actions_t *,const posix_spawnattr_t *,char *const[],char *const[]);
int __wrap_posix_spawn(pid_t *p,const char *n,const posix_spawn_file_actions_t *a,const posix_spawnattr_t *b,char *const v[],char *const e[]) {
    return should_fail("spawn") ? ENOMEM : __real_posix_spawn(p,n,a,b,v,e);
}
static bool spawn_case;
int __real_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
int __wrap_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*f)(void *), void *p) {
    return should_fail("thread") ? EAGAIN : __real_pthread_create(t,a,f,p);
}
static ssize_t output_write(struct fd *fd, const void *buf, size_t n) {
    // Exercise successful short writes through both forwarders.
    if (n > 3) n = 3;
    return write(fd->real_fd, buf, n);
}
static int output_close(struct fd *fd) { return close(fd->real_fd); }
static const struct fd_ops output_ops = {.write=output_write, .close=output_close};
static int handler(int argc, char **argv, int in, int out, int err) {
    inject = false; committing = true; calls++;
    assert(current->did_exec && argc == 3 && argv[0] && argv[1] && argv[2]);
    (void)in;
    if (out >= 0) {
        assert(out >= 3 && (fcntl(out,F_GETFD,0) & FD_CLOEXEC));
        assert(write(out,"stdout\rprogress\n",16) == 16);
    }
    if (err >= 0) {
        assert(err >= 3 && (fcntl(err,F_GETFD,0) & FD_CLOEXEC));
        assert(write(err,"stderr\n",7) == 7);
    }
    return -1; // masking negative results must avoid undefined left shift.
}
noreturn void __wrap_do_exit(int status) {
    if(spawn_case) {
        inject=false; calls++;
        assert(!current->is_native_proxy && current->native_pid==0);
    } else assert(committing && calls == 1);
    assert(status == expected_status && live_allocations==0);
    for (unsigned i=0;i<2;i++) if (outputs[i]) assert(outputs[i]->refcount == 1);
    longjmp(exited,1);
}
static unsigned descriptors(void) {
    DIR *d=opendir("/proc/self/fd"); assert(d); unsigned n=0;
    struct dirent *e; while((e=readdir(d))) if(e->d_name[0]!='.') n++;
    closedir(d); return n;
}
int main(int argc, char **argv) {
    assert(argc==2);
    assert(mount_root(&realfs,"/")==0 && become_first_process()==0);
    assert(native_offload_add_handler(NULL,handler)==-1);
    assert(native_offload_add_handler("",handler)==-1);
    assert(native_offload_add_handler("probe",NULL)==-1);
    spawn_case = getenv("TEST_SPAWN") != NULL;
    kind="alloc"; inject=true; failure_at=1;
    assert(native_offload_add_handler("failed",handler)==-1);
    inject=false; assert(!native_offload_lookup("/bin/failed"));
    assert(native_offload_add_handler("probe",handler)==0);
    if(spawn_case) assert(native_offload_add("child=/bin/true")==0);
    const char *guest_file=spawn_case ? "/bin/child" : "/bin/probe";
    const char *native_file=spawn_case ? "/bin/true" : "[builtin]";
    for(unsigned i=0;i<2;i++) {
        char path[]="/tmp/offload-setup-XXXXXX";
        int f=mkstemp(path); assert(f>=0); unlink(path);
        outputs[i]=fd_create(&output_ops); assert(outputs[i]);
        outputs[i]->real_fd=f;
        lock(&current->files->lock);
        struct fd *old=current->files->files[i+1];
        current->files->files[i+1]=outputs[i];
        unlock(&current->files->lock); if(old)fd_close(old);
    }
    current->sighand->action[SIGUSR1_].handler=0x12345;
    current->altstack=0x44440000; current->altstack_size=8192;
    strcpy(current->comm,"unchanged");
    const char packed[]="/bin/probe\0relative\0/tmp/output\0";
    unsigned before=descriptors(), tests=0;
    char cwd[4096], after[4096]; assert(getcwd(cwd,sizeof(cwd)));
    bool lowfd=!strcmp(argv[1],"lowfd");
    if(lowfd) { close(STDIN_FILENO); before--; }
    if(strcmp(argv[1],"success") && strcmp(argv[1],"cloexec") && !lowfd) {
        kind=argv[1]; assert(!strcmp(kind,"alloc")||!strcmp(kind,"pipe")||!strcmp(kind,"thread")||!strcmp(kind,"fcntl")||!strcmp(kind,"spawn")||!strcmp(kind,"open")||!strcmp(kind,"chdir"));
        // Stop when the selected failure position no longer exists. That final
        // invocation is a genuine success and validates no stale setup state.
        for(failure_at=1;failure_at<32;failure_at++) {
            inject=true; sequence=0; injections=0;
            if(setjmp(exited)==0) {
                expected_status=spawn_case ? 0 : 255<<8;
                int ret=native_offload_exec(native_file,guest_file,3,packed,"");
                inject=false;
                assert(ret<0 && injections==1 && calls==0);
                assert(!current->did_exec && !strcmp(current->comm,"unchanged"));
                assert(current->altstack==0x44440000 && current->altstack_size==8192);
                assert(current->sighand->action[SIGUSR1_].handler==0x12345);
                assert(descriptors()==before && live_allocations==0);
                assert(getcwd(after,sizeof(after)) && !strcmp(cwd,after));
                for(unsigned i=0;i<2;i++)assert(outputs[i]->refcount==1);
                tests++; continue;
            }
            assert(injections==0 && descriptors()==before); break;
        }
        assert(failure_at<32 && tests>0);
    } else {
        if(!strcmp(argv[1],"cloexec")) {
            lock(&current->files->lock);
            for(unsigned i=0;i<2;i++) {
                bit_set(i+1,current->files->cloexec);
                // Test retains solely for assertions; exec closes table refs.
                fd_retain(outputs[i]);
            }
            unlock(&current->files->lock);
        }
        expected_status=spawn_case ? 0 : 255<<8;
        if(setjmp(exited)==0) {
            assert(native_offload_exec(native_file,guest_file,3,packed,"")==0);
            abort();
        }
        assert(descriptors()==before);
    }
    assert(current->did_exec && calls==1);
    assert(getcwd(after,sizeof(after)) && !strcmp(cwd,after));
    if(spawn_case) { int status; assert(waitpid(-1,&status,WNOHANG)==-1 && errno==ECHILD); }
    if(!spawn_case) {
        const char *expected[2]={"stdout\nprogress\n","stderr\n"};
        for(unsigned i=0;i<2;i++) {
            char data[32]={0};
            assert(lseek(outputs[i]->real_fd,0,SEEK_SET)==0);
            ssize_t n=read(outputs[i]->real_fd,data,sizeof(data));
            assert(n>=0);
            if(!strcmp(argv[1],"cloexec")) assert(n==0);
            else assert((size_t)n==strlen(expected[i]) && !memcmp(data,expected[i],n));
        }
    }
    for(unsigned i=0;i<2;i++) {
        bool closed=!strcmp(argv[1],"cloexec");
        assert(current->files->files[i+1] == (closed ? NULL : outputs[i]));
        if(!closed) current->files->files[i+1]=NULL;
        fd_close(outputs[i]);
    }
    printf("offload-setup-ok mode=%s failures=%u\n",argv[1],tests);
}
