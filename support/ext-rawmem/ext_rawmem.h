#ifndef JIT_ARMV6M_TEST_EXT_RAWMEM_H_
#define JIT_ARMV6M_TEST_EXT_RAWMEM_H_

#include <cstdint>

/* Wire opcodes and buffer, mirrored in fuzz/ts/lib/rawmem_ext.ts. */

constexpr uint32_t RAWMEM_BYTES = 1024;

constexpr uint8_t RAWMEM_LD8 = 0x80;
constexpr uint8_t RAWMEM_LD16 = 0x81;
constexpr uint8_t RAWMEM_LD32 = 0x82;
constexpr uint8_t RAWMEM_ST8 = 0x83;
constexpr uint8_t RAWMEM_ST16 = 0x84;
constexpr uint8_t RAWMEM_ST32 = 0x85;
constexpr uint8_t RAWMEM_MEMMOVE = 0x86;
constexpr uint8_t RAWMEM_MEMCMP = 0x87;
constexpr uint8_t RAWMEM_SLICECMP = 0x88;

constexpr uint32_t RAWMEM_ADDR_MASK = RAWMEM_BYTES - 1;

/* Static, so its address is a link-time constant and no extension state is
 * needed. Zeroed before each program: the target's copy outlives one
 * program in a qemu_exec batch, the reference VM's does not. */
extern uint8_t g_rawMem[RAWMEM_BYTES];

#endif // JIT_ARMV6M_TEST_EXT_RAWMEM_H_
