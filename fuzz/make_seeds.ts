// jit-armv6m/fuzz — writes the whole seeds/ corpus in the whole-program
// envelope format harness.cpp/oracle_server.ts speak (bytecode.ts's
// encodeJitProgram).
//
// Every seed goes through validateProgram here, so one that doesn't
// validate fails this script instead of silently becoming a seed the
// harness discards on every execution.
//
// Three groups: the small single-procedure shapes (also what
// test/qemu/fixtures.cpp exercises on real hardware), the
// multi-procedure/CALL shapes no single-procedure format could express at
// all (isa-core.md §8.2 rejects self-recursion, so a lone procedure can
// never legally CALL anything), and the large shapes aimed at specific
// compiled-size guards.
//
// Run: npx ts-node --transpile-only jit-armv6m/fuzz/make_seeds.ts
// (from the repo root, with TS_NODE_PROJECT=jit-armv6m/fuzz/tsconfig.json)

import * as fs from "fs"
import * as path from "path"
import { decodeLeb128, decodeBody, encodeJitProgram, validateProgram } from "../../packages/machine/src/index"
import type { RtlInstr, RtlProc, RtlProgram } from "../../packages/machine/src/index"

const SEED_DIR = path.join(__dirname, "seeds")

/** dump_seeds.cpp's staging output: ../test/corpus_programs.h's bodies,
 *  each as one arg_count LEB128 followed by that procedure's own body
 *  bytes. Wrapped into whole-program envelopes below rather than
 *  re-authored here, so the shapes test/qemu/fixtures.cpp exercises on real
 *  hardware stay a single definition. A dedicated directory, not a guess
 *  about which files in seeds/ happen to be in the older format — a
 *  new-format seed's header bytes decode as a plausible arg_count and body
 *  too, so that guess could not be made reliably. */
const STAGING_DIR = path.join(__dirname, "seeds_raw")

