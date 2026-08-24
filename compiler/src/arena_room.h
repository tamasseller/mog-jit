// jit-armv6m/compiler — the one seam between the Runtime-agnostic
// translator and the runtime's own arena/eviction machinery. Kept
// separate from emitter.h so Emitter itself stays a plain buffer wrapper
// with no notion of "there might be more room somewhere else."
#ifndef JIT_ARMV6M_COMPILER_ARENA_ROOM_H_
#define JIT_ARMV6M_COMPILER_ARENA_ROOM_H_

#include <cstdint>

namespace jitc
{

class Emitter;

/** Grows the arena headroom available to an in-progress translation,
 *  evicting/compacting other resident procedures as needed and rebasing
 *  `e` in place (Emitter::rebase) to the new, larger buffer —
 *  docs/design.md §11's "one compaction extension," which relocates the
 *  in-progress bytes exactly like one more registered procedure's
 *  code_ptr. A no-op if `e` already has `neededHalfwords` of headroom.
 *  Nothing here reports success/failure explicitly: if even evicting
 *  everything didn't free enough, `e`'s own capacity is left unchanged,
 *  and the ordinary Emitter::overflowed() mechanism (checked once, at the
 *  end of translateProc, same as ever) catches it — `ensureRoom`'s
 *  `neededHalfwords` is always a worst-case upper bound (blocks.h's
 *  instrMaxBytes), not a hard requirement, so "couldn't fully satisfy the
 *  request but the actual emission still fit" is a normal outcome, not an
 *  error. */
// No virtual destructor: every implementation is a plain stack local
// (compile_proc_real.cpp's RuntimeArenaRoom, constructed fresh per
// compileProc call), never heap-allocated or destroyed through a base
// pointer — adding one would pull in libstdc++'s deleting-destructor
// thunk and, through it, real operator delete/malloc/_sbrk, on a
// bare-metal target with neither a heap nor any use for one.
struct ArenaRoom
{
    virtual void ensureRoom(Emitter &e, uint32_t neededHalfwords) = 0;
};

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_ARENA_ROOM_H_
