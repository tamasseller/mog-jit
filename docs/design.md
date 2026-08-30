# MCU JIT: Generic Core → ARMv6-M

> **Status:** design plus implementation state. Assumes
> `packages/machine/docs/isa-core.md` throughout. `jit-armv6m/compiler` (C++)
> is the sole implementation, targeting the real dispatch/eviction runtime
> (`jit-armv6m/runtime`) on real ARMv6-M hardware via `qemu-system-arm`. A TS
> prototype (`jit-armv6m/prototype`) existed earlier as a faster-iteration
> blueprint for working out the algorithm before committing it to C++; it was
> retired once the native port reached full feature parity. §16 lists
> currently-open gaps and follow-ups only.

---

## 1. Goal

One C++ entry point, callable from bare-metal firmware, that JIT-compiles
and executes Generic Core programs injected at runtime
(`jit-armv6m/src/runtime/executor.h`):

```c++
struct ProgramResult { uint32_t value; uint32_t trapped; };

class Executor
{
public:
    static Executor onStack(uint32_t stackLimit, uint32_t interruptReserve);
    static Executor split(uint32_t codeArenaBase, uint32_t codeArenaSize,
                          uint32_t stackLimit, uint32_t interruptReserve);

    ProgramResult run(const uint8_t *programBytes, uint32_t programSize,
                      uint32_t *args, uint32_t argCount);
};
```

Memory is settled once, when the `Executor` is built; `run` takes only what
is specific to one program, and hands the arena back as it found it, so the
same `Executor` runs as many programs as the caller has.

`args`/`argCount` is the entry procedure's whole argument vector, in
frame-slot order — the same shape `packages/machine`'s own
`run(program, extension, args)` takes, so the two sides of a differential
comparison are handed identical inputs. `argCount` must equal the entry
procedure's declared `arg_count` exactly; a mismatch is
`RESOURCE_PROGRAM_ENTRY_ARG_COUNT` (§12) rather than a clamp, since the
procedure reads exactly the slots it declared and the frame it reclaims on
the way out is sized from that same number. This replaced a single
`argIn` word, which could only ever express the acc-borne last argument
(§6) and left an entry procedure declaring two or more reading
uninitialized window registers — or, past four, reclaiming a frame nobody
had pushed.

No bare arena-less configuration: both take an explicit `stackLimit` and
are checked against it up front. A caller that just wants a plain global
arena declares one itself (one line, sized to what it actually needs) and
uses `Executor::split`.

### 1.1 Wire envelope and frame

`programBytes`/`programSize` is one whole serialized program:

```
jit program := max_call_depth:LEB128 total_depth:LEB128
               proc_count:LEB128 procedure{proc_count}     -- isa-core.md §5.5
               frame:u16le
```

