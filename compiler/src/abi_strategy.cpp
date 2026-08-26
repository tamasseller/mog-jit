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
// call site regardless of value: calleeIndex either fits imm8 as a bare
// MOVS, or is force-pooled exactly like the record. That makes the whole
// sequence's own length a true compile-time constant, which is the only
// reason k (below) has a closed form at all — the record's own value
// bakes k in, so the old design (synthesizing the record inline) made the
// sequence's length depend on k, needing a 5-round fixed-point search to
// find a k whose own encoding doesn't shift the length that determined
// it. A pooled site can't have that problem: its own length never varies
// with its value.
static constexpr uint32_t CALL_SEQUENCE_HALFWORDS = 1 /*record*/ + 1 /*calleeIndex*/ + 3 /*movHi + ldr(callHelper) + bx*/;
static constexpr uint32_t CALL_SEQUENCE_BYTES = CALL_SEQUENCE_HALFWORDS * 2;

void abiEmitCall(Assembler &a, uint32_t procIdx, uint32_t calleeIndex)
{
    // Guarantee both pool slots up front — reserve() may itself flush a
    // still-open chunk (with its own branch-around), and that has to
    // finish *before* preCallPc is read below, or k would end up
    // measuring from the wrong position. After this, neither
    // materializeImm32Pooled call below can trigger a flush of its own.
    a.reserve(CALL_SEQUENCE_BYTES, /*poolEntries=*/2);
    uint32_t preCallPc = a.pc();

    uint32_t k = (preCallPc - STUB_SIZE) + CALL_SEQUENCE_HALFWORDS * 2;
    uint32_t record = packRecord(procIdx, k + 1);
    a.materializeImm32Pooled(ENTRY_IDX_REG, record);

    if(fitsImm8((int32_t)calleeIndex))
    {
        a.emit(ArmV6M::movs(R(ENTRY_OFFSET_REG), ArmV6M::Imm<8>((uint16_t)calleeIndex)));
    }
    else
    {
        a.materializeImm32Pooled(ENTRY_OFFSET_REG, calleeIndex);
    }
    a.emit(ArmV6M::mov(ArmV6M::AnyReg(ENTRY_JUMP_REG), ArmV6M::AnyReg(HELPER_VEC_REG)));
    a.emit(ArmV6M::ldr(R(ENTRY_JUMP_REG), R(ENTRY_JUMP_REG), ArmV6M::Uoff<2, 5>(0))); // callHelper, index 0
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
    if(savesLR && initialSpilledCount > 0)
    {
        uint32_t bytes = 4 * initialSpilledCount;
        if(fitsImm8((int32_t)bytes))
        {
            a.emit(ArmV6M::movs(R(ENTRY_OFFSET_REG), ArmV6M::Imm<8>((uint16_t)bytes)));
        }
        else
        {
            a.materializeImm32(ENTRY_OFFSET_REG, bytes);
        }
        a.emit(ArmV6M::mov(ArmV6M::AnyReg(ENTRY_JUMP_REG), ArmV6M::AnyReg(HELPER_VEC_REG)));
        a.emit(ArmV6M::ldr(R(ENTRY_JUMP_REG), R(ENTRY_JUMP_REG), ArmV6M::Uoff<2, 5>(28))); // returnHelperFromStackReclaim, index 7
        a.emit(ArmV6M::bx(ArmV6M::AnyReg(ENTRY_JUMP_REG)));
        return;
    }
    a.emit(ArmV6M::mov(ArmV6M::AnyReg(ENTRY_JUMP_REG), ArmV6M::AnyReg(HELPER_VEC_REG)));
    a.emit(ArmV6M::ldr(R(ENTRY_JUMP_REG), R(ENTRY_JUMP_REG), ArmV6M::Uoff<2, 5>(savesLR ? 8 : 4))); // returnHelperFromStack / returnHelperFromLr
    a.emit(ArmV6M::bx(ArmV6M::AnyReg(ENTRY_JUMP_REG)));
}

} // namespace jitc
