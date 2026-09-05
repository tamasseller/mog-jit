#include "translate_internal.h"
#include "stack_budget.h"
#include "decode_instr.h"
#include "ext.h"
#include "runtime.h"
#include "abi_strategy.h"
#include "arithmetic.h"

using R = ArmV6M::LoReg;

using namespace jitc;

const Instr *Ctx::peek()
{
    if(!this->hasLookahead)
    {
        if(this->body.atEnd())
        {
            return nullptr;
        }

        const bool ok = decodeInstr(this->body.next(), this->body, this->lookahead);
        assert(ok); // GCOV_EXCL_LINE — the scan already refused every encoding this can
        (void)ok;

        this->hasLookahead = true;
    }

    return &this->lookahead;
}

uint32_t Ctx::peekStoreFold(uint32_t otherwise)
{
    const Instr *next = this->peek();

    if(next != nullptr && next->op == Op::STORE && inWindow(this->window.tos, next->target))
    {
        return physReg(this->consume().target);
    }

    return otherwise;
}

Effect Ctx::handleComparisonEmission(const Instr &instr)
{
    Combo combo = instr.combo;

    if(combo == Combo::REG_ACC || combo == Combo::REG_REG)
    {
        if(inWindow(this->window.tos, instr.target))
        {
            return emitComparison(a, this->accState.operand(), instr.op, Shape::ofReg(physReg(instr.target)));
        }
        else
        {
            a.emit(ArmV6M::ldrSp(R(SCRATCH_REG), spillImm(a, this->window.spillOffset(instr.target))));
            return emitComparison(a, this->accState.operand(), instr.op, Shape::ofReg(SCRATCH_REG));
        }
    }
    else if(combo == Combo::IMM_ACC)
    {
        return emitComparison(a, this->accState.operand(), instr.op, Shape::ofImm(instr.imm));
    }
    else
    {
        const Effect cmp = emitComparison(a, this->accState.operand(), instr.op, Shape::ofReg(this->window.topReg()));

        if(combo == Combo::POP_ACC)
        {
            // The POP writes a register but leaves NZCV alone, so the
            // comparison's own effect is still the one that lands.
            this->accState.apply(this->window.finishPop(a));
        }

        return cmp;
    }
}

void Ctx::handleExt(uint8_t opcode)
{
    ExtSite site(this->a, this->window, this->accState, this->body, opcode, this->savesLR);
    extEmit(site);

    // An emitter is opaque: what it did to the machine is not ours to reason about.
    this->accState.apply(Effect::clobber());
}

/* The one cut vertex of the translator's recursion: every cycle runs through
 * here, so one check per level bounds them all. */
