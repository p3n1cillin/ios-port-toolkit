# Changing Apple's code on a running port, safely

Some iOS behaviour is decided by fixed per-model tables in Apple's code (CoreMotion's motion capabilities, its compass
factory). A port to non-Apple hardware sometimes has to change a few instructions. This page is how the S10 Lite
port does it without breaking code signing. Three ways, from the lightest:

| Where the code is | How | Example |
| --- | --- | --- |
| a process we inject a library into | rewrite the page in memory (`vm_protect` with `VM_PROT_COPY`, write, protect back) | backboardd: CoreBrightness's 5 s wait for an ALS (`inject_bbd.c`, `no_als_wait`) |
| a program file we control (in `/var`) | patch the file **and** its code signature's page hash | locationd (`tools/ios/macho_patch.py`) |
| the dyld shared cache (every process) | the kernel changes the bytes **as the file is read** | CoreMotion's motion caps and compass factory (`apfs_vnops.c`) |

## The rule that matters: page hashes

XNU checks every code page of a signed file against its CodeDirectory as the page comes in. If a page fails, the
process is not killed (there is no `CS_KILL` here). But it loses `CS_VALID`, and with that **its entitlements**.

Test 233 changed one word of the shared cache without the hash:

- every process that ran CoreMotion logged `SecTaskLoadEntitlements failed error=22 cs_flags=26000000`;
- backboardd could no longer create HID services, so the **touch screen stopped working**.

So every changed page needs its new SHA-256 written into the CodeDirectory's hash slot too. Nothing else checks the
CodeDirectory: the CMS signature is not verified (the AMFI stand-in accepts any signature), and there is no trust
cache.

## The shared cache: a read-time patch in the kernel's APFS

The cache lives in the system image, which is several GB and flashed rarely. Rather than rewriting it, the kernel's
APFS driver changes the bytes as they are read (`apfs_patch_content`, called from `read(2)`, the cached read and
`pagein`). Each patch entry is:

```
{ file name, file size, file offset, length, old bytes, new bytes }
```

A patch applies only if the name, the size **and** the old bytes all match. A different iOS build, cache or file is
left alone, and a mismatch is logged. Patch the instruction and its page hash in the same table.

## Recipe (any dyld cache address)

1. **Find the instruction.** Find a unique string used near it with `tools/ios/dsc_whose.py CACHE 'text'`, which
   gives the string's address and every adrp+add that reaches it. Read the code with `tools/ios/dsc_sel.py CACHE
   IMAGE OFFSET COUNT`, which disassembles and annotates strings and selectors. For callers, use
   `tools/ios/dsc_callers.py CACHE IMAGE ADDR`. For where a call goes, use `tools/ios/dsc_target.py CACHE ADDR`: it
   follows the stub island to the real function (a hand-added offset sent Test 372 to the wrong one).
2. **Locate it in the files.** `tools/ios/dsc_fileoff.py CACHE ADDR` gives the subcache file, the offset and the
   current word.
3. **Work out the signature change.** `tools/ios/dsc_cdpatch.py SUBCACHE OFFSET OLDWORD NEWWORD` gives the
   CodeDirectory's hash slot (file offset), and the old and new hash for that page (16 KB pages, SHA-256 on iOS 16).
4. **Add both entries** to `apfs_content_patches[]`, rebuild the kernel, and boot.
5. **Check it is live** before judging the result: `r5qfs peek /System/Cryptexes/OS/System/Library/Caches/com.apple.dyld/SUBCACHE OFF 4`
   over r5qd reads the bytes as the kernel serves them (gate experiments with a boot-arg, e.g. `bbfake`).
5. **Verify in memory:** `cm_test peek ADDR` from any process shows the new word. Also check that the boot shows no
   `SecTaskLoadEntitlements failed`.

## A program file in /var

`tools/ios/macho_patch.py IN OUT OFFSET OLDWORD NEWWORD` changes one word of a thin arm64 Mach-O and updates the page
hash in every CodeDirectory of its signature, so the ad hoc signature stays valid page by page. To find code in such
a binary:

- `tools/ios/macho_xref.py FILE 'text'`: adrp+add references to a string;
- `tools/ios/macho_callers.py FILE ADDR…`: bl/b to an address;
- `llvm-objdump -d --start-address --stop-address` to read it.

Install with r5qd `put` (a temporary file, then a rename, so a running copy keeps its old file), then `kill` the
daemon and launchd restarts it. Keep the original beside it (`locationd.orig`).

## Live iteration without reboots: r5qd

`ports/r5q/wifi/r5qd.c` is a small token-authenticated daemon on the private network. It does `put`, `run`, `kill`,
`ps`, and **`reload LIB PROGRAM`**: the library is copied to `/var/r5q/lib`, named in `kern.exec_inject`, and the
program is killed, so launchd restarts it with the new library. That took the brightness and magnetometer work from
a 4-minute rebuild-flash-reboot cycle to seconds. A reload lasts until the next reboot: build it into the root image
afterwards.

## What not to do

- Don't patch a widely-used predicate when only one caller needs a different answer. Patch that caller's call. The
  compass check (CoreMotion+0x11808) has dozens of callers, so only the compass factory's `bl` was changed.
- Don't ship patched Apple binaries or cache pieces in the repository. Publish the patch (offsets, old and new
  bytes, how they were found), and make the patched file from the user's own copy.

## Reading what iOS itself logs (info and debug lines included)

The console (OS_ACTIVITY_DT_MODE, the kernel log) shows only default-level lines. Most daemons say what they are
doing at info level (`#I ...` in CommCenter and the baseband manager). Those lines are kept on the phone in the unified
log, so read them there:

1. Copy `/private/var/db/diagnostics` and `/private/var/db/uuidtext` to the PC. The S10 Lite port uses
   `work/ios-r5q/wifi/r5q_pull.py`: binary-safe over r5qd, 60000 bytes a request, resumable, about 1 MB/s. The shared
   cache's string file alone is about 100 MB.
2. `python3 tools/ios/tracev3.py LOGDIR 'REGEX'` (or `--pid N`, `--sub com.apple.telephony.abm`). It prints the
   entries with their format strings filled in. Private arguments stay `<private>`.
3. The copy holds private device data (names, addresses, identifiers). Keep it local and never commit it.
