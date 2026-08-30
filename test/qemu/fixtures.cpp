#include "fixtures.h"
#include "instr.h"
#include "encode_instr.h"
#include "corpus_programs.h" // fixtures 28-32's own Instr[] bodies, shared with test/tools/dump_corpus.cpp

using namespace jitc;

// ---- Fixture 1: single-argument call, entirely acc-passed. expect 42.
static const Instr f1Proc0[] = {CONST(37), call(1), bare(Op::RETURN)};
static const Instr f1Proc1[] = {LOAD(0), opImm(Op::ADD, 5), bare(Op::RETURN)};
static Program f1Prog;

// ---- Fixture 2: 3-argument call with a phase-misaligned shuffle and
// surviving leftover locals. expect 1629.
static const Instr f2Proc0[] = {
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
static const Instr f2Proc1[] = {LOAD(0), opReg(Op::ADD, 1), opReg(Op::ADD, 2), bare(Op::RETURN)};
static Program f2Prog;

// ---- Fixture 3: out-of-window LOAD/STORE/REG_ACC/REG_REG, no CALL. expect 55.
static const Instr f3Proc0[] = {
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
static Program f3Prog;

// ---- Fixture 4: CALL with stackArgs(6) > WINDOW_SIZE(4). expect 280.
static const Instr f4Proc0[] = {
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
static const Instr f4Proc1[] = {
    LOAD(0),
    opReg(Op::ADD, 1), opReg(Op::ADD, 2), opReg(Op::ADD, 3),
    opReg(Op::ADD, 4), opReg(Op::ADD, 5), opReg(Op::ADD, 6),
    bare(Op::RETURN),
};
static Program f4Prog;

// ---- Fixture 5: operand-fold. expect 10.
static const Instr f5Proc0[] = {
    CONST(10), PUSH(), // a -- k=0
    CONST(20), PUSH(), // b -- k=1
    CONST(30), PUSH(), // c -- k=2
    CONST(40), PUSH(), // d -- k=3
    LOAD(0),             // acc = a; accState depends on physReg(0)
    PUSH(),                // e = a -- k=4, evicts k=0's register
    bare(Op::RETURN),
};
static Program f5Prog;

// ---- Fixture 6: destination-fold. expect 99.
static const Instr f6Proc0[] = {
    CONST(10), PUSH(),
    CONST(20), PUSH(),
    CONST(30), PUSH(),
    CONST(40), PUSH(),
    CONST(99), STORE(0), // a := 99; accState depends on physReg(0)
    PUSH(),                // e = a (now 99) -- k=4, evicts k=0's register
    bare(Op::RETURN),
};
static Program f6Prog;

// ---- Fixture 7: a 3-deep call chain. expect 106.
static const Instr f7Proc0[] = {CONST(5), call(1), bare(Op::RETURN)};
static const Instr f7Proc1[] = {LOAD(0), call(2), opImm(Op::ADD, 1), bare(Op::RETURN)};
static const Instr f7Proc2[] = {LOAD(0), opImm(Op::ADD, 100), bare(Op::RETURN)};
static Program f7Prog;

// ---- Fixture 8: LOOP body closed by a bare terminator, not BLOCK_END.
// arg 0 -> 999 (cond-false exit tail), arg != 0 -> 42 (body runs once,
// returns directly).
static const Instr f8Proc0[] = {
    bare(Op::LOOP), bare(Op::BLOCK_END),  // condition sub-block is empty — testAccNonzero(arg)
    CONST(42), bare(Op::RETURN),           // body — bare terminator closes it
    CONST(999), bare(Op::RETURN),          // reached only via the cond-false exit
};
static Program f8Prog;

// ---- Fixture 9: a genuine (non-degenerate) LOOP with real accumulation
// and a back-edge — sum(1..n). arg 4 -> 10, arg 0 -> 0, arg 1 -> 1.
static const Instr f9Proc0[] = {
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
static Program f9Prog;

// ---- Fixture 10: BR_TABLE if/else fusion, non-last case closed via a
// bare RETURN, last case closes normally. arg <= 10 -> 111, arg > 10 -> 222.
static const Instr f10Proc0[] = {
    LOAD(0), opImm(Op::GT_U, 10), brTable(2),
        CONST(111), bare(Op::RETURN),   // case 0 (n <= 10) — bare terminator
        CONST(222), bare(Op::BLOCK_END), // case 1 (n > 10) — normal close
    bare(Op::RETURN),
};
static Program f10Prog;

// ---- Fixture 11: BR_TABLE N>2, the shared jump-table helper.
// arg 0/1/2/3 -> 100/200/300/400.
static const Instr f11Proc0[] = {
    LOAD(0), brTable(4),
        CONST(100), bare(Op::BLOCK_END),
        CONST(200), bare(Op::BLOCK_END),
        CONST(300), bare(Op::BLOCK_END),
        CONST(400), bare(Op::BLOCK_END),
    bare(Op::RETURN),
};
static Program f11Prog;

// ---- Fixture 12: a comparison feeds further arithmetic — (n > 4) * 5.
// arg 6 -> 5, arg 3 -> 0.
static const Instr f12Proc0[] = {LOAD(0), opImm(Op::GT_U, 4), opImm(Op::MUL, 5), bare(Op::RETURN)};
static Program f12Prog;

// ---- Fixtures 13-16: unary ops, one procedure per op.
static const Instr f13Neg[] = {LOAD(0), bare(Op::NEG), bare(Op::RETURN)};       // arg 5 -> 0xFFFFFFFB
static Program f13Prog;
static const Instr f14Not[] = {LOAD(0), bare(Op::NOT), bare(Op::RETURN)};       // arg 5 -> 0xFFFFFFFA
static Program f14Prog;
static const Instr f15Clz[] = {LOAD(0), bare(Op::CLZ), bare(Op::RETURN)};       // arg 1 -> 31, arg 0 -> 32
static Program f15Prog;
static const Instr f16Revbits[] = {LOAD(0), bare(Op::REVBITS), bare(Op::RETURN)}; // arg 1 -> 0x80000000
static Program f16Prog;

// ---- Fixture 17: PEEK_PEEK two-op-in-place. 10 & 12 = 8.
static const Instr f17Proc0[] = {
    CONST(12), PUSH(),                 // k=0 = 12
    CONST(10),                          // acc = 10 (pending)
    opStack(Op::AND, Combo::PEEK_PEEK), // k0 := 10 & 12 = 8; acc poisoned
    POP(), bare(Op::RETURN),
};
static Program f17Prog;

// ---- Fixture 18: branch-range guard forced into the long (invert-and-
// branch) form — case 0's own body is padded past the 240-byte safe span,
// so the dispatch guard itself can't be a bare short-form conditional
// branch. arg <= 100 -> 1 (via the long form), arg > 100 -> 2 (normal).
static const Instr f18Proc0[] = {
    LOAD(0), opImm(Op::GT_U, 100), brTable(2),
        bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT),
        bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT),
        bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT),
        bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT),
        CONST(1), bare(Op::RETURN),      // case 0 (n <= 100) — 20 NOTs + terminator, 21*16 = 336 > 240
        CONST(2), bare(Op::BLOCK_END),   // case 1 (n > 100) — normal
    bare(Op::RETURN),
};
static Program f18Prog;

