#ifndef JIT_ARMV6M_COMPILER_EXT_H_
#define JIT_ARMV6M_COMPILER_EXT_H_

#include <cstdint>

namespace jitc
{

class Assembler;
class Window;   // incomplete by design — see this file's header
class AccState; // incomplete by design

constexpr uint32_t EXT_FLAG_NEEDS_LR = 1u << 0;     // clobbers lr: helper dispatch, or call-shaped
constexpr uint32_t EXT_FLAG_CALL_SHAPED = 1u << 1;  // §11.2 call-shaped — rejected in v1
constexpr uint32_t EXT_FLAG_ATOMIC = 1u << 2;       // emitted halfwords must stay contiguous

constexpr uint32_t extDecl(uint32_t flags, int32_t tosDelta, uint32_t halfwords, uint32_t poolWords = 0)
{
    return (flags & 0x7u)
        | (((uint32_t)tosDelta & 0xfu) << 3)
        | ((halfwords & 0x3fu) << 7)
        | ((poolWords & 0x3u) << 13);
}

constexpr uint32_t extDeclFlags(uint32_t w) { return w & 0x7u; }
constexpr bool extDeclHas(uint32_t w, uint32_t flag) { return (extDeclFlags(w) & flag) != 0; }
constexpr uint32_t extDeclHalfwords(uint32_t w) { return (w >> 7) & 0x3fu; }
constexpr uint32_t extDeclPoolWords(uint32_t w) { return (w >> 13) & 0x3u; }

/** Sign-extended from the stored 4 bits. */
constexpr int32_t extDeclTosDelta(uint32_t w)
{
    uint32_t f = (w >> 3) & 0xfu;
    return (int32_t)(f >= 8u ? f | 0xfffffff0u : f);
}

constexpr int32_t EXT_TOS_DELTA_MIN = -8;
constexpr uint32_t EXT_MAX_HALFWORDS = 63;
constexpr uint32_t EXT_MAX_POOL_WORDS = 3;

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
 * followed by accIsNowIn or accInvalidate. */
class ExtSite
{
    jitc::Window &window;
    jitc::AccState &acc;
    const uint8_t *at;
    uint32_t decl;

public:
    jitc::Assembler &a;

    ExtSite(jitc::Assembler &a, jitc::Window &window, jitc::AccState &acc, const uint8_t *at, uint32_t decl)
        : window(window), acc(acc), at(at), decl(decl), a(a) {}

    ExtSite(const ExtSite &) = delete;
    ExtSite &operator=(const ExtSite &) = delete;

    const uint8_t *opcode() const { return at; }
    const uint8_t *operands() const { return at + 1; }
    uint32_t declaration() const { return decl; }

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

/* Bound at link time, not through a table: an extension replaces the weak
 * defaults in ext_default.cpp. Direct calls keep the translator's stack
 * bound derivable from the call graph. */
extern "C" uint32_t extDecode(const uint8_t *bytes, uint32_t bytesLen, uint32_t offset, uint32_t *decl);
extern "C" void extEmit(ExtSite &site);
extern "C" uint32_t extHelperStackBytes();

#endif // JIT_ARMV6M_COMPILER_EXT_H_
