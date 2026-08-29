/* Public interface into enter_program.cpp's enterProgram family.
 * ProcSlot/Runtime are internal (runtime_internal.h) and deliberately not
 * declared here: this header must stay safe for runtime.S to #include
 * under __ASSEMBLER__, which is also why the layout #defines at the bottom
 * live here rather than beside the structs they describe. */
#ifndef JIT_ARMV6M_RUNTIME_HOST_H_
#define JIT_ARMV6M_RUNTIME_HOST_H_

#ifndef __ASSEMBLER__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* compiler/src/ext.h. Incomplete is enough — only ever a pointer here. */
struct ExtHooks;

typedef struct
{
    /* Meaning decided entirely by `trapped` (LANDING_* below). Nothing is
     * encoded in these bits under SUCCESS or TRAP: a program may return any
     * uint32_t and a trap code may be any uint32_t without aliasing. */
    uint32_t value;
    uint32_t trapped;
} ProgramResult;

/* programBytes/programSize is one serialized program: the jit-armv6m
 * envelope (max_call_depth:LEB128 total_depth:LEB128 — encodeJitProgram in
 * packages/machine/src/bytecode.ts) prepended to an isa-core.md §5.5
 * program. proc_count and the whole-program stats come from those bytes,
 * never from a parameter, so nothing can drift out of sync.
 *
 * args/argCount is the entry procedure's whole argument vector in
 * frame-slot order; the runtime splits it across the calling convention
 * itself (isa-core.md §4.6). argCount must equal the entry procedure's
 * declared arg_count exactly — neither clamping nor zero-filling could be
 * right, since the frame it reclaims on the way out is sized from that
 * number, so a mismatch reports RESOURCE_PROGRAM_ENTRY_ARG_COUNT.
 *
 * Both variants check the program's worst-case TOS depth and call-chain
 * length up front, and on failure report RESOURCE_EXHAUSTED_STACK_BUDGET
 * without touching that memory. stackLimit is the lowest address the
 * excursion must never reach; interruptReserve is headroom held back for
 * exception entry. */

/* One ARMv6-M exception frame. A convenience value for interruptReserve,
 * not the only legal one. */
#define ARMV6M_EXCEPTION_FRAME_BYTES 32

/* The current C stack is the whole work area. The arena anchors at
 * stackLimit and grows up; Runtime and the operand stack grow down from
 * sp, converging from opposite ends of one checked range — which is what
 * lets the translator encroach into the arena's unused reservation. */
ProgramResult enterProgramOnStack(
    const uint32_t *args,
    uint32_t argCount,
    const uint8_t *programBytes,
    uint32_t programSize,
    const struct ExtHooks *extension,
    uint32_t codeArenaSize,
    uint32_t stackLimit,
    uint32_t interruptReserve);

/* Same, but the arena is a caller-owned region (a distinct SRAM bank, a
 * static array) described by codeArenaBase/codeArenaSize. Runtime and the
 * operand stack still live on the C stack; codeArenaSize therefore never
 * enters the stack-limit check. */
ProgramResult enterProgramSplit(
    const uint32_t *args,
    uint32_t argCount,
    const uint8_t *programBytes,
    uint32_t programSize,
    const struct ExtHooks *extension,
    uint32_t codeArenaBase,
    uint32_t codeArenaSize,
    uint32_t stackLimit,
    uint32_t interruptReserve);

#ifdef __cplusplus
}
#endif

#endif /* __ASSEMBLER__ */

/* Checked against Runtime's real layout by static_asserts in
 * runtime_internal.h, so drift fails the build rather than corrupting
 * memory. Same for the EntryArgs offsets below (entry_args.h). */
#define RUNTIME_DISPATCH_TABLE_OFFSET 44 /* &runtime + this = dispatchBase (== &slots[1]) */
#define DISPATCH_SENTINEL_OFFSET 16      /* dispatchBase - this = &slots[0] (the sentinel) */

/* Per-excursion extension scratch, at a constant offset from the runtime
 * pointer: the sentinel ProcSlot's bodyPtr/staticInfo, which nothing else
 * reads. Its lastUsed is NOT available — returnHelperTail stamps the LRU
 * tick there unconditionally, sentinel included. Zeroed by Runtime::init. */
#define RUNTIME_EXT_STATE_OFFSET 36
#define RUNTIME_EXT_STATE_WORDS 2

/* The 32-bit target's offsets — `spilled` is a pointer, so a 64-bit host
 * build lays the struct out wider and the asserts are conditioned on
 * pointer width rather than weakened. */
#define ENTRY_ARGS_SPILLED_OFFSET 0       /* const uint32_t *spilled */
#define ENTRY_ARGS_SPILLED_COUNT_OFFSET 4 /* uint32_t spilledCount */
#define ENTRY_ARGS_WINDOW_OFFSET 8        /* uint32_t window[WINDOW_SIZE] */
#define ENTRY_ARGS_ACC_OFFSET 24          /* uint32_t acc */

/* ProgramResult::trapped. All three arrive at the same landing, and they
 * are the only ways an excursion can end: an ordinary RETURN from the entry
 * procedure (the boot record's resume-offset field is zero, so it doubles
 * as LANDING_SUCCESS), a bytecode TRAP at any depth (trapHelper), or the
 * translator running out of arena (runtimeBail). */
