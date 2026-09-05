// jit-armv6m/fuzz — writes the whole seeds/ corpus in the whole-program
// envelope format harness.cpp/oracle_server.ts speak (bytecode.ts's
// encodeJitEnvelope) — unframed: the fuzzer mutates these bytes, and
// harness.cpp reaches Runtime::loadProgram without passing the frame check.
//
// Every seed goes through validateProgram here, so one that doesn't
// validate fails this script instead of silently becoming a seed the
// harness discards on every execution.
//
// Three groups: the small single-procedure shapes (also what
// test/qemu/test_*.cpp exercise on real hardware), the
// multi-procedure/CALL shapes no single-procedure format could express at
// all (isa-core.md §8.2 rejects self-recursion, so a lone procedure can
// never legally CALL anything), and the large shapes aimed at specific
// compiled-size guards.
//
// Run: npx ts-node --transpile-only jit-armv6m/fuzz/ts/make_seeds.ts
// (from the repo root, with TS_NODE_PROJECT=jit-armv6m/fuzz/tsconfig.json)

import * as fs from "fs"
import * as path from "path"
import { decodeLeb128, decodeBody, encodeJitEnvelope, extInstr, validateProgram } from "../../../packages/machine/src/index"
import type { RtlInstr, RtlProc, RtlProgram } from "../../../packages/machine/src/index"
import { rawMemExtension } from "./lib/rawmem_ext"

const SEED_DIR = path.join(__dirname, "..", "seeds")

/** dump_seeds.cpp's staging output: ../support/bytecode/corpus_programs.h's bodies,
 *  each as one arg_count LEB128 followed by that procedure's own body
 *  bytes. Wrapped into whole-program envelopes below rather than
 *  re-authored here, so the shapes test/qemu/test_*.cpp exercise on real
 *  hardware stay a single definition. A dedicated directory, not a guess
 *  about which files in seeds/ happen to be in the older format — a
 *  new-format seed's header bytes decode as a plausible arg_count and body
 *  too, so that guess could not be made reliably. */
const STAGING_DIR = path.join(__dirname, "..", "seeds_raw")

const EXT = rawMemExtension()

function write(name: string, program: RtlProgram): void
{
    const stats = validateProgram(program, EXT) // throws rather than emit a seed the harness would discard
    const bytes = encodeJitEnvelope(program, EXT)
    fs.writeFileSync(path.join(SEED_DIR, name), bytes)
    console.log(`wrote seeds/${name} (${bytes.length} bytes, ${program.procedures.length} proc, `
        + `totalDepth ${stats.totalDepth}, maxCallDepth ${stats.maxCallDepth})`)
}

// ── hand-authored multi-procedure shapes ────────────────────────────────

const ret = (body: RtlInstr[]): RtlInstr[] => [...body, { op: "RETURN" }]

/** One CALL, callee takes no arguments: the whole argument shuffle is
 *  skipped, so this isolates the dispatch/return half on its own. */
const callNoArgs: RtlProgram = {
    procedures: [
        { argCount: 0, body: ret([{ op: "CALL", calleeIndex: 1 }, { op: "ADD", combo: "IMM_ACC", imm: 1 }]) },
        { argCount: 0, body: ret([{ op: "CONST", imm: 7 }]) },
    ],
}

/** Callee takes one argument, which by §4.6 arrives in acc alone — no
 *  stack argument at all, the boundary case of the shuffle. */
const callOneArg: RtlProgram = {
    procedures: [
        { argCount: 0, body: ret([{ op: "CONST", imm: 3 }, { op: "CALL", calleeIndex: 1 }]) },
        { argCount: 1, body: ret([{ op: "MUL", combo: "REG_ACC", target: 0 }]) },
    ],
}

/** Callee takes four arguments: three pushed on the operand stack plus the
 *  last in acc, exactly filling the 4-register window, so both spillForCall
 *  and reloadAfterCall do real work. */
const callFourArgs: RtlProgram = {
    procedures: [
        {
            argCount: 0,
            body: ret([
                { op: "CONST", imm: 1 }, { op: "PUSH" },
                { op: "CONST", imm: 2 }, { op: "PUSH" },
                { op: "CONST", imm: 3 }, { op: "PUSH" },
                { op: "CONST", imm: 4 },
                { op: "CALL", calleeIndex: 1 },
            ]),
        },
        {
            argCount: 4,
            body: ret([
                { op: "LOAD", target: 0 },
                { op: "ADD", combo: "REG_ACC", target: 1 },
                { op: "ADD", combo: "REG_ACC", target: 2 },
                { op: "ADD", combo: "REG_ACC", target: 3 },
            ]),
        },
    ],
}

/** Callee takes six arguments: two of them spill *below* the window, so the
 *  callee's own prologue/discardWindow reclaim path runs, not just the
 *  in-window case. */
const callSpilledArgs: RtlProgram = {
    procedures: [
        {
            argCount: 0,
            body: ret([
                { op: "CONST", imm: 1 }, { op: "PUSH" },
                { op: "CONST", imm: 2 }, { op: "PUSH" },
                { op: "CONST", imm: 3 }, { op: "PUSH" },
                { op: "CONST", imm: 4 }, { op: "PUSH" },
                { op: "CONST", imm: 5 }, { op: "PUSH" },
                { op: "CONST", imm: 6 },
                { op: "CALL", calleeIndex: 1 },
            ]),
        },
        {
            argCount: 6,
            body: ret([
                { op: "LOAD", target: 0 },
                { op: "ADD", combo: "REG_ACC", target: 5 },
                { op: "SUB", combo: "REG_ACC", target: 1 },
            ]),
        },
    ],
}

/** A three-deep chain (0 → 1 → 2), so maxCallDepth in the envelope is
 *  greater than one and the tight per-site depth bound has something to
 *  actually add up. */
const callChain: RtlProgram = {
    procedures: [
        { argCount: 0, body: ret([{ op: "CONST", imm: 5 }, { op: "PUSH" }, { op: "CONST", imm: 6 }, { op: "CALL", calleeIndex: 1 }]) },
        { argCount: 2, body: ret([{ op: "LOAD", target: 0 }, { op: "CALL", calleeIndex: 2 }, { op: "ADD", combo: "REG_ACC", target: 1 }]) },
        { argCount: 1, body: ret([{ op: "NEG" }]) },
    ],
}

/** Two independent call sites onto the same callee, one of them inside a
 *  BR_TABLE case — a call under an open construct, where the window state
 *  at the call boundary is whatever that case left. */
