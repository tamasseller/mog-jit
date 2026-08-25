#include "abi_strategy.h"
#include "emitter.h"
#include "registers.h"
#include "imm_synth.h"
#include "armv6.h"

#include <cassert>

namespace jitc
{

using R = ArmV6M::LoReg;

namespace
{

constexpr uint32_t PC = 15;

} // namespace

void emitPrologueStub(Emitter &e)
{
    e.emit(ArmV6M::mov(ArmV6M::AnyReg(ENTRY_JUMP_REG), ArmV6M::AnyReg(LRU_TICK_REG))); // MOV r3, r11 — low-mirror the LRU tick
    e.emit(ArmV6M::str(R(ENTRY_JUMP_REG), R(ENTRY_IDX_REG), ArmV6M::Uoff<2, 5>(4)));   // STR r3, [r1, #4] — entry.last_used = old tick
    e.emit(ArmV6M::adds(R(ENTRY_JUMP_REG), ArmV6M::Imm<8>(1)));                        // ADDS r3, r3, #1
    e.emit(ArmV6M::mov(ArmV6M::AnyReg(LRU_TICK_REG), ArmV6M::AnyReg(ENTRY_JUMP_REG))); // MOV r11, r3 — publish the bumped tick
    e.emit(ArmV6M::add(ArmV6M::AnyReg(ENTRY_OFFSET_REG), ArmV6M::AnyReg(PC)));         // ADD r2, r2, pc
    e.emit(ArmV6M::bx(ArmV6M::AnyReg(ENTRY_OFFSET_REG)));                              // BX r2
}

void abiEmitPrologue(Emitter &e, bool savesLR)
{
    emitPrologueStub(e);
    if(savesLR)
    {
        e.emit(ArmV6M::pushWithLr(ArmV6M::LoRegs{0}));
    }
}

uint32_t packRecord(uint32_t procIdx, uint32_t offsetPlus1)
{
    return (procIdx & 0xffffu) | (offsetPlus1 << 16);
}

namespace
{

uint32_t buildCallSequenceLength(uint32_t procIdx, uint32_t calleeIndex, uint32_t k)
{
    uint32_t record = packRecord(procIdx, k + 1);
    uint32_t len = synthesizeImm32Length(record);
    len += fitsImm8((int32_t)calleeIndex) ? 1 : synthesizeImm32Length(calleeIndex);
    len += 3; // movHi + ldr(callHelper) + bx
    return len;
}

void buildCallSequenceEmit(Emitter &e, uint32_t procIdx, uint32_t calleeIndex, uint32_t k)
{
    uint32_t record = packRecord(procIdx, k + 1);
    emitSynthesizeImm32(e, ENTRY_IDX_REG, record);
    if(fitsImm8((int32_t)calleeIndex))
    {
        e.emit(ArmV6M::movs(R(ENTRY_OFFSET_REG), ArmV6M::Imm<8>((uint16_t)calleeIndex)));
    }
    else
    {
        emitSynthesizeImm32(e, ENTRY_OFFSET_REG, calleeIndex);
    }
    e.emit(ArmV6M::mov(ArmV6M::AnyReg(ENTRY_JUMP_REG), ArmV6M::AnyReg(HELPER_VEC_REG)));
    e.emit(ArmV6M::ldr(R(ENTRY_JUMP_REG), R(ENTRY_JUMP_REG), ArmV6M::Uoff<2, 5>(0))); // callHelper, index 0
    e.emit(ArmV6M::bx(ArmV6M::AnyReg(ENTRY_JUMP_REG)));
}

/** K is a byte offset from the procedure's own body start (past the
 *  fixed-size stub) to this sequence's own resume point — which depends
 *  on how many instructions this same sequence takes to encode K itself
 *  (the packed record's immediate). Fixed-point, not two-pass: stable in
 *  one or two iterations for any realistic procedure size. */
uint32_t findResumeOffset(uint32_t procIdx, uint32_t calleeIndex, uint32_t preCallPc)
{
    uint32_t guess = 0;
    for(int i = 0; i < 5; i++)
    {
        uint32_t next = (preCallPc - STUB_SIZE) + buildCallSequenceLength(procIdx, calleeIndex, guess) * 2;
        if(next == guess)
        {
            return guess;
        }
        guess = next;
    }
    assert(false && "abiEmitCall: CALL resume offset failed to converge"); // GCOV_EXCL_LINE
    return guess;
}

} // namespace

void abiEmitCall(Emitter &e, uint32_t procIdx, uint32_t calleeIndex)
{
    uint32_t preCallPc = e.pc();
    uint32_t k = findResumeOffset(procIdx, calleeIndex, preCallPc);
    buildCallSequenceEmit(e, procIdx, calleeIndex, k);
}

void abiEmitReturn(Emitter &e, bool savesLR, uint32_t initialSpilledCount)
{
    // The rare case: this procedure's own out-of-window arguments sit
    // below the pushed record, so the record's own retriever needs an
    // extra reclaim no parameterless routine can size on its own. Load
    // that one per-procedure fact — the byte count — into r2 and dispatch
    // to the shared helper that expects it there, instead of the bare
    // fetch variant.
    if(savesLR && initialSpilledCount > 0)
    {
        uint32_t bytes = 4 * initialSpilledCount;
        if(fitsImm8((int32_t)bytes))
        {
            e.emit(ArmV6M::movs(R(ENTRY_OFFSET_REG), ArmV6M::Imm<8>((uint16_t)bytes)));
        }
        else
        {
            emitSynthesizeImm32(e, ENTRY_OFFSET_REG, bytes);
        }
        e.emit(ArmV6M::mov(ArmV6M::AnyReg(ENTRY_JUMP_REG), ArmV6M::AnyReg(HELPER_VEC_REG)));
        e.emit(ArmV6M::ldr(R(ENTRY_JUMP_REG), R(ENTRY_JUMP_REG), ArmV6M::Uoff<2, 5>(28))); // returnHelperFromStackReclaim, index 7
        e.emit(ArmV6M::bx(ArmV6M::AnyReg(ENTRY_JUMP_REG)));
        return;
    }
    e.emit(ArmV6M::mov(ArmV6M::AnyReg(ENTRY_JUMP_REG), ArmV6M::AnyReg(HELPER_VEC_REG)));
    e.emit(ArmV6M::ldr(R(ENTRY_JUMP_REG), R(ENTRY_JUMP_REG), ArmV6M::Uoff<2, 5>(savesLR ? 8 : 4))); // returnHelperFromStack / returnHelperFromLr
    e.emit(ArmV6M::bx(ArmV6M::AnyReg(ENTRY_JUMP_REG)));
}

} // namespace jitc
