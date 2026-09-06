#ifndef JIT_ARMV6M_FUZZ_QEMU_EXEC_COV_RT_H_
#define JIT_ARMV6M_FUZZ_QEMU_EXEC_COV_RT_H_

#include <stdint.h>

/* 2048 bits in 256 bytes of .bss. Sized against what is left of the 8KB ram
 * once the code arena and Runtime have taken theirs — see linker_cov.ld. */
#define COV_BITMAP_BITS_LOG2 11
#define COV_BITMAP_BYTES (1u << (COV_BITMAP_BITS_LOG2 - 3))

extern uint8_t g_covBitmap[COV_BITMAP_BYTES];

/* One `COVBITS:` count and one `COV:` hex dump, at the end of a batch. */
void covReport(void);

#endif // JIT_ARMV6M_FUZZ_QEMU_EXEC_COV_RT_H_
