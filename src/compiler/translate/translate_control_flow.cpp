#include "translate_internal.h"
#include "abi_strategy.h"
#include "decode_instr.h"
#include "runtime.h"

using namespace jitc;

using R = ArmV6M::LoReg;

/** A conditional branch to an *already emitted* target. `Label` chains
 *  forward references only, so a back-edge computes its own displacement —
 *  and picks the same narrow/wide shapes `emitBranch` does, since a
 *  conditional branch reaches ±254 bytes and an unconditional one ±2046. */
static bool emitBackBranch(Assembler &a, uint32_t target, ArmV6M::Condition condition, BranchWidth width)
{
    if(width == BranchWidth::Narrow)
    {
        const int32_t delta = (int32_t)target - (int32_t)(a.pc() + 4);

        if(!ArmV6M::Ioff<1, 8>::isInRange(delta))
        {
            return false; // retried whole, in Wide (translateProc)
        }

        a.emit(ArmV6M::condBranch(condition, ArmV6M::Ioff<1, 8>((int16_t)delta)));
        return true;
    }

    assert(width == BranchWidth::Wide);

    Label fallThrough;

    const auto branchOk = a.branchTo(fallThrough, ArmV6M::inverse(condition));
    assert(branchOk);
    (void)branchOk;

    const int32_t delta = (int32_t)target - (int32_t)(a.pc() + 4);

    if(!ArmV6M::Ioff<1, 11>::isInRange(delta))
    {
        runtimeBail(&a.runtime, RESOURCE_LIMIT_LOOP_BACK_EDGE);
        return false;
    }

    a.emit(ArmV6M::b(ArmV6M::Ioff<1, 11>((int16_t)delta)));

    return a.bind(fallThrough);
}

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


/** A case closer that runs on into another case instead of leaving the
 *  construct (isa-core.md §4.5). For the two-block form the two coincide:
 *  the case `DEFAULT` names is `case[1]`, which is also the next one. */
static bool continuesIntoAnotherCase(Op op)
{
    return op == Op::FALLTHROUGH || op == Op::DEFAULT;
}

void Ctx::localJumpCleanup(uint32_t tos)
{
    this->accState.flushLive(a, ACC_REG);
    this->accState.apply(this->window.restore(a, tos));
}

void Ctx::handleGlobalJump(Instr term, uint32_t tos)
{
    if(term.op == Op::RETURN)
    {
        // A void procedure reaches here with acc dead (isa-core.md §8.7) and
        // has nothing to canonicalize — no valid program reads what r0 then
        // holds, and deciding that is the validator's job, not this one's.
        this->accState.flushLive(a, ACC_REG);
        this->accState.apply(this->window.discard(a));
        this->accState.apply(abiEmitReturn(a, this->savesLR, this->initialSpilledCount));
    }
    else
    {
        assert(term.op == Op::TRAP);

        this->accState.apply(a.materializeImm32(ACC_REG, (uint32_t)term.imm));
        this->accState.apply(abiEmitTrap(a));
    }

    this->accState.edge();
    this->window.tos = tos;
}

/**
 * `BR_TABLE 1` whose case[0] is empty — what an `if` with no `else` lowers
 * to (isa-core.md §7.1). Branching over case[1] on the inverse condition is
 * the whole construct: no block for the empty case, and no branch out of
 * case[1] to a merge that sits immediately after it.
 */
