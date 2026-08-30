// enterProgram*'s argument plumbing and its two entry-argument guards.
// A fake enterDispatch captures the descriptor so a plumbing mistake fails
// in milliseconds with the values printed, rather than as a hung guest.
#include "Test.h"

#include "runtime.h"
#include "dispatch_abi.h"
#include "entry_args.h"
#include "ext.h"
#include "ext_stub.h"
#include "encode_instr.h"
#include "instr.h"
#include "host_runtime_support.h"

using namespace jitc;

namespace
{

/* What the real enterDispatch would have marshalled, captured verbatim.
 * Deep-copied rather than kept by pointer: the descriptor lives in
 * enterProgramCore's own frame and is dead the moment it returns. */
struct Captured
{
    bool called = false;
    uint32_t spilledCount = 0;
    uint32_t spilled[16] = {};
    uint32_t window[WINDOW_SIZE] = {};
    uint32_t acc = 0;
};

Captured g_captured;

/* A program whose entry procedure declares `entryArgCount` and whose body
 * is a bare CONST/RETURN — nothing here ever executes it, so the body only
 * has to encode and scan cleanly. */
uint32_t buildProgram(uint32_t entryArgCount, uint32_t totalDepth, uint8_t *out, uint32_t cap)
{
    static const Instr body[] = {CONST(7), bare(Op::RETURN)};
    ProcSource procs[] = {ProcSource{entryArgCount, body, 2}};
    return encodeJitProgram(/*maxCallDepth=*/0, totalDepth, procs, 1, out, cap);
}

/* Declines every byte, so a program containing one is rejected — which is
 * observable only if enterProgram* actually installed it. */
uint32_t decliningDecode(const uint8_t *, uint32_t, uint32_t, uint32_t *)
{
    return 0;
}

uint32_t acceptingDecode(const uint8_t *, uint32_t, uint32_t, uint32_t *decl)
{
    *decl = extDecl(0x80, 0, /*tosDelta=*/0, /*maxTransient=*/0, /*halfwords=*/2);
    return 1;
}

const ExtStub DECLINING = {decliningDecode};
const ExtStub ACCEPTING = {acceptingDecode};

ProgramResult enterWithExtension(const ExtStub *ext, const uint8_t *bytes, uint32_t len)
{
    static uint8_t arena[512];
    ExtScope scope(ext);
    g_captured = Captured{};
    return enterProgramSplit(nullptr, 0, bytes, len,
        (uint32_t)(uintptr_t)arena, sizeof(arena), /*stackLimit=*/0, /*interruptReserve=*/0);
}

ProgramResult enter(uint32_t *args, uint32_t argCount, const uint8_t *bytes, uint32_t len)
{
    static uint8_t arena[512];
    g_captured = Captured{};
    /* stackLimit 0 makes the up-front budget check trivially pass, which is
     * not what these TESTs are about — the two boundary cases for that
     * check live in test/qemu/main.cpp, against a real measured sp. */
    return enterProgramSplit(args, argCount, bytes, len,
        (uint32_t)(uintptr_t)arena, sizeof(arena), /*stackLimit=*/0, /*interruptReserve=*/0);
}

} // namespace

/* Stands in for runtime.S. Returns LANDING_SUCCESS with a recognisable
 * value, so a TEST can tell "the guard let this through" from "the guard
 * rejected it" without depending on anything the entry procedure computes. */
extern "C" uint64_t enterDispatch(void*, Runtime *runtime, const EntryArgs *entryArgs)
{
    (void)runtime;
    g_captured.called = true;

    const auto count = entryArgs->spilledEnd - entryArgs->spilledStart;
    g_captured.spilledCount = count;
    for(uint32_t i = 0; i < count && i < 16; i++)
    {
        g_captured.spilled[i] = entryArgs->spilledStart[i];
    }
    
    for(uint32_t i = 0; i < WINDOW_SIZE; i++) g_captured.window[i] = entryArgs->window[i];
    g_captured.acc = entryArgs->acc;

    return (uint64_t)LANDING_SUCCESS << 32 | 0xABCDu;
}