const callInBranch: RtlProgram = {
    procedures: [
        {
            argCount: 1,
            body: [
                { op: "LOAD", target: 0 },
                { op: "BR_TABLE", imm: 1 },
                    { op: "CONST", imm: 1 }, { op: "CALL", calleeIndex: 1 }, { op: "STORE", target: 0 },
                    { op: "BLOCK_END" },
                    { op: "CONST", imm: 2 }, { op: "CALL", calleeIndex: 1 }, { op: "STORE", target: 0 },
                    { op: "BLOCK_END" },
                { op: "LOAD", target: 0 },
                { op: "RETURN" },
            ],
        },
        { argCount: 1, body: ret([{ op: "SHL", combo: "IMM_ACC", imm: 2 }]) },
    ],
}

/** A call inside a loop body, so the callee's own spill/reload interacts
 *  with a back-edge's window restore. */
const callInLoop: RtlProgram = {
    procedures: [
        {
            argCount: 1,
            body: [
                { op: "CONST", imm: 0 }, { op: "PUSH" },
                { op: "LOOP_PRE" },
                    { op: "LOAD", target: 0 }, { op: "CALL", calleeIndex: 1 },
                    { op: "ADD", combo: "REG_ACC", target: 1 }, { op: "STORE", target: 1 },
                    { op: "LOAD", target: 0 }, { op: "SUB", combo: "IMM_ACC", imm: 1 }, { op: "STORE", target: 0 },
                    { op: "BLOCK_END" },
                    { op: "LOAD", target: 0 },
                    { op: "BLOCK_END" },
                { op: "LOAD", target: 1 },
                { op: "RETURN" },
            ],
        },
        { argCount: 1, body: ret([{ op: "XOR", combo: "IMM_ACC", imm: 0x5a }]) },
    ],
}

/** A jump-table BR_TABLE whose every case leaves acc live, so isa-core.md
 *  §8.7 carries it across the merge and the RETURN reads it. Worth a seed
 *  of its own: this is the region two crashes already came from, and a
 *  blind mutator rarely builds a live-in-every-case dispatch on its own. */
const brTableLiveMerge: RtlProgram = {
    procedures: [
        {
            argCount: 1,
            body: [
                { op: "LOAD", target: 0 },
                { op: "BR_TABLE", imm: 2 },
                    { op: "CONST", imm: 10 }, { op: "BLOCK_END" },
                    { op: "CONST", imm: 20 }, { op: "BLOCK_END" },
                    { op: "CONST", imm: 30 }, { op: "BLOCK_END" },
                { op: "RETURN" },
            ],
        },
    ],
}

/** The opposite: `BR_TABLE 1` shaped as an if-without-else, whose one real
 *  case leaves acc live while the empty default case cannot — so the merge
 *  is dead and the code after it carries its own producer. */
const ifThenDeadMerge: RtlProgram = {
    procedures: [
        {
            argCount: 1,
            body: [
                { op: "LOAD", target: 0 },
                { op: "BR_TABLE", imm: 1 },
                    { op: "BLOCK_END" },
                    { op: "CONST", imm: 99 }, { op: "BLOCK_END" },
                { op: "CONST", imm: 11 },
                { op: "RETURN" },
            ],
        },
    ],
}

/** Fused form of the same: a comparison immediately before the BR_TABLE 1,
 *  which blocks.cpp folds into the branch condition without ever
 *  materializing a 0/1 — the exact optimization isa-rationale.md names as
 *  the reason §8.7's rule is unconditional. */
const fusedIfThenDeadMerge: RtlProgram = {
    procedures: [
        {
            argCount: 1,
            body: [
                { op: "LOAD", target: 0 },
                { op: "NE", combo: "IMM_ACC", imm: 0 },
                { op: "BR_TABLE", imm: 1 },
                    { op: "BLOCK_END" },
                    { op: "CONST", imm: 77 }, { op: "BLOCK_END" },
                { op: "CONST", imm: 22 },
                { op: "RETURN" },
            ],
        },
    ],
}

/** A plain pre-test countdown loop. Replaces the legacy `loop` seed, which
 *  never validated at all (a BLOCK_END with no open block) and so was
 *  silently discarded on every single execution the harness ever ran it
 *  through. */
const countdownLoop: RtlProgram = {
    procedures: [
        {
            argCount: 1,
            body: [
                { op: "CONST", imm: 0 }, { op: "PUSH" },
                { op: "LOOP_PRE" },
                    { op: "LOAD", target: 1 }, { op: "ADD", combo: "REG_ACC", target: 0 }, { op: "STORE", target: 1 },
                    { op: "LOAD", target: 0 }, { op: "SUB", combo: "IMM_ACC", imm: 1 }, { op: "STORE", target: 0 },
                    { op: "BLOCK_END" },
                    { op: "LOAD", target: 0 },
                    { op: "BLOCK_END" },
                { op: "LOAD", target: 1 },
                { op: "RETURN" },
            ],
        },
    ],
}

// ── large shapes, aimed at specific size guards ─────────────────────────
//
// Every guard below is a range check on a *compiled* size — a Thumb branch
// offset field, the literal pool's PC-relative reach, an SP-relative
// offset's immediate width. A blind mutator working up from 10-to-50-byte
// seeds essentially never reaches any of them: it would have to grow a
// program by hundreds of coherent bytes before the first one even comes
// into play. These give it a starting point on the right side of each
// threshold, so mutation explores *around* the guard rather than never
// arriving. Several are expected to bail with RESOURCE_ERROR outright —
// that is a correct outcome, and the neighbourhood is the point.

const repeat = <T>(n: number, f: (i: number) => T[]): T[] =>
    Array.from({ length: n }, (_, i) => f(i)).flat()

/** A BR_TABLE 1 whose case body compiles well past blocks.cpp's own
 *  SAFE_COND_BRANCH_SPAN (240 bytes), forcing emitGuardedBranch's
 *  placeholder-plus-long-unconditional-branch path instead of a plain
 *  conditional branch. */
const longBranchSpan: RtlProgram = {
    procedures: [
        {
            argCount: 1,
            body: [
                { op: "LOAD", target: 0 },
                { op: "BR_TABLE", imm: 1 },
                    { op: "BLOCK_END" },
                    ...repeat<RtlInstr>(60, i => [
                        { op: "CONST", imm: 0x10000 + i },
                        { op: "STORE", target: 0 },
                    ]),
                    { op: "BLOCK_END" },
                { op: "LOAD", target: 0 },
                { op: "RETURN" },
            ],
        },
    ],
}

/** A loop whose *condition* compiles past Ioff<1,11>'s own reach — that is
 *  what the back-edge spans under isa-core.md §7.2's block order — so the
 *  branch cannot encode and translateLoop must bail rather than emit an
 *  out-of-range offset.
 *
 *  The padding writes to a scratch slot rather than to the one the
 *  condition tests, so the loop still terminates: a condition that
 *  overwrote its own test value would run forever and the harness would
 *  skip this seed as non-terminating (§9) instead of reaching the bail. */
