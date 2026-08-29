#include "translate_internal.h"
#include "abi_strategy.h"
#include "decode_instr.h"
#include "runtime_internal.h"

using namespace jitc;

using R = ArmV6M::LoReg;

static constexpr uint32_t TRANSLATE_BODY_STACK_MARGIN = 224;

bool jitc::emitNarrowBranch(Assembler &a, Label &label, ArmV6M::Condition condition)
{
    return a.branchTo(label, condition);
}

bool jitc::emitWideBranch(Assembler &a, Label &label, ArmV6M::Condition condition)
{
    Label fallThrough;

    const auto branchOk = a.branchTo(fallThrough, ArmV6M::inverse(condition));
    assert(branchOk);

    if(!a.branchTo(label))
    {
        return false;
    }

    const auto bindOk = a.bind(fallThrough);
    assert(bindOk);

    return true;
}

bool Ctx::checkStackFloor()
{
    register uint32_t sp asm("sp");

    if(sp < TRANSLATE_BODY_STACK_MARGIN || sp - TRANSLATE_BODY_STACK_MARGIN < a.runtime.liveStackFloor())
    {
        runtimeBail(&a.runtime, RESOURCE_EXHAUSTED_TRANSLATOR_STACK);
        return false;
    }

    return true;
}

ArmV6M::Condition testAccNonzero(Assembler &a, AccState &accState)
{
    uint32_t r = accState.peek().peek(a, SCRATCH_REG);
    a.emit(ArmV6M::cmp(R(r), ArmV6M::Imm<8>(0)));
    return ArmV6M::Condition::NE;
}

void Ctx::localJumpCleanup(uint32_t tos)
{
    this->accState.flushLive(a, ACC_REG);
    if(!this->window.restore(a, tos))
    {
        runtimeBail(&a.runtime, RESOURCE_LIMIT_WINDOW_RECLAIM);
    }
}

void Ctx::handleGlobalJump(Instr term, uint32_t tos)
{
    if(term.op == Op::RETURN)
    {
        this->accState.flush(a, ACC_REG);

        if(!this->window.discard(a))
        {
            runtimeBail(&a.runtime, RESOURCE_LIMIT_WINDOW_RECLAIM);
        }

        abiEmitReturn(a, this->savesLR, this->initialSpilledCount);
    }
    else
    {
        assert(term.op == Op::TRAP);

        a.materializeImm32(ACC_REG, (uint32_t)term.imm);
        abiEmitTrap(a);
    }

    this->accState.poison();
    this->window.tos = tos;
}

uint32_t Ctx::translateIfThen(uint32_t pc, EmitBranch emitBranch)
{
    if(!checkStackFloor()) return -1;

    const auto entryTos = this->window.tos;
    const bool fused = this->hasPendingComparisonCondition;
    this->hasPendingComparisonCondition = false;

    Label skip;

    const auto cond = fused ? this->pendingComparisonCondition : testAccNonzero(a, this->accState);

    if(!emitBranch(a, skip, cond))
    {
        return -1;
    }

    if(fused)
    {
        this->accState.producer(Shape::ofImm(0));
    }
    
    if(DecodedInstr term; this->processUntilTerminator(pc, emitBranch, false, term))
    {
        if(term.instr.op == Op::BLOCK_END)
        {
            this->localJumpCleanup(entryTos);
        }
        else
        {
            this->handleGlobalJump(term.instr, entryTos);
        }

        this->accState.poison();

        if(a.bind(skip))
        {
            return term.next;
        }
    }

    return -1;
}

uint32_t Ctx::translateIfThenElse(uint32_t pc, EmitBranch emitBranch)
{
    if(!checkStackFloor()) return -1;


    const auto entryTos = this->window.tos;
    const bool fused = this->hasPendingComparisonCondition;
    this->hasPendingComparisonCondition = false;

    Label end, otherwise;

    if(fused)
    {
        if(!emitBranch(a, otherwise, this->pendingComparisonCondition))
        {
            return -1;
        }
    }
    else
    {
        this->accState.flush(a, ACC_REG);

        a.emit(ArmV6M::cmp(R(ACC_REG), ArmV6M::Imm<8>(1)));

        if(!emitBranch(a, end, ArmV6M::Condition::HI))
        {
            return -1;
        }

        a.emit(ArmV6M::cmp(R(ACC_REG), ArmV6M::Imm<8>(1)));

        if(!emitBranch(a, otherwise, ArmV6M::Condition::EQ))
        {
            return -1;
        }
    }

    this->accState.producer(Shape::ofImm(0));

    if(DecodedInstr term; this->processUntilTerminator(pc, emitBranch, false, term))
    {
        if(term.instr.op == Op::BLOCK_END)
        {
            this->localJumpCleanup(entryTos);
            if(!a.branchTo(end))
            {
                return -1;
            }
        }
        else
        {
            this->handleGlobalJump(term.instr, entryTos);
        }

        a.flushPool();
        if(!a.bind(otherwise))
        {
            return -1;
        }

        this->accState.producer(Shape::ofImm(1));

        if(DecodedInstr term2; this->processUntilTerminator(term.next, emitBranch, false, term2))
        {
            if(term2.instr.op == Op::BLOCK_END)
            {
                this->localJumpCleanup(entryTos);
            }
            else
            {
                this->handleGlobalJump(term2.instr, entryTos);
            }

            this->accState.poison();

            if(end.chain != -1)
            {
                if(!a.bind(end))
                {
                    return -1;
                }
            }

            return term2.next;
        }
    }

    return -1;
}

