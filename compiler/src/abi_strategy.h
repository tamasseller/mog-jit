// jit-armv6m/compiler — the real ABI's call/return sequences, ported from
// jit-armv6m/prototype/src/translateProc.ts's abiRealStrategy and
// jit-armv6m/prototype/src/runtime.ts's emitPrologueStub/packRecord.
// noEvictionStrategy is TS-only scaffolding (a plain BL stand-in for a
// runtime with no dispatch table) and is NOT ported — this native compiler
// only ever targets the real dispatch/eviction runtime.
#ifndef JIT_ARMV6M_COMPILER_ABI_STRATEGY_H_
#define JIT_ARMV6M_COMPILER_ABI_STRATEGY_H_

#include <cstdint>

namespace jitc {

class Emitter;

constexpr uint32_t STUB_SIZE = 12; // bytes — 6 halfwords; test_abi_strategy.cpp asserts this against emitPrologueStub()'s own emitted length

/** runtime.ts's emitPrologueStub, ported verbatim — the fixed 6-instruction
 *  sequence every compiled procedure starts with, that runtime.S's
 *  translator_trampoline and callHelper/returnHelper* all resume into
 *  byte-for-byte as-is. A live ABI boundary, not open for revision here. */
void emitPrologueStub(Emitter &e);

/** translateProc.ts's abiRealStrategy.emitPrologue: the fixed stub above,
 *  followed by push{lr} if this procedure needs it protected — i.e. it
 *  makes at least one nested CALL of its own, which would otherwise
 *  clobber the incoming call/return record before this procedure's own
 *  RETURN can retrieve it (the record travels in lr now, not on the
 *  stack — qemu/runtime.S's callHelper). */
void abiEmitPrologue(Emitter &e, bool savesLR);

/** runtime.ts's packRecord, ported verbatim. */
uint32_t packRecord(uint32_t procIdx, uint32_t offsetPlus1);

/** translateProc.ts's abiRealStrategy.emitCall. procIdx is this
 *  procedure's own dispatch-table index (packRecord's own argument). The
 *  resume offset k is found by the same 5-round fixed-point search as the
 *  original (only the converged form is actually emitted). Unaffected by
 *  abiEmitPrologue's own conditional push{lr}: e.pc() already reflects it,
 *  the same as any other emitted instruction — STUB_SIZE only ever
 *  measures from right after the fixed stub. */
void abiEmitCall(Emitter &e, uint32_t procIdx, uint32_t calleeIndex);

/** translateProc.ts's abiRealStrategy.emitReturn. savesLR and
 *  initialSpilledCount (= max(0, argCount - WINDOW_SIZE)) together select
 *  which of qemu/runtime.S's three dispatch targets this procedure's own
 *  RETURN/TRAP reaches: returnHelperFromLr (the record is still in lr,
 *  untouched since entry), returnHelperFromStack (pop it — this
 *  procedure's own prologue pushed it), or — the rare case, savesLR *and*
 *  initialSpilledCount > 0 — an inline pop+incrSp here (neither shared
 *  variant can both retrieve the record *and* reclaim this procedure's
 *  own out-of-window arguments below it, which needs a per-procedure byte
 *  count no parameterless shared routine can know) followed by a
 *  tail-jump into the bare returnHelperTail. */
void abiEmitReturn(Emitter &e, bool savesLR, uint32_t initialSpilledCount);

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_ABI_STRATEGY_H_
