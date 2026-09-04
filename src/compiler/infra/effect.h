// jit-armv6m/compiler — what an emitted sequence did to the machine.
#ifndef JIT_ARMV6M_COMPILER_EFFECT_H_
#define JIT_ARMV6M_COMPILER_EFFECT_H_

#include <cstdint>

#include "armv6.h"

namespace jitc
{

/**
 * The bridge between emitting and tracking: which registers a sequence gave a
 * new value to, and what the condition flags say once it has run. Every
 * emitter returns one and `AccState::apply` is the only thing that reads one,
 * so a new emitter cannot quietly forget to say.
 *
 * What acc *means* afterwards is deliberately not in here — that follows from
 * the bytecode being translated, not from what was emitted.
 *
 * One word, packed by hand: these are built and consumed on the translator's
 * hottest path, and a struct of three bytes is not what a 32-bit callee
 * returns cheaply.
 */
class Effect
{
    /* [7:0] payload — a register mask, a register, or a condition — and the
     * kind above it, so the two come out with one UXTB and one LSRS. */
    uint32_t v;

    enum Kind : uint32_t
    {
        Untouched, // NZCV are exactly what they were; payload is the write mask
        ZeroOf,    // N/Z answer "is payload zero"
        Compare,   // a comparison op ran: NZCV hold the relation `payload`, and
                   // isa-core.md §4.2 makes its 0/1 the accumulator
        Gone       // anyone's guess
    };

    constexpr Effect(Kind k, uint32_t payload): v(payload | ((uint32_t)k << 8)) {}

public:
    static constexpr uint32_t bit(uint32_t r) { return 1u << r; }

    /** Nothing was emitted at all. */
    static constexpr Effect none() { return Effect(Untouched, 0); }

    /** Registers were given new values, by instructions that leave NZCV alone. */
    static constexpr Effect writes(uint32_t mask) { return Effect(Untouched, mask); }

    /** A value landed in `r`, `zSet` saying whether N/Z came out set from it. */
    static constexpr Effect into(uint32_t r, bool zSet)
    {
        return zSet ? Effect(ZeroOf, r) : Effect(Untouched, bit(r));
    }

    /** A bytecode comparison op, whose whole purpose is to produce a 0/1: a CMP
     *  the translator emits for itself reports `into(r, true)` instead. */
    static constexpr Effect comparison(ArmV6M::Condition c) { return Effect(Compare, (uint32_t)c); }

    /** A call, a helper reach, or an opaque emitter. Which registers it took
     *  with it is the caller's own business to declare — an emission this
     *  opaque leaves nothing worth tracking a mask for. */
    static constexpr Effect clobber() { return flagsUnknown(); }

    /** A merge: NZCV are whatever the other predecessor left. Nothing was
     *  emitted here, so there is nothing else to say. */
    static constexpr Effect flagsUnknown() { return Effect(Gone, 0); }

    constexpr uint32_t kind() const { return v >> 8; }
    constexpr uint32_t payload() const { return v & 0xffu; }

    constexpr bool isUntouched() const { return kind() == Untouched; }
    constexpr bool isZeroOf() const { return kind() == ZeroOf; }
    constexpr bool isCompare() const { return kind() == Compare; }
};

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_EFFECT_H_
