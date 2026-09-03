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

    ArmV6M::Condition handleComparisonEmission(const Instr &instr);

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

    /** The window register a following `STORE` folds into, or -1 — consuming
     *  that `STORE` when it does. */
    int32_t peekStoreFold();

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

/** `fold`'s register if it took, `otherwise` if it declined. */
inline uint32_t foldDest(int32_t fold, uint32_t otherwise)
{
    return fold >= 0 ? (uint32_t)fold : otherwise;
}

}
