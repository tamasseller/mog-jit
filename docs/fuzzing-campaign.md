# Differential fuzzing campaign — findings

Record of one campaign against `jit-armv6m/fuzz`, for review. The permanent
versions of the conclusions live in `design.md` §17 and `target-profile.md`;
this file is the working detail behind them — what was run, what broke, and
which side of each disagreement was wrong.

**Nine findings, all fixed.** §1 (`TRAP` does not unwind) was the one left
open at the end of the campaign, as a deliberate decision rather than an
oversight — it needed new runtime asm and an ABI contract change. It has
since been fixed too; that section records both the finding and the fix.
Five of the nine were invisible to the pre-existing harness by construction:
it never executed the code it emitted.

§2 was subsequently re-decided on review — the fix moved from the translator
to the validator — and its section records both the reasoning and the wrong
justification the first pass gave.

## Scale

| | |
|---|---|
| Crash campaign, final run | ~4.3M executions (720k × 6 workers), 94k validator-approved per worker, **0 crashes** |
| Execution sweep, final run | 12,000 inputs → 11,084 runnable → 10,287 compared on emulated ARM, **0 mismatches, 0 hangs** |
| Throughput, before → after | 170 → ~3,000 exec/s |
| Test suites | all green: `machine` 443/443, `core` 103/103, `codecs` 126/126, `target-js` 79/79, `target-cpp` 18/18, `example` 33/33, `test/host` 174/174 (clean build), `test/qemu` 14/14, `stack-usage-check` clean |
| Codecs suite, over the campaign | 11 failures at the campaign's starting commit → 0. Five cleared here (§2's missing producers, §9's `writesAcc`, §4.6's bare-`RETURN` corollary), the rest alongside |
| Re-verification after §2's re-decision | 2.76M executions, 359k validator-approved, **0 crashes**; 32 seeds (29 comparable) and a 3,000-program corpus sample (2,884 comparable) on emulated ARM, **0 mismatches, 0 hangs** |
| Re-verification after §1's fix | 2.72M executions, 336k validator-approved, **0 crashes**; 34 seeds (32 comparable, up from 30 — the ambiguity skip is gone), a 3,000-program sample, and a 400-program trap-only corpus filtered out of 76k approved programs (6 nested traps, 394 entry traps, **all 400 compared and matched**) |

## Findings at a glance

| # | Finding | Class | Wrong side | Status |
|---|---|---|---|---|
| 1 | `TRAP` does not unwind; a nested trap becomes a return value | miscompilation | jit-armv6m ABI | fixed (helper slot 8) |
| 2 | `BR_TABLE` → `RETURN` asserts on a poisoned `acc` | crash | **validator** (fixed on the translator side first — see §2) | fixed |
| 3 | `LOOP` condition's `BLOCK_END` not checked for `acc` liveness | crash | validator | fixed |
| 4 | `CALL` flushes `acc` even for a zero-argument callee | crash | translator | fixed |
| 5 | Register-form shift amount not masked to 5 bits | miscompilation | ISA spec | fixed (§4.1 narrowed) |
| 6 | Long-form guarded branch falls through into the literal pool | miscompilation | translator | fixed |
| 7 | `BR_TABLE 2` folds the implicit default into `case[1]` | miscompilation | translator | fixed |
| 8 | `LOOP` condition block's TOS surplus never dropped | miscompilation | translator | fixed |
| 9 | Callee entry `acc`: seeded to 0, and live when nothing sets it | miscompilation | `@ppl/machine` | fixed |

---

## 1. `TRAP` does not unwind — fixed by a new helper-vector slot

**Symptom.** Wrong answer, no crash, no bail. `translate_proc.cpp`'s
`handleGlobalJump` compiled `TRAP #code` to

```cpp
a.materializeImm32(ACC_REG, 0x80000000u | (uint32_t)term.imm);
returnSequence(ctx, a);            // an ordinary return
```

Correct for the **entry** procedure: that word *is* `ProgramResult.value`,
and `runtime_host.h` documented the high bit as this slice's trap sentinel.
Wrong for any **nested** procedure — the callee returned normally, the
sentinel landed in the caller's `acc` as an ordinary return value, and the
caller kept executing.

**Repro.** Minimized from a 195-instruction fuzz input to four
instructions:

```
proc 0 (argCount 0):  CALL 1 ; CONST 92 ; RETURN
proc 1 (argCount 0):  TRAP 754
```

Reference VM traps with 754 — a bytecode trap unwinds the whole program
there, and `ProgramResult`/isa-core §9 model it the same way. Emitted code
returned 92. Disassembled (`fuzz/dump_code.sh`):

```
proc 1:  ldr r0, [pc,#4]      ; r0 = 0x800002f2
         mov r3, sl
         ldr r3, [r3, #4]     ; helper slot 1 = returnHelperFromLr
         bx  r3               ; ← plain return
proc 0:  movs r0, #92         ; ← straight over the sentinel
```

