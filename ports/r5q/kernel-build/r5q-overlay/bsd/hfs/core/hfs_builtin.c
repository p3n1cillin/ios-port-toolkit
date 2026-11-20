/*
 * Copyright (c) 2015 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * Modified for the Galaxy S10 Lite (r5q) native iOS port, November 2026: hfs_iokit.cpp's work, without IOKit.
 * HFS (hfs-627.100.6) is built into this kernel rather than loaded as hfs.kext, so this registers it with the VFS the
 * way com_apple_filesystems_hfs::start() does, called once from redwood_ufs_init(). (The legacy encodings kext is not
 * used on iOS: its header is !TARGET_OS_IPHONE, and HFS's core does without it.) The IOKit lookups behind hfs_is_ejectable() and friends have no counterpart here: the phone's
 * flash is not ejectable, and nothing names media by content UUID.
 */
#include "kext-config.h"
#include <mach/mach_types.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#include <libkern/libkern.h>

#include "hfs_iokit.h"
#include "hfs.h"
#include "hfs_dbg.h"
#include "hfs_cnode.h"

#ifndef panic_on_assert
bool panic_on_assert;
#endif

extern struct vnodeopv_desc hfs_vnodeop_opv_desc;
extern struct vnodeopv_desc hfs_specop_opv_desc;
extern struct vnodeopv_desc hfs_fifoop_opv_desc;
extern struct vfsops hfs_vfsops;

static vfstable_t hfs_vfs_handle;

void
hfs_builtin_start(void)
{
	struct vnodeopv_desc *op_descs[] = {
		&hfs_vnodeop_opv_desc,
		&hfs_specop_opv_desc,
#if FIFO
		&hfs_fifoop_opv_desc,
#endif
	};
	struct vfs_fsentry vfe = {
		.vfe_vfsops = &hfs_vfsops,
		.vfe_vopcnt = sizeof(op_descs) / sizeof(op_descs[0]),
		.vfe_opvdescs = op_descs,
		.vfe_fsname = "hfs",
		.vfe_flags = (VFS_TBLNOTYPENUM | VFS_TBLLOCALVOL | VFS_TBLREADDIR_EXTENDED | VFS_TBL64BITREADY |
	    VFS_TBLVNOP_PAGEOUTV2 | VFS_TBLVNOP_PAGEINV2 | VFS_TBLTHREADSAFE | VFS_TBLNATIVEXATTR),
	};
	int ret;

	if (hfs_vfs_handle != NULL) {
		return;
	}
	ret = vfs_fsadd(&vfe, &hfs_vfs_handle);
	if (ret != 0) {
		printf("hfs: vfs_fsadd failed: %d\n", ret);
		hfs_vfs_handle = NULL;
		return;
	}
	hfs_init_zones();
	/*
	 * (Test 406: panic "Use sysctl_register_oid_early to register permanent nodes") hfs_sysctl_register() is how a
	 * kext adds its vfs.generic.hfs nodes; nodes inside the kernel image may not be added so. They are tunables with
	 * built-in defaults, and are left out.
	 */
	printf("hfs: built in (hfs-627.100.6), registered with the VFS\n");
}

int
hfs_is_ejectable(__unused const char *cdev_name)
{
	return 0;
}

void
hfs_iterate_media_with_content(__unused const char *content_uuid_cstring,
    __unused int (*func)(const char *bsd_name, const char *uuid_str, void *arg), __unused void *arg)
{
}

kern_return_t
hfs_get_platform_serial_number(__unused char *serial_number_str, __unused uint32_t len)
{
	return KERN_FAILURE;
}
