// Expected halfwords are cross-checked against arm-none-eabi-as, not
// re-derived from the same formulas under test.
#include "Test.h"
#include "emitter.h"
#include "abi_strategy.h"
#include "imm_synth.h"

using namespace jitc;

TEST(stubSizeMatchesActualEmittedLength)
{
    uint16_t buf[8];
    Emitter e(buf, 8);
    emitPrologueStub(e);
    CHECK(e.halfwordCount() * 2 == STUB_SIZE);
}

TEST(prologueStubExactEncoding)
{
    uint16_t buf[8];
    Emitter e(buf, 8);
    emitPrologueStub(e);
    CHECK(e.halfwordCount() == 6);
    CHECK(buf[0] == 0x465B); // MOV r3, r11
    CHECK(buf[1] == 0x604B); // STR r3, [r1, #4]
    CHECK(buf[2] == 0x3301); // ADDS r3, #1
    CHECK(buf[3] == 0x469B); // MOV r11, r3
    CHECK(buf[4] == 0x447A); // ADD r2, r2, pc
    CHECK(buf[5] == 0x4710); // BX r2
}

TEST(packRecord)
{
    CHECK(packRecord(0, 1) == 0x00010000u);
    CHECK(packRecord(5, 2) == 0x00020005u);
    // procIdx=-1 truncates to u16 0xffff — the sentinel returnHelper's SXTH
    // fix resolves to "one slot behind the dispatch table base".
    CHECK(packRecord(0xffffu, 1) == 0x0001ffffu);
}

TEST(abiEmitCallFitsImm8CalleeIndex)
{
    // procIdx=0, calleeIndex=1, called right after the 6-halfword prologue
    // (preCallPc = STUB_SIZE) — i.e. the CALL site of a procedure whose own
    // entry instruction is itself a CALL.
    uint16_t buf[16];
    Emitter e(buf, 16);
    emitPrologueStub(e); // advances e.pc() to STUB_SIZE, matching abiEmitCall's real call site
    uint32_t before = e.halfwordCount();
    abiEmitCall(e, /*procIdx=*/0, /*calleeIndex=*/1);
    uint32_t n = e.halfwordCount() - before;
    CHECK(n == 7);
    CHECK(buf[before + 0] == 0x210F); // MOVS r1, #0x0F   (record low synth, byte 1 — k converged to 14)
    CHECK(buf[before + 1] == 0x0209); // LSLS r1, r1, #8
    CHECK(buf[before + 2] == 0x0209); // LSLS r1, r1, #8
    CHECK(buf[before + 3] == 0x2201); // MOVS r2, #1      (calleeIndex, fits imm8)
    CHECK(buf[before + 4] == 0x4653); // MOV r3, r10
    CHECK(buf[before + 5] == 0x681B); // LDR r3, [r3, #0] (callHelper)
    CHECK(buf[before + 6] == 0x4718); // BX r3
}

TEST(abiEmitCallConvergesForCalleeIndexNotFittingImm8)
{
    // calleeIndex=300 needs synthesizeImm32, which can shift the packed
    // record's own encoded length — exactly what the fixed-point search
    // exists for. The converged k (and thus the exact bytes) isn't
    // predictable without reimplementing the search, so this only checks
    // what must hold regardless of k: the search converges without
    // overflowing the buffer, and the sequence still ends in the fixed
    // movHi+ldr(callHelper)+bx tail.
    uint16_t buf[32];
    Emitter e(buf, 32);
    emitPrologueStub(e);
    uint32_t before = e.halfwordCount();
    abiEmitCall(e, /*procIdx=*/2, /*calleeIndex=*/300); // 300 > 0xff
    uint32_t n = e.halfwordCount() - before;
    CHECK(!e.overflowed());
    CHECK(n >= 3 + 1 + synthesizeImm32Length(300)); // record(>=1) + callee-index synth + fixed 3-instruction tail
    CHECK(buf[before + n - 3] == 0x4653); // MOV r3, r10
    CHECK(buf[before + n - 2] == 0x681B); // LDR r3, [r3, #0] (callHelper)
    CHECK(buf[before + n - 1] == 0x4718); // BX r3
}