#define LANDING_SUCCESS 0u        /* value = the entry procedure's own result */
#define LANDING_TRAP 1u           /* value = the bytecode TRAP's own code, at any call depth */
#define LANDING_RESOURCE_ERROR 2u /* value = one of the RESOURCE_* codes below — nothing ran, or ran to completion */

/* ProgramResult::value under LANDING_RESOURCE_ERROR.
 *
 *   31              16 15 12 11  8 7      0
 *  +-----------------+-----+-----+--------+
 *  | 0x5245 ("RE")   |class|reason| resvd |
 *  +-----------------+-----+-----+--------+
 *
 * The signature keeps a code legible in a hex dump, and class/reason are
 * decimal-looking hex digits so it reads off as class 3, reason 4. The low
 * byte is reserved for a future detail payload and is zero everywhere, so
 * forward-compatible consumers compare (value & 0xffffff00u).
 *
 * The signature must never be used as the discriminator; trapped is. A
 * bytecode TRAP may legitimately carry code 0x52453400 and still report
 * distinctly, the tag being a separate word.
 *
 * Classes split by what the caller can do: PROGRAM — fix the program;
 * EXHAUSTED — give it more memory; LIMIT — this backend cannot compile it.
 * design.md §12 carries each code's own site. */
#define RESOURCE_ERROR_SIGNATURE 0x5245u /* == (value >> 16) for every code below */
#define RESOURCE_ERROR_CLASS(v) (((v) >> 12) & 0xfu)
#define RESOURCE_ERROR_CLASS_PROGRAM 1u
#define RESOURCE_ERROR_CLASS_EXHAUSTED 2u
#define RESOURCE_ERROR_CLASS_LIMIT 3u

#define RESOURCE_PROGRAM_NO_PROCS 0x52451100u          /* proc_count is zero */
#define RESOURCE_PROGRAM_BODY_UNTERMINATED 0x52451200u /* a body ran off the blob with a block still open */
#define RESOURCE_PROGRAM_CALLEE_RANGE 0x52451300u      /* CALL names a procedure index the program doesn't have */
/* 0x52451400 reserved for a truncated envelope, which parseProgramHeader
 * asserts rather than reports — it runs before there is anywhere to report
 * to. Not produced by anything. */
#define RESOURCE_PROGRAM_ENTRY_ARG_COUNT 0x52451500u /* argCount != the entry procedure's own declared arg_count */
#define RESOURCE_PROGRAM_ENTRY_DEPTH 0x52451600u     /* entry procedure's out-of-window args over the envelope's own total_depth */
/* A wire byte in the extension range (>= 128, isa-core.md §11) or a reserved
 * core code (124-127, §5.3). Reported, not asserted: plausibly the right
 * program against an image built without that extension — a deployment
 * mismatch, not malformed bytes. Caught by Runtime::init's directory walk,
 * so no later path can see one. */
#define RESOURCE_PROGRAM_EXT_UNKNOWN 0x52451700u
/* A well-formed extension declaration asking for a capability this core
 * doesn't implement. Distinct from EXT_UNKNOWN because the remedy is a newer
 * core, not a different extension. */
#define RESOURCE_PROGRAM_EXT_UNSUPPORTED 0x52451800u
/* ExtHooks::abiVersion != EXT_ABI_VERSION (compiler/src/ext.h). */
#define RESOURCE_PROGRAM_EXT_ABI 0x52451900u
/* One of the four core opcodes isa-core.md §5.3 reserves but hasn't assigned
 * (124-127) — not an extension byte, those start at 128. */
#define RESOURCE_PROGRAM_RESERVED_OPCODE 0x52451a00u

#define RESOURCE_EXHAUSTED_ARENA 0x52452100u            /* code arena full with nothing left to evict */
#define RESOURCE_EXHAUSTED_STACK_BUDGET 0x52452200u     /* the up-front whole-excursion stack check failed; nothing was touched */
#define RESOURCE_EXHAUSTED_TRANSLATOR_STACK 0x52452300u /* the translator's own recursion reached the live stack floor */
#define RESOURCE_EXHAUSTED_SCAN_STACK 0x52452400u       /* ditto, in the pre-pass that builds the directory */

#define RESOURCE_LIMIT_WINDOW_RECLAIM 0x52453100u /* frame reclaim past ADD sp,#imm's reach — TOS depth over 131 */
#define RESOURCE_LIMIT_SPILL_OFFSET 0x52453200u   /* SP-relative spill slot past LDR/STR [sp,#imm]'s 1020 bytes */
#define RESOURCE_LIMIT_BRANCH_RANGE 0x52453300u   /* a forward fixup resolved beyond Bcc/B reach */
#define RESOURCE_LIMIT_LOOP_BACK_EDGE 0x52453400u /* a LOOP body outgrew B's own backward reach */
#define RESOURCE_LIMIT_ARG_COUNT 0x52453500u      /* arg_count over ProcSlot's own field width */
/* Unreachable: needs a body over 1MB, which neither build has room to
 * construct. A known gap. */
#define RESOURCE_LIMIT_BODY_BYTES 0x52453600u /* body size over ProcSlot's own field width */

#endif /* JIT_ARMV6M_RUNTIME_HOST_H_ */
