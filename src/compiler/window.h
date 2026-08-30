#ifndef JIT_ARMV6M_COMPILER_WINDOW_H_
#define JIT_ARMV6M_COMPILER_WINDOW_H_

#include <cstdint>
#include "registers.h"
#include "armv6.h"

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

    void pushValue(Assembler &e, AccState &accState);

    void pushFrom(Assembler &e, AccState &accState, uint32_t srcReg);

    void finishPop(Assembler &e);

    uint32_t spillOffset(uint32_t k) const;

    bool discard(Assembler &e) const;

    void spillForCall(Assembler &e, uint32_t stackArgs);

    static void fillCalleeArgs(Assembler &e, uint32_t stackArgs);

    void reloadAfterCall(Assembler &e, uint32_t targetTos);

    bool restore(Assembler &e, uint32_t targetTos);
};


} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_WINDOW_H_
