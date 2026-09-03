#include "accstate.h"
#include "assembler.h"

namespace jitc
{

void AccState::flush(Assembler &e, uint32_t dstReg)
{
    value.materialize(e, dstReg);
    setClean(dstReg);
}

} // namespace jitc
