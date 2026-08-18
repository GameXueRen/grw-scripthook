/* Image relative addressing, so the hook survives ASLR.
 * Every constant below is an RVA from the module base.
 */
#ifndef GRW_IMAGE_H
#define GRW_IMAGE_H

#include <windows.h>
#include <stdint.h>

/* The link time base the RVAs were taken against. Kept
 * only to document where the numbers came from.
 */
#define SH_LINK_BASE 0x140000000ULL

static uint64_t ShImageBase(void) {
    static uint64_t base = 0;
    if (!base) base = (uint64_t)(uintptr_t)GetModuleHandleA(NULL);
    return base;
}

static uint64_t ShImageSize(void) {
    static uint64_t size = 0;
    if (!size) {
        uint64_t b = ShImageBase();
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)
            (uintptr_t)b;
        const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)
            (uintptr_t)(b + (uint64_t)dos->e_lfanew);
        size = nt->OptionalHeader.SizeOfImage;
    }
    return size;
}

#define SH_IMG(rva) (ShImageBase() + (uint64_t)(rva))

/* True when addr lies inside the loaded image. */
static int ShInImage(uint64_t addr) {
    uint64_t b = ShImageBase();
    return addr >= b && addr < b + ShImageSize();
}

#endif
