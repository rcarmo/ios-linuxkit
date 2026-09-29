#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <unistd.h>

int main(void) {
    char path[]="/tmp/ish-poll-regular.XXXXXX";
    int fd=mkstemp(path);assert(fd>=0);assert(write(fd,"123\n",4)==4);
    int ro=open(path,O_RDONLY),wo=open(path,O_WRONLY),copy=dup(fd),dir=open("/tmp",O_RDONLY|O_DIRECTORY);
    assert(ro>=0&&wo>=0&&copy>=0&&dir>=0);assert(!unlink(path));
    int fds[]={fd,ro,wo,copy,dir};
    for(unsigned i=0;i<5;i++) {
        struct pollfd p={.fd=fds[i],.events=POLLIN|POLLOUT};
        assert(poll(&p,1,0)==1);assert((p.revents&(POLLIN|POLLOUT))==(POLLIN|POLLOUT));
        p.events=POLLIN;p.revents=0;assert(poll(&p,1,30)==1&&p.revents==POLLIN);
        p.events=POLLOUT;p.revents=0;assert(poll(&p,1,30)==1&&p.revents==POLLOUT);
        p.events=0;p.revents=0;assert(poll(&p,1,0)==0&&p.revents==0);
        p.events=POLLPRI;assert(poll(&p,1,0)==0&&p.revents==0);
        p.events=POLLIN;struct timespec ts={.tv_nsec=30000000};
        assert(ppoll(&p,1,&ts,NULL)==1&&p.revents==POLLIN);
        fd_set r,w;FD_ZERO(&r);FD_ZERO(&w);FD_SET(fds[i],&r);FD_SET(fds[i],&w);
        struct timeval tv={.tv_usec=30000};assert(select(fds[i]+1,&r,&w,NULL,&tv)>0);
        assert(FD_ISSET(fds[i],&r)&&FD_ISSET(fds[i],&w));
        FD_ZERO(&r);FD_SET(fds[i],&r);ts=(struct timespec){.tv_nsec=30000000};
        assert(pselect(fds[i]+1,&r,NULL,NULL,&ts,NULL)==1&&FD_ISSET(fds[i],&r));
    }
    // Regular files are ready even at EOF; epoll must still reject them.
    assert(lseek(ro,0,SEEK_END)==4);struct pollfd p={.fd=ro,.events=POLLIN};
    assert(poll(&p,1,0)==1&&p.revents==POLLIN);
    int ep=epoll_create1(0);assert(ep>=0);struct epoll_event ev={.events=EPOLLIN};
    errno=0;assert(epoll_ctl(ep,EPOLL_CTL_ADD,fd,&ev)==-1&&errno==EPERM);
    int pipefd[2];assert(!pipe(pipefd));
    struct pollfd mixed[]={{.fd=pipefd[0],.events=POLLIN},{.fd=ro,.events=POLLIN}};
    assert(poll(mixed,2,30)==1&&mixed[0].revents==0&&mixed[1].revents==POLLIN);
    assert(write(pipefd[1],"x",1)==1);assert(poll(mixed,2,30)==2);
    // Preserve host-backed pipe epoll registration/readiness.
    assert(!epoll_ctl(ep,EPOLL_CTL_ADD,pipefd[0],&ev));
    assert(epoll_wait(ep,&ev,1,30)==1&&(ev.events&EPOLLIN));
    close(ep);close(pipefd[0]);close(pipefd[1]);for(unsigned i=0;i<5;i++)close(fds[i]);
    puts("poll-regular-ok: access modes, EOF, dup, select/ppoll, mixed pipe, epoll rejection");
}
