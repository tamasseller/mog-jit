// jit-armv6m/compiler — the top-level per-procedure driver, ported from
// jit-armv6m/prototype/src/translateProc.ts, restricted to this slice's Op
// set (instr.h) via abi_strategy.h's real-ABI sequences only.
#ifndef JIT_ARMV6M_COMPILER_TRANSLATE_PROC_H_
#define JIT_ARMV6M_COMPILER_TRANSLATE_PROC_H_

#include <cstdint>
#include "proc.h"

namespace jitc {

struct TranslateResult {
    uint32_t halfwordCount;
    bool overflowed; // outCapacityHalfwords was exceeded
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
