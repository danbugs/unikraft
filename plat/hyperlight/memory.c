/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2024, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

/*
 * Called by plat/common/memory.c during ukplat_mem_init().
 * Hyperlight sets up page tables on the host side before guest entry,
 * so there are no guest-side mappings to initialise.
 */
int _ukplat_mem_mappings_init(void)
{
	return 0;
}
