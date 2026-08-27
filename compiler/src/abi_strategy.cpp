#include "abi_strategy.h"
#include "assembler.h"
#include "registers.h"
#include "imm_synth.h"
#include "armv6.h"

namespace jitc
{

using R = ArmV6M::LoReg;

static constexpr uint32_t PC = 15;

void emitPrologueStub(Assembler &a)
{
    // Fixed-length (STUB_SIZE, asserted by test_abi_strategy.cpp against
    // this function's own emitted length) and self-relocating (the ADD
    // r2,r2,pc below) — a pool flush landing anywhere in here would break
    // both.
    Assembler::AtomicScope atomic(a);
    a.emit(ArmV6M::mov(ArmV6M::AnyReg(ENTRY_JUMP_REG), ArmV6M::AnyReg(LRU_TICK_REG))); // MOV r3, r11 — low-mirror the LRU tick
    a.emit(ArmV6M::str(R(ENTRY_JUMP_REG), R(ENTRY_IDX_REG), ArmV6M::Uoff<2, 5>(4)));   // STR r3, [r1, #4] — entry.last_used = old tick
    a.emit(ArmV6M::adds(R(ENTRY_JUMP_REG), ArmV6M::Imm<8>(1)));                        // ADDS r3, r3, #1
    a.emit(ArmV6M::mov(ArmV6M::AnyReg(LRU_TICK_REG), ArmV6M::AnyReg(ENTRY_JUMP_REG))); // MOV r11, r3 — publish the bumped tick
    a.emit(ArmV6M::add(ArmV6M::AnyReg(ENTRY_OFFSET_REG), ArmV6M::AnyReg(PC)));         // ADD r2, r2, pc
    a.emit(ArmV6M::bx(ArmV6M::AnyReg(ENTRY_OFFSET_REG)));                              // BX r2
}

void abiEmitPrologue(Assembler &a, bool savesLR)
{
    emitPrologueStub(a);
    if(savesLR)
    {
        a.emit(ArmV6M::pushWithLr(ArmV6M::LoRegs{0}));
    }
}

uint32_t packRecord(uint32_t procIdx, uint32_t offsetPlus1)
{
    return (procIdx & 0xffffu) | (offsetPlus1 << 16);
}

// Both of a CALL sequence's own operands cost exactly one halfword at the
// call site regardless of value: materializeImm32(..., false) below
// disallows its own two-instruction-sequence forms, so it can only ever
// emit a bare MOVS (value fits imm8) or a pooled placeholder (it
// doesn't) — never anything wider. That makes the whole sequence's own
// length a true compile-time constant, which is the only reason k
// (below) has a closed form at all — the record's own value bakes k in,
// so the old design (synthesizing the record inline) made the sequence's
// length depend on k, needing a 5-round fixed-point search to find a k
// whose own encoding doesn't shift the length that determined it. Both
// the bare-MOVS and the pooled shapes are exactly one halfword, so
// neither can have that problem.
static constexpr uint32_t CALL_SEQUENCE_HALFWORDS = 1 /*record*/ + 1 /*calleeIndex*/ + 3 /*movHi + ldr(callHelper) + bx*/;
static constexpr uint32_t CALL_SEQUENCE_BYTES = CALL_SEQUENCE_HALFWORDS * 2;

void abiEmitCall(Assembler &a, uint32_t procIdx, uint32_t calleeIndex)
{
    // Reserved for both potential pool entries below *before* the atomic
    // scope starts — LITERAL_POOL_REACH_MARGIN (assembler.cpp) is already
    // sized for exactly this: a whole call sequence's worst case
    // (blocks.h's CALL_MAX_BYTES=64), comfortably more than
    // CALL_SEQUENCE_BYTES actually is. Nothing inside the scope below can
    // need to flush.
    a.ensurePoolRoom(2);

    uint32_t preCallPc = a.pc();

    uint32_t k = (preCallPc - STUB_SIZE) + CALL_SEQUENCE_HALFWORDS * 2;
    uint32_t record = packRecord(procIdx, k + 1);

    // record bakes in k, a closed-form offset computed from this
    // sequence's own fixed CALL_SEQUENCE_BYTES — a pool flush landing
    // anywhere in here would change that length and invalidate it.
    Assembler::AtomicScope atomic(a);
    a.materializeImm32(ENTRY_IDX_REG, record, false);
    a.materializeImm32(ENTRY_OFFSET_REG, calleeIndex, false);
    a.emit(ArmV6M::mov(ArmV6M::AnyReg(ENTRY_JUMP_REG), ArmV6M::AnyReg(HELPER_VEC_REG)));
    a.emit(ArmV6M::ldr(R(ENTRY_JUMP_REG), R(ENTRY_JUMP_REG), ArmV6M::Uoff<2, 5>(HELPER_CALL_OFFSET)));
    a.emit(ArmV6M::bx(ArmV6M::AnyReg(ENTRY_JUMP_REG)));
}

void abiEmitReturn(Assembler &a, bool savesLR, uint32_t initialSpilledCount)
{
    // The rare case: this procedure's own out-of-window arguments sit
    // below the pushed record, so the record's own retriever needs an
    // extra reclaim no parameterless routine can size on its own. Load
    // that one per-procedure fact — the byte count — into r2 and dispatch
    // to the shared helper that expects it there, instead of the bare
    // fetch variant. Unlike the call record above, this value has no
    // self-reference to its own encoded length (nothing forward of here
    // depends on knowing how many bytes this took), so it can go through
    // the ordinary pool-or-synthesize materializer with no closed-form
    // concern at all.
    // Reserved up front, same reasoning as abiEmitCall — the one
    // materializeImm32 call below is the only pool-eligible spot in this
    // whole function, and this sequence's own length is well within
    // LITERAL_POOL_REACH_MARGIN either way.
    a.ensurePoolRoom(1);

    // A helper-vector jump sequence — kept contiguous like the other
    // three, though (unlike abiEmitCall's record) nothing here is
    // actually self-referential.
    Assembler::AtomicScope atomic(a);

    if(savesLR && initialSpilledCount > 0)
    {
        a.materializeImm32(ENTRY_OFFSET_REG, 4 * initialSpilledCount);
        a.emit(ArmV6M::mov(ArmV6M::AnyReg(ENTRY_JUMP_REG), ArmV6M::AnyReg(HELPER_VEC_REG)));
        a.emit(ArmV6M::ldr(R(ENTRY_JUMP_REG), R(ENTRY_JUMP_REG), ArmV6M::Uoff<2, 5>(HELPER_RETURN_FROM_STACK_RECLAIM_OFFSET)));
        a.emit(ArmV6M::bx(ArmV6M::AnyReg(ENTRY_JUMP_REG)));
        return;
    }
    a.emit(ArmV6M::mov(ArmV6M::AnyReg(ENTRY_JUMP_REG), ArmV6M::AnyReg(HELPER_VEC_REG)));
    a.emit(ArmV6M::ldr(R(ENTRY_JUMP_REG), R(ENTRY_JUMP_REG), ArmV6M::Uoff<2, 5>(savesLR ? HELPER_RETURN_FROM_STACK_OFFSET : HELPER_RETURN_FROM_LR_OFFSET)));
    a.emit(ArmV6M::bx(ArmV6M::AnyReg(ENTRY_JUMP_REG)));
}

} // namespace jitc
