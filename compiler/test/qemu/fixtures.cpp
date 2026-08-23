#include "fixtures.h"
#include "instr.h"
#include "encode_instr.h"

#include <cassert>

using namespace jitc;

Proc *realProcs = nullptr;
uint32_t realProcCount = 0;

namespace
{

// ---- Fixture 1: single-argument call, entirely acc-passed. expect 42.
const Instr f1Proc0[] = {CONST(37), call(1), bare(Op::RETURN)};
const Instr f1Proc1[] = {LOAD(0), opImm(Op::ADD, 5), bare(Op::RETURN)};
Proc f1Procs[2];

// ---- Fixture 2: 3-argument call with a phase-misaligned shuffle and
// surviving leftover locals. expect 1629.
const Instr f2Proc0[] = {
    CONST(100), PUSH(), // leftover local 0 -- k=0
    CONST(200), PUSH(), // leftover local 1 -- k=1
    CONST(300), PUSH(), // leftover local 2 -- k=2
    CONST(10), PUSH(),  // stack arg 0 for the callee -- k=3
    CONST(20), PUSH(),  // stack arg 1 for the callee -- k=4
    CONST(999),         // last (acc) arg -- never pushed
    call(1),
    opReg(Op::ADD, 0),
    opReg(Op::ADD, 1),
    opReg(Op::ADD, 2),
    bare(Op::RETURN),
};
const Instr f2Proc1[] = {LOAD(0), opReg(Op::ADD, 1), opReg(Op::ADD, 2), bare(Op::RETURN)};
Proc f2Procs[2];

// ---- Fixture 3: out-of-window LOAD/STORE/REG_ACC/REG_REG, no CALL. expect 55.
const Instr f3Proc0[] = {
    CONST(10), PUSH(), // k=0
    CONST(20), PUSH(), // k=1
    CONST(30), PUSH(), // k=2
    CONST(40), PUSH(), // k=3
    CONST(50), PUSH(), // k=4 -- evicts k=0's register
    CONST(60), PUSH(), // k=5 -- evicts k=1's register; k=0,1 now spilled
    LOAD(0),                    // acc = 10 (out-of-window LOAD)
    opReg(Op::ADD, 1),           // acc = 10+20=30 (out-of-window REG_ACC operand)
    STORE(0),                     // k0 := 30 (out-of-window STORE)
    CONST(5),
    opRegWriteback(Op::ADD, 1),    // k1 := 5+20=25 (out-of-window REG_REG)
    LOAD(0),                        // acc = k0 = 30
    opReg(Op::ADD, 1),                // acc = 30 + k1(25) = 55
    bare(Op::RETURN),
};
Proc f3Procs[1];

// ---- Fixture 4: CALL with stackArgs(6) > WINDOW_SIZE(4). expect 280.
const Instr f4Proc0[] = {
    CONST(10), PUSH(), // arg0
    CONST(20), PUSH(), // arg1
    CONST(30), PUSH(), // arg2
    CONST(40), PUSH(), // arg3
    CONST(50), PUSH(), // arg4
    CONST(60), PUSH(), // arg5
    CONST(70),          // arg6 -- last arg, via acc, never pushed
    call(1),
    bare(Op::RETURN),
};
const Instr f4Proc1[] = {
    LOAD(0),
    opReg(Op::ADD, 1), opReg(Op::ADD, 2), opReg(Op::ADD, 3),
    opReg(Op::ADD, 4), opReg(Op::ADD, 5), opReg(Op::ADD, 6),
    bare(Op::RETURN),
};
Proc f4Procs[2];

// ---- Fixture 5: operand-fold. expect 10.
const Instr f5Proc0[] = {
    CONST(10), PUSH(), // a -- k=0
    CONST(20), PUSH(), // b -- k=1
    CONST(30), PUSH(), // c -- k=2
    CONST(40), PUSH(), // d -- k=3
    LOAD(0),             // acc = a; accState depends on physReg(0)
    PUSH(),                // e = a -- k=4, evicts k=0's register
    bare(Op::RETURN),
};
Proc f5Procs[1];

// ---- Fixture 6: destination-fold. expect 99.
const Instr f6Proc0[] = {
    CONST(10), PUSH(),
    CONST(20), PUSH(),
    CONST(30), PUSH(),
    CONST(40), PUSH(),
    CONST(99), STORE(0), // a := 99; accState depends on physReg(0)
    PUSH(),                // e = a (now 99) -- k=4, evicts k=0's register
    bare(Op::RETURN),
};
Proc f6Procs[1];

// ---- Fixture 7: a 3-deep call chain. expect 106.
const Instr f7Proc0[] = {CONST(5), call(1), bare(Op::RETURN)};
const Instr f7Proc1[] = {LOAD(0), call(2), opImm(Op::ADD, 1), bare(Op::RETURN)};
const Instr f7Proc2[] = {LOAD(0), opImm(Op::ADD, 100), bare(Op::RETURN)};
Proc f7Procs[3];

// ---- Fixture 8: LOOP body closed by a bare terminator, not BLOCK_END.
// arg 0 -> 999 (cond-false exit tail), arg != 0 -> 42 (body runs once,
// returns directly).
const Instr f8Proc0[] = {
    bare(Op::LOOP), bare(Op::BLOCK_END),  // condition sub-block is empty — testAccNonzero(arg)
    CONST(42), bare(Op::RETURN),           // body — bare terminator closes it
    CONST(999), bare(Op::RETURN),          // reached only via the cond-false exit
};
Proc f8Procs[1];

// ---- Fixture 9: a genuine (non-degenerate) LOOP with real accumulation
// and a back-edge — sum(1..n). arg 4 -> 10, arg 0 -> 0, arg 1 -> 1.
const Instr f9Proc0[] = {
    LOAD(0), PUSH(),                             // k=1: counter := n
    CONST(0), PUSH(),                            // k=2: total := 0
    bare(Op::LOOP),
        LOAD(1),
    bare(Op::BLOCK_END),                          // testAccNonzero(counter)
        LOAD(2), opReg(Op::ADD, 1), STORE(2),      // total += counter
        LOAD(1), opImm(Op::SUB, 1), STORE(1),      // counter -= 1
    bare(Op::BLOCK_END),                          // back-edge
    LOAD(2), bare(Op::RETURN),
};
Proc f9Procs[1];

// ---- Fixture 10: BR_TABLE if/else fusion, non-last case closed via a
// bare RETURN, last case closes normally. arg <= 10 -> 111, arg > 10 -> 222.
const Instr f10Proc0[] = {
    LOAD(0), opImm(Op::GT_U, 10), brTable(2),
        CONST(111), bare(Op::RETURN),   // case 0 (n <= 10) — bare terminator
        CONST(222), bare(Op::BLOCK_END), // case 1 (n > 10) — normal close
    bare(Op::RETURN),
};
Proc f10Procs[1];

// ---- Fixture 11: BR_TABLE N>2, the shared jump-table helper.
// arg 0/1/2/3 -> 100/200/300/400.
const Instr f11Proc0[] = {
    LOAD(0), brTable(4),
        CONST(100), bare(Op::BLOCK_END),
        CONST(200), bare(Op::BLOCK_END),
        CONST(300), bare(Op::BLOCK_END),
        CONST(400), bare(Op::BLOCK_END),
    bare(Op::RETURN),
};
Proc f11Procs[1];

// ---- Fixture 12: a comparison feeds further arithmetic — (n > 4) * 5.
// arg 6 -> 5, arg 3 -> 0.
const Instr f12Proc0[] = {LOAD(0), opImm(Op::GT_U, 4), opImm(Op::MUL, 5), bare(Op::RETURN)};
Proc f12Procs[1];

// ---- Fixtures 13-16: unary ops, one procedure per op.
const Instr f13Neg[] = {LOAD(0), bare(Op::NEG), bare(Op::RETURN)};       // arg 5 -> 0xFFFFFFFB
Proc f13Procs[1];
const Instr f14Not[] = {LOAD(0), bare(Op::NOT), bare(Op::RETURN)};       // arg 5 -> 0xFFFFFFFA
Proc f14Procs[1];
const Instr f15Clz[] = {LOAD(0), bare(Op::CLZ), bare(Op::RETURN)};       // arg 1 -> 31, arg 0 -> 32
Proc f15Procs[1];
const Instr f16Revbits[] = {LOAD(0), bare(Op::REVBITS), bare(Op::RETURN)}; // arg 1 -> 0x80000000
Proc f16Procs[1];

// ---- Fixture 17: PEEK_PEEK two-op-in-place. 10 & 12 = 8.
const Instr f17Proc0[] = {
    CONST(12), PUSH(),                 // k=0 = 12
    CONST(10),                          // acc = 10 (pending)
    opStack(Op::AND, Combo::PEEK_PEEK), // k0 := 10 & 12 = 8; acc poisoned
    POP(), bare(Op::RETURN),
};
Proc f17Procs[1];

// ---- Fixture 18: branch-range guard forced into the long (invert-and-
// branch) form — case 0's own body is padded past the 240-byte safe span,
// so the dispatch guard itself can't be a bare short-form conditional
// branch. arg <= 100 -> 1 (via the long form), arg > 100 -> 2 (normal).
const Instr f18Proc0[] = {
    LOAD(0), opImm(Op::GT_U, 100), brTable(2),
        bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT),
        bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT),
        bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT),
        bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT),
        CONST(1), bare(Op::RETURN),      // case 0 (n <= 100) — 20 NOTs + terminator, 21*16 = 336 > 240
        CONST(2), bare(Op::BLOCK_END),   // case 1 (n > 100) — normal
    bare(Op::RETURN),
};
Proc f18Procs[1];

