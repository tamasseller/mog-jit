// fuzz -- the crash-finding sink: run the real translateProc() pipeline
// under ASan/UBSan, with asserts live, over a batch of programs the TS
// driver has already lowered and validated. A crash here is a real
// "runtime+compiler stability" bug by construction, since the JIT is never
// supposed to see anything a validator has not approved.
//
// It never executes what it emits -- that is the QEMU half's job
// (src/qemu-exec/), whose image is built -DNDEBUG and so cannot see an
// assert. The two are blind to each other's findings; run both.
//
// The batch format is exec_runner.cpp's own, so both sinks read exactly the
// same file. Its entry-argument words are stepped over rather than used:
// nothing here runs anything.
//
// `Assembler::fail()`'s resource bail is the OTHER acceptable outcome
// ("runs correctly, or bails with a proper diagnostic") -- the harness only
// cares whether control escaped, not which RESOURCE_* code came back.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <csetjmp>

#include "translate_proc.h"
#include "decode_instr.h"
#include "runtime.h"
#include "envelope.h"

using namespace jitc;

// ── host-only resource-bail escape (mirrors test/host/host_runtime_support.cpp,
//    minus the 1test MOCK bookkeeping this harness has no use for) ─────────
static jmp_buf g_resourceEscape;

extern "C" [[noreturn]] void runtimeBail(Runtime *, uint32_t)
{
    longjmp(g_resourceEscape, 1);
}

extern const uint32_t trampolineAddr = 0xDEADBEEFu;

// ── the code arena ───────────────────────────────────────────────────
//
// Ordinary static storage, not a hand-placed low mapping: this driver is
// built -m32 (build.sh), so every address already fits the bare uint32_t
// the Assembler and every Runtime arena method address the arena through.
// Being ASan-instrumented storage is what a fixed mmap wouldn't give --
// an emitted halfword landing past arenaEnd is caught here rather than
// scribbling on a neighbouring page.
//
// Sized for pass 1, where the point is translating a procedure under no
// capacity pressure at all. Pass 2 asks for a far smaller slice of it,
// because that pass's point is the opposite: eviction, compaction, and
// the literal pool running out of reach.
static constexpr uint32_t ARENA_CAPACITY = 65536u;
static constexpr uint32_t PRESSURE_ARENA_CAP = 8192u;

alignas(8) static uint8_t g_arena[ARENA_CAPACITY];

static uint32_t arenaBase()
{
    return (uint32_t)(uintptr_t)g_arena;
}

// The arena size this input gets. Scaled to the program rather than a
// fixed constant, and drawn from the input's own bytes rather than an
// execution counter so a crash stays reproducible from the saved file
// alone.
//
// Absolute sizes don't work here: eviction and compaction only ever run
// when an in-progress translation exhausts an arena that some *other*
// procedure is already resident in, which is a narrow band around the
// program's own compiled size -- measured with probe_arena.cpp, a 3-
// procedure seed evicts between roughly 48 and 96 bytes of arena and never
// again above that. A constant tuned for one program size leaves every
// other size either bailing immediately or never under pressure at all.
//
// The estimate only has to be the right order of magnitude: blocks.h
// prices an ordinary instruction's worst case at 16 bytes and a call
// sequence at 64, so 8 bytes of code per byte of bytecode is a reasonable
// middle. The four multipliers then straddle it -- a quarter (constant
// eviction, frequent RESOURCE_ERROR), a half and 1x (real compaction), 4x
// (roomy, so the no-pressure path is covered too).
static uint32_t arenaSizeFor(const uint8_t *data, size_t size, uint32_t bodyOffset)
{
    uint32_t h = 2166136261u;
    for(size_t i = 0; i < size; i++) { h ^= data[i]; h *= 16777619u; }

    const uint32_t estimate = 8u * (uint32_t)(size - bodyOffset);
    static const uint32_t quarters[4] = {1, 2, 4, 16};
    uint32_t chosen = estimate / 4u * quarters[h & 3u];

    if(chosen < 32u) chosen = 32u;
    if(chosen > PRESSURE_ARENA_CAP) chosen = PRESSURE_ARENA_CAP;
    return chosen & ~3u;
}

