// jit-armv6m/fuzz — the "validator gate + reference-VM crosscheck" service
// a fuzz harness talks to over a Unix domain socket, so it never pays
// Node/V8 startup cost per test case (only once, here). Wraps @ppl/machine
// directly (packages/machine/src/{bytecode,validate,vm}.ts) rather than
// reimplementing any of the decode/validate/interpret logic — this process
// exists purely to keep that logic warm behind a fast IPC boundary.
//
// Wire format, one request/response per fuzz input:
//   request:  u32LE length, then that many raw bytes — one whole
//             jit-armv6m program envelope, byte for byte what
//             bytecode.ts's `encodeJitProgram` emits and what
//             runtime/enter_program.cpp's own `parseProgramHeader` +
//             `Runtime::init()` consume:
//                 LEB128 max_call_depth
//                 LEB128 total_depth
//                 LEB128 proc_count
//                 proc_count × ( LEB128 arg_count, body bytes )
//             The same buffer reaches both sides unmodified, so the C++
//             harness parses exactly the bytes this validated.
//   response: fixed RESP_SIZE bytes, see the layout below.
//
// Whole programs, not a lone procedure: a single-procedure program cannot
// legally contain a CALL at all (§8.2's call-graph acyclicity rejects
// self-recursion), which would leave the entire call path —
// translate_proc.cpp's CALL case, abi_strategy's argument shuffle,
// Runtime::init()'s multi-procedure directory walk,
// parseProgramHeader itself — permanently unreachable by the fuzzer.
//
// Run standalone: `npx ts-node --transpile-only jit-armv6m/fuzz/oracle_server.ts [socketPath]`

import * as net from "net"
import * as fs from "fs"
import { decodeJitProgram, encodeLeb128, encodeProgram, validateProgram, run, StepLimitExceeded, UnspecifiedShiftAmount } from "../../packages/machine/src/index"
import type { RtlProgram } from "../../packages/machine/src/index"
// One generator for the entry procedure's arguments, shared with the
// execution oracle: if the two differed, this server's reference result
// would not be the one qemu_exec.ts compares against.
import { entryArgsFor } from "./entry_args"

const SOCK_PATH = process.argv[2] || "/tmp/ppl-jit-oracle.sock"

// u8  valid          — 1 iff validateProgram accepted the decoded program
//                       AND it stays within withinRealisticProfile's own
//                       gate (docs/target-profile.md) — validator-approved
//                       but unrealistic programs (e.g. argCount in the
//                       hundreds) are deliberately treated the same as
//                       invalid ones here, so the fuzzer's search stays in
//                       the region where a crash is actually interesting
// u8  stage          — 0 ok, 1 malformed (LEB128/decode threw, or a
//                       non-canonical encoding), 2 outside the
//                       realistic-profile gate, 3 validate threw, 4 the
//                       reference VM itself threw on a validator-approved
//                       program (a genuine @ppl/machine bug, not a fuzz-
//                       input problem), 6 the reference VM hit its own
//                       step-limit watchdog — a legal non-terminating
//                       program (§9), not a finding, 7 the program did a
//                       shift by 32 or more, whose result isa-core.md
//                       §4.1 leaves unspecified — also legal, also not a
//                       finding, and specifically not something to
//                       compare a translator result against
// u8  refVmRan       — 1 iff `run()` was actually invoked (it is skipped
//                       only when an earlier stage already failed; every
//                       entry procedure is runnable, arguments included)
// u8  refVmOk        — VmResult.ok (meaningless if refVmRan == 0)
// i32 refVmAcc       LE
// i32 refVmTrapCode  LE, -1 when null
// u32 refVmSteps     LE
const RESP_SIZE = 1 + 1 + 1 + 1 + 4 + 4 + 4

// docs/target-profile.md's own realistic-profile table — kept in sync with
// that document by hand; add an entry there first, then here.
const REALISTIC_MAX_ARG_COUNT = 16
const REALISTIC_MAX_PROC_COUNT = 16

