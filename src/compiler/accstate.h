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
    enum class Kind : uint8_t
    {
        Clean,
        Pending,
        Poisoned
    };

    Kind kind;
    Shape shape;
    uint32_t reg;

public:
    AccState(): kind(Kind::Clean), reg(ACC_REG) {}

    Shape peek() const;

    void flush(Assembler &e, uint32_t dstReg);

    void flushLive(Assembler &e, uint32_t dstReg)
    {
        if(kind != Kind::Poisoned)
        {
            flush(e, dstReg);
        }
    }

    /** True when acc's value is only readable out of `r`. */
    bool livesIn(uint32_t r) const
    {
        if(kind == Kind::Poisoned) return false;
        return kind == Kind::Clean ? reg == r : (!shape.isImm && shape.reg == r);
    }

    /** Called before overwriting `r`, so acc stops depending on it. */
    void resolveIfLiveIn(Assembler &e, uint32_t r)
    {
        if(r != ACC_REG && livesIn(r))
        {
            flush(e, ACC_REG);
        }
    }

    void setClean(uint32_t r)
    {
        kind = Kind::Clean;
        reg = r;
    }

    void producer(Shape s)
    {
        kind = Kind::Pending;
        shape = s;
    }

    void poison()
    {
        kind = Kind::Poisoned;
    }
};

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_ACCSTATE_H_