**Why it was left open at the end of the campaign.** Not a codegen slip: a
real unwind needs a route out of compiled code that isn't a return, and
compiled code cannot `BL` an arbitrary address — every fixed routine it
reaches comes through the `r10` helper vector. So the fix meant new
hand-written asm in `runtime.S`, a new reserved vector slot, a new fixed
constant, and a change to the dispatch ABI's contract about what may leave
a compiled procedure. Deliberately not folded into a fuzzing pass.

**Fix.** Helper slot 8, `trapHelper` — six instructions:

```asm
trapHelper:                 @ in: r0 = trap code, r9 = runtime
    mov   r1, r9
    ldr   r3, [r1, #24]     @ slots[0].codePtr — the sentinel landing
    ldr   r1, [r1, #0]      @ runtime->savedSp
    movs  r2, #LANDING_TRAP
    mov   sp, r1
    bx    r3
```

The mechanism already existed, twice over, and neither half was reachable
from emitted code: `dispatch_abi.cpp`'s `runtimeBail` does exactly this
from C++ for `RESOURCE_ERROR`, and `enterDispatch` already parks a sentinel
landing below the dispatch table and already reads a tag out of `r2` at it.
What was missing was a door onto that path from the compiled side.

Three consequences fell out, each simplifying something:

- **A trap needs no teardown at all.** One `mov sp, savedSp` discards every
  window spill, every pushed call record and every out-of-window argument
  block across every frame between the trap and the entry procedure. So
  `handleGlobalJump`'s `TRAP` path emits no `discardWindow`, no record
  retrieval and no reclaim — a trap is now *cheaper* to emit than a return
  (`TrapAtTopLevel` went from 12 emitted halfwords to 10, and that with the
  pooled literal gone).
- **The high-bit sentinel is retired.** `ProgramResult.trapped` carries one
  of three `LANDING_*` tags instead, so nothing is encoded in `value`: a
  program may return any `uint32_t` and trap with any code without the two
  aliasing. `runtimeBail` took a distinct tag
  (`LANDING_RESOURCE_ERROR`), since "the program chose to stop" and "this
  implementation ran out of room" are different answers to the caller. The
  same argument one level down later split that tag's own `value` into the
  `RESOURCE_*` codes (design.md §12): "out of memory", "this program is
  malformed" and "no arena size would help" are three more.
- **`fuzz/qemu_exec` lost two skip categories** — the `trapDepth > 0`
  set-aside and the "result ambiguous under the high-bit trap encoding"
  one. On the seed corpus that moved 30 comparable programs to 32 of 34,
  and the last sweep before this change had set aside 65 of 2,951 corpus
  programs as ambiguous.

One incidental defect fixed on the way: `runtime_host.h` had no include
guard, only an `#ifndef __ASSEMBLER__`. That held while exactly one
translation unit included it and broke the moment a second did.

**Coverage.** `test/qemu` fixtures 38-40 — the nested repro above, a trap
in the entry procedure with five live pushed locals, and a trap two levels
down out of a frame whose out-of-window arguments sit *below* its pushed
call record (the `returnHelperFromStackReclaim` shape, whose teardown the
trap skips entirely). All three assert the tag exactly, which is why
`Fixture::expectTrapped` became `expectLanding`, a `uint32_t`. Plus fuzz
seeds `nested_trap` / `deep_nested_trap`, and host test
`TrapInsideCaseClosesItAndContinuesToNextCase` now expecting
`TRAP_VIA_HELPER` where it expected `RETURN_VIA_LR`.

The `test/qemu` image also had to grow: `rom` went from `0x8000` to
`0xA000`, which is the flash this QEMU machine model actually decodes —
measured via `-device loader` (a write past `0xA000` is silently dropped),
not the 64KB the linker script's own comment had assumed.

## 2. `BR_TABLE` → `RETURN` asserts on a poisoned `acc`

**Symptom.** `accstate.cpp:11`, `assert(kind != Kind::Poisoned)` in
`AccState::peek()`, on a validator-approved program. In an `NDEBUG` build
the assert is gone and `peek()` returns `Shape::ofReg(reg)` with a stale
`reg` — a silent wrong value rather than a crash.

**Repro.** 5 bytes: `argCount 1; BR_TABLE 1 { CONST 0 } ; RETURN`.

**Cause.** `afa2a6d` introduced §8.7's acc-clobbering rule on both sides at
once, but inconsistently. In `validate.ts` the BR_TABLE *merge* stayed an
AND across the cases (`let combinedAccLive = true`, the implicit default
contributing nothing), while `translate_proc.cpp` poisoned unconditionally
at every merge. So a program the validator approved reached `peek()` on a
poisoned `acc`. The same commit switched `LOOP`'s exit from
`accLive = cond.exitAccLive` to `accLive = false` — the strict rule —
which is the tell: `BR_TABLE`'s merge simply did not get updated with it.

**Which side — and a correction.** The translator, on the first pass; the
**validator**, on review, and that is how it now stands. The original
write-up justified the translator fix with "tightening the validator was
tried first and rejected 11 `packages/codecs` tests". That was a
misattribution: 11 was the pre-existing failure count at HEAD. Re-measured,
tightening the merge to `accLive = false` changes **no** codecs test outcome
(116/126 either way, identical failing set down to the subtest level).

