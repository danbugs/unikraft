/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2024, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

/*
 * Hyperlight memory layout constants.
 *
 * Values must match hyperlight-common layout.rs and
 * arch/amd64/vmem.rs.
 */

#ifndef __HYPERLIGHT_MEM_H__
#define __HYPERLIGHT_MEM_H__

/*
 * Scratch region — always-writable memory at the top of the virtual
 * address space.  Used for I/O buffers, the guest stack, exception
 * stack, and CoW page allocation (bump allocator).
 *
 * The host stores metadata at fixed offsets below the scratch top.
 */
#define HL_SCRATCH_TOP_GVA	0xFFFFFFFFFFFFEFFF

/* Offsets downward from (HL_SCRATCH_TOP_GVA + 1) */
#define HL_SCRATCH_SIZE_OFF	0x08 /* u64: total scratch size */
#define HL_SCRATCH_ALLOC_OFF	0x10 /* u64: bump allocator pointer */

/* Computed addresses */
#define HL_SCRATCH_SIZE_GVA	(HL_SCRATCH_TOP_GVA + 1 - HL_SCRATCH_SIZE_OFF)
#define HL_SCRATCH_ALLOC_GVA	(HL_SCRATCH_TOP_GVA + 1 - HL_SCRATCH_ALLOC_OFF)

/*
 * x86-64 page table entry flags.
 *
 * Hyperlight pre-sets the Accessed and Dirty bits to prevent the CPU
 * from writing to the page tables (which would fault on CoW PTEs).
 */
#define PTE_PRESENT	(1ULL << 0)
#define PTE_RW		(1ULL << 1)
#define PTE_ACCESSED	(1ULL << 5)
#define PTE_DIRTY	(1ULL << 6)
#define PTE_NX		(1ULL << 63)

#define PTE_ADDR_MASK	0x000FFFFFFFFFF000ULL /* bits 51:12 */
#define PTE_AVL_COW	(1ULL << 9)           /* software CoW marker */

#endif /* __HYPERLIGHT_MEM_H__ */
