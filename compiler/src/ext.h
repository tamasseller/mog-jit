// jit-armv6m/compiler — the extension seam (isa-core.md §11). The only
// header an extension includes.
//
// The core owns wire bytes 0..123; >= 128 belongs to one registered
// extension whose semantics the core never interprets. It needs exactly
// two things per opcode: the instruction's byte length, so the
// body-boundary and branch-span walks can step over it, and its declared
// effect (§11.2), so validate-side bookkeeping and needsLRSave stay
// correct. Both arrive from decode(), packed into one word riding in
// Instr's union — keeping the span budget, the prologue's lr decision and
// codegen structurally incapable of disagreeing.
//
// Operands are never held by the core: they are literal constants (§11.3)
// the extension re-reads from the wire at emit time.
//
// Window and AccState are forward-declared and never defined: an extension
// TU that names one fails to compile.
#ifndef JIT_ARMV6M_COMPILER_EXT_H_
#define JIT_ARMV6M_COMPILER_EXT_H_

#include <cstdint>

// For RUNTIME_EXT_STATE_* only; needs nothing but stdint itself.
#include "runtime_host.h"

namespace jitc
{

class Assembler;
class Window;   // incomplete by design — see this file's header
class AccState; // incomplete by design

/** Bump whenever this header changes shape. Checked at Runtime::init
 *  against ExtHooks::abiVersion — the only enforced point of the
 *  native-declares-a-subset-of-TS rule, since the wire carries only the
 *  consequences of effects, never the effects. */
constexpr uint32_t EXT_ABI_VERSION = 1;

// ── the packed declaration ───────────────────────────────────────────────
//
//  31    27 26   21 20  18 17   14 13    8 7        0
// +-------+-------+-----+-------+--------+----------+
// | unused|halfwds|maxTr|tosDlta| flags  |  opcode  |
//
// Build with extDecl(), read with the accessors.

constexpr uint32_t EXT_FLAG_NEEDS_LR = 1u << 0;     // clobbers lr: helper dispatch, or call-shaped
constexpr uint32_t EXT_FLAG_CALL_SHAPED = 1u << 1;  // §11.2 call-shaped — rejected in v1
constexpr uint32_t EXT_FLAG_TERMINATES = 1u << 2;   // ends its block like RETURN/TRAP — rejected in v1
constexpr uint32_t EXT_FLAG_READS_ACC = 1u << 3;    // real input includes whatever acc holds
constexpr uint32_t EXT_FLAG_WRITES_ACC = 1u << 4;   // leaves a fresh value in acc
constexpr uint32_t EXT_FLAG_ATOMIC = 1u << 5;       // emitted halfwords must stay contiguous

/** Pack one opcode's declaration.
 *
 *  opcode        the wire byte (>= 128).
 *  flags         EXT_FLAG_* above.
 *  tosDelta      net TOS depth change (§11.2). Must be <= 0; -tosDelta
 *                values are staged for the extension.
 *  maxTransient  peak TOS above entry depth. Must be 0 in v1: window.tos
 *                has to agree with the real sp exactly.
 *  halfwords     worst-case emitted halfword count, <= 63. The span walk
 *                budgets this; emission must not exceed it. */
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

/** Widest value each field admits, for a decoder that wants to check its
 *  own numbers before packing them. */
constexpr int32_t EXT_TOS_DELTA_MIN = -8;
constexpr uint32_t EXT_MAX_HALFWORDS = 63;

// ── registration ─────────────────────────────────────────────────────────

} // namespace jitc

/* ExtHooks and its contract types sit at global scope because they cross
 * runtime_host.h, which stays usable from plain C. The packing helpers
 * above are compiler-side and stay in namespace jitc. */

/** Three, mechanically: a helper reach is `MOV r3,r10 / LDR r3,[r3,#off] /
 *  BLX r3` — the only idiom Thumb-1 admits — so r3 is permanently spoken
 *  for. That leaves r0 (acc, only when the declaration reads it), r1, r2. */
constexpr uint32_t EXT_MAX_INPUTS = 3;
constexpr uint32_t EXT_MAX_STACK_INPUTS = 2;