const longLoopBackEdge: RtlProgram = {
    procedures: [
        {
            argCount: 1,
            body: [
                { op: "CONST", imm: 0 }, { op: "PUSH" },   // k1: scratch, what the padding writes
                { op: "CONST", imm: 1 }, { op: "PUSH" },   // k2: the counter the condition tests
                { op: "LOOP_PRE" },
                    { op: "LOAD", target: 2 }, { op: "SUB", combo: "IMM_ACC", imm: 1 }, { op: "STORE", target: 2 },
                    { op: "BLOCK_END" },
                    ...repeat<RtlInstr>(400, i => [
                        { op: "CONST", imm: 0x20000 + i },
                        { op: "STORE", target: 1 },
                    ]),
                    { op: "LOAD", target: 2 },
                    { op: "BLOCK_END" },
                { op: "LOAD", target: 2 },
                { op: "RETURN" },
            ],
        },
    ],
}

/** The other half of the budget §7.2's order splits: a loop whose *body*
 *  runs past that same reach, so it is the entry branch that cannot encode.
 *  Neither shape could be told from the other before the split. */
const longLoopEntryBranch: RtlProgram = {
    procedures: [
        {
            argCount: 1,
            body: [
                { op: "LOOP_PRE" },
                    ...repeat<RtlInstr>(400, i => [
                        { op: "CONST", imm: 0x20000 + i },
                        { op: "STORE", target: 0 },
                    ]),
                    { op: "CONST", imm: 0 },
                    { op: "STORE", target: 0 },
                    { op: "BLOCK_END" },
                    { op: "LOAD", target: 0 },
                    { op: "BLOCK_END" },
                { op: "LOAD", target: 0 },
                { op: "RETURN" },
            ],
        },
    ],
}

/** Nothing but immediates too wide for a MOVS, one after another: every one
 *  is a literal-pool candidate, so this drives assembler.cpp's pool
 *  chunking, its LITERAL_POOL_MAX_REACH guard and its POOL_MAX_PENDING
 *  flush threshold in a way a handful of constants never does. */
const literalPoolPressure: RtlProgram = {
    procedures: [
        {
            argCount: 1,
            body: [
                ...repeat<RtlInstr>(120, i => [
                    { op: "CONST", imm: 0x12340000 + i * 7 },
                    { op: "ADD", combo: "REG_ACC", target: 0 },
                    { op: "STORE", target: 0 },
                ]),
                { op: "LOAD", target: 0 },
                { op: "RETURN" },
            ],
        },
    ],
}

/** An operand stack as deep as this target can actually compile: 128 slots,
 *  just under `Window::discardWindow`'s own hard ceiling of
 *  WINDOW_SIZE + 127 = 131 (its single `ADD sp, sp, #imm` reclaim is a
 *  7-bit word immediate). So the deepest slot's SP-relative spill offset is
 *  around 500 bytes, and every LDR/STR against it goes through
 *  translate_proc.cpp's spillImm.
 *
 *  spillImm's own Uoff<2,8> (1020-byte) guard is unreachable:
 *  discardWindow's much tighter ceiling bails first on any program deep
 *  enough to reach it. That guard is defence in depth with nothing behind
 *  it, and a 300-deep
 *  seed only ever exercised the bail. */
const deepSpill: RtlProgram = {
    procedures: [
        {
            argCount: 0,
            body: [
                ...repeat<RtlInstr>(128, i => [
                    { op: "CONST", imm: i },
                    { op: "PUSH" },
                ]),
                { op: "LOAD", target: 0 },
                { op: "ADD", combo: "REG_ACC", target: 127 },
                { op: "RETURN" },
            ],
        },
    ],
}

/** 80 nested loops. The translator's real recursion is
 *  processUntilTerminator -> processNonTerminators -> back into a construct
 *  handler, and checkStackFloor is the only thing bounding it — this is the
 *  shape that actually drives it deep. */
const deepNesting: RtlProgram = (() =>
{
    const depth = 80
    const body: RtlInstr[] = []
    for(let i = 0; i < depth; i++)
    {
        body.push({ op: "LOOP_PRE" })
    }
    body.push({ op: "CONST", imm: 1 })
    // Close each body block, then each condition block — every condition
    // exits immediately, so none of these ever takes its back-edge.
    for(let i = 0; i < depth; i++)
    {
        body.push({ op: "BLOCK_END" })
        body.push({ op: "CONST", imm: 0 })
        body.push({ op: "BLOCK_END" })
    }
    body.push({ op: "CONST", imm: 2 })
    body.push({ op: "RETURN" })
    return { procedures: [{ argCount: 0, body }] }
})()

/** A BR_TABLE with 100 cases: the N > 2 form, so translateSwitch emits a
 *  real jump table plus the computed BX, with a table span (6 + (N+1)*2
 *  bytes) big enough to matter to its own ensurePoolRoom reservation. */
const largeSwitch: RtlProgram = (() =>
{
    const n = 100
    const body: RtlInstr[] = [{ op: "LOAD", target: 0 }, { op: "BR_TABLE", imm: n }]
    for(let i = 0; i <= n; i++)
    {
        body.push({ op: "CONST", imm: i })
        body.push({ op: "STORE", target: 0 })
        body.push({ op: "BLOCK_END" })
    }
    body.push({ op: "LOAD", target: 0 })
    body.push({ op: "RETURN" })
    return { procedures: [{ argCount: 1, body }] }
})()

/** The full procedure-table width the oracle's realistic-profile gate
 *  allows, as a straight call chain — so Runtime::init walks a full
 *  directory, maxCallDepth is one short of it, and the attached-Assembler
 *  pass has enough resident procedures for eviction to have real choices
 *  to make. */
const wideCallChain: RtlProgram = (() =>
{
    const n = 16
    const procedures: RtlProc[] = []
    for(let i = 0; i < n; i++)
    {
        procedures.push(i + 1 < n
            ? { argCount: 1, body: ret([{ op: "CALL", calleeIndex: i + 1 }, { op: "ADD", combo: "REG_ACC", target: 0 }]) }
            : { argCount: 1, body: ret([{ op: "NOT" }]) })
    }
    // Procedure 0 is the entry and takes no argument, so the VM can run it.
    procedures[0] = { argCount: 0, body: ret([{ op: "CONST", imm: 1 }, { op: "CALL", calleeIndex: 1 }]) }
    return { procedures }
})()

// ── the four smallest shapes, with no other definition anywhere ─────────

const constReturn: RtlProgram = { procedures: [{ argCount: 0, body: ret([{ op: "CONST", imm: 37 }]) }] }

const shift: RtlProgram = {
    procedures: [{ argCount: 0, body: ret([{ op: "CONST", imm: 5 }, { op: "SHR", combo: "IMM_ACC", imm: 3 }]) }],
}

/** The one seed reaching a terminator that isn't RETURN. */
const trap: RtlProgram = { procedures: [{ argCount: 0, body: [{ op: "TRAP", imm: 5 }] }] }

