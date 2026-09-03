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

int32_t Ctx::peekStoreFold()
{
    const Instr *next = this->peek();

    if(next != nullptr && next->op == Op::STORE && inWindow(this->window.tos, next->target))
    {
        return (int32_t)physReg(this->consume().target);
    }

    return -1;
}

ArmV6M::Condition Ctx::handleComparisonEmission(const Instr &instr)
{
    Combo combo = instr.combo;

    if(combo == Combo::REG_ACC || combo == Combo::REG_REG)
    {
        if(inWindow(this->window.tos, instr.target))
        {
            return emitComparison(a, this->accState.shape(), instr.op, Shape::ofReg(physReg(instr.target)));
        }
        else
        {
            a.emit(ArmV6M::ldrSp(R(SCRATCH_REG), spillImm(a, this->window.spillOffset(instr.target))));
            return emitComparison(a, this->accState.shape(), instr.op, Shape::ofReg(SCRATCH_REG));
        }
    }
    else if(combo == Combo::IMM_ACC)
    {
        return emitComparison(a, this->accState.shape(), instr.op, Shape::ofImm(instr.imm));
    }
    else
    {
        const auto ret = emitComparison(a, this->accState.shape(), instr.op, Shape::ofReg(this->window.topReg()));

        if(combo == Combo::POP_ACC)
        {
            this->window.finishPop(a);
        }

        return ret;
    }
}

void Ctx::handleExt(uint8_t opcode)
{
    ExtSite site(this->a, this->window, this->accState, this->body, opcode, this->savesLR);
    extEmit(site);
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

                    this->window.spillForCall(a, stackArgs);
                    Window::fillCalleeArgs(a, stackArgs);
                    abiEmitCall(a, this->procIdx, instr.calleeIndex);
                    this->window.reloadAfterCall(a, this->window.tos - stackArgs);

                    this->accState.producer(Shape::ofReg(ACC_REG));
                }

                break;
            }

            case Op::PUSH:
                this->window.pushValue(a, this->accState);
                break;

            case Op::NEG:
            case Op::NOT:
            case Op::SXTB:
            case Op::SXTH:
            case Op::UXTB:
            case Op::UXTH:
            {
                uint32_t dest = foldDest(this->peekStoreFold(), ACC_REG);
                uint32_t src = this->accState.shape().sourceReg(a, dest);

                this->accState.setClean(dest, emitUnary(a, instr.op, dest, src));
                break;
            }
            case Op::CLZ:
            case Op::REVBITS:
            {
                this->accState.flush(a, ACC_REG);
                uint32_t dest = foldDest(this->peekStoreFold(), ACC_REG);

                this->accState.setClean(dest, emitUnary(a, instr.op, dest, ACC_REG));
                break;
            }
            case Op::LOAD:
            {
                if(!inWindow(this->window.tos, instr.target))
                {
                    a.emit(ArmV6M::ldrSp(R(ACC_REG), spillImm(a, this->window.spillOffset(instr.target))));
                    this->accState.setClean(ACC_REG);
                    break;
                }

                const int32_t fold = this->peekStoreFold();

                this->accState.producer(Shape::ofReg(physReg(instr.target)));

                if(fold >= 0)
                {
                    this->accState.flush(a, (uint32_t)fold);
                }

                break;
            }

            case Op::STORE:
                if(!inWindow(this->window.tos, instr.target))
                {
                    uint32_t r = this->accState.shape().sourceReg(a, SCRATCH_REG);
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
                    this->accState.producer(Shape::ofImm(instr.imm)); // stay pending — a later consumer may fold it
                }
                else
                {
                    uint32_t target = foldDest(this->peekStoreFold(), ACC_REG);
                    this->accState.setClean(target, a.materializeImm32(target, (uint32_t)instr.imm));
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
                Shape operandStorage{};
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

                if(instr.combo == Combo::REG_REG)
                {
                    if(inWindow(this->window.tos, instr.target))
                    {
                        emitBinaryOp(a, instr.op, instr.combo, this->accState.shape(), operandStorage, physReg(instr.target));
                    }
                    else
                    {
                        emitBinaryOp(a, instr.op, instr.combo, this->accState.shape(), operandStorage, SCRATCH_REG);
                        a.emit(ArmV6M::strSp(R(SCRATCH_REG), spillImm(a, this->window.spillOffset(instr.target))));
                    }

                    this->accState.poison();
                }
                else if(instr.combo == Combo::PEEK_PEEK)
                {
                    emitBinaryOp(a, instr.op, instr.combo, this->accState.shape(), operandStorage, this->window.topReg());
                    this->accState.poison();
                }
                else
                {
                    const auto dest = foldDest(this->peekStoreFold(), ACC_REG);
                    const bool zLive = emitBinaryOp(a, instr.op, instr.combo, this->accState.shape(), operandStorage, dest);
                    this->accState.setClean(dest, zLive);

                    if(instr.combo == Combo::POP_ACC)
                    {
                        this->window.finishPop(a);
                    }
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
                this->accState.producer(Shape::ofFlags(this->handleComparisonEmission(instr)));

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

                this->accState.flush(a, foldDest(this->peekStoreFold(), ACC_REG));
                break;
            }

            case Op::LOOP:
                if(!this->translateLoop(width))
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
                assert(!this->accState.shape().isFlags() || (isThisLoopCondBlock && instr.op == Op::BLOCK_END)); // GCOV_EXCL_LINE — comparison fused into nothing; malformed program

                out = instr;
                return true;
        }
    }

    assert(false); // GCOV_EXCL_LINE
    return false;
}
