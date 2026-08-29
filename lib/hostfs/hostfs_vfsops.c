/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2024, Unikraft GmbH and The Unikraft Authors. */

/*
 * hostfs — VFS operations for host filesystem pass-through.
 *
 * Mounts are backed by Hyperlight host functions (fs_stat, fs_read_bytes,
 * fs_write_bytes, fs_mkdir, fs_unlink, fs_truncate, fs_list).  Each VFS
 * operation makes a synchronous host call via hl_hcall_*.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <vfscore/vnode.h>
#include <vfscore/mount.h>

#include <hyperlight-x86/hcall.h>

#include "hostfs.h"

extern struct vnops hostfs_vnops;

static int hostfs_mount(struct mount *mp, const char *dev,
			int flags __unused, const void *data __unused)
{
	struct hostfs_node *root;
	int mount_idx = 0;

	if (dev && dev[0] != '\0') {
		char *endp;
		long val = strtol(dev, &endp, 10);
		if (endp != dev && *endp == '\0')
			mount_idx = (int)val;
	}

	/* Query chunk size from host (once — first mount wins). */
	{
		__u64 chunk;
		if (hl_hcall_ulong("GetHostFsChunkSize", NULL, 0,
				    &chunk) == 0 && chunk > 0) {
			if (chunk > HOSTFS_MAX_CHUNK)
				chunk = HOSTFS_MAX_CHUNK;
			g_hostfs_chunk = (size_t)chunk;
		}
	}

	root = malloc(sizeof(*root));
	if (!root)
		return ENOMEM;

	memset(root, 0, sizeof(*root));
	root->hf_path[0] = '\0'; /* root is "" (relative to host mount) */
	root->hf_type = VDIR;
	root->hf_mount_idx = mount_idx;

	mp->m_root->d_vnode->v_data = root;
	mp->m_root->d_vnode->v_type = VDIR;
	mp->m_root->d_vnode->v_mode = S_IFDIR | 0755;

	return 0;
}

static int hostfs_unmount(struct mount *mp __unused, int flags __unused)
{
	return 0;
}

#define hostfs_sync    ((vfsop_sync_t)vfscore_nullop)
#define hostfs_vget    ((vfsop_vget_t)vfscore_nullop)
#define hostfs_statfs  ((vfsop_statfs_t)vfscore_nullop)

struct vfsops hostfs_vfsops = {
	hostfs_mount,
	hostfs_unmount,
	hostfs_sync,
	hostfs_vget,
	hostfs_statfs,
	&hostfs_vnops,
};

static struct vfscore_fs_type fs_hostfs = {
	.vs_name = "hostfs",
	.vs_init = NULL,
	.vs_op = &hostfs_vfsops,
};

UK_FS_REGISTER(fs_hostfs);
