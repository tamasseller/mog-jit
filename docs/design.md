# MCU JIT: Generic Core → ARMv6-M

> **Status:** design plus implementation state. Assumes
> `packages/machine/docs/isa-core.md` throughout. `jit-armv6m/compiler` (C++)
> is the sole implementation, targeting the real dispatch/eviction runtime
> (`jit-armv6m/runtime`) on real ARMv6-M hardware via `qemu-system-arm`. A TS
> prototype (`jit-armv6m/prototype`) existed earlier as a faster-iteration
> blueprint for working out the algorithm before committing it to C++; it was
> retired once the native port reached full feature parity, and citations to
> it below (mostly in §16) are historical — they record how a given mechanism
> was originally worked out or a bug originally found, not a live path. §16
> tracks what is verified, what is derived on paper, and what is still open.

---

## 1. Goal

Native C/C++ entry points, callable from bare-metal firmware, that
JIT-compile and execute one Generic Core program injected at runtime
(`jit-armv6m/runtime/runtime_host.h`):

```c
typedef struct { uint32_t value; uint32_t trapped; } ProgramResult;

ProgramResult enter_program_on_stack(uint32_t argIn, const uint8_t *programBytes,
                                     uint32_t programSize, uint32_t codeArenaSize,
                                     uint32_t stackLimit, uint32_t interruptReserve);

ProgramResult enter_program_split(uint32_t argIn, const uint8_t *programBytes,
                                  uint32_t programSize, uint32_t codeArenaBase,
                                  uint32_t codeArenaSize, uint32_t stackLimit,
                                  uint32_t interruptReserve);
```

No bare arena-less entry point: both variants take an explicit `stackLimit`
and are checked against it up front. A caller that just wants a plain
global arena declares one itself (one line, sized to what it actually
needs) and calls `enter_program_split`.

