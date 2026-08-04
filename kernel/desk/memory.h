/* desk/memory.h — slab allocator */
#pragma once
#include "../boot/types.h"
#include "../sys/serial.h"

#define HEAP_SIZE (2*1024*1024)  // 2MB heap
#define MIN_BLOCK 16
#define NUM_SLABS 7  // 16,32,64,128,256,512,1024

struct Slab {
    u32 block_size;
    u8 *start, *end;
    u8 *free_list;  // linked list of free blocks
    u32 total, free;
};

static u8 heap[HEAP_SIZE] __attribute__((aligned(16)));
static u8 *heap_ptr = heap;
static Slab slabs[NUM_SLABS];
static const u32 slab_sizes[NUM_SLABS] = {16,32,64,128,256,512,1024};

static void mem_init() {
    for(int i=0;i<NUM_SLABS;i++){
        slabs[i].block_size = slab_sizes[i];
        slabs[i].total = 0;
        slabs[i].free = 0;
    }
    out_str("[MEM] slab allocator ready\n");
}

// Find which slab to use for given size
static int slab_index(u32 size) {
    for(int i=0;i<NUM_SLABS;i++) if(size<=slab_sizes[i]) return i;
    return -1; // too big
}

// Grow a slab: allocate more blocks from heap
static void slab_grow(Slab *s) {
    u32 bs = s->block_size;
    u32 count = 64; // add 64 blocks at a time
    u32 needed = bs * count;
    if(heap_ptr + needed > heap + HEAP_SIZE) return;
    u8 *blk = heap_ptr;
    heap_ptr += needed;
    s->start = blk;
    s->end = blk + needed;
    s->total += count;
    s->free += count;
    // Build free list (linked through first 8 bytes of each block)
    for(u32 i=0;i<count;i++){
        u8 *b = blk + i*bs;
        *(u64*)b = (i<count-1) ? (u64)(b+bs) : 0; // next pointer
    }
    s->free_list = blk;
}

void *kmalloc(u32 size) {
    int si = slab_index(size);
    if(si<0) return 0; // too big for slab
    Slab *s = &slabs[si];
    if(!s->free_list) slab_grow(s); // grow if empty
    if(!s->free_list) return 0; // out of memory
    u8 *blk = s->free_list;
    s->free_list = (u8*)*(u64*)blk; // pop from free list
    s->free--;
    return blk;
}

void kfree(void *ptr) {
    if(!ptr) return;
    // Find which slab this pointer belongs to (linear scan)
    for(int i=0;i<NUM_SLABS;i++){
        Slab *s = &slabs[i];
        if((u8*)ptr >= s->start && (u8*)ptr < s->end){
            u32 bs = s->block_size;
            if(((u8*)ptr - s->start) % bs != 0) return; // misaligned
            // Push back to free list
            *(u64*)ptr = (u64)s->free_list;
            s->free_list = (u8*)ptr;
            s->free++;
            return;
        }
    }
}
