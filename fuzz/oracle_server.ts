// jit-armv6m/fuzz — the "validator gate + reference-VM crosscheck" service
// a fuzz harness talks to over a Unix domain socket, so it never pays
// Node/V8 startup cost per test case (only once, here). Wraps @ppl/machine
// directly (packages/machine/src/{bytecode,validate,vm}.ts) rather than
// reimplementing any of the decode/validate/interpret logic — this process
// exists purely to keep that logic warm behind a fast IPC boundary.
//
// Wire format, one request/response per fuzz input:
//   request:  u32LE length, then that many raw bytes — LEB128(argCount)
//             followed by the raw body bytes of a single procedure
//             (bytecode.ts's decodeBody format; no procCount/header, since
//             this harness only ever fuzzes one procedure at a time —
//             a lone procedure can't legally contain CALL anyway, since
//             §8.2's call-graph-acyclicity check rejects self-recursion,
//             so restricting to one procedure costs no coverage of
//             anything CALL-shaped could reach that a single body can't).
//   response: fixed RESP_SIZE bytes, see the layout below.
//
// Run standalone: `npx ts-node --transpile-only jit-armv6m/fuzz/oracle_server.ts [socketPath]`

import * as net from "net"
import * as fs from "fs"
import { decodeLeb128, decodeBody, encodeLeb128, encodeBody, validateProgram, run } from "../../packages/machine/src/index"
import type { RtlProgram } from "../../packages/machine/src/index"

const SOCK_PATH = process.argv[2] || "/tmp/ppl-jit-oracle.sock"

// u8  valid          — 1 iff validateProgram accepted the decoded program
//                       AND it stays within withinRealisticProfile's own
//                       gate (docs/target-profile.md) — validator-approved
//                       but unrealistic programs (e.g. argCount in the
//                       hundreds) are deliberately treated the same as
//                       invalid ones here, so the fuzzer's search stays in
//                       the region where a crash is actually interesting
// u8  stage          — 0 ok, 1 malformed (LEB128/decodeBody threw), 2
//                       outside the realistic-profile gate, 3 validate
//                       threw, 4 the reference VM itself threw on a
//                       validator-approved program (a genuine @ppl/machine
//                       bug, not a fuzz-input problem)
// u8  refVmRan       — 1 iff `run()` was actually invoked (skipped when
//                       argCount != 0 — vm.ts's own run() only supports a
//                       zero-arg entry procedure today; see README note)
// u8  refVmOk        — VmResult.ok (meaningless if refVmRan == 0)
// i32 refVmAcc       LE
// i32 refVmTrapCode  LE, -1 when null
// u32 refVmSteps     LE
const RESP_SIZE = 1 + 1 + 1 + 1 + 4 + 4 + 4

// docs/target-profile.md's own realistic-profile table — kept in sync with
// that document by hand; add an entry there first, then here.
const REALISTIC_MAX_ARG_COUNT = 16

/** Whether a validator-approved program still looks like something a real
 *  program would produce, as opposed to a technically-legal corner the
 *  generic validator has no target-specific reason to reject (isa-core.md
 *  says nothing about jit-armv6m's own window size or ABI encoding
 *  widths). Extend this — and docs/target-profile.md's table — as fuzzing
 *  turns up more "valid but no real program would do this" gaps. */
function withinRealisticProfile(program: RtlProgram): boolean
{
    return program.procedures.every(p => p.argCount <= REALISTIC_MAX_ARG_COUNT)
}

function handleRequest(payload: Buffer): Buffer
{
    const resp = Buffer.alloc(RESP_SIZE)

    let program: RtlProgram
    try
    {
        const { value: argCount, next } = decodeLeb128(payload, 0)
        const bodyBytes = payload.subarray(next)
        const body = decodeBody(bodyBytes)

        // Neither decodeLeb128 (this file) nor compiler/src/decode_instr.cpp's
        // own copy caps how many continuation bytes a LEB128 field may carry
        // — a canonical u32 encoding never needs more than 5, but nothing
        // stops a non-canonical, redundant-continuation-byte encoding from
        // decoding "successfully" anyway. bytecode.ts's own encoder always
        // emits the minimal/canonical form, so re-encoding and comparing
        // lengths catches an overlong field anywhere in the stream (the
        // argCount prefix or any instruction immediate) without having to
        // walk LEB128 fields by hand here. The C++ decoder has no such
        // guard and hits real shift-amount UB decoding one of these
        // (docs/target-profile.md) — reject it the same way a genuinely
        // malformed encoding is rejected, since no real encoder ever
        // produces one.
        if(encodeLeb128(argCount).length !== next || encodeBody(body).length !== bodyBytes.length)
        {
            resp[1] = 1
            return resp
        }

        program = { procedures: [{ argCount, body }] }
    }
    catch
    {
        resp[1] = 1
        return resp
    }

    try
    {
        validateProgram(program)
    }
    catch
    {
        resp[1] = 3
        return resp
    }

    if(!withinRealisticProfile(program))
    {
        resp[1] = 2
        return resp
    }

    resp[0] = 1

    if(program.procedures[0]!.argCount !== 0)
    {
        return resp // refVmRan stays 0
    }

    try
    {
        const result = run(program)
        resp[2] = 1
        resp[3] = result.ok ? 1 : 0
        resp.writeInt32LE(result.acc | 0, 4)
        resp.writeInt32LE(result.trapCode ?? -1, 8)
        resp.writeUInt32LE(result.steps >>> 0, 12)
    }
    catch
    {
        resp[1] = 4
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