// ---- Fixture 19: regression for emitAddSubRsub with accShape and the
// IMM_ACC operand both compile-time immediates (CONST directly followed by
// an immediate arithmetic op), with the combined immediate too large for
// the imm3/imm8 fast paths — materializing accShape into SCRATCH_REG and
// then the second immediate into that same register clobbers the first
// value, silently computing `k op k` instead of `accShape op k`.
// expect 5 + 1000 = 1005.
const Instr f19Proc0[] = {CONST(5), opImm(Op::ADD, 1000), bare(Op::RETURN)};
Proc f19Procs[1];

// ---- Fixture 20: the same aliasing bug class, but the operand register
// itself happens to be SCRATCH_REG, which happens for real whenever an
// out-of-window local gets reloaded via ldrSp(SCRATCH_REG, ...). argCount=1
// starts with slot 0 (the argument) in the window (tos=1); four PUSHes
// bring tos to 5, exactly the point (inWindow's own `tos - k <= WINDOW_SIZE`)
// where slot 0 gets evicted onto the real stack, so `opReg(ADD, 0)` must
// reload it through SCRATCH_REG right after CONST(100) leaves acc pending.
// expect 100 + argIn.
const Instr f20Proc0[] = {
    CONST(2), PUSH(), // slot1=2, tos=2
    CONST(3), PUSH(), // slot2=3, tos=3
    CONST(4), PUSH(), // slot3=4, tos=4
    CONST(5), PUSH(), // slot4=5, tos=5 — slot 0 (argIn) now spilled
    CONST(100),
    opReg(Op::ADD, 0),
    bare(Op::RETURN),
};
Proc f20Procs[1];

