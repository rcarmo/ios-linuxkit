#define _POSIX_C_SOURCE 200809L
#include "memory_pressure_linux.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TEXT_MAX (256 * 1024)
#define ANCESTORS_MAX 128
#define PATH_CAP MEMORY_PRESSURE_LINUX_PATH_MAX

struct control {
    uint64_t current, max, high;
    bool max_unlimited, high_unlimited, absent;
    dev_t dev;
    ino_t ino;
};
struct observation {
    char membership[PATH_CAP], mount_root[PATH_CAP], mountpoint[PATH_CAP];
    char *cgroup_text, *mount_text;
    struct control controls[ANCESTORS_MAX];
    unsigned count;
    uint64_t host_total;
    struct memory_pressure_linux_snapshot result;
};

static int bad(int error) {
    errno = error;
    return -1;
}

// Bounded full reads: reject NUL, absent final newline and oversized input.
static int text_read(int dir, const char *name, char *out, size_t cap) {
    int fd = openat(dir, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return -1;
    size_t n = 0;
    int error = 0;
    while (n < cap - 1) {
        ssize_t got = read(fd, out + n, cap - 1 - n);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            error = errno;
            break;
        }
        if (got == 0)
            break;
        n += (size_t) got;
    }
    if (!error && n == cap - 1) {
        char extra;
        ssize_t got;
        do { got = read(fd, &extra, 1); } while (got < 0 && errno == EINTR);
        if (got != 0)
            error = got < 0 ? errno : EOVERFLOW;
    }
    close(fd);
    if (error)
        return bad(error);
    if (!n || out[n - 1] != '\n' || memchr(out, 0, n))
        return bad(EINVAL);
    out[n] = 0;
    if (out[0] == '\n' || strstr(out, "\n\n"))
        return bad(EINVAL);
    return 0;
}

static bool canonical(const char *p) {
    if (*p != '/' || strlen(p) >= PATH_CAP)
        return false;
    if (!p[1])
        return true;
    for (const char *s = p + 1; *s;) {
        const char *end = strchr(s, '/');
        size_t n = end ? (size_t) (end - s) : strlen(s);
        if (!n || (n == 1 && *s == '.') || (n == 2 && !memcmp(s, "..", 2)))
            return false;
        for (size_t i = 0; i < n; i++)
            if ((unsigned char) s[i] < 32 || s[i] == 127)
                return false;
        if (!end)
            return true;
        s = end + 1;
        if (!*s)
            return false;
    }
    return true;
}

static bool beneath(const char *path, const char *root) {
    size_t n = strlen(root);
    return !strcmp(root, "/") || (!strncmp(path, root, n) &&
            (path[n] == 0 || path[n] == '/'));
}

// Linux mountinfo escapes only space, tab, newline and backslash. Reject the
// latter two controls through canonical(), and all unrecognized escapes.
static int unescape(const char *s, char *out) {
    size_t n = 0;
    while (*s) {
        unsigned char c = (unsigned char) *s++;
        if (c == '\\') {
            if (strlen(s) < 3)
                return bad(EINVAL);
            if (!strncmp(s, "040", 3)) c = ' ';
            else if (!strncmp(s, "011", 3)) c = '\t';
            else if (!strncmp(s, "012", 3)) c = '\n';
            else if (!strncmp(s, "134", 3)) c = '\\';
            else return bad(EINVAL);
            s += 3;
        }
        if (n >= PATH_CAP - 1)
            return bad(EOVERFLOW);
        out[n++] = (char) c;
    }
    out[n] = 0;
    return canonical(out) ? 0 : bad(EINVAL);
}

static int number(const char **p, uint64_t *value);