What actually depended on the lax reading, found by tainting `acc` that is
live only via a default edge: two programs, both the union codec in
`list-union.test.ts`, and both inside tests already failing for unrelated
reasons — which is why the counts didn't move.

The decisive evidence was in `isa-rationale.md:51-67`, added by the same
`afa2a6d`, which names this exact pattern as the thing the rule exists to
make invalid:

> It exists to close a JIT-backend bug (jit-armv6m's comparison-fusion
> optimization defers a comparison's boolean to a CPU condition code and
> **never materializes it on the edge that skips the branch body**) by
> construction: rather than teach every backend to correctly compile a
> pattern nothing legitimate uses, the pattern is simply not valid input.

That is the `BR_TABLE 1` skip edge, and the first fix did precisely what the
rationale rules out — taught the backend to materialize it.

**The structural reason the rule has to be unconditional.** `BR_TABLE`'s
implicit default is the only CFG edge in this ISA with *no instruction
slots*: every other edge (a case body, a loop condition, a loop body) is a
block you can append to, while the default is pure fall-through from the
dispatch. So "join requires a flush on every incoming edge" is not merely
awkward there, it is unimplementable — and the lax reading does not rescue a
value-producing branch either, since on the default edge `acc` holds the
*dispatch* value, not the selected arm's. Liveness was never the obstacle;
reachability was.

A rule conditioned on "the default is provably unreachable" was considered
and rejected as an interface: it makes the spec mean "whatever range
analysis this validator implements", so a second conforming implementation
could not be written from it. Exhaustiveness would have to be *declared* by
the opcode instead — a §5.3 candidate priced in `isa-rationale.md`, not
spent on a ternary that is parsed but not yet lowered.

**Fix.**

- `validate.ts`: the `BR_TABLE` merge is `accLive = false`, with no case's
  exit liveness consulted. `vm.ts` matches, on both the default path and a
  case's `BLOCK_END`, so the runtime assertion and the validator agree.
- `isa-core.md` §8.7: the two merge bullets replaced by one unconditional
  rule plus the reason, and the TOS-slot idiom a ternary would use.
  §7.1 gained a note that "default unreachable" for `if-else` is what the
  lowerer emits, not something validation establishes.
- `isa-rationale.md`: the closing "nothing here forecloses a real value
  crossing a branch later" corrected — it does foreclose the acc-carrying
  form, for the no-instruction-slots reason.
- `translate_proc.cpp`: `translateIfThen`'s skip-edge fixup stub removed,
  `translateIfThenElse`/`translateSwitch`'s `mergeLive` accumulation
  removed, all three back to `poison()`. `AccState::live()` removed.
- Real programs that relied on the lax reading, all needing one producer
  after the merge: the union codec test helper, `docs/codec-extension.md`
  §8.2's union-encoder example, and `test/corpus_programs.h`'s fixtures 30
  and 31, which now deliver each case's value through a TOS slot — the
  idiom §8.7 now documents. Fixture 30's `unitCodecBody` counterpart in
  `list-union.test.ts` also needed `CONST` before its bare `RETURN` (§4.6,
  finding 9b); with both, the whole `list-union` suite went from 4 failures
  at HEAD to green.

**Coverage.** Four validator-rejection fixtures in `validate.test.ts`
(the `N = 2` merge, the `BR_TABLE 1` skip edge — the fuzz repro verbatim,
its fused-comparison variant, and a switch). Seeds renamed to
`br_table_dead_merge` / `if_then_dead_merge` / `fused_if_then_dead_merge`
and made valid by adding the producer: the shape is still worth seeding, it
is just no longer the merge value that is interesting about it.

## 3. `LOOP` condition's `BLOCK_END` not checked for `acc` liveness

**Symptom.** Same assert, reached via `testAccNonzero`.

**Repro.** `LOOP { … AND REG_REG r1 } BLOCK_END …` — a write-back-in-place
combo as the last thing in the condition sub-block.

**Cause.** That `BLOCK_END` *is* the loop's continue/exit dispatch, and §8.7
counts a dispatch as a read of `acc`. `validate.ts`'s `LOOP` case never
checked the condition sub-block's own `exitAccLive`, so a `REG_REG`-poisoned
condition block validated fine.

**Which side.** The validator.

**Fix.** `validate.ts` checks `cond.exitAccLive` on the external entry, and
on the back-edge probe walk as well.

## 4. `CALL` flushes `acc` even for a zero-argument callee

**Symptom.** Same assert, at the `CALL` case's `accState.flush(a, ACC_REG)`.

**Repro (as minimized then).** `PUSH×5; XOR PEEK_PEEK; CALL 1; …` with
`proc 1 = {argCount 0, body [RETURN]}`.

**Cause.** The `CALL` case flushed `acc` unconditionally. §8.7 lists "a `CALL`
whose callee takes at least one argument" among the reads of `acc`, not every
`CALL` — §4.6 puts the last argument there only when there is one.

