// End-to-end proof: the real translator (translate_proc.h), reached
// through the real dispatch/eviction runtime (runtime_host.cpp,
// unmodified), actually compiles and runs every fixture (fixtures.cpp)
// correctly on real QEMU — including LOOP/BR_TABLE/comparisons/unary ops,
// terminator-closed blocks, a forced-long-branch case, actual
// eviction+compaction under a small arena, and both RESOURCE_ERROR sides
// (a too-small code arena, and enterProgramOnStack's own stack-usage
// pre-check).
#include <stdint.h>
#include <cassert>
#include "runtime_host.h"
#include "fixtures.h"
#include "instr.h"
#include "encode_instr.h"
#include "translate_proc.h"

using namespace jitc;

extern "C" {
void writeHexResult(uint32_t v);
void writeHexTrap(uint32_t v);
void semihostingExit(int code);
}

static const FlashProc dummyProcs[8] = {}; /* enterProgram's own procs param — never dereferenced on this path */

static bool runFixtures()
{
    bool allOk = true;

    for(uint32_t f = 0; f < fixtureCount; f++)
    {
        const Fixture &fx = fixtures[f];
        realProcs = fx.procs;
        realProcCount = fx.procCount;

        ProgramResult r = enterProgram(fx.argIn, fx.arenaSize, dummyProcs, fx.procCount);

        bool ok = (r.trapped != 0) == fx.expectTrapped && r.value == fx.expectValue;
        allOk = allOk && ok;

        if(r.trapped)
        {
            writeHexTrap(r.value);
        }
        else
        {
            writeHexResult(r.value);
        }
    }

    return allOk;
}

// enterProgramOnStack/enterProgramSplit — the layout-agnostic entry
// points. Both reach compileProc through the exact same lazy dispatch path
// the fixture loop above does (realProcs, unchanged) — only the work
// area's own placement, and the up-front stack-usage check ahead of it,
// differ. operandStackBytes/maxCallDepth below are hand-derived from each
// small program's own known shape rather than computed here — this file
// has no whole-program static analyzer of its own to call.
extern "C" uint8_t __bss_end; /* vectors.S/linker.ld's own symbol — the one genuinely safe floor for anything placed on the C stack */