TEST(abiEmitReturnLeafDispatchesToReturnHelperFromLr)
{
    uint16_t buf[4];
    Emitter e(buf, 4);
    abiEmitReturn(e, /*savesLR=*/false, /*initialSpilledCount=*/0);
    CHECK(e.halfwordCount() == 3);
    CHECK(buf[0] == 0x4653); // MOV r3, r10
    CHECK(buf[1] == 0x685B); // LDR r3, [r3, #4] (returnHelperFromLr, index 1)
    CHECK(buf[2] == 0x4718); // BX r3
}

TEST(abiEmitReturnOrdinaryNonLeafDispatchesToReturnHelperFromStack)
{
    // savesLR, but argCount <= WINDOW_SIZE (initialSpilledCount=0) — the
    // common non-leaf case, still a bare 3-instruction dispatch.
    uint16_t buf[4];
    Emitter e(buf, 4);
    abiEmitReturn(e, /*savesLR=*/true, /*initialSpilledCount=*/0);
    CHECK(e.halfwordCount() == 3);
    CHECK(buf[0] == 0x4653); // MOV r3, r10
    CHECK(buf[1] == 0x689B); // LDR r3, [r3, #8] (returnHelperFromStack, index 2)
    CHECK(buf[2] == 0x4718); // BX r3
}

TEST(abiEmitReturnDeepArgsNonLeafDispatchesToReturnHelperFromStackReclaim)
{
    // The rare case: savesLR and initialSpilledCount > 0 — neither bare
    // fetch variant can both retrieve the record and reclaim the original
    // out-of-window arguments below it, so this loads that one
    // per-procedure byte count into r2 and dispatches to the shared helper
    // that expects it there (index 7), instead of returnHelperFromStack.
    uint16_t buf[8];
    Emitter e(buf, 8);
    abiEmitReturn(e, /*savesLR=*/true, /*initialSpilledCount=*/3);
    CHECK(e.halfwordCount() == 4);
    CHECK(buf[0] == 0x220C); // MOVS r2, #12  (4 * initialSpilledCount)
    CHECK(buf[1] == 0x4653); // MOV r3, r10
    CHECK(buf[2] == 0x69DB); // LDR r3, [r3, #28] (returnHelperFromStackReclaim, index 7)
    CHECK(buf[3] == 0x4718); // BX r3
}

TEST(abiEmitReturnDeepArgsNonLeafSynthesizesLargeReclaimByteCount)
{
    // initialSpilledCount large enough that 4*initialSpilledCount doesn't
    // fit an 8-bit immediate — falls back to full synthesis instead of
    // silently truncating, same as abiEmitCall already does for a large
    // calleeIndex.
    uint16_t buf[16];
    Emitter e(buf, 16);
    abiEmitReturn(e, /*savesLR=*/true, /*initialSpilledCount=*/100); // 4*100 = 400 > 0xff
    CHECK(!e.overflowed());
    uint32_t n = e.halfwordCount();
    CHECK(n == synthesizeImm32Length(400) + 3);
    CHECK(buf[n - 3] == 0x4653); // MOV r3, r10
    CHECK(buf[n - 2] == 0x69DB); // LDR r3, [r3, #28] (returnHelperFromStackReclaim, index 7)
    CHECK(buf[n - 1] == 0x4718); // BX r3
}

TEST(abiEmitPrologueAddsPushLrOnlyWhenSavesLR)
{
    uint16_t buf1[8];
    Emitter e1(buf1, 8);
    abiEmitPrologue(e1, /*savesLR=*/false);
    CHECK(e1.halfwordCount() == 6); // just the stub

    uint16_t buf2[8];
    Emitter e2(buf2, 8);
    abiEmitPrologue(e2, /*savesLR=*/true);
    CHECK(e2.halfwordCount() == 7);
    CHECK(buf2[6] == 0xB500); // PUSH {lr}
}
