// Synthetic selection only. No 16KiB host allocation/performance claim.
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include "kernel/memory_cluster.h"
struct fixture { unsigned owner[4], flags[4]; bool mapped[4]; };
static bool compatible(uint64_t p, void *context) {
    struct fixture *f = context;
    assert(p >= 0x100 && p < 0x104);
    unsigned i = p - 0x100;
    return f->owner[i] == 7 && f->flags[i] == 3 && !f->mapped[i];
}
int main(void) {
    struct fixture f = {{7,7,7,7}, {3,3,3,3}, {0}};
    uint64_t start;
    for (unsigned i = 0; i < 4; i++) {
        assert(ish_memory_cluster_span(0x100+i,0x100,0x104,16384,compatible,&f,&start)==4 && start==0x100);
        assert(ish_memory_cluster_span(0x100+i,0x100,0x104,4096,compatible,&f,&start)==1 && start==0x100+i);
    }
    f.owner[2]=8;
    assert(ish_memory_cluster_span(0x100,0x100,0x104,16384,compatible,&f,&start)==1);
    f.owner[2]=7; f.flags[3]=1;
    assert(ish_memory_cluster_span(0x100,0x100,0x104,16384,compatible,&f,&start)==1);
    f.flags[3]=3; f.mapped[0]=true;
    assert(ish_memory_cluster_span(0x101,0x100,0x104,16384,compatible,&f,&start)==1);
    f.mapped[0]=false;
    assert(ish_memory_cluster_span(0x101,0x101,0x104,16384,compatible,&f,&start)==1);
    assert(ish_memory_cluster_span(0x101,0x100,0x103,16384,compatible,&f,&start)==1);
    assert(ish_memory_cluster_span(0x101,0x100,0x104,12288,compatible,&f,&start)==1);
    assert(ish_memory_cluster_span(0x101,0x100,0x104,32768,compatible,&f,&start)==1);
    assert(ish_memory_cluster_span(UINT64_MAX,0,UINT64_MAX,16384,compatible,&f,&start)==1);
    size_t actual = (size_t)sysconf(_SC_PAGESIZE);
    if (actual == 4096)
        assert(ish_memory_cluster_span(0x101,0x100,0x104,actual,compatible,&f,&start)==1);
    printf("cluster-geometry-ok actual_host_bytes=%zu synthetic_16KiB_only\n", actual);
}