function write(name: string, program: RtlProgram): void
{
    const stats = validateProgram(program) // throws rather than emit a seed the harness would discard
    const bytes = encodeJitProgram(program)
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
                { op: "BR_TABLE", imm: 2 },
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

/** A call inside a LOOP body, so the callee's own spill/reload interacts
 *  with a back-edge's window restore. */
const callInLoop: RtlProgram = {
    procedures: [
        {
            argCount: 1,
            body: [
                { op: "CONST", imm: 0 }, { op: "PUSH" },
                { op: "LOOP" },
                    { op: "LOAD", target: 0 },
                    { op: "BLOCK_END" },
                    { op: "LOAD", target: 0 }, { op: "CALL", calleeIndex: 1 },
                    { op: "ADD", combo: "REG_ACC", target: 1 }, { op: "STORE", target: 1 },
                    { op: "LOAD", target: 0 }, { op: "SUB", combo: "IMM_ACC", imm: 1 }, { op: "STORE", target: 0 },
                    { op: "BLOCK_END" },
                { op: "LOAD", target: 1 },
                { op: "RETURN" },
            ],
        },
        { argCount: 1, body: ret([{ op: "XOR", combo: "IMM_ACC", imm: 0x5a }]) },
    ],
}

/** A BR_TABLE whose every case leaves acc live, followed by code that
 *  needs its own producer because isa-core.md §8.7 drops acc at the merge
 *  regardless (§4.5's implicit default edge holds no instructions, so no
 *  value can be established on it). Worth a seed of its own: this is the
 *  region two crashes already came from, and a blind mutator rarely builds
 *  a live-in-every-case dispatch on its own. */
const brTableDeadMerge: RtlProgram = {
    procedures: [
        {
            argCount: 1,
            body: [
                { op: "LOAD", target: 0 },
                { op: "BR_TABLE", imm: 3 },
                    { op: "CONST", imm: 10 }, { op: "BLOCK_END" },
                    { op: "CONST", imm: 20 }, { op: "BLOCK_END" },
                    { op: "CONST", imm: 30 }, { op: "BLOCK_END" },
                { op: "CONST", imm: 40 },
                { op: "RETURN" },
            ],
        },
    ],
}

/** The same merge shape one level down: a BR_TABLE 1 (if-without-else)
 *  whose body leaves acc live while the skip edge cannot, so the merge is
 *  dead and the code after it carries its own producer. */
const ifThenDeadMerge: RtlProgram = {
    procedures: [
        {
            argCount: 1,
            body: [
                { op: "LOAD", target: 0 },
                { op: "BR_TABLE", imm: 1 },
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
                { op: "LOOP" },
                    { op: "LOAD", target: 0 },
                    { op: "BLOCK_END" },
                    { op: "LOAD", target: 1 }, { op: "ADD", combo: "REG_ACC", target: 0 }, { op: "STORE", target: 1 },
                    { op: "LOAD", target: 0 }, { op: "SUB", combo: "IMM_ACC", imm: 1 }, { op: "STORE", target: 0 },
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

/** A LOOP whose body compiles past Ioff<1,11>'s own reach, so the back-edge
 *  branch cannot encode and translateLoop must bail rather than emit an
 *  out-of-range offset. */
const longLoopBackEdge: RtlProgram = {
    procedures: [
        {
            argCount: 1,
            body: [
                { op: "LOOP" },
                    { op: "LOAD", target: 0 },
                    { op: "BLOCK_END" },
                    ...repeat<RtlInstr>(400, i => [
                        { op: "CONST", imm: 0x20000 + i },
                        { op: "STORE", target: 0 },
                    ]),
                    // Clear the condition slot so the loop runs exactly one
                    // iteration whatever the entry argument is; otherwise the
                    // body leaves slot 0 non-zero and it never exits. The
                    // 400-instruction body this seed exists for is untouched.
                    { op: "CONST", imm: 0 },
                    { op: "STORE", target: 0 },
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

/** 80 nested LOOPs. The translator's real recursion is
 *  processUntilTerminator -> processNonTerminators -> back into a construct
 *  handler, and checkStackFloor is the only thing bounding it — this is the
 *  shape that actually drives it deep. */
const deepNesting: RtlProgram = (() =>
{
    const depth = 80
    const body: RtlInstr[] = []
    for(let i = 0; i < depth; i++)
    {
        body.push({ op: "LOOP" })
        body.push({ op: "CONST", imm: 0 })   // condition block: exit immediately
        body.push({ op: "BLOCK_END" })
    }
    body.push({ op: "CONST", imm: 1 })
    for(let i = 0; i < depth; i++) body.push({ op: "BLOCK_END" }) // close each body block
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
    for(let i = 0; i < n; i++)
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

/** PEEK_PEEK plus POP: the two combos that read the operand stack in
 *  place, which none of the register-combo shapes cover. */
const arith: RtlProgram = {
    procedures: [
        {
            argCount: 0,
            body: ret([
                { op: "CONST", imm: 5 }, { op: "PUSH" },
                { op: "CONST", imm: 3 }, { op: "ADD", combo: "PEEK_PEEK" },
                { op: "POP" },
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

/** `BR_TABLE 2` reached with a dispatch value of neither 0 nor 1, and no
 *  comparison to fuse: isa-core.md §4.5's implicit default. Folding it into
 *  case[1] runs the else-arm where the ISA runs neither arm.
 *
 *  The witness has to be a STORE to a slot rather than acc: §8.7 drops acc
 *  at the merge, so "which arm ran" is only observable through state a case
 *  writes. k0 stays at its pre-dispatch 0 exactly when neither arm ran. */
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
                { op: "LOAD", target: 0 },
                { op: "RETURN" },
            ],
        },
    ],
}

/** The same question one arm down: `BR_TABLE 1` with a dispatch value past
 *  its case count. Correct all along (`acc != 0` and `acc >= 1` coincide at
 *  N == 1), and worth pinning precisely because it does. */
const brTable1Default: RtlProgram = {
    procedures: [
        {
            argCount: 0,
            body: [
                { op: "CONST", imm: 0 }, { op: "PUSH" },
                { op: "CONST", imm: 7 },
                { op: "BR_TABLE", imm: 1 },
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
                { op: "BR_TABLE", imm: 2 },
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

/** A PUSH inside a LOOP's *condition* sub-block. isa-core.md §8.1 has that
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
                { op: "LOOP" },
                    { op: "LOAD", target: 0 },
                    { op: "SUB", combo: "IMM_ACC", imm: 1 },
                    { op: "STORE", target: 0 },
                    { op: "LOAD", target: 0 },
                    { op: "PUSH" },
                    { op: "BLOCK_END" },
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
 *  test/qemu/fixtures.cpp fixture 47 pins down, absent from seeds/ until
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

const authored: [string, RtlProgram][] = [
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
    ["long_branch_span", longBranchSpan],
    ["long_loop_back_edge", longLoopBackEdge],
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
    ["br_table_dead_merge", brTableDeadMerge],
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
