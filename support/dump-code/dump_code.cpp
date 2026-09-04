// Diagnostic: translate one whole-program envelope (the fuzz input format)
// on the host and write each procedure's emitted Thumb to a raw .bin, for
// arm-none-eabi-objdump to disassemble. dump_code.sh does both steps.
//
// The one thing neither half of fuzz/ can tell you on its own: *what* the
// translator emitted. The host harness says whether it crashed, qemu_exec
// says whether the answer was wrong — reading the actual instructions is
// how you find out why.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <csetjmp>
#include <fstream>
#include <vector>

#include "translate_proc.h"
#include "decode_instr.h"
#include "runtime.h"
#include "envelope.h"

using namespace jitc;

static jmp_buf g_escape;
// Records the code, unlike the other drivers' stubs: this tool is what
// target-profile.md cites for the argCount ceiling measurement, and which
// RESOURCE_* code a bail reported is exactly what that measurement claims.
static uint32_t g_bailCode = 0;
extern "C" [[noreturn]] void runtimeBail(Runtime *, uint32_t code) { g_bailCode = code; longjmp(g_escape, 1); }
extern const uint32_t trampolineAddr = 0xDEADBEEFu;

// Generous on purpose: a diagnostic wants the code a procedure really
// compiles to, never the arena pressure harness.cpp's pass 2 goes looking
// for. Ordinary static storage works because this is an -m32 build
// (dump_code.sh), so a real address fits the uint32_t Runtime keeps it in.
static constexpr uint32_t ARENA_CAPACITY = 65536u;
alignas(8) static uint8_t g_arena[ARENA_CAPACITY];

int main(int argc, char **argv)
{
    if(argc < 2) { fprintf(stderr, "usage: %s <program-file> [out-prefix]\n", argv[0]); return 1; }
    const char *prefix = argc > 2 ? argv[2] : "code";

    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> in((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if(in.empty()) { fprintf(stderr, "empty input\n"); return 1; }
    const uint8_t *data = in.data();

    const Envelope env = readEnvelope(data, (uint32_t)in.size());
    const uint32_t procCount = env.procCount;
    const uint32_t bodyOffset = env.bodyOffset;

    alignas(8) uint8_t storage[1024] = {};
    const uint32_t arenaBase = (uint32_t)(uintptr_t)g_arena;

    CodeArena arena = CodeArena::region(arenaBase, ARENA_CAPACITY, /*stackLimit=*/0);
    Runtime &rt = *new(storage) Runtime(procCount, arena);
    BcReader wire = wireAtBodies(data, (uint32_t)in.size(), bodyOffset);
    if(uint32_t code = rt.loadProgram(wire); code != 0)
    {
        fprintf(stderr, "Runtime::loadProgram rejected this program: %08x\n", code);
        return 1;
    }

    for(uint32_t i = 0; i < procCount; i++)
    {
        // A fresh Runtime per procedure so each one's code starts at the
        // arena base and the .bin below is that procedure alone.
        memset(g_arena, 0, ARENA_CAPACITY);
        arena = CodeArena::region(arenaBase, ARENA_CAPACITY, /*stackLimit=*/0);
        new(storage) Runtime(procCount, arena);
        wire = wireAtBodies(data, (uint32_t)in.size(), bodyOffset);
        rt.loadProgram(wire);

        const uint32_t argCount = rt.slot(i).argCount();
        const uint32_t bodyBytes = rt.slot(i).bodyBytes();

        uint32_t halfwords = 0;
        bool bailed = false;
        g_bailCode = 0;
        rt.slot(i).lastUsed = 0; // callHelper's stamp
        if(setjmp(g_escape) == 0) halfwords = translateProc(i, rt, /*lruTick=*/1);
        else bailed = true;

        const uint16_t *code = (const uint16_t *)(uintptr_t)(rt.slot(i).codePtr & ~1u);

        char path[512];
        snprintf(path, sizeof(path), "%s.proc%u.bin", prefix, i);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if(!bailed) out.write((const char *)code, (std::streamsize)halfwords * 2);
        char bail[32] = "";
        if(bailed) snprintf(bail, sizeof(bail), " BAILED %08x", g_bailCode);
        printf("proc %u: argCount=%u bodyBytes=%u needsLRSave=%d -> %u halfwords (%u bytes)%s -> %s\n",
            i, argCount, bodyBytes, (int)rt.slot(i).needsLRSave(), halfwords, halfwords * 2,
            bail, path);
    }
    return 0;
}