// ---- Fixture 19: regression for emitAddSubRsub with accShape and the
// IMM_ACC operand both compile-time immediates (CONST directly followed by
// an immediate arithmetic op), with the combined immediate too large for
// the imm3/imm8 fast paths — materializing accShape into SCRATCH_REG and
// then the second immediate into that same register clobbers the first
// value, silently computing `k op k` instead of `accShape op k`.
// expect 5 + 1000 = 1005.
static const Instr f19Proc0[] = {CONST(5), opImm(Op::ADD, 1000), bare(Op::RETURN)};
static Program f19Prog;

// ---- Fixture 20: the same aliasing bug class, but the operand register
// itself happens to be SCRATCH_REG, which happens for real whenever an
// out-of-window local gets reloaded via ldrSp(SCRATCH_REG, ...). argCount=1
// starts with slot 0 (the argument) in the window (tos=1); four PUSHes
// bring tos to 5, exactly the point (inWindow's own `tos - k <= WINDOW_SIZE`)
// where slot 0 gets evicted onto the real stack, so `opReg(ADD, 0)` must
// reload it through SCRATCH_REG right after CONST(100) leaves acc pending.
// expect 100 + argIn.
static const Instr f20Proc0[] = {
    CONST(2), PUSH(), // slot1=2, tos=2
    CONST(3), PUSH(), // slot2=3, tos=3
    CONST(4), PUSH(), // slot3=4, tos=4
    CONST(5), PUSH(), // slot4=5, tos=5 — slot 0 (argIn) now spilled
    CONST(100),
    opReg(Op::ADD, 0),
    bare(Op::RETURN),
};
static Program f20Prog;

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
static const Instr f21Proc0[] = {
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
static Program f21Prog;

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
static const Instr f22Proc0[] = {
    CONST(77), PUSH(),                      // slot1 = 77 (stale sentinel)
    LOAD(0), opImm(Op::GE_U, 0x80), brTable(2),
        STORE(1), bare(Op::BLOCK_END),      // case 0 (false): probe
        STORE(1), bare(Op::BLOCK_END),      // case 1 (true): probe
    LOAD(1),
    bare(Op::RETURN),
};
static Program f22Prog;

// ---- Fixture 23: the same bug's LOOP-body half — the fused condition
// closing LOOP's own condition sub-block has the identical gap. x = 7 (not
// 0/1) is forced to 0 right inside the body instead of decremented
// normally, so the loop runs its body exactly once. expect 1.
static const Instr f23Proc0[] = {
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
static Program f23Prog;

// ---- Fixture 24: literal pooling, both routes at once. CONST's own
// hard-to-synthesize value pools, and so does the ADD's immediate operand
// (which reaches the pool through Combo::IMM_ACC rather than CONST), so
// this executes two PC-relative loads at different alignment parities.
// expect 0x12345678 + 0x11111111.
static const Instr f24Proc0[] = {CONST(0x12345678), opImm(Op::ADD, 0x11111111), bare(Op::RETURN)};
static Program f24Prog;

// ---- Fixture 25: a pooled load whose pool is flushed mid-procedure,
// with the flush's branch-around actually executed. BR_TABLE N>2 forces
// the flush (its jump table's raw halfwords must not land in a later
// scan window), so the pool lands in the middle of the code and control
// has to jump over it to reach the dispatch. The pooled value is read
// back afterwards from a local, proving both the load and the jump-around
// worked. expect 0xDEADBEEF.
static const Instr f25Proc0[] = {
    CONST(0xDEADBEEF), PUSH(), // pooled — the chunk is open across the BR_TABLE
    CONST(1), brTable(3),       // forces the flush, mid-code, before the table
        CONST(100), bare(Op::BLOCK_END),
        CONST(200), bare(Op::BLOCK_END),
        CONST(300), bare(Op::BLOCK_END),
    LOAD(0),
    bare(Op::RETURN),
};
static Program f25Prog;

// ---- Fixture 26: a pooled load in a procedure that does *not* start at
// the arena base. proc0 compiles to an odd number of halfwords (38 bytes,
// i.e. 2 mod 4), so without Runtime::allocate rounding its reservation up
// proc1 would land 2 bytes off a word boundary — and every
// Align(pc,4)-based literal offset in it, resolved procedure-relative at
// translation time, would then read 2 bytes away from its own pool word.
// This is the fixture that actually fails if that rounding is dropped.
// 5 ^ 0x0F0F0F0F = 0x0F0F0F0A, + 1 = 0x0F0F0F0B.
static const Instr f26Proc0[] = {CONST(5), call(1), opImm(Op::ADD, 1), bare(Op::RETURN)};
static const Instr f26Proc1[] = {LOAD(0), opImm(Op::XOR, 0x0F0F0F0F), bare(Op::RETURN)};
static Program f26Prog;

// ---- Fixture 27: a non-leaf procedure (proc1, argCount=5) with an
// out-of-window argument (k=0, argCount > WINDOW_SIZE(4)) sitting below its
// own pushed call/return record — the abiEmitReturn/returnHelperFromStack-
// Reclaim path (savesLR && initialSpilledCount > 0). k=0 is read after
// proc1's own nested call returns, exercising spillOffset's savesLR shift
// for a live read, not just the reclaim at RETURN. expect 1501:
// proc2(1) = 1001, + arg4(500) = 1501.
static const Instr f27Proc0[] = {
    CONST(1), PUSH(),  // arg0 for proc1 -- k=0, proc1's out-of-window arg
    CONST(2), PUSH(),  // arg1 -- k=1
    CONST(3), PUSH(),  // arg2 -- k=2
    CONST(4), PUSH(),  // arg3 -- k=3
    CONST(500),         // arg4 -- last, via acc
    call(1),
    bare(Op::RETURN),
};
static const Instr f27Proc1[] = {
    LOAD(0),              // acc = arg0 (k=0, out-of-window)
    call(2),               // proc2(arg0) -- makes proc1 non-leaf (savesLR)
    opReg(Op::ADD, 4),       // acc += arg4 (k=4, still in-window)
    bare(Op::RETURN),
};
static const Instr f27Proc2[] = {LOAD(0), opImm(Op::ADD, 1000), bare(Op::RETURN)};
static Program f27Prog;

// ---- Fixture 28: nested LOOP-in-LOOP, sum of triangular numbers
// (sum_{i=1..n} sum_{j=1..i} j). First fixture with 2-level LOOP nesting
// (maxSpanBytes/translateLoop recursion at depth 2 — every existing LOOP
// fixture nests one level only). All four working locals (k1..k4) are
// PUSHed once, ahead of the outer LOOP, and only ever STOREd inside either
// loop body — tos stays fixed at 5 (k0 arg + k1..k4) across both loops' own
// back-edges, which also spills k0 out of the window for free (WINDOW_SIZE
// is 4). Body lives in corpus_programs.h, shared with
// test/tools/dump_corpus.cpp. expect n=3 -> 10 (3+2+1 via 6+3+1), n=1 -> 1,
// n=0 -> 0.
static Program f28Prog;

// ---- Fixture 29: BR_TABLE nested inside a LOOP body. Each iteration
// dispatches on counter&1 (even -> total += counter*10, odd ->
// total += counter), both cases closed via BLOCK_END so control rejoins
// the loop's own decrement before the back-edge — the interaction between
// fused-branch dispatch and a live loop back-edge, distinct from fixture
// 23's fused *loop condition* itself. Body lives in corpus_programs.h.
// expect n=4 -> 64, n=5 -> 69, n=0 -> 0.
static Program f29Prog;

// ---- Fixture 30: LOOP nested inside a BR_TABLE case — the mirror image of
// fixture 29. Selector and n both travel packed into this fixture's single
// argument: selector in bits[15:8], n in bits[7:0]. That packing was once
// forced — enterDispatch's boot call passed one word through acc alone,
// with no caller-side shuffle to populate a window slot for a second
// argument — and is now merely how this fixture happens to be written; a
// two-argument entry procedure would work fine (fixtures 41-48). Kept as
// is because the packing is incidental to what it tests. Case 0 runs a full sum(1..n) LOOP using two extra PUSHed
// locals, then POPs them off again before the case's own BLOCK_END so tos
// returns to its pre-brTable value (1), matching case 1 (which never
// touches tos) — POP() mirrors PUSH() by loading the popped slot's own
// value back into acc (see fixture 17's own "acc poisoned" comment), so
// total is pushed *before* counter: the first POP discards counter's spent
// (zero) value, and the second POP is the one that lands the real result in
// acc, which the case then STOREs to the result slot k1 -- acc itself
// cannot cross a BR_TABLE's merge point (isa-core.md §8.7), so a
// value-producing dispatch delivers through a slot. Body lives in
// corpus_programs.h. expect (selector=0,n=4) -> 10; (selector=1,n=4) -> 12.
static Program f30Prog;

// ---- Fixture 31: large BR_TABLE (N=20) with a CALL inside one case. The
// first fixture combining a jump table with N well past 4 (stressing
// brTableJumpHelper's relocation math and the jump table's own literal-
// pool sizing at real scale) with a real CALL inside a case — re-exercises
// proc_scan.cpp's triggersLRSave/`(uint32_t)instr.imm > 2` fix end-to-end,
// since a large N combined with a real CALL is exactly the combination
// that bug could disagree on between the scan pass and the real
// translation pass. Each case STOREs to a result slot for the same reason
// fixture 30 does. Body lives in corpus_programs.h. expect selector=7 ->
// 1005, selector=3 -> 30, selector=19 -> 190.
static Program f31Prog;

// ---- Fixture 32: deep operand stack, 24 live locals. Everything past k3
// is out-of-window (WINDOW_SIZE=4), so the chain of ADDs reload-addresses
// 20 spilled slots in one procedure — several times deeper than the
// existing widest case (fixture 4's 6 stack args). Body lives in
// corpus_programs.h. expect 1+2+...+24 = 300.
static Program f32Prog;

// ---- Fixture 33: acc-fold thrash inside a loop. Per iteration:
// total := (total + 7) & 0xF -- an operand-fold-eligible ADD immediately
// followed by a never-folds AND, repeated across several loop iterations
// (existing fixtures 5/6/17 exercise fold/no-fold transitions exactly
// once; this repeats it every back-edge). expect n=5 -> 3 (0->7->14->5->
// 12->3, four decrements shown: 7,14,5,12,3 over 5 iterations).
static const Instr f33Proc0[] = {
    LOAD(0), PUSH(),  // k1 = counter := n
    CONST(0), PUSH(), // k2 = total := 0
    bare(Op::LOOP),
        LOAD(1),
    bare(Op::BLOCK_END), // while(counter != 0)
        LOAD(2), opImm(Op::ADD, 7), opImm(Op::AND, 0xF), STORE(2), // total := (total+7)&0xF
        LOAD(1), opImm(Op::SUB, 1), STORE(1),
    bare(Op::BLOCK_END), // back-edge
    LOAD(2), bare(Op::RETURN),
};
static Program f33Prog;

// ---- Fixtures 34/35: NEG/NOT consuming an out-of-window (spilled)
// operand. Mirrors fixtures 13/14, but with 4 PUSHes ahead of the arg so
// k0 is spilled by the time LOAD(0) reloads it, immediately followed by
// the unary op -- the exact shape emitUnary's src-parameter path needs
// (reading the reload's own destination register directly instead of
// forcing a flush through ACC_REG first). No existing fixture or host
// unit test covers a spilled operand here. expect arg=5 -> NEG:
// 0xFFFFFFFB, NOT: 0xFFFFFFFA (same values as fixtures 13/14).
static const Instr f34Proc0[] = {
    CONST(1), PUSH(), CONST(2), PUSH(), CONST(3), PUSH(), CONST(4), PUSH(),
    LOAD(0), bare(Op::NEG), bare(Op::RETURN),
};
static Program f34Prog;
static const Instr f35Proc0[] = {
    CONST(1), PUSH(), CONST(2), PUSH(), CONST(3), PUSH(), CONST(4), PUSH(),
    LOAD(0), bare(Op::NOT), bare(Op::RETURN),
};
static Program f35Prog;

// ---- Fixture 36: LOOP back-edge forced into the long-branch form.
// Padded with 20 bare(Op::NOT)s inside the loop body (mirroring fixture
// 18's technique, but applied to translateLoop's own back-edge rather
// than an if/else guard -- 21*ORDINARY_MAX_BYTES(16) = 336 >
// SAFE_COND_BRANCH_SPAN(240)). Counts down to 0 regardless of padding;
// the interesting part is that it compiles and runs at all. expect
// arg=1 -> 0, arg=50 -> 0.
static const Instr f36Proc0[] = {
    LOAD(0), PUSH(), // k1 = counter := n
    bare(Op::LOOP),
        LOAD(1),
    bare(Op::BLOCK_END), // while(counter != 0)
        bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT),
        bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT),
        bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT),
        bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT),
        LOAD(1), opImm(Op::SUB, 1), STORE(1),
    bare(Op::BLOCK_END), // back-edge, forced into the long form
    LOAD(1), bare(Op::RETURN),
};
static Program f36Prog;

