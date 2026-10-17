/* Kernel conformance: ControlMemory (heap and linear), QueryMemory and memory blocks. The
 * addresses are what Azahar hands out for an ELF program (kernel version 0x23A: linear memory
 * from 0x30000000, memory mode Dev1). Our own program, no game code. */
#include "svc.h"

#define HEAP 0x08000000u
#define MEMOP_FREE 1u
#define MEMOP_COMMIT 3u
#define MEMOP_LINEAR 0x10000u
#define RW 3u
#define DONT_CARE 0x10000000u

static void rc_line(const char *name, s32 rc) { check(name, (u32)rc); }

static char name_buf[48];
static const char *cat(const char *a, const char *b) {
    u32 n = 0;
    for (u32 i = 0; a[i]; i++) name_buf[n++] = a[i];
    for (u32 i = 0; b[i]; i++) name_buf[n++] = b[i];
    name_buf[n] = 0;
    return name_buf;
}

/* base, size, permission, state of the region holding addr; with `contains`, the base and size
   (which depend on how big this program's image is) are replaced by whether the region holds addr */
static void query_as(const char *name, u32 addr, int contains) {
    u32 q[5] = {0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA};
    s32 rc = svcQueryMemory(q, addr);
    rc_line(cat(name, ".rc"), rc);
    if (rc != 0) return;
    if (contains) {
        check(cat(name, ".contains"), addr - q[0] < q[1]);
    } else {
        check(cat(name, ".base"), q[0]);
        check(cat(name, ".size"), q[1]);
    }
    check(cat(name, ".perm"), q[2]);
    check(cat(name, ".state"), q[3]);
    check(cat(name, ".flags"), q[4]);
}

static void query(const char *name, u32 addr) { query_as(name, addr, 0); }
static void query_in(const char *name, u32 addr) { query_as(name, addr, 1); }

static u8 data_page[0x1000] __attribute__((aligned(0x1000)));
static const u8 rodata_page[16] = {1, 2, 3};

static void heap_checks(void) {
    u32 out = 0xAAAAAAAA;
    rc_line("heap.alloc_rc", svcControlMemory(&out, HEAP, 0, 0x4000, MEMOP_COMMIT, RW));
    check("heap.alloc_addr", out);
    volatile u32 *p = (volatile u32 *)out;
    p[0] = 0x12345678;
    p[0x3FFF / 4] = 0x9ABCDEF0;
    check("heap.readback", p[0] ^ p[0x3FFF / 4]);
    query("heap.query", HEAP);
    query("heap.query_inside", HEAP + 0x2000);
    query("heap.query_after_end", HEAP + 0x4000);
    out = 0xAAAAAAAA;
    rc_line("heap.alloc_again_rc", svcControlMemory(&out, HEAP, 0, 0x1000, MEMOP_COMMIT, RW));
    out = 0xAAAAAAAA;
    rc_line("heap.alloc_next_rc", svcControlMemory(&out, HEAP + 0x4000, 0, 0x2000, MEMOP_COMMIT, RW));
    check("heap.alloc_next_addr", out);
    query("heap.query_both", HEAP);
    out = 0xAAAAAAAA;
    rc_line("heap.free_part_rc", svcControlMemory(&out, HEAP + 0x1000, 0, 0x1000, MEMOP_FREE, 0));
    check("heap.free_part_addr", out);
    query("heap.query_hole", HEAP + 0x1000);
    query("heap.query_before_hole", HEAP);
    rc_line("heap.free_hole_again_rc", svcControlMemory(&out, HEAP + 0x1000, 0, 0x1000, MEMOP_FREE, 0));
    rc_line("heap.free_rest_rc", svcControlMemory(&out, HEAP, 0, 0x1000, MEMOP_FREE, 0));
    rc_line("heap.free_tail_rc", svcControlMemory(&out, HEAP + 0x2000, 0, 0x4000, MEMOP_FREE, 0));
    query_in("heap.query_freed", HEAP);

    rc_line("heap.misaligned_addr_rc", svcControlMemory(&out, HEAP + 4, 0, 0x1000, MEMOP_COMMIT, RW));
    rc_line("heap.misaligned_size_rc", svcControlMemory(&out, HEAP, 0, 0x800, MEMOP_COMMIT, RW));
    rc_line("heap.bad_perm_rc", svcControlMemory(&out, HEAP, 0, 0x1000, MEMOP_COMMIT, 0x10));
    rc_line("heap.exec_perm_rc", svcControlMemory(&out, HEAP, 0, 0x1000, MEMOP_COMMIT, 7));
    rc_line("heap.bad_op_rc", svcControlMemory(&out, HEAP, 0, 0x1000, 0x99, RW));
    rc_line("heap.free_unmapped_rc", svcControlMemory(&out, 0x20000000, 0, 0x1000, MEMOP_FREE, 0));
}

