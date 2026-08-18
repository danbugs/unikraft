/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2024, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

/*
 * Hyperlight console driver.
 *
 * Output currently uses DebugPrint (port 103) — one VM exit per byte,
 * output appears on the host's stderr.  This is a fallback path: it
 * requires no shared-memory setup and works even before the FlatBuffer
 * host-call protocol is initialised, which makes it suitable for early
 * boot diagnostics and error paths.
 *
 * TODO: Once the FlatBuffer dispatch infrastructure is in place,
 * switch the primary output path to HostPrint (a host function call
 * over shared memory).  HostPrint batches the entire buffer in a
 * single VM exit, so it is significantly faster.  DebugPrint should
 * then be reserved for pre-dispatch-init output and crash paths.
 *
 * Hyperlight does not provide console input — the guest runs
 * non-interactively — so console_in always returns 0.
 */

#include <uk/console/driver.h>
#include <hyperlight-x86/outb.h>

static __ssz hyperlight_console_out(struct uk_console *dev __unused,
				    const char *buf, __sz len)
{
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