/** A TRAP in a *nested* procedure — the four-instruction program
 *  qemu_exec minimized the original 195-instruction finding down to.
 *  Compiled as an ordinary return, proc1's code landed in proc0's acc as a
 *  return value and proc0 carried on to return 92; it has to trap 754
 *  (runtime.S's trapHelper unwinds the whole excursion). */
const nestedTrap: RtlProgram = {
    procedures: [
        { argCount: 0, body: [{ op: "CALL", calleeIndex: 1 }, { op: "CONST", imm: 92 }, { op: "RETURN" }] },
        { argCount: 0, body: [{ op: "TRAP", imm: 754 }] },
    ],
}

/** The same two levels down, out of the frame shape that is worst to
 *  unwind: proc1 takes 5 arguments, so its own arg0 sits out of window
 *  below the call record its prologue pushed, and both frames have live
 *  pushed locals when proc2 traps. One `mov sp, savedSp` has to subsume
 *  all of it — no window discard, no record retrieval, no reclaim. */
const deepNestedTrap: RtlProgram = {
    procedures: [
        {
            argCount: 0,
            body: [
                { op: "CONST", imm: 7 }, { op: "PUSH" },
                { op: "CONST", imm: 10 }, { op: "PUSH" },
                { op: "CONST", imm: 11 }, { op: "PUSH" },
                { op: "CONST", imm: 12 }, { op: "PUSH" },
                { op: "CONST", imm: 13 }, { op: "PUSH" },
                { op: "CONST", imm: 14 },
                { op: "CALL", calleeIndex: 1 },
                { op: "ADD", combo: "REG_ACC", target: 0 },
                { op: "RETURN" },
            ],
        },
        {
            argCount: 5,
            body: [
                { op: "LOAD", target: 0 }, { op: "PUSH" },
                { op: "CONST", imm: 21 },
                { op: "CALL", calleeIndex: 2 },
                { op: "RETURN" },
            ],
        },
        { argCount: 1, body: [{ op: "TRAP", imm: 1000 }] },
    ],
}

/** PEEK_PEEK and POP_ACC: the two combos that read the operand stack
 *  rather than a register, which none of the register-combo shapes cover.
 *  PEEK_PEEK clobbers acc (it writes back in place), so the LOAD is what
 *  re-establishes it — isa-core.md §8.7. */
const arith: RtlProgram = {
    procedures: [
        {
            argCount: 0,
            body: ret([
                { op: "CONST", imm: 5 }, { op: "PUSH" },
                { op: "CONST", imm: 3 }, { op: "ADD", combo: "PEEK_PEEK" }, // k0 = 8
                { op: "CONST", imm: 4 }, { op: "PUSH" },                    // k1 = 4
                { op: "LOAD", target: 0 },
                { op: "ADD", combo: "POP_ACC" },                            // 8 + 4
            ]),
        },
    ],
}

/** The signed opcodes and the four extend ops (isa-core.md §4.1-§4.3) —
 *  the shapes the DSL's own signed types lower to. Every value is chosen so
 *  the signed and unsigned answers differ, so a divergence here is a wrong
 *  result rather than an equal one reached differently. */
const signedAndExtend: RtlProgram = {
    procedures: [
        {
            argCount: 0,
            body: ret([
                { op: "CONST", imm: 0xffffff90 },     // -112
                { op: "SXTB" },                        // still -112
                { op: "PUSH" },
                { op: "CONST", imm: 0xffffff90 },
                { op: "UXTB" },                        // 0x90 = 144
                { op: "ASR", combo: "IMM_ACC", imm: 2 },
                { op: "PUSH" },
                { op: "CONST", imm: 0xdeadbeef },
                { op: "SXTH" },
                { op: "ASR", combo: "IMM_ACC", imm: 4 },
                { op: "LT_S", combo: "POP_ACC" },      // signed: differs from LT_U
                { op: "PUSH" },
                { op: "CONST", imm: 0xdeadbeef },
                { op: "UXTH" },
                { op: "GE_S", combo: "POP_ACC" },
                { op: "ADD", combo: "POP_ACC" },
            ]),
        },
    ],
}

// ── the extension seam ──────────────────────────────────────────────────
//
// The raw-memory test extension (rawmem_ext.ts / support/ext-rawmem/ext_rawmem.cpp) is
// the only extension either half carries, and until these existed no EXT
// instruction had ever been fuzzed: ExtSite's window and acc services, the
// hand-written MEMMOVE helper and extThunkHelper's AAPCS reach were all
// covered by fixed unit tests alone. Addresses are deliberately past the
// buffer and off alignment, since masking and aligning is the whole safety
// argument the emitted code carries instead of a bounds check.

const st = (addr: number, value: number, op: string): RtlInstr[] => [
    { op: "CONST", imm: addr }, { op: "PUSH" },
    { op: "CONST", imm: value },
    extInstr(op, []),
]

const ld = (addr: number, op: string): RtlInstr[] => [
    { op: "CONST", imm: addr },
    extInstr(op, []),
]

/** All six load/store widths, each addressed so the mask and the
 *  align-down both do something. */
const extLoadStore: RtlProgram = {
    procedures: [
        {
            argCount: 0,
            body: ret([
                ...st(0x40, 0x11223344, "ST32"),
                ...st(0x445, 0xaabbccdd, "ST16"),   // 0x445 masks to 0x45, aligns to 0x44
                ...st(0x803, 0xee, "ST8"),          // masks to 0x03
                ...ld(0x41, "LD8"),                 // 0x33
                { op: "PUSH" },
                ...ld(0x42, "LD16"),                // aligns to 0x42 → 0x1122
                { op: "ADD", combo: "POP_ACC" },
                { op: "PUSH" },
                ...ld(0x443, "LD32"),               // masks to 0x43, aligns to 0x40
                { op: "XOR", combo: "POP_ACC" },
                { op: "PUSH" },
                ...ld(0x1003, "LD8"),               // masks to 0x03
                { op: "ADD", combo: "POP_ACC" },
            ]),
        },
    ],
}

/** MEMMOVE: three operands off the stack, none of them acc, through the
 *  hand-written Thumb helper. Its destination range overlaps its source,
 *  which the forward byte copy makes defined rather than divergent. acc is
 *  re-established by the CONST after it — MEMMOVE leaves it undefined. */
const extMemmove: RtlProgram = {
    procedures: [
        {
            argCount: 0,
            body: ret([
                ...st(0x10, 0x01020304, "ST32"),
                ...st(0x14, 0x05060708, "ST32"),
                { op: "CONST", imm: 0x10 }, { op: "PUSH" },   // src
                { op: "CONST", imm: 0x12 }, { op: "PUSH" },   // dst start
                { op: "CONST", imm: 0x1a }, { op: "PUSH" },   // dst end
                extInstr("MEMMOVE", []),
                ...ld(0x14, "LD32"),
                { op: "PUSH" },
                ...ld(0x18, "LD32"),
                { op: "ADD", combo: "POP_ACC" },
            ]),
        },
    ],
}

