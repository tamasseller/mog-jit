// jit-armv6m/compiler/test — the extension seam (compiler/src/ext.h).
//
// M1 is the decode/declaration half: the core learns an extension
// instruction's byte length and its declared effect, and rejects every
// declaration asking for something it doesn't implement. Codegen is M2, so
// a well-formed op is accepted by the walk and bails at emit.
#include "Test.h"
#include "ext.h"
#include "decode_instr.h"
#include "proc_scan.h"
#include "encode_instr.h"
#include "instr.h"

using namespace jitc;

namespace
{

// Opcode bytes this fake extension owns. One shape per thing worth testing.
constexpr uint8_t EXT_INLINE = 0x80;      // one LEB128 operand, 2 halfwords, no lr
constexpr uint8_t EXT_HELPER = 0x81;      // no operand, clobbers lr, pops one
constexpr uint8_t EXT_CALL_SHAPED = 0x82; // rejected: call-shaped
constexpr uint8_t EXT_TERMINATES = 0x83;  // rejected: terminates
constexpr uint8_t EXT_TRANSIENT = 0x84;   // rejected: transient push
constexpr uint8_t EXT_NET_PUSH = 0x85;    // rejected: net TOS push
constexpr uint8_t EXT_DECLINED = 0x86;    // decode declines outright

uint32_t fakeDecode(const uint8_t *bytes, uint32_t bytesLen, uint32_t offset, uint32_t *decl)
{
    switch(bytes[offset])
    {
        case EXT_INLINE:
        {
            // A real operand, decoded with the bounded LEB128 ext.h mandates:
            // this runs from Runtime::init's walk on unvalidated bytes.
            uint32_t value, next;
            if(!decodeLeb128Checked(bytes, bytesLen, offset + 1, value, next))
            {
                return 0;
            }
            *decl = extDecl(EXT_INLINE, 0, /*tosDelta=*/0, /*maxTransient=*/0, /*halfwords=*/2);
            return next - offset;
        }
        case EXT_HELPER:
            *decl = extDecl(EXT_HELPER, EXT_FLAG_NEEDS_LR | EXT_FLAG_WRITES_ACC, -1, 0, 6);
            return 1;
        case EXT_CALL_SHAPED:
            *decl = extDecl(EXT_CALL_SHAPED, EXT_FLAG_CALL_SHAPED, 0, 0, 4);
            return 1;
        case EXT_TERMINATES:
            *decl = extDecl(EXT_TERMINATES, EXT_FLAG_TERMINATES, 0, 0, 2);
            return 1;
        case EXT_TRANSIENT:
            *decl = extDecl(EXT_TRANSIENT, 0, 0, /*maxTransient=*/1, 2);
            return 1;
        case EXT_NET_PUSH:
            *decl = extDecl(EXT_NET_PUSH, 0, /*tosDelta=*/1, 0, 2);
            return 1;
        default:
            return 0; // EXT_DECLINED and anything else
    }
}

// Claims a length running past the buffer — the core must not trust it.
uint32_t overrunDecode(const uint8_t *, uint32_t, uint32_t, uint32_t *decl)
{
    *decl = extDecl(EXT_INLINE, 0, 0, 0, 2);
    return 999;
}

// Claims no forward progress — would hang every walk if trusted.
uint32_t zeroLengthDecode(const uint8_t *, uint32_t, uint32_t, uint32_t *decl)
{
    *decl = extDecl(EXT_INLINE, 0, 0, 0, 2);
    return 0;
}

uint32_t greedyDecode(const uint8_t *, uint32_t, uint32_t, uint32_t *decl)
{
    *decl = extDecl(EXT_INLINE, 0, 0, 0, 2);
    return 1; // claims every byte it is shown
}

const ExtHooks FAKE = {EXT_ABI_VERSION, fakeDecode};
const ExtHooks GREEDY = {EXT_ABI_VERSION, greedyDecode};
const ExtHooks OVERRUN = {EXT_ABI_VERSION, overrunDecode};
const ExtHooks ZERO_LENGTH = {EXT_ABI_VERSION, zeroLengthDecode};

} // namespace

