// The anchor test: compiles jit-armv6m/prototype/test/call.test.ts's
// program 1 ("single-argument call, entirely acc-passed") end to end and
// asserts the *entire* emitted halfword array for both procedures against
// literals hand-derived from the ARMv6-M encoding tables and independently
// cross-checked against arm-none-eabi-as — proving the whole pipeline
// (bytecode -> Emitter/Window/AccState/binops/abi_strategy) composes
// correctly, fast, without QEMU. The QEMU fixture for this same program
// (test/qemu/fixtures.cpp, fixture #1) is the behavioral proof that these
// bytes, executed for real against the real dispatch/eviction runtime,
// actually produce 42.
#include "Test.h"
#include "translate_proc.h"
#include "encode_instr.h"

using namespace jitc;

namespace {
// proc0 (argCount 0): CONST(37), call(1), RETURN
const Instr kProc0Body[] = {CONST(37), call(1), bare(Op::RETURN)};
// proc1 (argCount 1): LOAD(0), opImm(ADD, 5), RETURN
const Instr kProc1Body[] = {LOAD(0), opImm(Op::ADD, 5), bare(Op::RETURN)};
const uint32_t kArgCounts[] = {0, 1};

/** Instr[] fixtures still read like their rtl.ts source (instr.h's own
 *  header) — this is the one place that turns one into the raw wire bytes
 *  Proc::body now expects, mirroring how translateProc.ts's own tests let
 *  encodeBody() do the same before ever reaching translateProc(). */
Proc makeProc(uint32_t argCount, const Instr *body, uint32_t count, uint8_t *bytesOut, uint32_t bytesCap) {
    uint32_t len = encodeBody(body, count, bytesOut, bytesCap);
    return Proc{argCount, bytesOut, len};
}
}

TEST(TranslateProc0EntryProcedure)
{
    // proc0 makes a CALL, so it's non-leaf (savesLR): its prologue gains
    // push{lr}, and — because that shifts preCallPc by 2 bytes — the
    // record's own resume offset (k) converges one step further than the
    // pre-redesign encoding (offsetPlus1=19=0x13, not 17=0x11), and its
    // RETURN dispatches through returnHelperFromStack (index 2, offset 8)
    // instead of the old single returnHelper. argCount=0 keeps
    // initialSpilledCount at 0, so this is still the ordinary non-leaf
    // case, not the rare inline-pop-and-reclaim one.
    uint16_t buf[32];
    uint8_t bodyBytes[16];
    Proc proc = makeProc(0, kProc0Body, 3, bodyBytes, sizeof(bodyBytes));
    TranslateResult r = translateProc(proc, /*procIdx=*/0, kArgCounts, 2, buf, 32);

    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 18);

    const uint16_t expected[] = {
        0x465B, 0x604B, 0x3301, 0x469B, 0x447A, 0x4710, // prologue stub
        0xB500,                                          // PUSH {lr}  (savesLR — proc0 makes a CALL)
        0x2025,                                          // MOVS r0, #37  (CONST 37, stays pending until CALL flushes it)
        0x2113, 0x0209, 0x0209,                           // record synth: MOVS r1,#0x13; LSLS r1,r1,#8 (x2)
        0x2201,                                            // MOVS r2, #1  (calleeIndex=1, fits imm8)
        0x4653, 0x681B, 0x4718,                            // MOV r3,r10; LDR r3,[r3,#0]; BX r3  (callHelper)
        0x4653, 0x689B, 0x4718,                            // MOV r3,r10; LDR r3,[r3,#8]; BX r3  (returnHelperFromStack, index 2)
    };
    for(uint32_t i = 0; i < r.halfwordCount; i++) CHECK(buf[i] == expected[i]);
}

TEST(TranslateProc1Callee)
{
    // §16 items 13/14's own last-argument fold: proc1's only reference to
    // slot 0 (its own last argument) is body[0]'s own LOAD, so the
    // callee-side prologue elides both the unconditional flush into
    // physReg(0) *and* that LOAD — the argument stays PENDING straight in
    // ACC_REG (already where it arrives) instead of round-tripping
    // through r7 first. One fewer instruction than the pre-fold encoding.
    uint16_t buf[32];
    uint8_t bodyBytes[16];
    Proc proc = makeProc(1, kProc1Body, 3, bodyBytes, sizeof(bodyBytes));
    TranslateResult r = translateProc(proc, /*procIdx=*/1, kArgCounts, 2, buf, 32);

    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 10);

    const uint16_t expected[] = {
        0x465B, 0x604B, 0x3301, 0x469B, 0x447A, 0x4710, // prologue stub
        0x1D40,                                            // ADDS r0, r0, #5  (LOAD(0)+opImm(ADD,5): LOAD elided, folded straight into acc)
        0x4653, 0x685B, 0x4718,                             // returnHelper tail
    };
    for(uint32_t i = 0; i < r.halfwordCount; i++) CHECK(buf[i] == expected[i]);
}

TEST(OverflowIsReportedRatherThanOverrunningTheBuffer)
{
    uint16_t buf[4]; // too small for even the 6-halfword prologue alone
    uint8_t bodyBytes[16];
    Proc proc = makeProc(0, kProc0Body, 3, bodyBytes, sizeof(bodyBytes));
    TranslateResult r = translateProc(proc, 0, kArgCounts, 2, buf, 4);
    CHECK(r.overflowed);
}

// The tests below exercise LOOP/BR_TABLE/comparisons-as-values/unary ops/
// last-argument-fold/block-nesting-overflow through translateProc()'s own
// main loop — end-to-end behavioral correctness for all of these is
// already proven on real QEMU (test/qemu/fixtures.cpp), but that binary
// isn't gcov-instrumented, so none of it shows up in this host suite's
// own coverage without a matching host-level test. Sizes below are the
// actual measured halfwordCount for each body (a structural regression
// guard, not a full hex dump — that's the QEMU fixtures' own job).

TEST(LoopClosesNormallyViaBlockEndBackEdge)
{
    // Real back-edge, not a terminator-closed body (loop.test.ts's own
    // degenerate case only ever exercises closeLoopBodyViaTerminator;
    // this exercises closeBlockEnd's own LoopCond->LoopBody transition
    // and the unconditional back-edge branch it emits).
    const Instr body[] = {
        CONST(3), PUSH(),
        bare(Op::LOOP),
            LOAD(0),
        bare(Op::BLOCK_END),
            LOAD(0), opImm(Op::SUB, 1), STORE(0),
        bare(Op::BLOCK_END),
        bare(Op::RETURN),
    };
    uint32_t argCounts[] = {0};
    uint8_t bodyBytes[32];
    Proc proc = makeProc(0, body, sizeof(body) / sizeof(body[0]), bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 17);
}

TEST(BrTableJumpTableHelperViaFullPipeline)
{
    // N > 2 — openBrTableJump + the shared emitBrTableHelper, neither
    // reachable through the N <= 2 fusion path this file's other tests
    // (and blocks.ts's own unit tests) exercise.
    const Instr body[] = {
        CONST(1), brTable(3),
            CONST(10), bare(Op::BLOCK_END),
            CONST(20), bare(Op::BLOCK_END),
            CONST(30), bare(Op::BLOCK_END),
        bare(Op::RETURN),
    };
    uint32_t argCounts[] = {0};
    uint8_t bodyBytes[32];
    Proc proc = makeProc(0, body, sizeof(body) / sizeof(body[0]), bodyBytes, sizeof(bodyBytes));
    uint16_t buf[64];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 64);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 34);
}

TEST(ComparisonFusesIntoBrTableGuard)
{
    const Instr body[] = {
        CONST(5), opImm(Op::GT_U, 3), brTable(2),
            CONST(1), bare(Op::BLOCK_END),
            CONST(2), bare(Op::BLOCK_END),
        bare(Op::RETURN),
    };
    uint32_t argCounts[] = {0};
    uint8_t bodyBytes[32];
    Proc proc = makeProc(0, body, sizeof(body) / sizeof(body[0]), bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 15);
}