/** Everything one extension opcode's emit() is given. Plain integers and
 *  raw bytes: nothing here exposes Window, AccState, abi_strategy or
 *  Runtime, so all four stay free to change. */
struct ExtSite
{
    /** The whole body and this instruction's own offset in it, so the
     *  extension re-reads its own literal operands (§11.3). `pc` addresses
     *  the opcode byte itself. */
    const uint8_t *bytes;
    uint32_t bytesLen;
    uint32_t pc;

    /** The packed declaration this opcode's own decode() produced. */
    uint32_t decl;

    /** Low registers holding the staged inputs, already moved out of the
     *  operand stack. in[0] is acc when the declaration reads it; the
     *  popped stack values follow, TOP FIRST. Every one of these is dead
     *  after this instruction, so emit() may freely clobber them. */
    uint8_t in[EXT_MAX_INPUTS];
    uint8_t inCount;

    /** Where a WRITES_ACC op must leave its result. */
    uint8_t out;

    /** Free to clobber beyond `in`: r3 (also the only helper-reach target,
     *  so it is gone the moment you make one) and r12/ip. Both are
     *  intra-sequence only — a BLX destroys r12. */
    uint32_t scratch;
};

struct ExtHooks
{
    /** Must equal jitc::EXT_ABI_VERSION. */
    uint32_t abiVersion;

    /** Decode the extension instruction at bytes[offset..), where
     *  bytes[offset] is its own opcode byte (>= 128).
     *
     *  Returns the instruction's total byte length, INCLUDING the opcode
     *  byte, so >= 1 — or 0 to reject the program (an opcode this
     *  extension doesn't own, a malformed operand, a length that would run
     *  past bytesLen). Fills decl via extDecl() on success only.
     *
     *  Called on unvalidated bytes, and repeatedly. Must be total and
     *  side-effect free: bound every read against bytesLen
     *  (decodeLeb128Checked, not the unchecked one) and be deterministic. */
    uint32_t (*decode)(const uint8_t *bytes, uint32_t bytesLen, uint32_t offset, uint32_t *decl);

    /** Emit this opcode's native code through `a`, reading its operands
     *  back off `site.bytes`.
     *
     *  The core stages every input and updates acc/TOS itself, so emit()
     *  must not touch the operand stack — Window and AccState are
     *  incomplete here.
     *
     *  Must emit at most the declared `halfwords`; exceeding it is caught,
     *  but an over-generous declaration costs arena bytes at every site. */
    void (*emit)(jitc::Assembler &a, const ExtSite &site);

    /** Worst-case bytes this extension's helpers consume, including
     *  EXT_THUNK_STACK_BYTES for anything reached through
     *  extEmitCHelperCall. Folded into enterProgram*'s up-front budget.
     *
     *  Getting it wrong is caught nowhere: too small and the static
     *  reservation stops being a bound. Prove it with -Wstack-usage=0 as
     *  an error, or hand-written naked Thumb (docs/design.md §3). */
    uint32_t helperStackBytes;
};

namespace jitc
{

/** Call a hand-written-Thumb helper with a known clobber set, reached by a
 *  plain BLX with operands in r0-r2; it must return via `bx lr` and gets no
 *  AAPCS guarantees (sp is legitimately 4-mod-8 inside an excursion). Use
 *  extEmitCHelperCall for anything the C compiler produced.
 *
 *  The declaration must set EXT_FLAG_NEEDS_LR: a BLX clobbers lr, which
 *  carries the live call/return record. */
void extEmitHelperCall(Assembler &a, const ExtSite &site, uint32_t helperAddr);

/** The same for an independently-compiled C helper, via extThunkHelper,
 *  which realigns sp to 8 and preserves lr. Its realignment clobbers
 *  r2/r3, so such a helper takes at most two arguments, in r0 and r1. */
void extEmitCHelperCall(Assembler &a, const ExtSite &site, uint32_t helperAddr);

void extEmitStateBase(Assembler &a, uint32_t dstLowReg);

constexpr uint32_t extStateOffset(uint32_t word)
{
    return RUNTIME_EXT_STATE_OFFSET + word * 4;
}

} // namespace jitc


#endif // JIT_ARMV6M_COMPILER_EXT_H_
