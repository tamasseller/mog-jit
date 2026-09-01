// jit-armv6m/compiler/test — proc_scan.h's boundary-finding walk, cross-
// checked against the same shapes packages/machine/src/bytecode.ts's
// decodeProcBody handles (including the §8.5 bare-terminator case-close
// wrinkle that decoder was missing before this port started).
#include "Test.h"
#include "proc_scan.h"
#include "encode_instr.h"
#include "instr.h"

using namespace jitc;

TEST(ScanProcBodyFindsAFlatBodysOwnEnd)
{
    const Instr body[] = {CONST(1), bare(Op::RETURN)};
    uint8_t bytes[16];
    uint32_t len = encodeBody(body, 2, bytes, sizeof(bytes));

    BodyScanResult r = scanProcBody(bytes, len, 0);
    CHECK(r.ok);
    CHECK(r.bodyBytes == len);
    CHECK(!r.needsLRSave);
}

TEST(ScanProcBodyStopsAtThisProceduresOwnEndNotPastIt)
{
    // Two procedures back to back, no separator — the scan must stop
    // exactly at proc 0's own RETURN, not run into proc 1's bytes.
    const Instr proc0[] = {CONST(1), bare(Op::RETURN)};
    const Instr proc1[] = {CONST(99), bare(Op::RETURN)};
    uint8_t bytes[32];
    uint32_t len0 = encodeBody(proc0, 2, bytes, sizeof(bytes));

    uint32_t outLen = len0;
    encodeInstr(proc1[0], bytes, outLen, sizeof(bytes));
    encodeInstr(proc1[1], bytes, outLen, sizeof(bytes));

    BodyScanResult r = scanProcBody(bytes, outLen, 0);
    CHECK(r.ok);
    CHECK(r.bodyBytes == len0);
}

TEST(ScanProcBodyDetectsCallAsNeedingLRSave)
{
    const Instr body[] = {call(3), bare(Op::RETURN)};
    uint8_t bytes[16];
    uint32_t len = encodeBody(body, 2, bytes, sizeof(bytes));

    BodyScanResult r = scanProcBody(bytes, len, 0);
    CHECK(r.ok);
    CHECK(r.needsLRSave);
}

TEST(ScanProcBodyDetectsBrTableAbove2AsNeedingLRSave)
{
    const Instr body[] = {CONST(0), brTable(3), CONST(1), bare(Op::BLOCK_END), CONST(2), bare(Op::BLOCK_END), CONST(3), bare(Op::BLOCK_END), bare(Op::RETURN)};
    uint8_t bytes[32];
    uint32_t len = encodeBody(body, 9, bytes, sizeof(bytes));

    BodyScanResult r = scanProcBody(bytes, len, 0);
    CHECK(r.ok);
    CHECK(r.needsLRSave);
}

TEST(ScanProcBodyBrTableOfTwoDoesNotNeedLRSave)
{
    const Instr body[] = {CONST(0), brTable(2), CONST(1), bare(Op::BLOCK_END), CONST(2), bare(Op::BLOCK_END), bare(Op::RETURN)};
    uint8_t bytes[32];
    uint32_t len = encodeBody(body, 7, bytes, sizeof(bytes));

    BodyScanResult r = scanProcBody(bytes, len, 0);
    CHECK(r.ok);
    CHECK(!r.needsLRSave);
}

TEST(ScanProcBodyDetectsClzAndRevbitsAsNeedingLRSave)
{
    const Instr clzBody[] = {bare(Op::CLZ), bare(Op::RETURN)};
    uint8_t bytes[16];
    uint32_t len = encodeBody(clzBody, 2, bytes, sizeof(bytes));
    CHECK(scanProcBody(bytes, len, 0).needsLRSave);

    const Instr revBody[] = {bare(Op::REVBITS), bare(Op::RETURN)};
    len = encodeBody(revBody, 2, bytes, sizeof(bytes));
    CHECK(scanProcBody(bytes, len, 0).needsLRSave);
}

