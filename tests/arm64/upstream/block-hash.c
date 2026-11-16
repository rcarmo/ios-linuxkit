// Exercise the production bucket function and actual-core hash teardown.
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include "asbestos/asbestos.h"
int main(void) {
    size_t used[FIBER_INITIAL_HASH_SIZE] = {0};
    for (unsigned i = 0; i < FIBER_INITIAL_HASH_SIZE; i++)
        used[fiber_block_hash((addr_t) 0x400000 + i * 4, FIBER_INITIAL_HASH_SIZE)]++;
    for (unsigned i = 0; i < FIBER_INITIAL_HASH_SIZE; i++) assert(used[i] == 1);
    // Same instruction offset in consecutive mappings exercises page folding.
    for (unsigned i = 0; i < FIBER_INITIAL_HASH_SIZE; i++) used[i] = 0;
    for (unsigned i = 0; i < FIBER_INITIAL_HASH_SIZE; i++)
        used[fiber_block_hash((addr_t) i << PAGE_BITS, FIBER_INITIAL_HASH_SIZE)]++;
    for (unsigned i = 0; i < FIBER_INITIAL_HASH_SIZE; i++) assert(used[i] == 1);
    for (size_t size = 1; size <= 65536; size *= 2)
        for (addr_t pc = (addr_t) -4096; pc != 0; pc += 4)
            assert(fiber_block_hash(pc, size) < size);

    struct asbestos *a = asbestos_new(NULL);
    assert(a);
    // Populate using the same helper as actual insertion/lookup/rehash, then
    // verify that the real disconnect/reclamation code removes every block.
    for (unsigned i = 0; i < 4096; i++) {
        struct fiber_block *b = calloc(1, sizeof(*b));
        assert(b);
        b->addr = (addr_t) 0x100000000 + i * 4;
        b->end_addr = b->addr + 3;
        b->used = 16;
        list_init(&b->jumps_from[0]); list_init(&b->jumps_from[1]);
        list_init_add(&a->hash[fiber_block_hash(b->addr, a->hash_size)], &b->chain);
        list_init_add(&a->page_hash[PAGE(b->addr) % FIBER_PAGE_HASH_SIZE].blocks[0], &b->page[0]);
        a->num_blocks++; a->mem_used += b->used;
    }
    assert(a->num_blocks == 4096 && a->mem_used == 65536);
    asbestos_invalidate_all(a);
    assert(a->num_blocks == 0 && a->mem_used == 0);
    assert(atomic_load(&a->invalidate_gen) == 1);
    for (size_t i = 0; i < a->hash_size; i++) assert(list_empty(&a->hash[i]));
    asbestos_free(a);
    puts("block-hash-ok");
}
