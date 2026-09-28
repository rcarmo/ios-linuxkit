#define _GNU_SOURCE
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
static volatile sig_atomic_t deliveries;
static void handler(int sig) { (void)sig; deliveries++; }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec+t.tv_nsec/1e9; }
static int timers(void) {
    struct sigaction sa={.sa_handler=handler}; sigemptyset(&sa.sa_mask); sigaction(SIGALRM,&sa,NULL);
    int failures=0;
    for(int explicit=0;explicit<2;explicit++) {
        timer_t t; deliveries=0;
        struct sigevent ev={.sigev_notify=SIGEV_SIGNAL,.sigev_signo=SIGALRM};
        if(timer_create(CLOCK_MONOTONIC,explicit?&ev:NULL,&t)) { perror("timer_create"); return 10; }
        struct itimerspec it={.it_value={0,50000000}};
        if(timer_settime(t,0,&it,NULL)) { perror("timer_settime"); return 11; }
        double deadline=now()+0.5;
        while(!deliveries&&now()<deadline) { struct timespec ts={0,5000000}; nanosleep(&ts,NULL); }
        timer_delete(t);
        printf("timer %s delivered=%d\n",explicit?"explicit":"NULL",deliveries);
        if(deliveries!=1) failures++;
    }
    return failures?1:0;
}
static int flags(void) {
    char dir[]="/tmp/openminis-flags-XXXXXX"; if(!mkdtemp(dir))return 10;
    char file[256],link[256]; snprintf(file,sizeof file,"%s/file",dir); snprintf(link,sizeof link,"%s/link",dir);
    int fd=open(file,O_CREAT|O_WRONLY|O_EXCL,0600); if(fd<0)return 11; close(fd); if(symlink(file,link))return 12;
    errno=0; fd=open(file,O_RDONLY|O_DIRECTORY); int e=errno; printf("O_DIRECTORY regular fd=%d errno=%d expected=%d\n",fd,e,ENOTDIR);
    int failures=fd>=0||e!=ENOTDIR; if(fd>=0)close(fd);
    errno=0; fd=open(link,O_RDONLY|O_NOFOLLOW); e=errno; printf("O_NOFOLLOW symlink fd=%d errno=%d expected=%d\n",fd,e,ELOOP);
    failures+=fd>=0||e!=ELOOP; if(fd>=0)close(fd); unlink(link); unlink(file); rmdir(dir); return failures?1:0;
}
static int signal_wait(void) {
    sigset_t set; sigemptyset(&set); sigaddset(&set,SIGUSR1); if(sigprocmask(SIG_BLOCK,&set,NULL))return 10;
    pid_t parent=getpid(), child=fork(); if(child<0)return 11;
    if(!child) { struct timespec delay={0,30000000}; nanosleep(&delay,NULL); _exit(kill(parent,SIGUSR1)?12:0); }
    struct timespec timeout={0,300000000}; siginfo_t si; memset(&si,0,sizeof si); errno=0;
    int result=sigtimedwait(&set,&si,&timeout),e=errno,status=0; waitpid(child,&status,0);
    printf("sigtimedwait result=%d errno=%d expected=%d child=%d\n",result,e,SIGUSR1,status);
    // Leave the signal blocked: if the bug retained it, unblocking would kill us.
    return result==SIGUSR1&&WIFEXITED(status)&&WEXITSTATUS(status)==0?0:1;
}
int main(int argc,char **argv) {
    if(argc!=2)return 2;
    if(!strcmp(argv[1],"timer"))return timers();
    if(!strcmp(argv[1],"flags"))return flags();
    if(!strcmp(argv[1],"signal"))return signal_wait();
    return 2;
}
