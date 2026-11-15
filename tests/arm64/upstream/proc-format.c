// Real proc_printf: length boundaries, complete output and bounded OOM fallback.
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fs/proc.h"
static bool fail_alloc, fail_format;
void *__real_malloc(size_t);
void *__wrap_malloc(size_t size) {
    if (fail_alloc) { fail_alloc = false; return NULL; }
    return __real_malloc(size);
}
int __real_vsnprintf(char *, size_t, const char *, va_list);
int __wrap_vsnprintf(char *buf, size_t size, const char *format, va_list args) {
    if (fail_format) { fail_format = false; return -1; }
    return __real_vsnprintf(buf, size, format, args);
}
int main(void) {
    const size_t lengths[] = {0, 1, 4094, 4095, 4096, 4097, 16384};
    char text[16385];
    for (size_t i = 0; i < sizeof(text)-1; i++) text[i] = 'a' + i % 26;
    text[sizeof(text)-1] = 0;
    for (size_t i = 0; i < sizeof(lengths)/sizeof(*lengths); i++) {
        struct proc_data data = {0};
        proc_printf(&data, "%.*s", (int)lengths[i], text);
        assert(data.size == lengths[i]);
        if (data.size) assert(!memcmp(data.data, text, data.size));
        free(data.data);
    }
    struct proc_data data = {0};
    proc_printf(&data, "prefix:");
    proc_printf(&data, "%s:%05d:%s", text, 42, "suffix");
    char expected[sizeof(text)+64];
    int length = snprintf(expected, sizeof(expected), "prefix:%s:%05d:suffix", text, 42);
    assert(data.size == (size_t)length && !memcmp(data.data, expected, data.size));
    free(data.data);
    data = (struct proc_data){0};
    fail_alloc = true;
    proc_printf(&data, "%s", text);
    assert(!fail_alloc && data.size == 4095 && !memcmp(data.data, text, data.size));
    free(data.data);
    data = (struct proc_data){0};
    fail_format = true;
    proc_printf(&data, "%s", text);
    assert(!fail_format && !data.size);
    free(data.data);
    puts("proc-format-bounds-ok");
}
