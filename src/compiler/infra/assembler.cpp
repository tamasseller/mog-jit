#include "assembler.h"
#include "runtime.h"

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

Assembler::Assembler(Runtime &rt, uint32_t lruTick): buf((uint16_t *)(uintptr_t)rt.getArenaCursor()), runtime(rt), lruTick(lruTick) {}

__attribute__((always_inline)) inline uint32_t Assembler::doEmit(uint16_t word)
{
    uint32_t at = pc();

    this->buf = this->runtime.ensureSpace(buf + count, lruTick);

    if(buf)
    {
        buf[count++] = word;
    }

    return at;
}

uint32_t Assembler::emit(uint16_t word)
{
    uint32_t at = doEmit(word);

    if(buf && !suppressPoolCheck)
    {
        ensurePoolRoom(0);
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

uint16_t Assembler::readRawHalfword(uint32_t siteOffset) const
{
    uint32_t idx = siteOffset / 2;
    if(idx >= count)
    {
        return 0; // GCOV_EXCL_LINE — see patchBranch's own comment
    }
    return buf[idx];
}

// ── immediates ───────────────────────────────────────────────────────────

__attribute__((always_inline)) inline void Assembler::parkPoolSite(uint32_t dstReg, uint32_t value)
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

Effect Assembler::materializeImm32(uint32_t dstReg, uint32_t value, bool allowTwoIsnSeq)
{
    if(ArmV6M::fitsImm8(value))
    {
        emit(ArmV6M::movs(R((uint16_t)dstReg), ArmV6M::Imm<8>(value)));
        return Effect::into(dstReg, true);
    }

    if(allowTwoIsnSeq)
    {
        if(ArmV6M::fitsImm8(~value))
        {
            emit(ArmV6M::movs(R((uint16_t)dstReg), ArmV6M::Imm<8>(~value)));
            emit(ArmV6M::mvns(R((uint16_t)dstReg), R((uint16_t)dstReg)));
            return Effect::into(dstReg, true);
        }

        const auto decomposed = unshift(value);
        assert((decomposed.pattern << decomposed.shift) == value);

        if(ArmV6M::fitsImm8(decomposed.pattern))
        {
            emit(ArmV6M::movs(R((uint16_t)dstReg), ArmV6M::Imm<8>(decomposed.pattern)));
            emit(ArmV6M::lsls(R((uint16_t)dstReg), R((uint16_t)dstReg), ArmV6M::Imm<5>(decomposed.shift)));
            return Effect::into(dstReg, true);
        }
    }

    ensurePoolRoom(1);
    parkPoolSite(dstReg, value);
    return Effect::into(dstReg, false); // a pool LDR sets no flags
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

    uint32_t branchSite = 0;
    if(emitGuard)
    {
        branchSite = doEmit(ArmV6M::b(ArmV6M::Ioff<1, 11>(0)));
    }

    if(pc() % 4 != 0)
    {
        doEmit(ArmV6M::nop());
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
        doEmit((uint16_t)(pendingValues[i] & 0xffff));
        doEmit((uint16_t)(pendingValues[i] >> 16));
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

uint32_t Assembler::finalize(uint32_t procIdx)
{
    flushPool();

    runtime.commit((uint32_t)(uintptr_t)(buf + count)); // buf is uint16_t*, count a halfword index — scale, don't add raw
    runtime.markCompiled(procIdx, (uint32_t)buf);

    return count;
}

} // namespace jitc
