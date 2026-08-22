// jit-armv6m/compiler — code buffer, ported from
// jit-armv6m/prototype/src/emit.ts's Emitter. No branch-placeholder/patch
// methods: straight-line code (no BR_TABLE/LOOP this slice) never branches,
// so those are simply omitted rather than ported unused.
#ifndef JIT_ARMV6M_COMPILER_EMITTER_H_
#define JIT_ARMV6M_COMPILER_EMITTER_H_

#include <cstdint>

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
