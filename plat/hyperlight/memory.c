/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2024, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

/*
 * Hyperlight memory initialisation.
 *
 * _ukplat_mem_mappings_init() is a no-op: the host sets up all
 * page-table mappings before guest entry.
 *
 * When CONFIG_LIBUKPAGING is enabled, ukplat_mem_init() allocates a
 * block of scratch memory for the frame allocator and adopts the
 * host's existing page tables.  Scratch memory is EPT-writable, so
 * page table frames allocated from it can be modified by both the CPU
 * page-table walker and guest code — unlike the PEB heap which sits
 * in the read-only snapshot EPT slot.
 */

#include <uk/config.h>

/*
 * Called by plat/common/memory.c during ukplat_memallocator_set().
 * Hyperlight sets up page tables on the host side before guest entry,
 * so there are no guest-side mappings to initialise.
 */
int _ukplat_mem_mappings_init(void)
{
	return 0;
}

/*
 * Overrides the weak ukplat_mem_init() from plat/common/memory.c.
 *
 * When paging is disabled, this is a simple no-op — the host already
 * set up all mappings, and the boot code will use the identity-mapped
 * FREE regions directly for the buddy allocator.
 *
 * When paging IS enabled, the version below allocates scratch memory
 * for the frame allocator and adopts the host's existing page tables.
 */
#if !CONFIG_LIBUKPAGING

int ukplat_mem_init(void)
{
	return 0;
}

#else /* CONFIG_LIBUKPAGING */

#include <uk/arch/limits.h>
#include <uk/assert.h>
#include <uk/essentials.h>
#include <uk/paging.h>
#include <uk/paging/arch.h>
#include <uk/plat/memory.h>
#include <uk/plat/common/bootinfo.h>
#include <uk/print.h>

#include <hyperlight-x86/hcall.h>
#include <hyperlight-x86/mem.h>

static struct uk_pagetable hyperlight_pt;

static inline __paddr_t read_cr3(void)
{
	__paddr_t val;

	__asm__ volatile("mov %%cr3, %0" : "=r"(val));
	return val & ~0xFFFULL;
}

/*
 * Determine how much scratch memory to give the paging frame allocator.
 *
 * 1. Ask the host via GetPagingBudget (explicit control).
 * 2. If the host doesn't register it, compute dynamically:
 *    use 75% of remaining scratch, leaving 25% for CoW faults.
 */
static __sz hl_paging_fa_size(void)
{
	__u64 budget;
	__u64 bump_pos, max_avail, available;

	/* Try host function first */
	budget = hl_call_get_paging_budget();
	if (budget > 0)
		return (__sz)(budget & ~((__u64)__PAGE_SIZE - 1));

	/* Dynamic fallback: read bump pointer, compute remaining */
	bump_pos = *(volatile __u64 *)HL_SCRATCH_ALLOC_GVA;
	max_avail = HL_MAX_GPA + 1 - 2 * __PAGE_SIZE; /* 2 reserved pages */
	if (bump_pos >= max_avail)
		return 0;

	available = max_avail - bump_pos;
	return (__sz)((available * 3 / 4) & ~((__u64)__PAGE_SIZE - 1));
}

/*
 * Overrides the weak ukplat_mem_init() from plat/common/memory.c.
 *
 * Hyperlight's memory has two EPT-backed regions:
 *  - Snapshot (low GPAs): code + PEB heap — EPT read-only
 *  - Scratch  (high GPAs): I/O + CoW pages — EPT writable
 *
 * Standard Unikraft feeds the PEB heap (FREE regions) to the frame
 * allocator, but those GPAs are EPT read-only.  Writing page table
 * entries backed by heap GPAs crashes with MmioWriteUnmapped.
 *
 * Instead, we allocate a block of scratch memory via the bump
 * allocator (hl_scratch_alloc_pages) and feed THAT to the frame
 * allocator.  Since scratch is EPT-writable, page table frames and
 * mapped-page frames are all writable — no CoW aliasing issues.
 *
 * The FREE heap regions are NOT consumed here.  With paging enabled,
 * boot.c uses HEAP_BASE to map the heap via uk_paging_page_map(),
 * backed entirely by scratch-allocated frames.
 */