TEST(ComparisonMaterializesAsOrdinaryValue)
{
    // No BR_TABLE/LOOP-exit right after it — §16 item 8's own
    // materializeComparison path, not fusion.
    const Instr body[] = {CONST(5), opImm(Op::GT_U, 3), bare(Op::RETURN)};
    uint32_t argCounts[] = {0};
    uint8_t bodyBytes[16];
    Proc proc = makeProc(0, body, sizeof(body) / sizeof(body[0]), bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 15);
}

TEST(NegViaFullPipeline)
{
    const Instr body[] = {CONST(5), bare(Op::NEG), bare(Op::RETURN)};
    uint32_t argCounts[] = {0};
    uint8_t bodyBytes[8];
    Proc proc = makeProc(0, body, sizeof(body) / sizeof(body[0]), bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 11);
}

TEST(ClzHelperViaFullPipeline)
{
    // Reaches emitUnary's own placeholderBL path plus translateProc's own
    // helper-site collection/patch step — emitClzHelper's own internals
    // are unit-tested directly (test_unaryops.cpp), this is the
    // caller-side wiring around it.
    const Instr body[] = {CONST(5), bare(Op::CLZ), bare(Op::RETURN)};
    uint32_t argCounts[] = {0};
    uint8_t bodyBytes[8];
    Proc proc = makeProc(0, body, sizeof(body) / sizeof(body[0]), bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 24);
}

TEST(LastArgumentFoldFallsBackToEagerFlushWhenReferencedTwice)
{
    // Two references to the last argument's own slot (neither of them
    // "body[0]'s own LOAD alone") — §16 items 13/14's own fallback: the
    // unconditional flush into physReg(argCount-1), not the elision this
    // file's own TranslateProc1Callee test exercises.
    const Instr body[] = {LOAD(0), LOAD(0), opReg(Op::ADD, 0), bare(Op::RETURN)};
    uint32_t argCounts[] = {1};
    uint8_t bodyBytes[16];
    Proc proc = makeProc(1, body, sizeof(body) / sizeof(body[0]), bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 11);
}

TEST(CaseClosesViaTerminatorThroughFullPipeline)
{
    // A non-last case closing via bare RETURN, dispatched through
    // translateProc's own closeFrameForTerminator — blocks.h's own
    // closeCaseViaTerminator is unit-tested directly (test_blocks.cpp),
    // this is the caller-side switch dispatch around it.
    const Instr body[] = {
        CONST(5), opImm(Op::GT_U, 3), brTable(2),
            CONST(1), bare(Op::RETURN),
            CONST(2), bare(Op::BLOCK_END),
        bare(Op::RETURN),
    };
    uint32_t argCounts[] = {0};
    uint8_t bodyBytes[32];
    Proc proc = makeProc(0, body, sizeof(body) / sizeof(body[0]), bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 17);
}

// The tests below close the remaining coverage-analysis gaps in
// translate_proc.cpp's main dispatch switch: POP, TRAP (both at the top
// level and inside an open block), out-of-window LOAD/STORE, a STORE not
// preceded by a foldable producer, CONST's own large-immediate/fold-into-
// STORE paths, REG_REG (opRegWriteback) both in- and out-of-window,
// POP_ACC/PEEK_PEEK stack combos, a LOOP body that closes via a bare
// RETURN instead of BLOCK_END (isa-core.md §7.2's own leniency), and
// REVBITS's own helper-patch tail (translateProc's own emitRevbitsHelper
// call, mirrored after ClzHelperViaFullPipeline above). None of these are
// exotic — they're the ordinary "not the fused/aligned case" side of
// paths this file's other tests already cover the *fused* side of.