static void linear_checks(void) {
    u32 a = 0xAAAAAAAA, b = 0xAAAAAAAA, c = 0xAAAAAAAA;
    rc_line("linear.alloc1_rc", svcControlMemory(&a, 0, 0, 0x2000, MEMOP_COMMIT | MEMOP_LINEAR, RW));
    check("linear.alloc1_addr", a);
    rc_line("linear.alloc2_rc", svcControlMemory(&b, 0, 0, 0x1000, MEMOP_COMMIT | MEMOP_LINEAR, RW));
    check("linear.alloc2_addr", b);
    volatile u32 *p = (volatile u32 *)a;
    p[0] = 0xCAFE0001;
    p[0x1FFC / 4] = 0xCAFE0002;
    check("linear.readback", p[0] + p[0x1FFC / 4]);
    query("linear.query", a);
    query("linear.query_second", b);
    rc_line("linear.free1_rc", svcControlMemory(&c, a, 0, 0x2000, MEMOP_FREE | MEMOP_LINEAR, 0));
    check("linear.free1_addr", c);
    query("linear.query_freed", a);
    rc_line("linear.alloc3_rc", svcControlMemory(&c, 0, 0, 0x1000, MEMOP_COMMIT | MEMOP_LINEAR, RW));
    check("linear.alloc3_addr", c);       /* reuses the freed space */
    rc_line("linear.alloc_big_rc", svcControlMemory(&c, 0, 0, 0x1000000, MEMOP_COMMIT | MEMOP_LINEAR, RW));
    check("linear.alloc_big_addr", c);
    rc_line("linear.alloc_huge_rc", svcControlMemory(&c, 0, 0, 0x40000000, MEMOP_COMMIT | MEMOP_LINEAR, RW));
    rc_line("linear.free_big_rc", svcControlMemory(&c, a + 0x2000 + 0x1000, 0, 0x1000, MEMOP_FREE | MEMOP_LINEAR, 0));
    rc_line("linear.free_unallocated_rc", svcControlMemory(&c, 0x30F00000, 0, 0x1000, MEMOP_FREE | MEMOP_LINEAR, 0));
}

static void query_checks(void) {
    u32 stack_word = 0;
    query_in("q.code", 0x00100000);
    query_in("q.code_inside", 0x00100010);
    query_in("q.rodata", (u32)rodata_page);
    query_in("q.data", (u32)data_page);
    query("q.stack", (u32)&stack_word);
    query("q.page_zero", 0);
    query("q.low_gap", 0x00001000);
    query("q.before_code", 0x000FF000);
    query("q.far", 0x60000000);
    query("q.tls", (u32)tls());
    query("q.config_mem", 0x1FF80000);
    query("q.shared_page", 0x1FF81000);
    query("q.end", 0xFFFFF000);
}

