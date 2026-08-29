/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2024, Unikraft GmbH and The Unikraft Authors. */

#ifndef __HOSTFS_H__
#define __HOSTFS_H__

#include <vfscore/vnode.h>

/*
 * Maximum bytes per host-call read/write chunk.
 *
 * The actual chunk size is queried from the host at mount time via
 * GetHostFsChunkSize and stored in g_hostfs_chunk (capped to this
 * value).  Stack buffers use this compile-time maximum.
 */
#define HOSTFS_MAX_CHUNK 32768

/*
 * Runtime chunk size — set at mount time from GetHostFsChunkSize,
 * capped to HOSTFS_MAX_CHUNK.  Defaults to HOSTFS_MAX_CHUNK if the
 * host function is not registered.
 */
extern size_t g_hostfs_chunk;

/*
 * hostfs stores minimal per-node metadata.  Actual data lives on the
 * host; every operation goes through an hl_hcall_* round-trip.
 *
 * The v_data field of every hostfs vnode points to a struct hostfs_node
 * whose hf_path is the host-relative path (relative to the mount root).
 */
struct hostfs_node {
	char	hf_path[1024];	/* host-relative path */
	int	hf_type;	/* VREG or VDIR */
	int	hf_mount_idx;	/* mount index for host calls */
};

#endif /* __HOSTFS_H__ */
