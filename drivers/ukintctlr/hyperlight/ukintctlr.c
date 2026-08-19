/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2024, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

/*
 * Stub interrupt controller driver for Hyperlight.
 *
 * Hyperlight supports hardware interrupts via the hw-interrupts
 * feature: the host creates an in-kernel PIC + LAPIC (on KVM) and
 * injects timer interrupts at vector 0x20 through an irqfd.  The
 * guest configures the timer period via port 107 (PvTimerConfig).
 *
 * TODO: Implement guest-side hw-interrupts support — PIC remap,
 * LAPIC EOI, IRQ masking/unmasking, and PvTimer configuration
 * (port 107).  Until then, all ops are no-ops.
 */

#include <uk/intctlr.h>

static int configure_irq(struct uk_intctlr_irq *irq __unused)
{
	return 0;
}

static void mask_irq(unsigned int irq __unused)
{
}

static void unmask_irq(unsigned int irq __unused)
{
}

static struct uk_intctlr_driver_ops hyperlight_intctlr_ops = {
	.configure_irq = configure_irq,
	.mask_irq      = mask_irq,
	.unmask_irq    = unmask_irq,
};

static struct uk_intctlr_desc intctlr = {
	.name = "hyperlight",
	.ops  = &hyperlight_intctlr_ops,
};

int uk_intctlr_probe(void)
{
	return uk_intctlr_register(&intctlr);
}
