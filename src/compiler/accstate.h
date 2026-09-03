#ifndef JIT_ARMV6M_COMPILER_ACCSTATE_H_
#define JIT_ARMV6M_COMPILER_ACCSTATE_H_

#include <cstdint>
#include "shape.h"
#include "instr.h"
#include "registers.h"

namespace jitc
{

class Assembler;

class AccState
{
    Shape value = Shape::ofReg(ACC_REG);

public:
    const Shape &shape() const { return value; }

    void flush(Assembler &e, uint32_t dstReg);

    /** A flags value is consumed by the branch right after it, never carried to a merge.
     *  Always inlined: as a call node of its own it would add a frame to the
     *  deepest chain translateProc's own budget is measured against. */
    __attribute__((always_inline)) inline void flushLive(Assembler &e, uint32_t dstReg)
    {
        if(value.isImm() || value.isReg())
        {
            flush(e, dstReg);
        }
    }

    /** True while acc holds a value at all — in a register, pending, or in the flags. */
    bool isLive() const { return !value.isPoisoned(); }

    /** True when acc's value is only readable out of `r`. */
    bool livesIn(uint32_t r) const
    {
        return value.isReg() && value.reg() == r;
    }

    /** Called before overwriting `r`, so acc stops depending on it. */
    void resolveIfLiveIn(Assembler &e, uint32_t r)
    {
        if(r != ACC_REG && livesIn(r))
        {
            flush(e, ACC_REG);
        }
    }

    void producer(Shape s) { value = s; }
    void setClean(uint32_t r) { value = Shape::ofReg(r); }
    void poison() { value = Shape::poisoned(); }
};

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_ACCSTATE_H_
