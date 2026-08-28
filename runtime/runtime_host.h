/* Public interface into the runtime's enterProgram family
 * (enter_program.cpp): the two entry points and the ProgramResult they
 * return. ProcSlot/Runtime themselves are not declared here — they're
 * internal (runtime_internal.h) — this header's own job is staying safe
 * for runtime.S to #include under __ASSEMBLER__ (see the two #defines at
 * the bottom), so it carries nothing beyond what a plain-C caller and the
 * assembler both need.
 *
 * Include-guarded like any other header: it was previously only
 * __ASSEMBLER__-guarded, which was enough while exactly one translation
 * unit included it, and stopped being enough the moment a second one did
 * (ProgramResult would be redeclared). Safe under __ASSEMBLER__ too. */
#ifndef JIT_ARMV6M_RUNTIME_HOST_H_
#define JIT_ARMV6M_RUNTIME_HOST_H_

/* runtime.S includes this too (for the two #defines at the bottom),
 * preprocessed but never compiled as C — __ASSEMBLER__ is predefined by GCC
 * for that case, so the struct/function declarations below stay hidden
 * from it. */
#ifndef __ASSEMBLER__

#include <stdint.h>

/* extern "C" so a plain-C host application can link this JIT in just as
 * easily as a C++ one. */
#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    /* What `value` means is decided entirely by `trapped` — see the
     * LANDING_* tags at the bottom of this header. Nothing is encoded in
     * `value`'s own bits under LANDING_SUCCESS or LANDING_TRAP: a program
     * may return any uint32_t, and a trap code may be any uint32_t,
     * without either aliasing the other. Under LANDING_RESOURCE_ERROR it
     * is a structured RESOURCE_* code, which costs that nothing, since
     * the tag is what disambiguates and it is a separate word. */
    uint32_t value;
    uint32_t trapped;
} ProgramResult;

/* programBytes/programSize is one whole serialized jit-armv6m program: the
 * jit-armv6m-specific envelope (max_call_depth:LEB128 total_depth:LEB128 —
 * packages/machine/src/bytecode.ts's encodeJitProgram) prepended to an
 * ordinary isa-core.md §5.5 program (proc_count:LEB128, then each
 * procedure's own arg_count:LEB128 immediately followed by its own body).
 * Both whole-program stats and proc_count come out of that envelope, not a
 * caller-supplied parameter — nothing else needs supplying separately, and
 * nothing here can drift out of sync with what the bytes themselves
 * actually contain. programSize bounds the walk against overflow, the same
 * asserted-not-recovered convention as everywhere else this translator
 * decodes wire bytes (decode_instr.h's own).
 *
 * args/argCount is the entry procedure's (index 0) whole argument vector,
 * args[0..argCount-1] in frame-slot order — the same shape
 * packages/machine's own `run(program, extension, args)` takes, since the
 * two are compared against each other directly. The runtime splits it
 * across the calling convention itself (isa-core.md §4.6: the last
 * argument travels in acc, the rest in the callee's frame), so a caller
 * never reproduces that layout by hand.
 *
 * argCount must equal the entry procedure's own declared arg_count
 * exactly; a mismatch reports RESOURCE_PROGRAM_ENTRY_ARG_COUNT rather
 * than guessing, since neither clamping nor zero-filling could be right:
 * the procedure reads exactly the slots it declared, and the frame it
 * reclaims on the way out is sized from that same number. argCount == 0
 * is the ordinary case for a program taking no arguments, and `args` is
 * then never read (a null pointer is fine).
 *
 * Both variants below set up the dispatch table (and, from the walk above,
 * every slot's own static half — runtime_internal.h's ProcSlot), then call
 * enterDispatch (runtime.S), which dispatches into the entry procedure
 * (index 0) and returns normally once that either completes or bails out.
 * callHelper/returnHelper/enterDispatch are part of the fixed runtime, not
 * parameters — this just extern-declares them by name.
 *
 * Call/return records travel in lr, on the same operand stack (sp) window
 * spills already use — there is no separate control stack.
 *
 * Both are checked, up front, against the program's own worst-case TOS
 * depth and call-chain length (the envelope above) — static, whole-program
 * properties `validateProgram` already computed before this was ever
 * encoded. stackLimit is the lowest address the excursion must never reach
 * or pass, and interruptReserve is how much headroom to hold back for
 * exception entry. On failure, the returned ProgramResult reports
 * RESOURCE_EXHAUSTED_STACK_BUDGET without ever touching that memory or
 * calling into enterDispatch at all. There is no arena-less "just give me somewhere to
 * put the code" variant: a caller that wants a plain global arena declares
 * one itself (one line, sized to what it actually needs) and passes it to
 * enterProgramSplit below. */

