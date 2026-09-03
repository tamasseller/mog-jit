#include "bytecode.h"

/* The memory-mapped accessor: internal flash, XIP QSPI, or a RAM buffer. A
 * handle is the address itself, so every call folds to a pointer bump and the
 * seam costs nothing beyond not being inlined. Weak, so a target whose
 * bytecode is not addressable links its own over these. */

extern "C" __attribute__((weak)) void bcOpen(BcCursor *c, BcHandle h, uint32_t)
{
    c->w = h;
}

extern "C" __attribute__((weak)) uint8_t bcNext(BcCursor *c)
{
    return *(const uint8_t *)(uintptr_t)c->w++;
}

extern "C" __attribute__((weak)) BcHandle bcTell(const BcCursor *c)
{
    return c->w;
}

extern "C" __attribute__((weak)) void bcHint(BcHandle, uint32_t)
{
}