/** An empty destination range — end at or below start copies nothing —
 *  and a source that wraps the mask on the way. */
const extMemmoveEmpty: RtlProgram = {
    procedures: [
        {
            argCount: 0,
            body: ret([
                ...st(0x20, 0x99887766, "ST32"),
                { op: "CONST", imm: 0x3fe }, { op: "PUSH" },  // src, wraps
                { op: "CONST", imm: 0x30 }, { op: "PUSH" },
                { op: "CONST", imm: 0x30 }, { op: "PUSH" },   // end == start
                extInstr("MEMMOVE", []),
                { op: "CONST", imm: 0x3fc }, { op: "PUSH" },
                { op: "CONST", imm: 0x30 }, { op: "PUSH" },
                { op: "CONST", imm: 0x38 }, { op: "PUSH" },
                extInstr("MEMMOVE", []),
                ...ld(0x30, "LD32"),
                { op: "PUSH" },
                ...ld(0x34, "LD32"),
                { op: "XOR", combo: "POP_ACC" },
            ]),
        },
    ],
}

/** MEMCMP: three operands, and the first extension op here to reach a
 *  helper through extThunkHelper's AAPCS realignment rather than a raw
 *  BLX. Both an equal and a differing range, so the early-out and the
 *  run-to-end path both run. */
const extMemcmp: RtlProgram = {
    procedures: [
        {
            argCount: 0,
            body: ret([
                ...st(0x50, 0x04030201, "ST32"),
                ...st(0x60, 0x04030201, "ST32"),
                ...st(0x70, 0x04ff0201, "ST32"),
                { op: "CONST", imm: 0x50 }, { op: "PUSH" },
                { op: "CONST", imm: 0x54 }, { op: "PUSH" },
                { op: "CONST", imm: 0x60 }, { op: "PUSH" },
                extInstr("MEMCMP", []),                       // equal → 0
                { op: "PUSH" },
                { op: "CONST", imm: 0x50 }, { op: "PUSH" },
                { op: "CONST", imm: 0x54 }, { op: "PUSH" },
                { op: "CONST", imm: 0x70 }, { op: "PUSH" },
                extInstr("MEMCMP", []),                       // differs at byte 2
                { op: "SUB", combo: "POP_ACC" },
            ]),
        },
    ],
}

/** SLICECMP: four operands, one more than the argument registers hold, so
 *  the emitted code pushes the fourth and releases it again — the one op
 *  whose maxTransient is not zero. */
const extSliceCmp: RtlProgram = {
    procedures: [
        {
            argCount: 0,
            body: ret([
                ...st(0x80, 0x04030201, "ST32"),
                ...st(0x90, 0x04030201, "ST32"),
                { op: "CONST", imm: 0x80 }, { op: "PUSH" },
                { op: "CONST", imm: 0x84 }, { op: "PUSH" },
                { op: "CONST", imm: 0x90 }, { op: "PUSH" },
                { op: "CONST", imm: 0x93 }, { op: "PUSH" },
                extInstr("SLICECMP", []),                     // longer a → length difference
                { op: "PUSH" },
                { op: "CONST", imm: 0x80 }, { op: "PUSH" },
                { op: "CONST", imm: 0x84 }, { op: "PUSH" },
                { op: "CONST", imm: 0x90 }, { op: "PUSH" },
                { op: "CONST", imm: 0x94 }, { op: "PUSH" },
                extInstr("SLICECMP", []),                     // equal
                { op: "ADD", combo: "POP_ACC" },
            ]),
        },
    ],
}

/** Every ExtSite stack service reached past the window: the operands sit
 *  deep enough that load/store/pop go through spillOffset rather than a
 *  register, which is where an off-by-one costs the whole answer. */
const extSpilledOperands: RtlProgram = {
    procedures: [
        {
            argCount: 0,
            body: ret([
                { op: "CONST", imm: 0x101 }, { op: "PUSH" },
                { op: "CONST", imm: 0x202 }, { op: "PUSH" },
                { op: "CONST", imm: 0x303 }, { op: "PUSH" },
                { op: "CONST", imm: 0x404 }, { op: "PUSH" },
                { op: "CONST", imm: 0x505 }, { op: "PUSH" },
                { op: "CONST", imm: 0xa0 }, { op: "PUSH" },   // address, spilled
                { op: "CONST", imm: 0x5a5a1234 },
                extInstr("ST32", []),
                ...ld(0xa0, "LD32"),
                { op: "ADD", combo: "REG_ACC", target: 0 },
                { op: "ADD", combo: "REG_ACC", target: 4 },
            ]),
        },
    ],
}

/** EXT inside a loop body, so the window and acc state cross a back edge
 *  with an extension site in between: a running checksum written into the
 *  buffer and read back on the next iteration. */
const extInLoop: RtlProgram = {
    procedures: [
        {
            argCount: 1,
            body: [
                { op: "CONST", imm: 0 }, { op: "PUSH" },
                { op: "LOOP_PRE" },
                    { op: "LOAD", target: 0 }, { op: "SHL", combo: "IMM_ACC", imm: 2 }, { op: "PUSH" },
                    { op: "LOAD", target: 1 }, { op: "ADD", combo: "REG_ACC", target: 0 },
                    extInstr("ST32", []),
                    { op: "LOAD", target: 0 }, { op: "SHL", combo: "IMM_ACC", imm: 2 },
                    extInstr("LD32", []),
                    { op: "STORE", target: 1 },
                    { op: "LOAD", target: 0 }, { op: "SUB", combo: "IMM_ACC", imm: 1 }, { op: "STORE", target: 0 },
                    { op: "BLOCK_END" },
                    { op: "LOAD", target: 0 },
                    { op: "BLOCK_END" },
                { op: "CONST", imm: 0 }, { op: "PUSH" },
                { op: "CONST", imm: 0x10 }, { op: "PUSH" },
                { op: "CONST", imm: 0 }, { op: "PUSH" },
                extInstr("MEMCMP", []),
                { op: "RETURN" },
            ],
        },
    ],
}

/** MEMMOVE where acc is already dead going in: a dispatch case starts with
 *  acc not live (isa-core.md §8.7), and a kill op there has nothing to
 *  destroy. The arm re-establishes one before the merge, which is what
 *  makes the whole thing legal. */
