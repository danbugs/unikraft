/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2024, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

/*
 * Hyperlight console driver.
 *
 * Two output paths:
 *
 *   1. DebugPrint (port 103) — one VM exit per byte.  Available from
 *      the very first instruction; used during early boot before the
 *      hcall subsystem is initialised.  Also the fallback if HostPrint
 *      fails.
 *
 *   2. HostPrint (host function call) — one VM exit per buffer.  Uses
 *      the FlatBuffer-based hcall protocol over the PEB's shared I/O
 *      stacks.  Requires hl_hcall_init() to have been called.
 *
 * The console transparently picks HostPrint when the hcall subsystem
 * is ready, falling back to DebugPrint otherwise.
 *
 * TODO: Route uk_pr_* output through port 99 (Log / OutBAction::Log)
 * instead of HostPrint (port 101).  Port 99 delivers a GuestLogData
 * FlatBuffer to the host's tracing framework, keeping kernel warnings
 * out of the HostPrint capture path (drain_output).  This requires
 * encoding GuestLogData FlatBuffers (level, source_file, line, source,
 * message) in the kernel — a different schema from hcall FlatBuffers.
 *
 * Hyperlight does not provide console input — the guest runs
 * non-interactively — so console_in always returns 0.
 */

#include <uk/console/driver.h>
#include <hyperlight-x86/hcall.h>
#include <hyperlight-x86/outb.h>

static __ssz hyperlight_console_out(struct uk_console *dev __unused,
				    const char *buf, __sz len)
{
	/* Try HostPrint first — one VM exit for the entire buffer */
	if (hl_hcall_ready()) {
		if (hl_call_host_print(buf, len) == 0)
			return len;
	}

	/* Fallback: DebugPrint, one byte at a time */
	for (__sz i = 0; i < len; i++)
		hyperlight_debug_putc(buf[i]);
	return len;
}

/*
 * Hyperlight guests run non-interactively — the host invokes guest
 * functions, not the other way around — so there is no console input
 * channel.  Always return 0 (no bytes read).
 */
static __ssz hyperlight_console_in(struct uk_console *dev __unused,
				   char *buf __unused, __sz len __unused)
{
	return 0;
}

static struct uk_console_ops hyperlight_console_ops = {
	.out = hyperlight_console_out,
	.in = hyperlight_console_in,
};

static struct uk_console hyperlight_console;

/* Called from setup.c during early boot */
void _ukplat_init_console(void)
{
	uk_console_init(&hyperlight_console, "Hyperlight",
			&hyperlight_console_ops,
			UK_CONSOLE_FLAG_STDOUT | UK_CONSOLE_FLAG_STDIN);
	uk_console_register(&hyperlight_console);
}