bool Ctx::GUARDED_processUntilTerminator(BranchWidth width, bool isThisLoopCondBlock, Instr& out)
{
    Runtime::DynamicStackGuard stackGuard(this->a.runtime, TRANSLATE_LEVEL_STACK_MARGIN);

    while(this->peek())
    {
        const Instr instr = this->consume();

        switch(instr.op)
        {
            case Op::EXT:
                this->handleExt((uint8_t)instr.extOpcode);
                break;

            case Op::CALL:
            {
                if(instr.calleeIndex >= this->a.runtime.getProcCount())
                {
                    runtimeBail(&a.runtime, RESOURCE_PROGRAM_CALLEE_RANGE);
                }
                else
                {
                    uint32_t calleeArgCount = this->a.runtime.slot(instr.calleeIndex).argCount();
                    uint32_t stackArgs = calleeArgCount > 0 ? calleeArgCount - 1 : 0;

                    if(calleeArgCount > 0)
                    {
                        this->accState.flush(a, ACC_REG);
                    }
                    else
                    {
                        this->accState.poison();
                    }

                    this->accState.apply(this->window.spillForCall(a, stackArgs));
                    this->accState.apply(Window::fillCalleeArgs(a, stackArgs));
                    this->accState.apply(abiEmitCall(a, this->procIdx, instr.calleeIndex));
                    this->accState.apply(this->window.reloadAfterCall(a, this->window.tos - stackArgs));

                    this->accState.pending(Shape::ofReg(ACC_REG));
                }

                break;
            }

            case Op::PUSH:
                this->accState.apply(this->window.pushValue(a, this->accState));
                break;

            case Op::DROP:
                /* A scope ending where no block boundary does (isa-core.md
                 * §4.4) — the same window unwind a BLOCK_END performs, just
                 * to a depth the instruction names. Acc is not touched, so
                 * anything pending in a reclaimed register is flushed out
                 * of it first. */
                this->accState.flushLive(a, ACC_REG);
                this->accState.apply(this->window.restore(a, this->window.tos - (uint32_t)instr.imm));
                break;

            case Op::NEG:
            case Op::NOT:
            case Op::SXTB:
            case Op::SXTH:
            case Op::UXTB:
            case Op::UXTH:
            {
                uint32_t dest = this->peekStoreFold(ACC_REG);
                uint32_t src = this->accState.sourceReg(a, dest);

                this->accState.apply(emitUnary(a, instr.op, dest, src));
                this->accState.pending(Shape::ofReg(dest));
                break;
            }
            case Op::CLZ:
            case Op::REVBITS:
            {
                this->accState.flush(a, ACC_REG);
                uint32_t dest = this->peekStoreFold(ACC_REG);

                this->accState.apply(emitUnary(a, instr.op, dest, ACC_REG));
                this->accState.pending(Shape::ofReg(dest));
                break;
            }
            case Op::LOAD:
            {
                if(!inWindow(this->window.tos, instr.target))
                {
                    a.emit(ArmV6M::ldrSp(R(ACC_REG), spillImm(a, this->window.spillOffset(instr.target))));
                    this->accState.apply(Effect::into(ACC_REG, false)); // LDR sets no flags
                    this->accState.pending(Shape::ofReg(ACC_REG));
                    break;
                }

                /* Nothing to emit without a `STORE` to fold: the value is
                 * already where it would be put, and stays pending so its own
                 * consumer can fold it as an operand instead. */
                this->accState.pending(Shape::ofReg(physReg(instr.target)));
                this->accState.flush(a, this->peekStoreFold(physReg(instr.target)));

                break;
            }

            case Op::STORE:
                if(!inWindow(this->window.tos, instr.target))
                {
                    uint32_t r = this->accState.sourceReg(a, SCRATCH_REG);
                    a.emit(ArmV6M::strSp(R(r), spillImm(a, this->window.spillOffset(instr.target))));
                }
                else
                {
                    this->accState.flush(a, physReg(instr.target));
                }
                break;

            case Op::CONST:
            {
                if(ArmV6M::fitsImm8(instr.imm))
                {
                    this->accState.pending(Shape::ofImm(instr.imm)); // a later consumer may fold it
                }
                else
                {
                    uint32_t target = this->peekStoreFold(ACC_REG);
                    this->accState.apply(a.materializeImm32(target, (uint32_t)instr.imm));
                    this->accState.pending(Shape::ofReg(target));
                }
                break;
            }
            case Op::ADD:
            case Op::SUB:
            case Op::RSUB:
            case Op::MUL:
            case Op::AND:
            case Op::OR:
            case Op::XOR:
            case Op::SHL:
            case Op::SHR:
            case Op::ASR:
            {
                Shape operandStorage = Shape::ofImm(0);
                switch (instr.combo)
                {
                    case Combo::REG_ACC:
                    case Combo::REG_REG:
                        if(inWindow(this->window.tos, instr.target))
                        {
                            operandStorage = Shape::ofReg(physReg(instr.target));
                        }
                        else
                        {
                            a.emit(ArmV6M::ldrSp(R(SCRATCH_REG), spillImm(a, this->window.spillOffset(instr.target))));
                            operandStorage = Shape::ofReg(SCRATCH_REG);
                        }
                        break;
                    case Combo::IMM_ACC:
                        operandStorage = Shape::ofImm(instr.imm);
                        break;
                    case Combo::POP_ACC:
                    case Combo::PEEK_PEEK:
                        operandStorage = Shape::ofReg(this->window.topReg());
                        break;
                    default: assert(false);
                };

                /* isa-core.md §4.1 modes 2 and 3 write the result back in
                 * place, so acc does not survive them — but the emission is
                 * the same one either way, and so is what it did to the
                 * flags. */
                const bool writesBack = instr.combo == Combo::REG_REG || instr.combo == Combo::PEEK_PEEK;
                const bool spilledWriteBack = instr.combo == Combo::REG_REG && !inWindow(this->window.tos, instr.target);

                const uint32_t dest = spilledWriteBack ? SCRATCH_REG
                    : (instr.combo == Combo::PEEK_PEEK ? this->window.topReg()
                    : (writesBack ? physReg(instr.target) : this->peekStoreFold(ACC_REG)));

                this->accState.apply(emitBinaryOp(a, instr.op, instr.combo, this->accState.operand(), operandStorage, dest));

                if(writesBack)
                {
                    this->accState.poison();
                }
                else
                {
                    this->accState.pending(Shape::ofReg(dest));
                }

                if(spilledWriteBack)
                {
                    a.emit(ArmV6M::strSp(R(SCRATCH_REG), spillImm(a, this->window.spillOffset(instr.target))));
                }

                if(instr.combo == Combo::POP_ACC)
                {
                    this->accState.apply(this->window.finishPop(a));
                }

                break;
            }
            case Op::EQ:
            case Op::NE:
            case Op::LT_S:
            case Op::LE_S:
            case Op::GT_S:
            case Op::GE_S:
            case Op::LT_U:
            case Op::LE_U:
            case Op::GT_U:
            case Op::GE_U:
            {
                this->accState.apply(this->handleComparisonEmission(instr));

                if(const Instr *la = this->peek(); la != nullptr)
                {
                    // Only the two-block form: its truthy test agrees with
                    // the comparison's own condition for a value a
                    // comparison produced, which is 0 or 1. A wider table
                    // needs the index itself.
                    bool fusesIntoBrTable = la->op == Op::BR_TABLE && la->imm == 1;
                    bool fusesIntoLoopExit = isThisLoopCondBlock && la->op == Op::BLOCK_END;

                    if(fusesIntoBrTable || fusesIntoLoopExit)
                    {
                        break; // whichever it is stays standing for the next round
                    }
                }

                this->accState.flush(a, this->peekStoreFold(ACC_REG));
                break;
            }

            case Op::LOOP_PRE:
            case Op::LOOP_POST:
                if(!this->translateLoop(width, instr.op == Op::LOOP_POST))
                {
                    return false;
                }
                break;

            case Op::BR_TABLE:
                // N=1 is the two-block truthy form (isa-core.md §4.5); wider
                // tables go through the jump-table helper. An empty case[0]
                // is an `if` with no `else` (§7.1), which needs neither a
                // block of its own nor a branch out of case[1].
                if(instr.imm != 1)
                {
                    if(!this->translateSwitch(width, instr.imm))
                    {
                        return false;
                    }
                }
                else if(const Instr *la = this->peek(); la != nullptr && la->op == Op::BLOCK_END)
                {
                    this->consume();

                    if(!this->translateIfThen(width))
                    {
                        return false;
                    }
                }
                else if(!this->translateIfThenElse(width)) // case[0]'s first instruction is still standing
                {
                    return false;
                }

                break;

            default:
                assert(isTerminator(instr));
                assert(!this->accState.isBoolean() || (isThisLoopCondBlock && instr.op == Op::BLOCK_END)); // GCOV_EXCL_LINE — comparison fused into nothing; malformed program

                out = instr;
                return true;
        }
    }

    assert(false); // GCOV_EXCL_LINE
    return false;
}
