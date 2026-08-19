/* The close range body blur, taken out at the switch.
 * The menu's DoF settings leave this proximity fade on,
 * and only a first person camera ever sits inside it. */
/* One byte: the imm of mov sil,1 ahead of the call that
 * checks name hash 0x826846F3. Firejumper93's Build 35,
 * decoded from the community FP mod's cheat table. */
#include <windows.h>
#include <string.h>
#include <stdint.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "image.h"

#define BLUR_MATCH  SH_IMG(0x14E7625C)
#define BLUR_IMM    5
#define BLUR_LEN    16

/* mov r14d,r12d / mov sil,imm / call hasher / cmp eax.
 * The imm at +5 is the byte this module owns.
 */
static const uint8_t BLUR_SIG[BLUR_LEN] = {
    0x45, 0x89, 0xE6, 0x40, 0xB6, 0x01, 0xE8, 0, 0, 0, 0,
    0x3D, 0xF3, 0x46, 0x68, 0x82
};
static const uint8_t BLUR_MASK[BLUR_LEN] = {
    1, 1, 1, 1, 1, 0, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1
};

extern void ShSetError(int err);
extern int ShReadableAddr(uint64_t addr, size_t len);

/* Refuse anything but the site we expect, so a build we
 * do not know is left alone.
 */
static int BlurSiteOk(void) {
    const uint8_t *at = (const uint8_t *)(uintptr_t)BLUR_MATCH;
    int i;

    if (!ShReadableAddr(BLUR_MATCH, BLUR_LEN)) return 0;
    for (i = 0; i < BLUR_LEN; i++)
        if (BLUR_MASK[i] && at[i] != BLUR_SIG[i]) return 0;
    return at[BLUR_IMM] == 0x00 || at[BLUR_IMM] == 0x01;
}

/** on=0 removes the close range blur, on=1 restores it. */
SH_API int ShSetCameraBlur(int on) {
    uint8_t *b = (uint8_t *)(uintptr_t)(BLUR_MATCH + BLUR_IMM);
    uint8_t want = on ? 0x01 : 0x00;
    DWORD old;

    if (!BlurSiteOk()) {
        ShSetError(SH_ERR_NO_CANDIDATE);
        return 0;
    }
    if (*b == want) { ShSetError(SH_OK); return 1; }

    if (!VirtualProtect(b, 1, PAGE_EXECUTE_READWRITE, &old)) {
        ShSetError(SH_ERR_UNWRITABLE);
        return 0;
    }
    *b = want;
    VirtualProtect(b, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), b, 1);
    ShSetError(SH_OK);
    return 1;
}

/** 1 when the blur byte is currently forced off. */
SH_API int ShCameraBlurOff(void) {
    const uint8_t *at = (const uint8_t *)(uintptr_t)BLUR_MATCH;

    if (!BlurSiteOk()) return 0;
    return at[BLUR_IMM] == 0x00;
}