// ---- Fixture 21: regression for armv6.h's isCondBranch, which excluded
// Condition::LE (0b1101) — this codebase's own largest valid condition —
// by checking `cond < 0b1101`. patchBranch treats any halfword
// isCondBranch rejects as unconditional and mis-patches it, crashing
// translateProc() outright — reachable via something as ordinary as
// `if (x <= 5)` (LE_S used directly as a BR_TABLE guard) or a
// `while (x > 0)` loop (GT_S's own inverse is LE). Two sub-cases in one
// procedure: an if/else guarded by LE_S, followed by a countdown loop
// whose exit condition is GT_S's inverse. expect (arg<=5 ? 100 : 200) + 0
// (the loop always counts down to 0).
const Instr f21Proc0[] = {
    LOAD(0), opImm(Op::LE_S, 5), brTable(2),      // if/else guarded directly by LE_S
        CONST(100), bare(Op::BLOCK_END),
        CONST(200), bare(Op::BLOCK_END),
    PUSH(),                                        // slot1 = branch result
    LOAD(0), PUSH(),                                // slot2 = counter := arg
    bare(Op::LOOP),
        LOAD(2), opImm(Op::GT_S, 0),                // while(counter > 0) -- exit condition is GT_S's own inverse
    bare(Op::BLOCK_END),
        LOAD(2), opImm(Op::SUB, 1), STORE(2),
    bare(Op::BLOCK_END),
    LOAD(1), opReg(Op::ADD, 2),                     // branch result + counter(now 0)
    bare(Op::RETURN),
};
Proc f21Procs[1];

