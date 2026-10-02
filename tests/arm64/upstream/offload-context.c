// Actual cooperative dispatcher: no host CWD/path translation/scans/workers.
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "kernel/init.h"
#include "kernel/calls.h"
#include "kernel/native_offload.h"
#include "fs/real.h"
#define ISH_INTERNAL
#include "fs/fake.h"
#undef ISH_INTERNAL
#include "fs/path.h"
static __thread jmp_buf exited;
static __thread unsigned allocation, fail_at, handler_calls;
static __thread bool injecting;
static __thread const char *directory;
static __thread const char *value;
static struct task *owner;
static pthread_barrier_t barrier;
static bool concurrent, refuse_thread;
int __real_pthread_create(pthread_t *,const pthread_attr_t *,void *(*)(void *),void *);
int __wrap_pthread_create(pthread_t *t,const pthread_attr_t *a,void *(*f)(void *),void *p) {
    return refuse_thread ? EAGAIN : __real_pthread_create(t,a,f,p);
}
static struct native_handler_context *contexts[2];
static __thread void *tracked[32];
static __thread unsigned live_allocations;
static void *track(void *p) {
    if (!p || !injecting) return p;
    for (unsigned i=0;i<32;i++) if (!tracked[i]) {
        tracked[i]=p; live_allocations++; return p;
    }
    abort();
}
void __real_free(void *);
void __wrap_free(void *p) {
    for (unsigned i=0;i<32;i++) if (p && tracked[i]==p) {
        tracked[i]=NULL; live_allocations--; break;
    }
    __real_free(p);
}
void *__real_malloc(size_t);
void *__real_calloc(size_t,size_t);
char *__real_strdup(const char *);
static bool fail(void) { return injecting && ++allocation == fail_at; }
void *__wrap_malloc(size_t n) { return fail() ? NULL : track(__real_malloc(n)); }
void *__wrap_calloc(size_t n,size_t s) { return fail() ? NULL : track(__real_calloc(n,s)); }
char *__wrap_strdup(const char *p) { return fail() ? NULL : track(__real_strdup(p)); }
int __wrap_chdir(const char *p) { (void)p; abort(); }
int __wrap_fchdir(int fd) { (void)fd; abort(); }
DIR *__wrap_opendir(const char *p) { (void)p; abort(); }
int __wrap_pipe(int *p) { (void)p; abort(); }
static void read_value(struct native_fs_context *fs,const char *path,const char *want) {
    struct fd *fd=native_fs_open(fs,path,O_RDONLY_,0); assert(!IS_ERR(fd));
    char data[64]={0}; ssize_t n=fd->ops->read(fd,data,sizeof(data));
    assert(n==(ssize_t)strlen(want) && !memcmp(data,want,n)); fd_close(fd);
}
static int legacy(int argc,char **argv,int in,int out,int err) {
    (void)argc; (void)argv; (void)in; (void)out; (void)err; abort();
}
static int handler(int argc,char **argv,struct native_handler_context *context) {
    injecting=false; handler_calls++;
    assert(current->did_exec && argc==4);
    assert(!strcmp(argv[0],"/bin/probe") && !strcmp(argv[1],"item"));
    assert(!strcmp(argv[2],"/mounted/value"));
    assert(!strcmp(argv[3],"https://example.invalid/untouched"));
    assert(current->sighand->action[SIGUSR1_].handler==SIG_DFL_);
    assert(!current->altstack && !current->altstack_size);
    struct native_fs_context *fs=native_handler_fs(context); assert(fs);
    // Later changes in the task's CWD cannot change the retained context.
    struct fd *cwd=generic_open("/",O_RDONLY_|O_DIRECTORY_,0); assert(!IS_ERR(cwd));
    fs_chdir(current->fs,cwd);
    if(concurrent) {
        unsigned index=!strcmp(directory,"/a") ? 0 : 1;
        contexts[index]=context;
        pthread_barrier_wait(&barrier);
        assert(native_handler_fs(contexts[1-index])==NULL);
    }
    read_value(fs,argv[1],value); read_value(fs,argv[2],"mounted");
    read_value(fs,"link","beta");
    struct fd *f=native_fs_open(fs,"link",O_RDONLY_|O_NOFOLLOW_,0);
    assert(IS_ERR(f) && PTR_ERR(f)==_ELOOP);
    f=native_fs_open(fs,"out",O_CREAT_|O_WRONLY_|O_TRUNC_,0666); assert(!IS_ERR(f));
    assert(f->ops->write(f,value,strlen(value))==(ssize_t)strlen(value)); fd_close(f);
    char path[64]; snprintf(path,sizeof(path),"%s/out",directory);
    struct statbuf stat; assert(generic_statat(AT_PWD,path,&stat,true)==0);
    assert((stat.mode & 0777)==0640 && stat.size==strlen(value));
    read_value(fs,"out",value);
    if(concurrent) pthread_barrier_wait(&barrier);
    return -1;
}
noreturn void __wrap_do_exit(int status) {
    assert(status==(255<<8) && handler_calls==1 && live_allocations==0); longjmp(exited,1);
}
static const char packed[]="/bin/probe\0item\0/mounted/value\0https://example.invalid/untouched\0";
static void set_directory(const char *dir) {
    struct fd *cwd=generic_open(dir,O_RDONLY_|O_DIRECTORY_,0); assert(!IS_ERR(cwd));
    fs_chdir(current->fs,cwd); current->fs->umask=0027;
    directory=dir; value=!strcmp(dir,"/a") ? "alpha" : "beta";
    current->did_exec=false; strcpy(current->comm,"unchanged");
    current->sighand->action[SIGUSR1_].handler=0x12345;
    current->altstack=0x44440000; current->altstack_size=8192;
}
static void *worker(void *argument) {
    struct task local=*owner;
    struct tgroup group={0}; lock_init(&group.lock); list_init(&group.threads);
    local.group=&group; group.leader=&local; local.vfork=NULL;
    list_add(&group.threads,&local.group_links); lock_init(&local.general_lock);
    local.fs=fs_info_copy(owner->fs); local.files=fdtable_copy(owner->files);
    local.sighand=sighand_copy(owner->sighand); current=&local;
    set_directory(argument);
    if(setjmp(exited)==0) { native_offload_exec("[builtin]","/bin/probe",4,packed,""); abort(); }
    assert(current->fs->root->refcount>=1);
    fs_info_release(local.fs); fdtable_release(local.files); sighand_release(local.sighand);
    pthread_mutex_destroy(&local.general_lock.m); pthread_mutex_destroy(&group.lock.m);
    return NULL;
}
static unsigned host_fds(void) {
    // opendir is deliberately forbidden, including legacy metadata scans.
    DIR *d=fdopendir(open("/proc/self/fd",O_RDONLY|O_DIRECTORY)); assert(d);
    unsigned n=0; struct dirent *e; while((e=readdir(d))) if(e->d_name[0]!='.') n++;
    closedir(d); return n;
}
int main(int argc,char **argv) {
    assert(argc==4);
    assert(native_offload_add_cooperative_handler(NULL,handler)==-1);
    assert(native_offload_add_cooperative_handler("",handler)==-1);
    assert(native_offload_add_cooperative_handler("probe",NULL)==-1);
    injecting=true; fail_at=1;
    assert(native_offload_add_cooperative_handler("failed",handler)==-1);
    injecting=false; assert(!native_offload_lookup("/bin/failed"));
    assert(native_offload_add_cooperative_handler("probe",handler)==0);
    const struct fs_ops *fs=!strcmp(argv[1],"fake") ? &fakefs : &realfs;
    assert(mount_root(fs,argv[2])==0 && become_first_process()==0); owner=current;
    // Even a failed first launch closes the registration window before a
    // pthread can execute. Subsequent retries cannot reopen it.
    refuse_thread=true; assert(task_start(current)==_EAGAIN); refuse_thread=false;
    assert(native_offload_add_cooperative_handler("late",handler)==-1);
    assert(native_offload_add_handler("late",legacy)==-1);
    assert(native_offload_add("child=/bin/true")==-1);
    char mounted[4096]; assert(snprintf(mounted,sizeof(mounted),"%s",argv[3])<(int)sizeof(mounted));
    assert(do_mount(&realfs,mounted,"/mounted","",0)==0);
    set_directory("/a");
    char before[4096],after[4096]; assert(getcwd(before,sizeof(before)));
    // Unsupported sibling/group-exit combinations return before commitment.
    struct task sibling={0}; lock(&pids_lock); list_add(&current->group->threads,&sibling.group_links); unlock(&pids_lock);
    assert(native_offload_exec("[builtin]","/bin/probe",4,packed,"")==_EBUSY);
    lock(&pids_lock); list_remove(&sibling.group_links); unlock(&pids_lock);
    current->group->doing_group_exit=true;
    assert(native_offload_exec("[builtin]","/bin/probe",4,packed,"")==_EBUSY);
    current->group->doing_group_exit=false;
    assert(!current->did_exec && !handler_calls);
    struct fd *saved=current->fs->pwd; current->fs->pwd=NULL;
    assert(native_offload_exec("[builtin]","/bin/probe",4,packed,"")==_ENOENT);
    current->fs->pwd=saved;
    assert(!current->did_exec && !handler_calls);
    unsigned root_refs=current->fs->root->refcount, pwd_refs=current->fs->pwd->refcount;
    unsigned descriptors=host_fds(), failures=0;
    for(fail_at=1;fail_at<16;fail_at++) {
        allocation=0; injecting=true;
        if(setjmp(exited)==0) {
            int err=native_offload_exec("[builtin]","/bin/probe",4,packed,""); injecting=false;
            assert(err==_ENOMEM && !handler_calls && !current->did_exec && live_allocations==0);
            assert(!strcmp(current->comm,"unchanged"));
            assert(current->sighand->action[SIGUSR1_].handler==0x12345);
            assert(current->altstack==0x44440000 && current->altstack_size==8192);
            assert(current->fs->root->refcount==root_refs && current->fs->pwd->refcount==pwd_refs);
            assert(host_fds()==descriptors); failures++; continue;
        }
        break;
    }
    assert(fail_at<16 && failures==7 && host_fds()==descriptors);
    // Real parallel dispatcher calls with independent guest tasks and CWDs.
    concurrent=true; assert(pthread_barrier_init(&barrier,NULL,2)==0);
    pthread_t threads[2];
    assert(pthread_create(&threads[0],NULL,worker,"/a")==0);
    assert(pthread_create(&threads[1],NULL,worker,"/b")==0);
    assert(pthread_join(threads[0],NULL)==0 && pthread_join(threads[1],NULL)==0);
    pthread_barrier_destroy(&barrier);
    assert(getcwd(after,sizeof(after)) && !strcmp(before,after));
    assert(host_fds()==descriptors);
    printf("offload-context-ok backend=%s failures=%u raw-argv/VFS/mount/metadata/concurrent-exec/no-chdir/no-scan/no-pipe\n",argv[1],failures);
}
