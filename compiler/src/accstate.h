// The acc fusion state machine (docs/design.md §10.1). A poisoned
// peek()/flush() asserts rather than throwing (this target builds
// -fno-exceptions throughout) — a translator-logic bug, never a legitimate
// runtime condition.
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

    // flush(), but safe at a control-flow merge point (translate_proc.cpp's
    // localJumpCleanup, and translateLoop's own entry) — Poisoned isn't an
    // error here, it's a no-op, since isa-core.md §8.7's acc-clobbering
    // convention already forbids anything downstream from reading it
    // regardless of which path arrived.
    void flushLive(Assembler &e, uint32_t dstReg)
    {
        if(kind != Kind::Poisoned)
        {
            flush(e, dstReg);
        }
    }

    void setClean(uint32_t r)
    {
        kind = Kind::Clean;
        reg = r;
    }

    // A producer (CONST/LOAD/POP/CALL-result) just ran — defer
    // materializing it.
    void producer(Shape s)
    {
        kind = Kind::Pending;
        shape = s;
    }

    // REG_REG/PEEK_PEEK just clobbered acc — nothing downstream may read
    // it until a new producer supersedes this state.
    void poison()
    {
        kind = Kind::Poisoned;
    }
};

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_ACCSTATE_H_