const extKillDeadAcc: RtlProgram = {
    procedures: [
        {
            argCount: 0,
            body: ret([
                ...st(0x10, 0x0a0b0c0d, "ST32"),
                { op: "CONST", imm: 0 },
                { op: "BR_TABLE", imm: 1 },
                    { op: "CONST", imm: 0x10 }, { op: "PUSH" },   // src
                    { op: "CONST", imm: 0x20 }, { op: "PUSH" },   // dst start
                    { op: "CONST", imm: 0x24 }, { op: "PUSH" },   // dst end
                    extInstr("MEMMOVE", []),
                    ...ld(0x20, "LD32"),
                    { op: "BLOCK_END" },
                    { op: "CONST", imm: 0 },
                    { op: "BLOCK_END" },
            ]),
        },
    ],
}

/** An extension site in a callee, and one in a BR_TABLE arm of the
 *  caller: the buffer is the only state that survives the call, so a
 *  clobbered window register shows up as a wrong answer rather than a
 *  crash. */
const extAcrossCall: RtlProgram = {
    procedures: [
        {
            argCount: 0,
            body: ret([
                { op: "CONST", imm: 0 }, { op: "PUSH" },       // §8.7's slot, reserved ahead of the dispatch
                { op: "CONST", imm: 0xc0 }, { op: "PUSH" },
                { op: "CONST", imm: 0x0f0f0f0f },
                extInstr("ST32", []),
                { op: "CONST", imm: 1 },
                { op: "BR_TABLE", imm: 1 },
                    { op: "CONST", imm: 0 }, { op: "STORE", target: 0 }, { op: "BLOCK_END" },
                    { op: "CONST", imm: 0xc0 }, { op: "CALL", calleeIndex: 1 }, { op: "STORE", target: 0 },
                    { op: "BLOCK_END" },
                { op: "LOAD", target: 0 },
            ]),
        },
        {
            argCount: 1,
            body: ret([
                { op: "LOAD", target: 0 },
                extInstr("LD32", []),
                { op: "PUSH" },
                { op: "LOAD", target: 0 }, { op: "ADD", combo: "IMM_ACC", imm: 2 },
                extInstr("LD8", []),
                { op: "ADD", combo: "POP_ACC" },
            ]),
        },
    ],
}

// ── regressions for what qemu_exec actually found ───────────────────────
//
// Each of these was a wrong answer (or a hang) from the emitted code, on a
// program the validator approved and the host-side fuzzer ran without a
// murmur. Kept as seeds so `qemu_exec.ts seeds` is a standing check on all
// three, and so the mutator starts from their shapes.

/** `BR_TABLE 2` reached with a dispatch value past its indexed cases, and
 *  no comparison to fuse: isa-core.md §4.5 sends it to the default case,
 *  where folding it into case[1] would run the last indexed arm instead.
 *
 *  The witness is a STORE to a slot rather than acc, so "which case ran" is
 *  observable however the merge's own liveness works out: k0 stays at its
 *  pre-dispatch 0 exactly when the empty default case ran. */
const brTable2Default: RtlProgram = {
    procedures: [
        {
            argCount: 0,
            body: [
                { op: "CONST", imm: 0 }, { op: "PUSH" },
                { op: "CONST", imm: 131118 },
                { op: "BR_TABLE", imm: 2 },
                    { op: "CONST", imm: 111 }, { op: "STORE", target: 0 }, { op: "BLOCK_END" },
                    { op: "CONST", imm: 222 }, { op: "STORE", target: 0 }, { op: "BLOCK_END" },
                    { op: "BLOCK_END" },
                { op: "LOAD", target: 0 },
                { op: "RETURN" },
            ],
        },
    ],
}

/** The same question one arm down: `BR_TABLE 1` with a dispatch value past
 *  its one indexed case, so the empty default case runs and k0 keeps its
 *  pre-dispatch value. */
const brTable1Default: RtlProgram = {
    procedures: [
        {
            argCount: 0,
            body: [
                { op: "CONST", imm: 0 }, { op: "PUSH" },
                { op: "CONST", imm: 7 },
                { op: "BR_TABLE", imm: 1 },
                    { op: "BLOCK_END" },
                    { op: "CONST", imm: 333 }, { op: "STORE", target: 0 }, { op: "BLOCK_END" },
                { op: "LOAD", target: 0 },
                { op: "RETURN" },
            ],
        },
    ],
}

/** A guarded branch forced onto its long form (a case body past the
 *  conditional-branch span) *while the literal pool has something
 *  pending*. Patching the long form's "not taken" edge to branch + 4 bytes
 *  lands exactly where the unconditional branch's pool flush goes, so that
 *  edge jumps into literal data and executes it. The wide immediate before
 *  the dispatch is what parks a pool entry;
 *  the long case body is what forces the long form. */
const longBranchOverPool: RtlProgram = {
    procedures: [
        {
            argCount: 1,
            body: [
                { op: "CONST", imm: 0x0019d156 }, // too wide for any MOVS/shift synthesis: parks a pool entry
                { op: "LT_U", combo: "IMM_ACC", imm: 0 },
                { op: "BR_TABLE", imm: 1 },
                    ...repeat<RtlInstr>(40, i => [
                        { op: "CONST", imm: 0x30000 + i },
                        { op: "STORE", target: 0 },
                    ]),
                    { op: "BLOCK_END" },
                    { op: "CONST", imm: 444 }, { op: "STORE", target: 0 }, { op: "BLOCK_END" },
                { op: "LOAD", target: 0 },
                { op: "RETURN" },
            ],
        },
    ],
}

/** The only seed reaching a *register-form* shift — `shift` above covers
 *  the immediate combo, which is a different code path entirely (one
 *  compile-time-masked LSRS #imm5, no dynamic amount anywhere).
 *
 *  Value and amount deliberately differ, so a shift emitted with its
 *  operands the wrong way round shows up as a wrong answer rather than a
 *  coincidence: 0x3C0 ASR 5 == 30.
 *
 *  The amount stays in range deliberately: §4.1 does not define a shift
 *  past 31, so the reference VM produces no result for one and there would
 *  be nothing to compare. */
const registerShiftDynamicAmount: RtlProgram = {
    procedures: [
        {
            argCount: 0,
            body: ret([
                { op: "CONST", imm: 5 },
                { op: "PUSH" },
                { op: "CONST", imm: 0x3C0 },
                { op: "ASR", combo: "POP_ACC" },
            ]),
        },
    ],
}

/** A one-argument callee whose body reads acc before writing it: §4.6 says
 *  the last argument arrives there. Seeding a callee's acc to 0 instead
 *  disagrees with the emitted code — the one finding so far where the JIT
 *  was right and @ppl/machine was wrong. */
const calleeReadsIncomingAcc: RtlProgram = {
    procedures: [
        { argCount: 0, body: ret([{ op: "CONST", imm: 3 }, { op: "CALL", calleeIndex: 1 }]) },
        { argCount: 1, body: ret([{ op: "MUL", combo: "REG_ACC", target: 0 }]) },
    ],
}

