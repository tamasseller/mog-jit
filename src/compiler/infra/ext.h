#ifndef JIT_ARMV6M_COMPILER_EXT_H_
#define JIT_ARMV6M_COMPILER_EXT_H_

#include <cstdint>

#include "bytecode.h"

namespace jitc
{

class Assembler;
class Window;   // incomplete by design — see this file's header
class AccState; // incomplete by design

constexpr uint32_t EXT_FLAG_NEEDS_LR = 1u << 0;     // clobbers lr: helper dispatch, or call-shaped
constexpr uint32_t EXT_FLAG_CALL_SHAPED = 1u << 1;  // §11.2 call-shaped — rejected in v1

constexpr uint32_t extDesc(uint32_t flags, int32_t tosDelta)
{
    return (flags & 0xfu) | (((uint32_t)tosDelta & 0xfu) << 4);
}

constexpr uint32_t extDescFlags(uint32_t w) { return w & 0xfu; }
constexpr bool extDescHas(uint32_t w, uint32_t flag) { return (extDescFlags(w) & flag) != 0; }

/** Sign-extended from the stored 4 bits. */
constexpr int32_t extDescTosDelta(uint32_t w)
{
    uint32_t f = (w >> 4) & 0xfu;
    return (int32_t)(f >= 8u ? f | 0xfffffff0u : f);
}

constexpr int32_t EXT_TOS_DELTA_MIN = -8;

}

/* Free to clobber at any extension site. Everything else is live core state:
 * r0 is acc's, reachable only through accInto/accIsNowIn/accInvalidate, r4-r7
 * are the window, r8-r11 the runtime ABI. */
constexpr uint32_t EXT_SCRATCH_MASK = (1u << 1) | (1u << 2) | (1u << 3) | (1u << 12);

/* One extension instruction's emission context. The operand stack and the
 * accumulator are reachable only through these calls — Window and AccState are
 * never defined here, so an extension TU that names one fails to compile.
 *
 * Every call that writes a window register first resolves an accumulator
 * living in it. Writing r0 outside accInto is a contract violation unless
 * followed by accIsNowIn or accInvalidate. Both helper reaches clobber r0
 * and invalidate the accumulator themselves; an op that has to keep a value
 * across one stages it on the operand stack, as its own maxTransient.
 *
 * The site owns the wire as well: `opcode()` is the byte the core already
 * read, `operand()` reads the next one. An emitter must consume exactly the
 * operands its own extDescribe did — that cursor is the core's position too,
 * and nothing re-derives the instruction's length behind it. */
class ExtSite
{
    jitc::Window &window;
    jitc::AccState &acc;
    BcReader &wire;
    uint8_t op;
    bool lrSaved;

public:
    jitc::Assembler &a;

    ExtSite(jitc::Assembler &a, jitc::Window &window, jitc::AccState &acc, BcReader &wire,
        uint8_t op, bool lrSaved)
        : window(window), acc(acc), wire(wire), op(op), lrSaved(lrSaved), a(a) {}

    ExtSite(const ExtSite &) = delete;
    ExtSite &operator=(const ExtSite &) = delete;

    uint8_t opcode() const { return op; }
    uint8_t operand() { return wire.next(); }

    /** Slots are absolute frame-relative indices, exactly as LOAD/STORE use. */
    uint32_t depth() const;
    uint32_t load(uint32_t slot, uint32_t dstReg);
    void store(uint32_t slot, uint32_t srcReg);

    void push(uint32_t srcReg);
    void pop(uint32_t dstReg);

    uint32_t accInto(uint32_t dstReg);
    void accIsNowIn(uint32_t reg);
    void accInvalidate();

    /** Hand-written Thumb with a known clobber set; no AAPCS guarantees. */
    void helperCall(uint32_t helperAddr);

    /** Independently-compiled C: at most two arguments, r0 and r1. */
    void cHelperCall(uint32_t helperAddr);
};

/* Bound at link time, direct calls for stack bound checking. One call per
 * phase, and nothing carries a description between them: extDescribe runs in
 * the body scan, extEmit in translation with the cursor where extDescribe's
 * was. An emitter owns everything else about its site, the literal pool
 * included. design.md §18. */
extern "C" bool extDescribe(uint8_t opcode, BcReader &wire, uint32_t *desc);
extern "C" void extEmit(ExtSite &site);
extern "C" uint32_t extHelperStackBytes();

#endif // JIT_ARMV6M_COMPILER_EXT_H_
