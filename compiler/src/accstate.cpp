#include "accstate.h"
#include "emitter.h"
#include "binops.h"

#include <cassert>

namespace jitc
{

Shape AccState::peek() const
{
    assert(kind != Kind::Poisoned); // GCOV_EXCL_LINE — a translator-logic bug, never legitimate input
    return kind == Kind::Pending ? shape : Shape::ofReg(reg);
}

void AccState::flush(Emitter &e, uint32_t dstReg)
{
    materializeShape(e, peek(), dstReg);
    kind = Kind::Clean;
    reg = dstReg;
}

void AccState::emitBinary(Emitter &e, Op op, Combo combo, const Shape *operand,
                          uint32_t dest, bool clobbersAcc)
{
    emitBinaryOp(e, op, combo, peek(), operand, dest);
    if(clobbersAcc)
    {
        poison();
    }
    else
    {
        setClean(dest);
    }
}

} // namespace jitc