/** A PUSH inside a loop's *condition* sub-block. isa-core.md §8.1 has that
 *  block's BLOCK_END drop the surplus like any other, and the translator
 *  did it for every BR_TABLE case but not here — so sp and the window model
 *  disagreed from the loop onward and the return sequence reclaimed the
 *  wrong amount. Deep enough (four slots before the loop) that the extra
 *  push spills past the window and becomes a real stack push. */
const pushInLoopCondition: RtlProgram = {
    procedures: [
        {
            argCount: 1,
            body: [
                { op: "PUSH" }, { op: "PUSH" }, { op: "PUSH" },
                // Counter from a constant, not from the incoming argument:
                // the condition below reads slot 0 and nothing in the loop
                // decrements it, so keying it to the argument made this
                // non-terminating for every value except the 0 the harness
                // happened to pass. The shape under test — a PUSH inside the
                // condition sub-block, four slots deep so it spills — is
                // unchanged.
                { op: "CONST", imm: 3 }, { op: "STORE", target: 0 },
                { op: "LOOP_PRE" },
                    { op: "BLOCK_END" },
                    { op: "LOAD", target: 0 },
                    { op: "SUB", combo: "IMM_ACC", imm: 1 },
                    { op: "STORE", target: 0 },
                    { op: "LOAD", target: 0 },
                    { op: "PUSH" },
                    { op: "BLOCK_END" },
                { op: "CONST", imm: 11908 },
                { op: "RETURN" },
            ],
        },
    ],
}

/** Entry procedures taking more than one argument. Both harness halves used
 *  to discard these — qemu_exec.ts skipped anything declaring more than one,
 *  oracle_server.ts anything declaring any at all — so a shape that hung the
 *  emulator deterministically for every count of five or more went unseen
 *  through a whole campaign (docs/fuzzing-campaign.md). Three counts, chosen
 *  for what each one reaches:
 *
 *   2  in-window only, so purely the window registers enterDispatch
 *      initializes
 *   5  one out-of-window word — the smallest count whose frame the epilogue
 *      reclaims and someone must push
 *   6  two of them, the smallest count in which their *order* is observable
 *      at all
 *
 *  Bodies fold every argument in with a distinct shift so a permuted window
 *  changes the answer; a sum would not. */
const entryArgs = (argCount: number): RtlProgram => ({
    procedures: [{
        argCount,
        body: ret([
            { op: "LOAD", target: 0 },
            ...Array.from({ length: argCount - 1 }, (_, i) => [
                { op: "SHL", combo: "IMM_ACC", imm: 4 } as RtlInstr,
                { op: "OR", combo: "REG_ACC", target: i + 1 } as RtlInstr,
            ]).flat(),
        ]),
    }],
})

/** An entry procedure whose arguments spill, that then TRAPs. enterDispatch
 *  captures savedSp *before* pushing those out-of-window words, so
 *  trapHelper's single `mov sp, savedSp` has to subsume them — the shape
 *  test/qemu/test_entry_args.cpp's EntryWithSixArgumentsThatTraps pins down, absent from seeds/ until
 *  now. Distinguishes a trap from a return: the value is the trap code. */
const entryArgsSpilledTrap: RtlProgram = {
    procedures: [{
        argCount: 6,
        body: [
            { op: "LOAD", target: 0 },
            { op: "SHL", combo: "IMM_ACC", imm: 4 },
            { op: "OR", combo: "REG_ACC", target: 5 },
            { op: "TRAP", imm: 754 },
        ],
    }],
}

/** Spilled entry arguments *and* a nested frame on top of them: proc0's own
 *  out-of-window words sit below everything proc1's prologue pushes, so a
 *  return has to unwind past both without disturbing the entry spill. */
const entryArgsSpilledCall: RtlProgram = {
    procedures: [
        {
            argCount: 6,
            body: ret([
                { op: "LOAD", target: 0 },
                { op: "SHL", combo: "IMM_ACC", imm: 4 },
                { op: "OR", combo: "REG_ACC", target: 5 },
                { op: "CALL", calleeIndex: 1 },
            ]),
        },
        { argCount: 1, body: ret([{ op: "LOAD", target: 0 }, { op: "ADD", combo: "IMM_ACC", imm: 3 }]) },
    ],
}

/** isa-core.md §4.5's two-block dispatch: truthy, total, and carrying acc
 *  across its merge rather than killing it (§8.7). This is the shape
 *  lower.ts gives a whole-expression ternary. */
const brTableAccMerge: RtlProgram = {
    procedures: [
        {
            argCount: 1,
            body: [
                { op: "LOAD", target: 0 }, { op: "GT_U", combo: "IMM_ACC", imm: 3 },
                { op: "BR_TABLE", imm: 1 },
                    { op: "CONST", imm: 11 }, { op: "BLOCK_END" },
                    { op: "CONST", imm: 22 }, { op: "BLOCK_END" },
                // acc is live here because every arm reaching the merge left it so
                { op: "ADD", combo: "IMM_ACC", imm: 1 },
                { op: "RETURN" },
            ],
        },
    ],
}

/** An arm that runs on into the next one instead of leaving the construct —
 *  so the merge has a single incoming edge. */
const brTableFallthrough: RtlProgram = {
    procedures: [
        {
            argCount: 1,
            body: [
                { op: "LOAD", target: 0 }, { op: "EQ", combo: "IMM_ACC", imm: 0 },
                { op: "BR_TABLE", imm: 1 },
                    { op: "FALLTHROUGH" },
                    { op: "CONST", imm: 7 }, { op: "BLOCK_END" },
                { op: "RETURN" },
            ],
        },
    ],
}

/** C's `case 0: case 1: X` — a lone `FALLTHROUGH` sharing the next case's
 *  body, inside an ordinary lenient dispatch. */
const switchSharedBody: RtlProgram = {
    procedures: [
        {
            argCount: 1,
            body: [
                { op: "CONST", imm: 0 }, { op: "PUSH" },
                { op: "LOAD", target: 0 }, { op: "BR_TABLE", imm: 3 },
                    { op: "FALLTHROUGH" },
                    { op: "CONST", imm: 10 }, { op: "STORE", target: 1 }, { op: "BLOCK_END" },
                    { op: "CONST", imm: 20 }, { op: "STORE", target: 1 }, { op: "BLOCK_END" },
                    { op: "BLOCK_END" },
                { op: "LOAD", target: 1 }, { op: "RETURN" },
            ],
        },
    ],
}

/** LOOP_POST: the body ahead of the test, so it always runs once. The
 *  translator's only difference from LOOP_PRE is the missing entry branch,
 *  which is exactly what this puts on the wire. */