// ---- Fixture 37: two-level savesLR/returnHelperFromStackReclaim chain,
// extending fixture 27's shape (one non-leaf procedure with an
// out-of-window arg below its own pushed call record) to two nested
// levels: proc0 -> proc1 (argCount=5, out-of-window arg, calls proc2) ->
// proc2 (argCount=5, out-of-window arg, calls proc3) -> proc3 (leaf).
// Exercises returnHelperFromStackReclaim at two call depths instead of
// one. expect proc3(10)=1010; proc2 = proc3(10)+arg4(500) = 1510; proc1 =
// proc2(...)+ownArg0(1)+ownArg4(50) = 1510+1+50 = 1561.
static const Instr f37Proc0[] = {
    CONST(1), PUSH(),  // arg0 for proc1 -- k=0, proc1's out-of-window arg
    CONST(2), PUSH(),  // arg1 -- k=1
    CONST(3), PUSH(),  // arg2 -- k=2
    CONST(4), PUSH(),  // arg3 -- k=3
    CONST(50),          // arg4 -- last, via acc
    call(1),
    bare(Op::RETURN),
};
static const Instr f37Proc1[] = {
    LOAD(0),               // acc = arg0 (k=0, out-of-window)
    PUSH(),                 // k=5 = saved copy of proc1's own arg0, survives across the nested call
    CONST(10), PUSH(),      // arg0 for proc2 -- k=6, proc2's own out-of-window arg
    CONST(11), PUSH(),      // arg1 -- k=7
    CONST(12), PUSH(),      // arg2 -- k=8
    CONST(13), PUSH(),      // arg3 -- k=9
    CONST(500),              // arg4 -- last, via acc
    call(2),                  // proc2(10,11,12,13,500) -- makes proc1 non-leaf (savesLR)
    opReg(Op::ADD, 5),         // acc += proc1's own saved arg0 (k=5)
    opReg(Op::ADD, 4),          // acc += arg4 (k=4, still in-window)
    bare(Op::RETURN),
};
static const Instr f37Proc2[] = {
    LOAD(0),           // acc = arg0 (k=0, out-of-window)
    call(3),            // proc3(arg0) -- makes proc2 non-leaf (savesLR)
    opReg(Op::ADD, 4),   // acc += arg4 (k=4, still in-window)
    bare(Op::RETURN),
};
static const Instr f37Proc3[] = {LOAD(0), opImm(Op::ADD, 1000), bare(Op::RETURN)};
static Program f37Prog;

