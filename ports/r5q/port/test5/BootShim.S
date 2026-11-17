/* Based on Mu-Silicium BootShim; see licenses/Mu-Silicium.txt.
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * R5Q Test 2 shim: ARM64 Image entry, command-mode display probe and
 * colour-band progress markers, persistent sec_log telemetry, fixed FD copy,
 * bounded XNUP copy, SEC entry.
 */
#define MDSS 0x0AE00000
#define INFO 0x9E300000      /* last MiB of Display Reserved, outside the frame */
#define SEC_LOG_BASE 0xC1600000

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
  mov x19, x1                   /* container (FD, then XNUP) */
  mov x24, x0                   /* DTB pointer from ABL, recorded only */

  /* Log banner to persistent sec_log */
  adr x0, _MsgStart
  mov w1, #(_MsgStartEnd - _MsgStart)
  bl _LogSec

  bl _DisplaySetup
  ldr x9, =INFO
  mov w10, #1
  str w10, [x9, #40]
  mov w0, #4
  ldr w1, =0xFF00FF00           /* Band 4: Green (DisplaySetup OK) */
  bl _PaintBand

  mov x4, x19
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
  ldr x9, =INFO
  mov w10, #2
  str w10, [x9, #40]
  mov w0, #5
  ldr w1, =0xFF0000FF           /* Band 5: Blue (FD copied) */
  bl _PaintBand

  adr x0, _MsgFd
  mov w1, #(_MsgFdEnd - _MsgFd)
  bl _LogSec

  /* Clear stale payload header even when the new container fails validation. */
  ldr x5, =PAYLOAD_BASE
  str xzr, [x5]
  ldr x6, =FD_SIZE
  add x4, x19, x6
  ldr w2, [x4]
  movz w3, #0x4e58
  movk w3, #0x5055, lsl #16
  cmp w2, w3
  b.ne _Rejected
  ldr w6, [x4, #4]
  cmp w6, #48
  b.lo _Rejected
  ldr w3, =(PAYLOAD_MAX - 8)
  cmp w6, w3
  b.hi _Rejected
  add x6, x6, #23
  and x6, x6, #~15

_CopyPayload:
  ldp x2, x3, [x4], #16
  stp x2, x3, [x5], #16
  subs x6, x6, #16
  b.ne _CopyPayload

  ldr x9, =INFO
  mov w10, #3
  str w10, [x9, #40]
  mov w0, #6
  ldr w1, =0xFFFFFFFF           /* Band 6: White (Payload accepted) */
  bl _PaintBand

  adr x0, _MsgPay
  mov w1, #(_MsgPayEnd - _MsgPay)
  bl _LogSec
  b _Entry

_Rejected:
  ldr x9, =INFO
  mov w10, #4
  str w10, [x9, #40]
  mov w0, #6
  ldr w1, =0xFFFF0000           /* Band 6: Red (Payload rejected) */
  bl _PaintBand

  adr x0, _MsgRej
  mov w1, #(_MsgRejEnd - _MsgRej)
  bl _LogSec

_Entry:
  adr x0, _MsgSec
  mov w1, #(_MsgSecEnd - _MsgSec)
  bl _LogSec

  dsb sy
  ic iallu
  dsb sy
  isb
  ldr x5, =FD_BASE
  br x5

/* Leaf: x0 = string pointer, w1 = string length.
   Appends to Samsung sec_log ring buffer at 0xC1600000. Clobbers x8-x15. */
_LogSec:
  ldr x8, =SEC_LOG_BASE
  ldr w9, [x8, #4]              /* check magic 0x4D474F4C "LOGM" */
  movz w10, #0x4F4C
  movk w10, #0x4D47, lsl #16
  cmp w9, w10
  b.ne 9f
  ldr w9, [x8, #8]              /* current w_idx */
  and w11, w9, #0x1FFFFF        /* offset within 2MB */
  cmp w11, #0x10
  b.lo 1f
  movz w10, #0xFF00
  movk w10, #0x001F, lsl #16
  cmp w11, w10
  b.lo 2f
1:
  mov w11, #0x10
2:
  add x12, x8, x11              /* dest pointer */
  mov x13, x0                   /* src pointer */
  mov w14, w1                   /* length */
3:
  ldrb w15, [x13], #1
  strb w15, [x12], #1
  subs w14, w14, #1
  b.ne 3b
  add w11, w11, w1
  and w9, w9, #~0x1FFFFF
  orr w9, w9, w11
  str w9, [x8, #8]              /* update w_idx */
  dsb sy
9:
  ret

/* Leaf.  Out: x20 framebuffer (0 = none), w21 stride, w22 height,
 * x25 CTL base driving INTF1 (CTL0 fallback).  Clobbers x9-x17. */
_DisplaySetup:
  ldr x9, =INFO
  movz w10, #0x3552             /* "R5Q2" */
  movk w10, #0x3251, lsl #16
  str w10, [x9]
  str wzr, [x9, #4]
  mov w10, #0xFF
  str w10, [x9, #8]
  str w24, [x9, #44]
  str w19, [x9, #48]
  str wzr, [x9, #52]
  mov x20, #0
  mov w21, #0
  mov w22, #0
  ldr x10, =MDSS
  adr x11, _Pipes
  mov x12, #0
1:
  ldr w13, [x11, x12, lsl #2]
  add x14, x10, x13
  ldr w15, [x14, #0x14]         /* SRC0_ADDR */
  tbz w15, #31, 2f              /* DRAM starts at 0x80000000 */
  mov w20, w15
  str w15, [x9, #4]
  str w12, [x9, #8]
  ldr w16, [x14, #0x24]
  str w16, [x9, #12]
  and w21, w16, #0xffff
  ldr w16, [x14, #0x00]
  str w16, [x9, #16]
  lsr w22, w16, #16
  ldr w16, [x14, #0x30]
  str w16, [x9, #20]
  b 3f
2:
  add x12, x12, #1
  cmp x12, #8
  b.lo 1b
3:
  cbz w21, 4f
  cmp w21, #0x4000
  b.ls 5f
4:
  mov w21, #4320                /* 1080 * 4 */
5:
  cbz w22, 6f
  cmp w22, #4096
  b.ls 7f
6:
  mov w22, #2400
7:
  add x14, x10, #0x6B000        /* INTF1 = MDSS + 0x6B800 */
  add x14, x14, #0x800
  ldr w15, [x14, #0x284]
  str w15, [x9, #24]
  ldr w15, [x14, #0x2B4]
  str w15, [x9, #28]

  /* Also enable INTF1 hardware autorefresh per TE pulse */
  movz w16, #0x0001
  movk w16, #0x8000, lsl #16
  str w16, [x14, #0x2B4]

  add x14, x10, #0x2000         /* CTL0 */
  mov w12, #0
8:
  ldr w15, [x14, #0xF4]         /* CTL_INTF_ACTIVE */
  tbnz w15, #1, 9f              /* bit 1 = INTF1 */
  add x14, x14, #0x200
  add w12, w12, #1
  cmp w12, #6
  b.lo 8b
  add x14, x10, #0x2000
  mov w12, #0xF0                /* no CTL claims INTF1: CTL0 fallback */
  ldr w15, [x14, #0xF4]
9:
  mov x25, x14
  str w12, [x9, #32]
  str w15, [x9, #36]
  str w14, [x9, #56]
  dsb sy
  ret

/* Leaf.  w0 band (0-7), w1 colour.  Fills 1/8 of the frame and pushes one
 * command-mode frame once the previous transfer has finished. */
_PaintBand:
  cbz x20, 9f
  lsr w2, w22, #3
  mul w3, w2, w21
  mul w4, w0, w3
  add x5, x20, w4, uxtw
  lsr w3, w3, #2
  cbz w3, 9f
1:
  str w1, [x5], #4
  subs w3, w3, #1
  b.ne 1b
  dsb sy
  cbz x25, 9f
  movz w6, #0x0010, lsl #16
2:
  ldr w7, [x25, #0x1C]
  tbz w7, #0, 3f
  subs w6, w6, #1
  b.ne 2b
  ldr x8, =INFO
  ldr w7, [x8, #52]
  add w7, w7, #1
  str w7, [x8, #52]
  b 9f
3:
  mov w7, #1
  str w7, [x25, #0x1C]          /* CTL_START */
  dsb sy
9:
  ret

  .balign 4
_Pipes:
  .long 0x25000, 0x27000, 0x29000, 0x2b000   /* DMA0-3 first: UEFI/ABL use DMA */
  .long 0x05000, 0x07000, 0x09000, 0x0b000   /* VIG0-3 */

_MsgStart:
  .ascii "\n[TEST2] >>> BootShim entered, DisplaySetup completed <<<\n"
_MsgStartEnd:

_MsgFd:
  .ascii "[TEST2] BootShim: UEFI FD copied to 0x9FC00000\n"
_MsgFdEnd:

_MsgPay:
  .ascii "[TEST2] BootShim: XNUP payload validated & copied to 0xF0000000\n"
_MsgPayEnd:

_MsgRej:
  .ascii "[TEST2] BootShim: XNUP payload rejected\n"
_MsgRejEnd:

_MsgSec:
  .ascii "[TEST2] BootShim: jumping to SEC at 0x9FC00000...\n"
_MsgSecEnd:

  .ltorg
  .balign 16
_Payload:
