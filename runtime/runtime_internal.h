/* Shared between enter_program.cpp, dispatch_abi.cpp, and compile_proc.cpp;
 * not part of runtime_host.h's public API. */
#ifndef RUNTIME_INTERNAL_H
#define RUNTIME_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <cassert>
#include "runtime_host.h"
#include "proc_scan.h"
#include "ext.h"
#include "decode_instr.h"

/* LANDING_SUCCESS/LANDING_TRAP/LANDING_RESOURCE_ERROR live in
 * runtime_host.h — they are part of ProgramResult's own contract, and
 * runtime.S needs LANDING_TRAP under __ASSEMBLER__. So do the RESOURCE_*
 * codes one of them carries in ProgramResult::value: the host is who
 * reads them. There is deliberately no blanket code here for a new bail
 * site to reach for. */

class Runtime;

/* An attached Assembler's own direct exit when it cannot free enough
 * arena room even after evicting everything resident (compiler/src/
 * assembler.h's fail()) — restores the caller's own saved sp and
 * transfers to the landing with code, never returning. The target
 * definition (runtime/compile_proc.cpp) is the old bailOut moved
 * verbatim: mov sp, savedSp; bx to the sentinel landing address, tagged
 * LANDING_TRAP. A host build has no such landing to jump to, so it
 * supplies its own longjmp-based definition instead (test/host's own
 * support file) — the same escape 1test's own CHECK() already unwinds
 * through, and the same pattern test_runtime_arena.cpp already uses to
 * satisfy trampolineAddr for a Runtime built outside the real runtime. */
extern "C" [[noreturn]] void runtimeBail(Runtime *runtime, uint32_t code);

/* One procedure's whole entry: the runtime's own mutable dispatch state
 * (codePtr/lastUsed, exactly what runtime.S's hand-written asm touches)
 * plus the static facts a whole-program first pass (Runtime::init, below)
 * reads out of the wire bytes once and compileProc would otherwise have
 * to rediscover on every recompile. One combined array rather than two
 * parallel ones: a struct may have only one trailing flexible-array
 * member, and docs/design.md §2's own founding principle is that
 * everything lives in one contiguous region reachable through the fixed
 * ABI pointers, not a second ad hoc allocation. Stepped up from the
 * original DispatchEntry's 8 bytes to 16 to fit the static half in,
 * keeping the power-of-two addressing runtime.S depends on (idx*16 is
 * still a bare shift, not a multiply). */
struct ProcSlot
{
    uint32_t codePtr;    /* mutable — dispatch address (Thumb bit set) or trampolineAddr */
    uint32_t lastUsed;   /* mutable — LRU tick, stamped by runtime.S's callHelper/returnHelperTail */
    uint32_t bodyPtr;    /* static — absolute flash address of body_bytes (past this proc's own arg_count LEB128) */
    uint32_t staticInfo; /* static, packed: bit31 needsLRSave; bits[30:20] argCount; bits[19:0] bodyBytes */

    /* argCount/bodyBytes' own field widths — a procedure whose real value
     * doesn't fit either is rejected outright by Runtime::init() rather
     * than silently truncated. Not load-bearing anywhere else; revisit if
     * a real target's own programs need more headroom in either. */
    static constexpr uint32_t MAX_ARG_COUNT = (1u << 11) - 1;
    static constexpr uint32_t MAX_BODY_BYTES = (1u << 20) - 1;

    void setStaticInfo(uint32_t argCount, uint32_t bodyBytes, bool needsLRSave)
    {
        staticInfo = (needsLRSave ? 0x80000000u : 0u) | (argCount << 20) | bodyBytes;
    }

    uint32_t argCount() const
    {
        return (staticInfo >> 20) & MAX_ARG_COUNT;
    }

    uint32_t bodyBytes() const
    {
        return staticInfo & MAX_BODY_BYTES;
    }

    bool needsLRSave() const
    {
        return (staticInfo & 0x80000000u) != 0;
    }
};
static_assert(sizeof(ProcSlot) == 16, "power-of-two: idx*16 must stay a shift, not a multiply — runtime.S's own hardcoded stride");

/* Link-time const, defined in dispatch_abi.cpp. `extern` has to be repeated
 * at the definition too: a `const` global defaults to internal linkage in
 * C++, so without it neither compileProc nor this class's own methods could
 * see it. */
extern const uint32_t trampolineAddr;