TEST(ExtDeclRoundTripsEveryField)
{
    uint32_t w = extDecl(0x9a, EXT_FLAG_NEEDS_LR | EXT_FLAG_ATOMIC, -3, 5, 63);
    CHECK(extDeclOpcode(w) == 0x9a);
    CHECK(extDeclHas(w, EXT_FLAG_NEEDS_LR));
    CHECK(extDeclHas(w, EXT_FLAG_ATOMIC));
    CHECK(!extDeclHas(w, EXT_FLAG_CALL_SHAPED));
    CHECK(extDeclTosDelta(w) == -3); // sign-extended back out of 4 bits
    CHECK(extDeclMaxTransient(w) == 5);
    CHECK(extDeclHalfwords(w) == 63);
}

TEST(ExtDeclHandlesTheExtremesOfTheSignedTosDeltaField)
{
    CHECK(extDeclTosDelta(extDecl(0x80, 0, 0, 0, 0)) == 0);
    CHECK(extDeclTosDelta(extDecl(0x80, 0, -1, 0, 0)) == -1);
    CHECK(extDeclTosDelta(extDecl(0x80, 0, EXT_TOS_DELTA_MIN, 0, 0)) == EXT_TOS_DELTA_MIN);
    CHECK(extDeclTosDelta(extDecl(0x80, 0, 7, 0, 0)) == 7);
}

TEST(DecodeLeb128CheckedRefusesToReadPastTheBuffer)
{
    // Every byte continuing, so an unbounded decoder walks off the end.
    const uint8_t truncated[] = {0x80, 0x80, 0x80};
    uint32_t value = 0, next = 0;
    CHECK(!decodeLeb128Checked(truncated, sizeof(truncated), 0, value, next));

    // Starting at or past the end.
    CHECK(!decodeLeb128Checked(truncated, sizeof(truncated), 3, value, next));

    // Overlong for a u32: six continuation bytes.
    const uint8_t overlong[] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00};
    CHECK(!decodeLeb128Checked(overlong, sizeof(overlong), 0, value, next));

    // And the ordinary case still works.
    const uint8_t fine[] = {0xe5, 0x8e, 0x26};
    CHECK(decodeLeb128Checked(fine, sizeof(fine), 0, value, next));
    CHECK(value == 624485);
    CHECK(next == 3);
}

TEST(DecodeInstrYieldsOpExtWithTheDeclarationAndLength)
{
    const ExtHooks *ext = &FAKE;
    const uint8_t bytes[] = {EXT_INLINE, 0xe5, 0x8e, 0x26}; // operand 624485, 3 LEB128 bytes

    DecodedInstr d = decodeInstr(bytes, sizeof(bytes), 0, ext);
    CHECK(d.instr.op == Op::EXT);
    CHECK(extDeclOpcode(d.instr.extDecl) == EXT_INLINE);
    CHECK(extDeclHalfwords(d.instr.extDecl) == 2);
    CHECK(d.next == 4); // opcode + its own three operand bytes
}

TEST(ExtDecodeLengthRejectsALengthPastTheBuffer)
{
    // The core checks the claimed length rather than trusting it: an
    // extension that overruns must produce a rejection, not an overrun.
    const ExtHooks *ext = &OVERRUN;
    const uint8_t bytes[] = {EXT_INLINE, 0x00};
    uint32_t decl = 0;
    CHECK(extDecodeLength(bytes, sizeof(bytes), 0, decl, ext) == 0);
}

TEST(ExtDecodeLengthRejectsNoForwardProgress)
{
    // A zero length would hang every walk that steps by it.
    const ExtHooks *ext = &ZERO_LENGTH;
    const uint8_t bytes[] = {EXT_INLINE, 0x00};
    uint32_t decl = 0;
    CHECK(extDecodeLength(bytes, sizeof(bytes), 0, decl, ext) == 0);
}

TEST(ExtDecodeLengthRejectsWhenNoExtensionWasPassed)
{
    const uint8_t bytes[] = {EXT_INLINE, 0x00};
    uint32_t decl = 0;
    CHECK(extDecodeLength(bytes, sizeof(bytes), 0, decl, /*ext=*/nullptr) == 0);
}

