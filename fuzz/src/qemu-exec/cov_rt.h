#ifndef JIT_ARMV6M_FUZZ_QEMU_EXEC_COV_RT_H_
#define JIT_ARMV6M_FUZZ_QEMU_EXEC_COV_RT_H_

#include <stdint.h>

/* One bit per four bytes of rom, indexed by the block's own address. Every
 * instrumented block opens with a 4-byte `bl`, so no two blocks' return
 * addresses are closer than four bytes and the index is injective — there is
 * nothing to collide, and a bit number maps back to an address. The rom is
 * PPL_BATCH_ADDR bytes because linker_big.ld ends it exactly where the batch
 * window starts, so this cannot be outgrown without moving that too. */
#define COV_BITMAP_BYTES (PPL_BATCH_ADDR / 32u)

extern uint8_t g_covBitmap[COV_BITMAP_BYTES];

/* One `COVBITS:` count and one `COV:` hex dump, at the end of a batch. */
void covReport(void);

#endif // JIT_ARMV6M_FUZZ_QEMU_EXEC_COV_RT_H_
