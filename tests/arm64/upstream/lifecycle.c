#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static volatile sig_atomic_t signalled;
static void handler(int s) { (void)s; signalled=1; }
static double now(void) { struct timespec t; assert(!clock_gettime(CLOCK_MONOTONIC,&t)); return t.tv_sec+t.tv_nsec/1e9; }
static void sleeps(void) {
    struct timespec bad={0,-1},req={0,200000000},rem={123,456};
    errno=0; assert(nanosleep(&bad,&rem)==-1&&errno==EINVAL);
    struct sigaction sa={.sa_handler=handler}; sigemptyset(&sa.sa_mask); assert(!sigaction(SIGUSR1,&sa,NULL));
    pid_t p=fork(); assert(p>=0);
    if(!p) { struct timespec d={0,30000000}; nanosleep(&d,NULL); kill(getppid(),SIGUSR1); _exit(0); }
    double start=now(); errno=0; int r=nanosleep(&req,&rem); double elapsed=now()-start;
    assert(r==-1&&errno==EINTR&&signalled&&elapsed<0.18);
    assert(rem.tv_sec==0&&rem.tv_nsec>0&&rem.tv_nsec<200000000);
    int status; assert(waitpid(p,&status,0)==p&&status==0);
    sigset_t set; sigemptyset(&set); sigaddset(&set,SIGUSR1); assert(!sigprocmask(SIG_BLOCK,&set,NULL));
    p=fork(); assert(p>=0);
    if(!p) { struct timespec d={0,30000000}; nanosleep(&d,NULL); kill(getppid(),SIGUSR1); _exit(0); }
    start=now(); assert(!nanosleep(&req,NULL)); assert(now()-start>=0.19);
    assert(waitpid(p,&status,0)==p&&status==0);
    struct timespec zero={0}; assert(sigtimedwait(&set,NULL,&zero)==SIGUSR1);
    assert(!sigprocmask(SIG_UNBLOCK,&set,NULL));
    puts("sleep-deadline-signal-ok");
}
static void proc_race(void) {
    for(int round=0;round<30;round++) {
        pid_t p=fork(); assert(p>=0);
        if(!p) { struct timespec d={0,2000000}; nanosleep(&d,NULL); _exit(0); }
        char name[96],buf[256]; const char *files[]={"maps","statm","auxv","cmdline","mem"};
        for(int pass=0;pass<3;pass++)for(int f=0;f<5;f++) {
            snprintf(name,sizeof name,"/proc/%d/%s",p,files[f]);
            int fd=open(name,O_RDONLY); if(fd<0)continue;
            (void)read(fd,buf,sizeof buf); close(fd);
        }
        int status; assert(waitpid(p,&status,0)==p&&status==0);
    }
    puts("proc-exit-bounded-ok rounds=30");
}
static void orphan(void) {
    char dir[]="/tmp/orphan-XXXXXX"; assert(mkdtemp(dir)); char a[128],b[128];
    snprintf(a,sizeof a,"%s/a",dir); snprintf(b,sizeof b,"%s/b",dir);
    for(int i=0;i<50;i++) {
        int fd=open(a,O_CREAT|O_RDWR|O_TRUNC,0600); assert(fd>=0); assert(write(fd,"old",3)==3);
        assert(!unlink(a)); struct stat s; assert(!fstat(fd,&s)&&s.st_size==3);
        char bytes[4]={0}; assert(pread(fd,bytes,3,0)==3&&!strcmp(bytes,"old")); assert(!close(fd));
        fd=open(a,O_CREAT|O_RDWR|O_TRUNC,0600); assert(fd>=0); assert(write(fd,"old",3)==3);
        int other=open(b,O_CREAT|O_WRONLY|O_TRUNC,0600); assert(other>=0); assert(write(other,"new",3)==3); close(other);
        assert(!rename(b,a)); assert(!fstat(fd,&s)&&s.st_size==3);
        memset(bytes,0,sizeof bytes); assert(pread(fd,bytes,3,0)==3&&!strcmp(bytes,"old")); close(fd); unlink(a);
    }
    rmdir(dir); puts("orphan-last-close-ok rounds=50");
}
int main(int argc,char **argv) { if(argc!=2)return 2; alarm(20); if(!strcmp(argv[1],"sleep"))sleeps();else if(!strcmp(argv[1],"proc"))proc_race();else if(!strcmp(argv[1],"orphan"))orphan();else return 2; }