// ---- Fixture 22: regression for the accState-merge-boundary bug —
// emitComparison never materializes its 0/1 result into any register
// (only CPU flags carry it into the fused branch), so a case body's own
// accState was silently left describing whatever it held before the
// comparison ran, instead of the correct, statically-known
// "comparison was false"/"comparison was true" constant. A bare STORE
// right at the top of each case exposes it: slot 1 starts at a stale
// sentinel (77, never 0/1), and each case's own probe should overwrite it
// with the comparison's real result instead. expect 0 for arg < 0x80
// (case 0), 1 for arg >= 0x80 (case 1).
const Instr f22Proc0[] = {
    CONST(77), PUSH(),                      // slot1 = 77 (stale sentinel)
    LOAD(0), opImm(Op::GE_U, 0x80), brTable(2),
        STORE(1), bare(Op::BLOCK_END),      // case 0 (false): probe
        STORE(1), bare(Op::BLOCK_END),      // case 1 (true): probe
    LOAD(1),
    bare(Op::RETURN),
};
Proc f22Procs[1];

// ---- Fixture 23: the same bug's LOOP-body half — the fused condition
// closing LOOP's own condition sub-block has the identical gap. x = 7 (not
// 0/1) is forced to 0 right inside the body instead of decremented
// normally, so the loop runs its body exactly once. expect 1.
const Instr f23Proc0[] = {
    CONST(7), PUSH(),                        // slot0 = 7 (stale sentinel)
    CONST(0), PUSH(),                        // slot1 = probe target
    bare(Op::LOOP),
        LOAD(0), opImm(Op::GT_S, 0), bare(Op::BLOCK_END),
        STORE(1),
        CONST(0), STORE(0),
    bare(Op::BLOCK_END),
    LOAD(1),
    bare(Op::RETURN),
};
Proc f23Procs[1];

// Every original fixture's own compiled output measures well under 110
// bytes — fixture 18 is a deliberate exception (it pads its own source
// body specifically to force the long-branch form). 64 bytes of source
// bytecode per procedure is generous headroom for any single procedure's
// own encoded wire body — a scratch region filled once at startup
// (encodeInto(), below), since Proc::body is raw wire bytes rather than
// something an Instr[] literal can become at compile time on its own.
constexpr uint32_t BYTES_PER_PROC = 64;
constexpr uint32_t TOTAL_PROCS = 28; // 2+2+1+2+1+1+3 (fixtures 1-7) + 16 (fixtures 8-23, one proc each)
uint8_t scratch[TOTAL_PROCS][BYTES_PER_PROC];

uint32_t nextScratchSlot = 0;

void encodeInto(uint32_t argCount, const Instr *body, uint32_t count, Proc &out)
{
    assert(nextScratchSlot < TOTAL_PROCS); // GCOV_EXCL_LINE — fixture-authoring bug, never a runtime condition
    uint8_t *slot = scratch[nextScratchSlot++];
    uint32_t len = encodeBody(body, count, slot, BYTES_PER_PROC);
    out = Proc{argCount, slot, len};
}

} // namespace