TEST(ScanProcBodyLoopBodyClosedByBareTerminatorFindsTheOuterTail)
{
    // isa-core.md §7.2's own allowance — the loop body's own bare RETURN
    // closes just the loop, not the procedure; the outer scope's own tail
    // (its cond-false exit path) still follows.
    const Instr body[] = {
        CONST(1), bare(Op::LOOP), bare(Op::BLOCK_END),
        CONST(42), bare(Op::RETURN),
        CONST(0), bare(Op::RETURN),
    };
    uint8_t bytes[32];
    uint32_t len = encodeBody(body, 7, bytes, sizeof(bytes));

    BodyScanResult r = scanProcBody(bytes, len, 0);
    CHECK(r.ok);
    CHECK(r.bodyBytes == len);
}

TEST(ScanProcBodyOrdinaryLoopBackEdgeClosedByBlockEndFindsTheOuterTail)
{
    // The everyday shape: the loop body itself closes via BLOCK_END (the
    // back-edge), not a terminator — distinct from the bare-terminator
    // case above.
    const Instr body[] = {
        CONST(1), bare(Op::LOOP), bare(Op::BLOCK_END),
        CONST(42), bare(Op::BLOCK_END),
        CONST(0), bare(Op::RETURN),
    };
    uint8_t bytes[32];
    uint32_t len = encodeBody(body, 7, bytes, sizeof(bytes));

    BodyScanResult r = scanProcBody(bytes, len, 0);
    CHECK(r.ok);
    CHECK(r.bodyBytes == len);
}

TEST(ScanProcBodyNonLastCaseClosedByBareTerminatorStillCountsAgainstN)
{
    // The §8.5 wrinkle this port exists to get right: case[0] closes via a
    // bare RETURN (not the whole construct), case[1] via BLOCK_END (the
    // construct's own real end) — a scan that doesn't decrement `remaining`
    // for the bare-terminator close would run straight past this
    // procedure's own real end and into whatever bytes follow it.
    const Instr proc0[] = {
        CONST(0), brTable(2),
        CONST(111), bare(Op::RETURN),
        CONST(222), bare(Op::BLOCK_END),
        CONST(333), bare(Op::RETURN),
    };
    uint8_t bytes[32];
    uint32_t len0 = encodeBody(proc0, 8, bytes, sizeof(bytes));

    // A second procedure's own bytes immediately follow, no separator —
    // proves the scan didn't run into them.
    const Instr proc1[] = {CONST(999), bare(Op::RETURN)};
    uint32_t outLen = len0;
    encodeInstr(proc1[0], bytes, outLen, sizeof(bytes));
    encodeInstr(proc1[1], bytes, outLen, sizeof(bytes));

    BodyScanResult r = scanProcBody(bytes, outLen, 0);
    CHECK(r.ok);
    CHECK(r.bodyBytes == len0);
}

TEST(ScanProcBodyStackFloorReachedReportsNotOk)
{
    const Instr body[] = {bare(Op::LOOP), CONST(1), bare(Op::BLOCK_END), bare(Op::RETURN)};
    uint8_t bytes[16];
    uint32_t len = encodeBody(body, 4, bytes, sizeof(bytes));

    register uint32_t sp asm("sp");
    BodyScanResult r = scanProcBody(bytes, len, 0, sp); // floor pinned at the current sp: no margin at all
    CHECK(!r.ok);
    CHECK(r.failCode == RESOURCE_EXHAUSTED_SCAN_STACK); // out of stack, not a malformed body
}

TEST(ScanProcBodyRejectsAnExtensionRangeOpcode)
{
    // 0x80 is the first extension opcode (isa-core.md §11). decodeInstr only
    // asserts on it and every shipping build is -DNDEBUG, so this walk is
    // what actually stops it — reported as its own reason, distinct from
    // both a malformed body and running out of stack.
    const uint8_t bytes[] = {0x80};

    BodyScanResult r = scanProcBody(bytes, sizeof(bytes), 0);
    CHECK(!r.ok);
    CHECK(r.failCode == RESOURCE_PROGRAM_EXT_UNKNOWN);
}