**Fix.** Flush only when `calleeArgCount > 0`; otherwise `poison()`, since
`CALL` clobbers `acc` regardless and the `producer()` after it re-establishes
it from the return value.

**Note on the repro.** Finding 9 later made `{argCount 0, body [RETURN]}`
invalid, so that exact input no longer validates. The fix stands on its own —
any poisoned `acc` before a call to a zero-argument callee reaches it.

## 5. Register-form shift amount not masked to 5 bits

**Symptom.** Wrong answer, no crash and no bail.

**Repro.** 4 instructions: `CONST 2784; PUSH; ASR POP_ACC; RETURN`.
Reference `0xae0` (2784), emitted `0`.

**Cause.** isa-core masked a shift amount to five bits. `binops.cpp`'s
`IMM_ACC` path did that at compile time (`rhs.imm & 31`), but the register
path emitted a bare `LSLS/LSRS/ASRS Rd, Rm` — and ARMv6-M's register form
reads `Rm[7:0]`, not `Rm[4:0]`. 2784 is 87×32: low five bits are zero, so the
ISA said shift-by-0 (identity), while the hardware shifts by 224 and yields 0
(or all sign bits, for `ASR`).

Measured on the emulated target, `0xdeadbeef` shifted by a register amount:

| amount | `lsls` | `lsrs` | `asrs` | ISA said (`n & 31`) |
|---|---|---|---|---|
| 0 | `deadbeef` | `deadbeef` | `deadbeef` | identity ✅ |
| 31 | `80000000` | `00000001` | `ffffffff` | ✅ |
| 32 | `00000000` | `00000000` | `ffffffff` | identity ❌ |
| 224 | `00000000` | `00000000` | `ffffffff` | identity ❌ |
| 256 | `deadbeef` | `deadbeef` | `deadbeef` | identity ✅ (`Rm[7:0]` is 0) |
| 2784 | `00000000` | `00000000` | `ffffffff` | identity ❌ |

Note the zero row: a register-form shift by zero is already the identity.
The *shift-by-zero* ARM quirk is a different one — `LSR`/`ASR`'s
**immediate** encoding, where `imm5 == 0` means shift-by-32 — and
`binops.cpp` has always handled that with a `MOVS`, at no extra
instruction. Nothing here was ever paying for a zero amount.

**Fix — in the ISA, not the translator.** Masking it in codegen costs two
extra instructions on every *dynamic* shift: a masked copy of the amount
via `LSLS #27` / `LSRS #27`, in a scratch register distinct from both the
value and the raw amount. Not an `AND` against 31, which is worse — ARMv6-M
has no AND-with-immediate, so it needs a third register just to hold the
constant. For reference, that is also what GCC emits for a C
`a << (b & 31)` on `-mcpu=cortex-m0`: `movs r3,#31; ands r3,r1; lsls
r0,r0,r3`, three instructions. On Thumb-2 the same source is one
(`and r1,r1,#31`); on x86, AArch64 and RISC-V the hardware masks for free.
ARMv6-M's Thumb-1 register form is the outlier, and the whole cost is its
missing AND-immediate.

So the masked result was never worth buying. isa-core.md §4.1 now defines
`SHL`/`SHR`/`ASR` for amounts `0..31` only; outside that the result is an
**unspecified value** — some 32-bit value, no trap, no other state touched,
and explicitly *not* C-style undefined behavior licensing a projection to
reason backwards about what the amount could have been. GCC's own escape is
the same one, one level down: C makes an out-of-range shift undefined, so
GCC emits a bare `lsls r0, r0, r1` and owes nobody the masked answer.

What that costs, honestly: a producer bug emitting an unbounded amount now
diverges silently between projections instead of being caught. The
immediate combo — where essentially every real shift amount lives — is
covered by making it a validator error, so only dynamic amounts are
exposed.

Consequences:

- `binops.cpp` back to a bare `LSLS/LSRS/ASRS Rd, Rm`; `maskScratchFor`
  deleted. Its `IMM_ACC` mask stays: free at compile time, and an unmasked
  count of 32+ would be UB for the translator's own C++ `<<`.
- `validate.ts` rejects `SHL`/`SHR`/`ASR` in the immediate combo with an
  amount outside `0..31` (`SHIFT_OPS` in `rtl.ts`).
- `vm.ts` throws `UnspecifiedShiftAmount` rather than pass its own
  five-bit masking off as the answer. Its own type, not a `Trap`: no
  projection raises this, and a trap is something every projection must
  agree on.
- `oracle_server.ts` reports it as stage 7 — a legal program with no
  reference result, like the step-limit's stage 6. Emphatically not stage
  4, which the harness aborts on.
- `qemu_exec.ts` skips those programs: run, but not compared.

**Coverage.** The three register-form shifts rejoined
`TwoOpInPlaceNativeCoversEveryOpcode` in `test_binops.cpp` — one
instruction each again — and `RegisterShiftMasksTheAmountToFiveBits` is
gone. Seed `register_shift_masked_amount` became
`register_shift_dynamic_amount`, same register-form path with an in-range
amount so it stays comparable (`0x3C0 ASR 5 == 30`; distinct operands, so
a shift emitted with its operands reversed shows up as a wrong answer).