/* One ARMv6-M exception frame (R0-R3,R12,LR,return-address,xPSR — 32
 * bytes). A convenience value for interruptReserve below, not the only
 * legal one — a caller with its own RTOS/ISR requirements beyond the bare
 * architectural minimum should pass whatever its own platform needs
 * instead. */
#define ARMV6M_EXCEPTION_FRAME_BYTES 32

/* The current C stack is the whole work area: Runtime, its dispatch table,
 * the operand stack, and the compiled-code arena (sized to codeArenaSize)
 * all come out of it — not stacked one after the other. The arena anchors
 * at stackLimit and grows up from there; Runtime/the operand stack grow
 * down from wherever sp already is, converging toward each other from
 * opposite ends of the same checked range. This leaves room for the
 * translator to temporarily encroach into whatever part of the arena's own
 * reservation is still unused. */
ProgramResult enterProgramOnStack(
    const uint32_t *args,
    uint32_t argCount,
    const uint8_t *programBytes,
    uint32_t programSize,
    uint32_t codeArenaSize,
    uint32_t stackLimit,
    uint32_t interruptReserve);

/* For hardware where the compiled-code arena wants to live somewhere other
 * than plain stack-adjacent RAM (a distinct SRAM bank, CCM, whatever a
 * given target's bus matrix makes worth using, or just a plain static
 * array declared by the caller) — codeArenaBase/codeArenaSize describe
 * that caller-owned region directly. Runtime, its dispatch table, and the
 * operand stack still live on the current C stack regardless. Same
 * stack-limit checking as enterProgramOnStack, except codeArenaSize never
 * enters that check at all, since that memory isn't on this stack to begin
 * with. */
ProgramResult enterProgramSplit(
    const uint32_t *args,
    uint32_t argCount,
    const uint8_t *programBytes,
    uint32_t programSize,
    uint32_t codeArenaBase,
    uint32_t codeArenaSize,
    uint32_t stackLimit,
    uint32_t interruptReserve);

#ifdef __cplusplus
}
#endif

#endif /* __ASSEMBLER__ */

/* runtime.S's own layout constants for reaching the Runtime struct from a
 * bare address — enterDispatch gets the runtime pointer directly (r9), but
 * still needs its dispatch table's own base address (r8) and the sentinel
 * slot one ProcSlot before it.
 *
 * These two #defines have to live in *this* header, not next to Runtime/
 * ProcSlot themselves (runtime_internal.h), because runtime.S #includes
 * this file under __ASSEMBLER__ and Runtime/ProcSlot are C++-only types a
 * bare preprocessor pass can't see — hence the split the two
 * static_asserts at the bottom of runtime_internal.h close: those check
 * the numbers *here* against Runtime's real layout *there*, so any drift
 * between the two files fails the build instead of corrupting memory
 * silently. Change a number on either side and the other file's own
 * assert is what catches the mismatch. */
#define RUNTIME_DISPATCH_TABLE_OFFSET 40 /* &runtime + this = dispatchBase (== &slots[1]) */
#define DISPATCH_SENTINEL_OFFSET 16      /* dispatchBase - this = &slots[0] (the sentinel) */

/* Field offsets into `struct EntryArgs` (entry_args.h), for the same
 * reason and by the same split as the two above: enterDispatch's own
 * argument-marshalling block reads all four out of the descriptor
 * enterProgramCore built for it, and a bare preprocessor pass can't see
 * the C++ struct. entry_args.h closes the loop with static_asserts
 * against the real layout, so drift fails the build.
 *
 * These are the *32-bit target's* offsets — `spilled` is a pointer, so a
 * 64-bit host build (test/host) legitimately lays the struct out wider.
 * runtime.S only ever runs on the target, so the asserts are conditioned
 * on the pointer width rather than weakened. */
#define ENTRY_ARGS_SPILLED_OFFSET 0       /* const uint32_t *spilled */
#define ENTRY_ARGS_SPILLED_COUNT_OFFSET 4 /* uint32_t spilledCount */
#define ENTRY_ARGS_WINDOW_OFFSET 8        /* uint32_t window[WINDOW_SIZE] */
#define ENTRY_ARGS_ACC_OFFSET 24          /* uint32_t acc */

/* ProgramResult::trapped — how the excursion ended, and therefore what
 * `value` holds. These are the tags enterDispatch's own sentinel landing
 * receives in r2 (runtime.S's .Lresume), so they live in this header
 * rather than runtime_internal.h: runtime.S #includes this file under
 * __ASSEMBLER__ and needs LANDING_TRAP by name.
 *
 * Every one of the three arrives at the same landing, and the three
 * routes to it are the whole story of how a compiled excursion can end:
 * an ordinary RETURN from the entry procedure (returnHelperTail resolving
 * the boot record's own procIdx of -1, whose "resume offset" field is
 * zero and so doubles as LANDING_SUCCESS), a bytecode TRAP at any call
 * depth (trapHelper, helper slot 8), or the translator running out of
 * arena (runtimeBail, from C++). */
