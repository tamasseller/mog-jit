// Expected halfwords are cross-checked against arm-none-eabi-as, not
// re-derived from the same formulas under test.
#include "Test.h"
#include "assembler.h"
#include "abi_strategy.h"
#include "imm_synth.h"

#include "host_runtime_support.h"

using namespace jitc;

// The 32-bit word a pooled literal load at halfword index site actually
// reaches, resolved exactly the way the hardware does — Align(pc,4) with
// pc being the site's own address + 4. Mirrors test_translate_proc.cpp's
// own loadedWord helper.
static uint32_t loadedWord(const uint16_t *buf, uint32_t site)
{
    uint16_t off;
    bool ok = ArmV6M::getLiteralOffset(buf[site], off);
    assert(ok); // GCOV_EXCL_LINE — only on a failing test's own bad site
    (void)ok;
    uint32_t target = ((site * 2 + 4) & ~3u) + off * 4u;
    return (uint32_t)buf[target / 2] | ((uint32_t)buf[target / 2 + 1] << 16);
}

TEST(stubSizeMatchesActualEmittedLength)
{
    TestAssembler e_ta(8);
    Assembler &e = e_ta.a;
    const uint16_t *buf = e_ta.code();
    emitPrologueStub(e);
    CHECK(e.halfwordCount() * 2 == STUB_SIZE);
}

TEST(prologueStubExactEncoding)
{
    TestAssembler e_ta(8);
    Assembler &e = e_ta.a;
    const uint16_t *buf = e_ta.code();
    emitPrologueStub(e);
    // Just the pc-relative resume jump: the LRU stamp that used to head this
    // is in runtime.S's callHelper/returnHelperTail now.
    CHECK(e.halfwordCount() == 2);
    CHECK(buf[0] == ArmV6M::add(ArmV6M::AnyReg(2), ArmV6M::AnyReg(15))); // ADD r2, r2, pc
    CHECK(buf[1] == ArmV6M::bx(ArmV6M::AnyReg(2))); // BX r2
}

TEST(packRecord)
{
    CHECK(packRecord(0, 1) == 0x00010000u);
    CHECK(packRecord(5, 2) == 0x00020005u);
    // procIdx=-1 truncates to u16 0xffff — the sentinel returnHelper's SXTH
    // fix resolves to "one slot behind the dispatch table base".
    CHECK(packRecord(0xffffu, 1) == 0x0001ffffu);
}

TEST(abiEmitCallFitsImm8CalleeIndexIsAFixedFiveHalfwordSequence)
{
    // procIdx=0, calleeIndex=1, called right after the 2-halfword prologue
    // (preCallPc = STUB_SIZE) — i.e. the CALL site of a procedure whose own
    // entry instruction is itself a CALL. The call record always costs
    // exactly one halfword (materializeImm32's two-instruction-sequence
    // forms are disallowed here, so it's a bare MOVS or a pooled
    // placeholder, never wider), so the sequence's own length is a
    // closed-form constant — record(1) + calleeIndex(1, fits imm8) +
    // movHi+ldr(callHelper)+bx(3) = 5 — rather than something a
    // fixed-point search has to converge on.
    TestAssembler e_ta(16);
    Assembler &e = e_ta.a;
    const uint16_t *buf = e_ta.code();
    emitPrologueStub(e); // advances e.pc() to STUB_SIZE, matching abiEmitCall's real call site
    uint32_t before = e.halfwordCount();
    abiEmitCall(e, /*procIdx=*/0, /*calleeIndex=*/1);
    uint32_t n = e.halfwordCount() - before;
    CHECK(n == 5);
    CHECK(ArmV6M::isLiteralAccess(buf[before + 0])); // the record's own pooled site
    CHECK(buf[before + 1] == ArmV6M::movs(ArmV6M::LoReg(2), ArmV6M::Imm<8>(1))); // MOVS r2, #1      (calleeIndex, fits imm8)
    CHECK(buf[before + 2] == ArmV6M::mov(ArmV6M::AnyReg(3), ArmV6M::AnyReg(10))); // MOV r3, r10
    CHECK(buf[before + 3] == ArmV6M::ldr(ArmV6M::LoReg(3), ArmV6M::LoReg(3), ArmV6M::Uoff<2, 5>(0))); // LDR r3, [r3, #0] (callHelper)
    CHECK(buf[before + 4] == ArmV6M::bx(ArmV6M::AnyReg(3))); // BX r3

    // k has a closed form now: (preCallPc - STUB_SIZE) + 5*2 = 0 + 10 =
    // 10, so the pooled record packs procIdx=0 with offsetPlus1=11 --
    // confirmed by flushing and reading the word back, not by re-deriving
    // the formula under test.
    e.finalize(0);
    CHECK(loadedWord(buf, before + 0) == packRecord(0, 11));
}

