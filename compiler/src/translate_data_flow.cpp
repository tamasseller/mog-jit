#include "translate_internal.h"
#include "decode_instr.h"
#include "runtime_internal.h"
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

static FoldResult peekStoreFold(const uint8_t *bytes, uint32_t bytesLen, uint32_t afterPc, uint32_t tos, const ExtHooks *ext)
{
    if(afterPc >= bytesLen)
    {
        return {-1, 0};
    }

    DecodedInstr d = decodeInstr(bytes, bytesLen, afterPc, ext);

    if(d.instr.op == Op::STORE && inWindow(tos, d.instr.target))
    {
        return {(int32_t)physReg(d.instr.target), d.next};
    }
    return {-1, 0};
}

static ArmV6M::Uoff<2, 8> spillImm(Assembler &a, uint32_t byteOffset)
{
    if(!ArmV6M::Uoff<2, 8>::isInRange(byteOffset))
    {
        runtimeBail(&a.runtime, RESOURCE_LIMIT_SPILL_OFFSET);
    }
    return ArmV6M::Uoff<2, 8>((uint16_t)byteOffset);
}

ArmV6M::Condition Ctx::handleComparisonEmission(const Instr &instr)
{
    Combo combo = instr.combo;

    if(combo == Combo::REG_ACC || combo == Combo::REG_REG)
    {
        if(inWindow(this->window.tos, instr.target))
        {
            return emitComparison(a, this->accState.peek(), instr.op, Shape::ofReg(physReg(instr.target)));
        }
        else
        {
            a.emit(ArmV6M::ldrSp(R(SCRATCH_REG), spillImm(a, this->window.spillOffset(instr.target))));
            return emitComparison(a, this->accState.peek(), instr.op, Shape::ofReg(SCRATCH_REG));
        }
    }
    else if(combo == Combo::IMM_ACC)
    {
        return emitComparison(a, this->accState.peek(), instr.op, Shape::ofImm(instr.imm));
    }
    else 
    {
        const auto ret = emitComparison(a, this->accState.peek(), instr.op, Shape::ofReg(this->window.topReg()));
        
        if(combo == Combo::POP_ACC)
        {
            this->window.finishPop(a);
        }

        return ret;
    }
}