## 6. Long-form guarded branch falls through into the literal pool

**Symptom.** The emitted code never finished — a hang, surfaced as a QEMU
timeout the sweep could attribute to one program.

**Repro.** Minimized 179 → 21 instructions. The load-bearing shape is a wide
immediate (parks a pool entry) plus a comparison, then a `BR_TABLE` whose case
body is long enough to force the long branch form.

**Cause, from the disassembly:**

```
  10:  cmp  r0, #0
  12:  bcs.n 0x16          ← "skip the long branch" = skip + 4
  14:  b.n  0x34           ← unconditional branch; flushes the pool right after
  16:  nop                 ← pool alignment pad
  18:  d156 0019           ← the literal 0x0019d156, executed as instructions
  1c:  movs r6, #0         ← where the case body actually starts
```

`emitGuardedBranch`'s long form patched its not-taken edge to `skip + 4`.
`Assembler::branchTo(Label&)` (the unconditional overload) flushes the pool
no-guard immediately after itself — documented as safe because nothing falls
through an unconditional branch. This edge falls exactly there. At 0x18 the
pool word decodes as `bne 0xc8`, taken, into nothing.

**Fix.** Route both branches through `Label`/`bind()`:

```cpp
Label fallThrough;
a.branchTo(fallThrough, ArmV6M::inverse(condition));
a.branchTo(label);
a.bind(fallThrough);
```

`bind()` is the one flush-safe way to resolve a fixup to "wherever we are
now", which makes this correct by construction rather than by arithmetic that
has to stay in step with the pool.

**Coverage.** `EmitGuardedBranchLongFormSkipsOverAPoolFlush` in
`test_blocks.cpp` (asserts the target equals `e.pc()` after the flush, and
that there really was a pool to skip); seed `long_branch_over_pool`.

## 7. `BR_TABLE 2` folds the implicit default into `case[1]`

**Symptom.** Wrong answer. Four separate corpus programs, one cause.

**Repro.** 14 instructions: `CONST 131118; BR_TABLE 2 {…}{…}; RETURN`.
Reference `RETURN 0x2002e` — 131118 untouched, because `acc ≥ N` runs no case
and nothing overwrote it. Emitted code ran `case[1]`.

**Cause.** The unfused path used `testAccNonzero`, i.e. a zero/non-zero test,
sending every `acc ≥ 2` to `case[1]`. isa-core §4.5 is unconditional: `acc ≥ N`
executes no case. §7.1's "default unreachable" for `if-else` describes what
the DSL lowerer emits, not a licence for the backend to assume it.

**Fix.** The fused path is unchanged and stays a single branch — a fused
comparison's result is 0 or 1 by construction, so its default really is
unreachable. The unfused path gets three ways out:

```
materialize acc into ACC_REG
cmp ACC_REG, #1
BHI  → end          (acc > 1: the implicit default, past both arms)
cmp ACC_REG, #1
BEQ  → otherwise    (acc == 1: case[1]; acc == 0 falls through to case[0])
```

The second `CMP` is re-emitted rather than reusing the first one's flags:
`emitGuardedBranch` may take its long form, whose `bind()` can flush the pool
in between. Nothing a flush emits writes flags today, and one `CMP` is a cheap
way not to depend on that. The dispatch value goes to `ACC_REG` simply
because it has to be in a register to be compared and that is where `flush()`
would put it — under §2's rule nothing downstream may read it there.

Independent of finding 2, and unaffected by it: this is about which *code*
runs, not about where a value lives. Keeping the fused path on a single
branch is a backend-local optimization, legal because eliding a branch to an
arm that can never be selected changes nothing observable — booleanness stays
the backend's business, never validation's.

A simplification fell out: both paths now reach `case[0]` only with `acc == 0`
and `case[1]` only with `acc == 1`, so each arm's entry value is a
compile-time constant and the fused/unfused special-casing of it is gone.

**Coverage.** Seeds `br_table2_default` and `br_table1_default` — the `N == 1`
form was correct all along (`acc != 0` and `acc >= 1` coincide there), pinned
precisely because it does. Both had to be rewritten under §2's rule: "which
arm ran" is only observable through state a case *writes*, so each case
`STORE`s a witness to a slot the code after the merge `LOAD`s, and the
untouched pre-dispatch value is what proves neither arm ran.

## 8. `LOOP` condition block's TOS surplus never dropped

**Symptom.** A hang.

**Repro.** 10 instructions:

```
argCount 1
PUSH ; PUSH ; PUSH
LOOP
  LOAD r0 ; PUSH
BLOCK_END              ← condition sub-block's own close
BLOCK_END              ← empty loop body
CONST 11908 ; RETURN
```

