/* Based on Mu-Silicium BootShim; see licenses/Mu-Silicium.txt.
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 * ARM64 Image entry, fixed FD copy, bounded XNUP copy, then SEC entry.
 */
_Head:
  adr x1, _Payload
  b _Start
  .quad 0x80000                 /* Linux ARM64 text_offset, as in stock Image */
  .quad TEST_IMAGE_SIZE
  .quad 0
  .quad 0
  .quad 0
  .quad 0
  .ascii "ARM\x64"
  .long 0

_Start:
  mov x4, x1
  ldr x5, =FD_BASE
  cmp x4, x5
  b.eq _FdCopied
  ldr x6, =FD_SIZE
_CopyFd:
  ldp x2, x3, [x4], #16
  stp x2, x3, [x5], #16
  subs x6, x6, #16
  b.ne _CopyFd
_FdCopied:
  // Clear stale payload header even when the new container fails validation.
  ldr x5, =PAYLOAD_BASE
  str xzr, [x5]
  ldr x6, =FD_SIZE
  add x4, x1, x6
  ldr w2, [x4]
  movz w3, #0x4e58
  movk w3, #0x5055, lsl #16
  cmp w2, w3
  b.ne _Entry
  ldr w6, [x4, #4]
  cmp w6, #48
  b.lo _Entry
  ldr w3, =(PAYLOAD_MAX - 8)
  cmp w6, w3
  b.hi _Entry
  add x6, x6, #23
  and x6, x6, #~15
_CopyPayload:
  ldp x2, x3, [x4], #16
  stp x2, x3, [x5], #16
  subs x6, x6, #16
  b.ne _CopyPayload
_Entry:
  dsb sy
  ic iallu
  dsb sy
  isb
  ldr x5, =FD_BASE
  br x5
  .ltorg
  .balign 16
_Payload:
