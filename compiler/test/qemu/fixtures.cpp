#include "fixtures.h"
#include "instr.h"

using namespace jitc;

const Proc *g_realProcs = nullptr;
uint32_t g_realProcCount = 0;

namespace {

// ---- Fixture 1: call.test.ts program 1 — "single-argument call,
// entirely acc-passed" — expect 42. ----------------------------------
const Instr f1_proc0[] = {CONST(37), call(1), bare(Op::RETURN)};
const Instr f1_proc1[] = {LOAD(0), opImm(Op::ADD, 5), bare(Op::RETURN)};
const Proc f1_procs[] = {
    {0, f1_proc0, 3},
    {1, f1_proc1, 3},
};

// ---- Fixture 2: call.test.ts program 2 — "3-argument call with a
// phase-misaligned shuffle and surviving leftover locals" — expect 1629.
const Instr f2_proc0[] = {
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
const Instr f2_proc1[] = {LOAD(0), opReg(Op::ADD, 1), opReg(Op::ADD, 2), bare(Op::RETURN)};
const Proc f2_procs[] = {
    {0, f2_proc0, sizeof(f2_proc0) / sizeof(f2_proc0[0])},
    {3, f2_proc1, sizeof(f2_proc1) / sizeof(f2_proc1[0])},
};

// ---- Fixture 3: deep-args.test.ts program 5 — out-of-window
// LOAD/STORE/REG_ACC/REG_REG, no CALL — expect 55. --------------------
const Instr f3_proc0[] = {
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
const Proc f3_procs[] = {
    {0, f3_proc0, sizeof(f3_proc0) / sizeof(f3_proc0[0])},
};

// ---- Fixture 4: deep-args.test.ts program 6 — CALL with
// stackArgs(6) > WINDOW_SIZE(4) — expect 280. -------------------------
const Instr f4_proc0[] = {
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
const Instr f4_proc1[] = {
    LOAD(0),
    opReg(Op::ADD, 1), opReg(Op::ADD, 2), opReg(Op::ADD, 3),
    opReg(Op::ADD, 4), opReg(Op::ADD, 5), opReg(Op::ADD, 6),
    bare(Op::RETURN),
};
const Proc f4_procs[] = {
    {0, f4_proc0, sizeof(f4_proc0) / sizeof(f4_proc0[0])},
    {7, f4_proc1, sizeof(f4_proc1) / sizeof(f4_proc1[0])},
};

// ---- Fixture 5: rotation.test.ts "operand-fold" -- expect 10. -------
const Instr f5_proc0[] = {
    CONST(10), PUSH(), // a -- k=0
    CONST(20), PUSH(), // b -- k=1
    CONST(30), PUSH(), // c -- k=2
    CONST(40), PUSH(), // d -- k=3
    LOAD(0),             // acc = a; accState depends on physReg(0)
    PUSH(),                // e = a -- k=4, evicts k=0's register
    bare(Op::RETURN),
};
const Proc f5_procs[] = {
    {0, f5_proc0, sizeof(f5_proc0) / sizeof(f5_proc0[0])},
};

// ---- Fixture 6: rotation.test.ts "destination-fold" -- expect 99. ---
const Instr f6_proc0[] = {
    CONST(10), PUSH(),
    CONST(20), PUSH(),
    CONST(30), PUSH(),
    CONST(40), PUSH(),
    CONST(99), STORE(0), // a := 99; accState depends on physReg(0)
    PUSH(),                // e = a (now 99) -- k=4, evicts k=0's register
    bare(Op::RETURN),
};
const Proc f6_procs[] = {
    {0, f6_proc0, sizeof(f6_proc0) / sizeof(f6_proc0[0])},
};

// ---- Fixture 7: abi-dispatch.test.ts's 3-deep call chain -- expect 106.
const Instr f7_proc0[] = {CONST(5), call(1), bare(Op::RETURN)};
const Instr f7_proc1[] = {LOAD(0), call(2), opImm(Op::ADD, 1), bare(Op::RETURN)};
const Instr f7_proc2[] = {LOAD(0), opImm(Op::ADD, 100), bare(Op::RETURN)};
const Proc f7_procs[] = {
    {0, f7_proc0, 3},
    {1, f7_proc1, 4},
    {1, f7_proc2, 3},
};

} // namespace

const Fixture g_fixtures[] = {
    {"call.test.ts#1 single-arg call", f1_procs, 2, false, 42},
    {"call.test.ts#2 phase-misaligned shuffle", f2_procs, 2, false, 1629},
    {"deep-args#5 out-of-window LOAD/STORE/REG_REG", f3_procs, 1, false, 55},
    {"deep-args#6 stackArgs > WINDOW_SIZE", f4_procs, 2, false, 280},
    {"rotation operand-fold", f5_procs, 1, false, 10},
    {"rotation destination-fold", f6_procs, 1, false, 99},
    {"abi-dispatch 3-deep call chain", f7_procs, 3, false, 106},
};
const uint32_t g_fixtureCount = sizeof(g_fixtures) / sizeof(g_fixtures[0]);