/* The whole per-execution state, allocated once by whichever enterProgram*
 * variant is in play and reached everywhere else through a single pointer
 * (r9). `slots` is a flexible array member, 0-based: `[0]` is the
 * sentinel (where a RETURN off the bottom of the excursion lands), and
 * `[1..procCount]` are the real per-procedure slots. The ABI's own
 * dispatchBase (r8) is `&slots[1]`, so every method here goes through
 * `slot()` to apply that `+1` exactly once rather than leaving each call
 * site to get it right independently.
 *
 * A plain, trivial aggregate deliberately: every caller builds one by
 * reinterpreting a raw, `alignas`-aligned byte buffer (a C-stack VLA or a
 * static array) as a `Runtime*` and calling `init()` to fill it in, never by
 * constructing one normally. A non-trivial type (virtual functions, a real
 * constructor) wouldn't survive that cast. */
class Runtime
{
public:
    uint32_t savedSp; /* written by enterDispatch's own asm, never by C++ */
    uint32_t arenaEnd;
    uint32_t arenaCursor;
    uint32_t procCount;
    /* The extension serving THIS program's bytes (compiler/src/ext.h),
     * from enterProgram*'s own argument. Here rather than in a file-static
     * because it is per-program state: lazy compilation means the
     * translator reads it back on each procedure's first dispatch, long
     * after enterProgram* returned, and Runtime is the context that lives
     * that long and is already threaded everywhere the translator looks.
     * Costs one word and shifts RUNTIME_DISPATCH_TABLE_OFFSET, which
     * runtime.S picks up from the macro rather than hardcoding. */
    const ExtHooks *ext;
    /* The lowest address the translator's own LOOP/BR_TABLE recursion may
     * safely reach, checked live via liveStackFloor() below by
     * translateBody's own guard (translate_proc.cpp), reached through the
     * const Runtime& every translateProc call already carries.
     * stackLimit is the hard, whole-execution floor. arenaOverlapsStack is
     * true only for enterProgramOnStack, whose code arena is anchored at
     * stackLimit itself and grows up from there — i.e. the same memory the
     * translator's own recursion runs on, unlike enterProgramSplit's
     * caller-supplied, unrelated memory. */
    uint32_t stackLimit;
    uint32_t arenaOverlapsStack; /* 0/1, not bool, so this struct's layout
                                  * (and RUNTIME_DISPATCH_TABLE_OFFSET/
                                  * DISPATCH_SENTINEL_OFFSET, runtime.S's own
                                  * hardcoded mirrors of it) never depends on
                                  * a compiler's bool size/alignment choice. */
    ProcSlot slots[];

private:
    /* Every dispatch-table codePtr write goes through one of these two:
     * BX/BLX require bit 0 set to stay in Thumb mode on ARMv6-M, and a value
     * computed from raw arena arithmetic carries no such bit on its own,
     * unlike a .thumb_func-tagged link-time symbol (already odd). */
    static void setCodePtr(ProcSlot &entry, uint32_t addr)
    {
        entry.codePtr = addr | 1u;
    }

    /* Slide an already-tagged codePtr down by delta bytes (compaction) —
     * delta is always even, so the Thumb bit survives the subtraction, but
     * re-asserting it here costs nothing. */
    static void slideCodePtr(ProcSlot &entry, uint32_t delta)
    {
        entry.codePtr = (entry.codePtr - delta) | 1u;
    }

public:
    /* Bytes of storage one Runtime needs for procCount procedures: the
     * fixed header plus one slot per procedure, plus the sentinel
     * (index 0). */
    static uint32_t storageBytesFor(uint32_t procCount)
    {
        return (uint32_t)sizeof(Runtime) + (procCount + 1) * (uint32_t)sizeof(ProcSlot);
    }

