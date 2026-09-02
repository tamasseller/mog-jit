#include "translate_internal.h"
#include "stack_budget.h"
#include "decode_instr.h"
#include "runtime.h"
#include "abi_strategy.h"
#include "arithmetic.h"

using R = ArmV6M::LoReg;

using namespace jitc;

struct FoldResult
{
    int32_t reg;
    uint32_t afterNext;

    inline auto redirectReg(uint32_t otherwise) const 
    {
        return this->reg >= 0 ? (uint32_t)this->reg : otherwise;
    }

    inline auto redirectAfterNext(uint32_t otherwise) const {
        return this->reg >= 0 ? this->afterNext : otherwise;
    }
};

static FoldResult peekStoreFold(const uint8_t *bytes, uint32_t bytesLen, uint32_t afterPc, uint32_t tos)
{
    if(afterPc >= bytesLen)
    {
        return {-1, 0};
    }

    DecodedInstr d = decodeInstr(bytes, bytesLen, afterPc);

    if(d.instr.op == Op::STORE && inWindow(tos, d.instr.target))
    {
        return {(int32_t)physReg(d.instr.target), d.next};
    }
    return {-1, 0};
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

/* An extension that outgrows its declaration is a diagnostic here rather
 * than arena pressure and a wide-branch retry further out. */
static void emitExtSite(Assembler &a, ExtSite &site, uint32_t budget)
{
    const uint32_t before = a.pc();
    extEmit(site);

    if(a.pc() - before > budget)
    {
        runtimeBail(&a.runtime, RESOURCE_PROGRAM_EXT_UNSUPPORTED);
    }
}

void Ctx::handleExt(const Instr &instr, uint32_t pc)
{
    const uint32_t decl = instr.extDecl;
    const uint32_t budget = extDeclHalfwords(decl) * 2;
    const uint32_t tosBefore = this->window.tos;
    const bool accWasLive = !this->accState.shape().isPoisoned();

    ExtSite site(this->a, this->window, this->accState, this->bytes + pc, decl);

    if(extDeclHas(decl, EXT_FLAG_ATOMIC))
    {
        Assembler::AtomicBlock atomic(this->a, extDeclPoolWords(decl), budget);
        emitExtSite(this->a, site, budget);
    }
    else
    {
        emitExtSite(this->a, site, budget);
    }

    /* The wire's total_depth was validated against this delta, and nothing
     * re-derives it here. */
    if(this->window.tos != tosBefore + (uint32_t)extDeclTosDelta(decl))
    {
        runtimeBail(&a.runtime, RESOURCE_PROGRAM_EXT_UNSUPPORTED);
    }

    /* The validator read the same declaration off the other half of the
     * extension (ExtOpEffect.killsAcc), so an emitter that disagrees with it
     * about acc is a diagnostic here rather than a poisoned read later. */
    if(extDeclHas(decl, EXT_FLAG_KILLS_ACC))
    {
        this->accState.poison();
    }
    else if(accWasLive && this->accState.shape().isPoisoned())
    {
        runtimeBail(&a.runtime, RESOURCE_PROGRAM_EXT_UNSUPPORTED);
    }
}

/* The one cut vertex of the translator's recursion: every cycle runs through
 * here, so one check per level bounds them all. */
bool Ctx::GUARDED_processUntilTerminator(uint32_t pc, BranchWidth width, bool isThisLoopCondBlock, DecodedInstr& out)
{
    Runtime::DynamicStackGuard stackGuard(this->a.runtime, TRANSLATE_LEVEL_STACK_MARGIN);

    while(pc < this->bytesLen)
    {
        DecodedInstr decoded = decodeInstr(this->bytes, this->bytesLen, pc);
        const Instr &instr = decoded.instr;
        uint32_t afterInstr = decoded.next;

        switch(instr.op)
        {
            case Op::EXT:
            {
                this->handleExt(instr, pc);
                pc = afterInstr;
                break;
            }
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

                pc = afterInstr;
                break;
            }

            case Op::PUSH:
                this->window.pushValue(a, this->accState);
                pc = afterInstr;
                break;

            case Op::NEG:
            case Op::NOT:
            case Op::SXTB:
            case Op::SXTH:
            case Op::UXTB:
            case Op::UXTH:
            {
                FoldResult fold = peekStoreFold(this->bytes, this->bytesLen, afterInstr, this->window.tos);
                uint32_t dest = fold.redirectReg(ACC_REG);
                uint32_t src = this->accState.shape().sourceReg(a, dest);

                emitUnary(a, instr.op, dest, src);
                this->accState.setClean(dest);
                
                pc = fold.redirectAfterNext(afterInstr);
                break;
            }
            case Op::CLZ:
            case Op::REVBITS:
            {
                this->accState.flush(a, ACC_REG);
                FoldResult fold = peekStoreFold(this->bytes, this->bytesLen, afterInstr, this->window.tos);
                uint32_t dest = fold.redirectReg(ACC_REG);

                emitUnary(a, instr.op, dest, ACC_REG);
                this->accState.setClean(dest);
                
                pc = fold.redirectAfterNext(afterInstr);
                break;
            }
            case Op::LOAD:
            {
                if(!inWindow(this->window.tos, instr.target))
                {
                    a.emit(ArmV6M::ldrSp(R(ACC_REG), spillImm(a, this->window.spillOffset(instr.target))));
                    this->accState.setClean(ACC_REG);

                    pc = afterInstr;
                    break;
                }

                FoldResult fold = peekStoreFold(this->bytes, this->bytesLen, afterInstr, this->window.tos);
                
                this->accState.producer(Shape::ofReg(physReg(instr.target)));

                if(fold.reg >= 0)
                {
                    this->accState.flush(a, (uint32_t)fold.reg);

                    pc = fold.afterNext;
                    break;
                }

                pc = afterInstr;
                break;
            }

            case Op::STORE:
                if(!inWindow(this->window.tos, instr.target))
                {
                    uint32_t r = this->accState.shape().sourceReg(a, SCRATCH_REG);
                    a.emit(ArmV6M::strSp(R(r), spillImm(a, this->window.spillOffset(instr.target))));

                    pc = afterInstr;
                    break;
                }
                else
                {
                    this->accState.flush(a, physReg(instr.target));
                    
                    pc = afterInstr;
                    break;
                }

            case Op::CONST:
            {
                if(ArmV6M::fitsImm8(instr.imm))
                {
                    this->accState.producer(Shape::ofImm(instr.imm)); // stay pending — a later consumer may fold it

                    pc = afterInstr;
                    break;
                }
                else
                {
                    FoldResult fold = peekStoreFold(this->bytes, this->bytesLen, afterInstr, this->window.tos);
                    uint32_t target = fold.redirectReg(ACC_REG);
                    a.materializeImm32(target, (uint32_t)instr.imm);
                    this->accState.setClean(target);

                    pc = fold.redirectAfterNext(afterInstr);
                    break;
                }
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
                    pc = afterInstr;
                    break;
                }
                else if(instr.combo == Combo::PEEK_PEEK)
                {
                    emitBinaryOp(a, instr.op, instr.combo, this->accState.shape(), operandStorage, this->window.topReg());
                    this->accState.poison();
                    pc = afterInstr;
                    break;
                }
                else
                {
                    FoldResult fold = peekStoreFold(this->bytes, this->bytesLen, afterInstr, this->window.tos);

                    const auto dest = fold.redirectReg(ACC_REG);
                    emitBinaryOp(a, instr.op, instr.combo, this->accState.shape(), operandStorage, dest);
                    this->accState.setClean(dest);

                    if(instr.combo == Combo::POP_ACC)
                    {
                        this->window.finishPop(a);
                    }

                    pc = fold.redirectAfterNext(afterInstr);
                    break;
                }
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

                if(afterInstr < this->bytesLen)
                {
                    DecodedInstr lookahead = decodeInstr(this->bytes, this->bytesLen, afterInstr);
                    
                    // Only the two-block form: its truthy test agrees with
                    // the comparison's own condition for a value a
                    // comparison produced, which is 0 or 1. A wider table
                    // needs the index itself.
                    bool fusesIntoBrTable = lookahead.instr.op == Op::BR_TABLE && lookahead.instr.imm == 1;
                    bool fusesIntoLoopExit = isThisLoopCondBlock && lookahead.instr.op == Op::BLOCK_END;

                    if(fusesIntoBrTable || fusesIntoLoopExit)
                    {
                        pc = afterInstr;
                        break;
                    }
                }

                FoldResult fold = peekStoreFold(this->bytes, this->bytesLen, afterInstr, this->window.tos);

                this->accState.flush(a, fold.redirectReg(ACC_REG));

                pc = fold.redirectAfterNext(afterInstr);
                break;
            }
            
            case Op::LOOP: 
                pc = this->translateLoop(afterInstr, width); 

                if(pc == -1) 
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
                    pc = this->translateSwitch(afterInstr, width, instr.imm);
                }
                else if(this->bytes[afterInstr] == BLOCK_END_OPCODE)
                {
                    pc = this->translateIfThen(afterInstr, width);
                }
                else
                {
                    pc = this->translateIfThenElse(afterInstr, width);
                }

                if(pc == -1) 
                {
                    return false;
                }

                break;

            default:
                assert(isTerminator(instr));
                assert(!this->accState.shape().isFlags() || (isThisLoopCondBlock && instr.op == Op::BLOCK_END)); // GCOV_EXCL_LINE — comparison fused into nothing; malformed program

                out = DecodedInstr{.instr = instr, .next = afterInstr};
                return true;
        }
    }

    assert(false); // GCOV_EXCL_LINE
    return false;
}

