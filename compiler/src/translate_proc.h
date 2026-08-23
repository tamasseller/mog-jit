// The top-level per-procedure driver — decodes one procedure's wire bytes
// and emits Thumb code for it via abi_strategy.h's real-ABI sequences.
#ifndef JIT_ARMV6M_COMPILER_TRANSLATE_PROC_H_
#define JIT_ARMV6M_COMPILER_TRANSLATE_PROC_H_

#include <cstdint>
#include "proc.h"

namespace jitc
{

struct TranslateResult
{
    uint32_t halfwordCount;
    // outCapacityHalfwords was exceeded, or this procedure's own
    // LOOP/BR_TABLE nesting recursion pushed the live stack pointer past
    // stackFloor (translateProc's stackFloor parameter below) — checked
    // live against the actual stack pointer, not a fixed depth count.
    // Either way, compileProc's own caller (compile_proc_real.cpp) treats
    // this bit as "bail out with RESOURCE_ERROR."
    bool overflowed;
};

// The per-procedure forward pass. procIdx is this procedure's own
// dispatch-table index (abiEmitCall's own packRecord argument).
// calleeArgCounts[i] is procedure i's own argCount — this function only
// ever reads calleeArgCounts[instr.calleeIndex].
//
// stackFloor is the lowest address the translator's own LOOP/BR_TABLE
// recursion (translateBody, one native call per nesting level) may safely
// reach, checked live against the actual stack pointer on every recursive
// call. Defaults to 0 (no limit) for callers with no real embedded stack
// budget in play — every host unit test constructs a Proc directly with no
// Runtime/stack-safety concept, so they get pure translation-correctness
// checking, unchanged. The one real caller that matters,
// compile_proc_real.cpp, always passes Runtime::liveStackFloor()'s own
// live value instead.
TranslateResult translateProc(
    const Proc &proc,
    uint32_t procIdx,
    const uint32_t *calleeArgCounts, uint32_t calleeCount,
    uint16_t *outBuf, uint32_t outCapacityHalfwords,
    uint32_t stackFloor = 0);

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_TRANSLATE_PROC_H_
