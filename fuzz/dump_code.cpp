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

#include "proc.h"
#include "assembler.h"
#include "translate_proc.h"
#include "decode_instr.h"
#include "runtime_internal.h"

using namespace jitc;

static jmp_buf g_escape;
extern "C" [[noreturn]] void runtimeBail(Runtime *, uint32_t) { longjmp(g_escape, 1); }
extern const uint32_t trampolineAddr = 0xDEADBEEFu;

int main(int argc, char **argv)
{
    if(argc < 2) { fprintf(stderr, "usage: %s <program-file> [out-prefix]\n", argv[0]); return 1; }
    const char *prefix = argc > 2 ? argv[2] : "code";

    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> in((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if(in.empty()) { fprintf(stderr, "empty input\n"); return 1; }
    const uint8_t *data = in.data();

    uint32_t pos = 0;
    jitc::decodeLeb128(data, pos, pos); // max_call_depth
    jitc::decodeLeb128(data, pos, pos); // total_depth
    const uint32_t procCount = jitc::decodeLeb128(data, pos, pos);
    const uint32_t bodyOffset = pos;

    alignas(8) uint8_t storage[1024] = {};
    Runtime &rt = *reinterpret_cast<Runtime *>(storage);
    if(!rt.init(data, (uint32_t)in.size(), bodyOffset, procCount, 0x10000, 0x10000, 0, 0))
    {
        fprintf(stderr, "Runtime::init rejected this program\n");
        return 1;
    }

    uint32_t procPos = bodyOffset;
    for(uint32_t i = 0; i < procCount; i++)
    {
        uint32_t argCount = jitc::decodeLeb128(data, procPos, procPos);
        const uint32_t bodyBytes = rt.slot(i).bodyBytes();
        Proc proc{argCount, data + procPos, bodyBytes};
        procPos += bodyBytes;

        static uint16_t buf[8192];
        Assembler a(buf, sizeof(buf) / sizeof(buf[0]));
        uint32_t halfwords = 0;
        bool bailed = false;
        if(setjmp(g_escape) == 0) halfwords = translateProc(proc, i, a, rt);
        else bailed = true;

        char path[512];
        snprintf(path, sizeof(path), "%s.proc%u.bin", prefix, i);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write((const char *)buf, (std::streamsize)halfwords * 2);
        printf("proc %u: argCount=%u bodyBytes=%u needsLRSave=%d -> %u halfwords (%u bytes)%s -> %s\n",
            i, argCount, bodyBytes, (int)rt.slot(i).needsLRSave(), halfwords, halfwords * 2,
            bailed ? " BAILED" : "", path);
    }
    return 0;
}
