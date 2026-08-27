# Assembler restructuring — plan and status

## Status

**Stage 1: done, verified green.** **Stage 2: done, verified green.**
**Post-Stage-2 cleanup: done, verified green.**
**`materializeImm32` synthesis rework (independent, see below): done,
verified green.**
166/166 host tests (2 fewer — `imm32SynthCost`/`isPoolingEligible`'s own
tests no longer apply, see below), 9/9 QEMU tests.

| Stage 1 item | Status |
|---|---|
| 1.1 `Emitter`→`Assembler`, two constructors | done |
| 1.2 Pool stores values, not bytecode tags | done |
| 1.3 `Label`/`bind()` replace raw-pc fixups | done |
| 1.4 Unified `materializeImm32` | done |
| 1.5 `ArenaRoom` deleted, arena folded into `Assembler` | done |
| 1.6 Direct exit on `RESOURCE_ERROR` | done |
| 1.7 Force-pooled call record, fixpoint deleted | done |
| 1.8 Signature cleanup (`pendingPoolBytes` gone) | done |
| 1.9 Companion fixes | done (both) |
| Stack budget re-measurement | done (488, was 400) |

| Stage 2 item | Status |
|---|---|
| `runtime_host.cpp` split into `enter_program.cpp` + `dispatch_abi.{h,cpp}` | done |
| Threefold `runtimeStorage`/`enterProgramCore` duplication collapsed | done (`enterProgramWithHeader`) |
| `compile_proc_real.cpp` → `compile_proc.cpp` | done |
| `abi_strategy.{h,cpp}` moved next to layer-2 conventions | **not done** — see below |
| `RUNTIME_DISPATCH_TABLE_OFFSET`/`DISPATCH_SENTINEL_OFFSET` split resolved | done (documented, not merged — see below) |
| `docs/design.md` updates (§2, §9, §11, new §16 item) | done |
| Doc-drift cleanup from 1.9 | done |

| Post-Stage-2 cleanup item | Status |
|---|---|
| `Runtime::arenaBase` (dead field) deleted | done |
| Plain `enterProgram()` deleted; callers declare their own arena | done |
| `abiEmitReturn`'s redundant `fitsImm8` branch collapsed | done |

Two Stage 2 deviations from the written plan:

- **`abi_strategy.{h,cpp}` did not move.** On inspection its helper-vector
  indices and record packing (`packRecord`, the `Uoff<2,5>` index
  literals) are genuinely emission code — they belong with the other
  `compiler/src` emitters that share its `Assembler&` calling convention,
  not with `dispatch_abi.h`'s own extern declarations and stack-cost
  constants. Moving them would have split `abi_strategy.cpp` from the
  `STUB_SIZE`/`abiEmitPrologue` it's defined alongside for no real
  layering gain. Left in place.
- **`RUNTIME_DISPATCH_TABLE_OFFSET`/`DISPATCH_SENTINEL_OFFSET` couldn't
  physically merge.** The constants must stay in `runtime_host.h` (needed
  under `__ASSEMBLER__`, before `Runtime`/`ProcSlot` are even defined
  anywhere) while their `static_assert`s must stay in `runtime_internal.h`
  (need the complete struct definitions). "Resolved" means both files now
  carry an explicit, bidirectional comment pointing at the other half of
  the guarantee — not a physical move, which the `__ASSEMBLER__`
  constraint rules out.

Deviation from the written plan: no `translateProc(uint16_t*, uint32_t)`
convenience overload was kept — a single `Assembler&`-taking signature
turned out cleaner once in the code, at the cost of more mechanical
test-file edits than planned.

Two bugs found and fixed during implementation, not just documented (see
"Companion fixes," below, for the third — planned — one):

- **`growForAttached` failed immediately on a worst-case reservation
  shortfall**, contradicting the codebase's own stated philosophy that
  `reserve()`'s budgets are loose over-estimates, not hard requirements.
  This broke real eviction on QEMU (`EvictionCallerAndCalleeNeverCoresident`,
  `EvictionSlidesAProcedureHoldingAPooledLiteral`) until fixed to
  best-effort evict-then-stop, matching `emit()`'s own bounds check as
  the actual failure trigger. Reproduced and root-caused on the host via
  a throwaway probe (real `Runtime`, `mmap(MAP_32BIT)` so addresses
  survive the `uint32_t` truncation) before touching QEMU again.