    /* Fills in every field except savedSp (enterDispatch's own job),
     * points every dispatch slot at the translator trampoline, and — the
     * whole-program first pass docs/design.md §16 (and this session) found
     * missing entirely — walks the real wire bytes once to fill in every
     * slot's static half too: where its body lives, its own arg_count, and
     * whether it needs lr protected (jitc::scanProcBody, ported from
     * packages/machine/src/bytecode.ts's decodeProcBody). programBytes is
     * the whole serialized program (isa-core.md §5.5's proc_count +
     * procedures, past the jit-armv6m-specific max_call_depth/total_depth
     * envelope a caller's own parseProgramHeader already stripped);
     * bodyOffset is where procedure 0's own arg_count LEB128 begins.
     *
     * Returns 0 on success, else the RESOURCE_* code (runtime_host.h) for
     * whichever of the four ways a procedure can be rejected happened —
     * never touching arenaCursor/dispatch state past the procedure it
     * failed on. The caller returns that code as its own ProgramResult
     * without ever reaching enterDispatch. Doesn't allocate
     * codeArenaBase's own storage; where that lives is the caller's
     * choice. */
    uint32_t init(const uint8_t *programBytes, uint32_t programSize, uint32_t bodyOffset, uint32_t procCount,
        uint32_t codeArenaBase, uint32_t codeArenaSize, uint32_t stackLimit, uint32_t arenaOverlapsStack,
        const ExtHooks *extension = nullptr)
    {
        this->ext = extension;
        /* Rounded down, mirroring arenaCursor's own rounding up just below:
         * allocate()'s reserveFor() rounds every reservation up to a
         * multiple of 4, so if the gap between arenaCursor and arenaEnd
         * weren't itself always a multiple of 4, a procedure that exactly
         * fills the remaining capacity could have that rounding push
         * arenaCursor past arenaEnd. Both arenaCursor (allocate()) and this
         * gap (evict(), via occupiedSizeOf's codePtr differences) only ever
         * move by multiples of 4 afterward, so 4-aligning both ends here
         * keeps the gap a multiple of 4 for the arena's entire lifetime. */
        arenaEnd = (codeArenaBase + codeArenaSize) & ~3u;
        /* Rounded up here, not left to the caller: enterProgramOnStack
         * anchors the arena at stackLimit and enterProgramSplit takes the
         * base as a parameter, so neither can be covered by an alignas on
         * some array. Together with allocate()'s own rounding this makes
         * "arenaCursor is 4-aligned" inductive, which is what every
         * procedure's PC-relative literal loads depend on — see
         * allocate(). */
        arenaCursor = (codeArenaBase + 3u) & ~3u;
        // codeArenaSize smaller than codeArenaBase's own alignment slack —
        // a degenerate caller-supplied arena, not reachable from untrusted
        // wire bytes (those only ever narrow procCount/argCount/body sizes,
        // never codeArenaBase/codeArenaSize).
        assert(arenaCursor <= arenaEnd); // GCOV_EXCL_LINE
        this->procCount = procCount;
        this->stackLimit = stackLimit;
        /* The sentinel's own static half is the extension's scratch
         * (RUNTIME_EXT_STATE_OFFSET); its lastUsed is a real LRU field that
         * returnHelperTail stamps on the way out of the entry procedure,
         * read by nobody. Neither is written anywhere else and the storage
         * is a caller's VLA, so without this they start as whatever was on
         * the stack. */
        slots[0].lastUsed = 0;
        slots[0].bodyPtr = 0;
        slots[0].staticInfo = 0;
        this->arenaOverlapsStack = arenaOverlapsStack;

        /* Checked once, before the walk can call into the extension: an
         * extension built against a different seam version must not have
         * its decode() trusted at all. Nothing else can catch it — the wire
         * carries only the consequences of effects, never the effects. */
        if(extension != nullptr && extension->abiVersion != jitc::EXT_ABI_VERSION)
        {
            return RESOURCE_PROGRAM_EXT_ABI;
        }

        uint32_t pos = bodyOffset;
        for(uint32_t i = 0; i < procCount; i++)
        {
            assert(pos < programSize); // GCOV_EXCL_LINE — malformed/truncated program, matching decode_instr.cpp's own convention
            uint32_t argCount = jitc::decodeLeb128(programBytes, pos, pos);
            uint32_t bodyStart = pos;
            jitc::BodyScanResult scan = jitc::scanProcBody(programBytes, programSize, bodyStart, extension, stackLimit);
            /* Four separate rejections, not one: "this program is malformed"
             * and "this deployment is out of stack" want different answers
             * from whoever gets the ProgramResult back. */
            if(!scan.ok)
            {
                return scan.failCode; // the walk already named which of its five rejections this is
            }
            if(argCount > ProcSlot::MAX_ARG_COUNT)
            {
                return RESOURCE_LIMIT_ARG_COUNT;
            }
            if(scan.bodyBytes > ProcSlot::MAX_BODY_BYTES)
            {
                return RESOURCE_LIMIT_BODY_BYTES;
            }

            ProcSlot &s = slot(i);
            setCodePtr(s, trampolineAddr);
            s.lastUsed = 0;
            s.bodyPtr = (uint32_t)(uintptr_t)(programBytes + bodyStart);
            s.setStaticInfo(argCount, scan.bodyBytes, scan.needsLRSave);

            pos = bodyStart + scan.bodyBytes;
        }
        return 0;
    }

