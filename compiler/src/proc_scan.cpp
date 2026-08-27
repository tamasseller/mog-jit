#include "proc_scan.h"
#include "decode_instr.h"
#include "instr.h"

#include <cassert>

namespace jitc
{

// This function's own frame is tiny (no Emitter/Window/AccState, just a
// couple of scalars) — far less than translate_proc.cpp's own
// TRANSLATE_BODY_STACK_MARGIN, whose figure is dominated by exactly that
// state.
static constexpr uint32_t SCAN_STACK_MARGIN = 128;

static bool triggersLRSave(const Instr &instr)
{
    // Unsigned compare: instr.imm (int32_t) carries N's raw bit pattern,
    // and translate_proc.cpp's own dispatch passes it to translateSwitch's
    // uint32_t n exactly as-is — an N large enough to read as negative
    // here must still agree that it's ">2" (translateSwitch's helper-
    // vector path, which clobbers lr), or needsLRSave comes out false for
    // a body that actually goes on to clobber it.
    return instr.op == Op::CALL
        || (instr.op == Op::BR_TABLE && (uint32_t)instr.imm > 2)
        || instr.op == Op::CLZ
        || instr.op == Op::REVBITS;
}

enum class ScanFrameKind : uint8_t { Case, LoopCond, LoopBody };

struct ScanFrame
{
    ScanFrameKind kind;
    uint32_t remaining; // Case only
};

// Mirrors translate_proc.cpp's own translateBody: one recursive call per
// open LOOP/BR_TABLE, this level's own frame state held as a plain local
// rather than pushed onto an explicit stack (packages/machine/src/
// bytecode.ts's decodeProcBody's own array-based stack, ported here as
// recursion instead). frame is null at the top level. pc/needsLRSave are
// threaded by reference — every level contributes to the same running
// answer, not just its own. `stop` and `foundEnd` are distinct: `stop`
// means every caller up the recursion should unwind immediately (for
// *either* reason below); `foundEnd` means the reason was a genuine
// top-level terminator, not an overflow — the two must stay separate so a
// stack overflow can never be misreported as a clean scan.
static void scanBody(const uint8_t *bytes, uint32_t maxBytes, uint32_t &pc, bool &needsLRSave, ScanFrame *frame, uint32_t stackFloor, bool &stop, bool &foundEnd)
{
    register uint32_t sp asm("sp");
    if(sp < SCAN_STACK_MARGIN || sp - SCAN_STACK_MARGIN < stackFloor)
    {
        stop = true; // foundEnd stays false — caller reports !ok
        return;
    }

    while(pc < maxBytes)
    {
        DecodedInstr d = decodeInstr(bytes, maxBytes, pc);
        if(triggersLRSave(d.instr))
        {
            needsLRSave = true;
        }
        pc = d.next;

        if(d.instr.op == Op::BR_TABLE)
        {
            ScanFrame inner{ScanFrameKind::Case, (uint32_t)d.instr.imm};
            scanBody(bytes, maxBytes, pc, needsLRSave, &inner, stackFloor, stop, foundEnd);
            if(stop) return;
            continue;
        }
        if(d.instr.op == Op::LOOP)
        {
            ScanFrame inner{ScanFrameKind::LoopCond, 0};
            scanBody(bytes, maxBytes, pc, needsLRSave, &inner, stackFloor, stop, foundEnd);
            if(stop) return;
            continue;
        }

        if(d.instr.op == Op::BLOCK_END)
        {
            assert(frame != nullptr); // GCOV_EXCL_LINE — malformed input: BLOCK_END with no open block
            if(frame->kind == ScanFrameKind::Case)
            {
                frame->remaining--;
                if(frame->remaining == 0) return; // this BR_TABLE fully closed — unwind to the enclosing level
            }
            else if(frame->kind == ScanFrameKind::LoopCond)
            {
                frame->kind = ScanFrameKind::LoopBody;
            }
            else
            {
                return; // LoopBody's own ordinary closer
            }
            continue;
        }

        if(d.instr.op == Op::RETURN || d.instr.op == Op::TRAP)
        {
            // Empty (frame == nullptr) *before* considering this terminator
            // at all is the real end. A frame closing right here (a loop's
            // body, or a BR_TABLE's own last case) closes *that*
            // construct, not necessarily the procedure — the enclosing
            // scope's own bytes (a loop's cond-false exit path, a
            // BR_TABLE's own shared-end tail) may still follow, exactly as
            // they would after an ordinary BLOCK_END close. A LOOP/BR_TABLE
            // with nothing at all following it is validator-rejected
            // (isa-core.md §8.4), so this never has to tell "really done"
            // apart from "just closed one level" on its own — the *next*
            // terminator reached with frame == nullptr always settles it.
            if(frame == nullptr)
            {
                stop = true;
                foundEnd = true;
                return;
            }
            if(frame->kind == ScanFrameKind::LoopBody)
            {
                return;
            }
            if(frame->kind == ScanFrameKind::Case)
            {
                // blocks.cpp's resolveCaseClose: a bare terminator closes a
                // case exactly like a BLOCK_END would, counting against the
                // same N case-closers (isa-core.md §8.5) — mirrors the fix
                // to bytecode.ts's own decodeProcBody, which originally
                // missed this.
                frame->remaining--;
                if(frame->remaining == 0) return;
            }
            // LoopCond: a bare terminator can't legally close a condition
            // sub-block — fall through; malformed input surfaces as
            // running off maxBytes below instead.
            continue;
        }
    }
    // Ran off maxBytes without finding this level's own close: foundEnd
    // stays false, so the top-level caller reports !ok.
}

BodyScanResult scanProcBody(const uint8_t *bytes, uint32_t maxBytes, uint32_t startOffset, uint32_t stackFloor)
{
    uint32_t pc = startOffset;
    bool needsLRSave = false;
    bool stop = false;
    bool foundEnd = false;
    scanBody(bytes, maxBytes, pc, needsLRSave, nullptr, stackFloor, stop, foundEnd);
    return BodyScanResult{pc - startOffset, needsLRSave, foundEnd};
}

} // namespace jitc
