#ifndef JIT_ARMV6M_COMPILER_REGISTERS_H_
#define JIT_ARMV6M_COMPILER_REGISTERS_H_

#include <cstdint>
#include <cstddef>

struct HelperVec
{
    uint32_t call;
    uint32_t returnFromLr;
    uint32_t returnFromStack;
    uint32_t returnTail;
    uint32_t clz;
    uint32_t revbits;
    uint32_t brTableJump;
    uint32_t returnFromStackReclaim;
    uint32_t trap;
    uint32_t extThunk;
};

extern "C" const HelperVec helperVec;

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
constexpr uint32_t RUNTIME_PTR_REG = 9;            // r9 — docs/design.md §3
constexpr uint32_t HELPER_VEC_REG = 10;            // r10
constexpr uint32_t LRU_TICK_REG = 11;              // r11

constexpr uint32_t HELPER_CALL_OFFSET = offsetof(HelperVec, call);
constexpr uint32_t HELPER_RETURN_FROM_LR_OFFSET = offsetof(HelperVec, returnFromLr);
constexpr uint32_t HELPER_RETURN_FROM_STACK_OFFSET = offsetof(HelperVec, returnFromStack);
constexpr uint32_t HELPER_CLZ_OFFSET = offsetof(HelperVec, clz);
constexpr uint32_t HELPER_REVBITS_OFFSET = offsetof(HelperVec, revbits);
constexpr uint32_t HELPER_BR_TABLE_JUMP_OFFSET = offsetof(HelperVec, brTableJump);
constexpr uint32_t HELPER_RETURN_FROM_STACK_RECLAIM_OFFSET = offsetof(HelperVec, returnFromStackReclaim);
constexpr uint32_t HELPER_TRAP_OFFSET = offsetof(HelperVec, trap);
constexpr uint32_t HELPER_EXT_THUNK_OFFSET = offsetof(HelperVec, extThunk);

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_REGISTERS_H_