TEST(PopThroughFullPipeline)
{
    const Instr body[] = {CONST(5), PUSH(), POP(), bare(Op::RETURN)};
    uint32_t argCounts[] = {0};
    uint8_t bodyBytes[16];
    Proc proc = makeProc(0, body, sizeof(body) / sizeof(body[0]), bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 11);
}

TEST(TrapAtTopLevel)
{
    const Instr body[] = {trapInstr(3)};
    uint32_t argCounts[] = {0};
    uint8_t bodyBytes[8];
    Proc proc = makeProc(0, body, 1, bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 14);
}

TEST(TrapInsideCaseClosesItAndContinuesToNextCase)
{
    // Same shape as CaseClosesViaTerminatorThroughFullPipeline above, but
    // with TRAP as the terminator instead of RETURN — closeFrameForTerminator's
    // own "frame != nullptr" guard around TRAP specifically is a distinct
    // source line from RETURN's identical-shaped guard.
    const Instr body[] = {
        CONST(5), opImm(Op::GT_U, 3), brTable(2),
            CONST(1), trapInstr(9),
            CONST(2), bare(Op::BLOCK_END),
        bare(Op::RETURN),
    };
    uint32_t argCounts[] = {0};
    uint8_t bodyBytes[32];
    Proc proc = makeProc(0, body, sizeof(body) / sizeof(body[0]), bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 21);
}

TEST(LoadFromOutOfWindowSlot)
{
    // argCount=5 > WINDOW_SIZE(4) — slot 0 is spilled from procedure
    // entry, so LOAD(0) must reload it via ldrSp instead of reading a
    // window register directly.
    const Instr body[] = {LOAD(0), bare(Op::RETURN)};
    uint32_t argCounts[] = {5};
    uint8_t bodyBytes[8];
    Proc proc = makeProc(5, body, 2, bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 11);
}

TEST(StoreStandaloneInWindowWhenNotPrecededByAFoldableProducer)
{
    // POP doesn't peek-fold a following STORE (only CONST/LOAD/unary/
    // arithmetic do) — this is the switch's own bare `case Op::STORE`
    // dispatch, not the far-more-common fold path this file's other
    // tests exercise via peekStoreFold.
    const Instr body[] = {CONST(9), PUSH(), POP(), STORE(0), bare(Op::RETURN)};
    uint32_t argCounts[] = {1};
    uint8_t bodyBytes[16];
    Proc proc = makeProc(1, body, sizeof(body) / sizeof(body[0]), bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 14);
}

TEST(StoreStandaloneOutOfWindowAndLastArgFoldRefCountZero)
{
    // Two gaps at once: peekStoreFold never folds an out-of-window STORE
    // target regardless of what precedes it (fold.reg is a physical
    // register, meaningless for a spilled slot), so NEG's own STORE(0)
    // here always takes the standalone out-of-window path. Separately,
    // argCount=5's own last argument (slot 4) is never referenced by this
    // body at all — translateProc's own last-argument-fold scan finds
    // refCount==0 and leaves the incoming value as a plain ACC_REG
    // producer, the one case last-arg-fold's own dedicated tests above
    // don't otherwise reach (they all reference the last slot).
    const Instr body[] = {bare(Op::NEG), STORE(0), bare(Op::RETURN)};
    uint32_t argCounts[] = {5};
    uint8_t bodyBytes[8];
    Proc proc = makeProc(5, body, 3, bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 12);
}

TEST(ConstTooLargeForImm8SynthesizesInsteadOfStayingPending)
{
    const Instr body[] = {CONST(1000), bare(Op::RETURN)};
    uint32_t argCounts[] = {0};
    uint8_t bodyBytes[8];
    Proc proc = makeProc(0, body, 2, bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 12);
}

TEST(ConstFoldsDirectlyIntoAFollowingStore)
{
    const Instr body[] = {CONST(5), STORE(0), bare(Op::RETURN)};
    uint32_t argCounts[] = {1};
    uint8_t bodyBytes[8];
    Proc proc = makeProc(1, body, 3, bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 12);
}

