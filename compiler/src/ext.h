// jit-armv6m/compiler — the extension seam (isa-core.md §11). THE ONLY
// header an extension includes.
//
// The core owns wire bytes 0..123; everything >= 128 belongs to one
// registered extension, which the core never interprets. What it needs
// instead is exactly two things per opcode, and nothing else:
//
//   1. the instruction's BYTE LENGTH, so proc_scan.cpp's body-boundary
//      walk and blocks.cpp's branch-span walk can step over it;
//   2. its DECLARED EFFECT (§11.2), so validate-side bookkeeping and the
//      prologue's needsLRSave stay correct without knowing semantics.
//
// Both arrive from decode() below, packed into one 32-bit word that rides
// in Instr's existing union — so instrMaxBytes(const Instr&) and
// triggersLRSave(const Instr&) keep their signatures and become bitfield
// reads. That is deliberate: maxSpanBytes re-walks each instruction once
// per enclosing nesting level, so a per-instruction indirect call there
// would cost calls proportional to nesting depth, and it makes it
// structurally impossible for the span budget, the prologue's lr decision
// and codegen to ever see different answers.
//
// OPERANDS ARE NEVER HELD BY THE CORE. They are literal constants
// (§11.3), so the extension re-reads them from the wire at emit time. The
// core carries a length and an effect; that is the whole coupling.
//
// Window and AccState are forward-declared and never defined here on
// purpose: an extension TU that names one fails to compile, so the core's
// register window and acc fusion state stay free to change shape.
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

/** Bumped whenever anything in this header changes shape. Compared once,
 *  at Runtime::init, against the registered ExtHooks::abiVersion — the
 *  only enforced point of the rule that a native declaration must be a
 *  subset of the TS-side one (packages/machine/src/extension.ts). The wire
 *  carries only the *consequences* of effects (max_call_depth,
 *  total_depth), never the effects themselves, so nothing else can catch
 *  an extension built against a stale header. */
constexpr uint32_t EXT_ABI_VERSION = 1;

// ── the packed declaration ───────────────────────────────────────────────
//
//  31    27 26   21 20  18 17   14 13    8 7        0
// +-------+-------+-----+-------+--------+----------+
// | unused|halfwds|maxTr|tosDlta| flags  |  opcode  |
//
// Built with extDecl() below rather than by hand; read with the accessors.

constexpr uint32_t EXT_FLAG_NEEDS_LR = 1u << 0;     // clobbers lr: helper dispatch, or call-shaped
constexpr uint32_t EXT_FLAG_CALL_SHAPED = 1u << 1;  // §11.2 call-shaped — rejected in v1
constexpr uint32_t EXT_FLAG_TERMINATES = 1u << 2;   // ends its block like RETURN/TRAP — rejected in v1
constexpr uint32_t EXT_FLAG_READS_ACC = 1u << 3;    // real input includes whatever acc holds
constexpr uint32_t EXT_FLAG_WRITES_ACC = 1u << 4;   // leaves a fresh value in acc
constexpr uint32_t EXT_FLAG_ATOMIC = 1u << 5;       // emitted halfwords must stay contiguous

/** Pack one opcode's declaration.
 *
 *  opcode        the wire byte (>= 128) this describes.
 *  flags         EXT_FLAG_* above.
 *  tosDelta      net TOS depth change (§11.2). Must be <= 0: the TS side
 *                cannot represent a net push either (extension.ts), and
 *                v1 stages the popped values for the extension, so the
 *                count staged is -tosDelta.
 *  maxTransient  peak TOS above entry depth while executing. Must be 0 in
 *                v1 — window.tos has to agree with the real sp exactly
 *                (there is no per-procedure reservation), so allowing a
 *                transient push reopens a whole desync failure class for
 *                no shipped client.
 *  halfwords     worst-case emitted halfword count, <= 63. The span walk
 *                budgets this, and M2's emit path will require the real
 *                emission to match it exactly, not merely stay under. */
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

/** The ABI-boundary half of this header lives at global scope, alongside
 *  ProgramResult and for the same reason: ExtHooks crosses runtime_host.h,
 *  which is deliberately usable from a plain-C host (which needs only the
 *  incomplete type, to pass NULL). ExtSite and EXT_MAX_INPUTS come with it
 *  because they are part of that struct's own contract. The packing
 *  helpers above are compiler-side and stay in namespace jitc. */
