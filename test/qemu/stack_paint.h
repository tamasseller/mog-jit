// Stack-painting/high-water-mark empirical check — see stack_paint.cpp's
// own header comment for why this exists (docs/design.md G2/G3/G5).
#ifndef JIT_ARMV6M_TEST_QEMU_STACK_PAINT_H_
#define JIT_ARMV6M_TEST_QEMU_STACK_PAINT_H_

// Paints the stack region below the current sp with a sentinel byte. Call
// as literally the first line of main(), before anything else pushes a
// frame, to paint the largest region achievable.
void paintStack();

// Scans for how far the sentinel actually got overwritten over the whole
// run, reports it via semihosting unconditionally, and returns whether a
// comfortable margin (REQUIRED_SLACK_BYTES, stack_paint.cpp) remained.
// Call once, after every TEST has run.
bool reportStackHighWaterMark();

#endif // JIT_ARMV6M_TEST_QEMU_STACK_PAINT_H_