**Cause.** isa-core §8.1 has every `BLOCK_END` implicitly drop any TOS
surplus above its own block's entry depth. Every `BR_TABLE` case did this via
`localJumpCleanup`; `translateLoop` never did it for the condition sub-block.
A `PUSH` there — exactly as legal as one inside a case — left the surplus in
place on **both** edges out of the test, so the window model and the real `sp`
disagreed from the loop onward, and the procedure's return sequence reclaimed
the wrong amount and returned through a corrupted call record. Three slots
before the loop is enough that the extra push spills past the window and
becomes a real stack push.

**Fix.** After the condition sub-block closes and before the guarded branch:

```cpp
if(ctx.window.tos != entryTos)
{
    ctx.accState.flushLive(a, ACC_REG);
    restoreWindow(a, ctx.window, entryTos);
}
```

Before the branch so both edges see the same `sp`. Flush first because
`restoreWindow` pops window registers and the condition value may still be
living in one. Neither `POP` nor `ADD SP, SP, #imm` writes flags on ARMv6-M,
so a fused comparison's `CMP` still governs the branch.

**Coverage.** Seed `push_in_loop_condition`.

## 9. Callee entry `acc` — two reference-side bugs

The only findings where the translator was right and `@ppl/machine` was
wrong.

### 9a. `runProc` seeded a callee's `acc` to 0

**Repro.** `seeds/call_one_arg`: `proc 0 = CONST 3; CALL 1; RETURN`,
`proc 1 (argCount 1) = MUL REG_ACC r0; RETURN`. Emitted code gave 9 (3×3), the
VM gave 0.

**Cause.** §4.6: the callee's last argument arrives in `acc` as well as in
frame slot `r(N-1)` — the caller left it sitting there rather than pushing it,
which is the whole point of the convention. `runProc` did `let acc = 0`, so
every callee reading `acc` before writing it computed with 0.
`translateProc`'s entry flush into `physReg(argCount - 1)` exists precisely
because the value is already there.

**Fix.** `let acc = args.length > 0 ? args[args.length - 1]! : 0`. This one
change accounted for all three of the first sweep's mismatches (`call_chain`,
`call_one_arg`, `wide_call_chain`).

**Coverage.** Seed `callee_reads_incoming_acc`.

### 9b. Entry `acc` treated as live for a zero-argument procedure

**Repro.** 4 instructions: `proc 0 = LE_S IMM_ACC 107; CALL 1; RETURN`,
`proc 1 (argCount 0) = RETURN`. VM returned 0; emitted code returned the
caller's leftover `acc` (1).

**Cause.** `validate.ts` called `walk(0, proc.argCount, true)` — entry `acc`
hardcoded live for every procedure. Three places say otherwise: §4.6
conditions the incoming `acc` value on `N ≥ 1`; `isa-rationale.md` says
"nothing a caller left there survives the call"; `matcher.ts` notes a
zero-argument call has no last argument. So nothing establishes it, two
conforming implementations disagreed on the value, and a validator whose job
is "a validated program has one meaning" must reject the read.

**Fix.** `walk(0, proc.argCount, proc.argCount >= 1)`. Consequence worth
knowing: `{argCount: 0, body: [RETURN]}` is now a validation error — a
procedure that returns without computing anything has no defined return
value. The trivial procedure is `[CONST #x, RETURN]`. Stated explicitly in
isa-core §4.6 and §8.7.

**Cascade — 12 `machine` fixtures.** Each used `argCount: 0` with a leading
`acc` read (`PUSH`, a bare `RETURN`, a `BR_TABLE`) incidentally, while testing
depth math, dead-code rejection or block well-formedness. Fixed by prepending
`CONST(0)`, which establishes `acc` at no TOS cost, so every depth figure
those tests assert is unchanged.

**Cascade — a missing capability.** One fixture couldn't be expressed at all:
its extension's `exec` is `state.acc = state.callProc(...)`, but
`ExtOpEffect` had no way to declare that an op *writes* `acc`, and
`validate.ts` passed liveness through `EXT` ops unchanged. Added
`ExtOpEffect.writesAcc` as `readsAcc`'s counterpart, and wired both into
`validate.ts` (`readsAcc` existed for `raise.ts` but the validator never
checked it). Declared `writesAcc` on the seven `@ppl/codecs` opcodes whose
`exec` assigns `state.acc`: `LOAD_VAL`, `COUNT`, `TAG`, `READ`, `HAS_NEXT`,
`CALL_CODEC`, `CALL_CODEC_NEXT`. Without that, real codec procedures opening
with `READ; WRITE` were rejected. `packages/codecs` went 41 failures mid-change
→ 10, one *fewer* than its 11-failure baseline at HEAD.

---

## Harness bugs — the fuzzer was lying about its own coverage

Worth listing separately: several of these meant the pre-existing harness was
reporting success over work it wasn't doing.

