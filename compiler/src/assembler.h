// jit-armv6m/compiler — the assembler layer: code buffer, branch fixups,
// the literal pool, and immediate-materialization scheme selection; for a
// procedure attached to a real Runtime, also arena eviction/compaction and
// final dispatch-table registration. Runtime-coupling here is scoped to
// owning the output buffer's storage (arena growth, bailout, final
// registration) — translate_proc.cpp reads Runtime data (ProcSlot lookups,
// liveStackFloor()) directly through its own const Runtime& parameter.
#ifndef JIT_ARMV6M_COMPILER_ASSEMBLER_H_
#define JIT_ARMV6M_COMPILER_ASSEMBLER_H_

#include <cstdint>
#include "armv6.h"
// The RESOURCE_* codes fail() below reports, and nothing else — this
// header needs no Runtime type (see the forward declaration under it) and
// runtime_host.h needs nothing but stdint.h, so pulling it in here costs
// the Runtime-agnosticism above nothing.
#include "runtime_host.h"

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
    Runtime *runtime = nullptr; // null: detached

    uint16_t *buf;
    uint32_t capacity;
    uint32_t count = 0;
    bool suppressPoolCheck = false;

    uint32_t procIdx = 0;
    uint32_t lruTick = 0;

    static constexpr uint32_t POOL_MAX_PENDING = 16;
    uint32_t pendingSites[POOL_MAX_PENDING];
    uint32_t pendingValues[POOL_MAX_PENDING];
    uint32_t pendingCount = 0;

    bool linkIntoChain(Label &label, uint32_t site);
    void parkPoolSite(uint32_t dstReg, uint32_t value);
    void patchPoolSite(uint32_t siteOffset, uint32_t word);
    void flushPoolImpl(bool endOfProcedure);
    bool growForAttached();

public:
    // Detached: a fixed caller-supplied buffer, no arena, no Runtime.
    // Every host unit test and QEMU pre-measurement call uses this.
    explicit Assembler(uint16_t *buf, uint32_t capacityHalfwords);

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

    // Suppresses emit()'s own automatic pool-reach check for its lifetime
    // (restoring the previous state on destruction, so nesting is
    // harmless) — for a PC-sensitive, closed-form instruction sequence
    // (a fixed-length stub, a self-referential call record, a jump table
    // that must sit contiguous right after its own dispatch) where a pool
    // flush landing in the middle would corrupt it. Does not suppress
    // ensurePoolRoom()'s own pre-existing calls (materializeImm32,
    // abiEmitCall) — those still guard pendingSites/pendingValues from
    // overflowing; a caller wrapping a sequence in this scope must instead
    // reserve whatever room that sequence needs *before* entering it.
private:
    // The raw suppression, private so that reserve-then-suppress cannot be
    // written the wrong way round. AtomicBlock below is the public form;
    // flushPoolImpl uses this one directly because it IS the flush.
    class AtomicScope
    {
        Assembler &a;
        bool prev;

    public:
        explicit AtomicScope(Assembler &a) : a(a), prev(a.suppressPoolCheck) { a.suppressPoolCheck = true; }
        ~AtomicScope() { a.suppressPoolCheck = prev; }
        AtomicScope(const AtomicScope &) = delete;
        AtomicScope &operator=(const AtomicScope &) = delete;
    };

