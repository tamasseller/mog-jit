// jit-armv6m/compiler — the real ABI's call/return sequences (docs/
// design.md §6/§7/§9). This native compiler only ever targets the real
// dispatch/eviction runtime (jit-armv6m/runtime).
#ifndef JIT_ARMV6M_COMPILER_ABI_STRATEGY_H_
#define JIT_ARMV6M_COMPILER_ABI_STRATEGY_H_

#include <cstdint>

namespace jitc
{

class Assembler;

constexpr uint32_t STUB_SIZE = 4; // bytes — 2 halfwords; test_abi_strategy.cpp asserts this against emitPrologueStub()'s own emitted length

void emitPrologueStub(Assembler &a);

void abiEmitPrologue(Assembler &a, bool savesLR);

uint32_t packRecord(uint32_t procIdx, uint32_t offsetPlus1);

void abiEmitCall(Assembler &a, uint32_t procIdx, uint32_t calleeIndex);

void abiEmitReturn(Assembler &a, bool savesLR, uint32_t initialSpilledCount);

void abiEmitTrap(Assembler &a);

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_ABI_STRATEGY_H_
