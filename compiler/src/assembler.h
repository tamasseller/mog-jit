// jit-armv6m/compiler — the assembler layer: code buffer, branch
// placeholder/fixup, the literal pool (tracked by stored value, not
// bytecode tag — flushPool() never re-decodes anything and never scans
// its own output, so a BR_TABLE(N>2) jump table's raw halfwords are never
// at risk of being misread as a pooled site), immediate-materialization
// scheme selection, and — for a procedure attached to a real Runtime —
// arena eviction/compaction and final dispatch-table registration. This
// is the one seam between the environment-free core compiler
// (translate_proc.cpp and everything it calls) and the runtime's own
// dispatch/eviction machinery; nothing above this layer touches Runtime
// directly.
#ifndef JIT_ARMV6M_COMPILER_ASSEMBLER_H_
#define JIT_ARMV6M_COMPILER_ASSEMBLER_H_

#include <cstdint>
#include "armv6.h"

// Declared at global scope in runtime/runtime_internal.h, a plain
// aggregate every caller builds by reinterpreting a raw byte buffer —
// forward-declared here rather than pulled in by #include so a detached
// Assembler's own clients (every host unit test, the QEMU pre-
// measurement calls) stay exactly as Runtime-agnostic as before; only
// assembler.cpp itself needs the real definition.
class Runtime;

namespace jitc
{

// A pending forward-branch target: a chain of not-yet-resolved branch
// sites threaded through the branches' own encoded offsets (each new site
// points at the previous chain head, or self-links as the chain's first
// entry) — no side array needed, generalizing blocks.cpp's own
// endFixupChain trick so every caller gets it. bind() is the only way to
// resolve one, and it flushes the pool first — the ordering guarantee
// that makes a label's target always land after any pool words a flush
// might insert, never colliding with them.
struct Label
{
    int32_t chain = -1;
};

class Assembler
{
public:
    // Detached: a fixed caller-supplied buffer, no arena, no Runtime.
    // Every host unit test and QEMU pre-measurement call uses this.
    // stackFloor is returned by stackFloor() unchanged — the detached
    // case has no live Runtime to derive it from, so a caller that cares
    // (translateProc's own stack-nesting guard) supplies it directly.
    explicit Assembler(uint16_t *buf, uint32_t capacityHalfwords, uint32_t stackFloor = 0);

    // Attached: owns arena growth against a real Runtime and exits
    // directly (never returns) if it cannot free enough room — the
    // landing's own bailOut, folded in here rather than left for a caller
    // to check after the fact. procIdx is this procedure's own
    // dispatch-table index, needed by finalize() to register the
    // compiled result and by abi_strategy.cpp's force-pooled call record.
    // lruTick is the current r11 value, read once by the caller so this
    // class never needs `register ... asm("r11")` itself — that keeps
    // real inline asm out of the host build entirely.
    Assembler(Runtime *runtime, uint32_t procIdx, uint32_t lruTick);

    Assembler(const Assembler &) = delete;
    Assembler &operator=(const Assembler &) = delete;

    // ── raw buffer ──────────────────────────────────────────────────────
    uint32_t pc() const { return count * 2; }
    uint32_t halfwordCount() const { return count; }
    uint32_t emit(uint16_t word);

    // The live stack-recursion floor translateBody's own nesting guard
    // checks against — for an attached Assembler this is
    // Runtime::liveStackFloor(), read fresh every call since arenaCursor
    // moves between compilations; for a detached one it's the fixed value
    // given at construction.
    uint32_t stackFloor() const;

    // A translator-detected failure (arena exhausted with nothing left
    // to evict, or the live stack-nesting guard tripped). Calls
    // runtimeBail(), which is [[noreturn]] in every build: in production
    // it restores the caller's own saved sp and long-jumps to the
    // landing with RESOURCE_ERROR; the host build's own mocked
    // runtimeBail() (test/host/host_runtime_support.cpp) escapes the
    // same way via longjmp, the same mechanism 1test's own CHECK()
    // failures already unwind through. Every call site should still
    // `return` right after calling this regardless, as defense in depth
    // against a build where that contract doesn't hold.
    void fail();

    // ── branches ────────────────────────────────────────────────────────
    // Raw placeholder/patch primitives — blocks.cpp's own Frame bookkeeping
    // (TableInfo's raw halfword slots, the guard branch site openBrTable
    // hands back to its caller) still reaches these directly; Label/bind()
    // below is the flush-safe alternative for a fixup that resolves to
    // "wherever we are once this construct closes."
    uint32_t placeholderBranch();
    uint32_t placeholderCondBranch(ArmV6M::Condition c);
    void patchBranch(uint32_t siteOffset, uint32_t targetOffset);
    uint32_t readBranchTarget(uint32_t siteOffset) const;

    // Chain site onto label (self-linking it if label was empty) — the
    // building block bind() below walks back through.
    void branchTo(Label &label, ArmV6M::Condition c);
    void branchTo(Label &label);

    // Flush the pool (if one is open — safe to call on an empty Label),
    // then resolve every branch chained onto label to right here. This is
    // the one place a forward fixup may ever be resolved to "the current
    // position" — doing it through here rather than a bare
    // patchBranch(site, pc()) is what keeps a label's target from ever
    // landing on top of pool words a flush inserts.
    void bind(Label &label);

    // Flush the pool (if one is open), same as bind() but with no label
    // to resolve — blocks.cpp's raw jump-table-slot fixups (never
    // branches, so nothing to chain onto a Label) still need "wherever
    // we are, after any pending flush" before reading pc() for one of
    // these.
    void flushPool();

    void flushPoolNoGuard();

    // ── raw halfword slots (BR_TABLE N>2 jump tables — never pool data) ──
    void patchRawHalfword(uint32_t siteOffset, uint16_t value);

    void materializeImm32(uint32_t dstReg, uint32_t value, bool allowTwoIsnSeq = true);

    // What the currently-open pool chunk still owes the output stream —
    // one word per pending site (after dedup this may overcount slightly;
    // always a safe over-estimate, never an under-estimate), plus the
    // branch-around and worst-case pad its flush will emit. blocks.cpp's
    // emitGuardedBranch needs this because its own span bound walks
    // bytecode alone and so cannot see any of it.
    uint32_t poolDebt() const;

    // End-of-procedure: flush the pool (no branch-around — nothing
    // executes past a terminator), and for an attached Assembler commit
    // the arena allocation and register the result with Runtime. Returns
    // the final halfword count.
    uint32_t finalize();

    void ensurePoolRoom(uint32_t poolEntries);

private:
    uint16_t *buf;
    uint32_t capacity;
    uint32_t count = 0;
    uint32_t detachedStackFloor;

    Runtime *runtime = nullptr; // null: detached
    uint32_t procIdx = 0;
    uint32_t lruTick = 0;

    // The pool's whole deferral state — stored (site, value) pairs, not a
    // bytecode tag: flushPool() patches each site directly, with no scan
    // of the output buffer at all. pendingSites[0] doubles as the chunk's
    // own output-start for the reach guard — always the oldest pending
    // site, since sites are appended in emission order.
    static constexpr uint32_t POOL_MAX_PENDING = 16;
    uint32_t pendingSites[POOL_MAX_PENDING];
    uint32_t pendingValues[POOL_MAX_PENDING];
    uint32_t pendingCount = 0;

    void linkIntoChain(Label &label, uint32_t site);
    void parkPoolSite(uint32_t dstReg, uint32_t value);
    void patchPoolSite(uint32_t siteOffset, uint32_t word);
    void flushPoolImpl(bool endOfProcedure);
    bool growForAttached();
};

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_ASSEMBLER_H_
