// jit-armv6m/compiler — one procedure's own boundary, found by walking it
// (isa-core.md §5.5: a body is self-delimiting, no stored length). Reused
// by runtime/runtime_internal.h's directory-builder (Runtime::init()),
// which knows a procedure's own start but not, ahead of the walk, where it
// ends or whether it needs `lr` protected.
#ifndef JIT_ARMV6M_COMPILER_PROC_SCAN_H_
#define JIT_ARMV6M_COMPILER_PROC_SCAN_H_

#include <cstdint>

namespace jitc
{

struct BodyScanResult
{
    uint32_t bodyBytes;  // this procedure's own body length, discovered by walking it
    bool needsLRSave;    // true iff the body contains CALL, BR_TABLE(imm>2), CLZ, or REVBITS
    bool ok;             // false: hit stackFloor, or ran off maxBytes with something still open
    bool stackFloorHit;  // which of those two, when !ok: true = stackFloor, false = ran off maxBytes.
                         // The caller reports them as different things (runtime_host.h's
                         // RESOURCE_EXHAUSTED_SCAN_STACK vs RESOURCE_PROGRAM_BODY_UNTERMINATED —
                         // out of room here, versus a program that was never well-formed).
};

/** Finds one procedure's own body boundary and whether it needs `lr`
 *  protected, walking from bytes[startOffset..) — immediately past this
 *  procedure's own arg_count LEB128 — no further than maxBytes (the whole
 *  remaining wire blob, not a known body length, since that's exactly
 *  what this discovers). Ported from packages/machine/src/bytecode.ts's
 *  decodeProcBody, but as native recursion (one call per open
 *  LOOP/BR_TABLE) rather than an explicit frame-stack array — the same
 *  shape translate_proc.cpp's own translateBody uses for the identical
 *  nesting-tracking problem, checked live against stackFloor for the same
 *  reason (this runs before any Runtime exists to bail through the usual
 *  compileProc path, so the caller checks `ok` itself).
 *  Defaults to 0 (no limit) for callers with no real embedded stack in
 *  play. Malformed/truncated bytecode is out of scope here, same as
 *  everywhere else in this translator (decode_instr.h's own asserts,
 *  compiled out under -DNDEBUG) — this only guards the genuinely dynamic
 *  concern, recursion depth. */
BodyScanResult scanProcBody(const uint8_t *bytes, uint32_t maxBytes, uint32_t startOffset, uint32_t stackFloor = 0);

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_PROC_SCAN_H_
