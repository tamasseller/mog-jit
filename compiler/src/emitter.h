// jit-armv6m/compiler — code buffer, ported from
// jit-armv6m/prototype/src/emit.ts's Emitter. Branch-placeholder/patch
// methods now included (blocks.h's LOOP/BR_TABLE machinery needs them) —
// the "straight-line code never branches" scope boundary this file's
// header used to describe is gone now that BR_TABLE/LOOP are ported.
//
// Every patch/read method below bounds-checks its own siteOffset against
// what's actually been written (`count_`), unlike emit.ts's own
// unbounded-array version, which never needed to: this Emitter sits on a
// caller-supplied *fixed* buffer, so a placeholder emitted *after*
// overflow already started keeps returning the same (out-of-capacity)
// offset every time (emit()'s own "at" is `pc()`, computed before the
// capacity check) — patching that offset later must stay memory-safe even
// though the result is meaningless, since the caller only ever discovers
// overflow (and bails out) *after* translateProc's main loop has already
// run to completion.
#ifndef JIT_ARMV6M_COMPILER_EMITTER_H_
#define JIT_ARMV6M_COMPILER_EMITTER_H_

#include <cstdint>
#include "armv6.h"

namespace jitc {

/** Append-only halfword buffer over a caller-supplied fixed buffer +
 *  capacity — the same "buffer + capacity in, bytes-used-or-doesn't-fit
 *  out" contract compile_proc.cpp's mock translator already uses. */
class Emitter {
public:
    Emitter(uint16_t *buf, uint32_t capacityHalfwords)
        : buf_(buf), capacity_(capacityHalfwords) {}

    uint32_t pc() const { return count_ * 2; }

    /** Append one already-encoded halfword; returns its own byte offset.
     *  A no-op (besides setting overflowed()) past capacity — the caller
     *  (translateProc) checks overflowed() once at the end rather than
     *  every call. */
    uint32_t emit(uint16_t word) {
        uint32_t at = pc();
        if(count_ < capacity_) buf_[count_++] = word;
        else overflowed_ = true;
        return at;
    }

    /** Emit a conditional branch with a placeholder (zero) offset — the
     *  common case where the target is a forward reference not yet known.
     *  Returns the site's byte offset, to hand to patchBranch() later. */
    uint32_t placeholderCondBranch(ArmV6M::Condition c) {
        return emit(ArmV6M::condBranch(c, ArmV6M::Ioff<1, 8>(0)));
    }

    /** Same, for an unconditional branch. */
    uint32_t placeholderBranch() {
        return emit(ArmV6M::b(ArmV6M::Ioff<1, 11>(0)));
    }

    /** A placeholder BL (two halfwords) for a local, same-procedure helper
     *  call (blocks.h's BR_TABLE N>2 jump-table helper, unaryops.h's
     *  CLZ/REVBITS software helpers) — unlike CALL, which this ABI never
     *  reaches via a real BL at all (abi_strategy.h's own header). Returns
     *  the site's own byte offset. */
    uint32_t placeholderBL() {
        uint32_t at = pc();
        uint16_t hw1, hw2;
        ArmV6M::bl(0, hw1, hw2);
        emit(hw1);
        emit(hw2);
        return at;
    }

    /** Resolve a previously-emitted (conditional or unconditional) branch's
     *  target now that it's known. A no-op past what's actually been
     *  written (this file's own header) — an already-overflowed procedure
     *  is going to be discarded by the caller regardless. */
    void patchBranch(uint32_t siteOffset, uint32_t targetOffset) {
        uint32_t idx = siteOffset / 2;
        if(idx >= count_) return; // GCOV_EXCL_LINE — only reachable once overflow already started
        uint16_t isn = buf_[idx];
        int32_t delta = (int32_t)targetOffset - (int32_t)(siteOffset + 4);
        buf_[idx] = ArmV6M::isCondBranch(isn)
            ? ArmV6M::setCondBranchOffset(isn, ArmV6M::Ioff<1, 8>((int16_t)delta))
            : ArmV6M::setBranchOffset(isn, ArmV6M::Ioff<1, 11>((int16_t)delta));
    }

    /** Inverse of patchBranch(): the target byte offset a previously-
     *  emitted (conditional or unconditional) branch currently encodes.
     *  blocks.h threads a backpatch chain through a run of not-yet-
     *  resolved branches by pointing each one at the previous pending site
     *  instead of a real target — this recovers that link without a side
     *  array to hold it. Returns siteOffset itself past what's been
     *  written (a stable, self-terminating value for that same
     *  already-overflowed case). */
    uint32_t readBranchTarget(uint32_t siteOffset) const {
        uint32_t idx = siteOffset / 2;
        if(idx >= count_) return siteOffset; // GCOV_EXCL_LINE — see patchBranch's own comment
        uint16_t isn = buf_[idx];
        uint16_t raw;
        int32_t delta;
        if(ArmV6M::getCondBranchOffset(isn, raw)) delta = ArmV6M::signExtend(raw, 8) << 1;
        else { ArmV6M::getBranchOffset(isn, raw); delta = ArmV6M::signExtend(raw, 11) << 1; }
        return siteOffset + 4 + delta;
    }

    /** Overwrite a single already-emitted halfword with an arbitrary raw
     *  value — a BR_TABLE N>2 jump-table slot (blocks.h's
     *  openBrTableJump), not an instruction, so there's no encoding to
     *  preserve the way patchBranch() above has to. */
    void patchLiteral(uint32_t siteOffset, uint16_t value) {
        uint32_t idx = siteOffset / 2;
        if(idx >= count_) return; // GCOV_EXCL_LINE — see patchBranch's own comment
        buf_[idx] = value;
    }

    /** Resolve a placeholderBL() site once its target is known *within
     *  this same procedure*. */
    void patchBL(uint32_t siteOffset, uint32_t targetOffset) {
        uint32_t idx = siteOffset / 2;
        if(idx + 1 >= count_) return; // GCOV_EXCL_LINE — see patchBranch's own comment
        uint16_t hw1, hw2;
        ArmV6M::bl((int32_t)targetOffset - (int32_t)(siteOffset + 4), hw1, hw2);
        buf_[idx] = hw1;
        buf_[idx + 1] = hw2;
    }

    uint32_t halfwordCount() const { return count_; }
    bool overflowed() const { return overflowed_; }

private:
    uint16_t *buf_;
    uint32_t capacity_;
    uint32_t count_ = 0;
    bool overflowed_ = false;
};

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_EMITTER_H_