bool Ctx::translateIfThen(BranchWidth width)
{
    const auto entryTos = this->window.tos;

    Label end;

    const auto cond = this->accState.testNonzero(a);

    if(!emitBranch(a, end, ArmV6M::inverse(cond), width))
    {
        return false;
    }

    this->accState.edge();

    Instr term;
    if(!this->GUARDED_processUntilTerminator(width, false, term))
    {
        return false;
    }

    assert(!continuesIntoAnotherCase(term.op)); // GCOV_EXCL_LINE — malformed: nothing follows the default case

    if(isProcTerminator(term))
    {
        this->handleGlobalJump(term, entryTos);
    }
    else
    {
        this->localJumpCleanup(entryTos);
    }

    // The skip edge is the empty case, which establishes nothing (§8.7).
    this->accState.edge();

    return a.bind(end);
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
bool Ctx::translateIfThenElse(BranchWidth width)
{
    const auto entryTos = this->window.tos;

    Label end, otherwise;

    const auto cond = this->accState.testNonzero(a);

    if(!emitBranch(a, otherwise, cond, width))
    {
        return false;
    }

    for(uint32_t arm = 0; arm < 2; arm++)
    {
        this->accState.edge();

        Instr term;
        if(!this->GUARDED_processUntilTerminator(width, false, term))
        {
            return false;
        }

        if(isProcTerminator(term))
        {
            this->handleGlobalJump(term, entryTos);
        }
        else
        {
            this->localJumpCleanup(entryTos);
        }

        if(arm == 0)
        {
            // An arm that runs straight on into the next one — which is
            // where `otherwise` is bound, and which for N=1 is also the
            // case `DEFAULT` names — needs no branch, and no literal pool
            // spliced into the path it keeps running down.
            if(term.op == Op::BLOCK_END && !a.branchTo(end))
            {
                return false;
            }
            if(!continuesIntoAnotherCase(term.op))
            {
                a.flushPool();
            }
            if(!a.bind(otherwise))
            {
                return false;
            }
        }
    }

    this->accState.edge();
    this->accState.pending(Shape::ofReg(ACC_REG));

    return end.chain == -1 || a.bind(end);
}

bool Ctx::translateSwitch(BranchWidth width, uint32_t n)
{
    const auto entryTos = this->window.tos;
    assert(!this->accState.isBoolean());

    this->accState.flush(a, ACC_REG);
    this->accState.apply(a.materializeImm32(SCRATCH_REG, n));

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

    Label end, dflt;

    /* N indexed cases plus the default case (isa-core.md §4.5): the jump
     * table's last slot is a block of its own now, not the merge. */
    for(uint32_t i = 0; i <= n; i++)
    {
        a.patchRawHalfword(base + i * 2, (uint16_t)(a.pc() - base));

        /* The default case is where every DEFAULT closer lands — bound
         * here, once its own code starts, and patched back into each. */
        if(i == n && dflt.chain != -1 && !a.bind(dflt))
        {
            return false;
        }

        // Every case is entered through the jump-table helper's own BLX.
        this->accState.edge();

        Instr term;
        if(!this->GUARDED_processUntilTerminator(width, false, term))
        {
            return false;
        }

        if(isProcTerminator(term))
        {
            this->handleGlobalJump(term, entryTos);
            a.flushPool();
            continue;
        }

        this->localJumpCleanup(entryTos);

        if(term.op == Op::FALLTHROUGH)
        {
            /* Runs on into case i+1, whose own code is emitted next — so no
             * branch out, and no literal pool in between (isa-core.md §4.5). */
            continue;
        }

        if(term.op == Op::DEFAULT)
        {
            /* Forward to case[n], which is emitted last — the same forward
             * chain-and-patch `end` uses for the merge. */
            if(!a.branchTo(dflt))
            {
                return false;
            }

            a.flushPool();
            continue;
        }

        if(i < n)
        {
            if(!a.branchTo(end))
            {
                return false;
            }

            a.flushPool();
        }
    }

    /* Same as the two-block form: every case leaves acc in ACC_REG on its
     * way out, and a merge no valid program may read needs no distinction. */
    this->accState.edge();
    this->accState.pending(Shape::ofReg(ACC_REG));

    return end.chain == -1 || a.bind(end);
}

/**
 * isa-core.md §7.2's rotated shape, which the body-first block order is
 * there to make emittable in one pass:
 *
 *     B    cond          ; LOOP_PRE only — LOOP_POST just falls in
 *   body:  <body block>
 *   cond:  <condition block>
 *          Bcc  body
 *   out:
 *
 * One taken branch per iteration rather than two, and the entry branch is
 * the only thing the two openers differ in. It also splits the ±2046-byte
 * branch budget: the entry branch spans the body and the back-edge spans
 * the condition, where a condition-first layout needed one branch to span
 * both.
 */
bool Ctx::translateLoop(BranchWidth width, bool postTest)
{
    const auto entryTos = this->window.tos;

    this->accState.flushLive(a, ACC_REG);
    this->accState.apply(Effect::flagsUnknown());

    Label cond;

    if(!postTest && !a.branchTo(cond))
    {
        return false; // `branchTo` flushes the pool for us on the way
    }

    // Both openers enter the body across a CFG split (isa-core.md §8.7):
    // the condition's own branch, which LOOP_POST's sequential entry edge
    // meets right here.
    this->accState.edge();
    const auto bodyStart = a.pc();

    Instr bodyTerm;
    if(!this->GUARDED_processUntilTerminator(width, false, bodyTerm))
    {
        return false;
    }

    if(isProcTerminator(bodyTerm))
    {
        // §8.5 only allows this under LOOP_PRE, where the condition block
        // below is still reachable through the entry branch.
        assert(!postTest); // GCOV_EXCL_LINE — malformed program
        this->handleGlobalJump(bodyTerm, entryTos);
        a.flushPool();
    }
    else
    {
        assert(bodyTerm.op == Op::BLOCK_END);
        // The body's own closer is an unconditional continue into the
        // condition, which sits physically next — nothing to emit but the
        // TOS restore.
        this->localJumpCleanup(entryTos);
    }

    if(!postTest && !a.bind(cond))
    {
        return false;
    }

    // A merge, and acc crosses it: isa-core.md §8.7 has the condition
    // block inherit liveness from the entry edge and the body's
    // fallthrough both, so this is `translateBrTable`'s merge treatment,
    // not a split successor's. Every predecessor flushed acc into ACC_REG
    // on its way here — the opener above, and `localJumpCleanup` — so the
    // one agreed place is where it is read from. Only the flags go: the
    // back-edge arrives carrying the condition's own, which nothing here
    // may fuse against.
    this->accState.edge();
    this->accState.pending(Shape::ofReg(ACC_REG));

    Instr condTerm;
    if(!this->GUARDED_processUntilTerminator(width, true, condTerm))
    {
        return false;
    }

    assert(condTerm.op == Op::BLOCK_END);

    if(this->window.tos != entryTos)
    {
        this->accState.flushLive(a, ACC_REG);
        this->accState.apply(this->window.restore(a, entryTos));
    }

    const auto test = this->accState.testNonzero(a);

    if(!emitBackBranch(a, bodyStart, test, width))
    {
        return false;
    }

    // The exit falls out of the conditional branch — a split successor, so
    // acc is dead and the flags are the test's, which nothing may reuse.
    this->accState.edge();

    return true;
}

bool Ctx::translateBody(BranchWidth width)
{
    abiEmitPrologue(a, savesLR);

    if(window.tos >= 1)
    {
        accState.flush(a, physReg(window.tos - 1));
    }

    if(Instr term; GUARDED_processUntilTerminator(width, false, term))
    {
        assert(term.op == Op::RETURN || term.op == Op::TRAP); // GCOV_EXCL_LINE — BLOCK_END with no open block; malformed program
        this->handleGlobalJump(term, this->window.tos);
        assert(this->body.atEnd());

        return true;
    }

    return false;
}