// ---- Fixtures 38/39/40: a bytecode TRAP really unwinds (isa-core.md
// §4.5/§9). Fixture 38 is the four-instruction program fuzz/qemu_exec
// minimized the original 195-instruction finding down to: the trap sits in
// a *nested* procedure, so a TRAP compiled as an ordinary return handed
// 0x800002f2 to proc0 as a return value and proc0 went on to return 92.
// Now it reaches enterDispatch's landing directly, tagged LANDING_TRAP,
// with the code untouched in value. expect trapped=LANDING_TRAP, 754.
static const Instr f38Proc0[] = {call(1), CONST(92), bare(Op::RETURN)};
static const Instr f38Proc1[] = {trapInstr(754)};
static Program f38Prog;

// Fixture 39: TRAP in the *entry* procedure, the one case the old
// sentinel encoding got right — kept because it is now right for a
// different reason (the tag, not bit 31 of the value), and because five
// live pushed locals mean sp is nowhere near its entry value when the
// trap fires. trapHelper's own `mov sp, savedSp` is what makes that
// irrelevant; the old code needed discardWindow first. expect
// trapped=LANDING_TRAP, 41.
static const Instr f39Proc0[] = {
    CONST(1), PUSH(), CONST(2), PUSH(), CONST(3), PUSH(), CONST(4), PUSH(), CONST(5), PUSH(),
    trapInstr(41),
};
static Program f39Prog;

// Fixture 40: the worst frame shape to unwind out of — proc1 has
// argCount 5, so its own arg0 sits out of window *below* the call record
// its prologue pushed (abi_strategy.cpp's returnHelperFromStackReclaim
// case), and both frames have live pushed locals at the moment proc2
// traps two levels down. Nothing on any of the three frames is unwound a
// step at a time; the single sp restore subsumes all of it. expect
// trapped=LANDING_TRAP, 1000.
static const Instr f40Proc0[] = {
    CONST(7), PUSH(),   // k0 -- live across the call, never read again
    CONST(10), PUSH(),  // arg0 for proc1 -- k=1, proc1's own out-of-window arg
    CONST(11), PUSH(),  // arg1 -- k=2
    CONST(12), PUSH(),  // arg2 -- k=3
    CONST(13), PUSH(),  // arg3 -- k=4
    CONST(14),          // arg4 -- last, via acc
    call(1),
    opReg(Op::ADD, 0), bare(Op::RETURN), // unreachable once proc2 traps
};
static const Instr f40Proc1[] = {
    LOAD(0), PUSH(),    // k5 -- proc1's own saved copy of its out-of-window arg0
    CONST(21),          // proc2's only argument, via acc
    call(2),            // makes proc1 non-leaf: its prologue pushes the record
    bare(Op::RETURN),   // unreachable
};
static const Instr f40Proc2[] = {trapInstr(1000)};
static Program f40Prog;

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
// (putByte, test/encode_instr.cpp), which is what would actually
// catch it on a host build; the QEMU build's own -DNDEBUG strips that, so
// keep this margin real rather than tight.
static constexpr uint32_t SCRATCH_CAPACITY = 3072;
static uint8_t scratch[SCRATCH_CAPACITY];
static uint32_t scratchUsed = 0;

// max_call_depth is 0 for every fixture here, and total_depth is the entry
// procedure's own arg_count rather than a real whole-program figure:
// main.cpp's fixture loop does run every one of them through
// Executor::run's own real up-front stack-budget check, but so slack an
// envelope makes that check see almost no operand-stack/call-record cost,
// leaving essentially the fixed-cost floor (Runtime/dispatch-table size,
// ENTER_DISPATCH_FIXED_BYTES, TRANSLATOR_ENTRY_WORST_CASE_BYTES) — nowhere
// near tight enough to reject anything real. Exercising the check against
// real, hand-derived max_call_depth/total_depth values is what
// Executor::onStack/Executor::split's own dedicated scenarios in
// main.cpp are for instead.
//
// Not zero, though, which it used to be: enterProgramCore refuses to push a
// multi-argument entry procedure's out-of-window arguments past whatever
// total_depth claims (RESOURCE_PROGRAM_ENTRY_DEPTH), since that figure is
// what sized the reservation they land in. arg_count is the lower bound
// validateProgram itself guarantees for a real program — seeding every
// procedure's local peak at its own arg_count — so using it here keeps the
// envelope deliberately slack overall while staying honest about the one
// relationship that is load-bearing.
static Program finishProgram(const ProcSource *procs, uint32_t count)
{
    uint8_t *slot = scratch + scratchUsed;
    uint32_t len = encodeJitProgram(0, procs[0].argCount, procs, count, slot, SCRATCH_CAPACITY - scratchUsed);
    scratchUsed += len;
    return Program{slot, len, procs[0].argCount};
}

// ---- Fixtures 41-48: multi-argument ENTRY procedures.
//
// Everything above reaches an out-of-window argument through proc1 or
// deeper, never through proc0, because until enterDispatch learned to
// marshal an argument vector there was no way to give the entry procedure
// more than the single acc-borne word. Both halves of that gap are covered
// here: 2..4 arguments (window registers enterDispatch never used to
// initialize, so they arrived holding the caller's r8-r11) and 5+ (where the
// epilogue also reclaimed a frame nobody had pushed, landing .Lresume on a
// shifted sp — a deterministic hang, not a wrong answer).
//
// Bodies pack their arguments into nibbles rather than summing them: a sum
// is invariant under any permutation of the window, which is precisely the
// error class most likely here. args {1,2,3,...} therefore expect 0x123...,
// and any swapped register or mis-ordered spill slot changes the result.
static const uint32_t entryArgs2[] = {1, 2};
static const uint32_t entryArgs4[] = {1, 2, 3, 4};
static const uint32_t entryArgs5[] = {1, 2, 3, 4, 5};
static const uint32_t entryArgs6[] = {1, 2, 3, 4, 5, 6};
static const uint32_t entryArgs8[] = {1, 2, 3, 4, 5, 6, 7, 8};

// Nibble-pack k = 0..N-1: acc = arg0, then (acc << 4) | arg_k.
#define PACK_ARG(k) opImm(Op::SHL, 4), opReg(Op::OR, k)

// Fixture 41: two arguments — entirely in-window. arg0 lands in
// physReg(0) = r7, which enterDispatch had no way to write at all. expect
// 0x12.
static const Instr f41Proc0[] = {LOAD(0), PACK_ARG(1), bare(Op::RETURN)};
static Program f41Prog;

// Fixture 42: four arguments — exactly fills the window, and exactly
// fillCalleeArgs's WINDOW_SIZE-1 cap (r7/r6/r5 supplied by the caller, r4
// from acc). Still nothing spilled. expect 0x1234.
static const Instr f42Proc0[] = {LOAD(0), PACK_ARG(1), PACK_ARG(2), PACK_ARG(3), bare(Op::RETURN)};
static Program f42Prog;

// Fixture 43: five arguments — one spilled word, read through
// spillOffset(0) == 0. Leaf, so the whole frame comes back via
// discardWindow's single ADD sp. expect 0x12345.
static const Instr f43Proc0[] = {
    LOAD(0), PACK_ARG(1), PACK_ARG(2), PACK_ARG(3), PACK_ARG(4), bare(Op::RETURN),
};
static Program f43Prog;

