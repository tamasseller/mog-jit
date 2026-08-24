// Code buffer with branch-placeholder/patch methods for blocks.h's
// LOOP/BR_TABLE machinery.
//
// Every patch/read method below bounds-checks its own siteOffset against
// what's actually been written (`count`): this Emitter sits on a
// caller-supplied fixed buffer, so a placeholder emitted after overflow
// already started keeps returning the same (out-of-capacity) offset every
// time; patching that offset later must stay memory-safe even though the
// result is meaningless, since the caller only discovers overflow (and
// bails out) after translateProc's main loop has already run to
// completion.
#ifndef JIT_ARMV6M_COMPILER_EMITTER_H_
#define JIT_ARMV6M_COMPILER_EMITTER_H_

#include <cstdint>
#include "armv6.h"

namespace jitc
{

// Append-only halfword buffer over a caller-supplied fixed buffer + capacity.
class Emitter
{
public:
    Emitter(uint16_t *buf, uint32_t capacityHalfwords)
        : buf(buf), capacity(capacityHalfwords)
    {
    }

    uint32_t pc() const
    {
        return count * 2;
    }

    // Append one already-encoded halfword; returns its own byte offset. A
    // no-op (besides setting overflowed()) past capacity — the caller
    // (translateProc) checks overflowed() once at the end rather than
    // every call.
    uint32_t emit(uint16_t word)
    {
        uint32_t at = pc();
        if(count < capacity)
        {
            buf[count++] = word;
        }
        else
        {
            overflowedFlag = true;
        }
        return at;
    }

    // Emit a conditional branch with a placeholder (zero) offset — the
    // common case where the target is a forward reference not yet known.
    // Returns the site's byte offset, to hand to patchBranch() later.
    uint32_t placeholderCondBranch(ArmV6M::Condition c)
    {
        return emit(ArmV6M::condBranch(c, ArmV6M::Ioff<1, 8>(0)));
    }

    // Same, for an unconditional branch.
    uint32_t placeholderBranch()
    {
        return emit(ArmV6M::b(ArmV6M::Ioff<1, 11>(0)));
    }

    // A placeholder BL (two halfwords) for a local, same-procedure helper
    // call. Returns the site's own byte offset.
    uint32_t placeholderBL()
    {
        uint32_t at = pc();
        uint16_t hw1, hw2;
        ArmV6M::bl(0, hw1, hw2);
        emit(hw1);
        emit(hw2);
        return at;
    }

    // Resolve a previously-emitted (conditional or unconditional) branch's
    // target now that it's known. A no-op past what's actually been
    // written — an already-overflowed procedure is going to be discarded
    // by the caller regardless.
    void patchBranch(uint32_t siteOffset, uint32_t targetOffset)
    {
        uint32_t idx = siteOffset / 2;
        if(idx >= count)
        {
            return; // GCOV_EXCL_LINE — only reachable once overflow already started
        }
        uint16_t isn = buf[idx];
        int32_t delta = (int32_t)targetOffset - (int32_t)(siteOffset + 4);
        buf[idx] = ArmV6M::isCondBranch(isn)
            ? ArmV6M::setCondBranchOffset(isn, ArmV6M::Ioff<1, 8>((int16_t)delta))
            : ArmV6M::setBranchOffset(isn, ArmV6M::Ioff<1, 11>((int16_t)delta));
    }

    // Inverse of patchBranch(): the target byte offset a previously-emitted
    // (conditional or unconditional) branch currently encodes. blocks.h
    // threads a backpatch chain through a run of not-yet-resolved branches
    // by pointing each one at the previous pending site instead of a real
    // target — this recovers that link without a side array to hold it.
    uint32_t readBranchTarget(uint32_t siteOffset) const
    {
        uint32_t idx = siteOffset / 2;
        if(idx >= count)
        {
            return siteOffset; // GCOV_EXCL_LINE — see patchBranch's own comment
        }
        uint16_t isn = buf[idx];
        uint16_t raw;
        int32_t delta;
        if(ArmV6M::getCondBranchOffset(isn, raw))
        {
            delta = ArmV6M::signExtend(raw, 8) << 1;
        }
        else
        {
            ArmV6M::getBranchOffset(isn, raw);
            delta = ArmV6M::signExtend(raw, 11) << 1;
        }
        return siteOffset + 4 + delta;
    }

