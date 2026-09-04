#include "assembler.h"
#include "window.h"
#include "accstate.h"

#include "decode_instr.h"

#include <cassert>

namespace jitc
{

enum class BranchWidth { Narrow, Wide };

struct Ctx
{
    Assembler a;
    Window window;
    BcReader body;
    uint32_t procIdx;
    bool savesLR;
    uint32_t initialSpilledCount;

    AccState accState;

    /* One instruction decoded ahead of the loop and not yet consumed, owned
     * by peek()/consume() alone. A lookahead that declines to fold leaves it
     * here rather than rewinding, so no byte is ever read off the wire
     * twice. An Op::EXT lookahead has its operands still unread — the cursor
     * sits mid-instruction until the emitter takes it. */
    Instr lookahead;
    bool hasLookahead;

    Ctx(Runtime& r, uint32_t procIdx, uint32_t lruTick);

    void localJumpCleanup(uint32_t tos);
    void handleGlobalJump(Instr term, uint32_t tos);

    Effect handleComparisonEmission(const Instr &instr);

    /** The next instruction, left standing until something consumes it.
     *  Null at end-of-body. */
    const Instr *peek();

    /** Takes what peek() left standing; not calling it is how a fold
     *  declines, leaving the instruction for the next round. */
    Instr consume()
    {
        assert(this->hasLookahead); // GCOV_EXCL_LINE
        this->hasLookahead = false;
        return this->lookahead;
    }

    /** The register a following `STORE` folds the result into — consuming that
     *  `STORE` when there is one — or `otherwise` where the result would land
     *  anyway. */
    uint32_t peekStoreFold(uint32_t otherwise);

    /* Out of line on purpose: its ExtSite would otherwise sit in
     * GUARDED_processUntilTerminator's frame on every path through the switch. */
    __attribute__((noinline)) void handleExt(uint8_t opcode);

    bool GUARDED_processUntilTerminator(BranchWidth width, bool isThisLoopCondBlock, Instr &out);
    bool translateLoop(BranchWidth width);
    bool translateIfThen(BranchWidth width);
    bool translateIfThenElse(BranchWidth width);
    bool translateSwitch(BranchWidth width, uint32_t n);
    bool translateBody(BranchWidth width);
};

}
