#include "fixtures.h"
#include "instr.h"
#include "encode_instr.h"

using namespace jitc;

namespace
{

// ---- Fixture 1: single-argument call, entirely acc-passed. expect 42.
const Instr f1Proc0[] = {CONST(37), call(1), bare(Op::RETURN)};
const Instr f1Proc1[] = {LOAD(0), opImm(Op::ADD, 5), bare(Op::RETURN)};
Program f1Prog;

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
Program f2Prog;

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
Program f3Prog;

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
Program f4Prog;

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
Program f5Prog;

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
Program f6Prog;

// ---- Fixture 7: a 3-deep call chain. expect 106.
const Instr f7Proc0[] = {CONST(5), call(1), bare(Op::RETURN)};
const Instr f7Proc1[] = {LOAD(0), call(2), opImm(Op::ADD, 1), bare(Op::RETURN)};
const Instr f7Proc2[] = {LOAD(0), opImm(Op::ADD, 100), bare(Op::RETURN)};
Program f7Prog;

// ---- Fixture 8: LOOP body closed by a bare terminator, not BLOCK_END.
// arg 0 -> 999 (cond-false exit tail), arg != 0 -> 42 (body runs once,
// returns directly).
const Instr f8Proc0[] = {
    bare(Op::LOOP), bare(Op::BLOCK_END),  // condition sub-block is empty — testAccNonzero(arg)
    CONST(42), bare(Op::RETURN),           // body — bare terminator closes it
    CONST(999), bare(Op::RETURN),          // reached only via the cond-false exit
};
Program f8Prog;

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
Program f9Prog;

// ---- Fixture 10: BR_TABLE if/else fusion, non-last case closed via a
// bare RETURN, last case closes normally. arg <= 10 -> 111, arg > 10 -> 222.
const Instr f10Proc0[] = {
    LOAD(0), opImm(Op::GT_U, 10), brTable(2),
        CONST(111), bare(Op::RETURN),   // case 0 (n <= 10) — bare terminator
        CONST(222), bare(Op::BLOCK_END), // case 1 (n > 10) — normal close
    bare(Op::RETURN),
};
Program f10Prog;

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
Program f11Prog;

// ---- Fixture 12: a comparison feeds further arithmetic — (n > 4) * 5.
// arg 6 -> 5, arg 3 -> 0.
const Instr f12Proc0[] = {LOAD(0), opImm(Op::GT_U, 4), opImm(Op::MUL, 5), bare(Op::RETURN)};
Program f12Prog;

// ---- Fixtures 13-16: unary ops, one procedure per op.
const Instr f13Neg[] = {LOAD(0), bare(Op::NEG), bare(Op::RETURN)};       // arg 5 -> 0xFFFFFFFB
Program f13Prog;
const Instr f14Not[] = {LOAD(0), bare(Op::NOT), bare(Op::RETURN)};       // arg 5 -> 0xFFFFFFFA
Program f14Prog;
const Instr f15Clz[] = {LOAD(0), bare(Op::CLZ), bare(Op::RETURN)};       // arg 1 -> 31, arg 0 -> 32
Program f15Prog;
const Instr f16Revbits[] = {LOAD(0), bare(Op::REVBITS), bare(Op::RETURN)}; // arg 1 -> 0x80000000
Program f16Prog;

// ---- Fixture 17: PEEK_PEEK two-op-in-place. 10 & 12 = 8.
const Instr f17Proc0[] = {
    CONST(12), PUSH(),                 // k=0 = 12
    CONST(10),                          // acc = 10 (pending)
    opStack(Op::AND, Combo::PEEK_PEEK), // k0 := 10 & 12 = 8; acc poisoned
    POP(), bare(Op::RETURN),
};
Program f17Prog;

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
Program f18Prog;

// ---- Fixture 19: regression for emitAddSubRsub with accShape and the
// IMM_ACC operand both compile-time immediates (CONST directly followed by
// an immediate arithmetic op), with the combined immediate too large for
// the imm3/imm8 fast paths — materializing accShape into SCRATCH_REG and
// then the second immediate into that same register clobbers the first
// value, silently computing `k op k` instead of `accShape op k`.
// expect 5 + 1000 = 1005.
const Instr f19Proc0[] = {CONST(5), opImm(Op::ADD, 1000), bare(Op::RETURN)};
Program f19Prog;

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
Program f20Prog;

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
Program f21Prog;

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
Program f22Prog;

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
Program f23Prog;

// ---- Fixture 24: literal pooling, both routes at once. CONST's own
// hard-to-synthesize value pools, and so does the ADD's immediate operand
// (which reaches the pool through Combo::IMM_ACC rather than CONST), so
// this executes two PC-relative loads at different alignment parities.
// expect 0x12345678 + 0x11111111.
const Instr f24Proc0[] = {CONST(0x12345678), opImm(Op::ADD, 0x11111111), bare(Op::RETURN)};
Program f24Prog;

// ---- Fixture 25: a pooled load whose pool is flushed mid-procedure,
// with the flush's branch-around actually executed. BR_TABLE N>2 forces
// the flush (its jump table's raw halfwords must not land in a later
// scan window), so the pool lands in the middle of the code and control
// has to jump over it to reach the dispatch. The pooled value is read
// back afterwards from a local, proving both the load and the jump-around
// worked. expect 0xDEADBEEF.
const Instr f25Proc0[] = {
    CONST(0xDEADBEEF), PUSH(), // pooled — the chunk is open across the BR_TABLE
    CONST(1), brTable(3),       // forces the flush, mid-code, before the table
        CONST(100), bare(Op::BLOCK_END),
        CONST(200), bare(Op::BLOCK_END),
        CONST(300), bare(Op::BLOCK_END),
    LOAD(0),
    bare(Op::RETURN),
};
Program f25Prog;

// ---- Fixture 26: a pooled load in a procedure that does *not* start at
// the arena base. proc0 compiles to an odd number of halfwords (38 bytes,
// i.e. 2 mod 4), so without Runtime::allocate rounding its reservation up
// proc1 would land 2 bytes off a word boundary — and every
// Align(pc,4)-based literal offset in it, resolved procedure-relative at
// translation time, would then read 2 bytes away from its own pool word.
// This is the fixture that actually fails if that rounding is dropped.
// 5 ^ 0x0F0F0F0F = 0x0F0F0F0A, + 1 = 0x0F0F0F0B.
const Instr f26Proc0[] = {CONST(5), call(1), opImm(Op::ADD, 1), bare(Op::RETURN)};
const Instr f26Proc1[] = {LOAD(0), opImm(Op::XOR, 0x0F0F0F0F), bare(Op::RETURN)};
Program f26Prog;

// ---- Fixture 27: a non-leaf procedure (proc1, argCount=5) with an
// out-of-window argument (k=0, argCount > WINDOW_SIZE(4)) sitting below its
// own pushed call/return record — the abiEmitReturn/returnHelperFromStack-
// Reclaim path (savesLR && initialSpilledCount > 0). k=0 is read after
// proc1's own nested call returns, exercising spillOffset's savesLR shift
// for a live read, not just the reclaim at RETURN. expect 1501:
// proc2(1) = 1001, + arg4(500) = 1501.
const Instr f27Proc0[] = {
    CONST(1), PUSH(),  // arg0 for proc1 -- k=0, proc1's out-of-window arg
    CONST(2), PUSH(),  // arg1 -- k=1
    CONST(3), PUSH(),  // arg2 -- k=2
    CONST(4), PUSH(),  // arg3 -- k=3
    CONST(500),         // arg4 -- last, via acc
    call(1),
    bare(Op::RETURN),
};
const Instr f27Proc1[] = {
    LOAD(0),              // acc = arg0 (k=0, out-of-window)
    call(2),               // proc2(arg0) -- makes proc1 non-leaf (savesLR)
    opReg(Op::ADD, 4),       // acc += arg4 (k=4, still in-window)
    bare(Op::RETURN),
};
const Instr f27Proc2[] = {LOAD(0), opImm(Op::ADD, 1000), bare(Op::RETURN)};
Program f27Prog;

// Every original fixture's own compiled output measures well under 110
// bytes — fixture 18 is a deliberate exception (it pads its own source
// body specifically to force the long-branch form). This is a bump
// allocator, not a fixed per-program slot size, since a whole program's
// own encoded size varies far more than a single procedure's body did
// (fixture 18's ~350 bytes against most others' well under 30): sizing
// every slot for the worst case would waste RAM this target's own 8KB
// budget (test/qemu/linker.ld) can't spare. 3KB is generous over the real
// total (26 small envelopes + every fixture body combined measures well
// under 2.5KB) — an overrun here is a fixture-authoring bug, never a
// runtime condition, exactly like encodeInstr's own assert-and-move-on
// (putByte, compiler/src/encode_instr.cpp), which is what would actually
// catch it on a host build; the QEMU build's own -DNDEBUG strips that, so
// keep this margin real rather than tight.
constexpr uint32_t SCRATCH_CAPACITY = 3072;
uint8_t scratch[SCRATCH_CAPACITY];
uint32_t scratchUsed = 0;

// max_call_depth/total_depth are both 0 for every fixture here: none of
// them ever reach enterProgram through a path that actually checks them
// (that's what enterProgramOnStack/enterProgramSplit's own dedicated
// scenarios in main.cpp are for, with their own hand-derived, real
// values) — plain enterProgram never consults its envelope's stats at
// all, only parses past them to find proc_count.
Program finishProgram(const ProcSource *procs, uint32_t count)
{
    uint8_t *slot = scratch + scratchUsed;
    uint32_t len = encodeJitProgram(0, 0, procs, count, slot, SCRATCH_CAPACITY - scratchUsed);
    scratchUsed += len;
    return Program{slot, len};
}

// Instr[]'s own element count, paired with its own argCount — one
// ProcSource per procedure, exactly what fixtures.cpp already had to
// write out at every encodeInto() call site before this, just no longer
// naming a destination slot (finishProgram, above, owns that now).
#define PROC(argCount, body) ProcSource{argCount, body, sizeof(body) / sizeof(body[0])}

} // namespace