// The storage buffer in LLVMFuzzerTestOneInput is sized off this.
static constexpr uint32_t MAX_PROC_COUNT = 16;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if(size == 0 || size > 4096) return 0;

    const Envelope env = readEnvelope(data, (uint32_t)size);
    const uint32_t procCount = env.procCount;

    if(procCount == 0 || procCount > MAX_PROC_COUNT) return 0;
    static_assert(sizeof(Runtime) + (MAX_PROC_COUNT + 1) * sizeof(ProcSlot) <= 512,
        "grow this buffer if Runtime/ProcSlot grow, or if MAX_PROC_COUNT rises");
    alignas(8) uint8_t storage[512] = {};
    CodeArena arena = CodeArena::region(arenaBase(), ARENA_CAPACITY, /*stackLimit=*/0);
    Runtime &rt = *new(storage) Runtime(procCount, arena);
    BcReader wire = wireAtBodies(data, (uint32_t)size, env.bodyOffset);
    if(rt.loadProgram(wire) != 0)
    {
        return 0; // the JIT's own static ceiling rejected it -- graceful, not a bug
    }

    // ── pass 1: one procedure at a time, under no arena pressure ───────
    //
    // Every procedure, not just the entry one: a CALL site's own
    // translation reads the *callee's* slot (argCount, for the argument
    // shuffle and the dispatch-table offset), so the interesting
    // interaction is between procedures, and the callee is only ever
    // reached by translating it in its own right. This is also what the
    // real runtime does over an execution's lifetime, one dispatch at a
    // time.
    //
    // Re-initialised per procedure because there is no detached Assembler
    // any more -- every translation emits into a real Runtime's arena, and
    // a bail leaves that arena's bookkeeping mid-update. Throwing the
    // whole Runtime away between procedures is what keeps one procedure's
    // bail from hiding every procedure after it.
    memset(g_arena, 0, ARENA_CAPACITY);

    for(uint32_t i = 0; i < procCount; i++)
    {
        arena = CodeArena::region(arenaBase(), ARENA_CAPACITY, /*stackLimit=*/0);
        new(storage) Runtime(procCount, arena);
        wire = wireAtBodies(data, (uint32_t)size, env.bodyOffset);
        rt.loadProgram(wire); // already known to succeed

        if(setjmp(g_resourceEscape) == 0)
        {
            rt.slot(i).lastUsed = 0; // callHelper's stamp, which nothing here emits
            translateProc(i, rt, /*lruTick=*/1);
        }
        // else: Assembler::fail() -> runtimeBail() -> here. The JIT bailed
        // with RESOURCE_ERROR instead of crashing -- the other acceptable
        // outcome for a validator-approved program, not a finding.
    }

    // ── pass 2: the same procedures against a deliberately tight arena ──
    //
    // Pass 1 hands every translation more room than it can use, which
    // leaves the whole runtime half of the arena untouched:
    // findEvictionVictim, evict's compaction memmove and codePtr slides,
    // and the literal pool under genuine capacity pressure. This pass
    // drives exactly that, as close to what the real dispatch path does as
    // a host build can get: compile a slot only when it is cold, one
    // procedure at a time, with the LRU tick advancing so
    // findEvictionVictim's age comparison means something.
    //
    // Several rounds, not one: eviction only ever happens once the arena
    // is already full, so round 1 populates and later rounds are where a
    // procedure evicted out from under an earlier round gets recompiled
    // on top of a compacted arena.
    const uint32_t arenaSize = arenaSizeFor(data, size, env.bodyOffset);
    memset(g_arena, 0, arenaSize);

    arena = CodeArena::region(arenaBase(), arenaSize, /*stackLimit=*/0);
    new(storage) Runtime(procCount, arena);
    wire = wireAtBodies(data, (uint32_t)size, env.bodyOffset);
    if(rt.loadProgram(wire) != 0)
    {
        return 0;
    }

    // One escape for the whole pass, not one per procedure: a bail leaves
    // Runtime's arena bookkeeping mid-update, and production treats that
    // as the end of the whole excursion (RESOURCE_ERROR out of
    // enterProgram), never as something to continue from.
    if(setjmp(g_resourceEscape) == 0)
    {
        uint32_t lruTick = 1;
        for(uint32_t round = 0; round < 4; round++)
        {
            for(uint32_t i = 0; i < procCount; i++)
            {
                if(rt.isResident(i)) continue; // a dispatch only ever lands on a cold slot

                rt.slot(i).lastUsed = lruTick++; // callHelper's stamp, which nothing here emits
                translateProc(i, rt, lruTick);
            }
        }
    }

    return 0;
}

// The batch reader. One file, the same one src/qemu-exec/ is handed, so a
// campaign feeds both sinks from a single artefact.
#ifndef PPL_FUZZ_LIBFUZZER_BUILD
#include <fstream>
#include <vector>

static constexpr uint32_t BATCH_MAGIC = 0x50504C42u; /* "PPLB" */

static uint32_t readU32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int main(int argc, char **argv)
{
    if(argc < 2)
    {
        fprintf(stderr, "usage: %s <batch-file>\n", argv[0]);
        return 1;
    }

    std::ifstream f(argv[1], std::ios::binary);
    if(!f) { fprintf(stderr, "fuzz: cannot open %s\n", argv[1]); return 1; }
    const std::vector<uint8_t> batch((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    if(batch.size() < 8 || readU32(batch.data()) != BATCH_MAGIC)
    {
        fprintf(stderr, "fuzz: %s is not a batch (magic mismatch)\n", argv[1]);
        return 1;
    }

    const uint32_t count = readU32(batch.data() + 4);
    size_t at = 8;
    uint32_t ran = 0;

    for(uint32_t i = 0; i < count; i++)
    {
        if(at + 8 > batch.size()) { fprintf(stderr, "fuzz: batch truncated at program %u\n", i); return 1; }
        const uint32_t length = readU32(batch.data() + at);
        const uint32_t argCount = readU32(batch.data() + at + 4);
        at += 8 + 4 * (size_t)argCount;

        if(at + length > batch.size()) { fprintf(stderr, "fuzz: batch truncated at program %u\n", i); return 1; }

        // An exact-sized copy, so a read past the program's own end lands in
        // an ASan redzone rather than in the next program.
        std::vector<uint8_t> program(batch.begin() + (long)at, batch.begin() + (long)(at + length));
        at += length;

        LLVMFuzzerTestOneInput(program.data(), program.size());
        ran++;
    }

    fprintf(stderr, "fuzz: %u program(s) translated, no crash\n", ran);
    return 0;
}
#endif
