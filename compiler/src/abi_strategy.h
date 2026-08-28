// jit-armv6m/compiler — the real ABI's call/return sequences (docs/
// design.md §6/§7/§9). This native compiler only ever targets the real
// dispatch/eviction runtime (jit-armv6m/runtime).
#ifndef JIT_ARMV6M_COMPILER_ABI_STRATEGY_H_
#define JIT_ARMV6M_COMPILER_ABI_STRATEGY_H_

#include <cstdint>

namespace jitc
{

class Assembler;

constexpr uint32_t STUB_SIZE = 12; // bytes — 6 halfwords; test_abi_strategy.cpp asserts this against emitPrologueStub()'s own emitted length

/** The fixed 6-instruction sequence every compiled procedure starts with,
 *  that runtime.S's translatorTrampoline and callHelper/returnHelper* all
 *  resume into byte-for-byte as-is. A live ABI boundary, not open for
 *  revision here. */
void emitPrologueStub(Assembler &a);

/** The fixed stub above, followed by push{lr} if this procedure needs it
 *  protected — i.e. it makes at least one nested CALL of its own, which
 *  would otherwise clobber the incoming call/return record before this
 *  procedure's own RETURN can retrieve it (the record travels in lr now,
 *  not on the stack — runtime.S's callHelper). */
void abiEmitPrologue(Assembler &a, bool savesLR);

/** Packs procIdx and offsetPlus1 into the call/return record word —
 *  procIdx in the low 16 bits, offsetPlus1 in the high 16. */
uint32_t packRecord(uint32_t procIdx, uint32_t offsetPlus1);

/** procIdx is this procedure's own dispatch-table index (packRecord's own
 *  argument). The resume offset k is closed-form, not a fixed-point
 *  search: the record goes through materializeImm32 with its two-
 *  instruction-sequence forms disallowed, which always costs exactly one
 *  halfword at the call site regardless of value — a bare MOVS when the
 *  value fits imm8, otherwise a pooled placeholder (the 4-byte word
 *  lands at flush time, outside this sequence) — so its own emitted
 *  length can never depend on the value it encodes. The resume offset it
 *  encodes could otherwise never be computed without already knowing it.
 *  Unaffected by abiEmitPrologue's own conditional push{lr}: a.pc()
 *  already reflects it, the same as any other emitted instruction —
 *  STUB_SIZE only ever measures from right after the fixed stub. */
void abiEmitCall(Assembler &a, uint32_t procIdx, uint32_t calleeIndex);

/** savesLR and initialSpilledCount (= max(0, argCount - WINDOW_SIZE))
 *  together select which of runtime.S's four dispatch targets this
 *  procedure's own RETURN/TRAP reaches: returnHelperFromLr (the record is
 *  still in lr, untouched since entry), returnHelperFromStack (pop it —
 *  this procedure's own prologue pushed it), or, only when
 *  initialSpilledCount > 0 too, returnHelperFromStackReclaim — the same
 *  pop, plus an `add sp, sp, r2` for the out-of-window arguments sitting
 *  below the pushed record. r2 is loaded here with the one thing no
 *  parameterless routine can know: this procedure's own byte count. */
void abiEmitReturn(Assembler &a, bool savesLR, uint32_t initialSpilledCount);

/** A bytecode TRAP's own dispatch (isa-core.md §4.5): jump to runtime.S's
 *  trapHelper with the trap code already in ACC_REG. Takes neither of
 *  abiEmitReturn's two parameters and emits no teardown of its own,
 *  because it isn't a return — trapHelper restores the excursion's whole
 *  saved sp, which subsumes every window spill and pushed call record in
 *  one instruction, at any call depth. */
void abiEmitTrap(Assembler &a);

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_ABI_STRATEGY_H_
