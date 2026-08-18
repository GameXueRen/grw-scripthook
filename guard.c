/* Landing pad for the spawn trampoline. A spawner is
 * committed while its trampoline may still return.
 */
#include <stdint.h>

/* Raw bytes, not mnemonics: a different nop width would
 * move the pad and put the stale return in live code.
 */
__asm__(
    ".section .text\n"
    ".p2align 4\n"
    ".globl ShGuardAlign\n"
    "ShGuardAlign:\n"

    /* The stale return arrives with a dead spawner in
     * rcx, so bail out instead of running on.
     */
    "  .byte 0x48,0x85,0xC9\n"              /* test rcx,rcx  */
    "  .byte 0x75,0x01\n"                   /* jne  +1       */
    "  .byte 0xC3\n"                        /* ret           */

    /* Pad out the rest of the slot. Widths are pinned so
     * a rebuild cannot re-pack them and shrink the pad.
     */
    "  .byte 0x0F,0x1F,0x44,0x00,0x00\n"                 /* 5 */
    "  .byte 0x0F,0x1F,0x40,0x00\n"                      /* 4 */
    "  .byte 0x0F,0x1F,0x80,0x00,0x00,0x00,0x00\n"       /* 7 */
    "  .byte 0x0F,0x1F,0x00\n"                           /* 3 */
    "  .byte 0x66,0x0F,0x1F,0x44,0x00,0x00\n"            /* 6 */
    "  .byte 0x66,0x90\n"                                /* 2 */
    "  .byte 0x0F,0x1F,0x44,0x00,0x00\n"                 /* 5 */
    "  .byte 0x0F,0x1F,0x40,0x00\n"                      /* 4 */
    "  .byte 0x66,0x0F,0x1F,0x44,0x00,0x00\n"            /* 6 */
    "  .byte 0x0F,0x1F,0x00\n"                           /* 3 */

    "  .byte 0xC3\n"                        /* ret           */
);

extern void ShGuardAlign(void);

/* Referenced so the linker cannot drop the pad. */
void *const ShGuardTable[] = { (void *)ShGuardAlign };