    /* The extension serving this program, or nullptr. */
    const ExtHooks *extension() const
    {
        return ext;
    }

    /* Procedure idx's own slot — [0] is the sentinel, so every real
     * procedure index is offset by one. */
    ProcSlot &slot(uint32_t idx)
    {
        return slots[idx + 1];
    }

    const ProcSlot &slot(uint32_t idx) const
    {
        return slots[idx + 1];
    }

    uint32_t sentinelLandingAddress() const
    {
        return slots[0].codePtr;
    }

    bool isResident(uint32_t idx) const
    {
        return slot(idx).codePtr != trampolineAddr;
    }

    bool hasRoomFor(uint32_t need) const
    {
        return arenaEnd - arenaCursor >= need;
    }

    uint32_t liveStackFloor() const
    {
        return (arenaOverlapsStack && arenaCursor > stackLimit) ? arenaCursor : stackLimit;
    }

    static uint32_t reserveFor(uint32_t need)
    {
        return (need + 3u) & ~3u;
    }

    uint32_t allocate(uint32_t need)
    {
        uint32_t dest = arenaCursor;
        arenaCursor += reserveFor(need);
        return dest;
    }

    void markCompiled(uint32_t idx, uint32_t dest, uint32_t lruTick)
    {
        ProcSlot &entry = slot(idx);
        setCodePtr(entry, dest);
        entry.lastUsed = lruTick;
    }

    int findEvictionVictim(uint32_t now) const
    {
        int victim = -1;
        uint32_t oldestAge = 0;
        for(uint32_t i = 0; i < procCount; i++)
        {
            if(!isResident(i))
            {
                continue;
            }
            uint32_t age = now - slot(i).lastUsed;
            if(victim < 0 || age > oldestAge)
            {
                oldestAge = age;
                victim = (int)i;
            }
        }
        return victim;
    }

    uint32_t occupiedSizeOf(uint32_t idx) const
    {
        uint32_t addr = slot(idx).codePtr & ~1u;
        uint32_t gapEnd = arenaCursor;
        for(uint32_t i = 0; i < procCount; i++)
        {
            if(!isResident(i))
            {
                continue;
            }
            uint32_t a = slot(i).codePtr & ~1u;
            if(a > addr && a < gapEnd)
            {
                gapEnd = a;
            }
        }
        return gapEnd - addr;
    }

    void evict(uint32_t idx, const uint16_t *end)
    {
        uint32_t victimAddr = slot(idx).codePtr & ~1u;
        uint32_t victimSize = occupiedSizeOf(idx);
        uint32_t gapEnd = victimAddr + victimSize;
        uint32_t tailLen = (uint32_t)end - gapEnd;

        memmove((void *)(uintptr_t)victimAddr, (void *)(uintptr_t)gapEnd, tailLen);
        arenaCursor -= victimSize;

        ProcSlot &victimEntry = slot(idx);
        setCodePtr(victimEntry, trampolineAddr);
        victimEntry.lastUsed = 0;

        for(uint32_t i = 0; i < procCount; i++)
        {
            ProcSlot &entry = slot(i);
            if(entry.codePtr != trampolineAddr && (entry.codePtr & ~1u) >= gapEnd)
            {
                slideCodePtr(entry, victimSize);
            }
        }
    }
};

/* The other half of runtime_host.h's own split explanation, right above
 * its RUNTIME_DISPATCH_TABLE_OFFSET/DISPATCH_SENTINEL_OFFSET #defines:
 * runtime.S hardcodes both of those since it can't call offsetof itself,
 * and can't see this file's own Runtime/ProcSlot types at all (it
 * #includes runtime_host.h under __ASSEMBLER__, which hides this file
 * from it entirely) — so the two asserts below live here, next to the
 * real struct, checking the *other* file's numbers against it. Change
 * either side and this is what catches the drift at compile time instead
 * of letting it corrupt memory silently.
 *
 * The table offset is checked only for a 32-bit target, the only thing
 * runtime.S is ever assembled for: on the 64-bit host that builds the
 * compiler's own unit tests the struct is legitimately narrower by one
 * uint32_t than it would be with a pointer field, so nothing here depends
 * on pointer width to begin with any more (flashProcs is gone) — this
 * static_assert now holds on both widths, but is still guarded the same
 * way in case that ever changes again. */
