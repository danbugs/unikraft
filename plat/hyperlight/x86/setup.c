/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2024, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

/*
 * Platform setup for Hyperlight.
 *
 * hyperlight_entry() is the first C function called (from lcpu_start.S).
 * It initialises the console, parses the PEB to register memory, and
 * hands off to _ukplat_entry() for standard Unikraft boot.
 *
 * _ukplat_entry() follows the same pattern as plat/kvm: LCPU init,
 * early init, IRQ controller probe, memory init, stack allocation,
 * then jump to uk_boot_entry().
 */

#include <string.h>
#include <uk/arch/limits.h>
#include <uk/arch/types.h>
#include <uk/arch/x86_64.h>
#include <uk/asm/cfi.h>
#include <uk/boot.h>
#include <uk/assert.h>
#include <uk/essentials.h>
#include <uk/intctlr.h>
#include <uk/lcpu.h>
#include <uk/paging.h>
#include <uk/plat/memory.h>
#include <uk/plat/common/sections.h>
#include <uk/plat/common/bootinfo.h>

#include <hyperlight-x86/peb.h>
#include <hyperlight-x86/setup.h>

/* Console init provided by console.c */
void _ukplat_init_console(void);

/* Entry args saved by entry64.S into .data */
extern struct hyperlight_entry_args hyperlight_entry_args;

/* Global PEB pointer — other modules may need it later */
static struct hyperlight_peb *g_peb;

/* TODO: Support passing a command line from the host wrapper. */
static const char hyperlight_cmdline[] = "unikraft-hyperlight";

/**
 * Register PEB memory regions in bootinfo.
 *
 * The PEB's guest_heap region is registered as FREE memory so that
 * Unikraft's allocator can use it.  The input/output stacks are for
 * the FlatBuffer-based host-call protocol and are not general memory.
 */
static void hyperlight_init_mem(struct ukplat_bootinfo *bi,
				struct hyperlight_peb *peb)
{
	struct ukplat_memregion_desc mrd = {0};
	int rc;

	if (peb->guest_heap.size > 0) {
		mrd.pbase = peb->guest_heap.ptr;
		mrd.vbase = mrd.pbase; /* identity-mapped */
		mrd.pg_off = 0;
		mrd.len = peb->guest_heap.size;
		mrd.pg_count = mrd.len / __PAGE_SIZE;
		mrd.type = UKPLAT_MEMRT_FREE;
		mrd.flags = UKPLAT_MEMRF_READ | UKPLAT_MEMRF_WRITE;
#ifdef CONFIG_UKPLAT_MEMRNAME
		memcpy(mrd.name, "heap", sizeof("heap"));
#endif
		rc = ukplat_memregion_list_insert(&bi->mrds, &mrd);
		if (unlikely(rc < 0))
			UK_CRASH("Unable to add heap region\n");
	}

}

static void __noreturn ukplat_entry2(void *arg __unused)
{
	ukarch_cfi_unwind_end();
	uk_boot_entry();
	UK_BUG(); /* noreturn */
}

/*
 * TODO: Pre-fault IST exception stacks before LCPU init so that
 * the GDT/TSS/IDT setup (which writes to BSS) doesn't fault on
 * CoW pages after the asm handler is replaced by the C trap handler.
 * For now, the asm CoW handler from entry64.S handles these faults.
 */
static void _ukplat_entry(struct ukplat_bootinfo *bi)
{
	void *bstack;
	int rc;

	/* Initialize LCPU of bootstrap processor.
	 * This installs the native PAL's full IDT, replacing the
	 * minimal 1-entry IDT from entry64.S.
	 */
	rc = uk_lcpu_init(uk_pcpuvar_current_ptr_get(uk_lcpus));
	if (unlikely(rc))
		UK_CRASH("Bootstrap processor init failed: %d\n", rc);

	/* Switch from asm CoW handler to C event-based handler.
	 * Must be after uk_lcpu_init() since page faults now go
	 * through the native PAL's event system.
	 */
	{
		extern void hyperlight_cow_init(void);

		hyperlight_cow_init();
	}

	/* Execute early init */
	uk_boot_early_init(bi);

	/* Initialize IRQ controller */
	rc = uk_intctlr_probe();
	if (unlikely(rc))
		UK_CRASH("Interrupt controller init failed: %d\n", rc);

	/* Initialize memory */
	rc = ukplat_mem_init();
	if (unlikely(rc))
		UK_CRASH("Mem init failed: %d\n", rc);

	/* Allocate boot stack */
	bstack = ukplat_memregion_alloc(__STACK_SIZE, UKPLAT_MEMRT_STACK,
					UKPLAT_MEMRF_READ |
					UKPLAT_MEMRF_WRITE);
	if (unlikely(!bstack))
		UK_CRASH("Boot stack alloc failed\n");

	bstack = (void *)((__uptr)bstack + __STACK_SIZE);

	/*
	 * Pre-fault the new boot stack for CoW before switching.
	 * Without this, the first push after switching RSP would
	 * fault on a read-only CoW page.
	 */
	{
		volatile __u8 *sp = (volatile __u8 *)bstack;
		__u8 tmp = *(sp - 8);

		*(volatile __u8 *)(sp - 8) = tmp;
	}

	/* Switch away from the bootstrap stack */
	uk_pr_info("Switch from bootstrap stack to stack @%p\n", bstack);
	uk_arch_x86_64_jump_to((__u64)bstack, (__u64)ukplat_entry2);
}

/**
 * C entry point called from lcpu_start.S.
 *
 * @param lcpu       Pointer to bootstrap LCPU structure
 * @param entry_args Pointer to hyperlight_entry_args in .data
 */
void hyperlight_entry(struct uk_lcpu *lcpu __unused,
		      struct hyperlight_entry_args *entry_args)
{
	struct ukplat_bootinfo *bi;

	/* Initialize console so uk_pr_* works */
	_ukplat_init_console();

	/* Store PEB globally */
	g_peb = (struct hyperlight_peb *)entry_args->peb_address;
	if (unlikely(!g_peb))
		UK_CRASH("PEB address is NULL\n");

	uk_pr_info("Hyperlight: PEB @ %p\n", g_peb);
	uk_pr_info("  heap:  ptr=0x%lx size=0x%lx\n",
		   (unsigned long)g_peb->guest_heap.ptr,
		   (unsigned long)g_peb->guest_heap.size);

	/* Get bootinfo (pre-populated by linker via bootinfo.lds.S) */
	bi = ukplat_bootinfo_get();
	if (unlikely(!bi))
		UK_CRASH("Incompatible or corrupted bootinfo\n");

	/* Register PEB memory regions */
	hyperlight_init_mem(bi, g_peb);

	/* Set boot protocol and command line */
	memcpy(bi->bootprotocol, "hyperlight", sizeof("hyperlight"));
	bi->cmdline = (__u64)hyperlight_cmdline;
	bi->cmdline_len = sizeof(hyperlight_cmdline) - 1;

	_ukplat_entry(bi);
}
