// jit-armv6m/compiler — the acc fusion state machine, ported from
// jit-armv6m/prototype/src/accstate.ts. A poisoned peek()/flush() asserts
// rather than throwing (this target builds -fno-exceptions throughout) —
// a translator-logic bug, never a legitimate runtime condition, so this is
// a faithful port of intent even though the mechanism differs.
//
// flushLive (merge-safe flush, block-merge only) is NOT ported — no
// blocks this slice.
#ifndef JIT_ARMV6M_COMPILER_ACCSTATE_H_
#define JIT_ARMV6M_COMPILER_ACCSTATE_H_

#include <cstdint>
#include "shape.h"
#include "instr.h"
#include "registers.h"

namespace jitc {

class Emitter;

class AccState {
public:
    AccState() : kind_(Kind::Clean), reg_(ACC_REG) {}

    /** Read the current value as a foldable operand, without discharging
     *  it. Asserts if poisoned. */
    Shape peek() const;

    /** Force materialization into dstReg (the "flush" transition). */
    void flush(Emitter &e, uint32_t dstReg);

    void setClean(uint32_t reg) { kind_ = Kind::Clean; reg_ = reg; }

    /** A producer (CONST/LOAD/POP/CALL-result) just ran — defer
     *  materializing it. */
    void producer(Shape shape) { kind_ = Kind::Pending; shape_ = shape; }

    /** REG_REG/PEEK_PEEK just clobbered acc — nothing downstream may read
     *  it until a new producer supersedes this state. */
    void poison() { kind_ = Kind::Poisoned; }

private:
    enum class Kind : uint8_t { Clean, Pending, Poisoned };
    Kind kind_;
    Shape shape_{};
    uint32_t reg_;
};

/** Emit one arithmetic binary op and update accState to match. operand is
 *  nullptr for PEEK_PEEK (its right-hand operand is dest itself).
 *  clobbersAcc is true exactly for REG_REG/PEEK_PEEK. */
void emitBinary(
    Emitter &e, AccState &accState, Op op, Combo combo,
    const Shape *operand, uint32_t dest, bool clobbersAcc);

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_ACCSTATE_H_