const postTestLoop: RtlProgram = {
    procedures: [
        {
            argCount: 1,
            body: [
                { op: "CONST", imm: 0 }, { op: "PUSH" },
                { op: "LOOP_POST" },
                    { op: "LOAD", target: 1 }, { op: "ADD", combo: "IMM_ACC", imm: 1 }, { op: "STORE", target: 1 },
                    { op: "LOAD", target: 0 }, { op: "SUB", combo: "IMM_ACC", imm: 1 }, { op: "STORE", target: 0 },
                    { op: "BLOCK_END" },
                    { op: "LOAD", target: 0 },
                    { op: "BLOCK_END" },
                { op: "LOAD", target: 1 },
                { op: "RETURN" },
            ],
        },
    ],
}

/** DROP in both its forms, across the physical-register window's edge —
 *  four slots wide (registers.h), so unwinding past it has to reload
 *  spills exactly as a BLOCK_END's own restore does. */
const dropSlots: RtlProgram = {
    procedures: [
        {
            argCount: 1,
            body: [
                ...repeat<RtlInstr>(7, i => [
                    { op: "CONST", imm: 0x100 + i },
                    { op: "PUSH" },
                ]),
                { op: "DROP", imm: 6 },   // the extended form (§5.4's bias)
                { op: "CONST", imm: 9 }, { op: "PUSH" },
                { op: "DROP", imm: 1 },   // ...and a small one
                { op: "LOAD", target: 1 },
                { op: "ADD", combo: "REG_ACC", target: 0 },
                { op: "RETURN" },
            ],
        },
    ],
}

/** DEFAULT out of a jump-table dispatch: gap fillers that are nothing but
 *  a DEFAULT, and a non-empty case that runs its own body and then the
 *  default clause too. The forward branch translateSwitch chains for these
 *  is patched only when case[N] finally starts. */
const defaultCases: RtlProgram = {
    procedures: [
        {
            argCount: 1,
            body: [
                { op: "CONST", imm: 0 }, { op: "PUSH" },
                { op: "LOAD", target: 0 },
                { op: "BR_TABLE", imm: 4 },
                    { op: "CONST", imm: 100 }, { op: "STORE", target: 1 }, { op: "BLOCK_END" },
                    { op: "DEFAULT" },
                    { op: "CONST", imm: 200 }, { op: "STORE", target: 1 }, { op: "DEFAULT" },
                    { op: "DEFAULT" },
                    { op: "LOAD", target: 1 }, { op: "ADD", combo: "IMM_ACC", imm: 7 }, { op: "STORE", target: 1 },
                    { op: "BLOCK_END" },
                { op: "LOAD", target: 1 },
                { op: "RETURN" },
            ],
        },
    ],
}

/** DEFAULT out of the two-block form, where the case it names is also the
 *  physically next one — the shape a one-label `switch` with a `default:`
 *  clause lowers to, and the one path translateIfThenElse takes for it. */
const defaultTwoBlock: RtlProgram = {
    procedures: [
        {
            argCount: 1,
            body: [
                { op: "CONST", imm: 0 }, { op: "PUSH" },
                { op: "LOAD", target: 0 },
                { op: "BR_TABLE", imm: 1 },
                    { op: "CONST", imm: 10 }, { op: "STORE", target: 1 }, { op: "DEFAULT" },
                    { op: "LOAD", target: 1 }, { op: "ADD", combo: "IMM_ACC", imm: 1 }, { op: "STORE", target: 1 },
                    { op: "BLOCK_END" },
                { op: "LOAD", target: 1 },
                { op: "RETURN" },
            ],
        },
    ],
}

const authored: [string, RtlProgram][] = [
    ["br_table_acc_merge", brTableAccMerge],
    ["br_table_fallthrough", brTableFallthrough],
    ["switch_shared_body", switchSharedBody],
    ["entry_args_spilled_trap", entryArgsSpilledTrap],
    ["entry_args_spilled_call", entryArgsSpilledCall],
    ["entry_args_two", entryArgs(2)],
    ["entry_args_spilled", entryArgs(5)],
    ["entry_args_spilled_pair", entryArgs(6)],
    ["push_in_loop_condition", pushInLoopCondition],
    ["br_table2_default", brTable2Default],
    ["br_table1_default", brTable1Default],
    ["long_branch_over_pool", longBranchOverPool],
    ["register_shift_dynamic_amount", registerShiftDynamicAmount],
    ["callee_reads_incoming_acc", calleeReadsIncomingAcc],
    ["const_return", constReturn],
    ["shift", shift],
    ["trap", trap],
    ["nested_trap", nestedTrap],
    ["deep_nested_trap", deepNestedTrap],
    ["arith", arith],
    ["signed_and_extend", signedAndExtend],
    ["ext_load_store", extLoadStore],
    ["ext_memmove", extMemmove],
    ["ext_memmove_empty", extMemmoveEmpty],
    ["ext_memcmp", extMemcmp],
    ["ext_slicecmp", extSliceCmp],
    ["ext_spilled_operands", extSpilledOperands],
    ["ext_in_loop", extInLoop],
    ["ext_across_call", extAcrossCall],
    ["ext_kill_dead_acc", extKillDeadAcc],
    ["long_branch_span", longBranchSpan],
    ["long_loop_back_edge", longLoopBackEdge],
    ["long_loop_entry_branch", longLoopEntryBranch],
    ["post_test_loop", postTestLoop],
    ["drop_slots", dropSlots],
    ["default_cases", defaultCases],
    ["default_two_block", defaultTwoBlock],
    ["literal_pool_pressure", literalPoolPressure],
    ["deep_spill", deepSpill],
    ["deep_nesting", deepNesting],
    ["large_switch", largeSwitch],
    ["wide_call_chain", wideCallChain],
    ["loop", countdownLoop],
    ["call_no_args", callNoArgs],
    ["call_one_arg", callOneArg],
    ["call_four_args", callFourArgs],
    ["call_spilled_args", callSpilledArgs],
    ["call_chain", callChain],
    ["call_in_branch", callInBranch],
    ["call_in_loop", callInLoop],
    ["br_table_live_merge", brTableLiveMerge],
    ["if_then_dead_merge", ifThenDeadMerge],
    ["fused_if_then_dead_merge", fusedIfThenDeadMerge],
]

// ── write them all ──────────────────────────────────────────────────────

let staged = 0
if(fs.existsSync(STAGING_DIR))
{
    for(const name of fs.readdirSync(STAGING_DIR).sort())
    {
        const bytes = fs.readFileSync(path.join(STAGING_DIR, name))
        const { value: argCount, next } = decodeLeb128(bytes, 0)
        write(name, { procedures: [{ argCount, body: decodeBody(bytes.subarray(next)) }] })
        staged++
    }
}
else
{
    console.error(`no ${STAGING_DIR} — run ./dump_seeds.sh first to stage corpus_programs.h's own shapes`)
}

for(const [name, program] of authored) write(name, program)
console.log(`wrote ${staged} staged + ${authored.length} authored = ${staged + authored.length} seeds`)