TEST(RegRegOutOfWindowWritesBackToStackAfterComputing)
{
    // REG_REG (opRegWriteback) with an out-of-window target: the operand
    // reload (shared with REG_ACC) and REG_REG's own SCRATCH_REG-as-dest
    // plus explicit str-back are both otherwise only exercised indirectly
    // through binops.cpp's own lower-level tests, never through this
    // file's real dispatch.
    const Instr body[] = {opRegWriteback(Op::ADD, 0), CONST(1), bare(Op::RETURN)};
    uint32_t argCounts[] = {5};
    uint8_t bodyBytes[16];
    Proc proc = makeProc(5, body, 3, bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 14);
}

TEST(RegRegInWindowWritesBackToItsOwnRegister)
{
    const Instr body[] = {CONST(5), PUSH(), opRegWriteback(Op::ADD, 0), LOAD(0), bare(Op::RETURN)};
    uint32_t argCounts[] = {0};
    uint8_t bodyBytes[16];
    Proc proc = makeProc(0, body, sizeof(body) / sizeof(body[0]), bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 12);
}

TEST(PopAccStackComboThroughFullPipeline)
{
    const Instr body[] = {CONST(3), PUSH(), CONST(2), opStack(Op::ADD, Combo::POP_ACC), bare(Op::RETURN)};
    uint32_t argCounts[] = {0};
    uint8_t bodyBytes[16];
    Proc proc = makeProc(0, body, sizeof(body) / sizeof(body[0]), bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 11);
}

TEST(PeekPeekStackComboThroughFullPipeline)
{
    const Instr body[] = {CONST(3), PUSH(), CONST(6), opStack(Op::AND, Combo::PEEK_PEEK), POP(), bare(Op::RETURN)};
    uint32_t argCounts[] = {0};
    uint8_t bodyBytes[16];
    Proc proc = makeProc(0, body, sizeof(body) / sizeof(body[0]), bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 14);
}

TEST(LoopBodyClosesViaTerminatorInsteadOfBlockEnd)
{
    // isa-core.md §7.2's own leniency, mirroring fixture 8
    // (test/qemu/fixtures.cpp) at the host level: an empty condition
    // sub-block (testAccNonzero(arg) is the whole test), then a body that
    // closes via a bare RETURN rather than BLOCK_END —
    // closeFrameForTerminator's own LoopBody dispatch (blocks.h's
    // closeLoopBodyViaTerminator), not the Case dispatch the other
    // terminator-close tests exercise. Note there is no way to reach the
    // *other* half of that same dispatch with frame.kind == LoopCond —
    // closeLoopBodyViaTerminator itself asserts frame.kind == LoopBody
    // (blocks.cpp), because a loop's own condition sub-block can only
    // ever close via BLOCK_END (isa-core.md's block grammar has no
    // terminator-producing construct that isn't itself a nested frame of
    // its own, e.g. a BR_TABLE case) — genuinely unreachable, not merely
    // untested, which is why that assert carries its own GCOV_EXCL_LINE.
    const Instr body[] = {
        bare(Op::LOOP), bare(Op::BLOCK_END),
        CONST(42), bare(Op::RETURN),
        CONST(999), bare(Op::RETURN),
    };
    uint32_t argCounts[] = {0};
    uint8_t bodyBytes[16];
    Proc proc = makeProc(1, body, sizeof(body) / sizeof(body[0]), bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 18);
}

TEST(RevbitsHelperViaFullPipeline)
{
    // Mirrors ClzHelperViaFullPipeline above, for REVBITS's own separate
    // helper-site list/patch tail in translateProc() itself.
    const Instr body[] = {CONST(1), bare(Op::REVBITS), bare(Op::RETURN)};
    uint32_t argCounts[] = {0};
    uint8_t bodyBytes[8];
    Proc proc = makeProc(0, body, 3, bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 22);
}