TEST(abiEmitCallForcePoolsACalleeIndexNotFittingImm8Too)
{
    // calleeIndex=300 doesn't fit imm8, so it's force-pooled exactly like
    // the record — both operands cost exactly one halfword at the call
    // site regardless of value, keeping the sequence's own length a true
    // constant (still 5) even here.
    TestAssembler e_ta(16);
    Assembler &e = e_ta.a;
    const uint16_t *buf = e_ta.code();
    emitPrologueStub(e);
    uint32_t before = e.halfwordCount();
    abiEmitCall(e, /*procIdx=*/2, /*calleeIndex=*/300);
    uint32_t n = e.halfwordCount() - before;
    CHECK(n == 5);
    CHECK(ArmV6M::isLiteralAccess(buf[before + 0])); // record
    CHECK(ArmV6M::isLiteralAccess(buf[before + 1])); // calleeIndex, also pooled
    CHECK(buf[before + 2] == ArmV6M::mov(ArmV6M::AnyReg(3), ArmV6M::AnyReg(10))); // MOV r3, r10
    CHECK(buf[before + 3] == ArmV6M::ldr(ArmV6M::LoReg(3), ArmV6M::LoReg(3), ArmV6M::Uoff<2, 5>(0))); // LDR r3, [r3, #0] (callHelper)
    CHECK(buf[before + 4] == ArmV6M::bx(ArmV6M::AnyReg(3))); // BX r3

    e.finalize(0);
    CHECK(loadedWord(buf, before + 0) == packRecord(2, 11));
    CHECK(loadedWord(buf, before + 1) == 300u);
}

TEST(abiEmitReturnLeafDispatchesToReturnHelperFromLr)
{
    TestAssembler e_ta(4);
    Assembler &e = e_ta.a;
    const uint16_t *buf = e_ta.code();
    abiEmitReturn(e, /*savesLR=*/false, /*initialSpilledCount=*/0);
    CHECK(e.halfwordCount() == 3);
    CHECK(buf[0] == ArmV6M::mov(ArmV6M::AnyReg(3), ArmV6M::AnyReg(10))); // MOV r3, r10
    CHECK(buf[1] == ArmV6M::ldr(ArmV6M::LoReg(3), ArmV6M::LoReg(3), ArmV6M::Uoff<2, 5>(4))); // LDR r3, [r3, #4] (returnHelperFromLr, index 1)
    CHECK(buf[2] == ArmV6M::bx(ArmV6M::AnyReg(3))); // BX r3
}

TEST(abiEmitReturnOrdinaryNonLeafDispatchesToReturnHelperFromStack)
{
    // savesLR, but argCount <= WINDOW_SIZE (initialSpilledCount=0) — the
    // common non-leaf case, still a bare 3-instruction dispatch.
    TestAssembler e_ta(4);
    Assembler &e = e_ta.a;
    const uint16_t *buf = e_ta.code();
    abiEmitReturn(e, /*savesLR=*/true, /*initialSpilledCount=*/0);
    CHECK(e.halfwordCount() == 3);
    CHECK(buf[0] == ArmV6M::mov(ArmV6M::AnyReg(3), ArmV6M::AnyReg(10))); // MOV r3, r10
    CHECK(buf[1] == ArmV6M::ldr(ArmV6M::LoReg(3), ArmV6M::LoReg(3), ArmV6M::Uoff<2, 5>(8))); // LDR r3, [r3, #8] (returnHelperFromStack, index 2)
    CHECK(buf[2] == ArmV6M::bx(ArmV6M::AnyReg(3))); // BX r3
}

