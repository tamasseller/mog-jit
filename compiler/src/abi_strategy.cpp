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
    Assembler::AtomicBlock atomic(a, /*poolEntries=*/0);
    a.emit(ArmV6M::add(ArmV6M::AnyReg(ENTRY_OFFSET_REG), ArmV6M::AnyReg(PC))); // ADD r2, r2, pc
    a.emit(ArmV6M::bx(ArmV6M::AnyReg(ENTRY_OFFSET_REG)));                     // BX r2
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

static constexpr uint32_t CALL_SEQUENCE_HALFWORDS = 1 /*record*/ + 1 /*calleeIndex*/ + 3 /*movHi + ldr(callHelper) + bx*/;
static constexpr uint32_t CALL_SEQUENCE_BYTES = CALL_SEQUENCE_HALFWORDS * 2;

void abiEmitCall(Assembler &a, uint32_t procIdx, uint32_t calleeIndex)
{
    uint32_t preCallPc = a.pc();

    uint32_t k = (preCallPc - STUB_SIZE) + CALL_SEQUENCE_HALFWORDS * 2;
    uint32_t record = packRecord(procIdx, k + 1);

    Assembler::AtomicBlock atomic(a, /*poolEntries=*/2);
    a.materializeImm32(ENTRY_IDX_REG, record, false);
    a.materializeImm32(ENTRY_OFFSET_REG, calleeIndex, false);
    a.emit(ArmV6M::mov(ArmV6M::AnyReg(ENTRY_JUMP_REG), ArmV6M::AnyReg(HELPER_VEC_REG)));
    a.emit(ArmV6M::ldr(R(ENTRY_JUMP_REG), R(ENTRY_JUMP_REG), ArmV6M::Uoff<2, 5>(HELPER_CALL_OFFSET)));
    a.emit(ArmV6M::bx(ArmV6M::AnyReg(ENTRY_JUMP_REG)));
}

void abiEmitReturn(Assembler &a, bool savesLR, uint32_t initialSpilledCount)
{
    Assembler::AtomicBlock atomic(a, /*poolEntries=*/1);

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

void abiEmitTrap(Assembler &a)
{
    Assembler::AtomicBlock atomic(a, /*poolEntries=*/0);
    a.emit(ArmV6M::mov(ArmV6M::AnyReg(ENTRY_JUMP_REG), ArmV6M::AnyReg(HELPER_VEC_REG)));
    a.emit(ArmV6M::ldr(R(ENTRY_JUMP_REG), R(ENTRY_JUMP_REG), ArmV6M::Uoff<2, 5>(HELPER_TRAP_OFFSET)));
    a.emit(ArmV6M::bx(ArmV6M::AnyReg(ENTRY_JUMP_REG)));
}

} // namespace jitc