void initFixtures()
{
    encodeInto(0, f1Proc0, sizeof(f1Proc0) / sizeof(f1Proc0[0]), f1Procs[0]);
    encodeInto(1, f1Proc1, sizeof(f1Proc1) / sizeof(f1Proc1[0]), f1Procs[1]);

    encodeInto(0, f2Proc0, sizeof(f2Proc0) / sizeof(f2Proc0[0]), f2Procs[0]);
    encodeInto(3, f2Proc1, sizeof(f2Proc1) / sizeof(f2Proc1[0]), f2Procs[1]);

    encodeInto(0, f3Proc0, sizeof(f3Proc0) / sizeof(f3Proc0[0]), f3Procs[0]);

    encodeInto(0, f4Proc0, sizeof(f4Proc0) / sizeof(f4Proc0[0]), f4Procs[0]);
    encodeInto(7, f4Proc1, sizeof(f4Proc1) / sizeof(f4Proc1[0]), f4Procs[1]);

    encodeInto(0, f5Proc0, sizeof(f5Proc0) / sizeof(f5Proc0[0]), f5Procs[0]);

    encodeInto(0, f6Proc0, sizeof(f6Proc0) / sizeof(f6Proc0[0]), f6Procs[0]);

    encodeInto(0, f7Proc0, sizeof(f7Proc0) / sizeof(f7Proc0[0]), f7Procs[0]);
    encodeInto(1, f7Proc1, sizeof(f7Proc1) / sizeof(f7Proc1[0]), f7Procs[1]);
    encodeInto(1, f7Proc2, sizeof(f7Proc2) / sizeof(f7Proc2[0]), f7Procs[2]);

    encodeInto(1, f8Proc0, sizeof(f8Proc0) / sizeof(f8Proc0[0]), f8Procs[0]);
    encodeInto(1, f9Proc0, sizeof(f9Proc0) / sizeof(f9Proc0[0]), f9Procs[0]);
    encodeInto(1, f10Proc0, sizeof(f10Proc0) / sizeof(f10Proc0[0]), f10Procs[0]);
    encodeInto(1, f11Proc0, sizeof(f11Proc0) / sizeof(f11Proc0[0]), f11Procs[0]);
    encodeInto(1, f12Proc0, sizeof(f12Proc0) / sizeof(f12Proc0[0]), f12Procs[0]);
    encodeInto(1, f13Neg, sizeof(f13Neg) / sizeof(f13Neg[0]), f13Procs[0]);
    encodeInto(1, f14Not, sizeof(f14Not) / sizeof(f14Not[0]), f14Procs[0]);
    encodeInto(1, f15Clz, sizeof(f15Clz) / sizeof(f15Clz[0]), f15Procs[0]);
    encodeInto(1, f16Revbits, sizeof(f16Revbits) / sizeof(f16Revbits[0]), f16Procs[0]);
    encodeInto(0, f17Proc0, sizeof(f17Proc0) / sizeof(f17Proc0[0]), f17Procs[0]);
    encodeInto(1, f18Proc0, sizeof(f18Proc0) / sizeof(f18Proc0[0]), f18Procs[0]);
    encodeInto(0, f19Proc0, sizeof(f19Proc0) / sizeof(f19Proc0[0]), f19Procs[0]);
    encodeInto(1, f20Proc0, sizeof(f20Proc0) / sizeof(f20Proc0[0]), f20Procs[0]);
    encodeInto(1, f21Proc0, sizeof(f21Proc0) / sizeof(f21Proc0[0]), f21Procs[0]);
    encodeInto(1, f22Proc0, sizeof(f22Proc0) / sizeof(f22Proc0[0]), f22Procs[0]);
    encodeInto(0, f23Proc0, sizeof(f23Proc0) / sizeof(f23Proc0[0]), f23Procs[0]);
}

