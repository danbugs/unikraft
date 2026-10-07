/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2024, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

/*
 * Guest function dispatch for Hyperlight.
 *
 * After the initial boot (evolve), the guest halts with the address
 * of hyperlight_dispatch_function in RAX (x0 on arm64).  The host can
 * then invoke guest functions by pushing a FunctionCall FlatBuffer onto
 * the PEB input stack, setting RIP (PC) to that address, and running the
 * vCPU.
 *
 * The dispatch handler pops the FunctionCall from the input stack,
 * hands it to the cooperative step pump (step.c), pushes the entry's
 * Yield as its Vec<u8> result onto the output stack, and halts via port
 * 108.
 *
 * The pump is the sole consumer of guest function calls: it
 * runs the scheduler until it would go idle and hands the vCPU back with
 * a next-wakeup deadline.  Named calls (anything but `step`) are queued
 * on /dev/hlcall for a driver blocked in read() on it, so the workload
 * serves them on its own thread rather than on this dispatch stack.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uk/arch/types.h>
#include <uk/arch/limits.h>
#if defined(__x86_64__)
#include <uk/arch/x86_64.h>
#elif defined(__aarch64__)
#include <uk/plat/config.h>
#endif
#include <uk/alloc.h>
#include <uk/assert.h>
#include <uk/init.h>
#include <uk/print.h>
#include <uk/plat/native/except.h>

#include <hyperlight/dispatch.h>
#include <hyperlight/hcall.h>
#include <hyperlight/mem.h>
#include <hyperlight/outb.h>
#include <hyperlight/peb.h>
#include <hyperlight/step.h>

/* ── PEB I/O stack pointers (cached from PEB at init) ─────────── */

static volatile __u8 *g_input_stack;
static __u64 g_input_stack_size;
static volatile __u8 *g_output_stack;
static __u64 g_output_stack_size;
static int g_dispatch_ready;

/*
 * Scratch exception stack top GVA — queried from the host at init
 * via GetExnStackTop.  The host reads the value from hyperlight-common
 * so the guest does not hardcode layout constants.
 */
__u64 g_exn_stack_top;

/* ── Host-provided environment variables ─────────────────────────── */

/*
 * Bring the host-provided environment into the process: GetEnvVars
 * returns KEY=VALUE entries separated by NUL, and each is setenv()ed
 * over whatever was there.  The buffer is sized from the PEB on first
 * use -- the entries arrive on the input stack, so hl_hcall_max_payload()
 * bytes always hold them -- and kept, since this runs often.
 *
 * Runs as a late initcall, after posix-environ and before the ELF
 * loader's main(), so the loaded program starts with the variables; and
 * again ahead of every named call (hyperlight_dispatch_inner), since the
 * initcall does not re-run after a snapshot restore and that is how the
 * variables the new host set get in.  A loaded program has a libc
 * environ of its own that this cannot reach; drivers refresh it through
 * the HLCALL_IOC_GETENV ioctl (step.c).
 */
/* setenv() each KEY=VALUE entry of @buf (@len bytes, NUL-separated). */
static void hyperlight_dispatch_setenv(char *buf, __sz len)
{
	char *p, *end;

	for (p = buf, end = buf + len; p < end; p += strlen(p) + 1) {
		char *eq = strchr(p, '=');

		if (!eq || eq == p)
			continue;
		*eq = '\0';
		setenv(p, eq + 1, 1);
		*eq = '=';
	}
}

#if CONFIG_HYPERLIGHT_STEP
/* Only when the environment changed: the call carries the host's version
 * of it, and the step model keeps the copy it last fetched.
 */
static void hyperlight_dispatch_apply_host_env_version(__u64 version)
{
	const char *buf;
	__sz len;

	if (hyperlight_step_env(version, &buf, &len) == 1 && len)
		hyperlight_dispatch_setenv((char *)buf, len);
}
#endif /* CONFIG_HYPERLIGHT_STEP */

static void hyperlight_dispatch_apply_host_env(void)
{
	static char *buf;
	static __sz cap;
	int len;

	if (!buf) {
		cap = hl_hcall_max_payload();
		buf = cap ? uk_malloc(uk_alloc_get_default(), cap) : NULL;
		if (unlikely(!buf)) {
			uk_pr_err("dispatch: no memory for the host environment\n");
			return;
		}
	}

	len = hl_call_get_env_vars(buf, cap);
	if (len <= 0)
		return; /* none, or no such host function: not an error */
	if (unlikely((__sz)len >= cap)) {
		uk_pr_err("dispatch: host environment (%d bytes) does not fit\n",
			  len);
		return;
	}
	hyperlight_dispatch_setenv(buf, len);
}