#if UINTPTR_MAX == 0xFFFFFFFFu
static_assert(offsetof(Runtime, slots) + sizeof(ProcSlot) == RUNTIME_DISPATCH_TABLE_OFFSET,
    "runtime.S's own RUNTIME_DISPATCH_TABLE_OFFSET must match Runtime's real layout");
/* The extension scratch words are the sentinel slot's static half — its
 * lastUsed is not among them, since returnHelperTail stamps that
 * unconditionally (runtime_host.h explains why). Emitted code addresses
 * them by this constant, so it has to track the struct rather than be
 * believed. */
static_assert(offsetof(Runtime, slots) + offsetof(ProcSlot, bodyPtr) == RUNTIME_EXT_STATE_OFFSET,
    "RUNTIME_EXT_STATE_OFFSET must be the sentinel slot's first unused word");
static_assert(RUNTIME_EXT_STATE_WORDS * 4 + offsetof(ProcSlot, bodyPtr) == sizeof(ProcSlot),
    "the extension scratch must be exactly the sentinel slot's unused tail");
#endif
static_assert(sizeof(ProcSlot) == DISPATCH_SENTINEL_OFFSET,
    "runtime.S's own DISPATCH_SENTINEL_OFFSET must match sizeof(ProcSlot)");

/* Every RESOURCE_* code must be distinct. Nothing else enforces it: they
 * are #defines, so two names holding the same value compile fine and even
 * compare equal, which is exactly how a duplicate survived long enough to
 * make a test pass for the wrong reason. Add a code above, add it here. */
namespace
{
constexpr uint32_t RESOURCE_CODES[] = {
    RESOURCE_PROGRAM_NO_PROCS, RESOURCE_PROGRAM_BODY_UNTERMINATED,
    RESOURCE_PROGRAM_CALLEE_RANGE, RESOURCE_PROGRAM_ENTRY_ARG_COUNT,
    RESOURCE_PROGRAM_ENTRY_DEPTH, RESOURCE_PROGRAM_EXT_UNKNOWN,
    RESOURCE_PROGRAM_EXT_UNSUPPORTED, RESOURCE_PROGRAM_EXT_ABI,
    RESOURCE_PROGRAM_RESERVED_OPCODE,
    RESOURCE_EXHAUSTED_ARENA, RESOURCE_EXHAUSTED_STACK_BUDGET,
    RESOURCE_EXHAUSTED_TRANSLATOR_STACK, RESOURCE_EXHAUSTED_SCAN_STACK,
    RESOURCE_LIMIT_WINDOW_RECLAIM, RESOURCE_LIMIT_SPILL_OFFSET,
    RESOURCE_LIMIT_BRANCH_RANGE, RESOURCE_LIMIT_LOOP_BACK_EDGE,
    RESOURCE_LIMIT_ARG_COUNT, RESOURCE_LIMIT_BODY_BYTES,
};

constexpr bool resourceCodesDistinct()
{
    for(unsigned i = 0; i < sizeof(RESOURCE_CODES) / sizeof(RESOURCE_CODES[0]); i++)
    {
        for(unsigned j = i + 1; j < sizeof(RESOURCE_CODES) / sizeof(RESOURCE_CODES[0]); j++)
        {
            if(RESOURCE_CODES[i] == RESOURCE_CODES[j])
            {
                return false;
            }
        }
        /* The signature and a nonzero class nibble are what make a code
         * recognizable in a raw hex dump; the low byte stays reserved for a
         * future detail payload (runtime_host.h). */
        if((RESOURCE_CODES[i] >> 16) != RESOURCE_ERROR_SIGNATURE
            || RESOURCE_ERROR_CLASS(RESOURCE_CODES[i]) == 0
            || (RESOURCE_CODES[i] & 0xffu) != 0)
        {
            return false;
        }
    }
    return true;
}
} // namespace

static_assert(resourceCodesDistinct(),
    "RESOURCE_* codes must be distinct, carry the 0x5245 signature and a class nibble, and leave the low byte zero");

#endif /* RUNTIME_INTERNAL_H */
