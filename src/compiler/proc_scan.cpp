#include "proc_scan.h"
#include "ext.h"
#include "decode_instr.h"
#include "instr.h"

#include <cassert>

namespace jitc
{
static constexpr uint32_t SCAN_STACK_MARGIN = 128;

static bool triggersLRSave(const Instr &instr)
{
    if(instr.op == Op::EXT)
    {
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

static void scanBody(const uint8_t *bytes, uint32_t maxBytes, uint32_t &pc, bool &needsLRSave, ScanFrame *frame, uint32_t stackFloor, bool &stop, bool &foundEnd, uint32_t &failCode)
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
        if(bytes[pc] > LAST_CORE_OPCODE)
        {
            if(bytes[pc] < EXT_OPCODE_BASE)
            {
                stop = true; // foundEnd stays false — caller reports !ok
                failCode = RESOURCE_PROGRAM_RESERVED_OPCODE;
                return;
            }

            uint32_t decl = 0;
            if(extDecodeLength(bytes, maxBytes, pc, decl) == 0)
            {
                stop = true;
                failCode = RESOURCE_PROGRAM_EXT_UNKNOWN;
                return;
            }
            if(extDeclHas(decl, EXT_FLAG_CALL_SHAPED) || extDeclHas(decl, EXT_FLAG_TERMINATES)
                || extDeclMaxTransient(decl) != 0 || extDeclTosDelta(decl) > 0)
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

        if(d.instr.op == Op::BR_TABLE)
        {
            ScanFrame inner{ScanFrameKind::Case, (uint32_t)d.instr.imm};
            scanBody(bytes, maxBytes, pc, needsLRSave, &inner, stackFloor, stop, foundEnd, failCode);
            if(stop) return;
            continue;
        }
        if(d.instr.op == Op::LOOP)
        {
            ScanFrame inner{ScanFrameKind::LoopCond, 0};
            scanBody(bytes, maxBytes, pc, needsLRSave, &inner, stackFloor, stop, foundEnd, failCode);
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
                frame->remaining--;
                if(frame->remaining == 0) return;
            }
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

    scanBody(bytes, maxBytes, pc, needsLRSave, nullptr, stackFloor, stop, foundEnd, failCode);

    if(!foundEnd && failCode == 0)
    {
        failCode = RESOURCE_PROGRAM_BODY_UNTERMINATED;
    }
    
    return BodyScanResult{pc - startOffset, needsLRSave, failCode == 0, failCode};
}

} // namespace jitc