// Fixture 44: six arguments — TWO spilled words, the smallest shape in
// which their order is observable at all (five spills exactly one, so a
// reversed push loop looks identical there). expect 0x123456.
static const Instr f44Proc0[] = {
    LOAD(0), PACK_ARG(1), PACK_ARG(2), PACK_ARG(3), PACK_ARG(4), PACK_ARG(5), bare(Op::RETURN),
};
static Program f44Prog;

// Fixture 45: eight arguments — the post-wrap window phase (slots 4..6 in
// r7/r6/r5, slot 7 from acc into physReg(7) = r4) with four spilled words.
// expect 0x12345678.
static const Instr f45Proc0[] = {
    LOAD(0), PACK_ARG(1), PACK_ARG(2), PACK_ARG(3),
    PACK_ARG(4), PACK_ARG(5), PACK_ARG(6), PACK_ARG(7), bare(Op::RETURN),
};
static Program f45Prog;

// Fixture 46: six arguments AND a nested CALL, so the entry procedure is
// savesLR. Its own push{lr} lands above the words enterDispatch placed,
// which is exactly the +4 shift Window::spillOffset applies to
// k < initialSpilledCount — and the return goes through
// returnHelperFromStackReclaim with r2 = 8, on an *entry* frame. This is
// the shape that hangs deterministically without the fix; fixtures 27/37
// only ever reached it via proc1. expect 0x123456 + 1000.
static const Instr f46Proc0[] = {
    LOAD(0), PACK_ARG(1), PACK_ARG(2), PACK_ARG(3), PACK_ARG(4), PACK_ARG(5),
    PUSH(),           // keep the packed value while the callee runs
    CONST(0),
    call(1),
    opReg(Op::ADD, 6), // += the packed value
    bare(Op::RETURN),
};
static const Instr f46Proc1[] = {CONST(1000), bare(Op::RETURN)};
static Program f46Prog;

// Fixture 47: six arguments, body TRAPs. trapHelper restores sp from
// runtime->savedSp, which is only correct if savedSp was captured *before*
// enterDispatch pushed the arguments. Captured after, .Lresume would pop
// those words as the caller's r8-r11 and return to a garbage pc — a hang,
// so this fixture reports a wrong outcome rather than a wrong value.
static const Instr f47Proc0[] = {
    LOAD(0), PACK_ARG(1), PACK_ARG(2), PACK_ARG(3), PACK_ARG(4), PACK_ARG(5),
    trapInstr(0x123456),
};
static Program f47Prog;

// Fixture 48: six-argument entry whose callee traps — the same unwind one
// level down, with the entry procedure's own out-of-window arguments live
// below the frame being discarded.
static const Instr f48Proc0[] = {
    LOAD(0), PACK_ARG(1), PACK_ARG(2), PACK_ARG(3), PACK_ARG(4), PACK_ARG(5),
    call(1), bare(Op::RETURN),
};
static const Instr f48Proc1[] = {trapInstr(4242)};
static Program f48Prog;

#undef PACK_ARG

