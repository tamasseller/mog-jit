// Re-encodes ../test/corpus_programs.h's single-procedure bodies -- the
// same Instr[] shapes test/qemu/program_tests.cpp exercises on real hardware --
// as one arg_count:LEB128 immediately followed by that procedure's own body
// bytes, and writes them to seeds_raw/ as *staging* input for make_seeds.ts.
//
// Staging, not seeds: harness.cpp takes whole programs now (the
// max_call_depth/total_depth/proc_count envelope of bytecode.ts's
// encodeJitProgram), and the two envelope stats come out of
// validateProgram's own whole-program DFS, which exists only on the TS
// side. So this file produces the bodies and make_seeds.ts wraps them;
// seeds/ has exactly one owner, and nothing writes a format the harness
// would silently discard on every execution.
//
// Only single-procedure programs qualify: this staging format has no second
// body to point a CALL's own calleeIndex at, so corpusLargeBrTable* (two
// procedures) is left out -- make_seeds.ts authors its own
// multi-procedure/CALL shapes instead.
//
// A plain host program, no runtime/target dependency, matching build.sh's
// own compiler invocation style.
#include <cstdint>
#include <cstdio>
#include "instr.h"
#include "encode_instr.h"
#include "corpus_programs.h"

using namespace jitc;

namespace
{

struct SeedEntry
{
    const char *name;
    const Instr *body;
    uint32_t bodyCount;
    uint32_t argCount;
};

const SeedEntry entries[] = {
    {"nested_loop", corpusNestedLoopProc0, sizeof(corpusNestedLoopProc0) / sizeof(Instr), 1},
    {"br_table_in_loop", corpusBrTableInLoopProc0, sizeof(corpusBrTableInLoopProc0) / sizeof(Instr), 1},
    {"loop_in_br_table", corpusLoopInBrTableProc0, sizeof(corpusLoopInBrTableProc0) / sizeof(Instr), 1},
    {"deep_operand_stack", corpusDeepStackProc0, sizeof(corpusDeepStackProc0) / sizeof(Instr), 0},
};

bool writeFile(const char *path, const uint8_t *bytes, uint32_t len)
{
    FILE *f = std::fopen(path, "wb");
    if(!f)
    {
        std::perror(path);
        return false;
    }
    bool ok = std::fwrite(bytes, 1, len, f) == len;
    std::fclose(f);
    return ok;
}

} // namespace

int main()
{
    uint8_t buf[512];
    bool ok = true;
    for(const SeedEntry &e : entries)
    {
        uint32_t len = 0;
        encodeLeb128(e.argCount, buf, len, sizeof(buf));
        len += encodeBody(e.body, e.bodyCount, buf + len, sizeof(buf) - len);

        char path[256];
        std::snprintf(path, sizeof(path), "seeds_raw/%s", e.name);
        if(!writeFile(path, buf, len))
        {
            ok = false;
            continue;
        }
        std::printf("staged %s (%u bytes)\n", path, len);
    }
    return ok ? 0 : 1;
}
