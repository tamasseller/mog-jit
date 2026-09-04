#ifndef JIT_ARMV6M_COMPILER_WINDOW_H_
#define JIT_ARMV6M_COMPILER_WINDOW_H_

#include <cstdint>
#include "registers.h"
#include "armv6.h"
#include "effect.h"

namespace jitc
{

class Assembler;
class AccState;

bool inWindow(uint32_t tos, uint32_t k);

uint32_t physReg(uint32_t k);

/** Bails with RESOURCE_LIMIT_SPILL_OFFSET past LDR/STR [sp,#imm]'s reach. */
ArmV6M::Uoff<2, 8> spillImm(Assembler &a, uint32_t byteOffset);

class Window
{
    bool savesLR;
    uint32_t initialSpilledCount;

public:
    uint32_t tos;

    inline Window() = default;

    explicit Window(uint32_t argCount, bool savesLR = false)
        : tos(argCount), 
          savesLR(savesLR),
          initialSpilledCount(argCount > WINDOW_SIZE ? argCount - WINDOW_SIZE : 0)
    {
    }

    uint32_t topReg() const
    {
        return physReg(tos - 1);
    }

    /** True when finishPop reloads a spill into topReg's own register. */
    bool popUncovers() const
    {
        return (tos - 1) >= WINDOW_SIZE;
    }

    Effect pushValue(Assembler &e, AccState &accState);

    Effect pushFrom(Assembler &e, AccState &accState, uint32_t srcReg);

    Effect finishPop(Assembler &e);

    uint32_t spillOffset(uint32_t k) const;

    Effect discard(Assembler &e) const;

    Effect spillForCall(Assembler &e, uint32_t stackArgs);

    static Effect fillCalleeArgs(Assembler &e, uint32_t stackArgs);

    Effect reloadAfterCall(Assembler &e, uint32_t targetTos);

    Effect restore(Assembler &e, uint32_t targetTos);
};


} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_WINDOW_H_
