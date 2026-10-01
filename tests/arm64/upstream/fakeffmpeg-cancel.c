// The test-only source contains portable C; no Objective-C/Apple SDK adapter.
#define ISH_FFMPEG_TEST 1
#include "app/FakeFFmpeg.m"
#include <assert.h>
#include "kernel/init.h"
#include "kernel/calls.h"
#include "fs/real.h"
static bool cancel_case;
static bool copy_case;
static unsigned ticks, reads;
ssize_t __real_read(int fd, void *buf, size_t n);
ssize_t __wrap_read(int fd, void *buf, size_t n) {
    ssize_t result = __real_read(fd, buf, n);
    // First read is the header; second is the copy loop. Deliver between the
    // host read and next poll, then require closure without a completed copy.
    if (copy_case && ++reads == 2) send_signal(current, SIGINT_, SIGINFO_NIL);
    return result;
}
int __wrap_usleep(useconds_t delay) {
    assert(delay == 100000);
    if (cancel_case && ++ticks == 5) send_signal(current, SIGINT_, SIGINFO_NIL);
    return 0;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    cancel_case = !strcmp(argv[1], "cancel");
    copy_case = !strcmp(argv[1], "copy");
    assert(cancel_case || copy_case || !strcmp(argv[1], "normal"));
    assert(mount_root(&realfs, "/") == 0 && become_first_process() == 0);
    char in[] = "/tmp/ish-fake-input-XXXXXX", out[] = "/tmp/ish-fake-output-XXXXXX";
    int in_fd = mkstemp(in), out_fd = mkstemp(out);
    assert(in_fd >= 0 && out_fd >= 0); close(out_fd); assert(unlink(out) == 0);
    char bytes[4096]; memset(bytes, 42, sizeof(bytes));
    assert(write(in_fd, bytes, sizeof(bytes)) == sizeof(bytes)); close(in_fd);
    int null_fd = open("/dev/null", O_RDWR); assert(null_fd >= 0);
    struct native_cancel token; assert(native_cancel_begin(current, &token));
    char *args[] = {"ffmpeg", "-i", in, out, NULL};
    int ret = fake_ffmpeg_main(4, args, null_fd, null_fd, null_fd, &token);
    int sig = native_cancel_finish(current, &token);
    if (cancel_case) {
        assert(ret == 1 && sig == SIGINT_ && access(out, F_OK) < 0 && ticks == 5);
    } else if (copy_case) {
        assert(ret == 1 && sig == SIGINT_ && reads == 2);
        struct stat st; assert(stat(out, &st) == 0 && st.st_size == 0);
        unlink(out);
    } else {
        assert(ret == 0 && sig == 0);
        out_fd = open(out, O_RDONLY); assert(out_fd >= 0);
        char actual[4096]; assert(read(out_fd, actual, sizeof(actual)) == sizeof(actual));
        assert(!memcmp(actual, bytes, sizeof(bytes))); close(out_fd); unlink(out);
    }
    close(null_fd); unlink(in);
    puts("fakeffmpeg-cooperative-test-ok");
}