#define LANDING_SUCCESS 0u        /* value = the entry procedure's own result */
#define LANDING_TRAP 1u           /* value = the bytecode TRAP's own code, at any call depth */
#define LANDING_RESOURCE_ERROR 2u /* value = one of the RESOURCE_* codes below — nothing ran, or ran to completion */

/* ProgramResult::value under LANDING_RESOURCE_ERROR — which of the ways
 * this target can give up actually happened.
 *
 *   31              16 15 12 11  8 7      0
 *  +-----------------+-----+-----+--------+
 *  | 0x5245 ("RE")   |class|reason| resvd |
 *  +-----------------+-----+-----+--------+
 *
 * The 0x5245 signature keeps a code legible in a raw hex dump (fuzz/
 * qemu_exec/exec_runner.cpp prints it as "E:52453400"), and class/reason
 * are decimal-looking hex digits so that line reads straight off as class
 * 3, reason 4. The low byte is reserved for a future per-code detail
 * payload (which of two sites fired, an offending offset): it is zero in
 * every code below, so a consumer that wants to stay forward-compatible
 * compares (value & 0xffffff00u). Nothing populates it yet — don't add one
 * ad hoc.
 *
 * The signature is a human/harness affordance and must never be used as
 * the discriminator; ProgramResult::trapped is. A bytecode TRAP may
 * legitimately carry code 0x52453400 (isa-core.md §4.5 makes it a full
 * u32) and still reports distinctly, because the tag is a separate word.
 *
 * Three classes, split by what the caller can do about it — the only part
 * worth branching on:
 *   PROGRAM    the input is out of contract; fix the program.
 *   EXHAUSTED  genuinely out of room; give it more memory.
 *   LIMIT      unencodable at any size; this backend cannot compile it.
 * design.md §12 carries the same table with each code's own site. */
#define RESOURCE_ERROR_SIGNATURE 0x5245u /* == (value >> 16) for every code below */
#define RESOURCE_ERROR_CLASS(v) (((v) >> 12) & 0xfu)
#define RESOURCE_ERROR_CLASS_PROGRAM 1u
#define RESOURCE_ERROR_CLASS_EXHAUSTED 2u
#define RESOURCE_ERROR_CLASS_LIMIT 3u

#define RESOURCE_PROGRAM_NO_PROCS 0x52451100u          /* proc_count is zero */
#define RESOURCE_PROGRAM_BODY_UNTERMINATED 0x52451200u /* a body ran off the blob with a block still open */
#define RESOURCE_PROGRAM_CALLEE_RANGE 0x52451300u      /* CALL names a procedure index the program doesn't have */
/* 0x52451400 is reserved for a truncated program envelope, which
 * parseProgramHeader asserts rather than reports — it runs before there is
 * anywhere to report to. Not produced by anything yet. */
#define RESOURCE_PROGRAM_ENTRY_ARG_COUNT 0x52451500u /* argCount != the entry procedure's own declared arg_count */
#define RESOURCE_PROGRAM_ENTRY_DEPTH 0x52451600u     /* entry procedure's out-of-window args over the envelope's own total_depth */

#define RESOURCE_EXHAUSTED_ARENA 0x52452100u            /* code arena full with nothing left to evict */
#define RESOURCE_EXHAUSTED_STACK_BUDGET 0x52452200u     /* the up-front whole-excursion stack check failed; nothing was touched */
#define RESOURCE_EXHAUSTED_TRANSLATOR_STACK 0x52452300u /* the translator's own recursion reached the live stack floor */
#define RESOURCE_EXHAUSTED_SCAN_STACK 0x52452400u       /* ditto, in the pre-pass that builds the directory */

#define RESOURCE_LIMIT_WINDOW_RECLAIM 0x52453100u /* frame reclaim past ADD sp,#imm's reach — TOS depth over 131 */
#define RESOURCE_LIMIT_SPILL_OFFSET 0x52453200u   /* SP-relative spill slot past LDR/STR [sp,#imm]'s 1020 bytes */
#define RESOURCE_LIMIT_BRANCH_RANGE 0x52453300u   /* a forward fixup resolved beyond Bcc/B reach */
#define RESOURCE_LIMIT_LOOP_BACK_EDGE 0x52453400u /* a LOOP body outgrew B's own backward reach */
#define RESOURCE_LIMIT_ARG_COUNT 0x52453500u      /* arg_count over ProcSlot's own field width */
/* Nothing reaches the one below: it needs a body over 1MB, which neither the
 * host nor the QEMU build has room to construct. A known gap, not an oversight. */
#define RESOURCE_LIMIT_BODY_BYTES 0x52453600u /* body size over ProcSlot's own field width */

#endif /* JIT_ARMV6M_RUNTIME_HOST_H_ */