int ukplat_mem_init(void)
{
	struct ukplat_memregion_desc *mrd;
	__paddr_t scratch_block;
	__paddr_t cr3;
	__sz fa_size;
	__u64 fa_pages;
	int rc;

	/*
	 * Determine how much scratch to give the frame allocator.
	 * The host can control this via GetPagingBudget; otherwise
	 * we use 75% of remaining scratch.
	 */
	fa_size = hl_paging_fa_size();
	if (unlikely(fa_size < 16 * __PAGE_SIZE)) {
		uk_pr_err("Not enough scratch for paging (%lu bytes)\n",
			  (unsigned long)fa_size);
		return -ENOMEM;
	}

	fa_pages = fa_size / __PAGE_SIZE;
	scratch_block = hl_scratch_alloc_pages(fa_pages);

	uk_pr_info("Paging frame allocator: %lu pages (%lu KiB) from scratch GPA %lx\n",
		   (unsigned long)fa_pages,
		   (unsigned long)(fa_size >> 10),
		   (unsigned long)scratch_block);

	/*
	 * Initialise the frame allocator with the scratch block.
	 * uk_paging_pt_init() also does one-time arch init and allocates
	 * a dummy PML4 (which we discard below in favour of the host's).
	 */
	rc = uk_paging_pt_init(&hyperlight_pt, scratch_block, fa_size);
	if (unlikely(rc)) {
		uk_pr_err("uk_paging_pt_init failed: %d\n", rc);
		return rc;
	}

	/*
	 * Mark the PEB heap FREE regions as consumed so boot code doesn't
	 * try to use them for the buddy allocator — with HEAP_BASE
	 * enabled, the heap is mapped via paging, not identity-mapped.
	 */
	ukplat_memregion_foreach(&mrd, UKPLAT_MEMRT_FREE, 0, 0) {
		mrd->flags &= ~UKPLAT_MEMRF_PERMS;
	}

	/*
	 * Adopt the host's existing page tables.  The PML4 is in the
	 * scratch region; pgarch_directmap_paddr_to_vaddr converts
	 * the scratch GPA from CR3 to its GVA.
	 */
	cr3 = read_cr3();
	hyperlight_pt.pt_pbase = cr3;
	hyperlight_pt.pt_vbase = pgarch_directmap_paddr_to_vaddr(cr3);

	uk_pr_info("Adopting host page tables: CR3=%lx vbase=%lx\n",
		   (unsigned long)cr3,
		   (unsigned long)hyperlight_pt.pt_vbase);

	/*
	 * Set as active.  Writing the same CR3 just flushes the TLB —
	 * no page table switch.
	 */
	rc = uk_paging_pt_set_active(&hyperlight_pt);
	if (unlikely(rc))
		return rc;

	/*
	 * If the host mapped an initrd via map_file_cow, create
	 * first-stage page table entries for it.  The EPT already
	 * covers these GPAs (the host set that up), but the guest's
	 * CR3 page tables don't — they only cover the snapshot and
	 * scratch regions.  We identity-map the initrd so VA = GPA.
	 */
	ukplat_memregion_foreach(&mrd, UKPLAT_MEMRT_INITRD, 0, 0) {
		unsigned long pages = mrd->pg_count;

		rc = uk_paging_page_map(&hyperlight_pt,
					mrd->vbase, mrd->pbase, pages,
					UK_PAGING_PAGE_ATTR_PROT_READ, 0);
		if (unlikely(rc)) {
			uk_pr_err("Failed to map initrd at %lx: %d\n",
				  (unsigned long)mrd->vbase, rc);
			return rc;
		}

		uk_pr_info("Mapped initrd: %lu pages at VA %lx → GPA %lx\n",
			   pages, (unsigned long)mrd->vbase,
			   (unsigned long)mrd->pbase);
	}

	return 0;
}

#endif /* CONFIG_LIBUKPAGING */