static int hyperlight_dispatch_env_init(struct uk_init_ctx *ictx __unused)
{
	hyperlight_dispatch_apply_host_env();
	return 0;
}
uk_late_initcall(hyperlight_dispatch_env_init, 0x0);

/* ── Call size ─────────────────────────────────────────────────── */

/*
 * A FunctionCall arrives on the PEB input stack, so it can never be
 * larger than that stack.  Its size is chosen by the host when it
 * creates the sandbox and read from the PEB at init: it is the one
 * limit on a call, and nothing in the guest guesses it separately.
 * The dispatch copy below is sized from it, and so must be any other
 * guest-side buffer that has to hold a whole call (the step queue, and
 * through /dev/hlcall's ioctl the driver's read buffer).
 */
__u64 hyperlight_dispatch_max_call(void)
{
	return g_input_stack_size;
}

/* ── Public init ──────────────────────────────────────────────── */

void hyperlight_dispatch_init(const struct hyperlight_peb *peb)
{
	g_input_stack = (volatile __u8 *)peb->input_stack.ptr;
	g_input_stack_size = peb->input_stack.size;
	g_output_stack = (volatile __u8 *)peb->output_stack.ptr;
	g_output_stack_size = peb->output_stack.size;

	g_exn_stack_top = hl_call_get_exn_stack_top();
	if (!g_exn_stack_top)
		UK_CRASH("GetExnStackTop host function failed\n");

	g_dispatch_ready = 1;
}

/* ── Dispatch inner ─────────────────────────────────────────────── */

/* The FunctionCallResult hyperlight_dispatch_push_bytes_result() writes,
 * pre-encoded up to the vector's bytes (hyperlight-common 0.17): the root
 * table, a ReturnValueBox sharing its vtable, a ReturnValue of type
 * hlsizeprefixedbuffer (10), and that table, whose size and vector
 * length are patched in.  Every guest function returns a Vec<u8>.
 */
static const __u8 HL_BYTES_RESULT[] = {
	0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, /* size; root at 8 */
	0xf4, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x01, /* vtable 20; box */
	0x0c, 0x00, 0x00, 0x00, 0x08, 0x00, 0x0c, 0x00, /* box at 28; vtable */
	0x07, 0x00, 0x08, 0x00, 0x08, 0x00, 0x00, 0x00, /* ...; box: vtable at 20 */
	0x00, 0x00, 0x00, 0x0a, 0x0c, 0x00, 0x00, 0x00, /* bytes table at 48 */
	0x08, 0x00, 0x0c, 0x00, 0x04, 0x00, 0x08, 0x00, /* its vtable: size, value */
	0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* vtable at 40; size */
	0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* vector at 60; length */
};

static void hl_put_le32(volatile __u8 *p, __u32 v)
{
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}

/* Push a FunctionCallResult that returns @a followed by @b as one
 * Vec<u8>, written in place on the output stack.  -1 if it does not fit.
 */
static int hyperlight_dispatch_push_bytes_result(const __u8 *a, __u64 alen,
						 const __u8 *b, __u64 blen)
{
	__u64 n = alen + blen;
	__u64 total = sizeof(HL_BYTES_RESULT) + ((n + 3) & ~3ULL);
	volatile __u8 *p = hl_stack_reserve(g_output_stack,
					    g_output_stack_size, total);

	if (!p)
		return -1;
	memcpy((void *)p, HL_BYTES_RESULT, sizeof(HL_BYTES_RESULT));
	hl_put_le32(p, (__u32)(total - 4));
	hl_put_le32(p + 52, (__u32)n);
	hl_put_le32(p + 60, (__u32)n);
	if (alen)
		memcpy((void *)(p + 64), a, alen);
	if (blen)
		memcpy((void *)(p + 64 + alen), b, blen);
	for (__u64 i = 64 + n; i < total; i++)
		p[i] = 0;
	hl_stack_commit(g_output_stack, total);
	return 0;
}

/* An empty result: the entry ends without a Yield of its own.  Also
 * pushed by the shutdown path when a step pump is in flight, so the
 * host's entry completes normally when the guest process exits.
 */
void hyperlight_dispatch_push_empty_result(void)
{
	hyperlight_dispatch_push_bytes_result(NULL, 0, NULL, 0);
}

