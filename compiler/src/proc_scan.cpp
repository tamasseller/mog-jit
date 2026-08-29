#include "proc_scan.h"
#include "ext.h"
#include "decode_instr.h"
#include "instr.h"

#include <cassert>

namespace jitc
{

// Bounds scanBody's *own* recursion only — this runs before any Runtime/
// Assembler exists, so nothing else protects it. It is not, and was never
// meant to be, a proxy for the real translator's later recursion: this
// function's own frame is tiny (no Emitter/Window/AccState, just a couple
// of scalars), while translate_proc.cpp's translateLoop/translateIfThen/
// translateIfThenElse/translateSwitch carry exactly that state, so a depth
// this margin accepts can still exceed what real translation needs. That
// gap is closed directly at those four functions' own entry points
// (translate_proc.cpp's checkStackFloor, checked at every level of the
// real recursion) — not by trying to keep this number in sync with theirs.
static constexpr uint32_t SCAN_STACK_MARGIN = 128;

static bool triggersLRSave(const Instr &instr)
{
    // Unsigned compare: instr.imm (int32_t) carries N's raw bit pattern,
    // and translate_proc.cpp's own dispatch passes it to translateSwitch's
    // uint32_t n exactly as-is — an N large enough to read as negative
    // here must still agree that it's ">2" (translateSwitch's helper-
    // vector path, which clobbers lr), or needsLRSave comes out false for
    // a body that actually goes on to clobber it.
    if(instr.op == Op::EXT)
    {
        // Decided here, in the pre-pass, because the prologue is emitted
        // from ProcSlot's needsLRSave long before codegen sees the op.
        return extDeclHas(instr.extDecl, EXT_FLAG_NEEDS_LR);
    }
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
static void scanBody(const uint8_t *bytes, uint32_t maxBytes, uint32_t &pc, bool &needsLRSave, ScanFrame *frame, uint32_t stackFloor, const ExtHooks *ext, bool &stop, bool &foundEnd, uint32_t &failCode)
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
        // Before decoding, not after: decodeInstr trusts this walk to have
        // vetted the byte, and shipping builds are -DNDEBUG. Every
        // instruction of every procedure passes here before anything is
        // translated, which is what makes this the one gate for extension
        // bytes (isa-core.md §11) and reserved codes (§5.3).
        if(bytes[pc] > LAST_CORE_OPCODE)
        {
            // Three distinct cases, and the middle one is easy to get wrong:
            // 124-127 are reserved to the CORE (§5.3), not extension space,
            // so an extension is never offered one. Only >= 128 is its range.
            if(bytes[pc] < EXT_OPCODE_BASE)
            {
                stop = true; // foundEnd stays false — caller reports !ok
                failCode = RESOURCE_PROGRAM_RESERVED_OPCODE;
                return;
            }

            uint32_t decl = 0;
            if(extDecodeLength(bytes, maxBytes, pc, decl, ext) == 0)
            {
                stop = true;
                failCode = RESOURCE_PROGRAM_EXT_UNKNOWN;
                return;
            }
            if(extDeclHas(decl, EXT_FLAG_CALL_SHAPED) || extDeclHas(decl, EXT_FLAG_TERMINATES)
                || extDeclMaxTransient(decl) != 0 || extDeclTosDelta(decl) > 0)
            {
                // Well-formed, but declares a capability v1 doesn't
                // implement. Reported separately from "unknown opcode"
                // because the remedy differs: a newer core, not a
                // different image.
                stop = true;
                failCode = RESOURCE_PROGRAM_EXT_UNSUPPORTED;
                return;
            }
        }

        DecodedInstr d = decodeInstr(bytes, maxBytes, pc, ext);
        if(triggersLRSave(d.instr))
        {
            needsLRSave = true;
        }
        pc = d.next;

        if(d.instr.op == Op::BR_TABLE)
        {
            ScanFrame inner{ScanFrameKind::Case, (uint32_t)d.instr.imm};
            scanBody(bytes, maxBytes, pc, needsLRSave, &inner, stackFloor, ext, stop, foundEnd, failCode);
            if(stop) return;
            continue;
        }
        if(d.instr.op == Op::LOOP)
        {
            ScanFrame inner{ScanFrameKind::LoopCond, 0};
            scanBody(bytes, maxBytes, pc, needsLRSave, &inner, stackFloor, ext, stop, foundEnd, failCode);
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

        if(isProcTerminator(d.instr))
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

BodyScanResult scanProcBody(const uint8_t *bytes, uint32_t maxBytes, uint32_t startOffset,
    const ExtHooks *ext, uint32_t stackFloor)
{
    uint32_t pc = startOffset;
    bool needsLRSave = false;
    bool stop = false;
    bool foundEnd = false;
    uint32_t failCode = 0;
    scanBody(bytes, maxBytes, pc, needsLRSave, nullptr, stackFloor, ext, stop, foundEnd, failCode);
    // Ran off maxBytes with a level still open: the one rejection no site
    // above names for itself, since it is discovered by falling out of the
    // walk rather than by hitting anything.
    if(!foundEnd && failCode == 0)
    {
        failCode = RESOURCE_PROGRAM_BODY_UNTERMINATED;
    }
    return BodyScanResult{pc - startOffset, needsLRSave, failCode == 0, failCode};
}

} // namespace jitc
