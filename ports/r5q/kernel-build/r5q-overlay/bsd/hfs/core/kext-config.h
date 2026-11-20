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
 * Modified for the Galaxy S10 Lite (r5q) native iOS port, November 2026: HFS (hfs-627.100.6) built into the kernel
 * instead of loaded as hfs.kext, to hold /private/var on the phone's flash (wifi/STORAGE-PLAN.md). The kext's
 * configuration, made for a kernel build: the kernel's own CONFIG_PROTECT and QUOTA are overridden (no AppleKeyStore
 * here; no quotas). Kext names that a kernel build spells differently are mapped below.
 */
#ifndef _hfs_config_
#define _hfs_config_

/*
 * (Test 408: panic "__memmove_chk object size check failed (16 < 32)" in buildrecord(), on the first mount) HFS copies
 * the 32 bytes of Finder info across two adjacent 16-byte members in one bcopy, written for a kext, whose checked
 * string functions measure the whole object (__xnu_bos_loose); code inside the kernel gets member-strict checks
 * (osfmk/libsa/string.h, XNU_KERNEL_PRIVATE). HFS is built without the checked builtins, as the kext's rules allow.
 */
#undef _FORTIFY_SOURCE
#define _FORTIFY_SOURCE 0

#undef HFS_COMPRESSION
#define HFS_COMPRESSION 1   /* (the code assumes it in places; decmpfs is standard in this kernel) */
#undef FIFO
#define FIFO 1
#undef QUOTA
#define QUOTA 0
#undef CONFIG_PROTECT
#define CONFIG_PROTECT 0
#undef CONFIG_SECLUDED_RENAME
#define CONFIG_SECLUDED_RENAME 0

#ifdef KERNEL
#include <sys/types.h>
/* a kext calls these by the names it is given; inside the kernel they are spelled so */
#define ubc_create_upl ubc_create_upl_external
int sysctlbyname(const char *, void *, size_t *, void *, size_t);
#endif

#endif /* defined(_hfs_config_) */