TEST(enterProgramAcceptsAZeroArgumentEntryProcedure)
{
    uint8_t bytes[32];
    uint32_t len = buildProgram(/*entryArgCount=*/0, /*totalDepth=*/1, bytes, sizeof(bytes));

    ProgramResult r = enter(nullptr, 0, bytes, len);

    CHECK(!r.trapped);
    CHECK(r.value == 0xABCDu);
    CHECK(g_captured.called);
    CHECK(g_captured.spilledCount == 0);
    CHECK(g_captured.acc == 0);
}

TEST(enterProgramPlacesASingleArgumentInAcc)
{
    uint8_t bytes[32];
    uint32_t len = buildProgram(/*entryArgCount=*/1, /*totalDepth=*/1, bytes, sizeof(bytes));
    uint32_t args[] = {0xFEEDu};

    ProgramResult r = enter(args, 1, bytes, len);

    CHECK(!r.trapped);
    CHECK(g_captured.called);
    CHECK(g_captured.spilledCount == 0);
    // isa-core.md §4.6: the sole argument travels in acc, and the prologue
    // is what copies it into physReg(0) — so window[] stays empty here.
    CHECK(g_captured.acc == 0xFEEDu);
    for(uint32_t i = 0; i < WINDOW_SIZE; i++) CHECK(g_captured.window[i] == 0);
}

TEST(enterProgramMarshalsEveryArgCountThroughTheRealPlumbing)
{
    // test_entry_args.cpp establishes that buildEntryArgs agrees with a real
    // call site. This checks the rest of the path — that enterProgram*
    // actually reaches it with the caller's own vector, for both window
    // phases and a spilled tail.
    for(uint32_t n = 1; n <= 12; n++)
    {
        uint8_t bytes[32];
        uint32_t len = buildProgram(n, /*totalDepth=*/n, bytes, sizeof(bytes));

        uint32_t args[16];
        for(uint32_t k = 0; k < n; k++) args[k] = 0x4000u + k;

        ProgramResult r = enter(args, n, bytes, len);

        CHECK(!r.trapped);
        CHECK(g_captured.called);
        CHECK(g_captured.acc == args[n - 1]);
        CHECK(g_captured.spilledCount == (n > WINDOW_SIZE ? n - WINDOW_SIZE : 0));
        for(uint32_t i = 0; i < g_captured.spilledCount; i++)
        {
            CHECK(g_captured.spilled[i] == args[i]); // ascending: slot 0 pushed first
        }
        for(uint32_t k = n > WINDOW_SIZE ? n - WINDOW_SIZE : 0; k + 1 < n; k++)
        {
            CHECK(g_captured.window[physReg(k) - WINDOW_BASE] == args[k]);
        }
    }
}

TEST(enterProgramRejectsAnArgumentCountTheEntryProcedureDoesNotDeclare)
{
    uint8_t bytes[32];
    uint32_t len = buildProgram(/*entryArgCount=*/1, /*totalDepth=*/1, bytes, sizeof(bytes));
    uint32_t args[] = {1, 2};

    // Too many...
    ProgramResult over = enter(args, 2, bytes, len);
    CHECK(over.trapped == LANDING_RESOURCE_ERROR);
    CHECK(over.value == RESOURCE_PROGRAM_ENTRY_ARG_COUNT);
    CHECK(!g_captured.called); // nothing ran — this is the memory-safety fix

    // ...and too few. Neither is clamped: the procedure reads exactly the
    // slots it declared and reclaims a frame sized from that same number.
    ProgramResult under = enter(nullptr, 0, bytes, len);
    CHECK(under.trapped == LANDING_RESOURCE_ERROR);
    CHECK(under.value == RESOURCE_PROGRAM_ENTRY_ARG_COUNT);
    CHECK(!g_captured.called);
}