void __attribute__((used))
hyperlight_dispatch_inner(void)
{
	/*
	 * Buffer for the FunctionCall FlatBuffer.
	 *
	 * The pop returns a pointer into the input stack.  Any subsequent
	 * host function call (including uk_pr_err via HostPrint) pushes a
	 * response onto the SAME input stack at the same offset, clobbering
	 * the data.  We must copy it out before doing anything that might
	 * trigger a host call.
	 *
	 * Heap-allocated on the first entry, at the size of the input stack
	 * (see hyperlight_dispatch_max_call): the allocator is up by then,
	 * dispatch is single-threaded (one vCPU), and the dispatch entry
	 * runs on the exception stack, which is too small to hold a call.
	 */
	static __u8 *fc_buf;
	const __u8 *fc_raw;
	__u64 fc_len;

	if (!g_dispatch_ready) {
		uk_pr_err("dispatch: not initialised\n");
		goto push_result;
	}

	/* Pop the FunctionCall from the input stack.
	 * Copy immediately — the pointer is invalidated by any host call.
	 */
	if (hl_stack_pop(g_input_stack, &fc_raw, &fc_len) < 0) {
		uk_pr_err("dispatch: no function call on input stack\n");
		goto push_result;
	}

	if (!fc_buf) {
		fc_buf = uk_malloc(uk_alloc_get_default(), g_input_stack_size);
		if (unlikely(!fc_buf)) {
			uk_pr_err("dispatch: no memory for a %lu-byte call buffer\n",
				  (unsigned long)g_input_stack_size);
			goto push_result;
		}
	}
	if (unlikely(fc_len > g_input_stack_size)) {
		uk_pr_err("dispatch: FunctionCall larger than the input stack (%lu bytes)\n",
			  (unsigned long)fc_len);
		goto push_result;
	}

	memcpy(fc_buf, (const void *)fc_raw, fc_len);

	/*
	 * Bring the environment up to date ahead of a named call: after a
	 * snapshot restore the initcall does not re-run, and this is what
	 * picks up the variables the new host set.  Skipped for the pump's
	 * own `step` entry: it carries no workload of its own and can run
	 * thousands of times a second.
	 */
#if CONFIG_HYPERLIGHT_STEP
	/* A named call carries the host's environment version: the kernel
	 * fetches only when it changed.  `resume` skips it: the host gives
	 * the first call after a restore a new version.
	 */
	if (!hyperlight_step_fc_is_pump(fc_buf, fc_len) &&
	    !hyperlight_step_fc_is_resume(fc_buf, fc_len))
		hyperlight_dispatch_apply_host_env_version(
			hyperlight_step_fc_env_version(fc_buf, fc_len));

	{
		struct hl_step_yield y;
		__u8 rec[16];

		if (hyperlight_step_pump(fc_buf, fc_len, &y)) {
			/* The Yield comes back as the entry's result, with no
			 * exit of its own: the next wakeup, the flags and the
			 * status, little-endian, then the call's result.
			 */
			hl_put_le32(rec, (__u32)y.ns);
			hl_put_le32(rec + 4, (__u32)(y.ns >> 32));
			hl_put_le32(rec + 8, (__u32)y.flags);
			hl_put_le32(rec + 12, (__u32)y.status);
			if (hyperlight_dispatch_push_bytes_result(
				    rec, sizeof(rec), y.result,
				    y.result_len) == 0) {
				hyperlight_step_yield_delivered();
				return;
			}
			/* Cannot happen: a result is at most
			 * hl_hcall_max_payload(), which leaves room for it on
			 * the empty output stack.  Were it to, the host would
			 * see an entry without a Yield.
			 */
			uk_pr_err("dispatch: no room for the entry's Yield\n");
		}
	}
#else /* !CONFIG_HYPERLIGHT_STEP */
	hyperlight_dispatch_apply_host_env();
	/* No scheduler, so nothing can serve a guest function: the workload
	 * of such a kernel is a native main() that already ran at boot.
	 */
	uk_pr_warn("dispatch: no scheduler to run a guest function (fc_len=%lu)\n",
		   (unsigned long)fc_len);
#endif /* !CONFIG_HYPERLIGHT_STEP */

push_result:
	/* Every entry returns bytes; none means it kept no Yield. */
	hyperlight_dispatch_push_empty_result();
}

/* ── Snapshot pre-fault ─────────────────────────────────────────── */

