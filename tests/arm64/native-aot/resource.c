// Small launcher: isolate CLI peak RSS from Bun's inherited fork high-water mark.
#include <errno.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 2; }
    if (!pid) { execvp(argv[1], argv + 1); perror("execvp"); _exit(127); }
    int status; struct rusage usage;
    while (wait4(pid, &status, 0, &usage) < 0) { if (errno != EINTR) { perror("wait4"); return 2; } }
    fprintf(stderr, "RESOURCE {\"maxRSS\":%ld,\"cpuUs\":%ld}\n",
        usage.ru_maxrss * 1024L,
        usage.ru_utime.tv_sec * 1000000L + usage.ru_utime.tv_usec +
        usage.ru_stime.tv_sec * 1000000L + usage.ru_stime.tv_usec);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
