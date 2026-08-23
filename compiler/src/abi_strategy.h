// jit-armv6m/compiler — the real ABI's call/return sequences (docs/
// design.md §6/§7/§9). This native compiler only ever targets the real
// dispatch/eviction runtime (jit-armv6m/runtime).
#ifndef JIT_ARMV6M_COMPILER_ABI_STRATEGY_H_
#define JIT_ARMV6M_COMPILER_ABI_STRATEGY_H_

#include <cstdint>

namespace jitc
{

class Emitter;

constexpr uint32_t STUB_SIZE = 12; // bytes — 6 halfwords; test_abi_strategy.cpp asserts this against emitPrologueStub()'s own emitted length

/** The fixed 6-instruction sequence every compiled procedure starts with,
 *  that runtime.S's translatorTrampoline and callHelper/returnHelper* all
 *  resume into byte-for-byte as-is. A live ABI boundary, not open for
 *  revision here. */
void emitPrologueStub(Emitter &e);

/** The fixed stub above, followed by push{lr} if this procedure needs it
 *  protected — i.e. it makes at least one nested CALL of its own, which
 *  would otherwise clobber the incoming call/return record before this
 *  procedure's own RETURN can retrieve it (the record travels in lr now,
 *  not on the stack — runtime.S's callHelper). */
void abiEmitPrologue(Emitter &e, bool savesLR);

/** Packs procIdx and offsetPlus1 into the call/return record word —
 *  procIdx in the low 16 bits, offsetPlus1 in the high 16. */
uint32_t packRecord(uint32_t procIdx, uint32_t offsetPlus1);

/** procIdx is this procedure's own dispatch-table index (packRecord's own
 *  argument). The resume offset k is found by a 5-round fixed-point search
 *  (only the converged form is actually emitted). Unaffected by
 *  abiEmitPrologue's own conditional push{lr}: e.pc() already reflects it,
 *  the same as any other emitted instruction — STUB_SIZE only ever
 *  measures from right after the fixed stub. */
void abiEmitCall(Emitter &e, uint32_t procIdx, uint32_t calleeIndex);

/** savesLR and initialSpilledCount (= max(0, argCount - WINDOW_SIZE))
 *  together select which of runtime.S's three dispatch targets this
 *  procedure's own RETURN/TRAP reaches: returnHelperFromLr (the record is
 *  still in lr, untouched since entry), returnHelperFromStack (pop it —
 *  this procedure's own prologue pushed it), or — the rare case, savesLR
 *  *and* initialSpilledCount > 0 — an inline pop+incrSp here (neither
 *  shared variant can both retrieve the record *and* reclaim this
 *  procedure's own out-of-window arguments below it, which needs a
 *  per-procedure byte count no parameterless shared routine can know)
 *  followed by a tail-jump into the bare returnHelperTail. */
void abiEmitReturn(Emitter &e, bool savesLR, uint32_t initialSpilledCount);

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_ABI_STRATEGY_H_
