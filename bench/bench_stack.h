#ifndef JIT_ARMV6M_BENCH_BENCH_STACK_H_
#define JIT_ARMV6M_BENCH_BENCH_STACK_H_

#include <stdint.h>

/* Per-phase stack watermarking.
 *
 * test/qemu/stack_paint.cpp does this once for a whole image, to corroborate
 * design.md's stack-safety strategy. A benchmark needs the other shape: one
 * measurement attributable to one phase, so the JIT's excursion and a
 * compiled kernel's frame can be reported side by side rather than as a
 * single number covering both.
 *
 * Hence repainting. Everything between __bss_end and the live sp is
 * provably unused at the moment of the call, so repainting it before each
 * phase is as safe as painting it once at the top of main, and gives each
 * phase a fresh canvas. The same PAINT_CALL_SAFETY_MARGIN and `volatile`
 * that file documents apply here for the same reason: at -Os GCC will
 * rewrite a plain byte-fill loop into a memset call whose own frame lands
 * inside the region being filled.
 */

/** Repaints the unused stack region below the caller's sp. */
void benchPaintStack();

/** Bytes from __bss_end up to the lowest address touched since the last
 *  paint — small means the excursion came close to .bss. */
uint32_t benchUntouchedBytes();

/** Bytes of stack consumed below _stack_top since the last paint. */
uint32_t benchStackUsedBytes();

#endif // JIT_ARMV6M_BENCH_BENCH_STACK_H_