static int resolve(struct observation *o) {
    char *copy = strdup(o->cgroup_text);
    if (!copy)
        return -1;
    unsigned found = 0;
    char *save;
    for (char *line = strtok_r(copy, "\n", &save); line;
            line = strtok_r(NULL, "\n", &save)) {
        char *colon = strchr(line, ':');
        char *second = colon ? strchr(colon + 1, ':') : NULL;
        if (!colon || !second || colon == line || !canonical(second + 1)) {
            free(copy);
            return bad(EINVAL);
        }
        for (char *p = line; p < colon; p++)
            if (*p < '0' || *p > '9') {
                free(copy);
                return bad(EINVAL);
            }
        if (!strncmp(line, "0:", 2)) {
            if (second != line + 2 || ++found != 1 || !canonical(second + 1)) {
                free(copy);
                return bad(EINVAL);
            }
            strcpy(o->membership, second + 1);
        }
    }
    free(copy);
    if (found != 1)
        return bad(EINVAL);
    copy = strdup(o->mount_text);
    if (!copy)
        return -1;
    found = 0;
    int error = 0;
    for (char *line = strtok_r(copy, "\n", &save); line;
            line = strtok_r(NULL, "\n", &save)) {
        char *sep = strstr(line, " - ");
        if (!sep) { error = EINVAL; break; }
        char *post = sep + 3;
        if (strncmp(post, "cgroup2 ", 8))
            continue;
        // Require the three post-separator fields, no ambiguous extras.
        char *postsave, *fields[4];
        unsigned nf = 0;
        for (char *t = strtok_r(post, " ", &postsave); t && nf < 4;
                t = strtok_r(NULL, " ", &postsave)) fields[nf++] = t;
        if (nf != 3) { error = EINVAL; break; }
        (void) fields;
        *sep = 0;
        char *presave, *root = NULL, *point = NULL;
        bool invalid = false;
        unsigned index = 0;
        for (char *t = strtok_r(line, " ", &presave); t;
                t = strtok_r(NULL, " ", &presave), index++) {
            if (index < 3) {
                const char *p = t;
                uint64_t n;
                if (number(&p, &n)) invalid = true;
                else if (index == 2) {
                    if (*p++ != ':' || number(&p, &n) || *p) invalid = true;
                } else if (*p) invalid = true;
            }
            if (index == 3) root = t;
            if (index == 4) point = t;
        }
        char decoded_root[PATH_CAP], decoded_point[PATH_CAP];
        if (invalid || index < 6 || !root || !point || unescape(root, decoded_root) ||
                unescape(point, decoded_point)) { error = EINVAL; break; }
        if (beneath(o->membership, decoded_root)) {
            if (++found != 1) { error = EINVAL; break; }
            strcpy(o->mount_root, decoded_root);
            strcpy(o->mountpoint, decoded_point);
        }
    }
    free(copy);
    if (error || found != 1)
        return bad(error ? error : EINVAL);
    return 0;
}

