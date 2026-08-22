/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2024, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

/*
 * Hyperlight shutdown / power-management ops.
 *
 * Halt: writes to port 108 with the dispatch function address in EAX.
 * The host intercepts this VM exit to know the guest has finished
 * initialisation and is ready to receive function calls.
 *
 * RAX holds the address of hyperlight_dispatch_function — the host
 * captures this during evolve() and uses it as the RIP for subsequent
 * guest function calls via MultiUseSandbox::call().
 */

#include <uk/boot/earlytab.h>
#include <uk/pm.h>
#include <uk/prio.h>
#include <uk/plat/common/bootinfo.h>

#include <hyperlight-x86/outb.h>

/* Provided by dispatch.c */
extern void hyperlight_dispatch_function(void)
	__attribute__((noreturn));

static int hyperlight_shutdown(void)
{
	/*
	 * Halt via port 108.  The host intercepts this VM exit.
	 * RAX = dispatch function address — the host stores this
	 * and sets RIP here for each call() after evolve().
	 * cli + hlt after outl is a backstop — the VM exit from
	 * outl already stops the vCPU.
	 */
	__asm__ volatile(
		/* Hyperlight checks RSP alignment after halt */
		"andq $~0xf, %%rsp\n\t"
		"movq %0, %%rax\n\t"
		"movw $108, %%dx\n\t"
		"outl %%eax, %%dx\n\t"
		"cli\n\t"
		"hlt\n\t"
		: : "r"((__u64)hyperlight_dispatch_function) : "rax", "rdx"
	);
	__builtin_unreachable();
}

static int hyperlight_crash(void)
{
	hyperlight_out32(102, 0xFF); /* port 102 = Abort */
	__builtin_unreachable();
}

static const struct uk_pm_ops hyperlight_pm_ops = {
	.syshalt    = hyperlight_shutdown,
	.sysrestart = hyperlight_shutdown,
	.syscrash   = hyperlight_crash,
};

static int hyperlight_register_pm_ops(struct ukplat_bootinfo __unused *bi)
{
	return uk_pm_ops_register(&hyperlight_pm_ops);
}

UK_BOOT_EARLYTAB_ENTRY(hyperlight_register_pm_ops, UK_PRIO_EARLIEST);
