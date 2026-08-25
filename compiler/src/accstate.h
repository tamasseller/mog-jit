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

class Emitter;

class AccState
{
public:
    AccState()
        : kind(Kind::Clean), reg(ACC_REG)
    {
    }

    // Read the current value as a foldable operand, without discharging
    // it. Asserts if poisoned.
    Shape peek() const;

    // Force materialization into dstReg (the "flush" transition).
    void flush(Emitter &e, uint32_t dstReg);

    // flush(), but safe at a control-flow merge point (blocks.h's
    // closeBlockEnd/closeCaseViaTerminator/closeLoopBodyViaTerminator) —
    // Poisoned isn't an error here, it's a no-op, since the acc-clobbering
    // convention already forbids anything downstream from reading it
    // regardless of which path arrived.
    void flushLive(Emitter &e, uint32_t dstReg)
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

    // Emit one arithmetic binary op and update this state to match. operand is
    // nullptr for PEEK_PEEK (its right-hand operand is dest itself).
    // clobbersAcc is true exactly for REG_REG/PEEK_PEEK.
    void emitBinary(Emitter &e, Op op, Combo combo, const Shape *operand,
                    uint32_t dest, bool clobbersAcc);

private:
    enum class Kind : uint8_t
    {
        Clean,
        Pending,
        Poisoned
    };
    Kind kind;
    Shape shape{};
    uint32_t reg;
};

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_ACCSTATE_H_
