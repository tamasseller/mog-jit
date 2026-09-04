#ifndef JIT_ARMV6M_COMPILER_ACCSTATE_H_
#define JIT_ARMV6M_COMPILER_ACCSTATE_H_

#include <cstdint>
#include "shape.h"
#include "flagstate.h"
#include "effect.h"
#include "instr.h"
#include "registers.h"

namespace jitc

{

class Assembler;

/**
 * The accumulator, in native terms: everything a consumer needs to know to
 * read it. `Pending` is a value that has a home already — a register or an
 * unmaterialized literal; `Boolean` is a comparison's 0/1 that was never
 * materialized anywhere and lives in the flags; `Dead` is isa-core.md §8.7's
 * poison.
 *
 * The flag state rides along because `Boolean` is only meaningful while the
 * comparison that set it still stands — that coupling is this class's whole
 * reason to exist beside `Shape`.
 */
class AccState
{
    enum class Kind : uint8_t
    {
        Pending,
        Boolean,
        Dead
    };

    Kind kind = Kind::Pending;
    Shape value = Shape::ofReg(ACC_REG);
    FlagState flags;

    Effect materializeBoolean(Assembler &e, uint32_t dstReg);

    /** `flush` restricted to the `Pending` case, so the merge-point callers'
     *  call graph does not reach the 0/1 select they can never need. */
    void flushPending(Assembler &e, uint32_t dstReg);

    /** True when acc's value is only readable out of `r`. */
    bool livesIn(uint32_t r) const
    {
        return kind == Kind::Pending && value.isReg() && value.reg() == r;
    }

public:
    /** Acc as an operand. Only a `Pending` accumulator is one: a comparison's
     *  boolean is consumed by the branch after it or flushed by its own case,
     *  so nothing else ever sees it. */
    const Shape &operand() const
    {
        assert(kind == Kind::Pending); // GCOV_EXCL_LINE — a translator-logic bug
        return value;
    }

    /** True while acc is a comparison's un-materialized 0/1. */
    bool isBoolean() const { return kind == Kind::Boolean; }

    /** A register acc can be read from — `scratchReg` only if one must be made.
     *  Leaves acc where it was; the caller is not taking ownership of it. */
    uint32_t sourceReg(Assembler &e, uint32_t scratchReg);

    /** The condition a truthy branch on acc tests, emitting a CMP only when the
     *  flags do not already answer it. */
    ArmV6M::Condition testNonzero(Assembler &e);

    void flush(Assembler &e, uint32_t dstReg);

    /** A boolean is consumed by the branch right after it and a dead accumulator
     *  has nothing to carry, so neither reaches a merge.
     *  Always inlined: as a call node of its own it would add a frame to the
     *  deepest chain translateProc's own budget is measured against. */
    __attribute__((always_inline)) inline void flushLive(Assembler &e, uint32_t dstReg)
    {
        if(kind == Kind::Pending)
        {
            flushPending(e, dstReg);
        }
    }

    /** Called before overwriting `r`, so acc stops depending on it. */
    void resolveIfLiveIn(Assembler &e, uint32_t r)
    {
        if(r != ACC_REG && livesIn(r))
        {
            flush(e, ACC_REG);
        }
    }

    /** Acc reads from `s`. Nothing is emitted to make it so, so the flags
     *  stand exactly as they did — only `apply` ever moves those. */
    void pending(Shape s) { kind = Kind::Pending; value = s; }

    /** Acc's value is gone. Says nothing about the flags — what an emission
     *  did to those is `apply`'s business, and the two are independent
     *  (isa-core.md §4.1's write-back modes clobber one and not the other). */
    void poison() { kind = Kind::Dead; }

    /** A control-flow edge: acc is dead per isa-core.md §8.7, and the flags are
     *  whatever the predecessor left, which is not ours to know. */
    void edge() { poison(); apply(Effect::flagsUnknown()); }

    /** Fold in what an emitted sequence did to the machine — the one way the
     *  flag state ever moves. A comparison also lands acc on its 0/1, that
     *  being the one acc consequence an emission settles by itself. */
    void apply(Effect e)
    {
        if(e.isUntouched())   { flags.clobberedAny(e.payload()); }
        else if(e.isZeroOf()) { flags.zeroOf(e.payload()); }
        else if(e.isCompare())
        {
            flags.compare((ArmV6M::Condition)e.payload());
            kind = Kind::Boolean;
        }
        else { flags.forget(); }
    }
};

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_ACCSTATE_H_
