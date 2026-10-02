// Actual procfs endpoint: absent in gadget builds, read-only in native builds.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "kernel/init.h"
#include "kernel/fs.h"
#include "kernel/errno.h"
#include "fs/real.h"
#include "fs/proc.h"
#include "fs/path.h"
#ifdef ISH_JIT
#include "asbestos/guest-arm64/jit.h"
#endif
int main(void) {
    assert(mount_root(&realfs,"/")==0 && become_first_process()==0);
    struct statbuf stat;
#ifdef ISH_JIT
    assert(procfs.stat(NULL,"/ish/jit-layout",&stat)==0 && (stat.mode&0777)==0444);
    struct jit_layout before,after; assert(jit_layout_read(&before)==0 && !before.ready);
    assert(do_mount(&procfs,"","/proc","",0)==0);
    struct fd *fd=generic_open("/proc/ish/jit-layout",O_RDONLY_,0); assert(!IS_ERR(fd));
    char text[2048]={0}; ssize_t n=fd->ops->pread(fd,text,sizeof(text)-1,0);
    assert(n>0 && strstr(text,"\"ready\":0") && strstr(text,"\"abi\":0"));
    assert(fd->ops->pwrite(fd,"on",2,0)==_EPERM);
    assert(jit_layout_read(&after)==0 && !memcmp(&before,&after,sizeof(before)));
    fd_close(fd);
    printf("jit-layout-proc-ok readonly/not-ready %s",text);
#else
    assert(procfs.stat(NULL,"/ish/jit-layout",&stat)==_ENOENT);
    puts("jit-layout-proc-ok gadget endpoint absent");
#endif
}