/* Make every page of [start, start + len) writable. */
static void hyperlight_dispatch_touch(void *start, __sz len, int contents,
				      void *arg __unused)
{
	__uptr p = (__uptr)start & ~(__uptr)(__PAGE_SIZE - 1);
	__uptr end = (__uptr)start + len;

	for (; p < end; p += __PAGE_SIZE) {
		/* Pages whose contents do not matter get a fresh page; one
		 * shared with other data (the object's first and last) is
		 * copied like the rest.
		 */
		if (!contents && p >= (__uptr)start &&
		    p + __PAGE_SIZE <= end) {
			hyperlight_cow_fresh(p);
			continue;
		}
		{
			__u8 tmp = *(volatile __u8 *)p;
			*(volatile __u8 *)p = tmp;
		}
	}
}

/*
 * Pre-fault the kernel pages exception delivery writes, after snapshot
 * restore.
 *
 * Snapshot/restore marks every writable page Copy-on-Write (read-only
 * in its PTE).  The first write triggers a page fault.  The rest of
 * this comment is x86's (arm64 has no IST; see the function body).
 * Unikraft's IDT uses IST stacks for the #PF handler — but those
 * stacks are themselves CoW after restore.  The CPU can't push the
 * exception frame onto a read-only IST stack, so a CoW fault while
 * IST stacks are still CoW causes:
 *
 *   #PF → push to IST2 (CoW) → #PF → #DF → push to IST3 (CoW)
 *       → #PF → triple fault → shutdown
 *
 * This function runs under a temporary IDT whose #PF entry uses
 * IST=0 (current RSP on the scratch stack, always writable).  It
 * pre-faults what delivering an exception writes — the IST stacks,
 * the IDT, the nesting state, the TSS and the GDT — so the full
 * Unikraft IDT/IST mechanism works for subsequent CoW faults.  The IST
 * stacks' contents do not matter, so they get fresh pages instead of
 * copies, and the host backs only the ones an exception uses.  The rest
 * of .data and .bss faults in when first written, if ever: each page
 * pre-faulted costs a copy and a fresh scratch page on every restore.
 *
 * Called from hyperlight_dispatch_function's snapshot-restore path.
 */
void __attribute__((used))
hyperlight_dispatch_prefault(void)
{
#if defined(__x86_64__)
	uk_plat_native_except_state(hyperlight_dispatch_touch, NULL);
#elif defined(__aarch64__)
	/* On arm64 the CPU writes nothing on exception entry: the vectors
	 * store the registers on the native PAL's exception stacks (one
	 * each for critical, trap and IRQ), and those are all.
	 */
	hyperlight_dispatch_touch(
		(void *)uk_plat_native_except_get_except_stack_base(),
		3 * CPU_EXCEPT_STACK_SIZE, 0, NULL);
#endif
}

/* The dispatch entry is x86's; the arm64 entry is
 * in arm/entry64.S.
 */
#if defined(__x86_64__)
/* ── Dispatch entry point ───────────────────────────────────────── */

/*
 * Entry point for guest function calls.  The host sets RIP here
 * and runs the vCPU.  This function never returns — it halts the
 * vCPU after pushing the result.
 *
 * On entry, RFLAGS.ZF indicates whether a TLB flush is needed
 * (set by the host after snapshot/restore when page tables changed).
 *
 * RSP is loaded from g_exn_stack_top (the scratch exception stack
 * address queried from the host at init).  After evolve, the host
 * captures the guest's boot stack address (in BSS) as rsp_gva;
 * after snapshot/restore, that BSS page is Copy-on-Write (read-only).
 * Any push to a CoW page triggers #PF, but the IST stacks are also
 * CoW, causing a triple fault.  The scratch stack avoids this.
 *
 * Reading g_exn_stack_top (a BSS variable) before flushing the TLB
 * is safe: CoW pages are readable, and on a fresh-from-snapshot
 * vCPU the TLB is empty so the load fills from the new page tables.
 *
 * After snapshot/restore (ZF set), we must:
 *   1. Flush the TLB (page tables relocated to scratch)
 *   2. Install a temporary IDT with a #PF handler that uses IST=0
 *      (runs on the current scratch stack, always writable)
 *   3. Pre-fault all writable kernel pages so the IST stacks,
 *      IDT, TSS, and GDT become writable again
 *   4. Restore the full Unikraft IDT
 *   5. Continue with normal dispatch
 *
 * The temporary IDT reuses _cow_asm_pf_handler from entry64.S.
 */
