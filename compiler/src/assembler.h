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
// forward-declared here rather than pulled in by #include so a client
// that only needs the Assembler/Label surface (translate_proc.cpp's own
// non-Runtime-reading helpers, if any) doesn't have to see Runtime's
// real definition too; only assembler.cpp itself needs it.
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
    bool ensureSpace(const uint16_t *end, uint32_t lruTick);

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
    Runtime &runtime;

    Assembler(Runtime &runtime, uint32_t procIdx, uint32_t lruTick);

    Assembler(const Assembler &) = delete;
    Assembler &operator=(const Assembler &) = delete;

    class AtomicBlock
    {
        AtomicScope scope;

    public:
        AtomicBlock(Assembler &a, uint32_t poolEntries, uint32_t extraBytes = 0)
            : scope((a.ensurePoolRoom(poolEntries, extraBytes), a)) { }

        AtomicBlock(const AtomicBlock &) = delete;
        AtomicBlock &operator=(const AtomicBlock &) = delete;
    };

    uint32_t pc() const { return count * 2; }
    uint32_t halfwordCount() const { return count; }

    uint32_t emit(uint16_t word);

    uint32_t placeholderBranch();
    uint32_t placeholderCondBranch(ArmV6M::Condition c);
    bool patchBranch(uint32_t siteOffset, uint32_t targetOffset) __attribute__((warn_unused_result));
    uint32_t readBranchTarget(uint32_t siteOffset) const;

    bool branchTo(Label &label, ArmV6M::Condition c) __attribute__((warn_unused_result));
    bool branchTo(Label &label) __attribute__((warn_unused_result));

    bool bind(Label &label) __attribute__((warn_unused_result));

    void flushPool(bool emitGuard = false);

    void patchRawHalfword(uint32_t siteOffset, uint16_t value);

    void materializeImm32(uint32_t dstReg, uint32_t value, bool allowTwoIsnSeq = true);
    uint32_t poolDebt() const;

    uint32_t finalize();

    void ensurePoolRoom(uint32_t poolEntries, uint32_t extraBytes = 0);
};

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_ASSEMBLER_H_