void initFixtures()
{
    {
        ProcSource procs[] = {PROC(0, f1Proc0), PROC(1, f1Proc1)};
        f1Prog = finishProgram(procs, 2);
    }
    {
        ProcSource procs[] = {PROC(0, f2Proc0), PROC(3, f2Proc1)};
        f2Prog = finishProgram(procs, 2);
    }
    {
        ProcSource procs[] = {PROC(0, f3Proc0)};
        f3Prog = finishProgram(procs, 1);
    }
    {
        ProcSource procs[] = {PROC(0, f4Proc0), PROC(7, f4Proc1)};
        f4Prog = finishProgram(procs, 2);
    }
    {
        ProcSource procs[] = {PROC(0, f5Proc0)};
        f5Prog = finishProgram(procs, 1);
    }
    {
        ProcSource procs[] = {PROC(0, f6Proc0)};
        f6Prog = finishProgram(procs, 1);
    }
    {
        ProcSource procs[] = {PROC(0, f7Proc0), PROC(1, f7Proc1), PROC(1, f7Proc2)};
        f7Prog = finishProgram(procs, 3);
    }
    {
        ProcSource procs[] = {PROC(1, f8Proc0)};
        f8Prog = finishProgram(procs, 1);
    }
    {
        ProcSource procs[] = {PROC(1, f9Proc0)};
        f9Prog = finishProgram(procs, 1);
    }
    {
        ProcSource procs[] = {PROC(1, f10Proc0)};
        f10Prog = finishProgram(procs, 1);
    }
    {
        ProcSource procs[] = {PROC(1, f11Proc0)};
        f11Prog = finishProgram(procs, 1);
    }
    {
        ProcSource procs[] = {PROC(1, f12Proc0)};
        f12Prog = finishProgram(procs, 1);
    }
    {
        ProcSource procs[] = {PROC(1, f13Neg)};
        f13Prog = finishProgram(procs, 1);
    }
    {
        ProcSource procs[] = {PROC(1, f14Not)};
        f14Prog = finishProgram(procs, 1);
    }
    {
        ProcSource procs[] = {PROC(1, f15Clz)};
        f15Prog = finishProgram(procs, 1);
    }
    {
        ProcSource procs[] = {PROC(1, f16Revbits)};
        f16Prog = finishProgram(procs, 1);
    }
    {
        ProcSource procs[] = {PROC(0, f17Proc0)};
        f17Prog = finishProgram(procs, 1);
    }
    {
        ProcSource procs[] = {PROC(1, f18Proc0)};
        f18Prog = finishProgram(procs, 1);
    }
    {
        ProcSource procs[] = {PROC(0, f19Proc0)};
        f19Prog = finishProgram(procs, 1);
    }
    {
        ProcSource procs[] = {PROC(1, f20Proc0)};
        f20Prog = finishProgram(procs, 1);
    }
    {
        ProcSource procs[] = {PROC(1, f21Proc0)};
        f21Prog = finishProgram(procs, 1);
    }
    {
        ProcSource procs[] = {PROC(1, f22Proc0)};
        f22Prog = finishProgram(procs, 1);
    }
    {
        ProcSource procs[] = {PROC(0, f23Proc0)};
        f23Prog = finishProgram(procs, 1);
    }
    {
        ProcSource procs[] = {PROC(0, f24Proc0)};
        f24Prog = finishProgram(procs, 1);
    }
    {
        ProcSource procs[] = {PROC(0, f25Proc0)};
        f25Prog = finishProgram(procs, 1);
    }
    {
        ProcSource procs[] = {PROC(0, f26Proc0), PROC(1, f26Proc1)};
        f26Prog = finishProgram(procs, 2);
    }
    {
        ProcSource procs[] = {PROC(0, f27Proc0), PROC(5, f27Proc1), PROC(1, f27Proc2)};
        f27Prog = finishProgram(procs, 3);
    }
}

