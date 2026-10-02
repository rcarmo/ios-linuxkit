// Actual TCP transfer + cooperative dispatcher; deterministic backpressure/errors.
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "kernel/init.h"
#include "kernel/calls.h"
#include "kernel/native_offload.h"
#include "kernel/native_offload_internal.h" // test-only deadline/budget observation
#include "fs/fd.h"
#include "fs/real.h"
#include "fs/sock.h"
extern const struct fd_ops socket_fdops;
static const char *mode;
static struct task *owner;
static struct fd *stream;
static int peer_fd, original_flags, expected_status, failures;
static unsigned calls, sends, receives, close_calls;
static bool in_handler, inject_admit, cleanup_cancel, expire_retry;
static jmp_buf exited;
static pthread_barrier_t barrier;
static size_t drained;
static uint64_t now(void) {
    struct timespec t; assert(clock_gettime(CLOCK_MONOTONIC,&t)==0);
    return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec;
}
static unsigned descriptors(void) {
    DIR *d=opendir("/proc/self/fd"); assert(d); unsigned n=0;
    struct dirent *e; while((e=readdir(d))) if(e->d_name[0]!='.') n++;
    closedir(d); return n;
}
ssize_t __real_send(int,const void *,size_t,int);
ssize_t __wrap_send(int fd,const void *p,size_t n,int flags) {
    if(in_handler && fd==stream->real_fd) {
        sends++; assert((flags & MSG_DONTWAIT) && (flags & MSG_NOSIGNAL));
        assert(n<=NATIVE_IO_CHUNK && fcntl(fd,F_GETFL)==original_flags);
        if(!strcmp(mode,"eintr") && sends<4) { errno=EINTR; return -1; }
        if(!strcmp(mode,"zero")) return 0;
        if(!strcmp(mode,"error")) { errno=EIO; return -1; }
        if(!strcmp(mode,"partial") && n>3) n=3;
    }
    return __real_send(fd,p,n,flags);
}
ssize_t __real_recv(int,void *,size_t,int);
ssize_t __wrap_recv(int fd,void *p,size_t n,int flags) {
    if(in_handler && fd==stream->real_fd) {
        receives++; assert(flags & MSG_DONTWAIT);
        assert(n<=NATIVE_IO_CHUNK && fcntl(fd,F_GETFL)==original_flags);
        if(!strcmp(mode,"recv-eintr") && receives<4) { errno=EINTR; return -1; }
        if(!strcmp(mode,"recv-error")) { errno=EIO; return -1; }
    }
    return __real_recv(fd,p,n,flags);
}
int __real_getsockopt(int,int,int,void *,socklen_t *);
int __wrap_getsockopt(int f,int l,int o,void *v,socklen_t *n) {
    if(inject_admit && !strcmp(mode,"admit-option")) { errno=EIO; failures++; return -1; }
    return __real_getsockopt(f,l,o,v,n);
}
int __real_getpeername(int,struct sockaddr *,socklen_t *);
int __wrap_getpeername(int f,struct sockaddr *p,socklen_t *n) {
    if(inject_admit && !strcmp(mode,"admit-peer")) { errno=EIO; failures++; return -1; }
    return __real_getpeername(f,p,n);
}
int __real_nanosleep(const struct timespec *,struct timespec *);
int __wrap_nanosleep(const struct timespec *req,struct timespec *rem) {
    if(in_handler && !strcmp(mode,"retry-expired")) { expire_retry=true; return 0; }
    return __real_nanosleep(req,rem);
}
int __real_clock_gettime(clockid_t,struct timespec *);
int __wrap_clock_gettime(clockid_t c,struct timespec *t) {
    if(inject_admit && !strcmp(mode,"admit-clock")) { errno=EIO; failures++; return -1; }
    int rc=__real_clock_gettime(c,t);
    if(!rc && expire_retry && c==CLOCK_MONOTONIC) t->tv_sec++;
    return rc;
}
int __real_fd_close(struct fd *);
int __wrap_fd_close(struct fd *fd) {
    if(in_handler && fd==stream) close_calls++;
    return __real_fd_close(fd);
}
void __real_native_fs_context_destroy(struct native_fs_context *);
void __wrap_native_fs_context_destroy(struct native_fs_context *fs) {
    if(cleanup_cancel && in_handler) {
        assert(owner->native_cancel);
        send_signal(owner,SIGTERM_,SIGINFO_NIL);
        assert(native_cancel_signal(owner->native_cancel)==SIGTERM_);
    }
    __real_native_fs_context_destroy(fs);
}
static void *cancel_sender(void *unused) {
    (void)unused; pthread_barrier_wait(&barrier);
    struct timespec delay={.tv_nsec=10000000}; nanosleep(&delay,NULL);
    send_signal(owner,SIGTERM_,SIGINFO_NIL); return NULL;
}
static void *drain_peer(void *argument) {
    size_t expected=*(size_t *)argument; char data[4096]; uint64_t deadline=now()+1000000000;
    pthread_barrier_wait(&barrier);
    while(drained<expected && now()<deadline) {
        ssize_t n=recv(peer_fd,data,sizeof(data),MSG_DONTWAIT);
        if(n>0) { drained+=(size_t)n; continue; }
        assert(n<0 && (errno==EAGAIN || errno==EINTR));
        struct timespec delay={.tv_nsec=1000000}; nanosleep(&delay,NULL);
    }
    assert(drained==expected); return NULL;
}
static void *foreign(void *argument) {
    struct native_handler_context *c=argument; char b;
    assert(!native_handler_fs(c)); assert(native_handler_check(c)==_EPERM);
    assert(native_handler_signal(c)==_EPERM);
    assert(native_handler_read(c,&b,1,0)==_EPERM);
    assert(native_handler_write(c,1,&b,1,0)==_EPERM);
    return NULL;
}
static size_t prefill(void) {
    char data[4096]={0}; size_t total=0;
    for(;;) {
        ssize_t n=send(stream->real_fd,data,sizeof(data),MSG_DONTWAIT|MSG_NOSIGNAL);
        if(n>0) { total+=(size_t)n; assert(total<16*1024*1024); continue; }
        assert(n<0 && errno==EAGAIN); return total;
    }
}
static int handler(int argc,char **argv,struct native_handler_context *c) {
    calls++; in_handler=true; inject_admit=false;
    assert(argc==1 && !strcmp(argv[0],"/bin/probe") && current->did_exec);
    assert(current->native_cancel && stream->refcount==
        ((!strcmp(mode,"closed") || !strcmp(mode,"cloexec")) ? 4u : 6u)); // table + retained aliases
    assert(native_handler_signal(c)==0 && native_handler_check(c)==0);
    char buf[NATIVE_IO_CHUNK]={0}; uint64_t started=now();
    if(!strcmp(mode,"foreign")) {
        pthread_t worker; assert(pthread_create(&worker,NULL,foreign,c)==0);
        assert(pthread_join(worker,NULL)==0); assert(native_handler_check(c)==0);
    } else if(!strcmp(mode,"closed") || !strcmp(mode,"cloexec")) {
        assert(native_handler_write(c,1,"x",1,0)==_EBADF);
    } else if(!strcmp(mode,"cancel-read") || !strcmp(mode,"cancel-write")) {
        if(!strcmp(mode,"cancel-write")) prefill();
        assert(pthread_barrier_init(&barrier,NULL,2)==0);
        pthread_t sender; assert(pthread_create(&sender,NULL,cancel_sender,NULL)==0);
        pthread_barrier_wait(&barrier);
        ssize_t n=!strcmp(mode,"cancel-read") ? native_handler_read(c,buf,1,250) :
            native_handler_write(c,1,"cancel",6,250);
        assert(n==_ECANCELED && native_handler_signal(c)==SIGTERM_);
        assert(pthread_join(sender,NULL)==0); pthread_barrier_destroy(&barrier);
        assert(now()-started<500000000); // scheduling tolerance, not real-time guarantee
    } else if(!strcmp(mode,"timeout-read")) {
        assert(native_handler_read(c,buf,1,20)==_ETIMEDOUT);
        assert(native_handler_check(c)==_ETIMEDOUT);
        assert(now()-started>=20000000 && now()-started<500000000);
    } else if(!strcmp(mode,"timeout-write")) {
        prefill(); assert(native_handler_write(c,1,"x",1,20)==_ETIMEDOUT);
        assert(native_handler_write(c,2,"x",1,0)==_ETIMEDOUT);
    } else if(!strcmp(mode,"backpressure")) {
        size_t full=prefill(), expected=full+10000;
        assert(pthread_barrier_init(&barrier,NULL,2)==0);
        pthread_t consumer; assert(pthread_create(&consumer,NULL,drain_peer,&expected)==0);
        pthread_barrier_wait(&barrier);
        size_t written=0;
        while(written<10000) {
            size_t chunk=10000-written; if(chunk>sizeof(buf))chunk=sizeof(buf);
            ssize_t n=native_handler_write(c,1,buf,chunk,250); assert(n>0); written+=(size_t)n;
        }
        assert(pthread_join(consumer,NULL)==0); pthread_barrier_destroy(&barrier);
        assert(!c->io_error && c->output_left==NATIVE_IO_BYTES-10000);
    } else if(!strcmp(mode,"full-budget-out")) {
        size_t expected=NATIVE_IO_BYTES;
        assert(pthread_barrier_init(&barrier,NULL,2)==0);
        pthread_t consumer; assert(pthread_create(&consumer,NULL,drain_peer,&expected)==0);
        pthread_barrier_wait(&barrier);
        size_t written=0;
        while(written<expected) {
            size_t chunk=expected-written; if(chunk>sizeof(buf)) chunk=sizeof(buf);
            ssize_t n=native_handler_write(c,1,buf,chunk,250); assert(n>0); written+=(size_t)n;
        }
        assert(pthread_join(consumer,NULL)==0); pthread_barrier_destroy(&barrier);
        assert(!c->output_left && native_handler_write(c,2,"x",1,0)==_EFBIG);
    } else if(!strcmp(mode,"retry-expired")) {
        assert(native_handler_read(c,buf,1,20)==_ETIMEDOUT && receives==1);
        expire_retry=false;
    } else if(!strcmp(mode,"return-expired")) {
        // Return without polling; executor must impose failed terminal status.
        c->deadline_ns=0;
    } else if(!strcmp(mode,"budget-out")) {
        c->output_left=4;
        assert(native_handler_write(c,1,"123",3,0)==3);
        assert(native_handler_write(c,2,"4",1,0)==1);
        assert(native_handler_write(c,1,"5",1,0)==_EFBIG && !c->output_left);
    } else if(!strcmp(mode,"budget-in")) {
        c->input_left=4; assert(send(peer_fd,"12345",5,MSG_NOSIGNAL)==5);
        assert(native_handler_read(c,buf,4,0)==4);
        assert(native_handler_read(c,buf,1,0)==_EFBIG && !c->input_left);
    } else if(!strcmp(mode,"deadline")) {
        c->deadline_ns=now(); assert(native_handler_check(c)==_ETIMEDOUT);
        assert(native_handler_write(c,1,"x",1,0)==_ETIMEDOUT);
    } else if(!strcmp(mode,"zero") || !strcmp(mode,"error")) {
        assert(native_handler_write(c,1,"x",1,0)==_EIO);
        assert(native_handler_read(c,buf,1,0)==_EIO && sends==1);
    } else if(!strcmp(mode,"recv-error")) {
        assert(native_handler_read(c,buf,1,0)==_EIO);
        assert(native_handler_write(c,1,"x",1,0)==_EIO && !sends);
    } else if(!strcmp(mode,"broken")) {
        assert(shutdown(stream->real_fd,SHUT_WR)==0);
        // Host SIGPIPE is DEFAULT in this fixture; per-call suppression must work.
        assert(native_handler_write(c,1,"x",1,0)==_EPIPE);
    } else if(!strcmp(mode,"eof")) {
        assert(shutdown(peer_fd,SHUT_WR)==0);
        assert(native_handler_read(c,buf,1,0)==0 && native_handler_check(c)==0);
    } else if(!strcmp(mode,"pending-blocked") || !strcmp(mode,"ignored")) {
        assert(native_handler_signal(c)==0 && native_handler_check(c)==0);
    } else if(!strcmp(mode,"cleanup")) {
        cleanup_cancel=true;
    } else {
        assert(native_handler_read(c,buf,1,0)==_EAGAIN && !c->io_error);
        assert(native_handler_write(c,0,buf,1,0)==_EINVAL);
        assert(native_handler_write(c,1,NULL,1,0)==_EINVAL);
        assert(native_handler_write(c,1,buf,NATIVE_IO_CHUNK+1,0)==_EINVAL);
        assert(native_handler_read(c,buf,1,NATIVE_IO_WAIT_MS+1)==_EINVAL);
        assert(native_handler_write(c,1,buf,0,0)==0 && native_handler_check(c)==0);
        assert(send(peer_fd,"input",5,MSG_NOSIGNAL)==5);
        assert(native_handler_read(c,buf,8,20)==5 && !memcmp(buf,"input",5));
        const char *text="stdout-stderr"; size_t size=strlen(text), written=0;
        while(written<size) {
            ssize_t n=native_handler_write(c,1,text+written,size-written,20);
            assert(n>0); written+=(size_t)n;
        }
        assert(native_handler_write(c,2,"!",1,20)==1);
        size_t got=0; char output[64]={0};
        while(got<size+1) { ssize_t n=recv(peer_fd,output+got,sizeof(output)-got,0); assert(n>0); got+=(size_t)n; }
        assert(got==size+1 && !memcmp(output,text,size) && output[size]=='!');
        assert(c->input_left==NATIVE_IO_BYTES-5 && c->output_left==NATIVE_IO_BYTES-size-1);
    }
    assert(fcntl(stream->real_fd,F_GETFL)==original_flags);
    return 0; // sticky error/signal must not be masked by an accidental success.
}
noreturn void __wrap_do_exit(int status) {
    assert(status==expected_status && current->native_cancel==NULL);
    longjmp(exited,1);
}
static void tcp_pair(void) {
    int listener=socket(AF_INET,SOCK_STREAM,0); assert(listener>=0);
    struct sockaddr_in address={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    assert(bind(listener,(struct sockaddr *)&address,sizeof(address))==0 && listen(listener,1)==0);
    socklen_t n=sizeof(address); assert(getsockname(listener,(struct sockaddr *)&address,&n)==0);
    int client=socket(AF_INET,SOCK_STREAM,0); assert(client>=0);
    assert(connect(client,(struct sockaddr *)&address,n)==0);
    peer_fd=accept(listener,NULL,NULL); assert(peer_fd>=0); close(listener);
    stream=adhoc_fd_create(&socket_fdops); assert(stream);
    stream->real_fd=client; stream->socket.domain=AF_INET_; stream->socket.type=SOCK_STREAM_;
    stream->socket.netlink_peer_fd=-1; stream->flags=O_RDWR_;
    int small=4096; assert(setsockopt(client,SOL_SOCKET,SO_SNDBUF,&small,sizeof(small))==0);
    original_flags=fcntl(client,F_GETFL); assert(!(original_flags & O_NONBLOCK));
    for(unsigned i=0;i<3;i++) current->files->files[i]=fd_retain(stream);
    fd_close(stream); // three table refs
}
int main(int argc,char **argv) {
    assert(argc==2); mode=argv[1];
    assert(native_offload_add_cooperative_handler("probe",handler)==0);
    assert(mount_root(&realfs,"/")==0 && become_first_process()==0);
    owner=current; owner->thread=pthread_self(); tcp_pair();
    // Shared aliases must retain/refund independently without flag mutation.
    assert(stream->refcount==3);
    signal(SIGPIPE,SIG_DFL);
    bool refused=!strncmp(mode,"admit-",6) || !strcmp(mode,"unsupported") ||
        !strcmp(mode,"unix") || !strcmp(mode,"unconnected") || !strcmp(mode,"nested") ||
        !strcmp(mode,"file") || !strcmp(mode,"pipe") || !strcmp(mode,"mismatched") || !strcmp(mode,"wrong-type");
    if(!strcmp(mode,"unsupported")) {
        static const struct fd_ops fake={0}; stream->ops=&fake;
    } else if(!strcmp(mode,"unix")) stream->socket.domain=AF_LOCAL_;
    else if(!strcmp(mode,"unconnected")) {
        close(stream->real_fd); stream->real_fd=socket(AF_INET,SOCK_STREAM,0); assert(stream->real_fd>=0);
        original_flags=fcntl(stream->real_fd,F_GETFL);
    }
    if(!strcmp(mode,"file") || !strcmp(mode,"pipe")) {
        close(stream->real_fd); stream->ops=&realfs_fdops;
        if(!strcmp(mode,"file")) {
            char tmp[]="/tmp/offload-io-XXXXXX"; stream->real_fd=mkstemp(tmp); unlink(tmp);
        } else {
            int ends[2]; assert(pipe(ends)==0); stream->real_fd=ends[0]; close(ends[1]);
        }
        assert(stream->real_fd>=0); original_flags=fcntl(stream->real_fd,F_GETFL);
    }
    if(!strcmp(mode,"mismatched")) stream->socket.domain=AF_INET6_;
    if(!strcmp(mode,"wrong-type")) stream->socket.type=SOCK_DGRAM_;
    if(!strcmp(mode,"closed") || !strcmp(mode,"cloexec")) {
        if(!strcmp(mode,"closed")) { current->files->files[1]=NULL; fd_close(stream); }
        else bit_set(1,current->files->cloexec);
    }
    struct native_cancel other;
    if(!strcmp(mode,"nested")) assert(native_cancel_begin(current,&other));
    if(!strcmp(mode,"pending") || !strcmp(mode,"pending-kill") || !strcmp(mode,"pending-blocked")) {
        int sig=!strcmp(mode,"pending-kill") ? SIGKILL_ : SIGTERM_;
        if(!strcmp(mode,"pending-blocked")) current->blocked=sig_mask(SIGTERM_);
        send_signal(current,sig,SIGINFO_NIL);
    }
    if(!strcmp(mode,"ignored")) { current->sighand->action[SIGTERM_].handler=SIG_IGN_; send_signal(current,SIGTERM_,SIGINFO_NIL); }
    expected_status=(!strncmp(mode,"timeout-",8) || !strncmp(mode,"budget-",7) ||
        !strcmp(mode,"deadline") || !strcmp(mode,"zero") || !strcmp(mode,"error") ||
        !strcmp(mode,"recv-error") || !strcmp(mode,"broken") || !strcmp(mode,"closed") || !strcmp(mode,"cloexec") ||
        !strcmp(mode,"full-budget-out") || !strcmp(mode,"retry-expired") || !strcmp(mode,"return-expired")) ? 1<<8 : 0;
    if(!strncmp(mode,"cancel-",7) || !strcmp(mode,"pending") || !strcmp(mode,"cleanup")) expected_status=SIGTERM_;
    if(!strcmp(mode,"pending-kill")) expected_status=SIGKILL_;
    unsigned before=descriptors(), refs=stream->refcount;
    inject_admit=!strncmp(mode,"admit-",6);
    strcpy(current->comm,"unchanged"); current->altstack=0x44440000; current->altstack_size=8192;
    if(setjmp(exited)==0) {
        int error=native_offload_exec("[builtin]","/bin/probe",1,"/bin/probe\0","");
        inject_admit=false;
        assert(refused && error<0 && !calls && !current->did_exec);
        if(!strncmp(mode,"admit-",6)) assert(failures==1 && error==_EIO);
        if(!strcmp(mode,"nested")) { assert(error==_EBUSY); assert(native_cancel_finish(current,&other)==0); }
        else assert(current->native_cancel==NULL);
        assert(!strcmp(current->comm,"unchanged") && current->altstack==0x44440000 && current->altstack_size==8192);
    } else assert(!refused && current->did_exec);
    assert(descriptors()==before && fcntl(stream->real_fd,F_GETFL)==original_flags);
    unsigned want=refs-(!strcmp(mode,"cloexec") ? 1 : 0);
    assert(stream->refcount==want);
    if(!strcmp(mode,"pending") || !strcmp(mode,"pending-kill")) assert(!calls);
    else if(!refused) assert(calls==1);
    if(!strcmp(mode,"unsupported")) stream->ops=&socket_fdops;
    if(!strcmp(mode,"unix")) stream->socket.domain=AF_INET_;
    for(unsigned i=0;i<3;i++) if(current->files->files[i]) {
        struct fd *fd=current->files->files[i]; current->files->files[i]=NULL; fd_close(fd);
    }
    close(peer_fd);
    printf("offload-io-ok mode=%s calls=%u send=%u recv=%u cleanup=%u bounded/no-flags/no-workers/token-withdrawn\n",mode,calls,sends,receives,close_calls);
}
