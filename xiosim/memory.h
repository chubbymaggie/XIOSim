/*
 * Handling of virtual-to-phisycal memory translation.
 * Copyright, Svilen Kanev, 2014
*/

#ifndef MEMORY_H
#define MEMORY_H

#include "host.h"
#include "stats.h"

struct stat_sdb_t;

namespace xiosim {
namespace memory {

/* assuming 4KB pages */
const md_addr_t PAGE_SIZE = 4096;
const md_addr_t PAGE_SHIFT = 12; // log2(4K)
const md_addr_t PAGE_MASK = PAGE_SIZE - 1;

/* special address space id to indicate an already-translated address */
const int DO_NOT_TRANSLATE = -1;

/* initialize memory system  */
void init(int num_processes);

/* clean up */
void deinit();

/* register memory system-specific statistics */
void reg_stats(xiosim::stats::StatsDatabase* sdb);

/* map each (address-space-id,virtual-address) pair to a simulated physical address */
md_paddr_t v2p_translate(int asid, md_addr_t addr);

/* notify the virtual memory system of a non-speculative write.
 * This will allocate a new page if the page was unmapped. */
void notify_write(int asid, md_addr_t addr);

/* allocate physical pages that the simulated application requested */
void notify_mmap(int asid, md_addr_t addr, size_t length, bool mod_brk);

/* de-allocate physical pages released by the simulated application */
void notify_munmap(int asid, md_addr_t addr, size_t length, bool mod_brk);

/* update the brk point in address space @asid */
void update_brk(int asid, md_addr_t brk_end, bool do_mmap);

/* allocate physical pages for the stack of a current thread */
void map_stack(int asid, md_addr_t sp, md_addr_t bos);

/* Round @addr up to the nearest page. */
inline md_addr_t page_round_up(const md_addr_t addr) {
    return (addr + PAGE_MASK) & ~PAGE_MASK;
}

/* Round @addr down to the nearest page. */
inline md_addr_t page_round_down(const md_addr_t addr) {
    return addr & ~PAGE_MASK;
}

/* Get the offset of @addr in a page. */
inline md_addr_t page_offset(const md_addr_t addr) {
    return addr & PAGE_MASK;
}

/* This maps a virtual address to a low address where we're
 * pretending that the page table for this process is stored.
 * Shifting by PAGE_SHIFT yields the virtual page number.
 * We don't simulate the full multi-level table, so we (somewhat arbitrarily)
 * assume 1MB per process table.
 * XXX: The real address would be multiplied by the size of a PTE
 * but then our TLBs's banking hash function would need to discard these
 * bits, or all TLB requests just go to bank 0.
 * So, to save ourselves from making our TLB implementation diverge more
 * from caches, we don't do the multiply.
 */
inline md_addr_t page_table_address(const int asid, const md_addr_t addr) {
    const md_addr_t PAGE_TABLE_SIZE = (1 << 20);
    return (addr >> PAGE_SHIFT) + PAGE_TABLE_SIZE * (asid + 1);
}

}
}

#endif /* MEMORY_H */
