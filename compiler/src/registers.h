// jit-armv6m/compiler — register assignment (docs/design.md §3). Only the
// roles the real-ABI translator actually touches.
#ifndef JIT_ARMV6M_COMPILER_REGISTERS_H_
#define JIT_ARMV6M_COMPILER_REGISTERS_H_

#include <cstdint>

namespace jitc
{

constexpr uint32_t ACC_REG = 0;     // r0 — acc
constexpr uint32_t SCRATCH_REG = 2; // r2

constexpr uint32_t WINDOW_BASE = 4; // r4
constexpr uint32_t WINDOW_SIZE = 4; // r4..r7

// Real dispatch/call-return ABI (docs/design.md §3).
constexpr uint32_t ENTRY_IDX_REG = 1;              // r1
constexpr uint32_t ENTRY_OFFSET_REG = SCRATCH_REG; // r2
constexpr uint32_t ENTRY_JUMP_REG = 3;             // r3
constexpr uint32_t HELPER_VEC_REG = 10;            // r10
constexpr uint32_t LRU_TICK_REG = 11;              // r11

// Byte offsets into the flash-resident helper vector HELPER_VEC_REG points
// at (docs/design.md §11's table; runtime/runtime.S lays them out in this
// same order) — each entry is 4 bytes, so offset == index * 4. Index 3
// (returnHelperTail) is never loaded directly by compiled code, only
// reached by fallthrough/branch from index 2/7 or 1, so it has no constant
// here.
constexpr uint32_t HELPER_CALL_OFFSET = 0;                       // callHelper (index 0)
constexpr uint32_t HELPER_RETURN_FROM_LR_OFFSET = 4;             // returnHelperFromLr (index 1)
constexpr uint32_t HELPER_RETURN_FROM_STACK_OFFSET = 8;          // returnHelperFromStack (index 2)
constexpr uint32_t HELPER_CLZ_OFFSET = 16;                       // clzHelper (index 4)
constexpr uint32_t HELPER_REVBITS_OFFSET = 20;                   // revbitsHelper (index 5)
constexpr uint32_t HELPER_BR_TABLE_JUMP_OFFSET = 24;             // brTableJumpHelper (index 6)
constexpr uint32_t HELPER_RETURN_FROM_STACK_RECLAIM_OFFSET = 28; // returnHelperFromStackReclaim (index 7)

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_REGISTERS_H_
