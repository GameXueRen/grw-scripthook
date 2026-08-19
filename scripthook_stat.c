/* Protected ints, the game's obfuscated stat storage. */
/* Health, resources, skill points and XP are all one type:
 * a 0x28 byte struct of four byte pointers and an XOR key.
 * The value is those bytes bit-interleaved then XORed. */
#include <windows.h>
#include <string.h>
#include <stdint.h>

#define SH_BUILD 1
#include "scripthook.h"

extern int ShReadableAddr(uint64_t addr, size_t len);
extern void ShSetError(int err);

typedef struct {
    uint8_t *p[4];
    uint32_t key;
} ProtInt;

static int Load(uint64_t addr, ProtInt *s) {
    if (!ShReadableAddr(addr, sizeof(*s))) return 0;
    memcpy(s, (void *)(uintptr_t)addr, sizeof(*s));
    return 1;
}

/* Bit i lives in plane (i & 3) at position (i >> 2).
 * Verified against the engine codec at 0x1403EEDE0. */
SH_API int ShStatRead(uint64_t stat, uint32_t *out) {
    ProtInt s;
    uint8_t b[4];
    uint32_t v = 0;
    int i, bit = 0;

    if (!out) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    if (!Load(stat, &s)) { ShSetError(SH_ERR_UNWRITABLE); return 0; }
    for (i = 0; i < 4; i++) {
        if (!ShReadableAddr((uint64_t)(uintptr_t)s.p[i], 1)) {
            ShSetError(SH_ERR_UNWRITABLE);
            return 0;
        }
        b[i] = *s.p[i];
    }
    while (bit < 32) {
        for (i = 0; i < 4; i++) {
            v |= (uint32_t)(b[i] & 1) << bit;
            b[i] >>= 1;
            bit++;
        }
    }
    *out = v ^ s.key;
    ShSetError(SH_OK);
    return 1;
}

SH_API int ShStatWrite(uint64_t stat, uint32_t value) {
    ProtInt s;
    uint8_t b[4] = { 0, 0, 0, 0 };
    uint32_t enc;
    int i, bit = 0, round = 0;

    if (!Load(stat, &s)) { ShSetError(SH_ERR_UNWRITABLE); return 0; }
    enc = value ^ s.key;
    while (bit < 32) {
        for (i = 0; i < 4; i++) {
            b[i] |= (uint8_t)(((enc >> bit) & 1) << round);
            bit++;
        }
        round++;
    }
    for (i = 0; i < 4; i++) {
        if (!ShReadableAddr((uint64_t)(uintptr_t)s.p[i], 1)) {
            ShSetError(SH_ERR_UNWRITABLE);
            return 0;
        }
        *s.p[i] = b[i];
    }
    ShSetError(SH_OK);
    return 1;
}