// Instr[]'s own element count, paired with its own argCount — one
// ProcSource per procedure, exactly what fixtures.cpp already had to
// write out at every encodeInto() call site before this, just no longer
// naming a destination slot (finishProgram, above, owns that now).
#define PROC(argCount, body) ProcSource{argCount, body, sizeof(body) / sizeof(body[0])}

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
    {
        ProcSource procs[] = {PROC(1, corpusNestedLoopProc0)};
        f28Prog = finishProgram(procs, 1);
    }
    {
        ProcSource procs[] = {PROC(1, corpusBrTableInLoopProc0)};
        f29Prog = finishProgram(procs, 1);
    }
    {
        ProcSource procs[] = {PROC(1, corpusLoopInBrTableProc0)};
        f30Prog = finishProgram(procs, 1);
    }
    {
        ProcSource procs[] = {PROC(1, corpusLargeBrTableProc0), PROC(1, corpusLargeBrTableProc1)};
        f31Prog = finishProgram(procs, 2);
    }
    {
        ProcSource procs[] = {PROC(0, corpusDeepStackProc0)};
        f32Prog = finishProgram(procs, 1);
    }
    {
        ProcSource procs[] = {PROC(1, f33Proc0)};
        f33Prog = finishProgram(procs, 1);
    }
    {
        ProcSource procs[] = {PROC(1, f34Proc0)};
        f34Prog = finishProgram(procs, 1);
    }
    {
        ProcSource procs[] = {PROC(1, f35Proc0)};
        f35Prog = finishProgram(procs, 1);
    }
    {
        ProcSource procs[] = {PROC(1, f36Proc0)};
        f36Prog = finishProgram(procs, 1);
    }
    {
        ProcSource procs[] = {PROC(0, f37Proc0), PROC(5, f37Proc1), PROC(5, f37Proc2), PROC(1, f37Proc3)};
        f37Prog = finishProgram(procs, 4);
    }
    {
        ProcSource procs[] = {PROC(0, f38Proc0), PROC(0, f38Proc1)};
        f38Prog = finishProgram(procs, 2);
    }
    {
        ProcSource procs[] = {PROC(0, f39Proc0)};
        f39Prog = finishProgram(procs, 1);
    }
    {
        ProcSource procs[] = {PROC(0, f40Proc0), PROC(5, f40Proc1), PROC(1, f40Proc2)};
        f40Prog = finishProgram(procs, 3);
    }
    {
        ProcSource procs[] = {PROC(2, f41Proc0)};
        f41Prog = finishProgram(procs, 1);
    }
    {
        ProcSource procs[] = {PROC(4, f42Proc0)};
        f42Prog = finishProgram(procs, 1);
    }
    {
        ProcSource procs[] = {PROC(5, f43Proc0)};
        f43Prog = finishProgram(procs, 1);
    }
    {
        ProcSource procs[] = {PROC(6, f44Proc0)};
        f44Prog = finishProgram(procs, 1);
    }
    {
        ProcSource procs[] = {PROC(8, f45Proc0)};
        f45Prog = finishProgram(procs, 1);
    }
    {
        ProcSource procs[] = {PROC(6, f46Proc0), PROC(0, f46Proc1)};
        f46Prog = finishProgram(procs, 2);
    }
    {
        ProcSource procs[] = {PROC(6, f47Proc0)};
        f47Prog = finishProgram(procs, 1);
    }
    {
        ProcSource procs[] = {PROC(6, f48Proc0), PROC(0, f48Proc1)};
        f48Prog = finishProgram(procs, 2);
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

    {"nested LOOP-in-LOOP: sum of triangular numbers, n=3", &f28Prog, false, 10, 3},
    {"nested LOOP-in-LOOP: sum of triangular numbers, n=1", &f28Prog, false, 1, 1},
    {"nested LOOP-in-LOOP: sum of triangular numbers, n=0", &f28Prog, false, 0, 0},

    {"BR_TABLE nested inside LOOP body, n=4", &f29Prog, false, 64, 4},
    {"BR_TABLE nested inside LOOP body, n=5", &f29Prog, false, 69, 5},
    {"BR_TABLE nested inside LOOP body, n=0", &f29Prog, false, 0, 0},

    // argIn packs selector<<8 | n (see corpusLoopInBrTableProc0's own comment in corpus_programs.h).
    {"LOOP nested inside BR_TABLE case, selector=0,n=4", &f30Prog, false, 10, 4},
    {"LOOP nested inside BR_TABLE case, selector=1,n=4", &f30Prog, false, 12, 260},

    {"large BR_TABLE (N=20) with CALL, selector=7", &f31Prog, false, 1005, 7},
    {"large BR_TABLE (N=20) with CALL, selector=3", &f31Prog, false, 30, 3},
    {"large BR_TABLE (N=20) with CALL, selector=19", &f31Prog, false, 190, 19},

    {"deep operand stack, 24 live locals", &f32Prog, false, 300},

    {"acc-fold thrash inside a loop, n=5", &f33Prog, false, 3, 5},

    {"NEG on out-of-window (spilled) operand", &f34Prog, false, 0xFFFFFFFBu, 5},
    {"NOT on out-of-window (spilled) operand", &f35Prog, false, 0xFFFFFFFAu, 5},

    {"LOOP back-edge forced into long-branch form, n=1", &f36Prog, false, 0, 1},
    {"LOOP back-edge forced into long-branch form, n=50", &f36Prog, false, 0, 50},

    {"two-level savesLR/returnHelperFromStackReclaim chain", &f37Prog, false, 1561},

    {"nested TRAP unwinds instead of returning its code", &f38Prog, LANDING_TRAP, 754},
    {"TRAP in the entry procedure, five live locals deep", &f39Prog, LANDING_TRAP, 41},
    {"TRAP two levels down, out-of-window args below a pushed record", &f40Prog, LANDING_TRAP, 1000},

    // Multi-argument entry procedures. argIn is unused for all of these (the
    // count comes from Program::entryArgCount), so each names its own vector
    // in the `args` field instead.
    {"entry procedure, 2 args (in-window only)", &f41Prog, false, 0x12u, 0, entryArgs2},
    {"entry procedure, 4 args (fills the window exactly)", &f42Prog, false, 0x1234u, 0, entryArgs4},
    {"entry procedure, 5 args (one spilled word)", &f43Prog, false, 0x12345u, 0, entryArgs5},
    {"entry procedure, 6 args (two spilled words — order observable)", &f44Prog, false, 0x123456u, 0, entryArgs6},
    {"entry procedure, 8 args (post-wrap window phase)", &f45Prog, false, 0x12345678u, 0, entryArgs8},
    {"entry procedure, 6 args + nested CALL (savesLR reclaim)", &f46Prog, false, 0x123456u + 1000u, 0, entryArgs6},
    {"entry procedure, 6 args, TRAPs (savedSp precedes the arg pushes)", &f47Prog, LANDING_TRAP, 0x123456u, 0, entryArgs6},
    {"entry procedure, 6 args, callee TRAPs (unwind over live entry args)", &f48Prog, LANDING_TRAP, 4242u, 0, entryArgs6},
};
const uint32_t fixtureCount = sizeof(fixtures) / sizeof(fixtures[0]);