- Two hand-derived test expectations were simply arithmetic errors
  (`imm32SynthCost(400)` is 3, not 4; a TRAP's pooled record flushes
  early via `bind()`, not at end-of-procedure) — corrected against actual
  emitted bytes, not re-guessed.

Not written: the plan's suggested new host test for "attached Assembler
growing over a real `Runtime`." The exact scenario was exercised via the
throwaway probe above, which found both real bugs, but no permanent test
for it exists yet.

---

The rest of this document is the original restructuring plan, kept as
the design record — both stages are now done; deviations from the plan
as written are called out inline and in the status section above.

## Context

`jit-armv6m` had no clean boundary between "JIT compilation logic" and
"specifics of code generation." The evidence was concrete and mostly
mechanical:

- **The literal pool lived in the wrong place.** Its whole state was four
  fields on `translate_proc.cpp`'s `Ctx` plus five file-local functions
  (`literalPoolDebt`, `ensureRoom`, `pooledLiteralValue`,
  `flushLiteralPool`, `guardLiteralPoolReach`), while the primitives it
  drove (`placeholderLiteralLoad`, `getLiteralOffsetAt`,
  `patchLiteralOffset`) sat in `Emitter`. Each of those three had exactly
  one production caller, all in `translate_proc.cpp`.
- **Pool debt was threaded by hand** through three `blocks.h` signatures
  (`emitGuardedBranch`, `openBrTable`, `closeBlockEnd`) as
  `pendingPoolBytes`, to be consumed at *one* comparison.
- **Pooling was unreachable from most of the compiler.** `abi_strategy.cpp`,
  `blocks.cpp`, and `shape.cpp`'s `materializeShape` all called
  `emitSynthesizeImm32` directly — `materializeShape` was the biggest
  blind spot, since every `AccState::flush` funneled through it with no
  pool access at all.
- **`ArenaRoom` was a fake isolation.** A one-method abstract interface
  with exactly one implementor, to hide a `Runtime` the host build
  *already* instantiated directly (`test_runtime_arena.cpp`, whose entire
  seam was one `extern const uint32_t trampolineAddr`). `runtime_internal.h`
  is 372 lines of portable, header-only C++ with no inline asm — nothing
  to isolate from.
- **A structural hazard fell out of the split.** Because
  `flushLiteralPool` recovered each pooled word by *re-decoding the
  bytecode* at flush time, it had to scan the output window for
  `isLiteralAccess` halfwords — which is why `BR_TABLE N>2`'s jump table
  needed a forced flush before it, and why a flush could never be
  inserted at the many `patchBranch(site, e.pc())` sites in `blocks.cpp`
  where a pending fixup resolves to the current position.