The envelope is jit-armv6m's own
(`packages/machine/src/jit-armv6m.ts`'s `encodeJitEnvelope`), prepended to an
ordinary isa-core.md §5.5 program (`proc_count:LEB128`, then each procedure's
own `arg_count:LEB128` immediately followed by its own body). `proc_count` and
both whole-program stats come out of that envelope, not a caller-supplied
parameter — isa-core.md §5.5/§11.4's own extension point ("a procedure
header's extension fields... added when a real need appears"): a bare-metal
JIT needs `max_call_depth`/`total_depth` before it can compile a single
instruction (§2's static stack reservation, below), and `validateProgram`
already computes both, once, before the program is ever serialized.

`frame` is FNV-1a-32 over everything preceding it, XOR-folded to 16 bits, with
the contract version folded into the seed
(`0x811C9DC5 ^ PROGRAM_CONTRACT_VERSION`); `encodeJitProgram` appends it and
`Executor::run` verifies it before anything else reads a byte
(`src/runtime/program_frame.h`). Its job is the binding between the validator
and the JIT: the JIT relies on the validator to hand it only programs it can
deal with, and nothing else on the wire records that the two ever agreed.

It is deliberately not a signature and not error correction. Adversarial
substitution and transmission errors are an application's problem, and an
application that has them should solve them properly rather than have every
other deployment carry the weight. What this catches is accident: an
off-by-one length, a buffer nobody filled in, a stale pointer, a producer
built against a different contract. Those are exactly the cases that would
otherwise reach a walk that trusts its input — `parseProgramHeader` and
`loadProgram` bound themselves with `assert`, which `-DNDEBUG` strips from
every real image.

Three consequences of the shape. There is no length field: the hash is taken
over exactly `programSize - 2` bytes, so a wrong `programSize` reads the
stored value from the wrong place and fails — which is also why nothing here
ever reads past the caller's buffer. There is no version byte: skew surfaces
as a mismatch, and truncation, corruption and skew all report
`RESOURCE_PROGRAM_FRAME` because all three mean the same thing. And the hash
is folded, not truncated: FNV's prime is odd, so bit 0 survives every
multiply and the raw low half is little more than a parity of the input.

Cost on `-mcpu=cortex-m0 -Os`: 68 bytes, inlined into `Executor::run`.

`trapped` is 0 for a normal return, nonzero for a `TRAP` code (isa-core.md
§4.5) propagated out or a resource error, whose own `value` names which one
(§12). None of these return until the program terminates.

Constraints: the target is ARMv6-M (Cortex-M0/M0+ baseline Thumb, no
Thumb-2). The code arena may be too small to hold every procedure's
compiled code at once, so execution must still proceed, compiling and
evicting on demand. Translation is single-pass and as context-free as
possible, followed by one branch-range fixup pass. Generated code is
position-independent (no embedded absolute addresses), so eviction and
compaction never need a relocation pass.

`Executor` (`jit-armv6m/src/runtime/executor.h`) owns the `CodeArena` and
nothing else, so one of them serves any number of programs; `Executor::run`
takes one encoded blob plus its arguments, places that program's `Runtime`
in its own frame and hands both that and the arena back empty on return.
`Executor::onStack`/`Executor::split` are its two named configurations.

`Runtime` is the per-program state, and it is two things rather than one: a
`DispatchTable` (`dispatch_table.h`) it owns, saying where each procedure's
code ended up, and a reference to the `Executor`'s `CodeArena`
(`code_arena.h`), saying where compiled code goes and how far down the stack
may come to meet it. Neither half needs the other. `Runtime` itself carries
only what does — eviction, which picks an LRU victim from the table and
compacts the arena, and the stack floor, whose check has to bail out through
the table's sentinel.

The arena outliving the program is what makes an `Executor` reusable, so a
run hands back exactly what it found: `CodeArena::Excursion` saves the cursor
and the live stack floor and restores both, the same save-and-restore shape
the per-level stack guard uses. Everything else — end, `stackLimit`, the
interrupt reserve — was settled when the `Executor` was built.

`Executor::onStack` makes the current C stack the whole work area:
`Runtime`, its dispatch table, the operand stack and the compiled-code
arena all come out of it. It takes no arena size — the arena is exactly
`[stackLimit, codeLimit)`, so a program gets every byte its own reservation
(§2) leaves over, and a caller has no second number to get wrong.
`Executor::split` puts the arena in
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
    codeLimit ......................... = SP(entry) − requiredStackBytes
    helper / translator frame           dynamic, self-checked
    operand stack                       call/return records interleaved
        JIT frame J ... JIT frame 1
    dispatch table + sentinel slot      fixed
    Executor::run frame                 MSP → PSP switch happens here
    app stack frame K ... frame 1       (MSP)
(higher addresses)
```

`Executor::run` computes a hard ceiling for compiled code once, at entry:
`codeLimit = SP(at entry) − requiredStackBytes`. Under
`Executor::onStack` that ceiling *is* the arena's end, so nothing is
left stranded between the two. Every term of
`requiredStackBytes` (`jit-armv6m/src/runtime/executor.cpp`, summing the
fixed-cost constants `jit-armv6m/src/runtime/stack_budget.h` declares) is
derived from the program's own wire envelope (§1) or a measured constant:

| Term | Source |
|---|---|
| `Runtime` plus its dispatch table | `sizeof(Runtime) + (procCount+1)·sizeof(ProcSlot)` |
| Operand stack | `operandStackBytes` = `totalDepth · 4`, from the program's own envelope |
| Live call/return records | `maxCallDepth · CALL_RECORD_BYTES`, `maxCallDepth` from the same envelope |
| Fixed implementation overhead | `ENTER_DISPATCH_FIXED_BYTES` + `EXECUTOR_RUN_FRAME_BYTES` |
| Deepest transient | the larger of `TRANSLATOR_ENTRY_WORST_CASE_BYTES` and `extHelperStackBytes()` + `EXT_THUNK_STACK_BYTES` — see below |
| Exception entry | `interruptReserve` |

The last row is a maximum, not a sum: a translation and an extension helper
are the two things that sit on top of whatever depth the compiled code has
itself reached, and they cannot coexist. Nothing executes while the
translator runs, and `proc_scan.cpp` refuses any extension op shaped like a
call (§11.2), so no helper can reach a dispatch. Lifting that restriction
turns the maximum back into a sum.

Because those are static, ordinary code growth needs no runtime check
against the stack side: the reservation makes a collision impossible by
construction, and hitting `codeLimit` is the same evict-and-compact trigger
§8 already has, anchored to a precomputed line rather than a detected
collision. The check runs once, before any of that memory is touched,
against a caller-supplied `stackLimit` (the lowest address the excursion
must never reach); on failure `RESOURCE_EXHAUSTED_STACK_BUDGET` comes back
with nothing set up.

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
`RESOURCE_EXHAUSTED_TRANSLATOR_STACK` once there is no room for both the
code still to emit and this procedure's nesting depth. `alloca` overflow is undefined behavior
rather than graceful failure, so the check happens before growing. The
translator's *fixed* prologue footprint, paid on entry before it can check
anything, belongs in the static helper-stack term above; only depth beyond
that floor is policed live.

This is sound only because nothing the translator holds across the pass (a
block-nesting record's jump-table base, a pending branch fixup) is an
absolute arena address. Every offset is relative to the current procedure's
own start, the same position-independence §11 requires of emitted code, so
compaction sliding the code region never invalidates a still-open record.

`TRANSLATOR_ENTRY_WORST_CASE_BYTES` (`jit-armv6m/runtime/dispatch_abi.h`)
is re-measured via `-fstack-usage` whenever the real translator's own call
chain changes shape, itemized per function on the real path, never
guessed. Build-time enforcement (a per-file `-Wstack-usage=`/
`-Werror=stack-usage=` pin) remains a reasonable
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
| `r3` | Entry ABI: jump target | Dead the instant control lands, which is what lets `returnHelperTail` borrow it for the LRU stamp (§9). |
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
is no smarter recovery at that point: `RESOURCE_EXHAUSTED_ARENA` (§12). Worst case the
loop is O(n²) in the resident-procedure count, fine given how small `n` is
on any real embedded target and that this is already the rare, expensive
path.

**Compaction** slides surviving procedures' code down to close an evicted
one's gap, then updates only the dispatch table's `code_ptr` entries:
O(procedure count), not O(code size). A procedure's code length comes from
neighbors rather than a stored field (`occupiedSizeOf`,
`jit-armv6m/src/runtime/runtime.h`): compaction keeps every resident
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
    uint32_t last_used;   // mutable — LRU tick, stamped by callHelper/returnHelperTail (§9)
    uint32_t body_ptr;    // static — absolute flash address of this procedure's own body_bytes
    uint32_t static_info; // static, packed: bit31 needs_lr_save; bits[30:20] arg_count; bits[19:0] body_bytes
};
```

No `state` field: "not resident" is `code_ptr == translator_trampoline`. No
doubly-linked LRU list: a linked list needs 4-6 pointer writes to unlink and
relink on *every touch*, the hot path of every call and return, where a
timestamp needs one store — and one place to put it, since both routines
that reach a slot at all already hold its address — and eviction, the rare
heavy path, absorbs a linear minimum scan instead. No `size` field (§8 derives it); the bytes it
would have cost fold into widening `last_used` to a full word, which at 32
bits doesn't realistically wrap in an embedded system's lifetime, so the
scan is a plain comparison.

The static half (`body_ptr`/`static_info`) is what makes this table
double as the whole-program procedure directory: `Executor::run`'s one-time
wire-format walk
(`Runtime::loadProgram`, `jit-armv6m/src/runtime/runtime.cpp`) fills it in for
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
MOV   r2, r11               ; low-mirror the LRU tick (STR has no hi form)
STR   r2, [r1, #4]          ; entry.last_used = old tick
ADDS  r2, #1
MOV   r11, r2               ; publish
MOVS  r2, #1                ; offset+1 = 1, hardwired
BX    r3
```

The LRU stamp lives here, and in `returnHelperTail`, rather than in the
per-procedure stub it used to head. It is byte-for-byte identical in every
copy, and both routines already hold `slotAddr` with a low register dead in
hand — `r2` between the `LSLS` and the `MOVS` here, `r3` before the
`code_ptr` load there. One flash copy each instead of 8 bytes of arena per
resident procedure.

`returnHelperTail` stamps *unconditionally*, the sentinel included. Guarding
it would put a branch on the one path `slots[-1]` exists to keep free —
return from the entry procedure — to protect a word nothing reads: the
eviction scan runs over `slot(i) == slots[i+1]`, so the sentinel is never a
candidate. The cost is paid instead in §18.1, where the extension scratch
gives up its first word to stay clear of `last_used`.

**The per-procedure prologue stub** is the first two instructions of every
compiled procedure (`emitPrologueStub`), copied into the arena ahead of the
body, which is why it is emitted as data the translator can copy rather than
reached by name:

```
ADD  r2, r2, pc             ; r2 = (offset+1) + (this instruction's address + 4)
BX   r2                     ; a real branch, never a write to pc
; ... procedure body starts here
```

Two instructions is all that is left, and all that can be: `ADD` reads *this
procedure's own* pc, so its position is the datum. Everything that used to
precede it was position-independent and moved to the helpers above.

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
the sentinel slot, marshals the entry procedure's arguments, then
tail-branches into `callHelper` with a boot record of `proc_idx = 0xffff`
and `Q_idx = 0`.

The marshalling makes `enter_dispatch` the entry procedure's *caller*: a
compiled prologue and epilogue draw no distinction between arriving here
and arriving from a compiled `CALL`, so the arguments have to land exactly
where `spillForCall`/`fillCalleeArgs` would have left them (§6) — out-of-
window slots pushed ascending so slot 0 is furthest from `sp`, slots
`N-4..N-2` in their `physReg` window registers, slot `N-1` in acc. Which
value goes in which register depends on `N mod WINDOW_SIZE`, a four-way
branch in assembly and one array index in C, so the split is deliberate:
`runtime/entry_args.h` computes a small descriptor (using the translator's
own `physReg`, not a second copy of the formula) and the asm is a push loop
plus five loads. It sits after the `savedSp` store, because `trapHelper`
and `runtimeBail` both restore `sp` from there and must discard the pushed
arguments along with everything else. The result comes back as a `uint64_t`
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
  `CMP` plus a conditional branch, no table — *when* the dispatch value is a
  fused comparison's own 0/1, which is what §7.1's "the default is
  unreachable for `if-else`" is about. Unfused, `acc` is an arbitrary u32
  and §4.5's `acc ≥ N` outcome is real, so `N == 2` needs three ways out
  (`CMP #1` plus `BHI` past both arms, then `BEQ` to the second): folding
  `acc ≥ 2` into `case[1]` ran the else-arm where the ISA runs neither arm.
  `N > 2`
  (`compiler/src/blocks.cpp`'s `openBrTableJump`) needs a literal-pool jump
  table plus a computed `BX`, but not one dispatch routine per site: one
  flash-resident copy for the whole program (§11's reserved slot 6,
  `brTableJumpHelper`, `jit-armv6m/runtime/runtime.S`),
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
  direction.** Branch-fusion never materializes the comparison's 0/1
  result anywhere — only CPU flags carry it into the guard — so `accState`
  is left completely untouched across the fusion unless something
  re-establishes it. `accState` is seeded with the statically-known
  constant (`Imm(0)` entering `case[0]`/`Imm(1)` entering `case[1]` or a
  loop body) exactly when the branch is genuinely fused — never for
  `testAccNonzero`'s unfused fallback, which flushes for real before the
  branch and needs no seeding.

isa-core.md §8.7 makes this a validation error generally, and the rule is
unconditional in both directions: a CFG split (`BR_TABLE`/`LOOP`) clobbers
`acc` on entry to every successor, *and* `acc` is dead after the whole
construct however its cases ended. `validate.ts`/`vm.ts` reject both,
rather than relying on this one seeded case alone.

The leaving direction is worth stating separately, because it is what makes
this backend's fusion legal at all: `BR_TABLE`'s implicit default (§4.5,
`acc ≥ N` runs no case) is the one edge in the ISA that holds no
instructions, so nothing can establish a value on it and no merge after a
dispatch can be given one on every edge. That is why `translateIfThen`,
`translateIfThenElse` and `translateSwitch` all `poison()` at their merge
point unconditionally, with no fixup on the skip edge — a fixup that
materialized the fused comparison's 0/1 there was tried, and isa-rationale
.md's own reasoning for the rule rules it out: the pattern is invalid input,
not something a backend has to compile.

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
| 4-6 | `clzHelper`, `revbitsHelper`, `brTableJumpHelper` |
| 7 | `returnHelperFromStackReclaim` |
| 8 | `trapHelper` |

Slot 8 is the odd one out: every other entry either returns to its caller
or tail-jumps into another procedure's code, while `trapHelper` ends the
whole excursion. A bytecode `TRAP` (isa-core.md §4.5) unwinds every frame
between itself and the entry procedure, which it does the same way
`runtimeBail` handles a resource error — restore `Runtime::savedSp`, jump
to the sentinel landing `enterDispatch` parked below the dispatch table,
carrying `LANDING_TRAP` in `r2` and the trap code in `r0`. One
`mov sp, savedSp` subsumes every window spill, pushed call record and
out-of-window argument block in one instruction, which is why a `TRAP`
call site emits no teardown at all — no `discardWindow`, no record
retrieval, no reclaim — and ends up shorter than a `RETURN`.

Slots 4-6 started as reserved space, and a stable table of host-provided
functions the bytecode invokes by index remains the on-device form of the
same extension point `@ppl/machine`'s reference interpreter already has a
hook for (`run<E>(prog, extension?)`) — the next such addition takes slot
9.

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
`Assembler` itself now, not a virtual call through a separate interface —
the `ArenaRoom` abstraction this paragraph once described had only ever
one implementor, hiding a `Runtime` the host build already instantiates
directly, so it was retired. An *attached* `Assembler`
(constructed over a real `Runtime*`) runs the ordinary
`findEvictionVictim`/`evict` loop internally, best-effort: `neededBytes`
is always a worst-case upper bound, so evicting everything resident and
still coming up short is a normal outcome, not a failure — only a later
real overflow at `emit()` is genuine, and *that* exits directly
(`Assembler::fail(RESOURCE_EXHAUSTED_ARENA)` → `runtimeBail`,
`runtime/dispatch_abi.cpp`) rather than propagating a flag, since the caller (`compileProc`,
`runtime/compile_proc.cpp`) has nothing useful left to do once arena
exhaustion is real. `Runtime::evict` (`src/runtime/runtime.cpp`) is
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
`markCompiled` stamps the live tick rather than zeroing `last_used`:
`callHelper` already stamped this slot on the way to the trampoline, and
zeroing would present a procedure that was just paid for as the oldest thing
in the arena — the next victim, evicted before running once.

---
## 12. Report and error model

`LANDING_RESOURCE_ERROR` is a failure mode this target introduces, distinct
from anything isa-core.md §9's static guarantees cover. The tag is the
discriminator; `value` names which of fourteen ways it happened, as a
`RESOURCE_*` code from `runtime/resource_codes.h` (`0x5245` signature, class
nibble, reason nibble, low byte reserved for a future detail payload).
`TRAPPED` carries the ISA's own `TRAP #code` value unchanged.

Three classes, split by what the caller can do about it — the only part
worth branching on. `PROGRAM`: the input is out of contract, fix the
program. `EXHAUSTED`: genuinely out of room, give it more memory. `LIMIT`:
unencodable at any size, this backend cannot compile it at all.

| code | value | site | predicate |
|---|---|---|---|
| `RESOURCE_PROGRAM_NO_PROCS` | `0x52451100` | `executor.cpp` `Executor::run` | `proc_count == 0` |
| `RESOURCE_PROGRAM_BODY_UNTERMINATED` | `0x52451200` | `Runtime::init` via `proc_scan.cpp`'s `scanBody` | ran off the blob with a block still open |
| `RESOURCE_PROGRAM_CALLEE_RANGE` | `0x52451300` | `translate_proc.cpp` `processNonControl` | `calleeIndex >= procCount` |
| `RESOURCE_PROGRAM_FRAME` | `0x52451400` | `executor.cpp` `Executor::run` | §1.1's frame does not verify: truncated, corrupt, or another contract version |
| `RESOURCE_PROGRAM_ENTRY_ARG_COUNT` | `0x52451500` | `executor.cpp` `Executor::run` | `argCount != slot(0).argCount()` |
| `RESOURCE_PROGRAM_ENTRY_DEPTH` | `0x52451600` | `executor.cpp` `Executor::run` | the entry procedure's out-of-window args exceed `total_depth` |
| `RESOURCE_PROGRAM_EXT_UNKNOWN` | `0x52451700` | `Runtime::init` via `proc_scan.cpp`'s `scanBody` | a wire byte past `LAST_CORE_OPCODE`: the extension range (§11) or a reserved code (§5.3) |
| `RESOURCE_PROGRAM_EXT_UNSUPPORTED` | `0x52451800` | `Runtime::init`, and `translate_proc.cpp`'s `EXT` arm | a declaration asking for a capability this core doesn't implement, or one the emitted code then contradicts (halfword overrun, `tosDelta` mismatch) |
| `RESOURCE_PROGRAM_RESERVED_OPCODE` | `0x52451a00` | `Runtime::init` via `proc_scan.cpp`'s `scanBody` | one of the four core codes §5.3 reserves but hasn't assigned (124-127) |
| `RESOURCE_EXHAUSTED_ARENA` | `0x52452100` | `assembler.cpp` `emit` | buffer full and nothing left to evict (§8) |
| `RESOURCE_EXHAUSTED_STACK_BUDGET` | `0x52452200` | `executor.cpp`, both variants | the up-front §2 check; nothing was touched |
| `RESOURCE_EXHAUSTED_TRANSLATOR_STACK` | `0x52452300` | `translate_proc.cpp` `checkStackFloor` | translator recursion reached the live floor |
| `RESOURCE_EXHAUSTED_SCAN_STACK` | `0x52452400` | `proc_scan.cpp` `scanBody` | ditto, in the directory pre-pass |
| `RESOURCE_LIMIT_WINDOW_RECLAIM` | `0x52453100` | `window.cpp` `discardWindow`/`restoreWindow` | reclaim past `Uoff<2,7>` — TOS depth over 131 |
| `RESOURCE_LIMIT_SPILL_OFFSET` | `0x52453200` | `translate_proc.cpp` `spillImm` | spill slot past `Uoff<2,8>` |
| `RESOURCE_LIMIT_BRANCH_RANGE` | `0x52453300` | `assembler.cpp` `patchBranch` | fixup past `Ioff<1,8>`/`Ioff<1,11>` |
| `RESOURCE_LIMIT_LOOP_BACK_EDGE` | `0x52453400` | `translate_proc.cpp` `translateLoop` | back-edge past `Ioff<1,11>` |
| `RESOURCE_LIMIT_ARG_COUNT` | `0x52453500` | `Runtime::init` | `arg_count` over `ProcSlot`'s field width |
| `RESOURCE_LIMIT_BODY_BYTES` | `0x52453600` | `Runtime::init` | body size over `ProcSlot`'s field width |

Two things this deliberately does not cover. Stack overflow proper still
shouldn't happen — §2's regions are sized from `validateProgram`'s own
figures and checked before use; the two `EXHAUSTED_*_STACK` codes are the
*translator's own* C recursion against a live floor, not the compiled
program's operand stack. And malformed wire bytes stay asserted rather
than reported, the convention `decode_instr.h` and `proc_scan.h` already
document: `PROGRAM_BODY_UNTERMINATED`, `PROGRAM_CALLEE_RANGE` and
`PROGRAM_EXT_UNKNOWN` are the three checks that exist because the walk
needs them anyway, not the start of a validating decoder. What used to be
unreportable here is the truncated envelope: `parseProgramHeader` runs
before there is anywhere to report to, so §1.1's frame is checked ahead of
it and `PROGRAM_FRAME` is what a short or wrong buffer comes back as.

`PROGRAM_EXT_UNKNOWN` is the one of those three that is not really about
malformedness: a byte in the extension range is plausibly the *right*
program against an image built without that extension registered, so it is
reported rather than asserted. With an extension registered it means that
extension declined the byte; §18 has the rest of the seam. It is also the only place that byte is
stopped. `decodeInstr` merely asserts, and both the QEMU suite and
`fuzz/qemu_exec` build `-DNDEBUG`, so before `scanBody` gained its own
check a body byte of `0x80` decoded as `CONST 20` on real hardware and
silently reinterpreted the rest of the instruction stream. The check sits
in the pre-pass because that walk already decodes every instruction of
every procedure before anything is translated, which is what makes one
gate sufficient.

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

## 16. Stack safety

Consolidated strategy for what were three separate gaps (a missing
build-time check, a stale hand-derived constant, and a live guard that
only ran at recursion depth 0) into one: the real recursion
(`translateLoop`/`translateIfThen`/`translateIfThenElse`/`translateSwitch`
→ `processUntilTerminator` → `processNonTerminators`, back into those) now
checks live SP at every one of those four entry points via a shared
`checkStackFloor` (`translate_proc.cpp`) — not just once, at
`translateBody`'s own depth 0 — closing the actual gap directly rather
than trying to keep a separate static margin in sync with it.
`proc_scan.cpp`'s own pre-compilation `scanBody` recursion keeps its own
`SCAN_STACK_MARGIN`, now documented as bounding only its own (lighter)
frame — it was never a valid proxy for the real translator's heavier one,
and no longer needs to be, since `checkStackFloor` covers that directly.

`TRANSLATOR_ENTRY_WORST_CASE_BYTES` (`stack_budget.h`) is re-derived from
GCC's own call graph rather than hand-traced: 512, being 24 hand-maintained
asm bytes plus a measured `TRANSLATOR_ENTRY_CPP_BYTES` —
`translateLoop`/`translateIfThen`/`translateIfThenElse`/`translateSwitch`/
`translateBody` are confirmed fully inlined into `translateProc` at `-Os`
and don't appear as separate frames to budget.

`test/qemu/Makefile`'s `stack-usage-check` target runs `tools/stack-margin.ts`
over GCC's `-fcallgraph-info` output: it takes a signature filter, walks the
subgraph below every matching function, cuts recursion back-edges to give the
per-level cost, and computes the margin from the measured frames. It rejects
anything it cannot bound exactly — dynamic frames, dynamic objects, indirect
calls, calls with no definition in the graph. `--guard` names the functions that
re-check the floor at entry — those carry a `GUARDED_` name prefix, so the
pattern cannot match an unguarded function by accident. One rule covers them,
applied identically to the root and to every guarded callee: a guarded function
contributes its own frame and nothing below it. Recursion is cut only at a
guarded function; a cycle anywhere else is rejected outright rather than
silently cut.

The check inside the guard runs after that function's prologue, so at the check
sp is already `CFA - frame` and the guarantee is `CFA - frame - M > floor`,
while the deepest point before the next check is `CFA - M`. Sound, with the
function's own frame as slack. Paying that one frame is what buys a rule with no
assumption about prologue placement in it. It holds without case analysis
because exactly one function is guarded — `GUARDED_processUntilTerminator`, the
cut vertex every translator recursion cycle passes through — so the frame in the
guarantee and the frame charged at the cut are the same number; `--max` turns the report into a gate, and the Makefile feeds it
`TRANSLATE_LEVEL_STACK_MARGIN` read straight out of the source. A per-file
`-Werror=stack-usage=512` backstops the one thing a static byte-count
comparison can't catch on its own: an accidental unbounded `alloca`/VLA
in a tracked function.

`test/qemu/stack_paint.cpp` corroborates all of the above empirically on
real QEMU — the only independent check available on this hardware
(Cortex-M0 has no MPU, no `MSPLIM`, and `vectors.S` never switches to
PSP, so this is one flat MSP stack with nothing hardware-enforced to
catch an overflow; `linker.ld`'s own header already documents a real past
incident of exactly this). Painting the stack region with a sentinel
before any test runs, then measuring how far it's overwritten afterward,
caught a real bug during development: at `-Os`, GCC rewrote the fill loop
into a `memset` call, which needed its own stack frame at the exact
moment the paint region's own upper bound was the pre-call `sp` — the
call overwrote its own return address with the sentinel and jumped into
`0xAAAAAAAA` on return. Fixed with `volatile` (blocks the rewrite
entirely) plus a fixed safety margin below the measured `sp` regardless
(defense in depth) — see `stack_paint.cpp`'s own comment for the full
diagnosis via `qemu-system-arm -d exec`.

---

## 17. Differential fuzzing

`fuzz/` is two harnesses, because they catch disjoint bug classes and each
is structurally blind to the other's.

`fuzz/harness.cpp` runs the real translator on the host, under ASan/UBSan
with asserts live, on whole programs a `validateProgram` gate has already
approved (over a socket to `fuzz/oracle_server.ts`, so Node starts once
rather than per test case). Each input is translated twice — once with a
detached `Assembler`, then again with attached ones against a small real
arena at a fixed low address, which is what reaches `Runtime::allocate`/
`findEvictionVictim`/`evict`'s compaction memmove and `finalize`'s dispatch
registration. It finds crashes, and nothing else: it never executes what it
emitted.

`fuzz/qemu_exec/` closes exactly that gap, and needs no new emulator —
§16's own `qemu-system-arm` setup already runs this translator plus the
real, unmodified `runtime/`. A batch of programs is loaded straight into
guest flash (`-device loader`; semihosting file I/O was tried first and
`SYS_OPEN` returns -1 on this machine), each is run through the real
`Executor::split`, and the results are diffed against `@ppl/machine`'s
reference VM. One boot per batch, so the emulator's startup cost amortizes
away.

**What the execution half found that the crash half could not.** Every one
of these produced no crash, no assert and no `RESOURCE_ERROR` — just the
wrong number, or no answer at all:

- A register-form shift emitted a bare `LSLS/LSRS/ASRS Rd, Rm`, and
  ARMv6-M's register form reads `Rm[7:0]` where isa-core.md then masked
  the amount to five bits. `2784` (`87*32`) meant "shift by 0" to the ISA
  and "shift by 224" to the hardware. Fixed in the ISA rather than the
  translator: §4.1 no longer defines a shift by 32 or more, so the bare
  register-form shift stands and the codegen is unchanged. See
  fuzzing-campaign.md finding 5 for why the alternative — masking to five
  bits with `LSLS #27`/`LSRS #27`, ARMv6-M having no AND-with-immediate —
  was not worth two extra instructions on every dynamic shift.
- `emitGuardedBranch`'s long form patched its "not taken" edge to
  *branch + 4 bytes*, which is precisely where the unconditional branch's
  own literal-pool flush lands — so that edge jumped into pool data and
  executed it. Now resolved through `Label`/`bind()`, the one flush-safe
  way to mean "wherever we are now".
- `BR_TABLE 2`'s unfused form folded `acc ≥ 2` into `case[1]`, running the
  else-arm where §4.5 runs neither arm (§10).
- A `PUSH` inside a `LOOP`'s *condition* sub-block never had its TOS
  surplus dropped at that block's own `BLOCK_END` (§8.1 drops it like any
  other, and every `BR_TABLE` case already did) — so `sp` and the window
  model diverged from the loop onward and the return sequence reclaimed the
  wrong amount, returning through a corrupted call record.
- Two where the *reference* side was wrong, not the JIT: `runProc` seeded a
  callee's `acc` to 0 where §4.6 puts the callee's last argument there, and
  `validateProgram` treated a zero-argument procedure's entry `acc` as live
  when nothing establishes it (§4.6 conditions that on `N >= 1`), so
  `{argCount: 0, body: [RETURN]}` validated with no defined result — the VM
  returning 0 where the emitted code returned the caller's leftover
  accumulator. Fixing the second needed `ExtOpEffect.writesAcc`, since an
  extension op that assigns `acc` had no way to say so.

- The structural one: `TRAP` did not unwind. It compiled to a normal
  return carrying `0x80000000 | code` — correct for the entry procedure,
  silently wrong for a nested one, whose caller read the sentinel as an
  ordinary return value and carried on. Now helper slot 8, `trapHelper`
  (§11), doing for a bytecode trap what `runtimeBail` does for a resource
  error: restore `Runtime::savedSp`, jump to the sentinel landing carrying
  `LANDING_TRAP`. That retired the sentinel encoding altogether —
  `ProgramResult.trapped` distinguishes the three outcomes now, so a
  program may return any `uint32_t` and trap with any code
  (docs/target-profile.md).

The `fuzz/seeds` corpus keeps a regression seed for each fixed finding, so
`qemu_exec.ts seeds` is a standing check on all of them.

---

## 18. Extension mechanism

isa-core.md §11 gives wire bytes **≥128** to one registered extension. The
core never interprets them. Note the two boundaries are not the same one,
which is the easy mistake here:

| bytes | owner | a program using one |
|---|---|---|
| 0-123 | core, assigned (§5.2) | translated |
| 124-127 | **core, reserved** (§5.3) — four codes it hasn't assigned yet | `RESERVED_OPCODE`: wants a newer core. Never offered to an extension, which would otherwise let one squat on core opcode space. |
| ≥128 | the registered extension (§5.1, §11) | `EXT_UNKNOWN` if nothing claims it |

The core needs exactly two things per extension opcode:

1. the **byte length**, so `proc_scan.cpp`'s body-boundary walk and
   `blocks.cpp`'s branch-span walk can step over it;
2. the **declared effect** (§11.2), so `needsLRSave` and the span budget
   stay right without knowing semantics.

Both come from `extDecode`, packed into one 32-bit word carried in
`Instr`'s existing union. That is the load-bearing choice:
`instrMaxBytes(const Instr&)` and `triggersLRSave(const Instr&)` keep their
signatures and become bitfield reads, so `maxSpanBytes` — which re-walks
every instruction once per enclosing nesting level — pays a shift rather
than an indirect call per level, `sizeof(Instr)` stays 8 (a `static_assert`
holds it there, keeping `DecodedInstr` off the sret path where the margin
against `SCAN_STACK_MARGIN` is ~56 bytes), and the span budget, the
prologue's `lr` decision and codegen cannot see different answers.

**Operands never reach the core.** They are literal constants (§11.3), so
the extension re-reads them from the wire when it emits. The core carries a
length and an effect; that is the whole coupling. It also means
`encode_instr.cpp` structurally cannot rebuild an extension instruction
from an `Instr`, so a fixture needing one splices its own bytes.

**One gate, not three.** `Runtime::init`'s directory walk already decodes
every instruction of every procedure before anything is translated, so the
extension is consulted there and `decodeInstr` trusts the result. Two things
the core checks rather than trusts, because both would turn a bad extension
into a hang or an overrun instead of a diagnostic: a claimed length of zero
(no forward progress), and one running past `bytesLen`.

**What the declaration carries** is only what the core cannot derive at the
site: `NEEDS_LR` and `halfwords` because the pre-pass consumes them before
any `Assembler` exists, `poolWords` because only the extension knows how
many literals an `ATOMIC` block will add, `CALL_SHAPED` because
`Executor::run`'s budget takes the *max* of a translation and an extension
helper rather than their sum, and `tosDelta` — which is not a driver but a
postcondition, checked against the real `window.tos` after emit, since the
wire's `total_depth` was validated against it and nothing re-derives it
here. Everything else an earlier draft declared is derivable or belongs
upstream: `maxTransient` is already folded into `total_depth` by
`validate.ts`, `terminates` is honoured in exactly one of three places on
the TS side, and the opcode byte is at `site.opcode()`.

Two rejections remain, at `init` with `EXT_UNSUPPORTED`: call-shaped, and a
net TOS push (not representable on the TS side either — `extension.ts`).

**The stack and the accumulator are a service, not a handout.** `ExtSite`
exposes `load`/`store` on absolute slot indices, `push`/`pop`, and
`accInto`/`accIsNowIn`/`accInvalidate`; `Window` and `AccState` are
forward-declared and never defined in `ext.h`, so an extension TU that names
one fails to compile. The boundary is a compile error rather than a
guideline, and it earns its keep on one invariant an extension could not
infer: **every call that writes a window register first resolves an
accumulator living in it.** The window is a rotating four-register file, so
`physReg(tos)` aliases `physReg(tos - WINDOW_SIZE)` and `finishPop` reloads
a spill into the very register `topReg()` names. `pushValue` avoids the
first only by coincidence — its flush destination happens to be the
aliasing register — which is not a property an open-coded `mov` + `tos++`
would inherit.

The cost is that `halfwords` now budgets core-emitted code too, and the
worst case per call (2 for `push`/`pop`, 1 for a spilled `load`/`store`,
`materializeImm32`'s own for `accInto`) depends on a `tos` the author
cannot know. That is survivable because the check is empirical: the
translator measures the real `a.pc()` delta and bails before anything runs,
so miscounting is a deterministic diagnostic, never a bad branch offset.

**The extension is per-program state, not per-image.** It arrives as an
`enterProgram*` argument and is stored on the `Runtime` that program runs
under, then threaded from there into the three walks that decode
instructions. It has to be stored rather than only passed down because
compilation is lazy — a procedure is translated on its first dispatch, long
after `enterProgram*` returned — and `Runtime` is the context that lives
that long and is already threaded everywhere the translator looks. The word
it costs shifts `RUNTIME_DISPATCH_TABLE_OFFSET` from 40 to 44, which
`runtime.S` picks up from the macro rather than hardcoding.

> That shift is also how `test/qemu/Makefile` acquired an explicit
> dependency from `runtime.S`'s object to the ABI header it includes.
> Makefile.ultimate generates header dependencies for `.c`/`.cpp` but its
> `%.S.o` rule has neither `DEPFLAGS` nor a `.d` prerequisite, so a stale
> assembly object kept the old offset while the C++ half used the new one —
> a binary whose two halves disagree about a struct offset, which builds
> cleanly, asserts nothing, and hangs on the first dispatch.

### 18.1 Extension state

Two words of per-excursion scratch at `RUNTIME_EXT_STATE_OFFSET`, for
whatever an extension needs to carry (a stream cursor, a buffer base, an
object handle). They are the sentinel `ProcSlot`'s own
`bodyPtr`/`staticInfo`: `slots[0]` exists only so a real procedure index can
be offset by one, and nothing reads its static half — `sentinelLandingAddress()`
reads its `codePtr`, and every loop over procedures runs over
`slot(i) == slots[i+1]`. So the words are already allocated, already
reachable through the pointer emitted code has, and cost no layout change at
all. `Runtime::init` zeroes them; a `static_assert` ties the offset to the
real struct.

Two rather than three, starting at `bodyPtr` rather than `lastUsed`, because
`returnHelperTail` (§9) stamps the sentinel's `lastUsed` on every return out
of the entry procedure. That word is therefore genuinely written and cannot
be lent out. Trading one scratch word for a branchless return-from-entry is
the right way round: the branch would sit on a hot path taken once per
excursion, the word costs an extension nothing it can't work around.

`extEmitStateBase(a, dst)` is one instruction — a whole-register `MOV` out
of r9, one of the three things Thumb-1 lets a hi register do, so it pays no
mirror tax — and `extStateOffset(i)` rides in the load's own immediate.
That is why the offsets are absolute rather than starting at zero: biasing
the base would cost an extra `ADD` at every site, and the immediate has room
either way.

### 18.2 Contiguous sequences

`Assembler::AtomicBlock` reserves and only then suppresses the pool-reach
check, in that order. Suppressing first and reserving inside would let the
very flush being guarded against land in the middle of the guarded
sequence. That ordering used to be prose with every caller honouring it by
hand; `AtomicScope` is now private, so constructing an `AtomicBlock` is the
only way to obtain the suppression, and the two halves cannot be separated
or reversed. Note `ensurePoolRoom` is advisory rather than a reservation —
it returns immediately when the pending set is empty — which is exactly why
the count belongs in the type rather than in a call the author might omit.

`EXT_FLAG_ATOMIC` is what wraps an extension site in one: the block covers
the core-emitted service code as well as the extension's own, so
`poolWords` has to be declared rather than derived.

### 18.3 Helper reach

Two forms, both costing one **pooled literal word** for the helper's address
rather than a slot in the flash-resident r10 vector. That vector is a fixed
core array, so handing out indices would make every extension's helper set
part of the core's own ABI; a pooled word needs no index, deduplicates
within a chunk, and is compaction-safe for the same reason every other
literal is. The price is pool-reach pressure — `poolDebt()` is charged
against `SAFE_COND_BRANCH_SPAN`.

| form | emitted | for |
|---|---|---|
| `site.helperCall` | pooled address into r3, `BLX r3` | hand-written Thumb with a known clobber set, like the core's own `clzHelper`. No AAPCS guarantees — sp is legitimately 4-mod-8 inside an excursion. |
| `site.cHelperCall` | address into r12, then the r10-vector reach to `extThunkHelper` | independently-compiled C. |

`extThunkHelper` (runtime.S, helper index 9) is `push {lr}` /
`REALIGN_ENTER` / `blx r12` / `REALIGN_LEAVE` / `pop {pc}`. It exists for
two things emitted code cannot do itself: realign sp to 8 for AAPCS, and
preserve `lr` across the call — emitted code cannot, because `lr` carries
the live call/return record rather than a return address. The target
travels in **r12/ip**, the AAPCS intra-procedure scratch register, which is
exactly what a veneer is for and leaves r0-r3 to the callee's arguments.

`REALIGN_ENTER` clobbers r2/r3, so **a C helper takes at most two
arguments**, in r0 and r1 — the same arity limit §3 already argues for on
cost grounds. Both forms require `EXT_FLAG_NEEDS_LR` in the declaration,
since a `BLX` clobbers `lr` and the prologue's decision to save it was made
from that flag back in the pre-pass.

**Stack cost.** `extHelperStackBytes()` is the extension's declared worst
case for its own C helper, and nothing else — `Executor::run` adds
`EXT_THUNK_STACK_BYTES` (12: 4 for the pushed `lr`, 8 for the realignment
slack) on top, since that frame is the runtime's and an extension has no way
to know it. The total goes into the up-front budget once, against the
translator's own entry cost rather than on top of it (§2) — helpers do not recurse into bytecode
while call-shaped ops are rejected, so the worst case is the deepest
bytecode stack plus one helper frame. Nothing verifies the number: too
small and the static reservation stops being a bound, so prove it the way
the core proves its own (§3) — `-Wstack-usage=0` promoted to an error, or
hand-written naked Thumb.
