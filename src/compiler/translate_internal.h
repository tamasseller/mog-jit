#include "assembler.h"
#include "window.h"
#include "accstate.h"

#include "decode_instr.h"

namespace jitc
{

enum class BranchWidth { Narrow, Wide };

struct Ctx
{
    Assembler a;
    Window window;
    const uint8_t *bytes;
    uint32_t bytesLen;
    uint32_t procIdx;
    bool savesLR;
    uint32_t initialSpilledCount;

    AccState accState;

    Ctx(Runtime& r, uint32_t procIdx, uint32_t lruTick);

    void localJumpCleanup(uint32_t tos);
    void handleGlobalJump(Instr term, uint32_t tos);

    ArmV6M::Condition handleComparisonEmission(const Instr &instr);

    /* Out of line on purpose: its ExtSite would otherwise sit in
     * GUARDED_processUntilTerminator's frame on every path through the switch. */
    __attribute__((noinline)) void handleExt(const Instr &instr, uint32_t pc);

    bool GUARDED_processUntilTerminator(uint32_t pc, BranchWidth width, bool isThisLoopCondBlock, DecodedInstr &out);
    uint32_t translateLoop(uint32_t pc, BranchWidth width);
    uint32_t translateIfThen(uint32_t pc, BranchWidth width);
    uint32_t translateIfThenElse(uint32_t pc, BranchWidth width);
    uint32_t translateSwitch(uint32_t pc, BranchWidth width, uint32_t n);
    bool translateBody(BranchWidth width);
};

}