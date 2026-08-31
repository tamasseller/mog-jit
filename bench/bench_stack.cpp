#include "bench_stack.h"

extern "C" uint8_t __bss_end;  // linker.ld
extern "C" uint8_t _stack_top; // linker.ld; vectors.S's own initial sp

namespace
{
constexpr uint8_t PAINT_BYTE = 0xAA;

/* Headroom below the live sp, left unpainted. At -Os GCC rewrites a plain
 * byte-fill loop into a memset() call, whose own pushed return address then
 * falls inside the region it was told to fill — a real incident, documented
 * at length in test/qemu/stack_paint.cpp. `volatile` below blocks that
 * rewrite; this margin survives it if some future flag reintroduces one. */
constexpr uint32_t PAINT_CALL_SAFETY_MARGIN = 64;
} // namespace

void benchPaintStack()
{
    register uint32_t sp asm("sp");

    uint8_t *lo = &__bss_end;
    uint8_t *hi = (uint8_t *)(uintptr_t)sp - PAINT_CALL_SAFETY_MARGIN;

    for (volatile uint8_t *p = lo; p < hi; p++)
    {
        *p = PAINT_BYTE;
    }
}

uint32_t benchUntouchedBytes()
{
    const volatile uint8_t *p = &__bss_end;

    while (p < &_stack_top && *p == PAINT_BYTE)
    {
        p++;
    }

    return (uint32_t)(p - &__bss_end);
}

uint32_t benchStackUsedBytes()
{
    return (uint32_t)(&_stack_top - &__bss_end) - benchUntouchedBytes();
}
