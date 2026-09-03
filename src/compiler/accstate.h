#ifndef JIT_ARMV6M_COMPILER_ACCSTATE_H_
#define JIT_ARMV6M_COMPILER_ACCSTATE_H_

#include <cstdint>
#include "shape.h"
#include "flagstate.h"
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

    bool materializeBoolean(Assembler &e, uint32_t dstReg);

    /** `flush` restricted to the `Pending` case, so the merge-point callers'
     *  call graph does not reach the 0/1 select they can never need. */
    void flushPending(Assembler &e, uint32_t dstReg);

public:
    /** Acc as an operand. Only a `Pending` accumulator is one: a comparison's
     *  boolean is consumed by the branch after it or flushed by its own case,
     *  so nothing else ever sees it. */
    const Shape &operand() const
    {
        assert(kind == Kind::Pending); // GCOV_EXCL_LINE — a translator-logic bug
        return value;
    }

    /** True while acc holds a value at all. */
    bool isLive() const { return kind != Kind::Dead; }

    /** True while acc is a comparison's un-materialized 0/1. */
    bool isBoolean() const { return kind == Kind::Boolean; }

    /** True when acc's value is only readable out of `r`. */
    bool livesIn(uint32_t r) const
    {
        return kind == Kind::Pending && value.isReg() && value.reg() == r;
    }

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

    /** Acc is `s`, and nothing is known about the flags. */
    void pending(Shape s) { kind = Kind::Pending; value = s; flags.forget(); }

    /** Acc is the 0/1 the comparison just emitted encodes. */
    void boolean(ArmV6M::Condition c) { kind = Kind::Boolean; flags.compare(c); }

    /** Acc reads from `r`, with nothing emitted to make it so — the flags
     *  therefore stand exactly as they did. */
    void retarget(uint32_t r) { kind = Kind::Pending; value = Shape::ofReg(r); }

    /** Acc is in `r`, put there by something that left N/Z set from it or not. */
    void setClean(uint32_t r, bool zSet = false) { retarget(r); noteFlags(r, zSet); }

    void poison() { kind = Kind::Dead; flags.forget(); }

    /** N/Z now reflect `r`, or nothing at all when `set` is false. */
    void noteFlags(uint32_t r, bool set) { if(set) { flags.zeroOf(r); } else { flags.forget(); } }

    /** `r` is about to be overwritten by something that leaves N/Z alone. */
    void clobbered(uint32_t r) { flags.clobbered(r); }

    /** Nothing is known about N/Z any more — a merge, or an opaque emitter. */
    void dropFlags() { flags.forget(); }
};

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_ACCSTATE_H_
