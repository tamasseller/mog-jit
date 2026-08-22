// jit-armv6m/compiler — register assignment, ported from
// jit-armv6m/prototype/src/registers.ts. Only the roles the real-ABI,
// straight-line-only translator actually touches this slice.
#ifndef JIT_ARMV6M_COMPILER_REGISTERS_H_
#define JIT_ARMV6M_COMPILER_REGISTERS_H_

#include <cstdint>

namespace jitc {

constexpr uint32_t ACC_REG = 0;    // r0 — acc
constexpr uint32_t SCRATCH_REG = 2; // r2

constexpr uint32_t WINDOW_BASE = 4; // r4
constexpr uint32_t WINDOW_SIZE = 4; // r4..r7

// Real dispatch/call-return ABI (docs/design.md §3).
constexpr uint32_t ENTRY_IDX_REG = 1;              // r1
constexpr uint32_t ENTRY_OFFSET_REG = SCRATCH_REG; // r2
constexpr uint32_t ENTRY_JUMP_REG = 3;             // r3
constexpr uint32_t HELPER_VEC_REG = 10;            // r10
constexpr uint32_t LRU_TICK_REG = 11;              // r11

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_REGISTERS_H_