public:
    // Reserve, THEN suppress — in that order, which is the whole reason
    // this is a type. Suppressing first and reserving inside would let the
    // very flush being guarded against land in the middle of the sequence.
    // That ordering rule used to be prose in the comment above, with every
    // caller honouring it by hand; constructing one of these is now the
    // only way to obtain the suppression at all.
    //
    // poolEntries is how many NEW literal-pool entries the guarded sequence
    // will park; extraBytes is any raw non-instruction payload it emits (a
    // jump table's own slots). Both are what the suppressed check would
    // otherwise have accounted for as it went.
    class AtomicBlock
    {
        AtomicScope scope;

    public:
        AtomicBlock(Assembler &a, uint32_t poolEntries, uint32_t extraBytes = 0)
            : scope((a.ensurePoolRoom(poolEntries, extraBytes), a)) { }

        AtomicBlock(const AtomicBlock &) = delete;
        AtomicBlock &operator=(const AtomicBlock &) = delete;
    };

    inline const Runtime &r() { return *this->runtime; }

    // ── raw buffer ──────────────────────────────────────────────────────
    uint32_t pc() const { return count * 2; }
    uint32_t halfwordCount() const { return count; }

    // Also runs the pool-reach check below (ensurePoolRoom(0)) unless
    // inside an AtomicScope — flushing (guarded) whenever the pending set
    // is at risk, regardless of what specific instruction was just
    // emitted. This is the general safety net; ensurePoolRoom's own
    // explicit calls remain for the "about to add N new entries" case,
    // which this can't anticipate on its own.
    uint32_t emit(uint16_t word);

    // A translator-detected failure, reported as the RESOURCE_* code the
    // caller names (runtime_host.h has the list and what each class means
    // to whoever gets it back). There is no default: every site knows
    // which of them it is, and a default argument is exactly the escape
    // hatch that grows a second blanket code.
    //
    // Calls runtimeBail(), which is [[noreturn]] in every build: in
    // production it restores the caller's own saved sp and long-jumps to
    // the landing with that code; the host build's own mocked
    // runtimeBail() (test/host/host_runtime_support.cpp) escapes the
    // same way via longjmp, the same mechanism 1test's own CHECK()
    // failures already unwind through. Every call site should still
    // `return` right after calling this regardless, as defense in depth
    // against a build where that contract doesn't hold.
    void fail(uint32_t code);

    // ── branches ────────────────────────────────────────────────────────
    // Raw placeholder/patch primitives — translate_proc.cpp's own
    // translateSwitch reaches patchRawHalfword directly for its jump
    // table's raw slots; Label/bind() below is the flush-safe alternative
    // for a fixup that resolves to "wherever we are once this construct
    // closes."
    uint32_t placeholderBranch();
    uint32_t placeholderCondBranch(ArmV6M::Condition c);
    bool patchBranch(uint32_t siteOffset, uint32_t targetOffset) __attribute__((warn_unused_result));
    uint32_t readBranchTarget(uint32_t siteOffset) const;

    // Chain site onto label (self-linking it if label was empty) — the
    // building block bind() below walks back through. The unconditional
    // overload also flushes the pool no-guard right after: nothing ever
    // falls through an unconditional branch, so whatever follows in the
    // buffer is never reached that way, and a guarded flush's own
    // branch-around would be wasted bytes here.
    bool branchTo(Label &label, ArmV6M::Condition c) __attribute__((warn_unused_result));
    bool branchTo(Label &label) __attribute__((warn_unused_result));

    // Flush the pool (if one is open — safe to call on an empty Label),
    // then resolve every branch chained onto label to right here. This is
    // the one place a forward fixup may ever be resolved to "the current
    // position" — doing it through here rather than a bare
    // patchBranch(site, pc()) is what keeps a label's target from ever
    // landing on top of pool words a flush inserts. Always guarded
    // (never the no-branch-around form): a bound label can be reached via
    // fallthrough (e.g. an if-then's "end", reached both by the skip
    // branch and by falling through the body), unlike an unconditional
    // branch's own target.
    bool bind(Label &label) __attribute__((warn_unused_result));

    // Flush the pool right now (guarded), regardless of ensurePoolRoom's
    // own risk heuristic — the explicit, unconditional counterpart to it.
    void flushPool();

    // Flush the pool with no branch-around, for a caller that already
    // knows nothing falls through here (right after an unconditional
    // jump, or a jump table only ever reached via its own dispatch) —
    // blocks.cpp's raw jump-table-slot fixups also need "wherever we are,
    // after any pending flush" before reading pc() for one of these.
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

    // Flush (guarded) if adding poolEntries more pool entries, plus
    // extraBytes of additional known-upcoming code with no flush
    // opportunity of its own (default 0 — most callers have none), would
    // put the pending set at risk of overrunning POOL_MAX_PENDING or
    // LITERAL_POOL_MAX_REACH.
    void ensurePoolRoom(uint32_t poolEntries, uint32_t extraBytes = 0);
};

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_ASSEMBLER_H_
