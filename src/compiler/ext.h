#ifndef JIT_ARMV6M_COMPILER_EXT_H_
#define JIT_ARMV6M_COMPILER_EXT_H_

#include <cstdint>

namespace jitc
{

class Assembler;
class Window;   // incomplete by design — see this file's header
class AccState; // incomplete by design

constexpr uint32_t EXT_ABI_VERSION = 1;


constexpr uint32_t EXT_FLAG_NEEDS_LR = 1u << 0;     // clobbers lr: helper dispatch, or call-shaped
constexpr uint32_t EXT_FLAG_CALL_SHAPED = 1u << 1;  // §11.2 call-shaped — rejected in v1
constexpr uint32_t EXT_FLAG_TERMINATES = 1u << 2;   // ends its block like RETURN/TRAP — rejected in v1
constexpr uint32_t EXT_FLAG_READS_ACC = 1u << 3;    // real input includes whatever acc holds
constexpr uint32_t EXT_FLAG_WRITES_ACC = 1u << 4;   // leaves a fresh value in acc
constexpr uint32_t EXT_FLAG_ATOMIC = 1u << 5;       // emitted halfwords must stay contiguous

constexpr uint32_t extDecl(uint32_t opcode, uint32_t flags, int32_t tosDelta,
    uint32_t maxTransient, uint32_t halfwords)
{
    return (opcode & 0xffu)
        | ((flags & 0x3fu) << 8)
        | (((uint32_t)tosDelta & 0xfu) << 14)
        | ((maxTransient & 0x7u) << 18)
        | ((halfwords & 0x3fu) << 21);
}

constexpr uint32_t extDeclOpcode(uint32_t w) { return w & 0xffu; }
constexpr uint32_t extDeclFlags(uint32_t w) { return (w >> 8) & 0x3fu; }
constexpr bool extDeclHas(uint32_t w, uint32_t flag) { return (extDeclFlags(w) & flag) != 0; }
constexpr uint32_t extDeclMaxTransient(uint32_t w) { return (w >> 18) & 0x7u; }
constexpr uint32_t extDeclHalfwords(uint32_t w) { return (w >> 21) & 0x3fu; }

/** Sign-extended from the stored 4 bits. */
constexpr int32_t extDeclTosDelta(uint32_t w)
{
    uint32_t f = (w >> 14) & 0xfu;
    return (int32_t)(f >= 8u ? f | 0xfffffff0u : f);
}

constexpr int32_t EXT_TOS_DELTA_MIN = -8;
constexpr uint32_t EXT_MAX_HALFWORDS = 63;

}

constexpr uint32_t EXT_MAX_INPUTS = 3;
constexpr uint32_t EXT_MAX_STACK_INPUTS = 2;

struct ExtSite
{
    const uint8_t *bytes;
    uint32_t bytesLen;
    uint32_t pc;

    uint32_t decl;

    uint8_t in[EXT_MAX_INPUTS];
    uint8_t inCount;

    uint8_t out;

    uint32_t scratch;
};

/* Bound at link time, not through a table: an extension replaces the weak
 * defaults in ext_default.cpp. Direct calls keep the translator's stack
 * bound derivable from the call graph. */
extern "C" uint32_t extDecode(const uint8_t *bytes, uint32_t bytesLen, uint32_t offset, uint32_t *decl);
extern "C" void extEmit(jitc::Assembler &a, const ExtSite &site);
extern "C" uint32_t extHelperStackBytes();

namespace jitc
{

void extEmitHelperCall(Assembler &a, const ExtSite &site, uint32_t helperAddr);

void extEmitCHelperCall(Assembler &a, const ExtSite &site, uint32_t helperAddr);

} // namespace jitc


#endif // JIT_ARMV6M_COMPILER_EXT_H_
