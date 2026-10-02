// Actual VFS context: independent guest roots/CWD, symlink limits and metadata.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "kernel/init.h"
#include "kernel/calls.h"
#include "kernel/native_offload.h"
#include "fs/real.h"
#define ISH_INTERNAL
#include "fs/fake.h"
#undef ISH_INTERNAL
#include "fs/path.h"
static unsigned fail_at, allocation;
void *__real_malloc(size_t);
void *__wrap_malloc(size_t size) {
    if(fail_at && ++allocation==fail_at) return NULL;
    return __real_malloc(size);
}
static struct native_fs_context *foreign_context;
static struct native_fs_context *snapshot(const char *root_path,const char *cwd_path) {
    struct fd *root=generic_open(root_path,O_RDONLY_|O_DIRECTORY_,0);
    struct fd *cwd=generic_open(cwd_path,O_RDONLY_|O_DIRECTORY_,0);
    assert(!IS_ERR(root)&&!IS_ERR(cwd));
    lock(&current->fs->lock);
    struct fd *old_root=current->fs->root, *old_cwd=current->fs->pwd;
    current->fs->root=root; current->fs->pwd=cwd;
    unlock(&current->fs->lock);
    struct native_fs_context *c=native_fs_context_create(); assert(!IS_ERR(c));
    lock(&current->fs->lock);
    current->fs->root=old_root; current->fs->pwd=old_cwd;
    unlock(&current->fs->lock);
    fd_close(root); fd_close(cwd); return c;
}
static void read_value(struct native_fs_context *c,const char *path,const char *want) {
    struct fd *f=native_fs_open(c,path,O_RDONLY_,0);
    if(IS_ERR(f))fprintf(stderr,"native-fs open %s error=%ld\n",path,(long)PTR_ERR(f));
    assert(!IS_ERR(f));
    char buf[32]={0}; ssize_t n=f->ops->read(f,buf,sizeof(buf));
    assert(n==(ssize_t)strlen(want)&&!memcmp(buf,want,n)); fd_close(f);
}
static void *worker(void *argument) {
    const char *dir=argument;
    struct task local={0}; current=&local;
    extern struct task *test_owner;
    assert(IS_ERR(native_fs_open(foreign_context,"/a/item",O_RDONLY_,0)));
    local.fs=fs_info_copy(test_owner->fs);
    local.euid=test_owner->euid; local.egid=test_owner->egid;
    struct fd *cwd=generic_open(dir,O_RDONLY_|O_DIRECTORY_,0); assert(!IS_ERR(cwd));
    fs_chdir(local.fs,cwd);
    struct native_fs_context *c=native_fs_context_create(); assert(!IS_ERR(c));
    for(unsigned i=0;i<50;i++) read_value(c,"item",!strcmp(dir,"/a")?"alpha":"beta");
    native_fs_context_destroy(c); fs_info_release(local.fs); return NULL;
}
struct task *test_owner;
int main(int argc,char **argv) {
    assert(argc==3);
    const struct fs_ops *fs=!strcmp(argv[1],"fake")?&fakefs:&realfs;
    assert(mount_root(fs,argv[2])==0 && become_first_process()==0);
    test_owner=current;
    char before[4096],after[4096]; assert(getcwd(before,sizeof(before)));
    unsigned root_refs=current->fs->root->refcount, cwd_refs=current->fs->pwd->refcount;
    for(fail_at=1;fail_at<=2;fail_at++) {
        allocation=0;
        struct native_fs_context *failed=native_fs_context_create();
        assert(IS_ERR(failed)&&PTR_ERR(failed)==_ENOMEM);
        assert(current->fs->root->refcount==root_refs && current->fs->pwd->refcount==cwd_refs);
    }
    fail_at=0;
    struct native_fs_context *a=snapshot("/","/a"),*b=snapshot("/","/b");
    read_value(a,"item","alpha"); read_value(b,"item","beta");
    read_value(a,"../b/item","beta"); read_value(a,"/b/item","beta");
    read_value(a,"link","beta");
    struct fd *f=native_fs_open(a,"link",O_RDONLY_|O_NOFOLLOW_,0);
    assert(IS_ERR(f)&&PTR_ERR(f)==_ELOOP);
    f=native_fs_open(a,"out",O_CREAT_|O_WRONLY_,0666); assert(!IS_ERR(f));
    assert(f->ops->write(f,"created",7)==7); fd_close(f);
    struct statbuf stat; assert(generic_statat(AT_PWD,"/a/out",&stat,true)==0);
    assert((stat.mode&0777)==0644); read_value(a,"out","created");
    // Explicit context constrains absolute symlinks and .. to its guest root.
    struct native_fs_context *jail=snapshot("/jail","/jail/inside");
    read_value(jail,"/value","jailed"); read_value(jail,"../../value","jailed");
    read_value(jail,"absolute","jailed");
    f=native_fs_open(jail,"/b/item",O_RDONLY_,0); assert(IS_ERR(f));
    native_fs_context_destroy(jail);
    // Non-root guest must still obey VFS read permissions, not raw host paths.
    struct attr mode=make_attr(mode,0000);
    assert(generic_setattrat(AT_PWD,"/a/item",mode,true)==0);
    current->euid=1234; current->egid=1234;
    f=native_fs_open(a,"item",O_RDONLY_,0); assert(IS_ERR(f)&&PTR_ERR(f)==_EACCES);
    current->euid=0; current->egid=0;
    mode=make_attr(mode,0644); assert(generic_setattrat(AT_PWD,"/a/item",mode,true)==0);
    // Longest-prefix mounted filesystem is selected by the guest VFS.
    char mounted[4096]; assert(snprintf(mounted,sizeof(mounted),"%s/mounted",argv[2])<(int)sizeof(mounted));
    assert(do_mount(&realfs,mounted,"/mounted","",0)==0);
    read_value(a,"/mounted/value","mounted");
    foreign_context=a;
    pthread_t threads[2];
    assert(pthread_create(&threads[0],NULL,worker,"/a")==0);
    assert(pthread_create(&threads[1],NULL,worker,"/b")==0);
    assert(pthread_join(threads[0],NULL)==0 && pthread_join(threads[1],NULL)==0);
    assert(getcwd(after,sizeof(after))&&!strcmp(before,after));
    native_fs_context_destroy(a); native_fs_context_destroy(b);
    printf("native-fs-context-ok backend=%s mount/root/CWD/symlink/metadata/concurrency\n",argv[1]);
}