The last point is what made this a restructuring rather than a tidy-up:
the same re-decode design that forced the scan is also what blocked
`abi_strategy` from pooling at all (a call record is not any
instruction's immediate, so it has no bytecode tag to carry), and what
forced `findResumeOffset`'s 5-round fixpoint to exist.

**Intended outcome.** One assembler-like layer owns buffer, branch
fixups, the literal pool, immediate-scheme selection, arena
eviction/compaction, and final registration — becoming the only link
between the runtime dispatch table and the compiler core. The core
compiler logic below it becomes environment-free and testable in
isolation, and the flush-ordering friction disappears by construction.

## Target layering

1. **Initialization** — `enterProgram*`, header parse, dispatch-table
   allocation, `Runtime::init`'s directory walk, static stack-stat check.
2. **Dispatch conventions** — `runtime.S`'s helpers, the helper vector,
   record packing, `STUB_SIZE`, `LANDING_TRAP`, `RESOURCE_ERROR_CODE`, the
   trap/landing mechanism.
3. **Per-procedure compile**
   - **3a Landing** — gather invocation + global refs, happy path and
     abnormal exit. (`compileProc`)
   - **3b `Assembler`** — the only holder of `Runtime` access/mutation.
   - **3c Core compiler** — environment-free; thoroughly unit-testable.

---

## Stage 1 — the `Assembler` (done)

### 1.1 Type and files

`emitter.h` → `assembler.{h,cpp}`, `Emitter` → `Assembler`. Two
constructors:

```cpp
Assembler(uint16_t *buf, uint32_t capacityHalfwords, uint32_t stackFloor = 0); // detached
Assembler(Runtime *runtime, uint32_t procIdx, uint32_t lruTick);               // attached
```

Detached = fixed capacity, failure latches (`overflowed()`), used by
every host test and QEMU pre-measurement call. Attached = owns arena
growth and exits directly on unrecoverable exhaustion. `lruTick` is
read once by the caller (`compileProc`) and passed in, keeping
`register ... asm("r11")` out of the Assembler and out of the host build.

### 1.2 The pool stores values, not bytecode tags

The enabling change. `pendingSites[]`/`pendingValues[]` (16 entries) replace
the bytecode-tag/re-decode scheme entirely.

- `materializeImm32(dstReg, value)` has no bytecode-pc parameter — callable
  from anywhere, including `abi_strategy.cpp`.
- No output-window scan → no hazard around `BR_TABLE(N>2)`'s jump table →
  the forced flush before `openBrTableJump` is gone.
- The old 8-bit bytecode-distance tag limit is gone; the only mid-procedure
  flush triggers left are reach (1020 bytes) and pool-full (16 entries).
- Identical values dedupe to one shared pool word.

### 1.3 `Label`/`bind()` replace raw-pc branch targets

```cpp
struct Label { int32_t chain = -1; };
void branchTo(Label &, ArmV6M::Condition);
void branchTo(Label &);
void bind(Label &);   // flush-if-pending, THEN resolve the whole chain to here
```

Generalizes `blocks.cpp`'s pre-existing `endFixupChain` self-linking
trick — the chain is threaded through the branches' own encoded offsets,
no side array. `bind()` is the *only* place a forward fixup resolves to
"wherever we are now," so a label's target can never land on top of pool
words a flush inserts. `Frame::nextCaseFixup`/`endFixupChain`/`exitFixup`
are `Label`-typed now.

### 1.4 Unified immediate materialization

`materializeImm32` is the one entry point (pool vs. inline synthesis,
decided by `isPoolingEligible`/`POOLING_MIN_LENGTH=4`). Now reachable
from `shape.cpp` (closing the `AccState::flush` blind spot),
`blocks.cpp` (comparison operands, `openBrTableJump`'s `n`), and
`abi_strategy.cpp`.

**Not converted, and not a bypass:** shift amounts. A shift's `IMM_ACC`
operand is consumed directly as an `Imm<5>` instruction field; pooling it
would silently reclassify the op from `ShiftImm` to `TwoOpInPlace` — a
behavior change, not a size change.

### 1.5 Arena ownership; `ArenaRoom` deleted

`arena_room.h` and `RuntimeArenaRoom` are gone. The attached `Assembler`
absorbs the evict-and-rebase loop, reusing `Runtime::findEvictionVictim`/
`Runtime::evict` unchanged.

```cpp
void reserve(uint32_t maxBytes, uint32_t poolEntries = 0);
uint32_t finalize(); // end-of-procedure flush; attached: allocate + markCompiled
```

### 1.6 Direct exit on `RESOURCE_ERROR`

```cpp
extern "C" [[noreturn]] void runtimeBail(Runtime *runtime, uint32_t code);
```

Target definition is the old `bailOut`, moved verbatim into
`compile_proc_real.cpp`. Host definition (`host_runtime_support.cpp`)
logs and `abort()`s rather than reproducing the sp-restore/jump escape —
reaching it in a host test means eviction genuinely couldn't recover,
which should fail loudly.

`translateProc` returns `uint32_t` (no `TranslateResult`). The
stack-nesting guard calls `Assembler::fail()` directly; on a detached
Assembler this latches and returns, on an attached one it never returns.

### 1.7 Force-pool the call record; fixpoint deleted

Both a `CALL`'s operands (the packed record, and `calleeIndex` when it
doesn't fit imm8) are force-pooled via `materializeImm32Pooled`, so the
sequence is a **compile-time constant** — 5 halfwords — rather than
something `findResumeOffset`'s old 5-round search had to converge on:

```cpp
static constexpr uint32_t CALL_SEQUENCE_HALFWORDS = 1 + 1 + 3; // record + calleeIndex + movHi/ldr/bx
uint32_t k = (preCallPc - STUB_SIZE) + CALL_SEQUENCE_HALFWORDS * 2; // closed form
```

`reserve(CALL_SEQUENCE_BYTES, /*poolEntries=*/2)` runs first, guaranteeing
no flush can land *inside* the sequence (which would shift `preCallPc`
out from under `k`).

### 1.8 Signature cleanup

`pendingPoolBytes` is gone from `emitGuardedBranch`/`openBrTable`/
`closeBlockEnd` — `blocks.cpp` reads `a.poolDebt()` directly.
`compile_proc_real.cpp` shrinks to: read `ProcSlot`, build
`calleeArgCounts`, construct an attached `Assembler`, call
`translateProc` (which finalizes the Assembler itself as its last step).

### 1.9 Companion fixes

- **`allocate`/`reserveFor` padding.** `growForAttached`'s eviction loop
  now compares against `Runtime::reserveFor(neededBytes)` (word-padded),
  not the raw byte count — closing the gap where `allocate()`'s own
  rounding could push `arenaCursor` past `arenaEnd` by up to 2 bytes.
- **`-ffixed-r8/r9/r10/r11`** added to `test/qemu/Makefile`'s `CXXFLAGS`.
  Previously asserted only in comments (`runtime.S`, `docs/design.md`),
  never actually passed to the compiler.

Four things were flagged here at the time. Two were already resolved by
Stage 2's own "Doc-drift cleanup" below (`runtime_host.h`'s overstated
"shares ProcSlot"/`TRAP`-propagation comments; `design.md`'s stale
sentinel-offset prose) — this paragraph just never got updated to say so.
The other two — `Runtime::arenaBase` being write-only, and `enterProgram`
never checking `arenaSize` against its own `ARENA_CAPACITY` — are fixed by
the post-Stage-2 cleanup below.

---

## Stage 2 — layer/file reorganization (done)

Purely structural.

- `runtime_host.cpp` split into layer 1 (`enter_program.cpp`: the entry
  points, `parseProgramHeader`, `requiredStackBytes`, `stackHasRoom`) and
  layer 2 (`dispatch_abi.{h,cpp}`: `helperVec`, `trampolineAddr`,
  `runtimeBail`, the fixed-cost constants). The threefold duplication of
  `parseProgramHeader` + `totalDepth * 4` + the `runtimeStorage` VLA +
  `enterProgramCore`, then across the three `enterProgram*` variants (this
  was still three at the time — see the post-Stage-2 cleanup below, which
  deletes plain `enterProgram`), collapsed into one shared
  `enterProgramWithHeader` — each variant still parses the header and
  runs its own stack-budget check first (the one thing that genuinely
  differed among them), then hands off.
- `compile_proc_real.cpp` → `compile_proc.cpp` (the "real" qualifier
  distinguished it from a mock retired per `design.md` §16 item 22);
  `runtimeBail`'s target definition moved out to `dispatch_abi.cpp`,
  next to `enterDispatch`'s own analogous sentinel/landing mechanism.
- `abi_strategy.{h,cpp}` **not** moved — see the deviation note above.
- `RUNTIME_DISPATCH_TABLE_OFFSET`/`DISPATCH_SENTINEL_OFFSET` split
  **documented, not merged** — see the deviation note above.
- `docs/design.md`: §9 (the `CALL` sequence is a constant 5 halfwords now,
  fixpoint description rewritten past-tense; the stale `r8−8`/`524280`
  sentinel-offset arithmetic, predating even Stage 1, fixed to `r8−16`/
  `1048560`), §11 (`ArenaRoom` "Done" paragraph rewritten for `Assembler`),
  §2 (`requiredStackBytes` file references; the stale
  `MOCK_TRANSLATOR_ENTRY_WORST_CASE_BYTES` paragraph, also predating
  Stage 1, replaced with a pointer at the now-current figure), new §16
  item 23 recording both stages and the two latent bugs Stage 1 fixed.
  (§10.2 turned out already accurate — its fixpoint-shaped language is
  about Thumb branch-range fixup, unrelated to the deleted
  `findResumeOffset`, which was always a §9 concern.)
- Doc-drift cleanup from 1.9: `runtime_host.h`'s stale "shares ProcSlot"
  claim and its overstated bytecode-`TRAP`-propagation comment both
  fixed; every stale `runtime_host.cpp`/`compile_proc_real.cpp` filename
  reference across comments and `README.md` swept to match the new
  layout.

---

## Post-Stage-2 cleanup (done)

Three loose ends, raised in review after Stage 2 landed:

- **`Runtime::arenaBase` was dead** — written once in `init()`, read
  nowhere. Deleted. `RUNTIME_DISPATCH_TABLE_OFFSET` (`runtime_host.h`)
  drops from 44 to 40 to match `Runtime`'s new layout;
  `runtime_internal.h`'s own `static_assert` pairing that `#define`
  against `offsetof(Runtime, slots)` is what would have caught a
  hand-arithmetic mistake here.
- **Plain `enterProgram()` deleted.** It was an arbitrary special case
  among the entry points: no `stackLimit` parameter, a fixed 512-byte
  `static` arena baked into `enter_program.cpp`
  (`ARENA_CAPACITY`/`arenaStorage`), and a blind
  `GENEROUS_TRANSLATOR_STACK_MARGIN` in place of a real budget check —
  exactly the kind of caller-invisible special-casing this whole
  restructuring exists to remove. A caller that wants a plain global
  arena now declares one itself (one line, sized to what it needs) and
  calls `enterProgramSplit` — the pattern
  `test/qemu/main.cpp`'s own `SplitThreeDeepCallChainSucceeds` already
  used. `enter_program.cpp`/`runtime_host.h` now have two entry points,
  not three. `test/qemu/main.cpp`'s fixture loop and its four
  eviction/`RESOURCE_ERROR` scenarios (previously the heaviest
  `enterProgram()` callers) moved to a file-local
  `enterProgramWithSharedArena` helper wrapping `enterProgramSplit`
  against one shared `static` buffer — the same shape the deleted
  function had, just no longer hidden inside the runtime. Switching these
  onto `enterProgramSplit` is a genuine (if inert in practice, since every
  fixture's own wire envelope encodes `max_call_depth`/`total_depth` as 0)
  behavior change: they now run the real up-front stack-budget check
  instead of none at all.
- **`abi_strategy.cpp`'s `abiEmitReturn` had a redundant branch.** Its
  deep-args reclaim-byte-count site hand-wrote
  `fitsImm8`-then-`MOVS`-else-`materializeImm32`, duplicating what
  `materializeImm32` already does internally — `imm32SynthCost` returns 1
  for any value that fits imm8, so `Assembler` was already going to emit
  that same single `MOVS`. Collapsed to one unconditional call.

---

## Verification record (Stage 1)

**Host** (`make -C test/host check`): 168/168 pass.
`test_assembler.cpp` (new) covers pool park/flush/dedupe with stored
values, both flush triggers (reach, pool-full — the tag trigger is gone),
`Label` bind/chain round-trips including a bind that flushes first, and
`imm32SynthCost`/`isPoolingEligible`. `test_emitter.cpp`/`test_imm_synth.cpp`
deleted (content absorbed or superseded).

**QEMU** (`make -C test/qemu qemu-test`): 9/9 pass, all 27 fixtures
produce identical values. Eviction scenarios in `main.cpp`
(`EvictionThreeDeepCallChain`, `EvictionCallerAndCalleeNeverCoresident`,
`EvictionSlidesAProcedureHoldingAPooledLiteral`) and both
`RESOURCE_ERROR` paths verified against the new direct-exit mechanism.

**Stack budget:** `TRANSLATOR_ENTRY_WORST_CASE_BYTES` re-measured with
`-fstack-usage` (not guessed): **488**, up from 400 — `compileProc`'s own
frame grew from 96 to 224 (it now holds the `Assembler` object directly),
and the deeper of two pre-`translateBody` chains is now the
last-argument-fold scan's eager-flush path (`AccState::flush` →
`materializeShape` → `materializeImm32` → `emitSynthesizeImm32Into`, 96
bytes) rather than the prologue's own `reserve()`/`growForAttached` chain
(64 bytes). Unaffected by Stage 2 — the file split moved this constant's
home (`dispatch_abi.h`), not the call chain it measures.

**Stage 2 rebuild:** clean rebuild of both suites after the file split
and the `docs/design.md` edits — 168/168 host, 9/9 QEMU, unchanged.
`run.elf`'s own `text` size dropped slightly (22512 → 22468 bytes) from
`enterProgramWithHeader` deduplicating what had been three inlined
copies.

**Post-Stage-2 cleanup rebuild:** clean rebuild after deleting
`arenaBase`, deleting plain `enterProgram()`, and collapsing
`abiEmitReturn`'s redundant branch — 168/168 host, 9/9 QEMU, unchanged
(including both `RESOURCE_ERROR` scenarios, still `0x52455343`).
`run.elf`'s `text` dropped again, 22468 → 22424 bytes — plain
`enterProgram()`'s own body (and the literal pool it carried) is simply
gone; `sharedArena`'s 512 bytes replace `arenaStorage`'s in `.bss`, a
wash.

---

## `materializeImm32` synthesis rework (done, independent of Stages 1/2)

Not part of this restructuring's own plan — a separate, later rework of
`Assembler::materializeImm32`'s internals, applied directly against
`assembler.{h,cpp}`. Recorded here because it invalidates several of this
document's own numbers and function names. Full description:
`docs/design.md` §16 item 25.

In short: the old byte-by-byte MSB-first synthesis
(`emitSynthesizeImm32Into`) and its `isPoolingEligible`/
`POOLING_MIN_LENGTH` cost-threshold gate are gone. `materializeImm32` now
tries three fixed shapes — direct imm8, bitwise-NOT-of-imm8, or an imm8
pattern shifted into place — before falling back to the pool, and
`materializeImm32Pooled` is folded into it via an `allowTwoIsnSeq` flag.
Every reference to `materializeImm32Pooled`/`isPoolingEligible`/
`imm32SynthCost`/`POOLING_MIN_LENGTH`/`emitSynthesizeImm32Into` earlier in
this document (§1.2, §1.4, §1.7, the Stage 1 test-coverage list, the
Verification record) describes the code as it stood at Stage 1/2
completion — accurate as history, not as the current API.

One real bug surfaced and was fixed during this rework (not by this
document's own author): the shift-trick branch was missing its `return`,
so a value that decomposed cleanly (e.g. `1000 = 125 << 3`) was
synthesized correctly *and then also* pooled — a silent double
materialization, caught by re-running the host suite (7/166 tests failing
at the time) and root-caused to the one missing statement.

**Not done, flagged instead:** `dispatch_abi.h`'s
`TRANSLATOR_ENTRY_WORST_CASE_BYTES` (488) cited
`Assembler::emitSynthesizeImm32Into`'s own stack frame as part of its
"second chain." Tracing that chain against the new code surfaced a
problem deeper than a stale function name: the call site it was anchored
on (`translate_proc.cpp`'s last-argument-fold `accState.flush`, right
before `translateBody`'s first call) never actually reaches
`materializeImm32` — `accState` is freshly constructed and every
`producer()` call reaching that point sets `Shape::ofReg(ACC_REG)`, never
a pending immediate, so `materializeShape`'s imm branch is dead code
there. The 488 figure is left in place as the last known-good number
rather than replaced with an unverified guess — `dispatch_abi.h`'s own
comment now carries this finding and what a correct re-derivation needs
to re-establish. This is a real, unresolved open item, not a documentation
nicety: getting a stack-safety budget wrong is a genuine hardware-safety
concern, not something to patch over with a plausible-looking number.

**Verification:** 166/166 host (168 minus the two deleted
`imm32SynthCost`/`isPoolingEligible` tests; every hand-derived byte/count
expectation touching a shift-trick-eligible value re-derived against
actual emitted output, not re-guessed), 9/9 QEMU, both unchanged in
outcome. `run.elf`'s `text` dropped again, 22424 → 22264 bytes — the new
scheme's 1-2-instruction shapes cover more values more cheaply than the
old byte-by-byte synthesis did.