TEST(ComparisonImmediatelyBeforeBrTableJumpTableDoesNotFuse)
{
    // Fusion only applies for BR_TABLE N<=2 (if/if-else) — a comparison
    // right before a genuine N>2 jump-table selector must materialize
    // as an ordinary 0/1 value instead, exercising the "hasLookahead but
    // op doesn't qualify" side of fusesIntoBrTable's own condition.
    const Instr body[] = {
        LOAD(0), opImm(Op::GT_U, 3), brTable(3),
            CONST(10), bare(Op::BLOCK_END),
            CONST(20), bare(Op::BLOCK_END),
            CONST(30), bare(Op::BLOCK_END),
        bare(Op::RETURN),
    };
    uint32_t argCounts[] = {1};
    uint8_t bodyBytes[32];
    Proc proc = makeProc(1, body, sizeof(body) / sizeof(body[0]), bodyBytes, sizeof(bodyBytes));
    uint16_t buf[64];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 64);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 38);
}

TEST(LoopConditionClosesViaAnExplicitFusedComparison)
{
    // The other half of LoopClosesNormallyViaBlockEndBackEdge's own
    // sibling: that test's condition relies on testAccNonzero's fallback
    // (a bare LOAD with no comparison) — this one has a real comparison
    // immediately before the condition's own BLOCK_END, hitting
    // fusesIntoLoopExit rather than the fallback.
    const Instr body[] = {
        CONST(3), PUSH(),
        bare(Op::LOOP),
            LOAD(0), opImm(Op::GT_S, 0),
        bare(Op::BLOCK_END),
            LOAD(0), opImm(Op::SUB, 1), STORE(0),
        bare(Op::BLOCK_END),
        bare(Op::RETURN),
    };
    uint32_t argCounts[] = {0};
    uint8_t bodyBytes[32];
    Proc proc = makeProc(0, body, sizeof(body) / sizeof(body[0]), bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 16);
}

TEST(ComparisonMaterializedResultFoldsDirectlyIntoAFollowingStore)
{
    // materializeComparison's own fold.reg>=0 path (the comparison-as-
    // value analog of CONST/arithmetic's own store-fold) — every other
    // comparison-as-value test in this file has the result land in
    // ACC_REG (fold.reg<0) instead.
    const Instr body[] = {CONST(5), opImm(Op::GT_U, 3), STORE(0), bare(Op::RETURN)};
    uint32_t argCounts[] = {1};
    uint8_t bodyBytes[16];
    Proc proc = makeProc(1, body, sizeof(body) / sizeof(body[0]), bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 17);
}

TEST(LastArgumentFoldEagerlyFlushesWhenBodyStartReferenceIsNotALoad)
{
    // The last-arg-fold's own LOAD-elision fast path is keyed on the
    // first reference being a body-start *LOAD* specifically — a
    // REG_ACC read (opReg) at the very same body-start position must
    // still take the eager-flush path, since it reads via physReg(slot)
    // rather than acc's own pending value.
    const Instr body[] = {opReg(Op::ADD, 1), bare(Op::RETURN)};
    uint32_t argCounts[] = {2};
    uint8_t bodyBytes[8];
    Proc proc = makeProc(2, body, 2, bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 11);
}

TEST(BlockNestingBeyondMaxReportsOverflowInsteadOfRecursingFurther)
{
    // 40 nested LOOPs — past MAX_BLOCK_NESTING(32) — translateBody bails
    // out (nestingExceeded) before ever processing any of them; the real
    // hardware counterpart of a bound the prototype's own JS call stack
    // never needed enforcing (translate_proc.cpp's own header comment).
    Instr body[41];
    for(int i = 0; i < 40; i++) body[i] = bare(Op::LOOP);
    body[40] = bare(Op::BLOCK_END);
    uint32_t argCounts[] = {0};
    uint8_t bodyBytes[64];
    Proc proc = makeProc(0, body, 41, bodyBytes, sizeof(bodyBytes));
    uint16_t buf[16];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 16);
    CHECK(r.overflowed);
}
