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
    if(instr.op == Op::EXT)
    {
        return extDeclHas(instr.extDecl, EXT_FLAG_NEEDS_LR);
    }
    return instr.op == Op::CALL
        || (instr.op == Op::BR_TABLE && (uint32_t)instr.imm >= 2)
        || instr.op == Op::CLZ
        || instr.op == Op::REVBITS;
}

/* One open block-nesting level. Both openers are just a count of closers
 * still to come: `N + 1` for a `BR_TABLE`'s cases plus its default case
 * (isa-core.md §4.5), two for a `LOOP`'s condition and body sub-blocks. The
 * two close identically; they differ only in what §8.5 lets close them,
 * which is why `dispatch` exists and why nothing but the assertions reads
 * it. */
struct ScanFrame
{
    uint32_t remaining;
    bool dispatch;
};

static void GUARDED_scanBody(const uint8_t *bytes, uint32_t maxBytes, uint32_t &pc, bool &needsLRSave, ScanFrame *frame, uint32_t stackFloor, bool &stop, bool &foundEnd, uint32_t &failCode)
{
    register uint32_t sp asm("sp");
    if(sp < SCAN_STACK_MARGIN || sp - SCAN_STACK_MARGIN < stackFloor)
    {
        stop = true; // foundEnd stays false — caller reports !ok
        failCode = RESOURCE_EXHAUSTED_SCAN_STACK;
        return;
    }

    while(pc < maxBytes)
    {
        if(bytes[pc] >= MISC_BASE && bytes[pc] <= LAST_CORE_OPCODE)
        {
            /* An escape's operand shape is defined only when its sub-code is
             * assigned (§5.3), so an unassigned one cannot even be skipped. */
            uint32_t sub = 0, next = 0;
            if(!decodeLeb128Checked(bytes, maxBytes, pc + 1, sub, next)
                || !miscSubCodeAssigned(bytes[pc], sub))
            {
                stop = true;
                failCode = RESOURCE_PROGRAM_RESERVED_OPCODE;
                return;
            }
        }

        if(bytes[pc] > LAST_CORE_OPCODE)
        {
            uint32_t decl = 0;
            if(extDecodeLength(bytes, maxBytes, pc, decl) == 0)
            {
                stop = true;
                failCode = RESOURCE_PROGRAM_EXT_UNKNOWN;
                return;
            }
            /* Executor::run's budget rests on the first of these: a helper
             * that could reach a dispatch would nest under the translator. */
            if(extDeclHas(decl, EXT_FLAG_CALL_SHAPED) || extDeclTosDelta(decl) > 0)
            {
                stop = true;
                failCode = RESOURCE_PROGRAM_EXT_UNSUPPORTED;
                return;
            }
        }

        DecodedInstr d = decodeInstr(bytes, maxBytes, pc);
        if(triggersLRSave(d.instr))
        {
            needsLRSave = true;
        }
        pc = d.next;

        if(d.instr.op == Op::BR_TABLE || d.instr.op == Op::LOOP)
        {
            ScanFrame inner = d.instr.op == Op::BR_TABLE
                ? ScanFrame{(uint32_t)d.instr.imm + 1, true}
                : ScanFrame{2, false};
            GUARDED_scanBody(bytes, maxBytes, pc, needsLRSave, &inner, stackFloor, stop, foundEnd, failCode);
            if(stop) return;
            continue;
        }

        if(d.instr.op == Op::FALLTHROUGH)
        {
            // Closes this case and continues into the next one, so the frame
            // stays open with one fewer case to go (isa-core.md §4.5).
            assert(frame != nullptr && frame->dispatch && frame->remaining > 1); // GCOV_EXCL_LINE — malformed input
            frame->remaining--;
            continue;
        }

        if(d.instr.op == Op::BLOCK_END)
        {
            assert(frame != nullptr); // GCOV_EXCL_LINE — malformed input: BLOCK_END with no open block
            if(--frame->remaining == 0) return; // construct fully closed — unwind to the enclosing level
            continue;
        }

        if(isProcTerminator(d.instr))
        {
            if(frame == nullptr)
            {
                stop = true;
                foundEnd = true;
                return;
            }
            // §8.5: a terminator closes a dispatch case, or a LOOP's *body*
            // sub-block — never a LOOP's condition, which needs a BLOCK_END.
            assert(frame->dispatch || frame->remaining == 1); // GCOV_EXCL_LINE — malformed input
            if(--frame->remaining == 0) return;
            continue;
        }
    }
}

BodyScanResult scanProcBody(const uint8_t *bytes, uint32_t maxBytes, uint32_t startOffset, uint32_t stackFloor)
{
    uint32_t pc = startOffset;
    bool needsLRSave = false;
    bool stop = false;
    bool foundEnd = false;
    uint32_t failCode = 0;

    GUARDED_scanBody(bytes, maxBytes, pc, needsLRSave, nullptr, stackFloor, stop, foundEnd, failCode);

    if(!foundEnd && failCode == 0)
    {
        failCode = RESOURCE_PROGRAM_BODY_UNTERMINATED;
    }
    
    return BodyScanResult{pc - startOffset, needsLRSave, failCode == 0, failCode};
}

} // namespace jitc