| Problem | Effect | Fix |
|---|---|---|
| `srand(time(nullptr))` | every worker started in the same second explored one identical sequence; 8 workers "found" one identical crash 8 times | XOR in the pid |
| Oracle socket opened per test case | 170 exec/s | one connection per run → ~3,000 exec/s |
| No corpus feedback | mutants were rejected by the validator and discarded; the loop mutated five fixed seeds forever | retain validator-approved inputs (the only signal available without coverage instrumentation) |
| Oracle stage 4 discarded | "the reference VM threw on a validator-approved program" — the oracle's own documented genuine-bug signal — arrived with `valid == 0` and hit the early return | surfaced and aborted on; every worker hit it within seconds |
| `writeInt32LE(trapCode)` | throws for codes ≥ 2³¹, and the oracle's own `catch` relabelled that as stage 4 | `| 0` coercion; same four bytes |
| `MAX_STEPS` watchdog reported as stage 4 | a legal non-terminating program (§9) looked like a validator/VM inconsistency | typed `StepLimitExceeded`, its own stage 6 |
| Seed `loop` never validated | silently discarded on every execution since it was written | `make_seeds.ts` runs `validateProgram` on every seed before writing it |
| `dump_seeds.cpp` wrote the legacy single-procedure format into `seeds/` | would plant seeds the harness discards 100% of the time | stages into `seeds_raw/`; `make_seeds.ts` owns `seeds/` alone |
| `return` inside a top-level `for` in `qemu_exec.ts` | exits the whole CommonJS module — silently ended an 808-batch sweep at its first hang, losing three already-found mismatches | `continue`; mismatches also print as found, not only in the summary |
| Program copied into a 1 KB RAM buffer | capped the execution oracle at ~21% of the corpus, excluding every large size-guard program | run in place from flash — the translator only reads the body |

## Coverage expansion

**Whole-program envelope.** The harness fuzzed one procedure, so isa-core
§8.2's call-graph acyclicity made `CALL` unreachable by construction — the
`CALL` case, `abi_strategy.cpp`'s argument shuffle and `Runtime::init`'s
multi-procedure directory walk were all permanently dead to the fuzzer. Input
is now byte-for-byte `encodeJitProgram`'s output, and every procedure is
translated, since a call site reads the *callee's* slot.

