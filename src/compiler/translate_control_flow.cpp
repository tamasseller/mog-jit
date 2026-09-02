#include "translate_internal.h"
#include "abi_strategy.h"
#include "decode_instr.h"
#include "runtime.h"

using namespace jitc;

using R = ArmV6M::LoReg;

static bool emitBranch(Assembler &a, Label &label, ArmV6M::Condition condition, BranchWidth width)
{
    if(width == BranchWidth::Narrow)
    {
        return a.branchTo(label, condition);
    }
    else
    {
        assert(width == BranchWidth::Wide);

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
}

ArmV6M::Condition testAccNonzero(Assembler &a, AccState &accState)
{
    uint32_t r = accState.shape().sourceReg(a, SCRATCH_REG);
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

/**
 * `BR_TABLE 1` whose case[0] is empty — what an `if` with no `else` lowers
 * to (isa-core.md §7.1). Branching over case[1] on the inverse condition is
 * the whole construct: no block for the empty case, and no branch out of
 * case[1] to a merge that sits immediately after it.
 */
uint32_t Ctx::translateIfThen(uint32_t pc, BranchWidth width)
{
    const auto entryTos = this->window.tos;
    const bool fused = this->accState.shape().isFlags();

    Label end;

    const auto cond = fused ? this->accState.shape().cond() : testAccNonzero(a, this->accState);

    if(!emitBranch(a, end, ArmV6M::inverse(cond), width))
    {
        return -1;
    }

    if(fused)
    {
        this->accState.producer(Shape::ofImm(1));
    }
    else
    {
        this->accState.poison();
    }

    DecodedInstr term;
    if(!this->GUARDED_processUntilTerminator(pc + 1, width, false, term))
    {
        return -1;
    }

    assert(term.instr.op != Op::FALLTHROUGH); // GCOV_EXCL_LINE — malformed: nothing follows the default case

    if(isProcTerminator(term.instr))
    {
        this->handleGlobalJump(term.instr, entryTos);
    }
    else
    {
        this->localJumpCleanup(entryTos);
    }

    // The skip edge is the empty case, which establishes nothing (§8.7).
    this->accState.poison();

    return a.bind(end) ? term.next : (uint32_t)-1;
}

/**
 * isa-core.md §4.5's two-block dispatch: `BR_TABLE 1` is a truthy test, so
 * `acc = 0` takes case[0] and every other value takes case[1]. Total by
 * construction — no range check to emit for a third outcome that cannot
 * happen — and that is also what lets acc cross the merge (§8.7): every arm
 * flushes it to ACC_REG on its way out, so the merge always finds it in one
 * agreed place. Whether it is *readable* there is the validator's question,
 * not this one's: a merge some case left dead is one no valid program reads,
 * so there is nothing here to decide.
 */
uint32_t Ctx::translateIfThenElse(uint32_t pc, BranchWidth width)
{
    const auto entryTos = this->window.tos;
    const bool fused = this->accState.shape().isFlags();

    Label end, otherwise;

    // Fused, the comparison's own condition is "acc would be 1", which is
    // exactly "not zero" for a value a comparison produced.
    const auto cond = fused ? this->accState.shape().cond() : testAccNonzero(a, this->accState);

    if(!emitBranch(a, otherwise, cond, width))
    {
        return -1;
    }

    uint32_t next = pc;

    for(uint32_t arm = 0; arm < 2; arm++)
    {
        // §8.7 says a case starts with acc dead, so no valid program reads
        // what is left here — but the value is known on the paths where it
        // is, and saying so costs nothing: case[0] is reached exactly when
        // acc was zero, and a fused comparison's case[1] exactly when it
        // would have been one.
        if(arm == 0)
        {
            this->accState.producer(Shape::ofImm(0));
        }
        else if(fused)
        {
            this->accState.producer(Shape::ofImm(1));
        }
        else
        {
            this->accState.poison();
        }

        DecodedInstr term;
        if(!this->GUARDED_processUntilTerminator(next, width, false, term))
        {
            return -1;
        }
        next = term.next;

        if(isProcTerminator(term.instr))
        {
            this->handleGlobalJump(term.instr, entryTos);
        }
        else
        {
            this->localJumpCleanup(entryTos);
        }

        if(arm == 0)
        {
            // A FALLTHROUGH arm runs straight on into the next one, which
            // is where `otherwise` is bound — so it needs no branch, and no
            // literal pool spliced into the path it keeps running down.
            if(term.instr.op == Op::BLOCK_END && !a.branchTo(end))
            {
                return -1;
            }
            if(term.instr.op != Op::FALLTHROUGH)
            {
                a.flushPool();
            }
            if(!a.bind(otherwise))
            {
                return -1;
            }
        }
    }

    this->accState.setClean(ACC_REG);

    if(end.chain != -1 && !a.bind(end))
    {
        return -1;
    }

    return next;
}

uint32_t Ctx::translateSwitch(uint32_t pc, BranchWidth width, uint32_t n)
{

    const auto entryTos = this->window.tos;
    assert(!this->accState.shape().isFlags());

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

    /* N indexed cases plus the default case (isa-core.md §4.5): the jump
     * table's last slot is a block of its own now, not the merge. */
    for(uint32_t i = 0; i <= n; i++)
    {
        a.patchRawHalfword(base + i * 2, (uint16_t)(a.pc() - base));

        this->accState.poison();

        DecodedInstr term;
        if(!this->GUARDED_processUntilTerminator(pc, width, false, term))
        {
            return -1;
        }
        pc = term.next;

        if(isProcTerminator(term.instr))
        {
            this->handleGlobalJump(term.instr, entryTos);
            a.flushPool();
            continue;
        }

        this->localJumpCleanup(entryTos);

        if(term.instr.op == Op::FALLTHROUGH)
        {
            /* Runs on into case i+1, whose own code is emitted next — so no
             * branch out, and no literal pool in between (isa-core.md §4.5). */
            continue;
        }

        if(i < n)
        {
            if(!a.branchTo(end))
            {
                return -1;
            }

            a.flushPool();
        }
    }

    /* Same as the two-block form: every case leaves acc in ACC_REG on its
     * way out, and a merge no valid program may read needs no distinction. */
    this->accState.setClean(ACC_REG);

    if(end.chain != -1)
    {
        if(!a.bind(end))
        {
            return -1;
        }
    }

    return pc;
}

uint32_t Ctx::translateLoop(uint32_t pc, BranchWidth width)
{

    const auto entryTos = this->window.tos;

    this->accState.flushLive(a, ACC_REG);
    const auto start = a.pc();

    if(DecodedInstr condTerm; this->GUARDED_processUntilTerminator(pc, width, true, condTerm))
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

        const bool fused = this->accState.shape().isFlags();

        const auto cond = fused ? this->accState.shape().cond() : testAccNonzero(a, this->accState);

        Label out;
        if(!emitBranch(a, out, ArmV6M::inverse(cond), width))
        {
            return -1;
        }

        if(fused)
        {
            this->accState.producer(Shape::ofImm(1));
        }

        if(DecodedInstr bodyTerm; this->GUARDED_processUntilTerminator(condTerm.next, width, false, bodyTerm))
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

bool Ctx::translateBody(BranchWidth width)
{

    abiEmitPrologue(a, savesLR);

    if(window.tos >= 1)
    {
        accState.flush(a, physReg(window.tos - 1));
    }

    if(DecodedInstr decoded; GUARDED_processUntilTerminator(0, width, false, decoded))
    {
        const Instr &instr = decoded.instr;

        assert(instr.op == Op::RETURN || instr.op == Op::TRAP); // GCOV_EXCL_LINE — BLOCK_END with no open block; malformed program
        this->handleGlobalJump(instr, this->window.tos);
        assert(decoded.next == this->bytesLen);

        return true;
    }

    return false;
}