// Never follow symlinks under the configured root; each component is pinned
// while opening the next. Re-observation also compares directory identities.
static int directory(int rootfd, const char *path) {
    if (!canonical(path))
        return bad(EINVAL);
    int fd = dup(rootfd);
    if (fd < 0)
        return -1;
    char copy[PATH_CAP];
    strcpy(copy, path);
    char *save;
    for (char *p = strtok_r(copy, "/", &save); p; p = strtok_r(NULL, "/", &save)) {
        int next = openat(fd, p, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        int error = errno;
        close(fd);
        if (next < 0)
            return bad(error);
        fd = next;
    }
    return fd;
}

static int number(const char **p, uint64_t *value) {
    const char *s = *p;
    uint64_t n = 0;
    if (*s < '0' || *s > '9')
        return bad(EINVAL);
    while (*s >= '0' && *s <= '9') {
        unsigned d = (unsigned) (*s++ - '0');
        if (n > (UINT64_MAX - d) / 10)
            return bad(EOVERFLOW);
        n = n * 10 + d;
    }
    *p = s;
    *value = n;
    return 0;
}

static int scalar(int dir, const char *name, uint64_t *n, bool *unlimited) {
    char text[64];
    if (text_read(dir, name, text, sizeof(text)))
        return -1;
    *unlimited = !strcmp(text, "max\n");
    if (*unlimited) {
        *n = 0;
        return 0;
    }
    const char *p = text;
    if (number(&p, n))
        return -1;
    return !strcmp(p, "\n") ? 0 : bad(EINVAL);
}

static int meminfo(int procfd, uint64_t *available, uint64_t *total) {
    char text[16384];
    if (text_read(procfd, "meminfo", text, sizeof(text)))
        return -1;
    unsigned seen = 0;
    char *save;
    for (char *line = strtok_r(text, "\n", &save); line;
            line = strtok_r(NULL, "\n", &save)) {
        unsigned bit;
        const char *p;
        if (!strncmp(line, "MemAvailable:", 13)) { bit = 1; p = line + 13; }
        else if (!strncmp(line, "MemTotal:", 9)) { bit = 2; p = line + 9; }
        else continue;
        if (seen & bit)
            return bad(EINVAL);
        seen |= bit;
        while (*p == ' ' || *p == '\t') p++;
        uint64_t n;
        if (number(&p, &n))
            return -1;
        if (*p != ' ' && *p != '\t')
            return bad(EINVAL);
        while (*p == ' ' || *p == '\t') p++;
        if (strcmp(p, "kB"))
            return bad(EINVAL);
        if (n > UINT64_MAX / 1024)
            return bad(EOVERFLOW);
        if (bit == 1) *available = n * 1024;
        else *total = n * 1024;
    }
    if (seen != 3 || !*total || *available > *total)
        return bad(EINVAL);
    return 0;
}

static int percent(const char **p, unsigned *result) {
    uint64_t whole;
    if (number(p, &whole))
        return -1;
    if (**p != '.')
        return bad(EINVAL);
    (*p)++;
    unsigned fraction = 0, digits = 0;
    while (**p >= '0' && **p <= '9') {
        if (++digits > 3)
            return bad(EINVAL);
        fraction = fraction * 10 + (unsigned) (*(*p)++ - '0');
    }
    if (!digits || whole > 100 || (whole == 100 && fraction))
        return bad(EINVAL);
    while (digits++ < 3) fraction *= 10;
    *result = (unsigned) whole * 1000 + fraction;
    return 0;
}

static int pressure(int dir, const char *name, struct memory_pressure_linux_snapshot *r) {
    char text[1024], *save;
    if (text_read(dir, name, text, sizeof(text)))
        return -1;
    unsigned seen = 0;
    for (char *line = strtok_r(text, "\n", &save); line;
            line = strtok_r(NULL, "\n", &save)) {
        unsigned bit;
        const char *p;
        if (!strncmp(line, "some ", 5)) { bit = 1; p = line + 5; }
        else if (!strncmp(line, "full ", 5)) { bit = 2; p = line + 5; }
        else return bad(EINVAL);
        if (seen & bit)
            return bad(EINVAL);
        seen |= bit;
        const char *keys[] = {"avg10=", " avg60=", " avg300="};
        unsigned avg10 = 0;
        for (unsigned i = 0; i < 3; i++) {
            size_t n = strlen(keys[i]);
            if (strncmp(p, keys[i], n))
                return bad(EINVAL);
            p += n;
            unsigned value;
            if (percent(&p, &value))
                return -1;
            if (!i) avg10 = value;
        }
        if (strncmp(p, " total=", 7))
            return bad(EINVAL);
        p += 7;
        uint64_t total;
        if (number(&p, &total))
            return -1;
        if (*p)
            return bad(EINVAL);
        unsigned *value = bit == 1 ? &r->psi_some_millipercent : &r->psi_full_millipercent;
        if (avg10 > *value) *value = avg10;
    }
    return seen == 3 ? 0 : bad(EINVAL);
}

static uint64_t subtract(uint64_t a, uint64_t b) {
    return a > b ? a - b : 0;
}

static void bound(struct memory_pressure_linux_snapshot *r, uint64_t limit,
        uint64_t current, enum memory_pressure_linux_source source, const char *path) {
    uint64_t free_bytes = subtract(limit, current);
    if (free_bytes < r->available_bytes) {
        r->available_bytes = free_bytes;
        r->allowance_bytes = limit;
        r->source = source;
        strcpy(r->source_path, path);
    }
}

static int observe(int rootfd, int procfd, struct observation *o) {
    if (text_read(procfd, "self/cgroup", o->cgroup_text, TEXT_MAX) ||
            text_read(procfd, "self/mountinfo", o->mount_text, TEXT_MAX) || resolve(o))
        return -1;
    uint64_t available = 0, total = 0;
    if (meminfo(procfd, &available, &total))
        return -1;
    o->host_total = total;
    o->result.available_bytes = available;
    o->result.allowance_bytes = total;
    o->result.source = MEMORY_PRESSURE_LINUX_HOST;
    strcpy(o->result.source_path, "/proc/meminfo");
    if (pressure(procfd, "pressure/memory", &o->result))
        return -1;
    char hierarchy[PATH_CAP];
    strcpy(hierarchy, o->membership);
    for (;;) {
        if (o->count == ANCESTORS_MAX)
            return bad(EOVERFLOW);
        const char *relative = hierarchy + strlen(o->mount_root);
        if (!strcmp(o->mount_root, "/")) relative = !strcmp(hierarchy, "/") ? "" : hierarchy;
        char path[PATH_CAP];
        int len = snprintf(path, sizeof(path), "%s%s", o->mountpoint,
                !strcmp(o->mountpoint, "/") && *relative == '/' ? relative + 1 : relative);
        if (len < 0 || len >= PATH_CAP)
            return bad(EOVERFLOW);
        int fd = directory(rootfd, path);
        if (fd < 0)
            return -1;
        struct control *c = &o->controls[o->count++];
        struct stat st;
        int error = 0;
        bool current_unlimited = false;
        if (fstat(fd, &st)) error = errno;
        else {
            c->dev = st.st_dev;
            c->ino = st.st_ino;
            if (scalar(fd, "memory.current", &c->current, &current_unlimited)) {
                error = errno;
                if (error == ENOENT && !strcmp(hierarchy, "/") && !strcmp(o->mount_root, "/")) {
                    // Require all controls absent, not a partial fixture/controller.
                    struct stat unused;
                    if (fstatat(fd, "memory.max", &unused, AT_SYMLINK_NOFOLLOW) == -1 && errno == ENOENT &&
                            fstatat(fd, "memory.high", &unused, AT_SYMLINK_NOFOLLOW) == -1 && errno == ENOENT) {
                        c->absent = true;
                        error = 0;
                    }
                }
            } else if (current_unlimited) error = EINVAL;
            else if (scalar(fd, "memory.max", &c->max, &c->max_unlimited) ||
                    scalar(fd, "memory.high", &c->high, &c->high_unlimited)) error = errno;
            if (!error && pressure(fd, "memory.pressure", &o->result)) error = errno;
        }
        close(fd);
        if (error)
            return bad(error);
        if (!c->absent) {
            if (!c->max_unlimited) bound(&o->result, c->max, c->current, MEMORY_PRESSURE_LINUX_CGROUP_MAX, hierarchy);
            if (!c->high_unlimited) bound(&o->result, c->high, c->current, MEMORY_PRESSURE_LINUX_CGROUP_HIGH, hierarchy);
        }
        if (!strcmp(hierarchy, o->mount_root))
            break;
        char *slash = strrchr(hierarchy, '/');
        if (slash == hierarchy) hierarchy[1] = 0;
        else *slash = 0;
    }
    return 0;
}

static bool stable(const struct observation *a, const struct observation *b) {
    if (strcmp(a->cgroup_text, b->cgroup_text) || strcmp(a->mount_text, b->mount_text) ||
            a->count != b->count || a->host_total != b->host_total)
        return false;
    for (unsigned i = 0; i < a->count; i++) {
        const struct control *x = &a->controls[i], *y = &b->controls[i];
        if (x->dev != y->dev || x->ino != y->ino || x->absent != y->absent ||
                x->max != y->max || x->high != y->high ||
                x->max_unlimited != y->max_unlimited || x->high_unlimited != y->high_unlimited)
            return false;
    }
    return true;
}

static int clock_ns(const struct memory_pressure_linux_options *o, uint64_t *ns) {
    if (o->now_ns)
        return o->now_ns(o->context, ns);
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t))
        return -1;
    if (t.tv_sec < 0 || t.tv_nsec < 0 || t.tv_nsec >= 1000000000 ||
            (uint64_t) t.tv_sec > (UINT64_MAX - (uint64_t) t.tv_nsec) / 1000000000)
        return bad(EOVERFLOW);
    *ns = (uint64_t) t.tv_sec * 1000000000 + (uint64_t) t.tv_nsec;
    return 0;
}