Fixture fixtures[] = {
    {"single-arg call", &f1Prog, false, 42},
    {"phase-misaligned shuffle", &f2Prog, false, 1629},
    {"out-of-window LOAD/STORE/REG_REG", &f3Prog, false, 55},
    {"stackArgs > WINDOW_SIZE", &f4Prog, false, 280},
    {"rotation operand-fold", &f5Prog, false, 10},
    {"rotation destination-fold", &f6Prog, false, 99},
    {"3-deep call chain", &f7Prog, false, 106},

    {"LOOP body closed by RETURN, cond-false exit", &f8Prog, false, 999, 0},
    {"LOOP body closed by RETURN, body runs once", &f8Prog, false, 42, 1},
    {"LOOP body closed by RETURN, body runs once (n=7)", &f8Prog, false, 42, 7},

    {"LOOP sum(1..n), n=4", &f9Prog, false, 10, 4},
    {"LOOP sum(1..n), n=0", &f9Prog, false, 0, 0},
    {"LOOP sum(1..n), n=1", &f9Prog, false, 1, 1},

    {"BR_TABLE if/else, non-last case via RETURN, n<=10", &f10Prog, false, 111, 0},
    {"BR_TABLE if/else, non-last case via RETURN, n=10", &f10Prog, false, 111, 10},
    {"BR_TABLE if/else, non-last case via RETURN, n>10", &f10Prog, false, 222, 11},

    {"BR_TABLE N>2 jump table, selector 0", &f11Prog, false, 100, 0},
    {"BR_TABLE N>2 jump table, selector 1", &f11Prog, false, 200, 1},
    {"BR_TABLE N>2 jump table, selector 2", &f11Prog, false, 300, 2},
    {"BR_TABLE N>2 jump table, selector 3", &f11Prog, false, 400, 3},

    {"comparison feeds arithmetic (n>4)*5, n=6", &f12Prog, false, 5, 6},
    {"comparison feeds arithmetic (n>4)*5, n=3", &f12Prog, false, 0, 3},

    {"NEG(5)", &f13Prog, false, 0xFFFFFFFBu, 5},
    {"NOT(5)", &f14Prog, false, 0xFFFFFFFAu, 5},
    {"CLZ(1)", &f15Prog, false, 31, 1},
    {"CLZ(0)", &f15Prog, false, 32, 0},
    {"REVBITS(1)", &f16Prog, false, 0x80000000u, 1},

    {"PEEK_PEEK two-op-in-place AND", &f17Prog, false, 8},

    {"branch-range forced long form, n<=100", &f18Prog, false, 1, 0},
    {"branch-range forced long form, n>100", &f18Prog, false, 2, 200},

    {"binops both-imm aliasing regression: CONST(5)+ADD#1000", &f19Prog, false, 1005},
    {"binops SCRATCH_REG-operand aliasing regression, argIn=1", &f20Prog, false, 101, 1},
    {"binops SCRATCH_REG-operand aliasing regression, argIn=42", &f20Prog, false, 142, 42},

    // isCondBranch's own Condition::LE off-by-one (see fixture 21's own
    // body comment) — BR_TABLE's own guard skips case 0 when the fused
    // condition is true, so case 1 (200) fires for arg<=5 and case 0 (100)
    // for arg>5.
    {"isCondBranch LE regression: arg<=5 (true branch) + countdown", &f21Prog, false, 200, 3},
    {"isCondBranch LE regression: arg==5 (boundary, true branch) + countdown", &f21Prog, false, 200, 5},
    {"isCondBranch LE regression: arg>5 (false branch) + countdown", &f21Prog, false, 100, 9},

    {"accState fusion-merge regression: case[0] (false) probe", &f22Prog, false, 0, 5},
    {"accState fusion-merge regression: case[1] (true) probe", &f22Prog, false, 1, 200},
    {"accState fusion-merge regression: LOOP body probe", &f23Prog, false, 1},

    {"pooled literal: CONST and IMM_ACC operand", &f24Prog, false, 0x23456789u},
    {"pooled literal: mid-code flush with executed jump-around", &f25Prog, false, 0xDEADBEEFu},
    {"pooled literal in a procedure past an odd-sized one", &f26Prog, false, 0x0F0F0F0Bu},

    {"savesLR return with out-of-window args below the pushed record", &f27Prog, false, 1501},
};
const uint32_t fixtureCount = sizeof(fixtures) / sizeof(fixtures[0]);