Fixture fixtures[] = {
    {"single-arg call", f1Procs, 2, false, 42},
    {"phase-misaligned shuffle", f2Procs, 2, false, 1629},
    {"out-of-window LOAD/STORE/REG_REG", f3Procs, 1, false, 55},
    {"stackArgs > WINDOW_SIZE", f4Procs, 2, false, 280},
    {"rotation operand-fold", f5Procs, 1, false, 10},
    {"rotation destination-fold", f6Procs, 1, false, 99},
    {"3-deep call chain", f7Procs, 3, false, 106},

    {"LOOP body closed by RETURN, cond-false exit", f8Procs, 1, false, 999, 0},
    {"LOOP body closed by RETURN, body runs once", f8Procs, 1, false, 42, 1},
    {"LOOP body closed by RETURN, body runs once (n=7)", f8Procs, 1, false, 42, 7},

    {"LOOP sum(1..n), n=4", f9Procs, 1, false, 10, 4},
    {"LOOP sum(1..n), n=0", f9Procs, 1, false, 0, 0},
    {"LOOP sum(1..n), n=1", f9Procs, 1, false, 1, 1},

    {"BR_TABLE if/else, non-last case via RETURN, n<=10", f10Procs, 1, false, 111, 0},
    {"BR_TABLE if/else, non-last case via RETURN, n=10", f10Procs, 1, false, 111, 10},
    {"BR_TABLE if/else, non-last case via RETURN, n>10", f10Procs, 1, false, 222, 11},

    {"BR_TABLE N>2 jump table, selector 0", f11Procs, 1, false, 100, 0},
    {"BR_TABLE N>2 jump table, selector 1", f11Procs, 1, false, 200, 1},
    {"BR_TABLE N>2 jump table, selector 2", f11Procs, 1, false, 300, 2},
    {"BR_TABLE N>2 jump table, selector 3", f11Procs, 1, false, 400, 3},

    {"comparison feeds arithmetic (n>4)*5, n=6", f12Procs, 1, false, 5, 6},
    {"comparison feeds arithmetic (n>4)*5, n=3", f12Procs, 1, false, 0, 3},

    {"NEG(5)", f13Procs, 1, false, 0xFFFFFFFBu, 5},
    {"NOT(5)", f14Procs, 1, false, 0xFFFFFFFAu, 5},
    {"CLZ(1)", f15Procs, 1, false, 31, 1},
    {"CLZ(0)", f15Procs, 1, false, 32, 0},
    {"REVBITS(1)", f16Procs, 1, false, 0x80000000u, 1},

    {"PEEK_PEEK two-op-in-place AND", f17Procs, 1, false, 8},

    {"branch-range forced long form, n<=100", f18Procs, 1, false, 1, 0},
    {"branch-range forced long form, n>100", f18Procs, 1, false, 2, 200},

    {"binops both-imm aliasing regression: CONST(5)+ADD#1000", f19Procs, 1, false, 1005},
    {"binops SCRATCH_REG-operand aliasing regression, argIn=1", f20Procs, 1, false, 101, 1},
    {"binops SCRATCH_REG-operand aliasing regression, argIn=42", f20Procs, 1, false, 142, 42},

    // isCondBranch's own Condition::LE off-by-one (see fixture 21's own
    // body comment) — BR_TABLE's own guard skips case 0 when the fused
    // condition is true, so case 1 (200) fires for arg<=5 and case 0 (100)
    // for arg>5.
    {"isCondBranch LE regression: arg<=5 (true branch) + countdown", f21Procs, 1, false, 200, 3},
    {"isCondBranch LE regression: arg==5 (boundary, true branch) + countdown", f21Procs, 1, false, 200, 5},
    {"isCondBranch LE regression: arg>5 (false branch) + countdown", f21Procs, 1, false, 100, 9},

    {"accState fusion-merge regression: case[0] (false) probe", f22Procs, 1, false, 0, 5},
    {"accState fusion-merge regression: case[1] (true) probe", f22Procs, 1, false, 1, 200},
    {"accState fusion-merge regression: LOOP body probe", f23Procs, 1, false, 1},
};
const uint32_t fixtureCount = sizeof(fixtures) / sizeof(fixtures[0]);