TEST(ScanProcBodyStepsOverAnExtensionOpUsingItsDeclaredLength)
{
    // The whole point of hooking the decoder: this walk finds the body
    // boundary without knowing anything about the extension.
    const ExtHooks *ext = &FAKE;
    uint8_t bytes[16];
    uint32_t n = 0;
    bytes[n++] = EXT_INLINE;
    bytes[n++] = 0xe5;
    bytes[n++] = 0x8e;
    bytes[n++] = 0x26;
    const Instr tail[] = {bare(Op::RETURN)};
    n += encodeBody(tail, 1, bytes + n, sizeof(bytes) - n);

    BodyScanResult r = scanProcBody(bytes, n, 0, ext);
    CHECK(r.ok);
    CHECK(r.failCode == 0);
    CHECK(r.bodyBytes == n);
    CHECK(!r.needsLRSave);
}

TEST(ScanProcBodyTakesNeedsLRSaveFromTheDeclaration)
{
    // The prologue is emitted from ProcSlot's needsLRSave long before
    // codegen sees the op, so the declaration has to settle it here.
    const ExtHooks *ext = &FAKE;
    uint8_t bytes[8];
    uint32_t n = 0;
    bytes[n++] = EXT_HELPER;
    const Instr tail[] = {bare(Op::RETURN)};
    n += encodeBody(tail, 1, bytes + n, sizeof(bytes) - n);

    BodyScanResult r = scanProcBody(bytes, n, 0, ext);
    CHECK(r.ok);
    CHECK(r.needsLRSave);
}

TEST(ScanProcBodyRejectsEachCapabilityV1DoesNotImplement)
{
    const ExtHooks *ext = &FAKE;
    const uint8_t rejected[] = {EXT_CALL_SHAPED, EXT_TERMINATES, EXT_TRANSIENT, EXT_NET_PUSH};
    for(uint32_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++)
    {
        uint8_t bytes[4];
        uint32_t n = 0;
        bytes[n++] = rejected[i];
        const Instr tail[] = {bare(Op::RETURN)};
        n += encodeBody(tail, 1, bytes + n, sizeof(bytes) - n);

        BodyScanResult r = scanProcBody(bytes, n, 0, ext);
        CHECK(!r.ok);
        CHECK(r.failCode == RESOURCE_PROGRAM_EXT_UNSUPPORTED); // a newer core, not a different image
    }
}

TEST(ScanProcBodyReportsAnOpcodeTheExtensionDeclines)
{
    const ExtHooks *ext = &FAKE;
    const uint8_t bytes[] = {EXT_DECLINED};

    BodyScanResult r = scanProcBody(bytes, sizeof(bytes), 0, ext);
    CHECK(!r.ok);
    CHECK(r.failCode == RESOURCE_PROGRAM_EXT_UNKNOWN);
}

TEST(TheCoreReservedOpcodesAreNeverOfferedToAnExtension)
{
    // The boundary bug this pins: 124-127 are reserved to the CORE
    // (isa-core.md §5.3) and the extension range starts at 128 (§5.1), so
    // an extension must never get a chance to claim one. GREEDY accepts
    // every byte it is shown, so if the gate used "> LAST_CORE_OPCODE"
    // instead of ">= EXT_OPCODE_BASE" these would scan clean and let an
    // extension squat on core opcode space.
    const ExtHooks *ext = &GREEDY;
    for(uint8_t code = 124; code < 128; code++)
    {
        const uint8_t bytes[] = {code};
        BodyScanResult r = scanProcBody(bytes, sizeof(bytes), 0, ext);
        CHECK(!r.ok);
        CHECK(r.failCode == RESOURCE_PROGRAM_RESERVED_OPCODE);
    }

    // ...and 128 is the first byte it legitimately does get.
    const uint8_t first[] = {0x80, 100 /* RETURN */};
    BodyScanResult r = scanProcBody(first, sizeof(first), 0, ext);
    CHECK(r.ok);
    CHECK(r.failCode == 0);
}
