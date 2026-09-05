#include "proc_scan.h"
#include "stack_budget.h"
#include "ext.h"
#include "decode_instr.h"
#include "instr.h"

#include <cassert>

namespace jitc
{
static bool triggersLRSave(const Instr &instr)
{
    return instr.op == Op::CALL
        || (instr.op == Op::BR_TABLE && (uint32_t)instr.imm >= 2)
        || instr.op == Op::CLZ
        || instr.op == Op::REVBITS;
}

/* One open block-nesting level. Both openers are just a count of closers
 * still to come: `N + 1` for a `BR_TABLE`'s cases plus its default case
 * (isa-core.md §4.5), two for a loop's body and condition sub-blocks. The
 * two close identically; they differ only in what §8.5 lets close them,
 * which is why `dispatch` exists and why nothing but the assertions reads
 * it. */
struct ScanFrame
{
    uint32_t remaining;
    bool dispatch;
};

static void GUARDED_scanBody(BcReader &r, bool &needsLRSave, ScanFrame *frame, uint32_t stackFloor, bool &stop, bool &foundEnd, uint32_t &failCode)
{
    register uint32_t sp asm("sp");
    if(sp < SCAN_STACK_MARGIN || sp - SCAN_STACK_MARGIN < stackFloor)
    {
        stop = true; // foundEnd stays false — caller reports !ok
        failCode = RESOURCE_EXHAUSTED_SCAN_STACK;
        return;
    }

    while(!r.atEnd())
    {
        Instr instr;
        if(!decodeInstr(r.next(), r, instr))
        {
            stop = true;
            failCode = RESOURCE_PROGRAM_RESERVED_OPCODE;
            return;
        }

        if(instr.op == Op::EXT)
        {
            /* The only place an extension's operands are stepped over, and
             * the only place its declaration is read at all. */
            uint32_t desc = 0;
            if(!extDescribe((uint8_t)instr.extOpcode, r, &desc))
            {
                stop = true;
                failCode = RESOURCE_PROGRAM_EXT_UNKNOWN;
                return;
            }

            /* Executor::run's budget rests on the first of these: a helper
             * that could reach a dispatch would nest under the translator. */
            if(extDescHas(desc, EXT_FLAG_CALL_SHAPED) || extDescTosDelta(desc) > 0)
            {
                stop = true;
                failCode = RESOURCE_PROGRAM_EXT_UNSUPPORTED;
                return;
            }

            if(extDescHas(desc, EXT_FLAG_NEEDS_LR))
            {
                needsLRSave = true;
            }
            continue;
        }

        if(triggersLRSave(instr))
        {
            needsLRSave = true;
        }

        if(instr.op == Op::BR_TABLE || isLoopOpener(instr.op))
        {
            ScanFrame inner = instr.op == Op::BR_TABLE
                ? ScanFrame{(uint32_t)instr.imm + 1, true}
                : ScanFrame{2, false};
            GUARDED_scanBody(r, needsLRSave, &inner, stackFloor, stop, foundEnd, failCode);
            if(stop) return;
            continue;
        }

        if(instr.op == Op::FALLTHROUGH || instr.op == Op::DEFAULT)
        {
            // Closes this case and continues into another one, so the frame
            // stays open (isa-core.md §4.5). `DEFAULT` jumps straight to the
            // last case, but every case between still has its own bytes to
            // walk, so the count moves by one either way.
            assert(frame != nullptr && frame->dispatch && frame->remaining > 1); // GCOV_EXCL_LINE — malformed input
            frame->remaining--;
            continue;
        }

        if(instr.op == Op::BLOCK_END)
        {
            assert(frame != nullptr); // GCOV_EXCL_LINE — malformed input: BLOCK_END with no open block
            if(--frame->remaining == 0) return; // construct fully closed — unwind to the enclosing level
            continue;
        }

        if(isProcTerminator(instr))
        {
            if(frame == nullptr)
            {
                stop = true;
                foundEnd = true;
                return;
            }
            // §8.5: a terminator closes a dispatch case, or a loop's *body*
            // sub-block — the first of the two, so `remaining` is still 2
            // there. A loop's condition (the second) needs a BLOCK_END.
            assert(frame->dispatch || frame->remaining == 2); // GCOV_EXCL_LINE — malformed input
            if(--frame->remaining == 0) return;
            continue;
        }
    }
}

BodyScanResult scanProcBody(BcReader &r, uint32_t stackFloor)
{
    const uint32_t before = r.remaining();
    bool needsLRSave = false;
    bool stop = false;
    bool foundEnd = false;
    uint32_t failCode = 0;

    GUARDED_scanBody(r, needsLRSave, nullptr, stackFloor, stop, foundEnd, failCode);

    if(!foundEnd && failCode == 0)
    {
        failCode = RESOURCE_PROGRAM_BODY_UNTERMINATED;
    }

    return BodyScanResult{before - r.remaining(), needsLRSave, failCode == 0, failCode};
}

} // namespace jitc