void __attribute__((noreturn, naked))
hyperlight_dispatch_function(void)
{
	__asm__ volatile(
		/*
		 * Load RSP from the scratch exception stack top.
		 * The value was queried from the host at init.
		 *
		 * On the normal dispatch path (ZF=0) the TLB is valid,
		 * so this read of the BSS variable is correct.  On the
		 * snapshot/restore path (ZF=1) the vCPU may be reused
		 * with a STALE TLB (in-place MultiUseSandbox::restore
		 * relocates the page tables into fresh scratch but does
		 * not flush our TLB), so this read can return a stale
		 * value — we re-load it below after flushing the TLB.
		 */
		"movq g_exn_stack_top(%%rip), %%rsp\n\t"

		/*
		 * ZF=1: snapshot/restore path (needs fixup).
		 * ZF=0: normal dispatch (skip to 1f).
		 */
		"jnz 1f\n\t"

		/* ── Snapshot fixup ────────────────────────────── */

		/* Flush TLB: reload CR3.  Page tables are in new
		 * scratch; stale TLB entries must go.
		 */
		"movq %%cr3, %%rax\n\t"
		"movq %%rax, %%cr3\n\t"

		/*
		 * Re-load RSP from g_exn_stack_top now that the TLB is
		 * fresh: the load above may have used a stale entry that
		 * mapped the BSS to a pre-restore physical page, yielding
		 * 0 and a wild RSP (→ #PF on the sidt below → triple
		 * fault).  With the TLB flushed this reads the restored
		 * value from the new page tables.
		 */
		"movq g_exn_stack_top(%%rip), %%rsp\n\t"

		/* Save the original IDTR (10 bytes, 16 allocated). */
		"subq $16, %%rsp\n\t"
		"sidt (%%rsp)\n\t"

		/*
		 * Build a temporary 15-entry IDT on the stack.
		 * Only entry #14 (#PF) is populated — everything
		 * else is zeroed (absent → #GP on delivery, which
		 * is fine: we only need #PF during pre-fault).
		 *
		 * IST=0 means the handler runs on the current
		 * (scratch) stack, not the CoW IST stacks.
		 */
		"subq $240, %%rsp\n\t"
		"movq %%rsp, %%rbx\n\t"
		"movq %%rsp, %%rdi\n\t"
		"xorq %%rax, %%rax\n\t"
		"movq $30, %%rcx\n\t"
		"cld\n\t"
		"rep stosq\n\t"

		/* Populate entry #14: _cow_asm_pf_handler, IST=0 */
		"leaq _cow_asm_pf_handler(%%rip), %%rax\n\t"
		"movw %%ax, 224(%%rbx)\n\t"
		"movw $0x08, 226(%%rbx)\n\t"
		"movb $0, 228(%%rbx)\n\t"
		"movb $0x8E, 229(%%rbx)\n\t"
		"shrq $16, %%rax\n\t"
		"movw %%ax, 230(%%rbx)\n\t"
		"shrq $16, %%rax\n\t"
		"movl %%eax, 232(%%rbx)\n\t"

		/* Load the temporary IDTR. */
		"subq $16, %%rsp\n\t"
		"movw $239, (%%rsp)\n\t"
		"movq %%rbx, 2(%%rsp)\n\t"
		"lidt (%%rsp)\n\t"
		"addq $16, %%rsp\n\t"

		/*
		 * Pre-fault all writable kernel pages (.data/.bss).
		 * This resolves CoW for IST stacks, IDT, TSS, and
		 * GDT arrays so the full Unikraft IDT works again.
		 */
		"callq hyperlight_dispatch_prefault\n\t"

		/* Restore the original IDTR (saved above the IDT). */
		"lidt 240(%%rbx)\n\t"

		/* Pop temp IDT (240) + saved IDTR (16). */
		"addq $256, %%rsp\n\t"

		/*
		 * Re-initialise the paging frame allocator.
		 * The FA lives entirely in scratch, which was zeroed on
		 * restore.  Without this, demand paging (mmap, stack
		 * growth) crashes on NULL FA function pointers.
		 */
		"callq hyperlight_paging_reinit\n\t"

		"1:\n\t"
		/* ── Normal dispatch ───────────────────────────── */
		"callq hyperlight_dispatch_inner\n\t"

		/* Halt — port 108 = dispatch complete. */
		"movw $108, %%dx\n\t"
		"outl %%eax, %%dx\n\t"
		"cli\n\t"
		"hlt\n\t"
		: : : "rax", "rbx", "rcx", "rdx", "rdi", "rsi", "memory"
	);
	__builtin_unreachable();
}
#endif /* __x86_64__ */