TEST(ScanProcBodyAcceptsTheTopOfCoreOpcodeSpace)
{
    // 109-124 are the CONST small forms (isa-core.md §5.2), so the last one
    // below §5.3's escapes is ordinary core, not a hole.
    const uint8_t bytes[] = {124 /* CONST #15 */, 102 /* RETURN */};
    BodyScanResult r = scanProcBody(bytes, sizeof(bytes), 0);
    CHECK(r.ok);
    CHECK(r.failCode == 0);
}

TEST(ScanProcBodyAcceptsAnAssignedEscapeSubCode)
{
    // MISC_UNARY #1 is CLZ (isa-core.md §5.3) — two bytes, and the walk has
    // to step over both.
    const uint8_t bytes[] = {126, 1 /* CLZ */, 102 /* RETURN */};
    BodyScanResult r = scanProcBody(bytes, sizeof(bytes), 0);
    CHECK(r.ok);
    CHECK(r.failCode == 0);
    CHECK(r.needsLRSave); // CLZ reaches a helper
}

TEST(ScanProcBodyRejectsAnUnassignedEscapeSubCode)
{
    // An unassigned sub-code has no defined operand shape, so it has no
    // length either — the walk cannot skip it and must stop. That includes
    // FALLTHROUGH (MISC_CF #0), which §5.3 assigns but nothing implements.
    const uint8_t cases[][3] = {
        {125, 0, 102}, // MISC_CF #0 — FALLTHROUGH, not implemented
        {125, 1, 102}, // MISC_CF, reserved
        {126, 2, 102}, // MISC_UNARY, past its assigned sub-codes
        {127, 0, 102}, // MISC_BINARY, entirely reserved
    };
    for(uint32_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        BodyScanResult r = scanProcBody(cases[i], 3, 0);
        CHECK(!r.ok);
        CHECK(r.failCode == RESOURCE_PROGRAM_RESERVED_OPCODE);
    }
}

TEST(ScanProcBodyRejectsAnEscapeWithNoSubCodeAtAll)
{
    // Truncated right after the escape byte: there is no sub-code to read.
    const uint8_t bytes[] = {126};
    BodyScanResult r = scanProcBody(bytes, sizeof(bytes), 0);
    CHECK(!r.ok);
    CHECK(r.failCode == RESOURCE_PROGRAM_RESERVED_OPCODE);
}

TEST(ScanProcBodyRejectsAnExtensionOpcodeAfterAValidPrefix)
{
    // Not just the first byte: the walk must stop mid-body too, and must not
    // report the truncation it would otherwise notice at the end instead.
    const Instr prefix[] = {CONST(1)};
    uint8_t bytes[16];
    uint32_t len = encodeBody(prefix, 1, bytes, sizeof(bytes));
    bytes[len++] = 0x80;

    BodyScanResult r = scanProcBody(bytes, len, 0);
    CHECK(!r.ok);
    CHECK(r.failCode == RESOURCE_PROGRAM_EXT_UNKNOWN);
}

TEST(ScanProcBodyRunningOffTheEndIsNotAStackFloorHit)
{
    // A LOOP with nothing closing it: the walk runs off maxBytes with a
    // level still open. Same !ok as the floor case above, and the other
    // half of the distinction failCode exists to draw — a body that
    // was never well-formed, which no amount of stack would fix.
    const Instr body[] = {bare(Op::LOOP), CONST(1)};
    uint8_t bytes[16];
    uint32_t len = encodeBody(body, 2, bytes, sizeof(bytes));

    BodyScanResult r = scanProcBody(bytes, len, 0);
    CHECK(!r.ok);
    CHECK(r.failCode == RESOURCE_PROGRAM_BODY_UNTERMINATED);
}
