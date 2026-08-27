# jit-armv6m: correctness audit

An audit of `compiler/`, `runtime/` and `compiler/src/armv6.h` against
`docs/design.md` and `packages/machine/docs/isa-core.md`, with priority on
**silent wrong answers** and **hard-to-reproduce crashes**.

Nothing here is fixed. Each finding records what is wrong, the evidence, and a
recommended fix, so they can be scheduled independently.

Everything under "Confirmed defects" was reproduced by compiling the real
`compiler/src/*.cpp` against a standalone driver and reading the emitted
halfwords — see [Reproducing](#reproducing) at the bottom. Baseline at the time
of the audit: `make test-host` → 153/153 pass.

---

## The systemic issue behind most of this

Every range and validity guard in the encoder and the translator is an
`assert`. `test/host/Makefile` builds `-O0` with asserts **on**; the real target
(`test/qemu/Makefile:73`) builds `-Os -DNDEBUG`. The one build where these
conditions matter is the one build that cannot detect them: the guard vanishes
and the value is truncated into the instruction word instead.

`ArmV6M::fmtImm7`/`fmtImm8`/`fmtReg3` are plain ORs with **no masking**
(`compiler/src/armv6.h:193`, `:212`, `:162`), so an out-of-range field does not
merely truncate — it bleeds into the neighbouring register and opcode bits and
produces a *different instruction*.

Two encoder wrappers also have asserts that are wrong on their own terms (F5,
F6), so even the host build would not catch those overflows.

---

## Confirmed defects

| | Defect | Class |
|---|---|---|
| F2 | A fused comparison's boolean is never materialised on the not-taken edge | wrong answer |
| F3 | `BR_TABLE 2` can't express a total two-way branch; JIT and reference VM disagree | spec gap |
| F4 | `PEEK_PEEK` on the last argument's home slot reads an uninitialised register | wrong answer |
| F5 | Branch offsets are silently truncated; the debug assert is 2× too lax | code corruption |
| F6 | `ADD sp,#512` assembles as `SUB sp,#0` | stack corruption |
| F7 | `arenaCursor` can overshoot `arenaEnd`, giving the next procedure a ~2 GB buffer | memory corruption |

F2–F4 are reachable on programs `validateProgram` approves. F3 goes further: it
is reachable from ordinary DSL source through the reference toolchain's own
lowerer, and there the *reference* side is the one that gets the wrong answer.

> **F1 (`SUB`/`REG_REG` on an out-of-window slot compiles to `slot = 0`,
> `compiler/src/binops.cpp:70-88`) is fixed** — `addOrSubWithImm` now borrows
> `ENTRY_JUMP_REG` as the temporary whenever `n == SCRATCH_REG`, instead of
> reusing `dest` (which broke when `dest` also aliased `SCRATCH_REG`).
> `ENTRY_JUMP_REG` is never live across bytecode instructions, so it's always
> free to borrow. Verified against the full `make test` suite plus new
> regression coverage in `test/host/test_binops.cpp`.

---

### F2 — a fused comparison's boolean is never materialised on the not-taken edge

`compiler/src/translate_proc.cpp:439-467` (if-then), `:469-519` (if-else), `:573-610` (loop)

§10.1 / §16 item 4 fixed the *entering* direction: on a fused branch,
`accState` is seeded with `Imm(0)`/`Imm(1)` inside each case body. The
**leaving** direction has the identical hazard and no guard. Branch fusion never
materialises the 0/1 anywhere, so on the edge that skips the body, `r0` still
holds the pre-comparison operand.

`packages/machine/src/validate.ts:247-258` sets `accLive = combinedAccLive`
after a `BR_TABLE` and `accLive = cond.exitAccLive` after a `LOOP` — the
validator explicitly declares acc live after both constructs, so this is
reachable on validator-approved input.

**Repro A** — `argCount = 1`, `LOAD 0; EQ #7; BR_TABLE 1; BLOCK_END; RETURN`:

```
2807  CMP  r0,#7
d000  BEQ  +0        ; arg == 7 -> skip the (empty) case body
2000  MOVS r0,#0     ; case[0] only
...   return r0
```

Reference VM with `arg == 7`: `acc = 1`, `acc >= N` so no case runs, `RETURN 1`.
The JIT returns **7**.

**Repro B** — `while (r0 < 5) r0++`, then `RETURN`:

```
2f05  CMP  r7,#5
d202  BHS  exit
1c7f  ADDS r7,r7,#1
4638  MOV  r0,r7
e7fa  B    back
exit: return r0
```

Reference VM: acc after the loop is the `LT_U` result, `0`. The JIT returns **5**.

**Fix.** Two viable shapes:

- Gate the fusion on a cheap "is acc read before being redefined after this
  construct?" scan — the same shape as the existing last-argument-fold scan at
  `translate_proc.cpp:663-695` — and fall back to `materializeComparison`
  (which already produces a real 0/1) when it is. Keeps the fusion win in the
  common case and costs nothing at runtime.
- Or unconditionally give the guarded branch a two-instruction landing pad that
  materialises the constant before joining the real target. Simpler, but +4
  bytes on every fused `if`/`while`, which is meaningful against the flash cap.

The first is recommended; both need the same scan to be worth anything.

---

### F3 — `BR_TABLE 2` can't express a total two-way branch; JIT and reference VM disagree

`compiler/src/translate_proc.cpp:477-535`, `packages/machine/src/lower.ts:349-380`

> **Scope note.** This one crosses the boundary declared at the top of this
> document. The trail starts in `translate_proc.cpp` but ends in
> `packages/machine/src/lower.ts`, and the recommendation is no longer a
> JIT-side change. Left here rather than split, because the two halves only
> make sense together.

#### The structural mismatch

isa-core.md §2.1 promises a **lenient test**: "any non-zero word means
'true'/'continue', so a comparison result, a plain count or a tag drives a
branch with no normalization step". isa-core.md §4.5 defines `BR_TABLE N` as
index-*exact*: `acc < N` runs `case[acc]`, `acc ≥ N` runs no case.

Those two only coexist when the top arm absorbs the tail of the selector space:

| `N` | construct (§7.1) | where `acc ≥ N` lands | lenient? |
|---|---|---|---|
| 1 | `if` (no else) | skip — which *is* the intended false edge | yes |
| 2 | `if-else` | **neither arm** | **no** |
| >2 | `switch` | the default — genuinely wanted | yes |

`N == 1` works because §7.3's complementary-comparison idiom makes the implicit
default carry the whole `acc ≥ 1` space. `N > 2` works because an out-of-range
selector landing on a default is what `switch` wants anyway — `translateSwitch`
clamps to slot `N` deliberately and `runtime.S:133-137` implements the clamp.

`N == 2` has no absorbing arm. A two-way *total* partition is being expressed on
a primitive that isn't total. §7.1 papers over the row with "default
unreachable" — an assumption about the producer that nothing in the ISA, the
encoding or the validator enforces.

#### The reference toolchain violates it

`lowerIf` (`lower.ts:349-380`) applies `logicInvertRoot` **only** on the no-else
path. The if-else path lowers `s.test` raw and swaps the arm order (`alternate`
→ `case[0]`, `consequent` → `case[1]`). That is correct whenever `test` is a
comparison — the result is 0/1 — and broken otherwise.

`logicInvertRoot` (`lower.ts:264-288`) already handles a non-comparison test by
wrapping it in `expr == 0`, which normalizes to 0/1. The no-else path gets that
guarantee for free. The if-else path throws it away.

Repro — `u32 x = N; u32 y = 7; if (x) { y = 100; } else { y = 200; } return y;`,
lowered by `lowerProc` and run on the reference VM:

```
0: CONST #5    1: PUSH    2: CONST #7    3: PUSH
4: LOAD 0      5: BR_TABLE 2
6: CONST #200  7: STORE 1   8: BLOCK_END      ; case[0] = else
9: CONST #100 10: STORE 1  11: BLOCK_END      ; case[1] = then
12: LOAD 1    13: RETURN
```

| `x` | `validateProgram` | reference VM |
|---|---|---|
| 0 | ok | 200 ✓ |
| 1 | ok | 100 ✓ |
| 2 | ok | **7** — neither arm ran |
| 5 | ok | **7** |
| 128 | ok | **7** |

Ordinary DSL source, the official lowerer, validator-approved, silently wrong.
The else-*less* shape of the same idea
(`if (x) { return 100; } else { return 200; }`) happens to be **rejected** —
`validateProgram` reports "ran off the end without finding this block's own
close" — so the bug survives validation exactly when the construct has a
fall-through successor, which is the common shape.

Note also that §7.1's table says `if-else` → "then = `case[0]`, else =
`case[1]`", the **opposite** order to what `lower.ts` emits. §7.1's order is
only consistent with a complementary (inverted) test, i.e. the spec documents a
normalizing lowering that `lowerIf`'s else branch never implemented.

#### The JIT is the side that gets it right

`translateIfThenElse` uses `testAccNonzero` (`blocks.cpp:127-132`) on the
unfused path — `CMP r0,#0` / `BNE otherwise` — i.e. the lenient two-way test.
On the program above it runs the `then` arm and yields **100**, the correct
source semantics. The reference VM yields 7.

The original repro for this finding, `CONST 5; BR_TABLE 2; CONST 11;
BLOCK_END; CONST 22; BLOCK_END; RETURN`, still stands on its facts —
`validateProgram` ok, reference VM `5`, JIT **22**:

```
2005  MOVS r0,#5
2800  CMP  r0,#0
d101  BNE  case1      ; 5 != 0 -> case[1]
200b  MOVS r0,#11
e000  B    end
2016  MOVS r0,#22
```

What changes is the conclusion drawn from it. With the `RETURN` outside the
construct the default path is well-defined, so this is a pure engine
disagreement over an under-specified case, not a JIT miscompile.

One more thing narrows the exposure usefully: `fusesIntoBrTable`
(`translate_proc.cpp:329`) catches *every* comparison-fed `BR_TABLE 2`, and a
comparison result is provably 0/1. So the unfused `N == 2` path is entered
precisely when `acc ≥ 2` is possible. The risky case is essentially all of that
path's traffic, not an edge of it.

#### Why this can't just stay open

`fuzz/harness.cpp:11-17` already plumbs `refVmAcc`/`refVmOk`/`refVmTrapCode`
back from the oracle and marks the "execute the emitted Thumb and compare" step
as `TODO(execute)`. The moment that lands, this divergence fires on every fuzz
case with a non-boolean if-else discriminant. Whichever way it resolves, the two
engines have to agree first.

The gap also hid in plain sight in `packages/machine/test/e2e.test.ts`: every
if-*else* test uses a comparison test (`x > 3`, `:118-138`), and every truthy
`if (x)` test (`:99-117`) is else-less and so routes through `logicInvertRoot`.
The intersection — truthy discriminant *and* an else arm — is untested.

#### Resolution — open decision

Not chosen here; each option lands in a different component.

- **A — normalize in the lowerer.** `lowerIf`'s else path uses
  `logicInvertRoot(s.test)` with arms `[consequent, alternate]`, mirroring the
  no-else path and matching §7.1's documented order. Makes "default unreachable"
  true by construction. Free for a comparison test (invert the operator, swap the
  arms — same instruction count), one `EQ #0` for a truthy one. No JIT change.
- **B — fix the JIT.** 3-way test on the unfused path: `CMP r0,#1` / `BLO case0`
  / `BHI end` / fall into `case1`. One extra branch, unfused path only. Makes the
  engines agree — but leaves `if (x) A else B` wrong at the source level on
  *both* of them.
- **C — change the ISA for `N == 2`.** Define it as a lenient two-way test
  (`acc ≥ 1` → `case[1]`). No lowerer or JIT change, kills the bug at the root;
  costs a documented special case, and a 2-case `switch` loses its default arm
  (`lowerSwitch` would need reshaping).
- **D — enforce in the validator.** Track boolean-ness of `acc` and reject a
  `BR_TABLE 2` whose selector isn't provably normalized. Makes the JIT's lenient
  implementation legal by construction and retroactively invalidates this
  finding's own repro. Pairs naturally with A; costs a new dataflow property in
  `validate.ts`.

isa-core.md is deliberately left untouched for now: §7.1's arm order and
`lower.ts` disagree, and "correcting" either direction would itself pick a
resolution.

---

### F4 — `PEEK_PEEK` on the last argument's home slot reads an uninitialised register

`compiler/src/translate_proc.cpp:56-59` and `:663-695`

The last-argument fold (§6 / §10.1) leaves `physReg(argCount-1)` deliberately
unwritten while acc still carries the value, and discharges the obligation only
if a whole-body scan finds a reference. The scan's predicate is:

```cpp
static bool hasTargetField(const Instr &i)
{ return i.op == Op::LOAD || i.op == Op::STORE
      || i.combo == Combo::REG_ACC || i.combo == Combo::REG_REG; }
```

§10.1 says a register-mode operand reference in "mode 1/2/3" defers the
obligation — mode 2 is `PEEK_PEEK` and mode 3 is `POP_ACC`, neither of which
carries a target field, so neither is seen. But at `tos == argCount`,
`window.topReg()` **is** `physReg(argCount-1)`.

Repro — `argCount = 1`, `ADD.PEEK_PEEK; RETURN`:

```
19c7  ADDS r7,r0,r7     ; r7 never written by anything
```

The control case (`CONST 1; LOAD 0; RETURN`) correctly emits `4607 MOV r7,r0`
first.

Reference VM: `[tos-1] = acc + [tos-1]`, both the argument, so `2*arg`. The JIT
computes `arg + whatever the caller's shuffle left in r7`.

**Fix.** Make the scan also fail on `Combo::PEEK_PEEK`, `Combo::POP_ACC` and
`Op::POP`. Conservative (the scan doesn't track tos) and cheap — it only gives
up the fold, never miscompiles.

---

### F5 — branch offsets are silently truncated; the debug assert is 2× too lax

`compiler/src/armv6.h:71-92`, `compiler/src/assembler.cpp:101-113`,
`compiler/src/translate_proc.cpp:601`

`Ioff<a,n>`'s `minValue`/`maxValue` are **byte-range** bounds, but the
constructor checks the already-scaled value:

```cpp
Ioff(int16_t v): v((v >> a) & ~(-1 << n))
{ assert((v & ~(-1 << a)) == 0); assert(isInRange(v >> a)); }
```

so the assert admits twice the encodable range. Measured:

| requested | encoded | decodes back as |
|---|---|---|
| `Bcc` +254 | `d07f` | +254 ✓ |
| `Bcc` +256 | `d080` | **−256** |
| `Bcc` +300 | `d096` | **−212** |
| `Bcc` −258 | `d07f` | **+254** |
| `B` +2046 | `e3ff` | +2046 ✓ |
| `B` +2048 | `e400` | **−2048** |
| `B` +3000 | `e5dc` | **−1096** |

No assert fires on any of these. And nothing above the encoder checks reach
either:

- `translateLoop`'s back-edge (`translate_proc.cpp:601`) is a bare
  `emit(b(Ioff<1,11>(...)))` with no guard at all.
- `emitGuardedBranch`'s "long" form (`blocks.cpp:76-80`) is an unconditional
  `B`, itself capped at ±2046, and nothing checks that.
- `Label` chains thread each pending site through **its own encoded offset**
  (`assembler.cpp:137-141`). A chain link that doesn't fit is masked, so
  `bind()`'s walk follows a corrupted pointer and patches whatever halfword it
  lands on.

Repro — a well-formed `LOOP` with a 400-instruction body, and the equivalent
`BR_TABLE 1`, both ordinary programs:

```
LOOP, 400-instruction body:
  *** branch at +20   targets -1672  -- outside the 2430-byte procedure
  *** branch at +2422 targets +4110  -- outside the 2430-byte procedure
BR_TABLE 1, 400-instruction case:
  *** branch at +20   targets -1674  -- outside the 2428-byte procedure
```

At 200 instructions (~1.2 KB) both are clean, so the threshold is roughly 2 KB
of emitted code per block — well inside what a real procedure reaches.

**Fix** — fail loudly rather than adding codegen:

1. Correct the assert to `assert(isInRange(v))`.
2. Give `Assembler::patchBranch` a real failure path: if the delta is
   unencodable, call `Assembler::fail()` → `runtimeBail` → `RESOURCE_ERROR`.
   That is the diagnostic the design already has for arena exhaustion, and it
   costs one compare.
3. Range-check `translateLoop`'s back-edge the same way.

A genuine long-branch escape (pooled literal + `BX`) is a reasonable follow-up
if a real program turns out to need it, but is not required for correctness.

---

### F6 — `ADD sp,#512` assembles as `SUB sp,#0`

`compiler/src/armv6.h:56-69` and `:501-509`, used by `compiler/src/window.cpp:146` and `:205`

`Uoff<2,7>`'s field is 7 bits; `fmtImm7` ORs without masking, and bit 7 of the
`INCRSP` opcode is what distinguishes it from `DECRSP`. Measured:

| requested | encoded | actually means |
|---|---|---|
| `ADD sp,#508` | `b07f` | `ADD sp,#508` ✓ |
| `ADD sp,#512` | `b080` | **`SUB sp,#0`** |
| `ADD sp,#516` | `b081` | **`SUB sp,#4`** |

Reachable from `Window::discardWindow` and `restoreWindow` whenever more than
127 words are spilled (`tos > 131`), and from `argCount > 131` on the very first
`discardWindow`. `ProcSlot::MAX_ARG_COUNT` is 2047 (`runtime/runtime_internal.h:55`)
and `totalDepth` from the wire envelope is unbounded, so both are inside what
`Runtime::init` accepts. The result is a stack pointer that moves the wrong way
by the wrong amount — corruption, not a wrong value.

**Fix.** Emit the adjustment as a loop of `≤508`-byte `ADD sp,#imm`
instructions, or materialise the count into a low register and use the
hi-register `ADD sp, rN`. Either way add a real check rather than an assert.

---

### F7 — `arenaCursor` can overshoot `arenaEnd`, giving the next procedure a ~2 GB buffer

`compiler/src/assembler.cpp:31-34` vs `runtime/runtime_internal.h:247-268`

`Assembler` floors: `capacity = (arenaEnd - arenaCursor) / 2`.
`Runtime::allocate` rounds up: `arenaCursor += (need + 3) & ~3`.
`arenaCursor` is 4-aligned by `init` (`runtime_internal.h:178`) but **`arenaEnd`
is not** (`:170`), so the gap need not be a multiple of 4.

With a gap of 102 bytes and a procedure that exactly fills the buffer:

```
arenaEnd = 20000066   cursor after allocate = 20000068   <-- past arenaEnd
next Assembler capacity = (arenaEnd - cursor)/2 = 2147483647 halfwords
```

The next compilation then gets a `buf` outside the arena and a capacity that
never trips `emit`'s bounds check — unbounded writes into whatever follows.
`enterProgramSplit` takes `codeArenaBase`/`codeArenaSize` straight from the
caller with no rounding, so any caller passing a size that is odd or 2 mod 4
relative to the base is exposed. `hasRoomFor`/`reserveFor` exist for exactly
this but are only ever called from `test/host/test_runtime_arena.cpp` — the
production path doesn't use them.

**Fix.** One line in `Runtime::init`:
`arenaEnd = (codeArenaBase + codeArenaSize) & ~3u;`. That makes the gap always a
multiple of 4 and the floor-division exact.

---

## Hardening gaps

Not reproduced, but structural.

### H1 — the translator's live stack-nesting guard never runs below depth 0

`compiler/src/translate_proc.cpp:612-618`, with `// XXX stack check` markers left at `:523` and `:575`

§16 item 20 states `translateBody` "reads the real stack pointer on every
recursive call". It does not: `translateBody` is called **once**, from
`translateProc:697`. The real recursion runs
`translateLoop`/`translateIfThen`/`translateIfThenElse`/`translateSwitch` →
`processUntilTerminator` → `processNonTerminators` → back into those, never
re-entering `translateBody`. So the guard fires at depth 0 only.

This matters because `proc_scan.cpp:45-50` *does* check at every level, with a
128-byte margin and much smaller frames. `Runtime::init` therefore accepts a
nesting depth that `compileProc` then blows the stack on — and on this target
that stack holds the operand stack, the dispatch table, and (under
`enterProgramOnStack`) the arena itself. Silent corruption, not a fault. This is
the most likely source of a hard-to-reproduce crash in the whole system.

`blocks.cpp:34-67`'s `maxSpanBytes` recurses over the same nesting with no guard
at all, and is called from inside the deepest point.

**Fix.** Move the check to where the recursion actually is — the top of each
`translate*` function, or the `LOOP`/`BR_TABLE` arms of `processNonTerminators`
— and give `maxSpanBytes` an equivalent (it needs a failure path, so a
"too deep" result that forces the long-branch form and then fails).

### H2 — `CALL`'s callee index is never bounds-checked

`compiler/src/translate_proc.cpp:125`, `compiler/src/abi_strategy.cpp:62`, `runtime/runtime.S:30-32`

`r.slot(instr.calleeIndex)` reads past the dispatch table for any index
≥ `procCount`, and the same unchecked index is baked into emitted code, where
`callHelper` turns it into `slotAddr = r8 + idx*16` and `BX`es whatever word it
finds.

Related: `stackArgs > window.tos` underflows `window.tos - stackArgs` at `:134`,
which reaches `windowRuns(bottom, huge)` and writes past
`RegRun::regs[WINDOW_SIZE]` (`window.cpp:50`) — a stack buffer overflow inside
the compiler.

The trust model (`fuzz/harness.cpp:1-9`) delegates this to `validateProgram`.
Given the JIT ingests a blob injected at runtime, a single compare against
`procCount` converting an arbitrary jump into `RESOURCE_ERROR` is cheap defence
in depth.

### H3 — `decodeLeb128` has no length bound

`compiler/src/decode_instr.cpp:105-123`

No `bytesLen` parameter; the only guards are `assert(pos < UINT32_MAX)` and
`assert(shift < 32)`, both compiled out. A trailing byte with the continuation
bit set walks off the end of the program buffer, and `shift >= 32` is UB. It is
called on the very first bytes of the untrusted blob (`enter_program.cpp:44-48`),
from `Runtime::init`, from `scanProcBody`, and from every `decodeInstr`.

Related: `translate_proc.cpp:407-408` ends `processUntilTerminator` with
`assert(false); for(;;);` — under `-DNDEBUG` that is an **infinite loop** in the
translator, reached whenever the byte stream runs out mid-block.

### H4 — `procCount == 0` dispatches through `slots[1]`

`runtime/runtime_internal.h:144-147`, `runtime/runtime.S:269`

`storageBytesFor(0)` allocates only the sentinel, but `enterDispatch` still boots
with `Q_idx = 0`, i.e. `slots[1]` — one slot past the allocation. Nothing rejects
`procCount == 0`.

---

## Cheap wins against the complexity cap

- **`compile_proc.cpp:36-40`'s `calleeArgCounts` VLA is dead.** It is written and
  never read — `translateProc` takes `const Runtime&` and reads
  `r.slot(...).argCount()` directly (`translate_proc.cpp:125`). Deleting it
  removes a `procCount`-sized VLA from the tightest stack in the system, the
  loop that fills it, and lets `CALLEE_ARG_COUNTS_BYTES_PER_PROC` come out of
  `requiredStackBytes` entirely.
- **`TRANSLATOR_ENTRY_WORST_CASE_BYTES` is self-declared stale**
  (`dispatch_abi.h:66-84`), and its first term is a best case, not a worst case:
  the comment attributes 24 bytes to "`push{r0,r1,r2}` plus `REALIGN_ENTER`",
  but `runtime.S:196` pushes `{r0,r1,r2,lr}` (16) and `REALIGN_ENTER` costs
  8-12, i.e. 24-28. A hard ceiling under-reserving by up to 4 bytes.
- **`TRAP` is indistinguishable from a legitimate result with bit 31 set.**
  `translate_proc.cpp:432` encodes `0x80000000 | code` into the return value
  while `ProgramResult::trapped` stays 0. `runtime_host.h:26-28` documents this;
  §1 of `design.md` claims the opposite.
- **`fusesIntoBrTable` uses `imm <= 2`** (`translate_proc.cpp:329`), which admits
  `N == 0` and any negative `N` from a large LEB128. Those route to
  `translateSwitch`, whose `assert(hasPendingComparisonCondition == false)` leaks
  the pending condition into the next construct under `-DNDEBUG`. Should be
  `imm == 1 || imm == 2`.
- **`proc_scan.cpp:19`'s `instr.imm > 2` is a signed compare** on an
  unsigned-decoded value, and disagrees with `translate_proc.cpp:366`'s `switch`
  for `N >= 2^31`: `needsLRSave` stays false while `translateSwitch` emits a
  `BLX` that destroys the `lr`-borne call record.
- **`inWindow(tos, k)` is `tos - k <= WINDOW_SIZE`** (`window.cpp:16`), which
  admits `k == tos` — one past the legal `k < tos` (isa-core.md §8.6).
- **Three open questions left in the pool logic**: `assembler.cpp:151`
  (`// XXX flush?`), `:170` (`/// XXX Why false?`), `:282`
  (`// XXX Why endOfProcedure?`).

---

## Reproducing

The probes are standalone drivers built against the unmodified compiler
sources — no tree changes, no 1test, no Runtime beyond a hand-populated
`ProcSlot`:

```sh
cd jit-armv6m
g++ -std=c++17 -O0 -fno-exceptions -DNDEBUG \
    -I compiler/src -I runtime -o probe probe.cpp compiler/src/*.cpp
```

A driver needs three pieces of scaffolding, all of which
`fuzz/harness.cpp` and `test/host/test_translate_proc.cpp` already
demonstrate: a `longjmp`-based `runtimeBail`, a stand-in `trampolineAddr`, and a
`FakeRuntime` wrapping `alignas(8) uint8_t[sizeof(Runtime) + 2*sizeof(ProcSlot)]`
with `setStaticInfo` called on slot 0. Bodies are authored as `Instr[]` literals
and run through `encodeBody` (`compiler/src/encode_instr.h`), then handed to
`translateProc` over a detached `Assembler`.

The bodies used above:

| finding | argCount | body |
|---|---|---|
| F2 A | 1 | `LOAD(0), opImm(Op::EQ, 7), brTable(1), bare(Op::BLOCK_END), bare(Op::RETURN)` |
| F2 B | 1 | `LOOP, LOAD(0), opImm(Op::LT_U, 5), BLOCK_END, LOAD(0), opImm(Op::ADD, 1), STORE(0), BLOCK_END, RETURN` |
| F3 | 0 | `CONST(5), brTable(2), CONST(11), BLOCK_END, CONST(22), BLOCK_END, RETURN` |
| F4 | 1 | `opStack(Op::ADD, Combo::PEEK_PEEK), bare(Op::RETURN)` |
| F5 | 1 | `LOOP, CONST(1), BLOCK_END, bare(Op::CLZ) × 400, BLOCK_END, RETURN` |

F5's check is a post-pass over the emitted halfwords: decode every `B`/`Bcc` via
`getBranchOffset`/`getCondBranchOffset` plus `signExtend`, and flag any whose
target falls outside `[0, halfwordCount*2)`. F6 and F7 need no translation at
all — F6 is a direct call to `ArmV6M::incrSp(Uoff<2,7>(n))`, F7 is
`Assembler`'s and `Runtime::allocate`'s arithmetic replayed on plain integers.

Building the same probes **without** `-DNDEBUG` shows the assert-only
protection: F6 aborts at `armv6.h:67`, while F5 and F7 still pass silently.

F3's *reference-side* repro needs no probe at all — it runs against
`packages/machine` directly, `lowerProc` the DSL source, then `validateProgram`
and `run` the result. The selector sweep and the "§7.1 vs `lower.ts` arm order"
observation both come from there, not from the emitted halfwords.

---

## If these get fixed

F2 and F4 each deserve a host regression test asserting the emitted halfwords
(the pattern `test_translate_proc.cpp` already uses), plus a `test/qemu` fixture
comparing against the reference result where the value is what's wrong. F5-F7
are better served by a bounds test on the encoder wrappers and one arena test
with a deliberately mis-aligned `codeArenaSize`.

F3 is different: what the JIT side should assert depends on which resolution is
picked, so a halfword test written now would just pin down whichever behaviour
happens to be there. The part that is worth adding regardless is on the
*reference* side — `packages/machine/test/e2e.test.ts` has no truthy-discriminant
if-else case at all (`if (x) ... else ...` over `x ∈ {0, 1, 2, 5, 0x80}`), and
that gap is what let the lowering bug through.

Worth considering separately: build `test/host` a second time with `-DNDEBUG`.
Every finding above except F2/F3/F4 is invisible to the current suite purely
because asserts are on there and off on the target.