void memory_pressure_linux_options_init(struct memory_pressure_linux_options *o) {
    if (!o) return;
    *o = (struct memory_pressure_linux_options) {
        .root_path = "/", .proc_path = "/proc", .max_attempts = 2,
        .max_sample_ns = 250000000, .psi_some_millipercent = 1000,
        .psi_full_millipercent = 100,
    };
}

int memory_pressure_linux_sample(const struct memory_pressure_linux_options *o,
        struct memory_pressure_linux_snapshot *out) {
    if (!out)
        return bad(EINVAL);
    memset(out, 0, sizeof(*out));
    if (!o || !o->root_path || !o->proc_path || o->root_path[0] != '/' ||
            o->proc_path[0] != '/' || !o->max_attempts || o->max_attempts > 3 ||
            !o->max_sample_ns || o->psi_some_millipercent > 100000 || o->psi_full_millipercent > 100000)
        return bad(EINVAL);
    int rootfd = open(o->root_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (rootfd < 0) return -1;
    int procfd = open(o->proc_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (procfd < 0) { int e = errno; close(rootfd); return bad(e); }
    struct observation *a = calloc(2, sizeof(*a));
    char *texts = malloc(4 * TEXT_MAX);
    int error = 0, rc = -1;
    uint64_t capture = 0, previous = 0;
    if (!a || !texts) { error = ENOMEM; goto done; }
    if (clock_ns(o, &capture)) { error = errno; goto done; }
    previous = capture;
    for (unsigned attempt = 0; attempt < o->max_attempts; attempt++) {
        memset(a, 0, 2 * sizeof(*a));
        for (unsigned i = 0; i < 2; i++) {
            a[i].cgroup_text = texts + (i * 2) * TEXT_MAX;
            a[i].mount_text = texts + (i * 2 + 1) * TEXT_MAX;
            if (i && o->between_reads) o->between_reads(o->context, attempt);
            if (observe(rootfd, procfd, &a[i])) { error = errno; goto done; }
            uint64_t now;
            if (clock_ns(o, &now)) { error = errno; goto done; }
            if (now < previous || now - capture > o->max_sample_ns) { error = EAGAIN; goto done; }
            previous = now;
        }
        if (!stable(&a[0], &a[1])) { error = EAGAIN; continue; }
        struct memory_pressure_linux_snapshot r = a[0].result;
        if (a[1].result.available_bytes < r.available_bytes) {
            unsigned some = r.psi_some_millipercent, full = r.psi_full_millipercent;
            r = a[1].result;
            if (some > r.psi_some_millipercent) r.psi_some_millipercent = some;
            if (full > r.psi_full_millipercent) r.psi_full_millipercent = full;
        } else {
            if (a[1].result.psi_some_millipercent > r.psi_some_millipercent)
                r.psi_some_millipercent = a[1].result.psi_some_millipercent;
            if (a[1].result.psi_full_millipercent > r.psi_full_millipercent)
                r.psi_full_millipercent = a[1].result.psi_full_millipercent;
        }
        r.available_bytes = subtract(r.available_bytes, o->safety_reserve_bytes);
        r.allowance_bytes = subtract(r.allowance_bytes, o->safety_reserve_bytes);
        r.psi = (o->psi_some_millipercent && r.psi_some_millipercent >= o->psi_some_millipercent) ||
                (o->psi_full_millipercent && r.psi_full_millipercent >= o->psi_full_millipercent);
        r.critical = !r.available_bytes || r.psi;
        r.capture_ns = capture;
        r.finish_ns = previous;
        *out = r;
        rc = 0;
        break;
    }
done:
    free(texts);
    free(a);
    close(procfd);
    close(rootfd);
    return rc ? bad(error ? error : EAGAIN) : 0;
}