namespace
{

constexpr uint32_t GENEROUS_ARENA = 400;
// A small, non-negative margin above __bss_end — not a raw subtraction
// from the measured sp at the call site: this fixture corpus has a real
// .bss footprint (scratch et al.), so "sp minus some generous-looking
// constant" can land below __bss_end (inside .bss/.data instead of
// genuinely free stack space) depending on how big that footprint is.
// Anchoring above __bss_end instead is the one bound that's actually safe
// regardless.
constexpr uint32_t GENEROUS_SLACK = 512;

uint32_t currentSp()
{
    register uint32_t sp asm("sp");
    return sp;
}

uint32_t stackLimitAboveBss()
{
    return (uint32_t)(uintptr_t)&__bss_end + GENEROUS_SLACK;
}

Proc makeProc(uint32_t argCount, const Instr *body, uint32_t count, uint8_t *bytesOut, uint32_t bytesCap)
{
    uint32_t len = encodeBody(body, count, bytesOut, bytesCap);
    return Proc{argCount, bytesOut, len};
}

bool testOnStackGenerousSucceeds()
{
    // A calls B — one live call record while B executes; B's own peak tos
    // is 1 (argCount=1, no further pushes).
    const Instr proc0Body[] = {CONST(37), call(1), bare(Op::RETURN)};
    const Instr proc1Body[] = {LOAD(0), opImm(Op::ADD, 5), bare(Op::RETURN)};
    uint8_t bytes0[16], bytes1[16];
    Proc procs[] = {
        makeProc(0, proc0Body, 3, bytes0, sizeof(bytes0)),
        makeProc(1, proc1Body, 3, bytes1, sizeof(bytes1)),
    };
    realProcs = procs;
    realProcCount = 2;

    uint32_t stackLimit = stackLimitAboveBss();
    ProgramResult r = enterProgramOnStack(0, dummyProcs, 2, GENEROUS_ARENA,
        /*operandStackBytes=*/1 * 4, /*maxCallDepth=*/1, stackLimit, /*interruptReserve=*/0);

    bool ok = !r.trapped && r.value == 42;
    writeHexResult(r.value);
    return ok;
}

bool testSplitThreeDeepCallChainSucceeds()
{
    // A->B->C — two live records while C executes; each procedure's own
    // peak tos is 1 (argCount=1, no further pushes).
    const Instr proc0Body[] = {CONST(5), call(1), bare(Op::RETURN)};
    const Instr proc1Body[] = {LOAD(0), call(2), opImm(Op::ADD, 1), bare(Op::RETURN)};
    const Instr proc2Body[] = {LOAD(0), opImm(Op::ADD, 100), bare(Op::RETURN)};
    uint8_t bytes0[16], bytes1[16], bytes2[16];
    Proc procs[] = {
        makeProc(0, proc0Body, 3, bytes0, sizeof(bytes0)),
        makeProc(1, proc1Body, 4, bytes1, sizeof(bytes1)),
        makeProc(1, proc2Body, 3, bytes2, sizeof(bytes2)),
    };
    realProcs = procs;
    realProcCount = 3;

    static uint8_t arena[GENEROUS_ARENA];
    uint32_t stackLimit = stackLimitAboveBss();
    ProgramResult r = enterProgramSplit(0, dummyProcs, 3,
        (uint32_t)(uintptr_t)arena, GENEROUS_ARENA,
        /*operandStackBytes=*/2 * 4, /*maxCallDepth=*/2, stackLimit, /*interruptReserve=*/0);

    // C: 5 + 100 = 105; B: 105 + 1 = 106; A returns B's result unchanged.
    bool ok = !r.trapped && r.value == 106;
    writeHexResult(r.value);
    return ok;
}

bool testOnStackRejectsBeforeTouchingAnything()
{
    // stackLimit == (about) the entry sp itself — any nonzero requirement
    // fails the check immediately, before enterDispatch (or compileProc)
    // ever runs; testOnStackGenerousSucceeds() already proved these
    // programs compile fine given room, so a RESOURCE_ERROR here can only
    // be the stack-usage pre-check, not a translator/runtime problem.
    const Instr proc0Body[] = {CONST(37), call(1), bare(Op::RETURN)};
    const Instr proc1Body[] = {LOAD(0), opImm(Op::ADD, 5), bare(Op::RETURN)};
    uint8_t bytes0[16], bytes1[16];
    Proc procs[] = {
        makeProc(0, proc0Body, 3, bytes0, sizeof(bytes0)),
        makeProc(1, proc1Body, 3, bytes1, sizeof(bytes1)),
    };
    realProcs = procs;
    realProcCount = 2;

    uint32_t stackLimit = currentSp(); // measured before this callee's own prologue — strictly higher than sp once inside it
    ProgramResult r = enterProgramOnStack(0, dummyProcs, 2, GENEROUS_ARENA,
        /*operandStackBytes=*/1 * 4, /*maxCallDepth=*/1, stackLimit, /*interruptReserve=*/0);

    bool ok = r.trapped && r.value == 0x52455343u; // RESOURCE_ERROR_CODE, "RESC"
    if(r.trapped)
    {
        writeHexTrap(r.value);
    }
    else
    {
        writeHexResult(r.value);
    }
    return ok;
}

} // namespace