**Attached-`Assembler` pass.** Every input is translated a second time
against a real arena at a fixed low address (`mmap`, fatal if unavailable —
a silently skipped pass is worse than one that doesn't start), reaching
`Runtime::allocate`/`findEvictionVictim`/`evict`'s compaction memmove and
`finalize`'s dispatch registration. `probe_arena.cpp` exists because the
first version of this reached none of it: eviction only fires in a narrow
band around a program's compiled size, so the arena is sized as a multiple of
an estimate rather than a constant.

**Execution oracle (`fuzz/qemu_exec/`).** The half that found five of the
nine. Batches of programs are loaded into guest flash, run through the real
`enterProgramSplit` on `qemu-system-arm` against the unmodified `runtime/`,
and diffed against the reference VM. One boot per batch. `minimize_exec.ts`
shrinks a failing program by deleting whole instructions and re-encoding, so
every candidate stays validator-approved, with ddmin-style coarse-to-fine
widths — one boot per pass, including in `--hang` mode, where a timed-out
batch identifies the first hanging variant by exactly how far it got.

**Seeds.** 32, all validated at generation time: the small single-procedure
shapes (shared with `test/qemu/fixtures.cpp` via `test/corpus_programs.h`),
multi-procedure/`CALL` shapes no single-procedure format can express, large
shapes aimed at specific compiled-size guards, and one regression seed per
fixed finding. `qemu_exec.ts seeds` is a standing check on all of them.

## Calibration discovered along the way

**The depth ceiling is not about `argCount`.** `Window::discardWindow`
reclaims a procedure's whole spilled frame in one `ADD sp, sp, #imm` — Thumb
T2, a 7-bit word immediate, so 508 bytes / 127 words. That caps compilable
**total TOS depth** at `WINDOW_SIZE + 127 = 131`: arguments and pushed
operands together, not just arguments as `target-profile.md` originally
framed it. Ungated, **84% of a real fuzz corpus landed above it**, so 84% of
the budget went to programs that could only ever bail and whose emitted code
neither half could look at. Gated at `REALISTIC_MAX_TOTAL_DEPTH = 128`;
`RESOURCE_ERROR` went 84% → 0.5%, and 13 mismatches surfaced immediately in
the newly reachable region. That 84% was all one reason
(`RESOURCE_LIMIT_WINDOW_RECLAIM`), which took this whole section to work
out; `qemu_exec.ts` now buckets its bail count by code, so the same
diagnosis is a line of output rather than an investigation.

**`spillImm`'s guard is unreachable.** It checks an SP-relative offset
against `Uoff<2, 8>`'s 1020 bytes (255 words), which `discardWindow`'s much
tighter ceiling bails before reaching. Defence in depth with nothing behind
it. Left in place — the two ceilings are independent things that could move —
but the `deep_spill` seed was aiming at it with depth 300 and only ever
exercised the bail; reduced to 128, where it compiles and runs. It has its
own code (`RESOURCE_LIMIT_SPILL_OFFSET`), so "unreachable" is now a corpus
check — a nonzero bucket for it means one of the two ceilings moved —
rather than an argument from their relative tightness.

**The `argCount ≤ 131` "open question" was already closed.**
`target-profile.md` recorded it as an unfixed assert; `discardWindow` and
`restoreWindow` both carry the `fail()` guard now. Measured directly with
`dump_code.sh`: 131 compiles, 132 / 500 / 972 / 2047 all bail with
`RESOURCE_LIMIT_WINDOW_RECLAIM`. Doc corrected — what remains is a
capability limit, not a crash.

## Environment notes

Recorded because each cost real time and none is discoverable from the docs.

- **Semihosting `SYS_OPEN` returns -1** on this QEMU/machine combination for
  every path tried, the `:tt` stdin special case included, while `SYS_WRITE0`
  works. The batch therefore arrives via `-device loader` into guest flash,
  which needs no semihosting for input at all.
- **`-device loader` caps a blob at `ram_size`.** At `-m 8k` it silently
  refused anything over 8192 bytes with nothing but "Cannot load specified
  image". `qemu_exec.ts` passes `-m 64k`; this board takes its SRAM size from
  its own DC0 register, not from `-m`, so the guest still sees exactly 8 KB.
- **Flash on `lm3s811evb` ends at 0xA000** (measured, not assumed). The
  runner's `rom` region is therefore 16 KB, not `test/qemu`'s 32 KB, which
  both gives a 24 KB batch window and makes the *linker* guarantee image and
  batch never overlap.
- **`-serial none`, not `-nographic`.** The latter wires the model's UART to
  stdio and interleaves it with the semihosting result lines. Semihosting
  output arrives on **stderr** under `target=native`.

## Not covered

- ~~Entry procedures taking more than one argument~~ — recorded here as a
  harness limitation, which it was not. `enterProgram*` passing a single
  `argIn` meant the *runtime* could not run such a program: an entry
  procedure declaring 2-4 arguments read window registers `enterDispatch`
  never initialized, and one declaring 5 or more also reclaimed a frame
  nobody had pushed, landing `.Lresume` on a shifted `sp` — a deterministic
  hang. Measured over 8,000 validator-approved programs: 0.04% in the first
  band, **2.56% in the second**. The skip was hiding it, and `test/qemu`'s
  own out-of-window-argument fixtures (27, 37) only ever covered `proc1`
  and `proc2`, never `proc0`. Fixed by giving `enterProgram*` a real
  argument vector (`runtime/entry_args.h`, `design.md` §9); the skip is
  gone and every entry procedure is now compared.
- Non-terminating programs — legal per §9, and fatal to a batch.
- `EXT` opcodes — the seam decodes them and validates their declarations
  (design.md §18), but no extension is registered in either fuzz half, so
  nothing generates one. Registering one also requires its wire format to
  round-trip byte-exactly, or `oracle_server.ts`'s canonical-length gate
  rejects every ext-carrying seed as malformed.

## Files touched

New: `fuzz/README.md`, `fuzz/make_seeds.ts`, `fuzz/dump_code.{cpp,sh}`,
`fuzz/probe_arena.cpp`, `fuzz/qemu_exec/` (runner image, driver, minimizer),
25 seeds, this document. A further 9 pre-existing seeds were rewritten into
the whole-program envelope format (34 in `seeds/` in total).

Modified — runtime: `runtime.S` (`trapHelper`), `dispatch_abi.{h,cpp}`,
`runtime_host.h`, `runtime_internal.h`, `enter_program.cpp`. Translator:
`binops.cpp`, `blocks.cpp`, `translate_proc.cpp`, `abi_strategy.{h,cpp}`,
`registers.h`, `accstate.h`. Harness: `harness.cpp`, `oracle_server.ts`,
`dump_seeds.{cpp,sh}`, `make_seeds.ts`. `@ppl/machine`: `vm.ts`,
`validate.ts`, `extension.ts`. `@ppl/codecs`:
`engine/codec-extension.ts`, `test/list-union.test.ts`. Tests:
`test_binops.cpp`, `test_blocks.cpp`, `test_translate_proc.cpp`,
`test/corpus_programs.h`, `test/qemu/{fixtures.h,fixtures.cpp,main.cpp,linker.ld}`,
`machine/test/{validate,extension}.test.ts`.
Docs: `design.md` §10 (the `BR_TABLE` bullet), §10.1's acc-fold
paragraph and the new §17, `target-profile.md`, `isa-core.md`
§4.1/§4.6/§7.1/§8.7, `isa-rationale.md`, `docs/codec-extension.md` §8.2,
`jit-armv6m/README.md`.

Finding 5's resolution reaches further than the rest, being a spec change:
`isa-core.md` §4.1, `rtl.ts` (`SHIFT_OPS`), `validate.ts`, `vm.ts`,
`oracle_server.ts` (stage 7), `qemu_exec.ts`, `make_seeds.ts`,
`binops.cpp`, `test_binops.cpp`, and
`target-js/test/binary-op-codegen.runtime.test.ts`, whose differential
matrix fed shift amounts of 32 and above straight into `evalBinary`.