/** At most three inputs, and the reason is mechanical rather than a policy
 *  choice: a helper reach is `MOV r3,r10 / LDR r3,[r3,#off] / BLX r3` — the
 *  only idiom Thumb-1 admits, since LDR cannot use a hi register as base —
 *  so r3 is permanently spoken for and cannot hold an operand. That leaves
 *  r0 (acc), r1 and r2. Stack inputs use r1 and r2 only; r0 carries acc,
 *  and only when the declaration says the op reads it. */
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

    /** Free to clobber for the duration, beyond `in`: r3 (also the only
     *  possible helper-reach target, so it is gone the moment you make
     *  one) and r12/ip. r12 is unclaimed anywhere in this JIT and is
     *  hardware-pushed on exception entry, but it is caller-saved, so a
     *  BLX destroys it — intra-sequence only, never across a helper call
     *  or an instruction boundary. */
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
     *  Called from Runtime::init's directory walk, before anything has
     *  validated these bytes, and again per nesting level from the span
     *  walk. So it must be total and side-effect free: bound every read
     *  against bytesLen (decodeLeb128Checked in decode_instr.h is the
     *  bounded LEB128 to use — the unchecked one has no length limit), and
     *  return the same answer every time for the same bytes. */
    uint32_t (*decode)(const uint8_t *bytes, uint32_t bytesLen, uint32_t offset, uint32_t *decl);

    /** Emit this opcode's native code through `a`, reading its operands
     *  back off `site.bytes`.
     *
     *  The core has already: materialized acc into r0 if the declaration
     *  reads it, moved every stack input into `site.in`, and it will pop
     *  those values and update acc's own state afterwards. So emit() does
     *  nothing about the operand stack itself — it cannot, and must not
     *  try: `Window` and `AccState` are incomplete types here.
     *
     *  Must emit at most the declared `halfwords`. Exceeding it is caught
     *  and reported (the span walk already budgeted that number, and a
     *  conditional branch's reach was computed from it), but a declaration
     *  that is merely generous costs real arena bytes at every site. */
    void (*emit)(jitc::Assembler &a, const ExtSite &site);

    /** Worst-case stack this extension's helpers consume, in bytes,
     *  including EXT_THUNK_STACK_BYTES for anything reached through
     *  extEmitCHelperCall. Folded into enterProgram*'s own up-front budget
     *  check, once — helpers do not recurse into bytecode in v1 (call-shaped
     *  is rejected), so the worst case is the deepest bytecode stack plus
     *  one helper frame, exactly like interruptReserve.
     *
     *  Getting this wrong is not caught anywhere: too small and the static
     *  reservation stops being a bound. Prove it the way the core proves
     *  its own (docs/design.md §3): -Wstack-usage=0 promoted to an error,
     *  or hand-written naked Thumb. */
    uint32_t helperStackBytes;
};

namespace jitc
{

/** Emit a call to one of this extension's own helpers, address in flash.
 *
 *  This form is for a helper written in hand-written Thumb with a known
 *  clobber set, like the core's own clzHelper: it is reached by a plain
 *  BLX, with the staged operands sitting in r0-r2 exactly as emit() got
 *  them, and it must return via `bx lr`. It gets no AAPCS guarantees — in
 *  particular sp is legitimately 4-mod-8 inside an excursion.
 *
 *  Use extEmitCHelperCall below for anything the C compiler produced.
 *
 *  Costs one pooled literal word plus a BLX. The declaration must set
 *  EXT_FLAG_NEEDS_LR: a BLX clobbers lr, which carries the live call/return
 *  record, and the prologue's decision to save it was made from that flag
 *  back in the pre-pass. */
void extEmitHelperCall(Assembler &a, const ExtSite &site, uint32_t helperAddr);

/** The same, for an independently-compiled C helper: routes through
 *  runtime.S's extThunkHelper, which realigns sp to 8 for AAPCS and
 *  preserves lr across the call.
 *
 *  The thunk's realignment clobbers r2/r3, so a C helper reached this way
 *  takes at most TWO arguments, in r0 and r1. Declare tosDelta and
 *  READS_ACC so the core stages no more than that. */
void extEmitCHelperCall(Assembler &a, const ExtSite &site, uint32_t helperAddr);

void extEmitStateBase(Assembler &a, uint32_t dstLowReg);

constexpr uint32_t extStateOffset(uint32_t word)
{
    return RUNTIME_EXT_STATE_OFFSET + word * 4;
}

} // namespace jitc


#endif // JIT_ARMV6M_COMPILER_EXT_H_
