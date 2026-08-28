// Regenerates the diverse-shape entries in seeds/ from
// ../test/corpus_programs.h -- the same Instr[] bodies
// test/qemu/fixtures.cpp exercises on real hardware, re-encoded here to
// exactly the single-procedure wire format harness.cpp's own
// LLVMFuzzerTestOneInput expects: one arg_count:LEB128 immediately
// followed by that procedure's own body bytes, nothing else (no
// proc_count, no envelope) -- see harness.cpp's own
// `decodeLeb128(data, 0, bodyOffset)` at offset 0. Only single-procedure
// programs qualify: this format has no second body to point a CALL's own
// calleeIndex at, so corpusLargeBrTable* (two procedures) is left out.
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
        std::snprintf(path, sizeof(path), "seeds/%s", e.name);
        if(!writeFile(path, buf, len))
        {
            ok = false;
            continue;
        }
        std::printf("wrote %s (%u bytes)\n", path, len);
    }
    return ok ? 0 : 1;
}