`programBytes`/`programSize` is one whole serialized program: a
jit-armv6m-specific envelope (`max_call_depth:LEB128 total_depth:LEB128` —
`packages/machine/src/bytecode.ts`'s `encodeJitProgram`) prepended to an
ordinary isa-core.md §5.5 program (`proc_count:LEB128`, then each
procedure's own `arg_count:LEB128` immediately followed by its own body).
`proc_count` and both whole-program stats come out of that envelope, not a
caller-supplied parameter — isa-core.md §5.5/§11.4's own extension point
("a procedure header's extension fields... added when a real need
appears"): a bare-metal JIT needs `max_call_depth`/`total_depth` before it
can compile a single instruction (§2's static stack reservation, below),
and `validateProgram` already computes both, once, before the program is
ever serialized.

`trapped` is 0 for a normal return, nonzero for a `TRAP` code (isa-core.md
§4.5) propagated out or a `RESOURCE_ERROR` (§12). None of these return
until the program terminates.

Constraints: the target is ARMv6-M (Cortex-M0/M0+ baseline Thumb, no
Thumb-2). The code arena may be too small to hold every procedure's
compiled code at once, so execution must still proceed, compiling and
evicting on demand. Translation is single-pass and as context-free as
possible, followed by one branch-range fixup pass. Generated code is
position-independent (no embedded absolute addresses), so eviction and
compaction never need a relocation pass.

`enter_program_on_stack` makes the current C stack the whole work area:
`Runtime`, its dispatch table, the operand stack and the compiled-code
arena all come out of it. `enter_program_split` puts the arena in
caller-supplied memory instead (a distinct SRAM bank, CCM, whatever the
target's bus matrix makes worth using) while `Runtime`, the dispatch table
and the operand stack stay on the C stack, since the translator and any C
helpers reached through it are ordinary C using that stack regardless of
where the code they emit lands.

---

## 2. Memory layout

Everything lives in one contiguous descending region, not a set of separate
reservations. ARMv6-M's stack descends while compiled code advances upward,
which is the geometry that makes sharing work:

```
(lower addresses)
    compiled code, growing up from stackLimit
        compiled fn 1
        compiled fn 2
        ...
    codeLimit ......................... margin, currently unused
    helper / translator frame           dynamic, self-checked
    operand stack                       call/return records interleaved
        JIT frame J ... JIT frame 1
    dispatch table + sentinel slot      fixed
    enter_program frame                 MSP → PSP switch happens here
    app stack frame K ... frame 1       (MSP)
(higher addresses)
```

`enter_program` computes a hard ceiling for compiled code once, at entry:
`codeLimit = SP(at entry) − requiredStackBytes`. Every term of
`requiredStackBytes` (`jit-armv6m/runtime/enter_program.cpp`, summing the
fixed-cost constants `jit-armv6m/runtime/dispatch_abi.h` declares) is
derived from the program's own wire envelope (§1) or a measured constant:

| Term | Source |
|---|---|
| `Runtime` plus its dispatch table | `sizeof(Runtime) + (procCount+1)·sizeof(ProcSlot)` |
| Operand stack | `operandStackBytes` = `totalDepth · 4`, from the program's own envelope |
| Live call/return records | `maxCallDepth · CALL_RECORD_BYTES`, `maxCallDepth` from the same envelope |
| compileProc's own callee-argCount lookup table | `procCount · CALLEE_ARG_COUNTS_BYTES_PER_PROC` (a VLA, not a fixed cap — §9's `ProcSlot` doesn't give O(1) indexing by callee for free) |
| Fixed implementation overhead | `ENTER_DISPATCH_FIXED_BYTES` plus the translator's entry worst case |
| Exception entry | `interruptReserve` |

Because those are static, ordinary code growth needs no runtime check
against the stack side: the reservation makes a collision impossible by
construction, and hitting `codeLimit` is the same evict-and-compact trigger
§8 already has, anchored to a precomputed line rather than a detected
collision. The check runs once, before any of that memory is touched,
against a caller-supplied `stackLimit` (the lowest address the excursion
must never reach); on failure `RESOURCE_ERROR` comes back with nothing set
up.

**`operandStackBytes` is `totalDepth · 4`, uncredited.** The tempting
`max(0, totalDepth − 4) · 4`, on the theory that the 4-register window (§5)
absorbs the top four live slots for free, is wrong twice over: a worst-case
path can end in a leaf that is pure acc-in/acc-out (`argCount` 1, no `PUSH`
at all), crediting zero absorption at its own peak, and a `CALL` site spills
whatever is resident but isn't an outgoing argument regardless of window
room, because the callee needs a canonical register layout for its incoming
args. Depth alone doesn't determine how much the window absorbs at any
moment; a tighter figure needs an analysis tracking actual spilled bytes
through the real call-boundary shuffling.

**The translator's own exception.** Nothing in `@ppl/machine` bounds
`BR_TABLE`/`LOOP` nesting depth, and different procedures nest arbitrarily
differently, so the translator's block-nesting bookkeeping has no
program-wide worst case. Rather than reserve for it, it is ordinary dynamic
stack usage (C recursion, or an explicit `alloca`-grown array, coexisting
with the translator's own call stack) checked *live* against whatever of
`codeLimit`'s margin remains. The translator is the one thing allowed to
encroach on `codeLimit`, provided it tracks that and fails into
`RESOURCE_ERROR` once there is no room for both the code still to emit and
this procedure's nesting depth. `alloca` overflow is undefined behavior
rather than graceful failure, so the check happens before growing. The
translator's *fixed* prologue footprint, paid on entry before it can check
anything, belongs in the static helper-stack term above; only depth beyond
that floor is policed live.

This is sound only because nothing the translator holds across the pass (a
block-nesting record's jump-table base, a pending branch fixup) is an
absolute arena address. Every offset is relative to the current procedure's
own start, the same position-independence §11 requires of emitted code, so
compaction sliding the code region never invalidates a still-open record.

The now-retired TS prototype's own mock translator (formerly
`prototype/qemu/compile_proc.cpp`) had no Frame stack at all: it was an
unconditionally-terminating copy from a precompiled blob, so its worst case
was one number (§16 item 19). The real translator's own figure,
`TRANSLATOR_ENTRY_WORST_CASE_BYTES` (`jit-armv6m/runtime/dispatch_abi.h`),
has been re-measured via `-fstack-usage` twice since — once when the real
translator replaced the mock (item 19) and again when `compileProc`'s own
call chain moved onto `Assembler` (item 23) — each time itemized per
function on the real path, never guessed. Build-time enforcement (a
per-file `-Wstack-usage=`/`-Werror=stack-usage=` pin, matching the
now-retired mock translator's own analogous rule) remains a reasonable
follow-up, not done.

**Interrupt isolation.** Checked against ARMv6-M's exception-entry
pseudocode (ARM DDI 0419E, `PushStack()`/`ExceptionTaken()`): the hardware
pushes its 8-word exception frame (`R0-R3`, `R12`, `LR`, return address,
`xPSR`, 32 bytes) onto `SP_process` only if
`CONTROL.SPSEL=='1' && CurrentMode==Thread` at the instant of the
exception, and `ExceptionTaken()` unconditionally clears `SPSEL` on entry to
Handler mode. A higher-priority exception preempting a running handler
therefore fires with `CurrentMode==Handler`, fails that condition, and dumps
to `SP_main`. Nested and tail-chained interrupts can never stack more than
one frame onto whichever stack Thread-mode code was using.

So: run the JIT in Thread mode on PSP (an explicit `MSR PSP` / `MSR CONTROL`
/ `ISB` sequence at entry, since the reset default is MSP), and reserve MSP
exclusively for Handler mode pointed at separate memory. Exactly one 32-byte
frame needs budgeting regardless of interrupt priority count or nesting
depth. `interruptReserve` is a parameter rather than a baked constant
(`ARMV6M_EXCEPTION_FRAME_BYTES` is the bare architectural minimum), so a
caller with RTOS or ISR requirements beyond that passes its own figure.

Register isolation is free from AAPCS's callee-saved convention for
`r4-r8`/`r10`/`r11`, since an interrupt handler is an ordinary AAPCS
function. `r9` is AAPCS's *platform* register, whose saved-ness is a build
convention rather than a blanket guarantee, and this design uses it for the
runtime pointer (§3), so the build has to commit to treating it as
callee-saved. That is the usual embedded convention, but a toolchain setting
to pin down deliberately.

---
## 3. Register assignment

| Reg | Role | Notes |
|---|---|---|
| `r0` | `acc` | Never touched by call/return bookkeeping; carries the live last argument straight into the callee. |
| `r1` | Entry ABI: index, then slot address | Starts as the packed record (at a `CALL` site) or a dispatch index, becomes `slotAddr` for the tail jump (§9). Doubles as translator scratch, disjoint lifetimes. |
| `r2` | Entry ABI: offset+1 | The Thumb-mode bit is pre-folded into the value, consumed by the prologue stub's `ADD r2,r2,pc` / `BX r2` (§9). |
| `r3` | Entry ABI: jump target | Dead the instant control lands, so it is free scratch inside the prologue stub. |
| `r4-r7` | TOS window, circularly renamed | §5. |
| `r8` | Dispatch table base | Hi register: needs a low mirror for any load/store through it. |
| `r9` | Runtime pointer | Hi register, but only ever moved whole via `MOV` into a C function's argument register, never a load/store base, so it never pays the mirror tax. |
| `r10` | Static helper vector base | Flash-resident, link-time fixed, never touched by compaction or eviction (§11). |
| `r11` | LRU tick counter | Monotonic, full 32 bits; doesn't realistically wrap, so the eviction scan is a plain comparison. |
| `sp` | Operand/TOS spill stack, and call/return records | Genuinely the same stack the host used to call in, not a second one (§2). |
| `lr` | Carries the live call/return record | Set by `callHelper` (§9), not by any hardware `BL` in compiled code: this JIT never emits one. A procedure making a nested `CALL` saves it in its own prologue. |

**The hi-register constraint shapes everything.** Thumb-1's hi-register
operations are exactly `ADD`, `CMP`, `MOV`, plus `BX`/`BLX`. No
`LDR`/`STR`/`LDM`/`STM` encoding accepts a high register as its base, not
even the multiple-register forms. So every touch of `r8` (or `r10`) that
addresses memory detours through a low register: `MOV Rlo, r8`, the real
load/store on `Rlo`, `MOV r8, Rlo` back if it changed. That per-touch tax
is the strongest argument for making dispatch-table entry a *shared
routine* rather than inlining it at every `CALL`/`RETURN` site: a fixed cost
is worth amortizing precisely because it can't be designed away. It is not
a blanket tax, though: `ADD r1,r1,r8` is one instruction, so it is worth
checking at each hi-register touch whether the consuming operation is one of
the three exceptions before reaching for a mirror. `PUSH`/`POP` address
`sp` implicitly and were never subject to it.

**Why `acc` is `r0`.** AAPCS passes a native function's first argument and
its return value in `r0`, so a single-argument helper (§10's
`CLZ`/`REVBITS`, and anything else in §11's helper vector) is reachable with
a bare `BLX`: `acc` is already the argument register going in and the
return register coming out, no `MOV` on either side. This only pays for
arity-1 helpers, since this design never holds more than one value hot in
`acc`, and a 2-argument helper would need a second operand shuffled in from
the window or spill stack regardless of which register hosts `acc`.

Helpers do not have to be strict leaves. What is required is a
*documented, statically-provable* worst-case stack cost, folded into §2's
static reservation. Zero is the tightest case and still the right target for
something as small as `CLZ`/`REVBITS`, via `-Wstack-usage=0` promoted to
`-Werror=stack-usage=`: GCC's only way to preserve `lr` across a nested
call is pushing it, so a 0-byte report transitively proves leaf-ness too.
Hand-writing the helper (`__attribute__((naked))` plus inline Thumb)
guarantees it by construction instead, matching how this project already
hand-writes its own Thumb encoder.

**Naming collision to watch for.** isa-core.md's worked examples use `r0`,
`r1`, … as *abstract* frame-relative slot names (`LOAD 0` reads frame slot
0), a different namespace from this section's *physical* ARM `r0`.
Disambiguate explicitly ("isa-core's `r0`" versus plain `r0`) anywhere both
could appear near each other; the Appendix is exactly such a spot.

---

## 4. Wire vs physical register indexing

isa-core.md §2.5 defines TOS as per-frame: every invocation gets its own
entry point. The window mapping (§5) rides directly on that, defined purely
in terms of a procedure's own frame-relative `rN`/`tos` as the bytecode
already expresses them, with nothing global layered on top.

That is sound because of §6's canonical-phase invariant: every frame base
is forced to phase 0 (`k=0 → r7`) regardless of the caller's own
frame-relative `tos` at the call site. A procedure's translation therefore
only ever reasons about its own frame-relative depth, never the call chain's
accumulated depth, which is what makes it context-free: nothing crosses a
`CALL` boundary except the shuffle itself (§6).

---

## 5. The register window

Physical register for frame-relative index `k` (isa-core's `rN`, or TOS
depth relative to the current frame base):

```
in_window(k)  ⟺  tos − k ≤ 4
phys(k)       =  r7 − (k mod 4)          when in_window(k)
              =  spilled to the sp stack  otherwise
```

`phys`'s cyclic direction is descending. That is chosen, not arbitrary: it
is what makes historical spill-stack reloads batchable at all, by making an
already-emitted sequence of individual chronological spills exactly
equivalent to one hypothetical batched `PUSH`, so a batched `POP` can read
it back later (`compiler/src/window.h`'s header has the full argument).
Direction aside, the property that matters is that `phys(k)` is a pure
function of `k` and the current `tos`, never of push/pop history along
whichever control-flow path arrived here. Two `BR_TABLE` cases, or a `LOOP`
back-edge, that reconverge at the same `tos` therefore agree on `phys(k)`
for every live `k` with no cross-path reconciliation.

**Spill and fill.** Pushing slot `k+4` evicts slot `k`'s current value to
the spill stack via a real `sp`-decrementing single-register `PUSH` before
the register is overwritten. Popping back down unconditionally reloads it
with a real `sp`-incrementing `POP`, with no liveness tracking, which keeps
the translator simple at the cost of leaving cheap dead-reload elimination
on the table.

**`LOAD`/`STORE` and register-mode operands work the same way across the
window boundary.** A slot that has fallen out of window (any local
surviving past `WINDOW_SIZE` more pushes, `CALL` args among them) is still
an ordinary reference: `LOAD`/`STORE` or a `REG_ACC`/`REG_REG` operand
resolves to the spill stack (`window.ts`'s `spillOffset`) instead of a
register, one `LDR`/`STR` against `sp` rather than zero instructions. Same
"most recently spilled sits closest to `sp`" addressing the window's own
spill/fill uses, read directly rather than through a `PUSH`/`POP` pair. Any
procedure with more than `WINDOW_SIZE` concurrently-live locals hits this,
with no `CALL` involved.

**Block-exit truncation costs instructions.** isa-core.md §8.1 drops TOS
surplus above the enclosing entry depth implicitly at `BLOCK_END`/`RETURN`:
a validator-level guarantee, not a bytecode-level pop sequence. Because
`phys(k)` cycles through `r4`-`r7` as `tos` grows, a path that pushed extra
values has overwritten (and spilled) in-window slots belonging to the target
depth. The translator synthesizes an explicit pop-multiple-equivalent
restore at every truncation point (`BLOCK_END`, `LOOP` back-edge, `RETURN`)
to bring `r4`-`r7` back to what the target depth's `phys(k)` expects. Both
depths are statically known, so this is local codegen, the same operation as
the call-boundary shuffle (§6) on a different trigger.

---

## 6. Calling convention

isa-core.md §6: the callee's frame base is the caller's TOS at the call;
args `0..K-1` on the stack, arg `N-1` (if any) in `acc`.

**Canonical-phase invariant.** Every procedure's native code is translated
assuming its own frame base lands at phase 0 (`k=0 → r7`), independent of
the caller's absolute `tos` at the call site, which varies per site and per
invocation. Two things depend on it: the same compiled procedure is called
from differently-phased sites, and return must be re-enterable after the
callee's *caller* was itself evicted and recompiled (§7), which requires
recompiling the same bytecode to reproduce the same native layout, which
holds only if translation never depends on caller-side compile-time
context.

**The shuffle.** Before `CALL`, the caller's window is generally at a
non-zero phase relative to what the callee expects at `k=0`. Let
`S = max(callee.arg_count − 1, 0)` be the stack-passed argument count and
`callerTos` the caller's frame-relative `tos` at the call. The window holds
`w = min(callerTos, 4)` resident slots, of which the top `m = min(S, w)` are
exactly the top `m` stack args; the other `w − m` are the caller's older
locals still resident alongside them. Any of the `S` args below the window's
bottom edge are already spilled at their correct address and never move.

A single `PUSH {r4-r7}` cannot implement this. Hardware `PUSH`/`POP` store
and load in fixed *ascending-register-number* order, which matches the
order a `k`-keyed spill address needs — largest `k` closest to `sp`, the
layout both the callee's canonical `phys(0)..` reload and §5's
chronological spill sequence rely on — only while the pushed range doesn't
wrap `phys`'s `r4`→`r7` boundary; within one run, `phys`'s descending
direction already makes ascending register coincide with descending `k`
for free. Concretely: with the window holding `k = 5,6,7,8` (wrapped
once), `phys(k)` is `r6,r5,r4,r7`, so register-ascending order visits them
as `k = 7,6,5,8`, and a batched push would write the wrong value to the
wrong slot address.

What works is two *different* orderings for two *different* consumers
(`compiler/src/window.cpp`'s `spillForCall`/`fillCalleeArgs`/
`reloadAfterCall`):

1. **The caller's own non-argument locals** (`w − m` of them) need no
   ordering argument at all. Thumb's `PUSH`/`POP` register list is an
   arbitrary 8-bit mask, not required to be contiguous, and if nothing
   touches a set of registers between a `PUSH{set}` and a later
   `POP{that same set}`, hardware's own inverse guarantee restores exactly
   what was pushed regardless of `k`, wrap or ordering. So the whole
   leftover set spills in **one** `PUSH` of whatever mask it happens to be
   and returns in **one** mirrored `POP` of that mask. It was never a reload
   keyed by `k`, just a round-trip forced by the callee's execution
   intervening. (Contrast `restoreWindow`'s block-exit case, where nothing
   intervenes and the same round-trip correctly reduces to no instructions.)
2. **The stack-passed args** (`m` of them) are a real remap: instead of
   returning to their own registers they are popped straight into the
   callee's canonical `phys(0)..phys(m-1)`. That consumer, one combined
   ascending-register `POP`, reads its *lowest* register from the address
   closest to `sp` — under §5's descending map that lowest register is
   `phys(m-1)`, the *highest* arg index — so it wants the largest `k`
   closest to `sp`, which a batched `PUSH` delivers in at most two
   instructions when the arg range wraps: push the pre-wrap run (the
   smaller `k`s) first, the post-wrap run (the larger `k`s) second, so
   whichever executes second lands lower and is what the `POP` reads
   first.

Net cost: `1 + ≤2` to spill, `1` to fill (`fillCalleeArgs`, if `S > 0`),
and symmetrically `1 + ≤WINDOW_SIZE` to restore, the second term being
whatever is genuinely deeper than the leftover range, spilled long before
this call via individual natural-order spills with no single `PUSH` to
mirror. The common `argCount ≤ 1` case (`S = 0`) costs nothing at all.

**A procedure whose own body contains a `CALL` must save and restore the
call/return record.** `callHelper` delivers it in `lr` (§9) and the nested
call overwrites `lr` with the nested callee's own record, so a non-leaf
procedure pushes it in its own prologue (`abiEmitPrologue`'s `push {lr}`)
once per activation, and `RETURN` retrieves it from there
(`returnHelperFromStack`, §7). A leaf procedure never needs to.

**`sp` must genuinely track current depth, with no fixed per-procedure
reservation.** Reserving a `localPeak`-sized block per procedure via
`SUB sp,#4·localPeak` on entry, addressing every spill at a constant offset
from that unmoving base, does keep frames from colliding, but its real
footprint is the *sum* of per-procedure maxima along the call chain.
isa-core.md §8.3 computes the whole-program bound as a *maximum* over call
sites and local peaks, explicitly tighter than that sum, and only *reusing*
the same storage as frames come and go achieves it. So `sp` is a real moving
stack pointer: every ordinary spill an `sp`-decrementing `PUSH`, every fill
an `sp`-incrementing `POP`, nothing reserved up front. This also makes the
leftovers' natural order and the shuffle's batched pushes land correctly
with no "permanent versus transient" storage concept anywhere, since a
callee's spills land strictly below whatever the caller had already pushed,
purely because `sp` only ever moves further down.

**`stackArgs ≥ WINDOW_SIZE`** needs some of the callee's own arguments
addressable from its first instruction despite starting below the window,
which is §5's spill-stack addressing for `LOAD`/`STORE` generally rather
than just at the boundary. Once that existed,
`spillForCall`/`reloadAfterCall` needed no logic change: the natural
chronological ordering they rely on already produces the right memory
layout for a callee's deep arguments. Three constraints only a concrete deep
case exposes (originally found via the now-retired TS prototype's own
`deep-args.test.ts`; `test/host/test_window.cpp` and
`test_abi_strategy.cpp` cover the equivalent native shapes):

- `fillCalleeArgs` caps at `WINDOW_SIZE - 1`. `physReg` is periodic mod
  `WINDOW_SIZE`, so at `stackArgs === WINDOW_SIZE` exactly, `physReg(0)` and
  the callee's acc-delivered last argument land on the same register and the
  prologue write clobbers what `fillCalleeArgs` just placed there.
- `fillCalleeArgs` needs the same "at most two batched `POP`s, larger-`k`
  first" mechanism (`popRuns`) that `restoreWindow`/`reloadAfterCall` use:
  with deep arguments the register-delivered range no longer starts at phase
  0, so it can wrap `physReg`'s cyclic boundary, and one combined `POP`
  across the wrap would reassign which value lands where.
- `reloadAfterCall` caps its historical-reload range at `targetTos`, not the
  window's bottom. Everything below the caller's resident window is
  unrelated leftover data only while `stackArgs ≤` that window; past that,
  some of what is deeper was consumed as an argument, and reloading it reads
  memory the callee's epilogue already reclaimed.

**Callee-side prologue.** The shuffle is the caller's job. The callee's
smaller obligation is that isa-core.md §4.6's last argument arrives in `acc`
rather than at its home register `phys(argidx)`, and nothing in the bytecode
guarantees `acc` survives untouched until the first instruction reading it
(in the Appendix example, `CONST #1` clobbers `acc` before `v` is read
back). Rather than an unconditional copy, this folds into §10.1's state
machine as its own entry state, so a procedure whose first real instruction
reads the argument straight back, a common shape for small procedures, pays
nothing.

---
## 7. Return

`RETURN` cannot compile to a bare native return: the caller may have been
evicted from the arena while the callee was running. The call/return record
therefore carries `(proc_idx, offset)`, not a raw code address, where
`offset` is a native-code offset relative to the caller's compiled procedure
start. Both halves are compile-time constants at the point the translator
emits the `CALL` (the caller's own index, and the byte offset just past this
call sequence within the caller's about-to-be-fully-emitted code), packed
into one 32-bit immediate: `proc_idx` in the low half, `offset + 1` in the
high half.

The record travels in `lr`. This JIT never emits a hardware `BL`/`BLX` in
compiled code, so `lr` is otherwise dead, and using it costs neither a
dedicated region nor a per-call stack word. `callHelper` (§9) moves the
packed value there once, in flash, never duplicated per call site. A
procedure making a nested `CALL` saves it to its own stack in its own
prologue, once per activation (§6).

Re-entry is stable across eviction and recompilation because of §6's
canonical-phase invariant: translation is a pure function of
`(proc_idx, bytecode)`, so recompiling reproduces the same layout and a
saved `offset` stays valid.

`RETURN`'s own compiled form inlines none of this. It loads one shared
runtime routine's address from the static helper vector (§11) and tail-jumps
to it, three instructions:

```
MOV  r3, r10                ; low-mirror the helper vector base
LDR  r3, [r3, #4]           ; returnHelperFromLr (index 1)
BX   r3
```

Four shared entry points feed one tail (`jit-armv6m/runtime/runtime.S`), chosen
per procedure by what that procedure's own prologue did:

| Helper (vector index) | When | What it does |
|---|---|---|
| `returnHelperFromLr` (1) | leaf procedure | `MOV r1, lr`: the record has sat untouched since entry |
| `returnHelperFromStack` (2) | ordinary non-leaf | `POP {r1}`: retrieve what its own prologue pushed |
| `returnHelperFromStackReclaim` (7) | non-leaf with out-of-window arguments below the pushed record | `POP {r1}` then `ADD sp, sp, r2`: same retrieval, plus reclaiming what `discardWindow` deliberately left behind |
| `returnHelperTail` (3) | (shared unpack+dispatch) | reached by fallthrough from index 2/7, or by a plain branch from index 1 |

The fourth variant exists because reclaiming out-of-window arguments
sitting below the pushed record needs a procedure-specific byte count no
parameterless routine can know on its own — but the count itself is the
*only* thing that varies, so the call site (`abiEmitReturn`,
`compiler/src/abi_strategy.cpp`) loads it into `r2` and lets the helper do
the retrieval and the reclaim together, rather than inlining either.

The shared tail unpacks and dispatches:

```
LSRS r2, r1, #16            ; r2 = offset+1
SXTH r1, r1                 ; r1 = caller_idx, sign-extended
LSLS r1, r1, #4             ; · sizeof(ProcSlot)
ADD  r1, r1, r8             ; r1 = slotAddr
LDR  r3, [r1, #0]           ; r3 = code_ptr
BX   r3
```

`SXTH`, not `UXTH`: the boot record's sentinel `proc_idx` of `0xffff`
("index −1") must resolve to `r8 − 16` (`sizeof(ProcSlot)`, §9), the
sentinel slot, not `r8 + 1048560`.

---

## 8. Eviction, compaction and pinning

**Nothing is pinned.** isa-core.md-level reasoning suggests the currently
executing procedure must never be evicted, since its code is what the core
is fetching from, but the concrete mechanism here rules the hazard out
structurally: `callHelper` always hands the caller's record off (into `lr`,
or onto the caller's own stack) *before* tail-jumping toward the callee. By
the time the translator ever runs, which is the only place eviction
triggers, control has already left every byte of arena-resident code, and
every suspended frame including the immediate caller is exactly as
evictable as any deeper ancestor.

So the eviction loop runs over the whole table with no exclusions, evicting
the global LRU minimum each round until either enough room appears or the
table empties with the new procedure still not fitting, which can only
happen when that one procedure alone is larger than the entire arena. There
is no smarter recovery at that point: `RESOURCE_ERROR` (§12). Worst case the
loop is O(n²) in the resident-procedure count, fine given how small `n` is
on any real embedded target and that this is already the rare, expensive
path.

**Compaction** slides surviving procedures' code down to close an evicted
one's gap, then updates only the dispatch table's `code_ptr` entries:
O(procedure count), not O(code size). A procedure's code length comes from
neighbors rather than a stored field (`occupiedSizeOf`,
`jit-armv6m/runtime/runtime_internal.h`): compaction keeps every resident
procedure packed back to back with no gaps, so a scan for whichever other
resident entry has the next-closest `code_ptr` above this one's (or the
arena's high-water mark if none) gives the boundary, and boundary minus
`code_ptr` is the size. Same cost class as the LRU minimum scan, paid only
on the already-expensive evict/compact path, never per touch.

---

## 9. Dispatch table and the shared handoff

Both control-transfer edges, forward `CALL` and `RETURN`, reduce to the same
two-parameter entry: `r1` = dispatch index, `r2` = offset + 1, with `CALL`
supplying a hardwired `1` (a fresh call never resumes mid-procedure) and
`RETURN` supplying whatever it just unpacked. Neither site branches on
whether the target is resident; that decision lives entirely in what the
dispatch slot currently holds.

**Dispatch table entry: 16 bytes.**

```c
struct ProcSlot {
    uint32_t code_ptr;    // mutable — dispatch address (Thumb bit set) or translator_trampoline
    uint32_t last_used;   // mutable — LRU tick, bumped by the prologue stub
    uint32_t body_ptr;    // static — absolute flash address of this procedure's own body_bytes
    uint32_t static_info; // static, packed: bit31 needs_lr_save; bits[30:20] arg_count; bits[19:0] body_bytes
};
```

No `state` field: "not resident" is `code_ptr == translator_trampoline`. No
doubly-linked LRU list: a linked list needs 4-6 pointer writes to unlink and
relink on *every touch*, the hot path of every call and return, where a
timestamp needs one store, and eviction, the rare heavy path, absorbs a
linear minimum scan instead. No `size` field (§8 derives it); the bytes it
would have cost fold into widening `last_used` to a full word, which at 32
bits doesn't realistically wrap in an embedded system's lifetime, so the
scan is a plain comparison.

The static half (`body_ptr`/`static_info`) is what makes this table
double as the whole-program procedure directory §16 originally tracked as
missing entirely: `enter_program`'s one-time wire-format walk
(`Runtime::init`, `jit-armv6m/runtime/runtime_internal.h`) fills it in for
every procedure before `enter_dispatch` ever runs, and `compileProc` reads
a procedure's own `arg_count`/body location/`needs_lr_save` straight out
of its own slot instead of any fixture- or caller-supplied side channel.
It has to live in the *same* table as the mutable dispatch half — not a
second array — because a struct may have only one trailing flexible-array
member, and everything here is meant to be reachable through the one
fixed ABI pointer (`r8`), not a second, independently-based allocation.
Stepped up from a tight 8 bytes to 16 to fit the static half in, while
keeping `idx → slotAddr` a single `LSLS r1,r1,#4` rather than a `MULS`
that first has to materialize the entry size.

The table is preceded by a **sentinel slot** at index −1, whose `code_ptr`
is `enter_dispatch`'s own landing address, so the entry procedure's `RETURN`
dispatches back into C through the identical mechanism as any other return.

**`CALL Q`** compiles to one fixed, unconditional, constant-length
five-instruction sequence (`abiEmitCall`, `compiler/src/abi_strategy.cpp`):

```
LDR           r1, [pc, #...]         ; #REC(P_idx, K+1) — pooled, never inline-synthesized
MOVS/LDR      r2, #Q_idx             ; imm8 if it fits, else also pooled
MOV           r3, r10                 ; low-mirror the helper vector base
LDR           r3, [r3, #0]            ; callHelper (index 0)
BX            r3                      ; tail; never returns to this site
; K = byte offset of the next instruction, from this procedure's body start
```

The record's own value depends on `K`, and `K` in turn depends on how many
instructions this whole sequence takes to encode — a circular sizing
problem an earlier version of this design solved by iterating
(`findResumeOffset`) to a fixed point. `Assembler::materializeImm32Pooled`
(`compiler/src/assembler.{h,cpp}`) removes the circularity instead: a
pooled load is exactly one halfword at the call site regardless of the
value it loads (the 4-byte word itself lands later, at the next literal-
pool flush), so both operands above cost exactly one halfword each no
matter what they encode, making the sequence's own length a compile-time
constant and `K` closed-form. The record is *forced* through the pool
(never left to the ordinary pool-or-synthesize threshold): that circular
dependency is exactly what forcing exists to break.

The record setup has to be part of this same fixed sequence, before the
handoff. The call site cannot know which path the callee takes (real code,
or the trampoline with an eviction and compaction cycle possibly nested
inside), because that is decided by the slot's contents rather than anything
the caller inspects, and by the time either path runs control has already
left the caller. There is no later point at which bookkeeping needed for a
correct eventual return could still be installed.

**`callHelper`** fuses record handoff and dispatch, so a `CALL` makes one
hop rather than two:

```
MOV   lr, r1                ; persist the record; r1 free again immediately
LSLS  r1, r2, #4            ; Q_idx · sizeof(ProcSlot)
ADD   r1, r1, r8            ; r1 = slotAddr
LDR   r3, [r1, #0]          ; r3 = code_ptr
MOVS  r2, #1                ; offset+1 = 1, hardwired
BX    r3
```

**The per-procedure prologue stub** is the first six instructions of every
compiled procedure (`emitPrologueStub`), copied into the arena ahead of the
body, which is why it is emitted as data the translator can copy rather than
reached by name:

```
MOV  r3, r11                ; low-mirror the LRU tick (STR has no hi form)
STR  r3, [r1, #4]           ; entry.last_used = old tick
ADDS r3, r3, #1             ; bump on the low copy: low-reg ADDS takes an immediate
MOV  r11, r3                ; publish
ADD  r2, r2, pc             ; r2 = (offset+1) + (this instruction's address + 4)
BX   r2                     ; a real branch, never a write to pc
; ... procedure body starts here
```

The stub's fixed size is load-bearing: `ADD r2,r2,pc` reads "address of this
instruction + 4", which lands exactly on the first byte past the stub, so
`offset + 1 = 1` resolves to body start and any other offset is a byte
distance from that same point.

Reading `PC` as an ordinary *source* operand via the hi-register `ADD` is
universally defined, unchanged from ARMv4T Thumb onward, and is the idiom
`ADR` and every PC-relative literal-pool load already rely on. A
data-processing *write* to `PC` (`ADD PC,PC,r2`) is a different question on
ARMv6-M and on an ARMv7-M core running the same binary, so the design keeps
it out entirely and reaches its target through `BX`, the one place control
transfer is unambiguously specified across the family. The Thumb-mode bit is
pre-folded into `r2` at the source: every producer of an offset is a
compile-time constant, so storing `offset + 1` costs nothing and removes a
separate bit-set step from the stub.

**The translator-entry trampoline** is what every uncompiled slot's
`code_ptr` points at, reached by a bare `BX` like any other slot target,
with `r0` (acc), `r1` (slotAddr) and `r2` (offset+1) all needing to survive.
It is the one piece of the runtime that must be hand-written assembly, since
it makes a real call into the translator's C body whose address isn't known
until link time:

```
push {r0, r1, r2, lr}       ; lr holds the live record; bl clobbers it
MOV  r3, r8                 ; low-mirror the dispatch base
SUBS r0, r1, r3             ; r0 = slotAddr − base
LSRS r0, r0, #3             ; r0 = idx (compileProc's 1st argument)
MOV  r1, r9                 ; runtime pointer (2nd argument)
REALIGN_ENTER
BL   compileProc
REALIGN_LEAVE
POP  {r0, r1, r2}
POP  {r3}                   ; POP can't target lr directly
MOV  lr, r3
LDR  r3, [r1, #0]           ; the code_ptr compileProc just wrote
BX   r3
```

`r8`-`r11` need no saving: they are AAPCS callee-saved, and `compileProc` is
built with `-ffixed-r8/r9/r10/r11` so the compiler cannot touch them.

`REALIGN_ENTER`/`REALIGN_LEAVE` round `sp` down to an 8-byte boundary before
handing off to independently-compiled C and restore it after. Nothing in
this runtime's own hand-written excursion needs AAPCS's 8-byte-SP guarantee,
none of it being independently compiled, but the callee across a real `BL`
might use `LDRD`/`STRD` on a core that has them, or simply assume its locals
land 8-aligned. The macro is branchless and indifferent to whether `sp` was
already aligned: it reserves a full 8 bytes below the true `sp` (enough for
the stash whether rounding needed 0 or 4 of it), rounds down, then stashes
the real `sp` at a fixed +4 offset into that slack rather than pushing it,
since a push after rounding down would re-misalign `sp` for the call.

**`enter_dispatch`** is the host's entry into the ABI, an ordinary AAPCS
function rather than C inline asm: the `BX`-chain that follows is opaque to
the compiler either way, and as a real function it needs no clobber list, no
`-ffixed-rN` fight over spare operand registers, and no staging array. It
saves `r4`-`r7` and (mirrored through low registers) `r8`-`r11`, sets up
`r9` (runtime pointer), `r8` (`runtime + RUNTIME_DISPATCH_TABLE_OFFSET`),
`r10` (`g_helperVec`) and `r11` (tick 0), writes its own resume address into
the sentinel slot, then tail-branches into `callHelper` with a boot record of
`proc_idx = 0xffff` and `Q_idx = 0`. The result comes back as a `uint64_t`
(value in `r0`, trapped flag in `r1`) rather than a two-word struct: AAPCS32
returns a composite in registers only up to 4 bytes, while a double-word
integer goes in `r0:r1` directly, with nothing needing to survive the call.

---
## 10. Translation pipeline

**Pass 1, per-instruction emission.** A single forward walk with no
cross-instruction analysis beyond the local `tos` the pass already tracks,
the same invariant `validateProgram` relies on for isa-core.md §8.3.
Per-opcode-class notes:

- **Arithmetic/comparison** (isa-core.md §4.1/§4.2 addressing modes): a
  register-mode operand already in window is one native ALU op; out of
  window it takes a spill-stack `LDR` first. Peek and pop modes follow from
  §5's window (peek is the top-of-window register in place; pop reads and
  shrinks the window, with unconditional refill).
- **`BR_TABLE`.** ARMv6-M has no `TBB`/`TBH` (Thumb-2 only). `N ≤ 2`, the
  overwhelming common case (`if`/`if-else`, isa-core.md §7.1), compiles to
  `CMP` plus a conditional branch, no table. `N > 2`
  (`compiler/src/blocks.cpp`'s `openBrTableJump`) needs a literal-pool jump
  table plus a computed `BX`, but not one dispatch routine per site: one
  flash-resident copy for the whole program (§11's reserved slot 6,
  `brTableJumpHelper`, `jit-armv6m/runtime/runtime.S` — §16 item 21, done),
  reached by `BLX` through the helper vector, with the call site's own table
  addressed relative to `lr` exactly as it would be after a local `BL`.

  Table entries are clamped to `N`, one slot *past* the last real case, not
  `N - 1`: isa-core.md's `acc ≥ N` behavior is "no case body runs, `acc`
  left untouched", which a naive `N`-entry clamp turns into "re-run the last
  case". The register holding the clamped index is deliberately never `acc`,
  so it survives dispatch unmodified on both paths.

  **A non-obvious ARMv6-M trap here.** `BL`/`BLX` always set `lr` with bit 0
  forced to 1, the Thumb-mode marker a later `BX`/`POP{PC}` needs. Harmless
  for a branch target, where hardware strips it, but reading `lr` into a
  general register for address arithmetic (this routine's whole mechanism)
  carries the bit into the computation. Left in place, the *table lookup*
  address is odd, `LDRH` needs an aligned address, and a Cortex-M0 with no
  fault handler installed just hangs. Cleared for the lookup and not
  restored before the final `BX`, the *jump target* is even, `BX` reads bit
  0 as an ARM/Thumb mode switch rather than part of the address, and the CPU
  flips into ARM mode and decodes Thumb bytes as ARM. Both failure modes
  look identical from outside (a silent hang, no fault message) and only
  diverge under a live instruction trace. Any mechanism reading `lr` or `pc`
  into a register for address math needs the same clear-for-arithmetic,
  restore-for-branching discipline.
- **`CLZ`/`REVBITS`.** ARMv6-M has neither (`CLZ` is ARMv5T and Thumb-2;
  `RBIT` is ARMv7-M+), so both call a fixed software helper through the
  static helper vector (§11) rather than emitting inline code. Real cost,
  but no disadvantage against hand-written `-Os` C on the same core, which
  pays the identical software-emulation tax.
- **`MUL`** is native `MULS`, part of ARMv6-M baseline. **No `DIV`/`MOD`**
  in the ISA (isa-core.md §4.1), so nothing to synthesize.

### 10.1 Local peephole combining: the `acc` state machine

Three fusions this design needs (comparison plus branch, a producer folding
into a following `STORE`, and a producer folding a *preceding* value in as
its own operand) are one mechanism. What bounds it: **every ARMv6-M
ALU-class instruction is strictly binary**, two source operands and at most
one free destination, with no ternary form, so combining never chains more
than one bytecode instruction deep in either direction. It resolves *at* the
next instruction, never several ahead.

**The state** (`compiler/src/accstate.h`):

- **`CLEAN(reg)`**: already in a committed physical register, usually `r0`,
  sometimes an alias left by an earlier destination-fold.
- **`PENDING(shape)`**: not yet emitted. `shape` is `Imm(k)` (from `CONST`)
  or `Reg(r)` (from `LOAD` *or* `POP`, one class, since both mean "the value
  already sits in some resident register"). Classifying by *result shape*
  rather than opcode is what keeps the table small.
- **`POISONED`**: a write-back-in-place combo (`REG_REG`/`PEEK_PEEK`) just
  ran, so `acc` is clobbered by convention and nothing downstream may read
  it. `peek`/`flush` throw (assert, in the C++ port), since reading it is a
  translator or input-program bug.

**The transitions**, one bytecode instruction at a time:

| Current state | Next instruction | Action |
|---|---|---|
| `CLEAN` | a producer (`CONST`/`LOAD`/`POP`) | → `PENDING(shape)`, nothing emitted |
| `PENDING(shape)` | a compatible consumer (table below) | emit **one** instruction folding `shape` in as the left operand; peek one more token for a following `STORE` to fold as the destination → `CLEAN(dest)` |
| `PENDING(shape)` | no match in the table | **flush**: emit `shape`'s trivial materialization into `r0` → `CLEAN(r0)`; reprocess the next instruction fresh |
| `CLEAN(reg)` | an ordinary consumer | emit normally reading `reg`; still peek one token for a `STORE`-fold on the destination |

`LOAD rN; ADD rM; STORE rD` (three bytecode ops) fires both slots on one
native instruction: `LOAD` → `PENDING(Reg(rN))`, then `ADD rM` folds `rN` in
as the left operand *and* peeks `STORE rD` to fold the destination, giving
`ADDS rD, rN, rM`.

Arithmetic's two write-back-in-place addressing modes (isa-core.md §4.1 mode
2, `rN = acc ⟨op⟩ rN`, and mode 3, peek, `[tos-1] = acc ⟨op⟩ [tos-1]`) are
the same destination-fold, pre-supplied by the instruction's own combo
instead of needing the one-token peek: no new native-encoding case, since
peek's destination is just `phys(tos-1)`, the same window register a named
`rN` maps to. Both terminate the run exactly like a `STORE`-fold, `acc`
included, per the clobbering convention below.

Comparison-plus-branch fusion is a *third*, more aggressive axis rather than
a special case: the destination isn't redirected, it is eliminated, because a
`BLOCK_END`/`BR_TABLE` test consumes the flags `CMP` already set and nothing
gets materialized. ARMv6-M has no compare-and-set instruction, so
materializing a real 0/1 from a bare `CMP` costs about 4-5 instructions on
its own. Three axes, one mechanism: fold a pending producer in as an operand
(front), fold a result's destination into a following `STORE` (back), or
skip materializing a comparison's result at all when a branch is its only
consumer (back, zero-destination). Without the front and back folds,
`if`/`while` conditions cost several instructions more than assumed
elsewhere in this document; the Appendix gives counts across all four tiers.

**Classification is by native-encoding shape, not opcode**, and this is a
hard ARMv6-M constraint:

| Class | Examples | Folds a pending operand? | Folds a following `STORE`? |
|---|---|---|---|
| Commutative 3-op (reg-reg or reg-imm3) | `ADD` | yes, either shape, either side | yes |
| Order-sensitive 3-op, mirror available | comparisons | `Reg` always; `Imm` via a swapped relational op, when it fits `imm8` and the other side is a register (below) | yes, when not branch-fused (`materializeComparison`) — branch-fusion consumes the destination outright instead |
| Order-sensitive 3-op, no mirror | `SUB`/`RSUB` | `Reg` always; `Imm` unconditionally for `RSUB` (`SUBIMM`), only if `k=0` for `SUB` (`RSB`) — below | yes |
| Shift by *immediate* (3-op) | `SHL`/`SHR`/`ASR` imm form | `Reg`, as the shifted value | yes |
| Destination-only, no own-value read | `NEG`, `NOT` | never — the encoder always reads `ACC_REG` specifically, never a caller-supplied source register, so a pending value is flushed first even though the *encoding itself* (`NEGS`/`MVNS Rd,Rm`, arbitrary distinct `Rd`/`Rm`) would allow the fold; a deliberate simplification, not a hardware constraint | yes |
| Software-helper call, no native form at all | `CLZ`, `REVBITS` | never — the helper's own calling convention fixes its argument register, same practical effect as the row above for a different, harder reason | yes |
| 2-op in-place, no immediate form | `AND`, `EOR`/`XOR`, `ORR`, `MUL`, shift by *register* | never: Thumb-1's 2-operand encoding reads the destination field as an input too | never in the sense of skipping the trailing `MOV` (that never happens), but the dispatcher still threads a fold target through, so the value lands in a following `STORE`'s own register at the same cost as not folding at all |

The next-to-last row is a worse failure mode than the ordering question
below: retargeting one of those computes the wrong result immediately
rather than risking a stale value later. Pending state must always flush
before one of these, without exception.

**Operand order is the real correctness wrinkle.** `PENDING(Reg(r))` folds
safely into anything regardless of the op, since it only skips a copy and
`r` genuinely *is* what `acc` would hold, leaving the operand's position in
the encoding unchanged. `PENDING(Imm(k))` is where order bites, and one pair
looks symmetric and isn't: `RSUB` (`operand − acc`) folds a pending `Imm(k)`
cleanly via Thumb's `SUBIMM` (`Rd = Rn − imm3`, exactly `operand − k`),
while `SUB` (`acc − operand`, wanting `k − operand`) has no matching Thumb-1
form at all, `RSB`'s only immediate being literal `0`, and must flush unless
`k` is `0`.

Comparisons have a way out `SUB` doesn't. All eight relational ops are
independently available (isa-core.md §4.2), so "`k < rN`" recasts as
"`rN > k`", the same truth value with operand and immediate on the sides
Thumb's `CMP Rn,#imm8` supports, via a mechanical swap table (`LT_S`↔`GT_S`,
`LE_S`↔`GE_S`, `LT_U`↔`GT_U`, `LE_U`↔`GE_U`, `EQ`↔`EQ`, `NE`↔`NE`).
isa-core.md §7.3's complementary-comparison table is the same mechanical
flavor one layer down, at lowering time, though a negation rather than a
swap.

**Why a one-token trigger stays sound with no lookahead past it.** Not from
raw ISA physics: arithmetic's two write-back-in-place modes both read `acc`
and, bit for bit, leave it untouched, and neither is a producer nor a
`STORE`/`PUSH`, so as a hardware fact a third reader downstream could
observe the same value. What makes the run-length argument hold is a
*declared* convention this toolchain already commits to: `rtl.ts`'s combo
table marks every write-back-in-place combo (`REG_REG` and `PEEK_PEEK`
alike) as clobbering `acc` (`clobbers: ["acc"]`, with `PEEK_PEEK` also
clobbering `"tos"`), and `raise.ts`'s `binary()` acts on it verbatim
(`this.acc = undefined`) when reconstructing structured code from flat
`RtlProc` bodies. So a write-back-in-place op terminates an acc-reading run
exactly like a producer would, by the same contract `raise.ts` and the
EAST-level fragment-combining logic (`builders.ts`/`orchestrator.ts`) rely
on.

The resulting invariant: a run of zero or more consecutive `STORE`/`PUSH`
instructions can validly read the same `acc` value, terminated by the next
producer *or* a write-back-in-place op. Four consequences:

- A second consecutive `STORE`/`PUSH` reading the same value works
  unmodified, since `CLEAN`'s `reg` already points at it.
- **Rotation eviction needs no rescue instruction.** §5's window rotation
  evicting the *specific* register a fused result lives in, before a new
  producer supersedes it, looks like it would, but `accState` can only
  depend on the exact register a given `PUSH` is about to evict by directly
  referencing that same slot, and nothing else maps to that register. So the
  hazard is reachable only one `PUSH` away (a `LOAD`, or a destination-fold
  `STORE`, of exactly the window's oldest slot, immediately followed by a
  `PUSH`), not "enough consecutive `PUSH`es to rotate a slot out", and there
  the value being pushed and the value being evicted are provably identical.
  The ordinary spill-then-flush a `PUSH` already emits handles it: the
  flush's destination and the dependency's register are the same one
  (`physReg(evictedByPush) === physReg(tos)`, both reducing to the same
  `k mod WINDOW_SIZE`), so it is a same-register self-move
  `materializeShape` already elides.
- **A `case` boundary is a control-flow merge, and `accState` is one
  linear, compile-time-sequential belief threaded through a single forward
  pass.** A value left `PENDING` at the end of one case and never read again
  within it (a bare `CONST` immediately followed by `BLOCK_END`, as a
  `switch` returning a per-case constant produces) would survive past that
  case's close and be silently overwritten by the next case's translation,
  so whatever the merged code read afterward would be the *last* case's
  value rather than whichever case ran. `accState` is therefore flushed
  unconditionally at every case boundary (`blocks.ts`'s `closeBlockEnd`,
  `accstate.ts`'s `flushLive`, a no-op when already `CLEAN` and safe when
  `POISONED`, unlike a bare `flush`): cheap when nothing was pending, paid
  only in the case that needs it.
- **A fused branch's *opening* is a merge point too, in the other
  direction, and had no equivalent guard** (found via §16 item 4's own
  cross-check, after this table's "no" cell above turned out wrong and
  prompted a closer look at the fusion code around it). Branch-fusion
  never materializes the comparison's 0/1 result anywhere — only CPU flags
  carry it into the guard — so `accState` was left completely untouched
  across the fusion, still describing whatever `acc` held *before* the
  comparison ran. Per isa-core.md, a comparison is an ordinary producer and
  neither `BR_TABLE` nor a loop condition's `BLOCK_END` clobbers `acc`
  (`validate.ts`'s `accLive` stays true across both, and `vm.ts`'s own
  `BR_TABLE` handling never touches it), so a case or loop body is
  entitled to read `acc` immediately and get the comparison's own boolean
  — but the translator would silently hand back the stale pre-comparison
  operand instead, reachable by a bare `STORE` as a case/loop body's first
  instruction with nothing in between to re-establish `acc`. Fixed by
  seeding `accState` with the statically-known constant (`Imm(0)`
  entering `case[0]`/`Imm(1)` entering `case[1]` or a loop body) exactly
  when the branch is genuinely fused — never for `testAccNonzero`'s
  unfused fallback, which never replaced `acc`'s real value in the first
  place and needs no seeding (`blocks.ts`/`blocks.h`'s `Frame.fusedBoolean`
  and `closeBlockEnd`'s `fusedLoopExit` parameter). Confirmed as a genuine
  reference-interpreter/QEMU divergence (not just a theoretical gap) before
  the fix, on both the prototype and the native port, with permanent
  regression tests at every level (`acc-fusion-boundary.test.ts`,
  `test_blocks.cpp`, `test/qemu/fixtures.cpp`#22/#23).

This holds against any bytecode honoring the acc-clobbering convention, not
just this project's own `lower.ts` output. Nothing statically enforces it
yet (§16 item 2).

**Callee-side prologue as a fold.** isa-core.md §4.6's last argument arrives
in `acc`, not at `phys(argidx)`, which holds stale data (whatever the
caller's shuffle left there, never overwritten, since the convention routes
this argument through `acc`). Rather than an unconditional
`MOVS phys(argidx), r0` before Pass 1 starts, a procedure with
`arg_count ≥ 1` starts Pass 1 at `CLEAN(r0)` plus one standing obligation,
"`phys(argidx)` isn't populated yet", resolved by the same one-token
machinery:

- **`LOAD argidx`** (or any acc-destination mode reading `argidx`) as the
  next instruction is a true no-op: it asks for exactly what `CLEAN(r0)`
  holds, so nothing is emitted and the obligation is discharged free. Common
  in practice, since any small procedure reading its last argument back near
  the top hits it.
- **A register-mode operand reference to `argidx`** (mode 1/2/3) substitutes
  `r0` for that one operand in the native encoding. These reads are
  non-consuming, the value staying live at that slot for later reads, so
  this defers the obligation past one instruction rather than discharging
  it.
- **Any other producer** forces the flush (`MOVS phys(argidx), r0`) right
  there, before it overwrites `acc`: the same cost as the unconditional
  version, possibly delayed by one token instead of paid regardless of
  whether anything needed it.
- **Rotation eviction** is the hazard above with `phys(argidx)`'s stale
  contents at risk instead of a fused result's.

No lookahead deeper than the one token used everywhere else: the obligation
persists across the same `STORE`/`PUSH`-shaped run rather than resolving
within a single step.

### 10.2 Pass 2: fixup

One job. *Branch range*: Thumb conditional branches reach ±252 bytes (8-bit
signed imm×2), so a procedure whose basic blocks span further needs the
standard invert-and-long-branch idiom any Thumb-1 assembler already uses.
Still unprototyped, and the only reason a fixup pass is needed at all.

*Jump tables* need none. `BR_TABLE N>2`'s table entries resolve exactly as
ordinary branch targets do (`blocks.ts`): each slot is a deferred fixup,
patched the moment the corresponding case's `BLOCK_END` is reached in the
single forward pass, never needing to have seen anything past that point.

---

## 11. Position independence under compaction

Compaction (§8) is cheap only if no generated instruction embeds an address
depending on the code's own location. Two things satisfy that by
construction: PC-relative literal-pool loads (`LDR Rd, [PC, #imm]`) stay
correct after a `memmove`, the displacement being relative to the
moved-together load site rather than absolute; and inter-procedure `CALL`s go
through the table-base-relative dispatch mechanism (§9), never a direct
`BL`/`BLX` to another procedure's address.

**A `BL` to a fixed routine outside the arena does not work**, for two
independent reasons. Its displacement is relative to the (moving) call site
rather than the (fixed) target, so a re-emitted `BL` needs a different
immediate after every compaction. And on a typical MCU memory map flash and
SRAM sit far enough apart (`0x08000000` versus `0x20000000` on the common
Cortex-M layout, roughly 384 MB) that `BL`'s ±4 MB range plausibly cannot
reach a flash-resident routine from arena-resident code at all, independent
of compaction. Patching call sites during compaction is therefore not an
option either: there may be no valid direct `BL` encoding regardless of when
it is patched.

**The fix is a static helper vector**: a second table, link-time fixed,
flash-resident, never touched by compaction or eviction, based in `r10`
(§3). Each entry is a plain 4-byte function pointer with the Thumb bit
already set (`.thumb_func` bakes it into the symbol value), so a call site
needs nothing beyond the index:

| Index | Entry |
|---|---|
| 0 | `callHelper` |
| 1 | `returnHelperFromLr` |
| 2 | `returnHelperFromStack` |
| 3 | `returnHelperTail` |
| 4-6 | reserved: `CLZ`, `REVBITS`, extension calls |
| 7 | `returnHelperFromStackReclaim` |

The reserved slots are not padding. A small, stable table of host-provided
functions the bytecode invokes by index is the on-device form of the same
extension point `@ppl/machine`'s reference interpreter already has a hook
for (`run<E>(prog, extension?)`).

Reaching a helper is `MOV r3, r10` / `LDR r3, [r3, #idx*4]` / `BLX r3` for a
returning helper such as `CLZ` (which returns via `BX LR`, so §3's
`lr`-is-transient convention applies to it), or the same with `BX` for the
tail-jumping `callHelper`/`returnHelper*`.

Keeping the helpers in flash rather than the arena matters on its own terms:
RAM is the scarce resource, and these routines are fixed mechanism code with
nothing derived from the program being translated. A per-call-site
literal-pool word holding the helper's address is the alternative, also
compaction-safe for the same reason as any other literal-pool load, and
reasonable for a genuinely rare target; but a shared slot in an existing
table costs nothing per site and is the right default whenever the target is
reached often enough to be worth a permanent slot, which `RETURN`'s helper
clearly is at one or more sites per procedure.

**One compaction extension is needed** once code grows against the
translator's own live-checked bookkeeping (§2). The procedure whose code is
growing has no dispatch-table entry yet and is therefore invisible to
compaction as described in §8, which only covers registered procedures.
Triggering compaction from that collision has to relocate the in-progress
code too and update the translator's own base pointer and cursor for it,
mechanically identical to updating one more `code_ptr`. The block-nesting
records need no equivalent update (§2's note on why).

**Done**: `compiler/src/assembler.{h,cpp}`'s `Assembler::reserve(maxBytes,
poolEntries)` is the one seam the otherwise Runtime-agnostic translator
has into this — checked at `translateProc`'s existing per-instruction
checkpoint (`blocks.h`'s `instrMaxBytes`, the same budget `maxSpanBytes`
already used for a different reason), before the prologue stub, and (via
the same call) before a literal-pool flush. `reserve` is a plain method on
`Assembler` itself now, not a virtual call through a separate interface
(§16 item 23 retired the `ArenaRoom` abstraction this paragraph originally
described — there was only ever one implementor, hiding a `Runtime` the
host build already instantiates directly). An *attached* `Assembler`
(constructed over a real `Runtime*`) runs the ordinary
`findEvictionVictim`/`evict` loop internally, best-effort: `neededBytes`
is always a worst-case upper bound, so evicting everything resident and
still coming up short is a normal outcome, not a failure — only a later
real overflow at `emit()` is genuine, and *that* exits directly
(`Assembler::fail()` → `runtimeBail`, `runtime/dispatch_abi.cpp`) rather
than propagating a flag, since the caller (`compileProc`,
`runtime/compile_proc.cpp`) has nothing useful left to do once arena
exhaustion is real. `Runtime::evict` (`runtime/runtime_internal.h`) is
unchanged: it still takes an `inProgressLenBytes` parameter (default 0,
so every other caller's behavior is unchanged) that extends its own
tail-relocation range from `arenaCursor` to
`arenaCursor + inProgressLenBytes` — the in-progress procedure's own base
is always exactly `arenaCursor`, since nothing has bumped it yet
(`Runtime::allocate` only ever runs once, on success), so this one memmove
keeps that invariant true on the other side: `Assembler` rereads
`arenaCursor` afterward and rebases its own buffer pointer there.
`compile_proc.cpp` itself needs no scratch buffer or final `memcpy` — it
constructs its `Assembler` directly over `arenaCursor` and lets `reserve`
grow it in place, then finalizes it (flush the pool, `Runtime::allocate`,
`Runtime::markCompiled`) as `translateProc`'s own last step.

---
## 12. Report and error model

`RESOURCE_ERROR` is a failure mode this target introduces, distinct from
anything isa-core.md §9's static guarantees cover. Stack overflow shouldn't
happen, since §2's regions are sized from `validateProgram`'s own figures
and checked before use. The real runtime failure is **arena exhaustion
where a single procedure's code and its own in-progress translator
bookkeeping don't together fit** even after evicting everything (§8): one
procedure larger than the whole arena, or a still-too-fragmented arena
pre-compaction. `TRAPPED` carries the ISA's own `TRAP #code` value
unchanged.

---

## 13. Precedents

- **WebAssembly baseline compilers** (V8 Liftoff, SpiderMonkey baseline):
  the closest modern analogue. Single forward pass, no register allocator, a
  compile-time value stack mapped onto physical registers with overflow
  spilled to a real stack. They don't need code eviction or compaction under
  a hard memory ceiling.
- **Forth native-code compilers** (VFX Forth, SwiftForth, classic
  subroutine-threaded native-compiling Forths): TOS caching in registers is
  a decades-old Forth technique; caching 4 deep with circular relabeling is
  a more aggressive version.
- **SPARC register windows, Itanium rotating register files**: the same
  trick underlying §5, cyclic relabeling of physical registers to avoid data
  movement across a boundary (a call, in SPARC's case; a window slide,
  here).
- **Copy-and-patch code generation** (Xu & Kjolstad): the same fast,
  template-driven, single-pass, no-real-register-allocation philosophy,
  though this ISA is small enough to hand-emit per opcode rather than needing
  a template-extraction toolchain.
- **HotSpot CodeCache eviction, and classic overlay linkers**
  (segmented-memory mainframes, N64/PS1 overlay systems): the arena plus
  LRU-eviction plus compaction scheme is a miniaturized software-managed
  code overlay.
- **MicroPython's `emitnative` backend**: a real, shipping compact
  Thumb-emitting bytecode-to-native compiler in C, worth reading for
  emission-pattern ideas even without eviction or compaction.

---

## 14. Performance estimate

Per Generic Core opcode, native Thumb instruction count:

| Case | Instructions |
|---|---|
| ALU op, operand in window | 1 |
| ALU op, operand spilled | 2 (extra `LDR`) |
| `PUSH`/`POP`, no window boundary crossed | 0 (pure relabeling) |
| `PUSH`/`POP`, crossing the 4-deep boundary | 1 (`STR`/`LDR`) |
| `CONST`/`LOAD`/`STORE` | 1-2 |
| `if`/`if-else` (`BR_TABLE` ≤2) | 2-3 (`CMP` plus branches) with §10.1's fusion; about 7 unfused |
| `CALL` | up to about 8 for the shuffle (§6: `1+≤2` to spill the caller's resident window, `+1` to fill the callee's args, `+≤4` to reload the caller's window after return) plus the 5-7 instruction call sequence and `callHelper`'s 6; nothing at all for the shuffle in the common `argCount ≤ 1` case |
| `RETURN` | 3 (helper-vector load plus `BX`), plus the shared tail's 6 |

Expansion from one Generic Core opcode to native instructions is roughly
1-3× for arithmetic-heavy code, maybe 4-6× amortized with control flow and
calls, with no bytecode-dispatch overhead at runtime since there is no
dispatch loop left. Expect throughput within a small constant factor
(roughly 2-4×) of equivalent `-Os` C on the same core, with `CLZ`/`REVBITS`
and the call shuffle as the main structural overheads. Neither is a real
disadvantage: `-Os` C pays the first too, and the second is comparable to
any real calling convention's bookkeeping.

The Appendix's data point: a leaf loop-and-comparison procedure (14 bytecode
opcodes / 24 bytes, excluding the structural `LOOP` marker) comes in at 21
native instructions (42 bytes) with no fusion, 16 (32 bytes) with
branch-fusion only, 13 (26 bytes) once destination-folding joins, and 10 (20
bytes) with the full §10.1 scheme, which is *below* the bytecode's own
instruction count and byte size. That example has no `CALL` and never
crosses the window boundary, so it exercises neither §6's shuffle nor §5's
spill/fill.

Translation throughput should land in the few-hundred-native-instructions-
per-bytecode-instruction range for a simple table-driven emitter. On a
48-133 MHz M0/M0+ that is microseconds per instruction translated, so
low-single-digit milliseconds to JIT-compile a modest few-hundred-byte
procedure: acceptable for compile-on-first-call, assuming arena and LRU
sizing keep eviction rare.

---

## 15. Code-size estimate

Order of magnitude, `-Os`:

| Component | Rough size |
|---|---|
| Per-opcode-class emitters (arithmetic/comparison templated by ALU op; move/const; control flow; `CALL`) | 800-1500 lines C/C++ |
| Fixup pass (branch range) | 150-300 lines |
| Runtime (dispatch, arena bump-alloc, LRU evict, compaction, init) | 300-500 lines |
| **Total** | **~1500-2500 lines**, plausibly **4-10 KB flash** |

Comparable in spirit to small threaded-code Forth kernels (famously 2-6 KB).
This does more work per opcode than a threaded dispatcher, but the opcode
count and addressing-mode space are both small and heavily templated,
keeping the emitter compact.

---
## Appendix - Worked Example: `leb128_len`

Hand-translation of isa-core.md's own worked example, `arg_count = 1`.
Frame-relative `tos` starts at 1 (isa-core.md §2.5) and, since this
bytecode contains no `PUSH`/`POP`, never moves, so per §5's formula
`phys(0) = r7` (`v`) and `phys(1) = r6` (`n`) are fixed for the whole body:
no rotation, no spill, no fill. isa-core's abstract `r0 = v` is not the
physical ARM `r0` the code below assigns to `acc` (§3's naming note).

The raw bytecode is 14 opcodes / 24 bytes (`CONST`, `STORE`, `LOAD`,
`GE_U`, `BLOCK_END`, `LOAD`, `SHR`, `STORE`, `CONST`, `ADD`, `STORE`,
`BLOCK_END`, `LOAD`, `RETURN`, excluding the structural `LOOP` marker, which
has no native emission). Four tiers, each subtracting one class of the naive
translation's waste, land at 21, 16, 13 and 10 native instructions.

**Tier 0, no fusion: 21 instructions (42 bytes).** Not worth a full listing,
but it is the honest baseline. Materializing `GE_U #0x80`'s result as a real
0/1 costs `CMP` / `BHS .t` / `MOVS r0,#0` / `B .d` / `.t: MOVS r0,#1` / `.d:`
(5 instructions) followed by a separate `CMP r0,#0` / `BEQ L_exit` to branch
on it (2 more): 7 instructions where §10.1's branch-fusion takes 2. Every
other bytecode op translates one for one with no folding. 1.5×
instruction-count expansion, 1.75× byte expansion.

**Tier 1, comparison-plus-branch fusion only** (§10.1's zero-destination
axis):

```
                                    ; --- prologue (§6), not in the bytecode ---
        MOVS  r7, r0                ; v's home (r7) = incoming last arg (acc)

                                    ; CONST #1 ; STORE 1
        MOVS  r0, #1                ; acc = 1
        MOVS  r6, r0                ; n (r6) = acc

L_cond:                             ; LOOP condition block
        MOVS  r0, r7                ; LOAD 0: acc = v
        CMP   r0, #0x80             ; GE_U #0x80, fused with the
        BLO   L_exit                ; BLOCK_END below: v<0x80 → exit

L_body:                             ; LOOP body block, falls through
        MOVS  r0, r7                ; LOAD 0: acc = v
        LSRS  r0, r0, #7            ; SHR #7
        MOVS  r7, r0                ; STORE 0: v = acc
        MOVS  r0, #1                ; CONST #1
        ADDS  r0, r0, r6            ; ADD 1: acc = 1 + n
        MOVS  r6, r0                ; STORE 1: n = acc
        B     L_cond                ; BLOCK_END: back-edge

L_exit:
        MOVS  r0, r6                ; LOAD 1: acc = n (return value)
        MOV   r3, r10               ; RETURN (§7): helper vector base
        LDR   r3, [r3, #4]          ; returnHelperFromLr
        BX    r3
```

16 instructions (32 bytes). 1.14× instruction-count expansion, 1.33× byte
expansion over the 14 opcodes / 24 bytes of bytecode. The 5 instructions
saved on the comparison (7→2) recur on every loop iteration, which is why
§10.1 treats this fusion as required rather than a nice-to-have.

**Tier 2, destination-fold also applied** (§10.1's back axis: a producer's
result redirected into a following `STORE` instead of a copy):

```
        MOVS  r7, r0                ; prologue (§6)

                                    ; CONST #1 ; STORE 1: fused
        MOVS  r6, #1                ; n (r6) = 1 directly, no acc round-trip

L_cond:
        MOVS  r0, r7                ; LOAD 0
        CMP   r0, #0x80             ; GE_U #0x80, branch-fused as before
        BLO   L_exit

L_body:
        MOVS  r0, r7                ; LOAD 0 stays unfused: its own consumer
                                    ; (SHR) isn't a STORE
        LSRS  r7, r0, #7            ; SHR #7 ; STORE 0: fused

        MOVS  r0, #1                ; CONST #1 stays unfused, same reason
        ADDS  r6, r0, r6            ; ADD 1 ; STORE 1: fused
        B     L_cond

L_exit:
        MOVS  r0, r6                ; LOAD 1 stays unfused: RETURN's ABI
                                    ; needs the value in r0
        MOV   r3, r10               ; RETURN (§7), unchanged
        LDR   r3, [r3, #4]
        BX    r3
```

13 instructions (26 bytes), already below the bytecode's own 14 opcodes;
1.08× byte expansion. Only three producer-consumer pairs fuse here
(`CONST`+`STORE`, `SHR`+`STORE`, `ADD`+`STORE`); every `LOAD` stays unfused
because its next consumer reads it as an operand, not a `STORE`. That is
what tier 3 picks up.

**Tier 3, the full §10.1 state machine** (operand-fold joins
destination-fold, so every `LOAD`'s `PENDING(Reg(...))` folds forward into
whatever reads it instead of being flushed into `r0`):

```
        MOVS  r7, r0                ; prologue (§6)

                                    ; CONST #1 ; STORE 1: dest-fold
        MOVS  r6, #1

L_cond:                             ; LOAD 0 ; GE_U #0x80 ; BLOCK_END, all
                                    ; three fused: LOAD → PENDING(Reg(r7)),
                                    ; folded as CMP's left operand, then
                                    ; branch-fused, v never touches r0
        CMP   r7, #0x80
        BLO   L_exit

L_body:                             ; LOAD 0 ; SHR #7 ; STORE 0, all three
                                    ; fused: r7 folded in as SHR's source
                                    ; and as its destination
        LSRS  r7, r7, #7

                                    ; CONST #1 ; ADD 1 ; STORE 1, all three
                                    ; fused: the pending #1 folds via
                                    ; Thumb's ADDIMM form (ADD is
                                    ; commutative, so the immediate's
                                    ; original side doesn't matter), and the
                                    ; destination folds into n's register
        ADDS  r6, r6, #1
        B     L_cond

L_exit:                             ; LOAD 1 → PENDING(Reg(r6)), but
                                    ; RETURN's ABI needs the value
                                    ; specifically in r0, so flush
        MOVS  r0, r6
        MOV   r3, r10               ; RETURN (§7), unchanged
        LDR   r3, [r3, #4]
        BX    r3
```

10 instructions (20 bytes): smaller by both measures than the bytecode it
was translated from (14 opcodes / 24 bytes), while being directly
executable machine code with no interpretation loop. 0.71×
instruction-count and 0.83× byte "expansion", genuine compression. Every
fold here is one of §10.1's three axes: destination-fold (`CONST`→`n`,
`SHR`→`v`, `ADD`→`n`), operand-fold (`LOAD`→`CMP`, `LOAD`→`SHR`,
`CONST`→`ADD`), and the mandatory zero-destination branch-fusion
(`GE_U`+`BLOCK_END`). Nothing needed a chain deeper than one bytecode
instruction on either side, confirming that the binary-op ceiling bounds
this cleanly.

---

## 16. State and open questions

**On the `prototype/` citations below.** `jit-armv6m/prototype` (the TS
translation algorithm this section originally cross-checked against, and
that most items below narrate finding/fixing bugs in first) has been
retired — deleted once `jit-armv6m/compiler` reached full feature parity, per
this doc's own top-of-file status line. Every `prototype/src/*.ts`,
`prototype/test/*.test.ts`, `program.ts`/`programAbi.ts` (the whole-program
drivers), and mock-translator (`compile_proc.cpp`) reference below is
historical: it records how that item was originally found, fixed, or
verified back when both implementations existed side by side, not a
currently-navigable path. Left as-is rather than mechanically repointed,
since in most items the file-pointer and the historical claim ("verified
identically on both implementations," "TDD: red test failed with the
predicted wrong value before the fix") are the same sentence — rewriting the
pointer away would erase the record of what was actually verified. The five
files that *did* survive, relocated rather than deleted, are
`jit-armv6m/runtime/{runtime.S,runtime_host.{cpp,h},runtime_internal.h,
semihosting.cpp}` — the real dispatch/eviction runtime, formerly at
`prototype/qemu/`.

**Verified on real `qemu-system-arm`** (`prototype/test/`, historical — see
above): the §6 shuffle including a phase-misaligned window with
non-argument locals resident alongside the args (`call.test.ts`),
`stackArgs ≥ WINDOW_SIZE` (`deep-args.test.ts`), §10.1's rotation-eviction
corner (`rotation.test.ts`), `BR_TABLE N>2` including the `lr` Thumb-bit trap
(`br-table.test.ts`), the §9 dispatch ABI end to end (`abi-dispatch.test.ts`),
eviction and compaction (`eviction.test.ts`), and both §2 entry variants
(`enter-program-variants.test.ts`) — all since re-verified directly against
`test/host`/`test/qemu`, which now cover the native port's
full instruction set (`LOOP`/`BR_TABLE`/unary ops/comparisons included, `EXT`
excluded on both sides by design), including eviction/compaction and both
`RESOURCE_ERROR` sides against genuinely native-compiled code on real QEMU
(items 17/18 below).

**Open:**

1. ~~`validateProgram` exposes no max-call-depth figure~~ — **done**:
   `ProgramStats.maxCallDepth`, one memoized DFS alongside `totalDepth`
   (`validate.ts`'s `depthsOf`). Still not wired into replacing the
   caller-supplied parameter at `enter_program_on_stack`/`_split` (§1) call
   sites — that's real static data available now, just not consumed yet.
2. ~~The acc-clobbering convention is declared, not enforced~~ — **done**:
   `validate.ts`'s `walk` now threads `accLive` alongside `tos` (`BR_TABLE`
   siblings reconciled by AND; `LOOP`'s own back-edge is a documented,
   accepted gap — see its doc comment), and `vm.ts`'s `runProc` now poisons
   `acc` after `REG_REG`/`PEEK_PEEK`, matching `raise.ts`. TDD: red tests in
   `validate.test.ts`/`vm.test.ts` first.
3. ~~`LOOP`'s back-edge is a control-flow merge and hadn't been audited for
   the case-boundary-flush hazard~~ — **done**: `openLoop`/`closeBlockEnd`'s
   `loopBody` branch both now call `accState.flushLive` before the
   condition sub-block's first instruction can run, on both the
   fall-through and the back-edge. TDD: `loop-merge.test.ts`'s red test
   (compiled-once condition ignoring what the body actually left pending)
   failed with the predicted wrong value (5 instead of 1) before the fix.
4. ~~§10.1's consumer-class table is reasoned per op by hand, not derived
   mechanically~~ — **done**: cross-checked cell by cell against
   `binops.cpp`/`.ts`, `unaryops.cpp`/`.ts`, `blocks.cpp`/`.ts` and their
   `translate_proc`/`translateProc` call sites on both sides (native and
   prototype agree exactly — no row was ported incompletely). Found two
   genuine transcription errors, now fixed in the table above: comparisons
   *do* fold into a following `STORE` when not branch-fused (the table
   said "no"), and `NEG`/`NOT` never fold a pending register operand in
   either implementation despite the ISA allowing it (the table said
   `Reg`) — both dispatchers flush unconditionally, by deliberate,
   identically-worded choice on both sides, not an oversight. Chasing the
   cross-check down further surfaced a real, previously-undetected
   correctness bug one layer over from the table itself — not a
   misclassified cell, but a merge-point gap in the fusion mechanism the
   table describes — fixed on both sides with regression tests at every
   level; see this section's own new bullet above ("A fused branch's
   *opening* is a merge point too...") for the mechanism and the fix.
5. ~~Pass 2's branch-range fixup is unimplemented~~ (§10.2) — **done**, no
   Pass 2 needed: `blocks.ts`'s `emitGuardedBranch` bounds the guarded span
5. ~~Pass 2's branch-range fixup is unimplemented~~ (§10.2) — **done**, no
   Pass 2 needed: `blocks.ts`'s `emitGuardedBranch` bounds the guarded span
   *before* emitting (a cheap, deliberately loose per-opcode over-estimate,
   `maxSpanBytes`), using a bare `condBranch` only when that's proven safe
   and the invert-and-long-branch idiom otherwise. `branch-range.test.ts`
   forces the long form at both call sites (`openBrTable`, the loop-exit)
   on real QEMU.
6. ~~Block-nesting `Frame` stack isn't fixed-size~~ — **done**: fixup
   arrays became a cursor + backpatch chain, and `BlockStack`/`Frame[]`
   itself was replaced by recursion (blocks.ts/translateProc.ts).
7. ~~Thumb-bit hygiene on every dispatch-table `code_ptr`~~ — **done**:
   `runtime_internal.h`'s `Runtime` now funnels every write through
   `setCodePtr`/`slideCodePtr` (private static helpers, unconditionally
   OR-ing in bit 0), so no call site has to reason case by case about
   whether a given value already carries it; `runtime.S`'s one hand-written
   assembly site (`.Lresume+1`, not `.thumb_func`-taggable) now cross-
   references the same convention in its own comment. Verified against both
   the prototype's own QEMU suite and the native compiler's (host + QEMU,
   all 7 fixtures matching exactly).
8. ~~The native compiler covers a straight-line slice only~~ — **done for
   the prototype**: `unaryops.ts` implements `NEG`/`NOT` (single native
   instructions) and `CLZ`/`REVBITS` (per-procedure software helpers,
   reached by a local `BL`, `emitBrTableHelper`'s own precedent — ARMv6-M
   has neither instruction natively); `blocks.ts`'s `materializeComparison`
   makes a comparison usable as an ordinary value, not just a branch
   condition, gated by a one-token lookahead so existing branch-fusion is
   unaffected. `BLOCK_END`/`LOOP`/`BR_TABLE` stayed deliberately out of the
   native compiler's scope at the time (items 17/18 tracked that
   separately) — **since done**: native picked up the full instruction set
   too (`compiler/src/blocks.cpp`/`unaryops.cpp`), see items 17/18.
   `EXT` still throws, untouched, on both sides, by design.
9. ~~`r9`'s callee-saved treatment is a build convention to pin down~~ (§2)
   — **done**: AAPCS designates `r9` a platform-defined register (SB/TR)
   only when the platform needs that role; `arm-none-eabi`'s bare-metal
   environment assigns it none, so it's plain `v6` (an ordinary
   callee-saved register) and the JIT's use of it needs no special
   handling.
10. ~~Item-number drift. Several code comments cite a "§16 item N" whose content has since moved~~ 
    — **done**: swept `call.test.ts`/
    `abi-dispatch.test.ts`/`emit.ts`/`blocks.ts`/`translateProc.ts`/
    `rotation.test.ts`/`window.ts`. Most had drifted onto a concern that's
    since been fully resolved (§6's shuffle proven end to end on QEMU, the
    rotation-eviction hazard closed) rather than just mis-numbered, so
    those got reworded past-tense instead of repointed; `emit.ts`/
    `blocks.ts`'s Pass-2/branch-target citation genuinely mapped onto a
    still-open item and now cites it correctly (item 5). Item 8's own
    cross-reference (`jit-armv6m/README.md`) already lined up, untouched.
11. ~~`binops.ts`/`compiler/binops.cpp` share a gap: `PEEK_PEEK` for a
    two-op-in-place op is unimplemented on both sides~~ — **done**: `dest`
    itself as the right-hand operand, the same idiom `emitAddSubRsub`
    already used for `PEEK_PEEK` ADD/SUB/RSUB — one line, no new native
    form needed. Landed prototype-first as recorded here originally;
    `compiler/binops.cpp` has since picked up the identical one-liner
    (`emitBinaryOp`'s `TwoOpInPlace` branch, citing this item in its own
    comment) once the native port caught up to the full instruction set
    (§16's own top summary/items 17-18).
12. ~~§10.1's immediate-side mirror-table optimization isn't implemented~~
    — **done**: `blocks.ts`'s `emitComparison`, a `MIRRORED_CONDITION`
    table alongside `DIRECT_CONDITION` (comparison-fusion.test.ts,
    including a signed/unsigned-boundary case on real QEMU).
13. ~~§6's "last argument as a fold" optimization~~ / ~~14's deferred
    ISA-level version~~ (merged, one JIT-level fix) — **done**:
    `translateProc.ts`'s callee prologue now stays a pending producer
    instead of unconditionally flushing whenever a whole-body reference
    count *proves* it's safe (zero references, or exactly one and it's
    `body[0]`'s own `LOAD`) — 14's goal ("stops being paid regardless of
    whether it's ever read") achieved at the JIT level per 13, no lowering
    change needed. `last-arg-fold.test.ts` checks both correctness and a
    real code-size reduction on real QEMU.
15. ~~The procedure directory... but nothing consumes it yet~~ — **done**:
    `program.ts`/`programAbi.ts` (the two real whole-program drivers) now
    build the directory once (`encodeProgram` + `buildProcDirectory`) and
    hand each procedure's `savesLR` to both `noEvictionStrategy`/
    `abiRealStrategy` and `translateProc` itself, which use it in place of
    their own `RtlInstr[]` scan whenever supplied (`needsLRSave`'s new
    `override` parameter) — every direct unit-test call, with no directory
    at hand, keeps falling back to the scan unchanged. `bytecodeReader.ts`
    gained a `Unary` `InstrKind` (`imm = code - 90`) so the skip-pass can
    tell `CLZ`/`REVBITS` (software helpers, `savesLR`-triggering) apart
    from `NEG`/`NOT` (single instructions, not). `EXT` still throws on both
    sides, by design (item 8). The whole-program procedure-directory
    concept itself has no native equivalent — `compiler/` currently gets its
    per-procedure `argCount`s through a plain array parameter
    (`compile_proc_real.cpp`'s `calleeArgCounts`, fixtures-supplied) rather
    than a built-once directory; with the TS reference retired, a native
    directory (if one turns out to be needed as the compiler grows a real
    whole-program driver) would be designed fresh rather than ported.
16. ~~The bigger migration this sets up for is still ahead~~ — **done for
    the prototype**: `bytecodeReader.ts` gained `decodeInstr` (`RtlInstr`'s
    shape minus `EXT`'s generic payload — nothing here needs it, since
    `EXT` already only ever throws), and `translateProc.ts`'s main loop
    now decodes one instruction at a time from `encodeBody(proc.body)`'s
    raw bytes — `pc` is a byte offset advanced by each instruction's own
    decoded `.next`, never a fixed `+1`/`+2` array-index step. `blocks.ts`'s
    `maxSpanBytes`/`openBrTable`/`closeBlockEnd` (item 5's span bounding)
    moved to the same byte stream, since they walk the same body. No
    caller-visible signature changed — `program.ts`/every test file still
    hands `translateProc` an `RtlProc`, encoded internally, right where the
    old code took a `body: RtlInstr[]` and stopped indexing it. All 147
    existing tests (every one on real QEMU) passed unmodified after the
    rewrite — the prototype's own "surfaces an inconsistency cheaply" bet
    (formerly this section's workflow note) paid off with none found.
    **Since done on the native side too**: `compiler/src/translate_proc.cpp`
    decodes from the same kind of raw byte stream throughout
    (`decode_instr.h`'s `decodeInstr`), never a pre-decoded array — there
    was no separate "native port" step needed for this one, since native
    was written this way from the start rather than retrofitted.
17. ~~Eviction/compaction is untested against genuinely native-compiled
    code on real QEMU~~ — **done**: `test/qemu/main.cpp` measures
    each fixture procedure's real compiled size once (via a throwaway
    `translateProc` call) purely to size an undersized arena, then drives
    the exercise through the ordinary lazy `enterProgramSplit`/`compileProc`
    path, reading each procedure's own metadata from the real program
    bytes (item 22's `ProcSlot` directory) rather than any fixture-only
    side channel — `testEvictionThreeDeepCallChain`,
    `testEvictionCallerAndCalleeNeverCoresident`, and
    `testResourceErrorSingleProcedureLargerThanArena` (code-area side) sit
    alongside `testOnStackRejectsBeforeTouchingAnything` (stack side), so
    both `RESOURCE_ERROR` triggers are proven against real native-compiled
    code, not the mock/TS-generated path.
18. ~~Once `LOOP`/`BR_TABLE` reach the native compiler, redo the
    block-nesting-as-recursion call there too~~ — **done**:
    `compiler/src/translate_proc.cpp`'s `translateBody` recurses per
    open `LOOP`/`BR_TABLE`, each with its own stack-local `Frame`
    (`blocks.h`/`blocks.cpp`), the same recursion-not-array design item 6
    settled on the prototype side — no heap, no fixed-depth array, just
    the C call stack.
19. ~~`requiredStackBytes` still budgets for the retired mock translator~~
    — **done**: `MOCK_TRANSLATOR_ENTRY_WORST_CASE_BYTES` (92 bytes,
    unconditionally added regardless of which translator was linked in —
    a real gap even before the prototype's own mock translator was
    deleted, since native's `compile_proc_real.cpp` was already the only
    translator this figure ever needed to cover) is replaced by
    `TRANSLATOR_ENTRY_WORST_CASE_BYTES` (324 bytes: `translator_trampoline`
    +`REALIGN_ENTER` (24) + `compileProc`'s own frame (104) +
    `translateProc`'s own frame (176, `Ctx` included — item 21's site-list
    arrays no longer part of it) + `memcpy` (20), each measured via
    `-fstack-usage`/`objdump`, not guessed). Deliberately covers only the
    *fixed*, one-time cost up to `translateBody`'s own first call, never
    the recursion itself — that's item 20's own live check's job now, not
    a bigger static sum. Build-time enforcement (a per-file `-Wstack-
    usage=`/`-Werror=stack-usage=` pin, matching the now-retired mock
    translator's own analogous rule) is a reasonable follow-up, not done
    here — the fixed value itself is the correctness fix; automatically
    catching future drift in it is a separate, lower-stakes concern.
20. ~~`MAX_BLOCK_NESTING = 32` is a flat, arbitrary constant~~, carried
    unchanged from the prototype, where nesting depth should instead be
    policed *live* against however much stack margin actually remains
    (§2's "The translator's own exception" — "a real translator replaces
    this constant with the live accounting above, not a bigger constant")
    — **done**: `translateBody` (`compiler/src/translate_proc.cpp`) reads
    the real stack pointer on every recursive call and bails into
    `TranslateResult::overflowed` the moment it (minus
    `TRANSLATE_BODY_STACK_MARGIN`'s own conservative per-level allowance)
    would reach or pass `stackFloor` — a new `translateProc` parameter,
    threaded from `Runtime::liveStackFloor()` (`jit-armv6m/runtime/
    runtime_internal.h`), read fresh by `compileProc` every call rather
    than cached, since it tracks `arenaCursor` for `enter_program_on_
    stack` specifically (`arenaOverlapsStack`, a new `Runtime` field —
    that variant's own code arena genuinely shares address space with the
    C stack the recursion also runs on, "anchored at `stackLimit` itself
    and grows up from there," so its live floor is `max(stackLimit,
    arenaCursor)`; the other two entry points have no such overlap, so
    theirs is just `stackLimit` — a real, caller-supplied bound for
    `enter_program_on_stack`/`_split`, or a new, deliberately generous
    internal one (`GENEROUS_TRANSLATOR_STACK_MARGIN`, since it has no
    caller-supplied figure to work from) for plain `enter_program`, which
    had no stack protection at all before this. `depth` itself (the old
    counter) is gone — nothing needs it once the check is genuinely live.
    Host-level regression: `test_translate_proc.cpp`'s
    `DeeplyNestedButWellFormedBlocksSucceedWithNoStackFloor` (50 levels,
    deliberately past the old fixed cap, succeeds under the default "no
    limit") and `BlockNestingReportsOverflowWhenLiveStackFloorIsUnsatisfiable`
    (a floor pinned at the current `sp`, failing immediately regardless of
    depth — avoids calibrating an exact per-level byte count against this
    host build's own `-O0`, which wouldn't transfer to the real target's
    `-Os` anyway) replace the old test's "just nest 40 loops past the
    fixed cap" mechanism, which stopped being meaningful once there's no
    fixed cap to exceed.
21. ~~`CLZ`/`REVBITS`/`BR_TABLE(N>2)`'s dispatch helper are emitted fresh
    into every procedure's own arena code~~ — **done**: all three now live
    once, flash-resident, in `jit-armv6m/runtime/runtime.S`
    (`clzHelper`/`revbitsHelper`/`brTableJumpHelper`, §11's reserved slots
    4-6 in `g_helperVec`), reached by the same `MOV r3,r10 / LDR r3,[r3,
    #idx*4] / BLX r3` idiom `abi_strategy.cpp` already used for slots 0-3
    — `BLX` even for `brTableJumpHelper`'s own tail-jump, since the call
    site's own jump table sits immediately after that `BLX` and the
    routine locates it through `lr`, exactly as it did through the old
    local `BL`'s own `lr` side effect. `compiler/src/unaryops.{h,cpp}`'s
    `UnaryHelperSites`/`kMaxUnaryHelperSites` and
    `translate_proc.cpp`'s `brTableHelperSites`/`kMaxBrTableHelperSites`
    (the fixed-size site-list caps this item's own investigation found
    silently unenforced under `-DNDEBUG` on the real hardware build) are
    deleted outright, along with `blocks.cpp`'s own now-redundant
    `emitBrTableHelper` and `unaryops.cpp`'s `emitClzHelper`/
    `emitRevbitsHelper` — nothing left to track or backpatch once every
    call site reaches a fixed, permanently-known vector slot. Every
    procedure's own emitted code shrank as a direct result (no more
    trailing helper-routine copies per procedure that used one); confirmed
    via `objdump` and the native QEMU suite's own `.text` size dropping
    accordingly, with the same fixtures (CLZ/REVBITS/BR_TABLE N>2) still
    producing identical values end to end.
22. ~~`FlashProc`/`enter_program`'s real production input path was
    write-only: `Runtime::flashProcs` was assigned in `init()` and read
    nowhere, `enter_program`/`_on_stack`/`_split` had never had a real
    caller anywhere in this repo, and `compile_proc_real.cpp` (despite its
    own name) read exclusively from a fixture-only global
    (`realProcs`/`test/qemu/fixtures.cpp`) that bypassed the wire format
    entirely — a stand-in for a subsystem that had never actually been
    built, not a simplified version of one that existed. Compounding it,
    `compile_proc_real.cpp` translated into a private static scratch
    buffer and `memcpy`'d the result into the arena afterward, contradicting
    §2/§11's own description of the emitter writing directly into the arena
    with eviction able to trigger mid-translation~~ — **done**, in three
    parts:
    - **The wire envelope** (§1): `packages/machine/src/bytecode.ts` gained
      `encodeJitProgram`/`decodeJitProgram`, prepending
      `max_call_depth`/`total_depth` to an ordinary `encodeProgram` blob —
      isa-core.md's own extension point for exactly this. Porting the
      boundary-finding side of this (`decodeProcBody`) surfaced a genuine,
      previously-unnoticed bug in it, unrelated to this item's own scope
      but blocking the native port below: a `BR_TABLE` case closed via a
      bare `RETURN`/`TRAP` (legal per §8.5, and exactly what
      `blocks.cpp`'s `closeCaseViaTerminator` produces) never decremented
      the case frame's own `remaining` counter, so a non-last case using
      that shape silently corrupted the boundary-finding for everything
      after it. Fixed with regression tests
      (`packages/machine/test/bytecode.test.ts`) before porting the
      algorithm natively, so the port wouldn't inherit it.
    - **The merged procedure directory** (§9): `DispatchEntry` → `ProcSlot`
      (above). `jitc::scanProcBody` (`compiler/src/proc_scan.{h,cpp}`,
      genuinely new code, not a move) ports `decodeProcBody`'s
      boundary-finding as native recursion — one call per open
      `LOOP`/`BR_TABLE`, mirroring `translate_proc.cpp`'s own
      `translateBody` shape — rather than an explicit frame-stack array,
      checked live against a stack floor for the identical reason
      `translateBody` already is. `Runtime::init()` walks the whole
      program once, building every slot's static half; `enter_program`'s
      own three variants take one `(programBytes, programSize)` pointer
      pair instead of a `FlashProc` array plus separately-supplied
      `procCount`/`operandStackBytes`/`maxCallDepth` (§1's own updated
      signatures) — nothing here can drift out of sync with what the
      wire bytes actually contain, because there is no second copy of any
      of it to drift.
    - **Direct-arena compilation** (§11, above): the scratch buffer and
      final `memcpy` are gone.
    - Also retired: `test/qemu/fixtures.cpp`'s hand-populated `Proc`
      arrays, replaced by real encoded programs (`compiler/src/
      encode_instr.h`'s new `ProcSource`/`encodeProgram`/`encodeJitProgram`
      — a native-side mirror of the TS encoder, since fixtures need to
      produce real wire bytes without a `ts-node` round trip). Doing this
      surfaced a second, independent bug — `Fixture fixtures[] = {...}`'s
      own static initializer captured each program's `bytes`/`size` *by
      value* at static-init time, before `initFixtures()` (a `main()`-time
      function call) had ever run to fill them in, so every fixture
      silently got a null pointer and a size of 0. Fixed by capturing each
      program's own fixed address (`&f1Prog`, valid immediately, exactly
      the trick the old `Proc*`-based scheme relied on without stating it)
      and reading through it later, instead of copying the two fields out
      by value. `test/host`'s 162 tests and `test/qemu`'s 9 all pass
      against the final shape, including a `RuntimeArenaRoom` unit test
      (host) and the existing eviction/`RESOURCE_ERROR` scenarios
      (QEMU) — none needed new content, just adapting to the new call
      shapes, since nothing about *what* they exercise changed.
23. **The compiler/runtime boundary had no real seam** — `ArenaRoom`
    (`compiler/src/arena_room.h`) was a one-method interface with exactly
    one implementor, existing only to hide a `Runtime` the host build
    already instantiated directly (`test_runtime_arena.cpp`); the literal
    pool lived in `translate_proc.cpp`'s own `Ctx` while the primitives it
    drove sat in `Emitter`; pool debt was threaded by hand through three
    `blocks.h` signatures; and because the pool recovered each word by
    *re-decoding bytecode* at flush time, it needed a scan of its own
    output, which is what forced a flush before every `BR_TABLE(N>2)`
    jump table and blocked `abi_strategy.cpp`'s call record from pooling
    at all (a record is not any instruction's own immediate, so it has no
    bytecode tag to carry) — the same gap that forced `findResumeOffset`'s
    5-round fixed-point search (§9) to exist. — **done**, in two stages
    (`docs/assembler-restructuring.md` has the full plan and verification
    record):
    - **Stage 1**: `compiler/src/emitter.h` → `assembler.{h,cpp}`. One
      `Assembler` (two constructors, detached/attached) now owns the
      buffer, branch fixups (a new `Label`/`bind()` that generalizes the
      pre-existing `endFixupChain` self-linking trick — resolving a
      forward fixup always flushes the pool *first*, so a label's target
      can never land on top of pool words a flush inserts), the literal
      pool (rewritten to track stored `(site, value)` pairs instead of a
      bytecode tag — no output scan, so the forced flush before
      `BR_TABLE(N>2)`'s jump table is gone entirely), immediate-scheme
      selection (`materializeImm32`, reachable everywhere now, closing
      `shape.cpp`'s own `AccState::flush` blind spot), and — when
      attached to a real `Runtime` — arena eviction/compaction and final
      dispatch-table registration. `abi_strategy.cpp`'s call record is
      now force-pooled (`materializeImm32Pooled`): a pooled site is always
      exactly one halfword regardless of value, which makes the whole
      call sequence's own length a compile-time constant and deletes
      `findResumeOffset`'s fixed-point search outright. An attached
      `Assembler` that cannot free enough room exits directly
      (`Assembler::fail()` → `runtimeBail`) instead of latching a flag for
      a caller to check later.

      Two real bugs surfaced and were fixed while landing this, not just
      documented: `growForAttached`'s eviction loop originally failed
      immediately whenever it couldn't fully satisfy a *worst-case*
      reservation, contradicting this codebase's own stated philosophy
      that such reservations are loose over-estimates — this broke real
      eviction on QEMU until fixed to evict-best-effort-then-stop, leaving
      the genuine failure trigger at `emit()`'s own bounds check
      (root-caused on the host first, via a throwaway probe using
      `mmap(MAP_32BIT)` so a real `Runtime` and real `evict()` survive
      `uint32_t` address truncation on a 64-bit host); and eviction now
      compares against `Runtime::reserveFor`'s padded size, not the raw
      byte count, closing the `allocate()`-overruns-`arenaEnd` gap
      `reserveFor`'s own comment already warned about but nothing
      enforced. `-ffixed-r8/r9/r10/r11` — asserted in comments here and in
      `runtime.S` but absent from every Makefile — is now actually passed
      to the QEMU build.
    - **Stage 2**: purely structural. `runtime_host.cpp` split into
      `enter_program.cpp` (layer 1: the three entry points,
      `parseProgramHeader`, `requiredStackBytes`, `stackHasRoom`,
      including a new `enterProgramWithHeader` collapsing what had been
      three copies of the `runtimeStorage` VLA + `enterProgramCore` call)
      and `dispatch_abi.{h,cpp}` (layer 2: the helper vector,
      `trampolineAddr`, the ABI's own fixed-cost constants,
      `runtimeBail`'s target definition, moved out of `compile_proc.cpp`
      — renamed from `compile_proc_real.cpp`, the "real" qualifier having
      distinguished it from a mock retired by item 22). Every stale file
      reference (comments, `README.md`) swept to match.

    `TRANSLATOR_ENTRY_WORST_CASE_BYTES` re-measured via `-fstack-usage`
    against the new call chain, not guessed: 488, up from 400 —
    `compileProc`'s own frame grew (it now holds the `Assembler` object
    as a local, not a separate `RuntimeArenaRoom`), and the deeper of two
    pre-`translateBody` chains is now the last-argument-fold scan's own
    eager-flush path, not the prologue's own arena-growth call.
    `test/host`'s 168 tests and `test/qemu`'s 9 both pass against the
    final shape.
24. **Three loose ends flagged (not fixed) by item 23** — **done**:
    - `Runtime::arenaBase` was write-only (set in `init()`, read nowhere)
      — deleted. `RUNTIME_DISPATCH_TABLE_OFFSET` drops from 44 to 40
      accordingly; the `static_assert` pairing it against `Runtime`'s real
      layout (`runtime_internal.h`) is what would have caught a
      hand-arithmetic mistake here.
    - Plain `enterProgram()` — no `stackLimit`, a fixed 512-byte `static`
      arena baked into `enter_program.cpp`, a blind
      `GENEROUS_TRANSLATOR_STACK_MARGIN` instead of a real budget check —
      was an arbitrary special case among the three entry points item 23's
      Stage 2 bullet still describes as three: deleted outright, along
      with the constant and the array. A caller that wants a plain global
      arena now declares one itself and calls `enterProgramSplit` — one
      line, the same pattern `test/qemu/main.cpp`'s own
      `SplitThreeDeepCallChainSucceeds` already used. `enter_program.cpp`
      now has two entry points, not three; `test/qemu/main.cpp`'s fixture
      loop and eviction/`RESOURCE_ERROR` scenarios (previously the four
      heaviest `enterProgram()` callers) moved to a single
      file-local `enterProgramWithSharedArena` helper wrapping
      `enterProgramSplit` against one shared `static` buffer, the same
      shape the deleted function had internally, just no longer hidden
      inside the runtime.
    - `abi_strategy.cpp`'s `abiEmitReturn` had a hand-written
      `fitsImm8`-then-`MOVS`-else-`materializeImm32` branch at its deep-args
      reclaim-byte-count site, duplicating logic `materializeImm32` already
      performs internally (`imm32SynthCost` returns 1 for anything that
      fits imm8, so `Assembler` already emits the same single `MOVS`) —
      collapsed to one unconditional call.