static void block_checks(void) {
    Handle h = 0, h2 = 0;
    u32 out = 0;
    svcControlMemory(&out, HEAP, 0, 0x4000, MEMOP_COMMIT, RW);

    rc_line("block.create_zero_rc", svcCreateMemoryBlock(&h, 0, 0x1000, RW, RW));
    check("block.create_zero_handle_set", h != 0);
    rc_line("block.create_bad_size_rc", svcCreateMemoryBlock(&h2, 0, 0x800, RW, RW));
    rc_line("block.create_bad_perm_rc", svcCreateMemoryBlock(&h2, 0, 0x1000, 5, RW));
    rc_line("block.create_bad_other_perm_rc", svcCreateMemoryBlock(&h2, 0, 0x1000, RW, 7));
    rc_line("block.create_dont_care_rc", svcCreateMemoryBlock(&h2, 0, 0x1000, RW, 0x10000000));
    if (h2) svcCloseHandle(h2);
    h2 = 0;
    rc_line("block.create_outside_rc", svcCreateMemoryBlock(&h2, 0x20000000, 0x1000, RW, RW));
    rc_line("block.create_low_rc", svcCreateMemoryBlock(&h2, 0x00001000, 0x1000, RW, RW));

    const u32 where = 0x10000000;
    rc_line("block.map_rc", svcMapMemoryBlock(h, where, RW, DONT_CARE));
    volatile u32 *p = (volatile u32 *)where;
    p[0] = 0x0BADF00D;
    check("block.map_readback", p[0]);
    query("block.map_query", where);
    rc_line("block.map_again_rc", svcMapMemoryBlock(h, where + 0x1000, RW, DONT_CARE));
    rc_line("block.unmap_rc", svcUnmapMemoryBlock(h, where));
    query("block.unmap_query", where);
    rc_line("block.unmap_again_rc", svcUnmapMemoryBlock(h, where));
    rc_line("block.map_misaligned_rc", svcMapMemoryBlock(h, where + 4, RW, DONT_CARE));
    rc_line("block.map_remap_rc", svcMapMemoryBlock(h, where, RW, DONT_CARE));
    check("block.map_remap_kept", ((volatile u32 *)where)[0]);   /* the block keeps its contents */
    svcUnmapMemoryBlock(h, where);
    rc_line("block.map_rw_for_auto_rc", svcMapMemoryBlock(h, where, RW, RW));
    rc_line("block.map_read_only_rc", svcMapMemoryBlock(h, where, 1, DONT_CARE));
    query("block.map_read_only_query", where);
    svcUnmapMemoryBlock(h, where);
    rc_line("block.map_bad_handle_rc", svcMapMemoryBlock(0x12345678, where, RW, RW));
    rc_line("block.close_rc", svcCloseHandle(h));

    /* a block over the program's own heap */
    ((volatile u32 *)HEAP)[0] = 0x600DF00D;
    Handle own = 0;
    rc_line("block.own_create_rc", svcCreateMemoryBlock(&own, HEAP, 0x1000, RW, RW));
    rc_line("block.own_map_dont_care_rc", svcMapMemoryBlock(own, where, RW, DONT_CARE));
    rc_line("block.own_map_rc", svcMapMemoryBlock(own, where, RW, RW));
    check("block.own_alias", ((volatile u32 *)where)[0]);
    ((volatile u32 *)where)[1] = 0x0A11A5ED;
    check("block.own_alias_write", ((volatile u32 *)HEAP)[1]);
    query("block.own_query", where);
    rc_line("block.own_unmap_rc", svcUnmapMemoryBlock(own, where));
    rc_line("block.own_close_rc", svcCloseHandle(own));
    svcControlMemory(&out, HEAP, 0, 0x4000, MEMOP_FREE, 0);
}

void _start(void) __attribute__((section(".text.start")));
void _start(void) {
    print("conf_mem: start");
    heap_checks();
    linear_checks();
    query_checks();
    block_checks();
    finish("conf_mem: done");
}
