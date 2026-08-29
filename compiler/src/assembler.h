#ifndef JIT_ARMV6M_COMPILER_ASSEMBLER_H_
#define JIT_ARMV6M_COMPILER_ASSEMBLER_H_

#include <cstdint>
#include "armv6.h"
#include "runtime_host.h"

class Runtime;

namespace jitc
{

struct Label
{
    int32_t chain = -1;
};

class Assembler
{
    uint16_t *buf;
    uint32_t count = 0;
    bool suppressPoolCheck = false;
    uint32_t lruTick = 0;

    static constexpr uint32_t POOL_MAX_PENDING = 16;
    uint32_t pendingSites[POOL_MAX_PENDING];
    uint32_t pendingValues[POOL_MAX_PENDING];
    uint32_t pendingCount = 0;

    bool linkIntoChain(Label &label, uint32_t site);
    void parkPoolSite(uint32_t dstReg, uint32_t value);
    void patchPoolSite(uint32_t siteOffset, uint32_t word);

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

    Assembler(Runtime &runtime, uint32_t lruTick);

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

    uint32_t finalize(uint32_t procIdx);

    void ensurePoolRoom(uint32_t poolEntries, uint32_t extraBytes = 0);
};

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_ASSEMBLER_H_
