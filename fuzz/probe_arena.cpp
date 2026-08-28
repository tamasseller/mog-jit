// Diagnostic, not part of a fuzzing run: proves the attached-Assembler
// pass in harness.cpp is actually reaching the runtime's arena machinery
// (allocate / findEvictionVictim / evict's compaction memmove / finalize's
// dispatch registration) rather than quietly bailing on the first
// procedure. Reports, per arena size, how many procedures ended up
// resident and how far arenaCursor moved, plus whether the run bailed.
//
// Build: see probe_arena.sh.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <csetjmp>
#include <fstream>
#include <vector>
#include <sys/mman.h>

#include "proc.h"
#include "assembler.h"
#include "translate_proc.h"
#include "decode_instr.h"
#include "runtime_internal.h"

using namespace jitc;

static jmp_buf g_escape;
extern "C" [[noreturn]] void runtimeBail(Runtime *, uint32_t) { longjmp(g_escape, 1); }
extern const uint32_t trampolineAddr = 0xDEADBEEFu;

static constexpr uint32_t ARENA_BASE = 0x30000000u;
static constexpr uint32_t ARENA_CAPACITY = 8192u;

int main(int argc, char **argv)
{
    if(argc < 2) { fprintf(stderr, "usage: %s <seed-file>...\n", argv[0]); return 1; }

    void *p = mmap((void *)(uintptr_t)ARENA_BASE, ARENA_CAPACITY, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if(p == MAP_FAILED || (uintptr_t)p != ARENA_BASE) { fprintf(stderr, "mmap failed\n"); return 2; }

    // harness.cpp's own arenaSizeFor multipliers, over its own
    // 8-bytes-of-code-per-bytecode-byte estimate.
    static const uint32_t quarters[4] = {1, 2, 4, 16};
    static const uint32_t nsizes = 4;

    for(int ai = 1; ai < argc; ai++)
    {
        std::ifstream f(argv[ai], std::ios::binary);
        std::vector<uint8_t> in((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if(in.empty()) continue;
        const uint8_t *data = in.data();

        uint32_t pos = 0;
        jitc::decodeLeb128(data, pos, pos); // max_call_depth
        jitc::decodeLeb128(data, pos, pos); // total_depth
        uint32_t procCount = jitc::decodeLeb128(data, pos, pos);
        uint32_t bodyOffset = pos;

        printf("%-32s procCount=%u\n", argv[ai], procCount);

        for(uint32_t si = 0; si < nsizes; si++)
        {
            const uint32_t estimate = 8u * (uint32_t)(in.size() - bodyOffset);
            uint32_t chosen = estimate / 4u * quarters[si];
            if(chosen < 32u) chosen = 32u;
            if(chosen > ARENA_CAPACITY) chosen = ARENA_CAPACITY;
            const uint32_t arenaSize = chosen & ~3u;

            alignas(8) uint8_t storage[512] = {};
            Runtime &rt = *reinterpret_cast<Runtime *>(storage);
            memset((void *)(uintptr_t)ARENA_BASE, 0, arenaSize);
            if(uint32_t code = rt.init(data, (uint32_t)in.size(), bodyOffset, procCount,
                ARENA_BASE, arenaSize, 0, 0); code != 0)
            {
                printf("    arena %5u: init rejected %08x\n", arenaSize, code);
                continue;
            }

            bool bailed = false;
            uint32_t compiles = 0;
            if(setjmp(g_escape) == 0)
            {
                uint32_t lruTick = 1;
                for(uint32_t round = 0; round < 4; round++)
                {
                    uint32_t ap = bodyOffset;
                    for(uint32_t i = 0; i < procCount; i++)
                    {
                        uint32_t argCount = jitc::decodeLeb128(data, ap, ap);
                        const uint8_t *body = data + ap;
                        uint32_t bodyBytes = rt.slot(i).bodyBytes();
                        ap += bodyBytes;
                        if(rt.isResident(i)) continue;
                        Proc proc{argCount, body, bodyBytes};
                        Assembler a(&rt, i, lruTick++);
                        translateProc(proc, i, a, rt);
                        compiles++;
                    }
                }
            }
            else
            {
                bailed = true;
            }

            uint32_t resident = 0;
            for(uint32_t i = 0; i < procCount; i++) if(rt.isResident(i)) resident++;
            printf("    arena %5u: compiles=%u resident=%u/%u cursorUsed=%u%s\n",
                arenaSize, compiles, resident, procCount,
                rt.arenaCursor - ARENA_BASE, bailed ? " BAILED" : (compiles > procCount ? " EVICTED" : ""));
        }
    }
    return 0;
}