// The one gate here that is a *hard* target ceiling rather than a taste
// judgement. `Window::discardWindow` reclaims a procedure's whole spilled
// frame in one `ADD sp, sp, #imm` (Thumb T2: a 7-bit word immediate, so 508
// bytes / 127 words), which caps compilable TOS depth at
// WINDOW_SIZE + 127 = 131 — for the operand stack in general, not just for
// argCount, which is how docs/target-profile.md originally framed it. Past
// that the translator can only bail with RESOURCE_ERROR.
//
// Worth gating rather than leaving to the bail: unbounded, the mutator put
// 84% of its corpus above this line, so 84% of the fuzzer's budget went to
// programs whose emitted code neither half of fuzz/ could ever look at.
// Measured with fuzz/qemu_exec, which is what made the ratio visible.
const REALISTIC_MAX_TOTAL_DEPTH = 128

/** Whether a validator-approved program still looks like something a real
 *  program would produce, as opposed to a technically-legal corner the
 *  generic validator has no target-specific reason to reject (isa-core.md
 *  says nothing about jit-armv6m's own window size or ABI encoding
 *  widths). Extend this — and docs/target-profile.md's table — as fuzzing
 *  turns up more "valid but no real program would do this" gaps. */
function withinRealisticProfile(program: RtlProgram): boolean
{
    return program.procedures.length <= REALISTIC_MAX_PROC_COUNT
        && program.procedures.every(p => p.argCount <= REALISTIC_MAX_ARG_COUNT)
}

/** The whole-program TOS bound validateProgram computes, against the hard
 *  ceiling above. Separate from withinRealisticProfile because it needs the
 *  validator's own result rather than just the program. */
function withinDepthCeiling(stats: { totalDepth: number }): boolean
{
    return stats.totalDepth <= REALISTIC_MAX_TOTAL_DEPTH
}