TEST(enterProgramRejectsOutOfWindowArgsPastTheEnvelopesOwnTotalDepth)
{
    // total_depth is trusted wire data, and enterDispatch is about to push
    // arg_count - WINDOW_SIZE words against a reservation sized from it. A
    // well-formed envelope never understates this (validateProgram seeds
    // every procedure's local peak at its own argCount), so reaching this
    // means the envelope was forged or mis-generated.
    uint8_t bytes[32];
    uint32_t len = buildProgram(/*entryArgCount=*/8, /*totalDepth=*/1, bytes, sizeof(bytes));

    uint32_t args[8];
    for(uint32_t k = 0; k < 8; k++) args[k] = k;

    ProgramResult r = enter(args, 8, bytes, len);

    CHECK(r.trapped == LANDING_RESOURCE_ERROR);
    CHECK(r.value == RESOURCE_PROGRAM_ENTRY_DEPTH);
    CHECK(!g_captured.called);
}

TEST(enterProgramAcceptsOutOfWindowArgsThatDoFitTotalDepth)
{
    // The other side of the same guard: 8 arguments need 4 pushed words, and
    // a total_depth of 8 covers them — the bound is on the pushed words, not
    // on arg_count itself, so a deliberately slack envelope still passes.
    uint8_t bytes[32];
    uint32_t len = buildProgram(/*entryArgCount=*/8, /*totalDepth=*/4, bytes, sizeof(bytes));

    uint32_t args[8];
    for(uint32_t k = 0; k < 8; k++) args[k] = k;

    ProgramResult r = enter(args, 8, bytes, len);

    CHECK(!r.trapped);
    CHECK(g_captured.called);
    CHECK(g_captured.spilledCount == 4);
}

TEST(enterProgramRejectsAProgramWithNoProcedures)
{
    // max_call_depth:0 total_depth:0 proc_count:0 — rejected before any
    // Runtime storage is sized, since entering procedure 0 would read one
    // ProcSlot past what storageBytesFor(0) allocates. Covered on the
    // emulated side too, but free here.
    const uint8_t bytes[] = {0x00, 0x00, 0x00};

    ProgramResult r = enter(nullptr, 0, bytes, sizeof(bytes));

    CHECK(r.trapped == LANDING_RESOURCE_ERROR);
    CHECK(r.value == RESOURCE_PROGRAM_NO_PROCS);
    CHECK(!g_captured.called);
}

/* max_call_depth=0 total_depth=0 proc_count=1 arg_count=0 body=[0x80, RETURN] */
static const uint8_t kExtProgram[] = {0x00, 0x00, 0x01, 0x00, 0x80, 100};

TEST(TheExtensionArgumentIsWhatInstallsTheExtension)
{
    // With none passed, an extension byte has nothing to claim it.
    ProgramResult none = enterWithExtension(nullptr, kExtProgram, sizeof(kExtProgram));
    CHECK(none.trapped == LANDING_RESOURCE_ERROR);
    CHECK(none.value == RESOURCE_PROGRAM_EXT_UNKNOWN);

    // Passing one that accepts gets the same bytes all the way past the
    // directory walk and into dispatch. (Nothing is translated here — this
    // file's own enterDispatch stands in for runtime.S — so the codegen
    // bail M1 stops at belongs to test_translate_proc.cpp, not here.)
    ProgramResult ok = enterWithExtension(&ACCEPTING, kExtProgram, sizeof(kExtProgram));
    CHECK(ok.trapped == LANDING_SUCCESS);
    CHECK(g_captured.called);

    // And one that declines is reported as such.
    ProgramResult declined = enterWithExtension(&DECLINING, kExtProgram, sizeof(kExtProgram));
    CHECK(declined.value == RESOURCE_PROGRAM_EXT_UNKNOWN);
}


TEST(AModestDeclaredHelperStackDoesNotDisturbTheBudget)
{
    // That the declared bytes are actually ADDED to the budget is checked in
    // test/qemu/main.cpp, against a real measured sp: here stackLimit is 0,
    // so the check reduces to `sp < needed` and the host's own sp is
    // whatever ASLR chose — the same reason this file's other budget cases
    // live over there.
    static const ExtStub MODEST = {acceptingDecode, nullptr, EXT_THUNK_STACK_BYTES};
    CHECK(enterWithExtension(&MODEST, kExtProgram, sizeof(kExtProgram)).trapped == LANDING_SUCCESS);
}
