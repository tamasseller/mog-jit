// The bytecode accessor seam (runtime/bytecode.h): whether a program is
// addressable is the accessor's business and nothing else's. Every program in
// the shared corpus is loaded and translated twice — once memory-mapped, once
// out of a block-buffered store eight bytes at a time, where a handle is an
// offset and no part of the program is ever addressable — and the emitted
// Thumb has to come out identical.
#include "Test.h"

#include "bc_buffered.h"
#include "bytecode_default.h"
#include "corpus_programs.h"
#include "encode_instr.h"
#include "host_runtime_support.h"
#include "runtime.h"
#include "translate_proc.h"
#include "wire.h"

using namespace jitc;

namespace
{
constexpr uint32_t MAX_PROCS = 2;
constexpr uint32_t MAX_HALFWORDS = 512;

struct Corpus
{
    const char *name;
    ProcSource procs[MAX_PROCS];
    uint32_t procCount;
};

#define ONE(body) ProcSource{1, body, sizeof(body) / sizeof(body[0])}

const Corpus CORPUS[] = {
    {"nested-loop", {ONE(corpusNestedLoopProc0)}, 1},
    {"br-table-in-loop", {ONE(corpusBrTableInLoopProc0)}, 1},
    {"loop-in-br-table", {ONE(corpusLoopInBrTableProc0)}, 1},
    {"large-br-table", {ONE(corpusLargeBrTableProc0), ONE(corpusLargeBrTableProc1)}, 2},
    {"deep-stack", {ONE(corpusDeepStackProc0)}, 1},
};

#undef ONE

constexpr uint32_t CORPUS_COUNT = sizeof(CORPUS) / sizeof(CORPUS[0]);

/* What one run of the whole pipeline produced: every procedure's emitted
 * halfwords, end to end, plus what the walk decided about each. */
struct Emitted
{
    uint32_t halfwords[MAX_HALFWORDS];
    uint32_t count = 0;
    uint32_t bodyBytes[MAX_PROCS] = {};
    bool needsLRSave[MAX_PROCS] = {};
    bool ok = false;
};

/* The real path, start to finish: loadProgram walks the directory off one
 * cursor and pins each body by the handle bcTell hands it, then translateProc
 * reopens that handle. Nothing here invents a handle or reads a byte itself,
 * which is what makes the two accessors comparable at all. */
void translateWhole(Emitted &out, BcHandle program, uint32_t len, uint32_t procCount)
{
    LowMemory low{1u << 20};
    alignas(8) uint8_t storage[sizeof(Runtime) + (MAX_PROCS + 1) * sizeof(ProcSlot)] = {};

    for(uint32_t i = 0; i < procCount; i++)
    {
        // A fresh arena per procedure, so each one's code starts at the
        // arena base and the halfwords below are that procedure alone.
        const uint32_t arenaBase = low.alloc(4096);
        CodeArena arena = CodeArena::region(arenaBase, 4096, /*stackLimit=*/0);
        Runtime &rt = *new(storage) Runtime(procCount, arena);

        for(uint32_t k = 0; k < procCount; k++)
        {
            rt.slot(k).codePtr = trampolineAddr;
        }

        BcReader wire;
        wire.open(program, len);

        uint32_t procCountOnWire = 0;
        decodeLeb128(wire, procCountOnWire);

        if(rt.loadProgram(wire) != 0)
        {
            return; // out.ok stays false
        }

        out.bodyBytes[i] = rt.slot(i).bodyBytes();
        out.needsLRSave[i] = rt.slot(i).needsLRSave();

        rt.slot(i).lastUsed = 0;
        const uint32_t n = translateProc(i, rt, /*lruTick=*/1);

        const uint16_t *code = low.code(rt.slot(i).codePtr & ~1u);
        for(uint32_t k = 0; k < n && out.count < MAX_HALFWORDS; k++)
        {
            out.halfwords[out.count++] = code[k];
        }
    }

    out.ok = true;
}

bool sameEmission(const Emitted &a, const Emitted &b, uint32_t procCount)
{
    if(!a.ok || !b.ok || a.count != b.count)
    {
        return false; // GCOV_EXCL_LINE — only reached by a failing comparison
    }

    for(uint32_t i = 0; i < procCount; i++)
    {
        if(a.bodyBytes[i] != b.bodyBytes[i] || a.needsLRSave[i] != b.needsLRSave[i])
        {
            return false; // GCOV_EXCL_LINE — ditto
        }
    }

    for(uint32_t i = 0; i < a.count; i++)
    {
        if(a.halfwords[i] != b.halfwords[i])
        {
            return false; // GCOV_EXCL_LINE — ditto
        }
    }

    return true;
}
} // namespace

TEST(ABlockBufferedStoreCompilesToTheSameCodeAsAMappedOne)
{
    for(uint32_t c = 0; c < CORPUS_COUNT; c++)
    {
        const Corpus &prog = CORPUS[c];

        uint8_t bytes[512];
        const uint32_t len = encodeProgram(prog.procs, prog.procCount, bytes, sizeof(bytes));

        Emitted mapped;
        translateWhole(mapped, bcMapped(bytes), len, prog.procCount);
        CHECK(mapped.ok);
        CHECK(mapped.count > 0);

        // Offset 0 is the handle for the whole program; every other handle
        // in the run comes from bcTell, never from arithmetic here.
        bcBufferedAttach(bytes, len);
        Emitted buffered;
        {
            BcScope scope(&BC_BUFFERED);
            translateWhole(buffered, /*program=*/0, len, prog.procCount);
        }

        CHECK(sameEmission(mapped, buffered, prog.procCount));

        // The store really was serving one small block at a time: a run that
        // happened to fit the program in the buffer would prove nothing.
        CHECK(bcBufferedFetches() > len / BC_BLOCK_BYTES);
        CHECK(bcBufferedHints() > 0); // Ctx::Ctx hints every pass but the first scan
    }
}
