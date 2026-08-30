#include "accstate.h"
#include "assembler.h"

#include <cassert>

namespace jitc
{

Shape AccState::peek() const
{
    assert(kind != Kind::Poisoned); // GCOV_EXCL_LINE — a translator-logic bug, never legitimate input
    return kind == Kind::Pending ? shape : Shape::ofReg(reg);
}

void AccState::flush(Assembler &e, uint32_t dstReg)
{
    peek().materialize(e, dstReg);
    kind = Kind::Clean;
    reg = dstReg;
    shape = Shape::ofReg(dstReg);
}

} // namespace jitc