// Eviction + compaction. This measures each procedure's own compiled size
// by calling the real translateProc() once per procedure up front (a
// throwaway measurement, discarded immediately) purely to size the arena
// — the actual exercise then goes through the ordinary lazy
// realProcs/compileProc path exactly like every other fixture, so
// compile_proc_real.cpp genuinely retranslates from the same wire bytes
// whenever a procedure gets evicted and later needed again (the same flash
// blob must reproduce the same layout, or a saved resume offset would no
// longer point at the right place).
namespace
{

uint32_t measuredHalfwords(const Proc &proc, uint32_t procIdx, const uint32_t *calleeArgCounts, uint32_t calleeCount)
{
    static uint16_t scratch[128];
    TranslateResult r = translateProc(proc, procIdx, calleeArgCounts, calleeCount, scratch, 128);
    assert(!r.overflowed); // GCOV_EXCL_LINE — the scratch buffer above is already generous for this test corpus
    return r.halfwordCount;
}

bool testEvictionThreeDeepCallChain()
{
    // Same chain as testSplitThreeDeepCallChainSucceeds() — here the point
    // is that it cannot all be resident together, so compiling the
    // deepest call forces evicting an ancestor (possibly the entry
    // procedure itself, still suspended on the control stack), which then
    // has to be recompiled from scratch when its own RETURN eventually
    // fires.
    const Instr proc0Body[] = {CONST(5), call(1), bare(Op::RETURN)};
    const Instr proc1Body[] = {LOAD(0), call(2), opImm(Op::ADD, 1), bare(Op::RETURN)};
    const Instr proc2Body[] = {LOAD(0), opImm(Op::ADD, 100), bare(Op::RETURN)};
    uint8_t bytes0[16], bytes1[16], bytes2[16];
    Proc procs[] = {
        makeProc(0, proc0Body, 3, bytes0, sizeof(bytes0)),
        makeProc(1, proc1Body, 4, bytes1, sizeof(bytes1)),
        makeProc(1, proc2Body, 3, bytes2, sizeof(bytes2)),
    };
    uint32_t argCounts[] = {0, 1, 1};

    uint32_t sizes[3];
    uint32_t total = 0, smallest = UINT32_MAX;
    for(uint32_t i = 0; i < 3; i++)
    {
        sizes[i] = measuredHalfwords(procs[i], i, argCounts, 3) * 2;
        total += sizes[i];
        if(sizes[i] < smallest)
        {
            smallest = sizes[i];
        }
    }
    uint32_t arenaSize = total - smallest + 4; // fits any single one, but not all three

    realProcs = procs;
    realProcCount = 3;
    ProgramResult r = enterProgram(0, arenaSize, dummyProcs, 3);

    bool ok = !r.trapped && r.value == 106;
    if(r.trapped)
    {
        writeHexTrap(r.value);
    }
    else
    {
        writeHexResult(r.value);
    }
    return ok;
}

bool testEvictionCallerAndCalleeNeverCoresident()
{
    // A calls B; the arena fits only one of the two at a time, so
    // compiling B evicts A (still suspended on the control stack, mid-
    // call) — then B's own RETURN has to recompile A from scratch before
    // it can resume. Cross-recompilation in both directions within a
    // single call/return round trip, not just one.
    const Instr proc0Body[] = {CONST(1), call(1), opImm(Op::ADD, 1000), bare(Op::RETURN)};
    const Instr proc1Body[] = {LOAD(0), opImm(Op::ADD, 1), bare(Op::RETURN)};
    uint8_t bytes0[16], bytes1[16];
    Proc procs[] = {
        makeProc(0, proc0Body, 4, bytes0, sizeof(bytes0)),
        makeProc(1, proc1Body, 3, bytes1, sizeof(bytes1)),
    };
    uint32_t argCounts[] = {0, 1};

    uint32_t size0 = measuredHalfwords(procs[0], 0, argCounts, 2) * 2;
    uint32_t size1 = measuredHalfwords(procs[1], 1, argCounts, 2) * 2;
    uint32_t arenaSize = (size0 > size1 ? size0 : size1) + 4; // fits at most one of the two at a time

    realProcs = procs;
    realProcCount = 2;
    ProgramResult r = enterProgram(0, arenaSize, dummyProcs, 2);

    // B: 1 + 1 = 2; A: 2 + 1000 = 1002.
    bool ok = !r.trapped && r.value == 1002;
    if(r.trapped)
    {
        writeHexTrap(r.value);
    }
    else
    {
        writeHexResult(r.value);
    }
    return ok;
}

bool testResourceErrorSingleProcedureLargerThanArena()
{
    // 41 arithmetic instructions is comfortably beyond any arena worth
    // testing against below — no eviction victim can ever free enough room
    // for a procedure that's bigger than the entire arena.
    Instr body[42];
    body[0] = CONST(0);
    for(int i = 1; i <= 40; i++)
    {
        body[i] = opImm(Op::ADD, 1);
    }
    body[41] = bare(Op::RETURN);
    uint8_t bytes[256];
    Proc proc = makeProc(0, body, 42, bytes, sizeof(bytes));
    uint32_t argCounts[] = {0};

    uint32_t size = measuredHalfwords(proc, 0, argCounts, 1) * 2;
    uint32_t arenaSize = size > 24 ? size - 24 : 4; // deliberately smaller than this one procedure's own size

    realProcs = &proc;
    realProcCount = 1;
    ProgramResult r = enterProgram(0, arenaSize, dummyProcs, 1);

    bool ok = r.trapped && r.value == 0x52455343u; // RESOURCE_ERROR_CODE, "RESC"
    if(r.trapped)
    {
        writeHexTrap(r.value);
    }
    else
    {
        writeHexResult(r.value);
    }
    return ok;
}

} // namespace

int main(void)
{
    initFixtures();

    bool allOk = runFixtures();
    allOk = testOnStackGenerousSucceeds() && allOk;
    allOk = testSplitThreeDeepCallChainSucceeds() && allOk;
    allOk = testOnStackRejectsBeforeTouchingAnything() && allOk;
    allOk = testEvictionThreeDeepCallChain() && allOk;
    allOk = testEvictionCallerAndCalleeNeverCoresident() && allOk;
    allOk = testResourceErrorSingleProcedureLargerThanArena() && allOk;

    semihostingExit(allOk ? 0 : 1);
    return 0;
}
