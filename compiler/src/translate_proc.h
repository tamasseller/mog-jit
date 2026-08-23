// jit-armv6m/compiler — the top-level per-procedure driver, ported from
// jit-armv6m/prototype/src/translateProc.ts, via abi_strategy.h's real-ABI
// sequences only (noEvictionStrategy is TS-only scaffolding, never ported
// — abi_strategy.h's own header has why).
#ifndef JIT_ARMV6M_COMPILER_TRANSLATE_PROC_H_
#define JIT_ARMV6M_COMPILER_TRANSLATE_PROC_H_

#include <cstdint>
#include "proc.h"

namespace jitc {

struct TranslateResult {
    uint32_t halfwordCount;
    /** outCapacityHalfwords was exceeded, *or* this procedure's own
     *  LOOP/BR_TABLE nesting exceeded MAX_BLOCK_NESTING (translate_proc
     *  .cpp) — the real-hardware counterpart of the recursion bound
     *  JS's own call stack never needed enforcing on the prototype side:
     *  compileProc's own caller (compile_proc_real.cpp) already treats
     *  this bit as "bail out with RESOURCE_ERROR," which is exactly the
     *  right response to either cause. */
    bool overflowed;
};

/** translateProc.ts's forward pass. procIdx is this procedure's own
 *  dispatch-table index (abiEmitCall's own packRecord argument).
 *  calleeArgCounts[i] is procedure i's own argCount — this function only
 *  ever reads calleeArgCounts[instr.calleeIndex], exactly like
 *  translateProc.ts. */
TranslateResult translateProc(
    const Proc &proc,
    uint32_t procIdx,
    const uint32_t *calleeArgCounts, uint32_t calleeCount,
    uint16_t *outBuf, uint32_t outCapacityHalfwords);

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_TRANSLATE_PROC_H_
