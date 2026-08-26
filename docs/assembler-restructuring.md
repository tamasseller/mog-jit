# Assembler restructuring — plan and status

## Status

**Stage 1: done, verified green.** 168/168 host tests, 9/9 QEMU tests.
**Stage 2: not started.**

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
the design record and as the Stage 2 task list.

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

Flagged, still not in scope: `Runtime::arenaBase` is write-only;
`enterProgram` never checks `arenaSize` against `ARENA_CAPACITY`;
`runtime_host.h`'s header comment overstates what it shares and what
bytecode `TRAP` propagates; `design.md`'s sentinel-offset prose is stale
(`ProcSlot` grew from 8 to 16 bytes).

---

## Stage 2 — layer/file reorganization (not started)

Purely structural; do only once Stage 1 stays green under real use.

- Split `runtime_host.cpp` into layer 1 (`enter_program.cpp`: the three
  entry points, `parseProgramHeader`, `requiredStackBytes`,
  `stackHasRoom`) and layer 2 (`dispatch_abi.{h,cpp}`: `helperVec`,
  `trampolineAddr`, `runtimeBail`, the fixed-cost constants). Collapse
  the threefold duplication of `parseProgramHeader` + `totalDepth * 4` +
  the `runtimeStorage` VLA + `enterProgramCore` across the three
  `enterProgram*` variants into one helper.
- `compile_proc_real.cpp` → `compile_proc.cpp` (the "real" qualifier
  distinguished it from a mock retired per `design.md` §16 item 22).
- Move `abi_strategy.{h,cpp}`'s helper-vector indices and record packing
  next to the layer-2 conventions they mirror, leaving the emission
  functions in `compiler/src`.
- Resolve the `RUNTIME_DISPATCH_TABLE_OFFSET`/`DISPATCH_SENTINEL_OFFSET`
  split — the constants live in the public header (for `runtime.S`'s
  `__ASSEMBLER__` include) but their `static_assert`s live in the
  internal one.
- `docs/design.md`: §10.2 (no fixup pass and no fixpoint), §11 (rewrite
  the `ArenaRoom` "Done" paragraph — now genuinely done, differently),
  §9, §2's `requiredStackBytes` table, plus a new §16 item recording this
  restructuring and the two latent bugs it fixed.
- Clean up the still-open doc-drift items noted under 1.9 above.

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
(64 bytes).
