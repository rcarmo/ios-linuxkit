// Real VFS + actual cooperative executor and test-only local handler.
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
#include "kernel/native_offload_internal.h"
#include "fs/real.h"
#include "fs/path.h"
#include "fs/inode.h"
#define ISH_INTERNAL
#include "fs/fake.h"
#undef ISH_INTERNAL
#include "handlers/local-copy.h"
static const char *mode;
static bool active, altered;
static unsigned opens, closes, reads, writes, calls, checkpoints;
static int expected;
static struct task *owner;
static struct fd *input, *output;
static const struct fd_ops *original_input, *original_output;
static struct fd_ops input_ops, output_ops;
static jmp_buf exited;
static pthread_barrier_t barrier;
static pthread_t sender;
static bool sender_started;
static const char *destination;
static bool allocating;
static unsigned injected_allocations, input_stats;
static const struct fs_ops *original_fs;
static struct fs_ops injected_fs;
static struct mount *changed_mount;
void *__real_malloc(size_t);
void *__real_calloc(size_t,size_t);
static bool allocation_fails(size_t size) {
    bool fail_fd = !strcmp(mode,"alloc-input") || !strcmp(mode,"alloc-output") || !strcmp(mode,"bind-alloc");
    bool fail_inode = !strcmp(mode,"inode-input") || !strcmp(mode,"inode-output");
    if (allocating && ((fail_fd && size==sizeof(struct fd)) ||
            (fail_inode && size==sizeof(struct inode_data)))) {
        injected_allocations++; return true;
    }
    return false;
}
void *__wrap_malloc(size_t size) { return allocation_fails(size) ? NULL : __real_malloc(size); }
void *__wrap_calloc(size_t n,size_t size) { return allocation_fails(n*size) ? NULL : __real_calloc(n,size); }
static int injected_fstat(struct fd *fd,struct statbuf *stat) {
    if(fd==input) {
        input_stats++;
        if(!strcmp(mode,"stat-input") || (!strcmp(mode,"stat-final") && input_stats==2)) return _EIO;
    }
    if(fd==output && !strcmp(mode,"stat-output")) return _EIO;
    return original_fs->fstat(fd,stat);
}
int __wrap_chdir(const char *p) { (void)p; abort(); }
int __wrap_fchdir(int fd) { (void)fd; abort(); }
int __wrap_pipe(int *fds) { (void)fds; abort(); }
DIR *__wrap_opendir(const char *p) { (void)p; abort(); }
int __real_unlink(const char *);
int __real_unlinkat(int,const char *,int);
int __wrap_unlink(const char *p) { assert(!active); return __real_unlink(p); }
int __wrap_unlinkat(int fd,const char *p,int flags) { assert(!active); return __real_unlinkat(fd,p,flags); }
int __real_pthread_create(pthread_t *,const pthread_attr_t *,void *(*)(void *),void *);
int __wrap_pthread_create(pthread_t *t,const pthread_attr_t *a,void *(*f)(void *),void *p) {
    assert(!active); return __real_pthread_create(t,a,f,p);
}
static void *cancel_sender(void *unused) {
    (void)unused; pthread_barrier_wait(&barrier);
    send_signal(owner,SIGTERM_,SIGINFO_NIL);
    pthread_barrier_wait(&barrier); return NULL;
}
static void cancel(void) {
    pthread_barrier_wait(&barrier); pthread_barrier_wait(&barrier);
}
static void resize_input(bool grow) {
    struct fd *f=generic_open("input",O_WRONLY_,0); assert(!IS_ERR(f));
    assert(f->mount->fs->fsetattr(f,make_attr(size,grow ? 20000 : 1))==0); fd_close(f);
}
static ssize_t input_read(struct fd *f,void *buf,size_t n) {
    reads++; assert(f==input && n<=NATIVE_IO_CHUNK);
    if(reads==1 && !strcmp(mode,"cancel-read")) cancel();
    if(!strcmp(mode,"read-error")) return _EIO;
    if(!strcmp(mode,"read-eintr")) return _EINTR;
    if(!strcmp(mode,"read-again")) return _EAGAIN;
    if(!strcmp(mode,"read-zero")) return 0;
    if(!strcmp(mode,"read-over")) return n+1;
    if(!strcmp(mode,"short") && n>193) n=193;
    if(!strcmp(mode,"ops-limit")) n=1;
    ssize_t got=original_input->read(f,buf,n);
    if(reads==1 && (!strcmp(mode,"growth") || !strcmp(mode,"shrink"))) {
        resize_input(!strcmp(mode,"growth")); altered=true;
    }
    return got;
}
static ssize_t output_write(struct fd *f,const void *buf,size_t n) {
    writes++; assert(f==output && n<=NATIVE_IO_CHUNK);
    if(writes==1 && !strcmp(mode,"cancel-write")) cancel();
    if(writes==1 && !strcmp(mode,"replace")) {
        assert(generic_renameat(AT_PWD,destination,AT_PWD,"saved-partial")==0);
        struct fd *replacement=generic_open(destination,O_CREAT_|O_EXCL_|O_WRONLY_,0600);
        assert(!IS_ERR(replacement) && replacement->ops->write(replacement,"replacement",11)==11);
        fd_close(replacement); altered=true;
        send_signal(owner,SIGTERM_,SIGINFO_NIL);
    }
    if(!strcmp(mode,"write-error")) return _ENOSPC;
    if(!strcmp(mode,"write-eintr")) return _EINTR;
    if(!strcmp(mode,"write-again")) return _EAGAIN;
    if(writes==2 && !strcmp(mode,"write-partial")) return _ENOSPC;
    if(!strcmp(mode,"write-zero")) return 0;
    if(!strcmp(mode,"write-over")) return n+1;
    if(!strcmp(mode,"short") && n>61) n=61;
    return original_output->write(f,buf,n);
}
struct fd *__real_native_fs_open(struct native_fs_context *,const char *,int,int);
struct fd *__wrap_native_fs_open(struct native_fs_context *c,const char *p,int flags,int permissions) {
    if(!active) return __real_native_fs_open(c,p,flags,permissions);
    opens++;
    if(opens==1) {
        assert(!(flags&O_CREAT_) && (flags&O_NOFOLLOW_) && (flags&O_NONBLOCK_));
        if(!strcmp(mode,"open-input")) return ERR_PTR(_EACCES);
    } else {
        assert(opens==2 && (flags&O_EXCL_) && (flags&O_NOFOLLOW_) && !(flags&O_TRUNC_) && permissions==0600);
        if(!strcmp(mode,"open-output")) return ERR_PTR(_ENOSPC);
    }
    allocating = (opens==1 && (!strcmp(mode,"alloc-input") || !strcmp(mode,"inode-input") || !strcmp(mode,"bind-alloc"))) ||
        (opens==2 && (!strcmp(mode,"alloc-output") || !strcmp(mode,"inode-output")));
    struct fd *fd=__real_native_fs_open(c,p,flags,permissions);
    allocating=false;
    if(IS_ERR(fd)) return fd;
    if(!changed_mount) {
        changed_mount=fd->mount; original_fs=fd->mount->fs; injected_fs=*original_fs;
        injected_fs.fstat=injected_fstat; changed_mount->fs=&injected_fs;
    }
    if(opens==1) {
        input=fd; original_input=fd->ops; input_ops=*fd->ops;
        input_ops.read=input_read; fd->ops=&input_ops;
        if(!strcmp(mode,"no-read")) input_ops.read=NULL;
    } else {
        output=fd; original_output=fd->ops; output_ops=*fd->ops;
        output_ops.write=output_write; fd->ops=&output_ops;
        if(!strcmp(mode,"no-write")) output_ops.write=NULL;
    }
    return fd;
}
int __real_fd_close(struct fd *);
int __wrap_fd_close(struct fd *f) {
    bool closing_input=active && f==input, closing_output=active && f==output;
    if(closing_input || closing_output) {
        closes++;
        if(closing_input) input=NULL;
        if(closing_output) output=NULL;
        if(!strcmp(mode,"cancel-close")) send_signal(owner,SIGTERM_,SIGINFO_NIL);
    }
    int result=__real_fd_close(f);
    if((closing_input && !strcmp(mode,"close-input")) || (closing_output && !strcmp(mode,"close-output"))) return _EIO;
    return result;
}
int __real_native_handler_check(struct native_handler_context *);
int __wrap_native_handler_check(struct native_handler_context *c) {
    if(active) {
        checkpoints++;
        if(!strcmp(mode,"deadline") && checkpoints==5) c->deadline_ns=0;
    }
    return __real_native_handler_check(c);
}
static int callback(int argc,char **argv,struct native_handler_context *c) {
    calls++; active=true;
    int result=native_local_copy(argc,argv,c);
    assert(input==NULL && output==NULL);
    if(changed_mount) changed_mount->fs=original_fs;
    assert(reads+writes<=NATIVE_COPY_OPS);
    if(injected_allocations) assert(injected_allocations==1);
    if(sender_started) {
        assert(pthread_join(sender,NULL)==0); pthread_barrier_destroy(&barrier); sender_started=false;
    }
    assert(result==expected || native_handler_signal(c)==SIGTERM_);
    active=false; return result;
}
noreturn void __wrap_do_exit(int status) {
    assert(current->native_cancel==NULL);
    int terminal=(!strncmp(mode,"cancel-",7) || !strcmp(mode,"replace") || !strcmp(mode,"pending")) ? SIGTERM_ : expected<<8;
    assert(status==terminal); longjmp(exited,1);
}
static unsigned descriptors(void) {
    DIR *d=fdopendir(open("/proc/self/fd",O_RDONLY|O_DIRECTORY)); assert(d); unsigned n=0;
    struct dirent *e; while((e=readdir(d))) if(e->d_name[0]!='.')n++;
    closedir(d); return n;
}
static void contents(const char *path,const char *expected_data,size_t size) {
    struct fd *fd=generic_open(path,O_RDONLY_,0); assert(!IS_ERR(fd));
    struct statbuf stat; assert(fd->mount->fs->fstat(fd,&stat)==0 && stat.size==size);
    char buf[4096]; size_t got=0;
    while(got<size) {
        size_t n=size-got; if(n>sizeof(buf))n=sizeof(buf);
        ssize_t r=fd->ops->read(fd,buf,n); assert(r>0 && (size_t)r<=n);
        if(expected_data) assert(!memcmp(buf,expected_data+got,r));
        else for(ssize_t i=0;i<r;i++)assert((unsigned char)buf[i]==(unsigned char)((got+i)*37+11));
        got+=(size_t)r;
    }
    fd_close(fd);
}
int main(int argc,char **argv) {
    assert(argc==4); mode=argv[3];
    assert(native_offload_add_cooperative_handler("local-copy",callback)==0);
    const struct fs_ops *fs=!strcmp(argv[1],"fake")?&fakefs:&realfs;
    assert(mount_root(fs,argv[2])==0 && become_first_process()==0); owner=current; owner->thread=pthread_self();
    struct fd *cwd=generic_open("/a",O_RDONLY_|O_DIRECTORY_,0); assert(!IS_ERR(cwd)); fs_chdir(current->fs,cwd);
    current->fs->umask=0077;
    char mounted[4096]; snprintf(mounted,sizeof(mounted),"%s/mounted",argv[2]);
    assert(do_mount(&realfs,mounted,"/mounted","",0)==0);
    const char *source="input"; destination="output"; size_t size=17011;
    expected=0;
    if(!strcmp(mode,"empty")) { source="empty"; size=0; }
    if(!strcmp(mode,"max")) { source="max"; size=NATIVE_COPY_MAX; }
    if(!strcmp(mode,"oversize")) source="oversize";
    if(!strcmp(mode,"fifo")) source="fifo";
    if(!strcmp(mode,"directory")) source="/a";
    if(!strcmp(mode,"symlink-input")) source="input-link";
    if(!strcmp(mode,"missing")) source="missing";
    if(!strcmp(mode,"mounted")) destination="/mounted/output";
    if(!strcmp(mode,"bind-alloc")) {
        if(fs==&fakefs) { assert(fakefs_bind_mount("/bound",mounted,false)==0); source="/bound/input"; }
        else source="/mounted/input";
    }
    if(!strcmp(mode,"umask")) current->fs->umask=0777;
    if(!strcmp(mode,"existing")) destination="existing";
    if(!strcmp(mode,"same")) destination=source;
    if(!strcmp(mode,"symlink-output")) destination="output-link";
    if(!strcmp(mode,"dangling-output")) destination="dangling";
    if(!strcmp(mode,"permission")) { current->euid=1234; current->egid=1234; source="denied"; }
    if(!strcmp(mode,"usage")) expected=NATIVE_COPY_USAGE;
    if(!strcmp(mode,"oversize") || !strcmp(mode,"fifo") || !strcmp(mode,"directory") ||
        !strcmp(mode,"symlink-input") || !strcmp(mode,"missing") || !strcmp(mode,"permission") ||
        !strcmp(mode,"existing") || !strcmp(mode,"same") || !strcmp(mode,"symlink-output") ||
        !strcmp(mode,"dangling-output") || !strcmp(mode,"open-input") || !strcmp(mode,"open-output") ||
        !strcmp(mode,"no-read") || !strcmp(mode,"no-write") || !strcmp(mode,"stat-input") || !strcmp(mode,"stat-output") ||
        !strcmp(mode,"alloc-input") || !strcmp(mode,"alloc-output") || !strcmp(mode,"bind-alloc") ||
        !strcmp(mode,"inode-input") || !strcmp(mode,"inode-output")) expected=NATIVE_COPY_REFUSED;
    if(!strncmp(mode,"read-",5) || !strncmp(mode,"write-",6) || !strncmp(mode,"close-",6) ||
        !strcmp(mode,"growth") || !strcmp(mode,"shrink") || !strcmp(mode,"ops-limit") || !strcmp(mode,"stat-final")) expected=NATIVE_COPY_IO;
    if(!strcmp(mode,"deadline")) expected=NATIVE_COPY_INTERRUPTED;
    if(!strcmp(mode,"cancel-read") || !strcmp(mode,"cancel-write")) {
        assert(pthread_barrier_init(&barrier,NULL,2)==0);
        assert(pthread_create(&sender,NULL,cancel_sender,NULL)==0); sender_started=true;
    }
    if(!strcmp(mode,"pending")) send_signal(current,SIGTERM_,SIGINFO_NIL);
    char packed[8192]; char *p=packed;
    const char *arguments[]={"/bin/local-copy",source,destination};
    for(unsigned i=0;i<3;i++){size_t n=strlen(arguments[i])+1;memcpy(p,arguments[i],n);p+=n;} *p=0;
    unsigned fd_count=descriptors(), root_refs=current->fs->root->refcount, pwd_refs=current->fs->pwd->refcount;
    char before[4096],after[4096]; assert(getcwd(before,sizeof(before)));
    if(setjmp(exited)==0) { native_offload_exec("[builtin]","/bin/local-copy",!strcmp(mode,"usage")?2:3,packed,""); abort(); }
    assert(descriptors()==fd_count && current->fs->root->refcount==root_refs && current->fs->pwd->refcount==pwd_refs);
    assert(getcwd(after,sizeof(after)) && !strcmp(before,after) && !sender_started);
    current->euid=0; current->egid=0;
    struct statbuf stat;
    if(!strcmp(mode,"umask")) {
        assert(generic_statat(AT_PWD,destination,&stat,true)==0 && (stat.mode&0777)==0);
        assert(generic_setattrat(AT_PWD,destination,make_attr(mode,0600),true)==0);
    }
    if(!expected && strcmp(mode,"pending") && strncmp(mode,"cancel-",7) && strcmp(mode,"replace")) {
        contents(destination,NULL,size); assert(generic_statat(AT_PWD,destination,&stat,true)==0 && (stat.mode&0777)==0600);
    }
    contents("existing","original",8);
    if(!strcmp(mode,"replace")) { assert(altered); contents(destination,"replacement",11); contents("saved-partial",NULL,4096); }
    else if(!strcmp(mode,"existing")) contents(destination,"original",8);
    else if(!strcmp(mode,"same")) contents(source,NULL,size);
    else if(!strcmp(mode,"symlink-output")) contents("input",NULL,size);
    else if(!strcmp(mode,"dangling-output")) assert(generic_statat(AT_PWD,"absent",&stat,true)==_ENOENT);
    else if(opens<2 || !strcmp(mode,"open-output") || !strcmp(mode,"alloc-output")) assert(generic_statat(AT_PWD,"output",&stat,true)==_ENOENT);
    else if(expected || !strncmp(mode,"cancel-",7)) {
        assert(generic_statat(AT_PWD,destination,&stat,true)==0 && stat.size<=NATIVE_COPY_MAX);
        assert(!reads || closes==2);
        size_t partial=0;
        if(!strcmp(mode,"write-partial") || !strcmp(mode,"shrink") || !strcmp(mode,"cancel-write") || !strcmp(mode,"deadline")) partial=4096;
        if(!strcmp(mode,"growth") || !strcmp(mode,"stat-final") || !strncmp(mode,"close-",6) || !strcmp(mode,"cancel-close")) partial=size;
        if(!strcmp(mode,"ops-limit")) partial=512;
        contents(destination,NULL,partial);
    }
    assert((!calls && !strcmp(mode,"pending")) || calls==1);
    printf("local-copy-ok backend=%s mode=%s calls=%u opens=%u closes=%u read=%u write=%u checkpoints=%u no-workers/no-clobber/partial-retained\n",argv[1],mode,calls,opens,closes,reads,writes,checkpoints);
}