function handleRequest(payload: Buffer): Buffer
{
    const resp = Buffer.alloc(RESP_SIZE)

    let program: RtlProgram
    let headerMaxCallDepth: number
    let headerTotalDepth: number
    try
    {
        const decoded = decodeJitProgram(payload)
        program = decoded.program
        headerMaxCallDepth = decoded.maxCallDepth
        headerTotalDepth = decoded.totalDepth

        // Trailing bytes past the last procedure's own terminator: the C++
        // side derives each body's end the same self-delimiting way, so it
        // would simply stop early and never look at them — no real encoder
        // emits any.
        if(decoded.next !== payload.length)
        {
            resp[1] = 1
            return resp
        }

        // Neither decodeLeb128 (bytecode.ts) nor compiler/src/
        // decode_instr.cpp's own copy caps how many continuation bytes a
        // LEB128 field may carry — a canonical u32 encoding never needs
        // more than 5, but nothing stops a non-canonical,
        // redundant-continuation-byte encoding from decoding
        // "successfully" anyway. bytecode.ts's own encoders always emit the
        // minimal/canonical form, so re-encoding and comparing lengths
        // catches an overlong field anywhere in the stream (either header
        // stat, proc_count, any arg_count, any instruction immediate)
        // without having to walk LEB128 fields by hand here. The C++
        // decoder has no such guard and hits real shift-amount UB decoding
        // one of these (docs/target-profile.md) — reject it the same way a
        // plain malformed encoding is rejected, since no real encoder ever
        // produces one.
        const canonicalLength = encodeLeb128(headerMaxCallDepth).length
            + encodeLeb128(headerTotalDepth).length
            + encodeProgram(program).length
        if(canonicalLength !== payload.length)
        {
            resp[1] = 1
            return resp
        }
    }
    catch
    {
        resp[1] = 1
        return resp
    }

    let stats
    try
    {
        stats = validateProgram(program)
    }
    catch
    {
        resp[1] = 3
        return resp
    }

    // Deliberately NOT gated on the envelope's two stats agreeing with what
    // validateProgram just recomputed, even though encodeJitProgram only
    // ever writes exactly those. The harness ignores both fields entirely
    // — they exist for enterProgram*'s own stack reservation, which a host
    // build never performs — so a forged header cannot produce a finding
    // here, while rejecting one costs most of the search: those two
    // leading LEB128s are a large fraction of a small program's bytes, and
    // mutating either was making about two thirds of otherwise-valid
    // mutants unusable (measured: 2.5% approval with the gate, 8%+
    // without). Restore the check the moment this harness grows to cover
    // enterProgram's own budgeting, where an understated depth is a real
    // "the runtime trusts its input" question.
    void headerMaxCallDepth
    void headerTotalDepth

    if(!withinRealisticProfile(program) || !withinDepthCeiling(stats))
    {
        resp[1] = 2
        return resp
    }

    resp[0] = 1

    // Every entry procedure has a reference result: enterProgram* takes the
    // whole argument vector.

    try
    {
        const result = run(program, undefined, entryArgsFor(program.procedures[0]!.argCount))
        resp[2] = 1
        resp[3] = result.ok ? 1 : 0
        resp.writeInt32LE(result.acc | 0, 4)
        // `| 0` matters: a trap code is a u32 (TRAP's own immediate is an
        // unsigned LEB128), and writeInt32LE *throws* on anything above
        // 2**31-1 — which this file's own catch below then relabelled as
        // stage 4, "the reference VM threw", making every large trap code
        // look like an @ppl/machine bug. The coercion keeps the same four
        // bytes; the harness reads the field back as int32 and can widen
        // it if it ever needs the value rather than just its identity.
        // The -1 null sentinel does alias trap code 0xFFFFFFFF, which is
        // harmless while refVmOk distinguishes the two (a trap always
        // reports refVmOk = 0).
        resp.writeInt32LE((result.trapCode ?? -1) | 0, 8)
        resp.writeUInt32LE(result.steps >>> 0, 12)
    }
    catch(e)
    {
        // The step-limit watchdog is not a finding: isa-core.md §9 promises
        // bounded resource usage, not termination, so a LOOP whose
        // condition never tests false is a legal program the reference VM
        // simply cannot finish. Its own stage, and not "valid", since there
        // is no reference result for a future execution oracle to compare
        // against — but emphatically not stage 4 either, which the harness
        // treats as a genuine validator/VM inconsistency and aborts on.
        //
        // A shift by 32 or more is the same kind of event one layer over:
        // isa-core.md §4.1 leaves its result unspecified, so the VM throws
        // rather than pass off its own five-bit masking as the answer.
        // Legal program, no reference result, and emphatically not stage 4
        // — the harness aborts on that one, and this would fire on every
        // input the mutator happened to give a large dynamic shift amount.
        // The immediate combo never gets here: validate.ts rejects an
        // out-of-range immediate amount, which is stage 3.
        resp[0] = 0
        resp[1] = e instanceof StepLimitExceeded ? 6
                : e instanceof UnspecifiedShiftAmount ? 7
                : 4
    }
    return resp
}

try { fs.unlinkSync(SOCK_PATH) } catch {}

const server = net.createServer((socket) =>
{
    let buf = Buffer.alloc(0)
    socket.on("data", (chunk: Buffer) =>
    {
        buf = Buffer.concat([buf, chunk])
        for(;;)
        {
            if(buf.length < 4) break
            const len = buf.readUInt32LE(0)
            if(buf.length < 4 + len) break
            const payload = buf.subarray(4, 4 + len)
            buf = buf.subarray(4 + len)
            socket.write(handleRequest(payload))
        }
    })
    socket.on("error", () => {}) // a fuzzer that crashed mid-request just drops the connection
})

server.listen(SOCK_PATH, () =>
{
    console.error(`oracle listening on ${SOCK_PATH}`)
})
