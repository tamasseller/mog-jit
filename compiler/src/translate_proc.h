// The top-level per-procedure driver — decodes one procedure's wire bytes
// and emits Thumb code for it via abi_strategy.h's real-ABI sequences.
#ifndef JIT_ARMV6M_COMPILER_TRANSLATE_PROC_H_
#define JIT_ARMV6M_COMPILER_TRANSLATE_PROC_H_

#include <cstdint>
#include "proc.h"
#include "arena_room.h"

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
//
// savesLROverride, if non-null, is the whole-program directory's own
// precomputed answer (runtime/runtime_internal.h's ProcSlot) to "does this
// body ever reach CALL/BR_TABLE(N>2)/CLZ/REVBITS" — needsLRSave(proc)
// answers the same question by scanning the body fresh, exactly right for
// a one-off host-test/pre-measurement call, but wasteful to redo on every
// recompile once a directory already has the answer. Null (every existing
// caller, unchanged) falls back to that scan.
//
// room, if non-null, lets the translator grow outBuf's own headroom
// mid-pass by evicting/compacting other resident procedures
// (arena_room.h, docs/design.md §11) — the one seam into Runtime-owned
// state this otherwise fully Runtime-agnostic function has. Null (every
// existing caller, unchanged) means outBuf's capacity is fixed for the
// whole pass, exactly as today: TranslateResult::overflowed reports
// exhaustion the same way either way.
TranslateResult translateProc(
    const Proc &proc,
    uint32_t procIdx,
    const uint32_t *calleeArgCounts, uint32_t calleeCount,
    uint16_t *outBuf, uint32_t outCapacityHalfwords,
    uint32_t stackFloor = 0,
    const bool *savesLROverride = nullptr,
    ArenaRoom *room = nullptr);

/** Whether proc's own body ever reaches an op needing lr protected before
 *  anything can clobber it: a nested CALL, blocks.h's own openBrTableJump
 *  (BR_TABLE N > 2 only), or unaryops.h's CLZ/REVBITS — all reached via
 *  BLX through the helper vector, which clobbers real hardware lr exactly
 *  like a local BL would. Exported for callers with no directory
 *  (runtime/runtime_internal.h's ProcSlot) to source the answer from
 *  instead — translateProc itself falls back to this whenever
 *  savesLROverride is null. */
bool needsLRSave(const Proc &proc);

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_TRANSLATE_PROC_H_
