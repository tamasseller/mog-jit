// jit-armv6m/compiler/test — the extension seam's scan-phase half
// (compiler/src/ext.h): the core steps over an extension instruction without
// knowing its encoding, and rejects every description asking for something it
// doesn't implement. Codegen lives in test_translate_proc.cpp.
#include "Test.h"
#include "ext.h"
#include "ext_stub.h"
#include "decode_instr.h"
#include "proc_scan.h"
#include "encode_instr.h"
#include "instr.h"
#include "wire.h"

using namespace jitc;

namespace
{

// Opcode bytes this fake extension owns. One shape per thing worth testing.
constexpr uint8_t EXT_INLINE = 0x80;      // one LEB128 operand, no lr
constexpr uint8_t EXT_HELPER = 0x81;      // no operand, clobbers lr, pops one
constexpr uint8_t EXT_CALL_SHAPED = 0x82; // rejected: call-shaped
constexpr uint8_t EXT_NET_PUSH = 0x85;    // rejected: net TOS push
constexpr uint8_t EXT_DECLINED = 0x86;    // describe declines outright

bool fakeDescribe(uint8_t opcode, BcReader &wire, uint32_t *desc)
{
    switch(opcode)
    {
        case EXT_INLINE:
        {
            uint32_t value;
            if(!decodeLeb128(wire, value))
            {
                return false;
            }
            *desc = extDesc(0, /*tosDelta=*/0);
            return true;
        }
        case EXT_HELPER:
            *desc = extDesc(EXT_FLAG_NEEDS_LR, -1);
            return true;
        case EXT_CALL_SHAPED:
            *desc = extDesc(EXT_FLAG_CALL_SHAPED, 0);
            return true;
        case EXT_NET_PUSH:
            *desc = extDesc(0, /*tosDelta=*/1);
            return true;
        default:
            return false; // EXT_DECLINED and anything else
    }
}

// Claims every byte it is shown.
bool greedyDescribe(uint8_t, BcReader &, uint32_t *desc)
{
    *desc = extDesc(0, 0);
    return true;
}

const ExtStub FAKE = {fakeDescribe};
const ExtStub GREEDY = {greedyDescribe};

} // namespace

TEST(ExtDescRoundTripsEveryField)
{
    uint32_t w = extDesc(EXT_FLAG_NEEDS_LR, -3);
    CHECK(extDescHas(w, EXT_FLAG_NEEDS_LR));
    CHECK(!extDescHas(w, EXT_FLAG_CALL_SHAPED));
    CHECK(extDescTosDelta(w) == -3); // sign-extended back out of 4 bits
}

TEST(ExtDescHandlesTheExtremesOfTheSignedTosDeltaField)
{
    CHECK(extDescTosDelta(extDesc(0, 0)) == 0);
    CHECK(extDescTosDelta(extDesc(0, -1)) == -1);
    CHECK(extDescTosDelta(extDesc(0, EXT_TOS_DELTA_MIN)) == EXT_TOS_DELTA_MIN);
    CHECK(extDescTosDelta(extDesc(0, 7)) == 7);
}

TEST(DecodeLeb128RefusesAnOverlongEncoding)
{
    // Six continuation bytes: more than a u32 can hold.
    const uint8_t overlong[] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00};
    BcReader r = wireOver(overlong, sizeof(overlong));
    uint32_t value = 0;
    CHECK(!decodeLeb128(r, value));

    // And the ordinary case still works.
    const uint8_t fine[] = {0xe5, 0x8e, 0x26};
    BcReader ok = wireOver(fine, sizeof(fine));
    CHECK(decodeLeb128(ok, value));
    CHECK(value == 624485);
    CHECK(ok.atEnd());
}

TEST(DecodeLeb128RefusesAValueTheBodyEndsInTheMiddleOf)
{
    // Every byte continuing, so an unbounded decoder walks off the end.
    const uint8_t truncated[] = {0x80, 0x80, 0x80};
    BcReader r = wireOver(truncated, sizeof(truncated));
    uint32_t value = 0;

    CHECK(!decodeLeb128(r, value));
}

TEST(DecodeInstrYieldsOpExtWithItsOpcodeAndNoOperandsRead)
{
    // The core learns which extension byte it is looking at and nothing
    // else — the operands stay standing for whichever phase asks next.
    const uint8_t bytes[] = {EXT_INLINE, 0xe5, 0x8e, 0x26};

    WireInstr d = decodeOne(bytes, sizeof(bytes));
    CHECK(d.ok);
    CHECK(d.instr.op == Op::EXT);
    CHECK(d.instr.extOpcode == EXT_INLINE);
    CHECK(d.consumed == 1);
}