bool Ctx::processUntilTerminator(uint32_t pc, EmitBranch emitBranch, bool isThisLoopCondBlock, DecodedInstr& out)
{
    while(pc < this->bytesLen)
    {
        DecodedInstr decoded = decodeInstr(this->bytes, this->bytesLen, pc, this->a.runtime.extension());
        const Instr &instr = decoded.instr;
        uint32_t afterInstr = decoded.next;

        switch(instr.op)
        {
            case Op::EXT:
            {
                const ExtHooks *hooks = this->a.runtime.extension();
                if(hooks == nullptr || hooks->emit == nullptr)
                {
                    runtimeBail(&a.runtime, RESOURCE_PROGRAM_EXT_UNKNOWN);
                    pc = afterInstr; // per fail()'s own contract
                    break;
                }

                const uint32_t decl = instr.extDecl;
                const uint32_t pops = (uint32_t)(-extDeclTosDelta(decl));
                const bool readsAcc = extDeclHas(decl, EXT_FLAG_READS_ACC);
                const bool writesAcc = extDeclHas(decl, EXT_FLAG_WRITES_ACC);

                ExtSite site{};
                site.bytes = this->bytes;
                site.bytesLen = this->bytesLen;
                site.pc = pc;
                site.decl = decl;
                site.out = (uint8_t)ACC_REG;
                site.scratch = (1u << ENTRY_JUMP_REG) | (1u << 12);

                if(readsAcc)
                {
                    this->accState.flush(a, ACC_REG);
                    site.in[site.inCount++] = (uint8_t)ACC_REG;
                }
                else if(pops > 0 || writesAcc)
                {
                    // Staging clobbers r1/r2, and a deferred acc must not be
                    // left depending on either. flushLive, not flush: a
                    // poisoned acc is legitimate here and stays poisoned.
                    this->accState.flushLive(a, ACC_REG);
                }

                // Top first, into r1 then r2 — never r0, which is acc's, and
                // never r3, which is the only register a helper reach can use.
                static const uint8_t STACK_STAGE[EXT_MAX_STACK_INPUTS] = {ENTRY_IDX_REG, SCRATCH_REG};
                for(uint32_t i = 0; i < pops; i++)
                {
                    uint32_t src = this->window.topReg();
                    uint8_t dst = STACK_STAGE[i];
                    if(src != dst)
                    {
                        a.emit(ArmV6M::mov(ArmV6M::AnyReg(dst), ArmV6M::AnyReg(src)));
                    }
                    site.in[site.inCount++] = dst;
                    this->window.finishPop(a);
                }

                const uint32_t before = a.pc();
                hooks->emit(a, site);

                if(a.pc() - before > extDeclHalfwords(decl) * 2)
                {
                    runtimeBail(&a.runtime, RESOURCE_PROGRAM_EXT_UNSUPPORTED);
                    pc = afterInstr;
                    break;
                }

                if(writesAcc)
                {
                    this->accState.setClean(ACC_REG);
                }

                pc = afterInstr;
                break;
            }
            case Op::CALL:
            {
                if(instr.calleeIndex >= this->a.runtime.procCount)
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

            case Op::POP:
                a.emit(ArmV6M::mov(ArmV6M::AnyReg(ACC_REG), ArmV6M::AnyReg(this->window.topReg()))); // materialize now — a bare POP can't safely stay PENDING
                this->accState.setClean(ACC_REG);
                this->window.finishPop(a); // must run after the read above — same register

                pc = afterInstr;
                break;

            case Op::NEG:
            case Op::NOT:
            {
                FoldResult fold = peekStoreFold(this->bytes, this->bytesLen, afterInstr, this->window.tos, this->a.runtime.extension());
                uint32_t dest = fold.redirectReg(ACC_REG);
                uint32_t src = this->accState.peek().peek(a, dest);

                emitUnary(a, instr.op, dest, src);
                this->accState.setClean(dest);
                
                pc = fold.redirectAfterNext(afterInstr);
                break;
            }
            case Op::CLZ:
            case Op::REVBITS:
            {
                this->accState.flush(a, ACC_REG);
                FoldResult fold = peekStoreFold(this->bytes, this->bytesLen, afterInstr, this->window.tos, this->a.runtime.extension());
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

                FoldResult fold = peekStoreFold(this->bytes, this->bytesLen, afterInstr, this->window.tos, this->a.runtime.extension());
                
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
                    uint32_t r = this->accState.peek().peek(a, SCRATCH_REG);
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
                    FoldResult fold = peekStoreFold(this->bytes, this->bytesLen, afterInstr, this->window.tos, this->a.runtime.extension());
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
                        emitBinaryOp(a, instr.op, instr.combo, this->accState.peek(), operandStorage, physReg(instr.target));
                    }
                    else
                    {
                        emitBinaryOp(a, instr.op, instr.combo, this->accState.peek(), operandStorage, SCRATCH_REG);
                        a.emit(ArmV6M::strSp(R(SCRATCH_REG), spillImm(a, this->window.spillOffset(instr.target))));
                    }

                    this->accState.poison();
                    pc = afterInstr;
                    break;
                }
                else if(instr.combo == Combo::PEEK_PEEK)
                {
                    emitBinaryOp(a, instr.op, instr.combo, this->accState.peek(), operandStorage, this->window.topReg());
                    this->accState.poison();
                    pc = afterInstr;
                    break;
                }
                else
                {
                    FoldResult fold = peekStoreFold(this->bytes, this->bytesLen, afterInstr, this->window.tos, this->a.runtime.extension());

                    const auto dest = fold.redirectReg(ACC_REG);
                    emitBinaryOp(a, instr.op, instr.combo, this->accState.peek(), operandStorage, dest);
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
                auto trueCondition = this->handleComparisonEmission(instr);

                if(afterInstr < this->bytesLen)
                {
                    DecodedInstr lookahead = decodeInstr(this->bytes, this->bytesLen, afterInstr, this->a.runtime.extension());
                    
                    bool fusesIntoBrTable = lookahead.instr.op == Op::BR_TABLE && (lookahead.instr.imm == 1 || lookahead.instr.imm == 2);
                    bool fusesIntoLoopExit = isThisLoopCondBlock && lookahead.instr.op == Op::BLOCK_END;

                    if(fusesIntoBrTable || fusesIntoLoopExit)
                    {
                        this->pendingComparisonCondition = trueCondition;
                        this->hasPendingComparisonCondition = true;
                        this->accState.poison();
                        
                        pc = afterInstr;
                        break;
                    }
                }

                FoldResult fold = peekStoreFold(this->bytes, this->bytesLen, afterInstr, this->window.tos, this->a.runtime.extension());
                uint32_t dest = fold.redirectReg(ACC_REG);

                Label falseLabel;
                if(!a.branchTo(falseLabel, ArmV6M::inverse(trueCondition))) return false;
                
                a.emit(ArmV6M::movs(R((uint16_t)dest), ArmV6M::Imm<8>(1)));

                Label endLabel;
                const auto endOk = a.branchTo(endLabel);
                assert(endOk);

                const auto falseOk = a.bind(falseLabel);
                assert(falseOk);

                a.emit(ArmV6M::movs(R((uint16_t)dest), ArmV6M::Imm<8>(0)));
                
                const auto endBound = a.bind(endLabel);
                assert(endBound);

                this->accState.setClean(dest);

                pc = fold.redirectAfterNext(afterInstr);
                break;
            }
            
            case Op::LOOP: 
                pc = this->translateLoop(afterInstr, emitBranch); 

                if(pc == -1) 
                {
                    return false;
                }

                break;

            case Op::BR_TABLE:
                switch(instr.imm)
                {
                    case 1: pc = this->translateIfThen(afterInstr, emitBranch); break;
                    case 2: pc = this->translateIfThenElse(afterInstr, emitBranch); break;
                    default: pc = this->translateSwitch(afterInstr, emitBranch, instr.imm);break;
                }

                if(pc == -1) 
                {
                    return false;
                }

                break;

            default:
                assert(isTerminator(instr));
                assert(!this->hasPendingComparisonCondition || (isThisLoopCondBlock && instr.op == Op::BLOCK_END)); // GCOV_EXCL_LINE — comparison fused into nothing; malformed program

                out = DecodedInstr{.instr = instr, .next = afterInstr};
                return true;
        }
    }

    assert(false); // GCOV_EXCL_LINE
    return false;
}