uint32_t Ctx::translateSwitch(uint32_t pc, EmitBranch emitBranch, uint32_t n)
{
    if(!checkStackFloor()) return -1;


    const auto entryTos = this->window.tos;
    assert(this->hasPendingComparisonCondition == false);

    this->accState.flush(a, ACC_REG);
    a.materializeImm32(SCRATCH_REG, n);

    uint32_t tableBytes = 6 + (n + 1) * 2;

    uint32_t base;
    {
        Assembler::AtomicBlock atomic(a, /*poolEntries=*/0, tableBytes);
        a.emit(ArmV6M::mov(ArmV6M::AnyReg(ENTRY_JUMP_REG), ArmV6M::AnyReg(HELPER_VEC_REG)));
        a.emit(ArmV6M::ldr(R(ENTRY_JUMP_REG), R(ENTRY_JUMP_REG), ArmV6M::Uoff<2, 5>(HELPER_BR_TABLE_JUMP_OFFSET)));
        a.emit(ArmV6M::blx(ArmV6M::AnyReg(ENTRY_JUMP_REG)));

        base = a.pc();
        for(uint32_t i = 0; i <= n; i++)
        {
            a.emit(0);
        }
    }

    a.flushPool();

    Label end;

    for(uint32_t i = 0; i < n; i++)
    {
        a.patchRawHalfword(base + i * 2, (uint16_t)(a.pc() - base));

        this->accState.poison();

        if(DecodedInstr term; this->processUntilTerminator(pc, emitBranch, false, term))
        {
            if(term.instr.op == Op::BLOCK_END)
            {
                this->localJumpCleanup(entryTos);
                if(i + 1 < n)
                {
                    if(!a.branchTo(end))
                    {
                        return -1;
                    }

                    a.flushPool();
                }
            }
            else
            {
                this->handleGlobalJump(term.instr, entryTos);
                a.flushPool();
            }

            pc = term.next;
        }
        else
        {
            return -1;
        }
    }

    a.patchRawHalfword(base + n * 2, (uint16_t)(a.pc() - base));

    this->accState.poison();

    if(end.chain != -1)
    {
        if(!a.bind(end))
        {
            return -1;
        }
    }

    return pc;
}

uint32_t Ctx::translateLoop(uint32_t pc, EmitBranch emitBranch)
{
    if(!checkStackFloor()) return -1;

    const auto entryTos = this->window.tos;

    this->accState.flushLive(a, ACC_REG);
    const auto start = a.pc();

    if(DecodedInstr condTerm; this->processUntilTerminator(pc, emitBranch, true, condTerm))
    {
        assert(condTerm.instr.op == Op::BLOCK_END);

        if(this->window.tos != entryTos)
        {
            this->accState.flushLive(a, ACC_REG);
            if(!this->window.restore(a, entryTos))
            {
                runtimeBail(&a.runtime, RESOURCE_LIMIT_WINDOW_RECLAIM);
            }
        }

        const bool fused = this->hasPendingComparisonCondition;
        this->hasPendingComparisonCondition = false;

        const auto cond = fused ? this->pendingComparisonCondition : testAccNonzero(a, this->accState);

        Label out;
        if(!emitBranch(a, out, ArmV6M::inverse(cond)))
        {
            return -1;
        }

        if(fused)
        {
            this->accState.producer(Shape::ofImm(1));
        }

        if(DecodedInstr bodyTerm; this->processUntilTerminator(condTerm.next, emitBranch, false, bodyTerm))
        {
            if(bodyTerm.instr.op == Op::BLOCK_END)
            {
                this->localJumpCleanup(entryTos);
                int32_t delta = (int32_t)start - (int32_t)(a.pc() + 4);
                if(!ArmV6M::Ioff<1, 11>::isInRange(delta))
                {
                    runtimeBail(&a.runtime, RESOURCE_LIMIT_LOOP_BACK_EDGE);
                    return -1;
                }
                a.emit(ArmV6M::b(ArmV6M::Ioff<1, 11>((int16_t)delta)));
            }
            else
            {
                this->handleGlobalJump(bodyTerm.instr, entryTos);
            }

            a.flushPool();

            this->accState.poison();

            if(!a.bind(out))
            {
                return -1;
            }

            return bodyTerm.next;
        }
    }
    
    return -1;
}

bool Ctx::translateBody(EmitBranch emitBranch)
{
    if(!checkStackFloor()) return false;

    abiEmitPrologue(a, savesLR);

    if(window.tos >= 1)
    {
        accState.flush(a, physReg(window.tos - 1));
    }

    if(DecodedInstr decoded; processUntilTerminator(0, emitBranch, false, decoded))
    {
        const Instr &instr = decoded.instr;

        assert(instr.op == Op::RETURN || instr.op == Op::TRAP); // GCOV_EXCL_LINE — BLOCK_END with no open block; malformed program
        this->handleGlobalJump(instr, this->window.tos);
        assert(decoded.next == this->bytesLen);

        return true;
    }

    return false;
}
