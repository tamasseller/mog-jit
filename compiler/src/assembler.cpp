#include "assembler.h"
#include "runtime_internal.h"
#include "imm_synth.h"

#include <cassert>
#include <cstring>

namespace jitc
{

using R = ArmV6M::LoReg;

static constexpr uint32_t LITERAL_POOL_MAX_REACH = 1020;
static constexpr uint32_t LITERAL_POOL_REACH_MARGIN = 128;

static uint32_t roundUpToWord(uint32_t v)
{
    return (v + 3u) & ~3u;
}

Assembler::Assembler(Runtime &rt, uint32_t procIdx, uint32_t lruTick)
    : buf((uint16_t *)(uintptr_t)rt.arenaCursor),
      capacity((rt.arenaEnd - rt.arenaCursor) / 2),
      runtime(rt), procIdx(procIdx), lruTick(lruTick) {}

// end is one past the last halfword written so far — the in-progress
// region's own high-water mark, which is exactly what evict() needs to
// know how much to slide. Room for one more halfword means end is still
// strictly below the arena's end.
bool Assembler::ensureSpace(const uint16_t *end, uint32_t lruTick)
{
    assert(end <= buf + capacity);

    if(end == buf + capacity)
    {
        int victim = runtime.findEvictionVictim(lruTick);
        if(victim < 0)
        {
            runtimeBail(&runtime, RESOURCE_EXHAUSTED_ARENA);
            return false;
        }

        // buf tracks arenaCursor, so buf + capacity is arenaEnd either
        // side of this — what moves is the in-progress region itself,
        // slid down by the victim's size along with everything above it.
        // Carry end along by offset, or the assert below would be reading
        // a pointer that no longer names this region.
        uint32_t written = (uint32_t)(end - buf);

        runtime.evict((uint32_t)victim, end);

        buf = (uint16_t *)(uintptr_t)runtime.arenaCursor;
        capacity = (runtime.arenaEnd - runtime.arenaCursor) / 2;
        end = buf + written;
    }

    assert(end < buf + capacity);
    return true;
}

uint32_t Assembler::emit(uint16_t word)
{
    uint32_t at = pc();

    if(this->ensureSpace(buf + count, lruTick))
    {
        buf[count++] = word;

        if(!suppressPoolCheck)
        {
            ensurePoolRoom(0);
        }
    }

    return at;
}

uint32_t Assembler::placeholderCondBranch(ArmV6M::Condition c)
{
    return emit(ArmV6M::condBranch(c, ArmV6M::Ioff<1, 8>(0)));
}

uint32_t Assembler::placeholderBranch()
{
    return emit(ArmV6M::b(ArmV6M::Ioff<1, 11>(0)));
}

bool Assembler::patchBranch(uint32_t siteOffset, uint32_t targetOffset)
{
    uint32_t idx = siteOffset / 2;
    assert(idx < count);  // GCOV_EXCL_LINE — only reachable once overflow already started

    uint16_t isn = buf[idx];
    int32_t delta = (int32_t)targetOffset - (int32_t)(siteOffset + 4);

    if(ArmV6M::isCondBranch(isn))
    {
        if(ArmV6M::Ioff<1, 8>::isInRange(delta))
        {
            buf[idx] = ArmV6M::setCondBranchOffset(isn, ArmV6M::Ioff<1, 8>((int16_t)delta));
            return true;
        }
    }
    else
    {
        if(ArmV6M::Ioff<1, 11>::isInRange(delta))
        {
            buf[idx] = ArmV6M::setBranchOffset(isn, ArmV6M::Ioff<1, 11>((int16_t)delta));
            return true;
        }
    }

    return false;
}

uint32_t Assembler::readBranchTarget(uint32_t siteOffset) const
{
    uint32_t idx = siteOffset / 2;
    if(idx >= count)
    {
        return siteOffset; // GCOV_EXCL_LINE — see patchBranch's own comment
    }
    uint16_t isn = buf[idx];
    uint16_t raw;
    int32_t delta;
    if(ArmV6M::getCondBranchOffset(isn, raw))
    {
        delta = ArmV6M::signExtend(raw, 8) << 1;
    }
    else
    {
        ArmV6M::getBranchOffset(isn, raw);
        delta = ArmV6M::signExtend(raw, 11) << 1;
    }
    return siteOffset + 4 + delta;
}

bool Assembler::linkIntoChain(Label &label, uint32_t site)
{
    if(!patchBranch(site, label.chain == -1 ? site : (uint32_t)label.chain))
    {
        return false;
    }

    label.chain = (int32_t)site;
    return true;
}

bool Assembler::branchTo(Label &label, ArmV6M::Condition c)
{
    return linkIntoChain(label, placeholderCondBranch(c));
}

bool Assembler::branchTo(Label &label)
{
    if(!linkIntoChain(label, placeholderBranch()))
    {
        return false;
    }

    flushPool();
    return true;
}

bool Assembler::bind(Label &label)
{
    uint32_t target = pc();
    for(int32_t site = label.chain; site != -1;)
    {
        uint32_t prevSite = readBranchTarget((uint32_t)site);

        if(!patchBranch((uint32_t)site, target))
        {
            return false;
        }

        site = (prevSite == (uint32_t)site) ? -1 : (int32_t)prevSite;
    }

    label.chain = -1;
    return true;
}

void Assembler::patchRawHalfword(uint32_t siteOffset, uint16_t value)
{
    uint32_t idx = siteOffset / 2;
    if(idx >= count)
    {
        return; // GCOV_EXCL_LINE — see patchBranch's own comment
    }
    buf[idx] = value;
}

// ── immediates ───────────────────────────────────────────────────────────

void Assembler::parkPoolSite(uint32_t dstReg, uint32_t value)
{
    uint32_t site = emit(ArmV6M::fmtImm8(ArmV6M::Imm8Op::LDR, (uint16_t)dstReg, 0));
    pendingSites[pendingCount] = site;
    pendingValues[pendingCount] = value;
    pendingCount++;
}

struct ShiftDecomposition 
{
    uint32_t pattern, shift;
};

static inline ShiftDecomposition unshift(uint32_t value)
{
    assert(value != 0);

    for(uint32_t shift = 0; ; shift++)
    {
        const auto pattern = value >> shift;

        if((pattern & 1) != 0)
        {
            return ShiftDecomposition {
                .pattern = pattern, 
                .shift = shift
            };
        }
    }
}

void Assembler::materializeImm32(uint32_t dstReg, uint32_t value, bool allowTwoIsnSeq)
{
    if(fitsImm8(value))
    {
        emit(ArmV6M::movs(R((uint16_t)dstReg), ArmV6M::Imm<8>(value)));
        return;
    }

    if(allowTwoIsnSeq)
    {
        if(fitsImm8(~value))
        {
            emit(ArmV6M::movs(R((uint16_t)dstReg), ArmV6M::Imm<8>(~value)));
            emit(ArmV6M::mvns(R((uint16_t)dstReg), R((uint16_t)dstReg)));
            return;
        }

        const auto decomposed = unshift(value);
        assert((decomposed.pattern << decomposed.shift) == value);

        if(fitsImm8(decomposed.pattern))
        {
            emit(ArmV6M::movs(R((uint16_t)dstReg), ArmV6M::Imm<8>(decomposed.pattern)));
            emit(ArmV6M::lsls(R((uint16_t)dstReg), R((uint16_t)dstReg), ArmV6M::Imm<5>(decomposed.shift)));
            return;
        }
    }

    ensurePoolRoom(1);
    parkPoolSite(dstReg, value);
}

uint32_t Assembler::poolDebt() const
{
    return pendingCount > 0 ? 4 * pendingCount + 4 : 0;
}

void Assembler::patchPoolSite(uint32_t siteOffset, uint32_t word)
{
    uint32_t idx = siteOffset / 2;
    if(idx >= count)
    {
        return; // GCOV_EXCL_LINE — see patchBranch's own comment
    }
    uint16_t off = (uint16_t)(word - ((siteOffset + 4) & ~3u));
    buf[idx] = ArmV6M::setLiteralOffset(buf[idx], ArmV6M::Uoff<2, 8>(off));
}

void Assembler::flushPool(bool emitGuard)
{
    if(pendingCount == 0)
    {
        return;
    }

    AtomicScope atomic(*this);

    uint32_t branchSite = 0;
    if(emitGuard)
    {
        branchSite = placeholderBranch();
    }

    if(pc() % 4 != 0)
    {
        emit(ArmV6M::nop());
    }

    for(uint32_t i = 0; i < pendingCount; i++)
    {
        bool isFirstOccurrence = true;
        for(uint32_t j = 0; j < i; j++)
        {
            if(pendingValues[j] == pendingValues[i])
            {
                isFirstOccurrence = false;
                break;
            }
        }

        if(!isFirstOccurrence)
        {
            continue;
        }

        uint32_t word = pc();
        emit((uint16_t)(pendingValues[i] & 0xffff));
        emit((uint16_t)(pendingValues[i] >> 16));
        patchPoolSite(pendingSites[i], word);

        for(uint32_t j = i + 1; j < pendingCount; j++)
        {
            if(pendingValues[j] == pendingValues[i])
            {
                patchPoolSite(pendingSites[j], word);
            }
        }
    }

    if(emitGuard)
    {
        const auto ok = patchBranch(branchSite, pc());
        assert(ok && "should be able to branch over literal pool");
    }

    pendingCount = 0;
}

void Assembler::ensurePoolRoom(uint32_t poolEntries, uint32_t extraBytes)
{
    if(pendingCount == 0)
    {
        return;
    }

    uint32_t poolEnd = roundUpToWord(pc() + 2 + extraBytes) + 4 * (pendingCount + poolEntries);
    bool reachAtRisk = poolEnd - pendingSites[0] + LITERAL_POOL_REACH_MARGIN > LITERAL_POOL_MAX_REACH;
    bool countAtRisk = pendingCount + poolEntries > POOL_MAX_PENDING;
    
    if(reachAtRisk || countAtRisk)
    {
        flushPool(true);
    }
}

uint32_t Assembler::finalize()
{
    flushPool();

    uint32_t need = count * 2;
    uint32_t dest = runtime.allocate(need);
    assert(dest == (uint32_t)(uintptr_t)buf); // GCOV_EXCL_LINE — growForAttached's own base-tracking invariant
    runtime.markCompiled(procIdx, dest, lruTick);

    return count;
}

} // namespace jitc