TEST(abiEmitReturnDeepArgsNonLeafDispatchesToReturnHelperFromStackReclaim)
{
    // The rare case: savesLR and initialSpilledCount > 0 — neither bare
    // fetch variant can both retrieve the record and reclaim the original
    // out-of-window arguments below it, so this loads that one
    // per-procedure byte count into r2 and dispatches to the shared helper
    // that expects it there (index 7), instead of returnHelperFromStack.
    TestAssembler e_ta(8);
    Assembler &e = e_ta.a;
    const uint16_t *buf = e_ta.code();
    abiEmitReturn(e, /*savesLR=*/true, /*initialSpilledCount=*/3);
    CHECK(e.halfwordCount() == 4);
    CHECK(buf[0] == ArmV6M::movs(ArmV6M::LoReg(2), ArmV6M::Imm<8>(12))); // MOVS r2, #12  (4 * initialSpilledCount)
    CHECK(buf[1] == ArmV6M::mov(ArmV6M::AnyReg(3), ArmV6M::AnyReg(10))); // MOV r3, r10
    CHECK(buf[2] == ArmV6M::ldr(ArmV6M::LoReg(3), ArmV6M::LoReg(3), ArmV6M::Uoff<2, 5>(28))); // LDR r3, [r3, #28] (returnHelperFromStackReclaim, index 7)
    CHECK(buf[3] == ArmV6M::bx(ArmV6M::AnyReg(3))); // BX r3
}

TEST(abiEmitReturnDeepArgsNonLeafSynthesizesLargeReclaimByteCount)
{
    // initialSpilledCount large enough that 4*initialSpilledCount doesn't
    // fit an 8-bit immediate — falls back to materializeImm32 instead of
    // silently truncating, same as abiEmitCall already does for a large
    // calleeIndex. Unlike the call record, this value has no self-
    // reference to its own encoded length, so it's free to use
    // materializeImm32's full repertoire (unlike abiEmitCall's own calls,
    // which disallow the two-instruction-sequence forms) — 400 = 25 << 4,
    // so it synthesizes inline via the shift-trick (MOVS + LSLS, 2
    // halfwords) rather than pooling.
    TestAssembler e_ta(16);
    Assembler &e = e_ta.a;
    const uint16_t *buf = e_ta.code();
    abiEmitReturn(e, /*savesLR=*/true, /*initialSpilledCount=*/100); // 4*100 = 400 > 0xff
    uint32_t n = e.halfwordCount();
    CHECK(n == 2 + 3); // MOVS + LSLS (400 = 25 << 4), then MOV/LDR/BX
    CHECK(buf[n - 3] == ArmV6M::mov(ArmV6M::AnyReg(3), ArmV6M::AnyReg(10))); // MOV r3, r10
    CHECK(buf[n - 2] == ArmV6M::ldr(ArmV6M::LoReg(3), ArmV6M::LoReg(3), ArmV6M::Uoff<2, 5>(28))); // LDR r3, [r3, #28] (returnHelperFromStackReclaim, index 7)
    CHECK(buf[n - 1] == ArmV6M::bx(ArmV6M::AnyReg(3))); // BX r3
}

TEST(abiEmitPrologueAddsPushLrOnlyWhenSavesLR)
{
    TestAssembler e1_ta(8);
    Assembler &e1 = e1_ta.a;
    const uint16_t *buf1 = e1_ta.code();
    abiEmitPrologue(e1, /*savesLR=*/false);
    CHECK(e1.halfwordCount() == 2); // just the stub

    TestAssembler e2_ta(8);
    Assembler &e2 = e2_ta.a;
    const uint16_t *buf2 = e2_ta.code();
    abiEmitPrologue(e2, /*savesLR=*/true);
    CHECK(e2.halfwordCount() == 3);
    CHECK(buf2[2] == ArmV6M::pushWithLr(ArmV6M::LoRegs{0})); // PUSH {lr}
}
