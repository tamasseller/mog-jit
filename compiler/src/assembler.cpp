#include "assembler.h"
#include "runtime_internal.h"
#include "imm_synth.h"

#include <cassert>
#include <cstring>

namespace jitc
{

using R = ArmV6M::LoReg;

// Uoff<2,8>'s own exact ceiling: a multiple of 4, below 1024.
static constexpr uint32_t LITERAL_POOL_MAX_REACH = 1020;
// Headroom the reach check keeps in hand, since it runs *before* an
// instruction whose own emission then extends the distance to the pool.
// Must exceed the most any single instruction or block close can emit —
// blocks.cpp prices CALL at 64 and translate_proc.cpp's own stack-margin
// comment budgets closeBlockEnd at 80.
static constexpr uint32_t LITERAL_POOL_REACH_MARGIN = 128;

static uint32_t roundUpToWord(uint32_t v)
{
    return (v + 3u) & ~3u;
}

Assembler::Assembler(uint16_t *buf, uint32_t capacityHalfwords, uint32_t stackFloor)
    : buf(buf), capacity(capacityHalfwords), detachedStackFloor(stackFloor) {}

Assembler::Assembler(Runtime *rt, uint32_t procIdx, uint32_t lruTick)
    : buf((uint16_t *)(uintptr_t)rt->arenaCursor),
      capacity((rt->arenaEnd - rt->arenaCursor) / 2),
      detachedStackFloor(0),
      runtime(rt), procIdx(procIdx), lruTick(lruTick) {}

bool Assembler::growForAttached()
{
    if(runtime == nullptr)
    {
        return false; // detached: fixed capacity, nothing to grow — emit()'s own bounds check catches any shortfall
    }

    int victim = runtime->findEvictionVictim(lruTick);
    if(victim < 0)
    {
        return false;
    }

    runtime->evict((uint32_t)victim, count * 2);

    buf = (uint16_t *)(uintptr_t)runtime->arenaCursor;
    capacity = (runtime->arenaEnd - runtime->arenaCursor) / 2;

    return true;
}

uint32_t Assembler::emit(uint16_t word)
{
    uint32_t at = pc();

    assert(count <= capacity);

    if(count == capacity)
    {
        if(!this->growForAttached())
        {
            fail();
            return at; // host's mocked runtimeBail() returns normally; every call site, including this one, must return right after per fail()'s own contract
        }
    }

    assert(count < capacity);

    buf[count++] = word;

    return at;
}

uint32_t Assembler::stackFloor() const
{
    return runtime != nullptr ? runtime->liveStackFloor() : detachedStackFloor;
}

void Assembler::fail()
{
    runtimeBail(runtime, RESOURCE_ERROR_CODE);
}

// ── branches ────────────────────────────────────────────────────────────

uint32_t Assembler::placeholderCondBranch(ArmV6M::Condition c)
{
    return emit(ArmV6M::condBranch(c, ArmV6M::Ioff<1, 8>(0)));
}

uint32_t Assembler::placeholderBranch()
{
    return emit(ArmV6M::b(ArmV6M::Ioff<1, 11>(0)));
}

void Assembler::patchBranch(uint32_t siteOffset, uint32_t targetOffset)
{
    uint32_t idx = siteOffset / 2;
    if(idx >= count)
    {
        return; // GCOV_EXCL_LINE — only reachable once overflow already started
    }
    uint16_t isn = buf[idx];
    int32_t delta = (int32_t)targetOffset - (int32_t)(siteOffset + 4);
    buf[idx] = ArmV6M::isCondBranch(isn)
        ? ArmV6M::setCondBranchOffset(isn, ArmV6M::Ioff<1, 8>((int16_t)delta))
        : ArmV6M::setBranchOffset(isn, ArmV6M::Ioff<1, 11>((int16_t)delta));
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

void Assembler::linkIntoChain(Label &label, uint32_t site)
{
    patchBranch(site, label.chain == -1 ? site : (uint32_t)label.chain);
    label.chain = (int32_t)site;
}

void Assembler::branchTo(Label &label, ArmV6M::Condition c)
{
    linkIntoChain(label, placeholderCondBranch(c));
}

void Assembler::branchTo(Label &label)
{
    linkIntoChain(label, placeholderBranch());
}

void Assembler::flushPool()
{
    flushPoolImpl(false);
}

void Assembler::bind(Label &label)
{
    // Flush first, so nothing already chained onto label can end up
    // resolving to a spot the flush then inserts pool words into — the
    // one ordering guarantee that makes this safe to call from anywhere
    // a fixup resolves to "wherever we are now."
    flushPoolImpl(false);
    uint32_t target = pc();
    for(int32_t site = label.chain; site != -1;)
    {
        uint32_t prevSite = readBranchTarget((uint32_t)site);
        patchBranch((uint32_t)site, target);
        site = (prevSite == (uint32_t)site) ? -1 : (int32_t)prevSite;
    }
    label.chain = -1;
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
    // The immediate field carries nothing meaningful — flushPool()
    // recovers the value from pendingValues, never by re-decoding this
    // instruction — so a placeholder zero is as good as any tag.
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

// Close the open chunk: branch around the pool (nothing executes past a
// terminator, so the end-of-procedure flush needs none), pad to a word
// boundary, then walk the chunk's own (site, value) pairs directly —
// nothing here scans the output buffer, so a BR_TABLE(N>2) jump table's
// raw halfwords are never at risk of being misread as a pooled site, no
// matter what's still open when one is emitted. Identical values dedupe
// to one shared pool word.
void Assembler::flushPoolImpl(bool endOfProcedure)
{
    if(pendingCount == 0)
    {
        return;
    }

    uint32_t branchSite = 0;
    if(!endOfProcedure)
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

    if(!endOfProcedure)
    {
        patchBranch(branchSite, pc());
    }
    pendingCount = 0;
}

void Assembler::ensurePoolRoom(uint32_t poolEntries)
{
    if(pendingCount == 0)
    {
        return;
    }
    uint32_t poolEnd = roundUpToWord(pc() + 2) + 4 * (pendingCount + poolEntries);
    bool reachAtRisk = poolEnd - pendingSites[0] + LITERAL_POOL_REACH_MARGIN > LITERAL_POOL_MAX_REACH;
    bool countAtRisk = pendingCount + poolEntries > POOL_MAX_PENDING;
    if(reachAtRisk || countAtRisk)
    {
        flushPoolImpl(false);
    }
}

uint32_t Assembler::finalize()
{
    flushPoolImpl(/*endOfProcedure=*/true);
    if(runtime != nullptr)
    {
        uint32_t need = count * 2;
        uint32_t dest = runtime->allocate(need);
        assert(dest == (uint32_t)(uintptr_t)buf); // GCOV_EXCL_LINE — growForAttached's own base-tracking invariant
        runtime->markCompiled(procIdx, dest);
    }
    return count;
}

} // namespace jitc