    // Overwrite a single already-emitted halfword with an arbitrary raw
    // value — a BR_TABLE N>2 jump-table slot, not an instruction, so
    // there's no encoding to preserve the way patchBranch() above has to.
    void patchLiteral(uint32_t siteOffset, uint16_t value)
    {
        uint32_t idx = siteOffset / 2;
        if(idx >= count)
        {
            return; // GCOV_EXCL_LINE — see patchBranch's own comment
        }
        buf[idx] = value;
    }

    // Emit an `LDR dstReg,[pc,#...]` whose 8-bit field carries scratchTag
    // instead of a real offset — translate_proc.cpp's literal pool parks
    // the literal's own bytecode-offset delta there until the pool is
    // flushed and the true forward distance is known. Goes through
    // fmtImm8 rather than ArmV6M::setLiteralOffset deliberately: a
    // bytecode delta is an arbitrary byte count, and setLiteralOffset's
    // Uoff<2,8> would (rightly) assert on one that isn't a multiple of 4.
    // Returns the site's byte offset, like the placeholders above.
    uint32_t placeholderLiteralLoad(uint32_t dstReg, uint8_t scratchTag)
    {
        return emit(ArmV6M::fmtImm8(ArmV6M::Imm8Op::LDR, (uint16_t)dstReg, scratchTag));
    }

    // Whether the halfword at siteOffset is a literal-load site and, if
    // so, its raw 8-bit field — either a placeholderLiteralLoad() scratch
    // tag or an already-resolved offset, since patchLiteralOffset() below
    // leaves the encoding still matching. Mirrors
    // ArmV6M::getLiteralOffset's own shape.
    bool getLiteralOffsetAt(uint32_t siteOffset, uint16_t &off) const
    {
        uint32_t idx = siteOffset / 2;
        if(idx >= count)
        {
            return false; // GCOV_EXCL_LINE — see patchBranch's own comment
        }
        return ArmV6M::getLiteralOffset(buf[idx], off);
    }

    // Resolve a placeholderLiteralLoad() site now that its pool word's
    // position is known. byteOffset is measured from Align(siteOffset+4,4),
    // so it's always a non-negative multiple of 4 — Uoff<2,8> asserts both.
    void patchLiteralOffset(uint32_t siteOffset, ArmV6M::Uoff<2, 8> byteOffset)
    {
        uint32_t idx = siteOffset / 2;
        if(idx >= count)
        {
            return; // GCOV_EXCL_LINE — see patchBranch's own comment
        }
        buf[idx] = ArmV6M::setLiteralOffset(buf[idx], byteOffset);
    }

    // Resolve a placeholderBL() site once its target is known within this
    // same procedure.
    void patchBL(uint32_t siteOffset, uint32_t targetOffset)
    {
        uint32_t idx = siteOffset / 2;
        if(idx + 1 >= count)
        {
            return; // GCOV_EXCL_LINE — see patchBranch's own comment
        }
        uint16_t hw1, hw2;
        ArmV6M::bl((int32_t)targetOffset - (int32_t)(siteOffset + 4), hw1, hw2);
        buf[idx] = hw1;
        buf[idx + 1] = hw2;
    }

    uint32_t halfwordCount() const
    {
        return count;
    }

    // How much more can still be written before overflowing — what
    // arena_room.h's ArenaRoom::ensureRoom checks against to decide
    // whether growing is even needed.
    uint32_t remainingHalfwords() const
    {
        return capacity - count;
    }

    // Repoints this Emitter at a new, (normally larger) underlying
    // buffer — docs/design.md §11's mid-translation compaction: the
    // caller (ArenaRoom::ensureRoom) has already relocated every
    // already-written halfword to newBuf itself (a real memmove,
    // Runtime::evict's own extended tail range), so count/overflowed stay
    // exactly as they were; only where those halfwords physically live
    // changes. Every method above resolves purely through `count`-relative
    // offsets into `buf`, never an address captured earlier, so nothing
    // else needs to know this happened.
    void rebase(uint16_t *newBuf, uint32_t newCapacityHalfwords)
    {
        buf = newBuf;
        capacity = newCapacityHalfwords;
    }

    bool overflowed() const
    {
        return overflowedFlag;
    }

private:
    uint16_t *buf;
    uint32_t capacity;
    uint32_t count = 0;
    bool overflowedFlag = false;
};

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_EMITTER_H_
