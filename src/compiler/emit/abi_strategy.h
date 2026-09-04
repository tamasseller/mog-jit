// jit-armv6m/compiler — the real ABI's call/return sequences (docs/
// design.md §6/§7/§9). This native compiler only ever targets the real
// dispatch/eviction runtime (jit-armv6m/runtime).
#ifndef JIT_ARMV6M_COMPILER_ABI_STRATEGY_H_
#define JIT_ARMV6M_COMPILER_ABI_STRATEGY_H_

#include <cstdint>

#include "effect.h"

namespace jitc
{

class Assembler;

constexpr uint32_t STUB_SIZE = 4; // bytes — 2 halfwords; test_abi_strategy.cpp asserts this against emitPrologueStub()'s own emitted length

void emitPrologueStub(Assembler &a);

void abiEmitPrologue(Assembler &a, bool savesLR);

constexpr uint32_t MAX_PROC_IDX = 0x7fffu;
constexpr uint32_t MAX_RESUME_OFFSET = 0xffffu;

constexpr uint32_t packRecord(uint32_t procIdx, uint32_t offsetPlus1)
{
    return (procIdx & 0xffffu) | (offsetPlus1 << 16);
}

Effect abiEmitCall(Assembler &a, uint32_t procIdx, uint32_t calleeIndex);

Effect abiEmitReturn(Assembler &a, bool savesLR, uint32_t initialSpilledCount);

Effect abiEmitTrap(Assembler &a);

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_ABI_STRATEGY_H_