TEST(ScanProcBodyReportsAnOpcodeNoExtensionWasPassedFor)
{
    const uint8_t bytes[] = {EXT_INLINE, 0x00};

    BodyScanResult r = scanBytes(bytes, sizeof(bytes));
    CHECK(!r.ok);
    CHECK(r.failCode == RESOURCE_PROGRAM_EXT_UNKNOWN);
}

TEST(ScanProcBodyStepsOverAnExtensionOpUsingWhatItConsumed)
{
    // The whole point of hooking the walk: it finds the body boundary
    // without knowing anything about the extension.
    ExtScope ext(&FAKE);
    uint8_t bytes[16];
    uint32_t n = 0;
    bytes[n++] = EXT_INLINE;
    bytes[n++] = 0xe5;
    bytes[n++] = 0x8e;
    bytes[n++] = 0x26;
    const Instr tail[] = {bare(Op::RETURN)};
    n += encodeBody(tail, 1, bytes + n, sizeof(bytes) - n);

    BodyScanResult r = scanBytes(bytes, n);
    CHECK(r.ok);
    CHECK(r.failCode == 0);
    CHECK(r.bodyBytes == n);
    CHECK(!r.needsLRSave);
}

TEST(ScanProcBodyTakesNeedsLRSaveFromTheDescription)
{
    // The prologue is emitted from ProcSlot's needsLRSave long before
    // codegen sees the op, so the description has to settle it here.
    ExtScope ext(&FAKE);
    uint8_t bytes[8];
    uint32_t n = 0;
    bytes[n++] = EXT_HELPER;
    const Instr tail[] = {bare(Op::RETURN)};
    n += encodeBody(tail, 1, bytes + n, sizeof(bytes) - n);

    BodyScanResult r = scanBytes(bytes, n);
    CHECK(r.ok);
    CHECK(r.needsLRSave);
}

TEST(ScanProcBodyRejectsEachCapabilityV1DoesNotImplement)
{
    ExtScope ext(&FAKE);
    const uint8_t rejected[] = {EXT_CALL_SHAPED, EXT_NET_PUSH};
    for(uint32_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++)
    {
        uint8_t bytes[4];
        uint32_t n = 0;
        bytes[n++] = rejected[i];
        const Instr tail[] = {bare(Op::RETURN)};
        n += encodeBody(tail, 1, bytes + n, sizeof(bytes) - n);

        BodyScanResult r = scanBytes(bytes, n);
        CHECK(!r.ok);
        CHECK(r.failCode == RESOURCE_PROGRAM_EXT_UNSUPPORTED); // a newer core, not a different image
    }
}

TEST(ScanProcBodyReportsAnOpcodeTheExtensionDeclines)
{
    ExtScope ext(&FAKE);
    const uint8_t bytes[] = {EXT_DECLINED};

    BodyScanResult r = scanBytes(bytes, sizeof(bytes));
    CHECK(!r.ok);
    CHECK(r.failCode == RESOURCE_PROGRAM_EXT_UNKNOWN);
}

TEST(TheTopOfCoreOpcodeSpaceIsNeverOfferedToAnExtension)
{
    // The boundary this pins: the core assigns every byte up to 127 and the
    // extension range starts at 128 (isa-core.md §5.1). GREEDY accepts every
    // byte it is shown, so an off-by-one in the gate would let it squat on
    // core opcode space — here the last CONST small form and §5.3's own
    // escapes, which are core too however their sub-codes are resolved.
    ExtScope ext(&GREEDY);
    const uint8_t lastConst[] = {124, 102 /* RETURN */};
    BodyScanResult constScan = scanBytes(lastConst, sizeof(lastConst));
    CHECK(constScan.ok);
    CHECK(constScan.failCode == 0);

    const uint8_t escape[] = {125, 0, 102 /* RETURN */};
    BodyScanResult escapeScan = scanBytes(escape, sizeof(escape));
    CHECK(!escapeScan.ok);
    CHECK(escapeScan.failCode == RESOURCE_PROGRAM_RESERVED_OPCODE); // core's own reason, never the extension's

    // ...and 128 is the first byte it legitimately does get.
    const uint8_t first[] = {0x80, 102 /* RETURN */};
    BodyScanResult r = scanBytes(first, sizeof(first));
    CHECK(r.ok);
    CHECK(r.failCode == 0);
}
