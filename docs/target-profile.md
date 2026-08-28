# Target profile — realistic-program limits beyond generic validation

`packages/machine/src/validate.ts` enforces isa-core.md §8's five *generic*
guarantees (TOS balance, call-graph acyclicity, the stack-depth bound,
dead-code rejection, header/block well-formedness) — properties that hold
for *any* target the generic machine could ever back. It has no concept of
`jit-armv6m` specifically, and rightly so: nothing about "this compiles to
Thumb, with a 4-register window and a fixed real-ABI helper vector" belongs
in a protocol-agnostic validator.

But a real `jit-armv6m` deployment has its own, much narrower notion of
"realistic" than the generic validator's ceilings do. `ProcSlot`'s own wire
field widths (`MAX_ARG_COUNT = 2047`, `MAX_BODY_BYTES ~1M`,
`runtime_internal.h`) are wire-format capacity limits, not target-realistic
ones — a real handwritten or lowered procedure never comes anywhere close
to either. The gap between "the generic validator allows it" and "any
plausible real program would ever produce it" is exactly the space
`jit-armv6m/fuzz/oracle_server.ts`'s validator gate was finding nothing but
noise in: `argCount = 972` is a perfectly valid, `validateProgram`-approved
program that no real caller would ever construct, and chasing what happens
to it finds ABI-encoding-width bugs, not `jit-armv6m` bugs.

This document collects those target-specific "realistic profile" limits —
constants worth reasoning about deliberately, checking somewhere, and
extending as more are found — separately from generic ISA validation.
`packages/machine`'s own `Extension` hook (isa-core.md §5.1, `extension.ts`)
is the natural long-term home for a "target profile" extension a validator
call could take alongside the generic checks; for now these live here and
in `oracle_server.ts`'s own extra gate, both by hand.

## TOS depth (argCount included) vs. the window's stack-reclaim encoding

**The crash this documents:** `jit-armv6m/fuzz`'s harness found that
`argCount = 972` (single procedure, body `[RETURN]`, otherwise entirely
ordinary) crashes `translateProc()` with an assertion failure in
`ArmV6M::Uoff<2, 7>`'s range check, reached from `Window::discardWindow`
(`compiler/src/window.cpp:146`) via `returnSequence`
(`translate_proc.cpp`). No `RESOURCE_ERROR` bailout — a raw `assert` abort,
on a program the generic validator fully approves.

**Why:** `WINDOW_SIZE` is 4 (`registers.h`) — any `argCount` beyond that
spills the extra arguments below the pushed call/return record, and
`discardWindow` reclaims all of them in a single instruction on the way out
of the procedure:

```cpp
// window.cpp
uint32_t spilled = spilledCount(tos) - (savesLR ? initialSpilledCount : 0);
if(spilled > 0)
    e.emit(ArmV6M::incrSp(ArmV6M::Uoff<2, 7>((uint16_t)(4 * spilled))));
```

That's Thumb's `ADD sp, sp, #imm` (T2 encoding): a 7-bit *word* immediate,
so the byte count it can reclaim in one shot maxes out at `4 * (2^7 - 1)
= 508` bytes, i.e. **127 spilled words**. Push `argCount` past
`WINDOW_SIZE + 127 = 131` and this instruction can no longer encode the
reclaim it needs — with no fallback (a multi-instruction reclaim sequence,
or a bailout), it just asserts.

**The two numbers to keep separate:**

| | value | meaning |
|---|---|---|
| Hard ABI ceiling | `argCount ≤ 131` | above this, `discardWindow`'s single-instruction reclaim can't encode the byte count at all and the translator bails with `RESOURCE_ERROR` (see below) — a capability limit, not a policy choice |
| Realistic-profile cap | `argCount ≤ 16` | `oracle_server.ts`'s own extra gate (`REALISTIC_MAX_ARG_COUNT`) — no real procedure needs more than a handful of parameters; keeping the fuzz search inside this band means every crash it finds is worth investigating on its own terms, not "well, nobody would ever call it with 900 arguments anyway" |

**Closed:** `discardWindow` now range-checks the reclaim and calls
`Assembler::fail()` instead of asserting, so the hard ceiling is a clean
`RESOURCE_ERROR` rather than an abort. Measured directly
(`fuzz/dump_code.sh` on hand-built one-instruction procedures):
`argCount = 131` compiles, and 132 / 500 / 972 / 2047 all bail.
`restoreWindow` carries the same guard on the same encoding.

What remains is a *capability* limit, not a crash: `ProcSlot::MAX_ARG_COUNT`
(2047) still admits far more than this ABI sequence can reclaim, so a
procedure between 132 and 2047 arguments simply cannot be compiled. Lifting
that needs a real multi-instruction reclaim, and nothing needs one today.
The realistic-profile cap below is therefore about keeping the fuzzer's
search somewhere interesting, not about avoiding an abort.

**It is not really about argCount.** `discardWindow` reclaims the whole
spilled frame, so 131 caps *total TOS depth* — arguments and pushed operands
together. That is the same number for a procedure with 131 arguments and for
one with none that pushes 132 operands, and the second is the case that
actually matters: it is what a fuzzer produces by the thousand.
`oracle_server.ts` gates on it (`REALISTIC_MAX_TOTAL_DEPTH = 128`, against
`validateProgram`'s own whole-program `totalDepth`) for a measured reason —
unbounded, **84% of a real fuzz corpus landed above the ceiling**, so 84% of
the fuzzer's budget went to programs that could only ever bail, and whose
emitted code neither half of `fuzz/` could look at. `fuzz/qemu_exec` is what
made that ratio visible; the crash-only harness had no way to tell a bail
from a pass.

**A consequence worth recording:** `translate_proc.cpp`'s `spillImm` guards
an SP-relative offset against `Uoff<2, 8>`'s 1020-byte ceiling, i.e. 255
words — which is *unreachable*, because `discardWindow`'s much tighter
7-bit reclaim bails first on any program deep enough to get there. It is
defence in depth with nothing behind it. Left in place (the cost is one
comparison, and the two ceilings are independent things that could move),
but no test or fuzz input can exercise it, and one shouldn't be written on
the assumption that it can.

## Non-canonical (overlong) LEB128 fields

**The crash this documents:** a fuzzed input decoded to `argCount=0`, body
`[SHR IMM_ACC 466312]` — but the *interesting* part wasn't the huge shift
amount (see below, a real bug, now fixed in isa-core.md §4.1); it's that
`466312` itself decoded from a wire-format immediate at all. The actual gap
is in the encoding question this section is about.

That exact instruction no longer validates — §4.1 now bounds an
*immediate* shift amount to `0..31` — but the encoding question survives
it unchanged: the same overlong field can carry any other operand, and a
shift amount computed at run time is still unbounded and still legal.

Separately, another fuzzed input hit `compiler/src/decode_instr.cpp`'s own
`decodeLeb128` with a genuine crash: `runtime error: shift exponent 35 is
too large for 32-bit type` — a real, if fairly narrow, undefined-behavior
bug, now guarded by an `assert(shift < 32)` there (matching that file's
existing "malformed encoding asserts rather than checks, compiled out
under `-DNDEBUG` in production" convention — decode_instr.h's own doc
comment). But an `assert` only turns UB into a clean abort; it doesn't stop
the fuzzer from re-finding the same "no real encoder would ever produce
this" input over and over.

**Why the wire encoding is at fault, not the value:** a LEB128 encoding of
a 32-bit value never legitimately needs more than 5 bytes — every encoder
in this codebase (`bytecode.ts`'s `encodeLeb128`) already only ever emits
the minimal/canonical form. But *decoding* enforces no such cap on either
side: `bytecode.ts`'s own `decodeLeb128` happily accumulates an arbitrary
number of continuation bytes (harmlessly, since it multiplies via JS
doubles), while `decode_instr.cpp`'s copy does the equivalent with a real
32-bit left shift, which is undefined behavior once the accumulated shift
reaches 32. A non-canonical, redundant-continuation-byte encoding — the
kind no real encoder produces, only a fuzzer or a hand-crafted malicious
input — slips past `validateProgram` today and reaches the C++ decoder
completely unchallenged.

**The gate:** rather than teach every LEB128 call site on both sides of the
language boundary to reject overlong fields individually,
`oracle_server.ts` re-encodes whatever it just decoded
(`encodeLeb128`/`encodeBody`) and compares byte lengths against the
original input — `bytecode.ts`'s encoders are always canonical, so any
length mismatch means some field along the way was non-canonical,
regardless of which one. Treated the same as a plain decode failure
(`stage=1`), not a separate realistic-profile cap, since this isn't a
"real but unrealistic" value question — it's a wire-encoding
well-formedness one.

## `TRAP` code vs. the trap-return sentinel

**The ambiguity this documents:** `runtime_host.h`'s `ProgramResult` packs
both outcomes of a whole excursion into one word, and
`translate_proc.cpp`'s `TRAP` handling tags a bytecode trap by setting bit
31 (`0x80000000u | (uint32_t)term.imm`), leaving `ProgramResult.trapped`
for `RESOURCE_ERROR` alone. That header's own comment already says this
slice "has no real error-reporting model" for `TRAP` and sentinel-encodes
it; what it doesn't say is that the encoding is *lossy in two directions*:

- a `RETURN` whose value has bit 31 set is indistinguishable from a `TRAP`
- a `TRAP` whose code already has bit 31 set aliases the same code with it
  cleared (`TRAP 0x80000005` and `TRAP 5` report identically)

isa-core.md §4.5 makes the trap code a full u32 ("an opaque error code;
`0` is unreachable/panic by convention, the rest of the space is
host-defined"), and §4.2's values are u32 throughout, so both halves of
the space are legal input.

| | value | meaning |
|---|---|---|
| Unambiguous return values | `acc < 2³¹` | above this, the caller cannot tell a returned value from a trap |
| Unambiguous trap codes | `code < 2³¹` | above this, the code aliases `code & 0x7fffffff` |

**Not gated, deliberately.** `jit-armv6m/fuzz/qemu_exec` *runs* programs
whose result falls in the ambiguous half (they still have to not hang or
fault) and only skips the result *comparison*, since comparing under a
lossy encoding manufactures false mismatches. Nothing about the
translation of a large `RETURN` value or `TRAP` code is itself suspect —
the gap is in the one-word result channel, and closing it means a real
error-reporting model (a second out-parameter, or a distinct `trapped`
value for a bytecode trap), not a cap on the ISA.

## `TRAP` does not unwind — a nested trap becomes a return value

**Structural, not a local codegen mistake.** `translate_proc.cpp`'s
`handleGlobalJump` compiles `TRAP #code` to

```
    materializeImm32(ACC_REG, 0x80000000 | code)
    returnSequence(...)          // an ordinary return
```

For the *entry* procedure that is exactly right: that word is
`ProgramResult.value`, and `runtime_host.h` already documents the high bit
as this slice's trap sentinel. For any *nested* procedure it is wrong. The
callee returns normally, the sentinel lands in the caller's `acc` as an
ordinary return value, and the caller keeps executing — usually
overwriting it immediately.

Minimal repro (`fuzz/qemu_exec/minimize_exec.ts` reduced a 195-instruction
input to this):

```
proc 0 (argCount 0):  CALL 1 ; CONST 92 ; RETURN
proc 1 (argCount 0):  TRAP 754
```

The reference VM traps with 754 (a bytecode trap unwinds the whole program
there, and `ProgramResult`/§9 model it the same way). The emitted code
returns 92. Disassembled, `proc 1` is
`ldr r0, =0x800002f2 ; bx returnHelperFromLr` — a plain return — and
`proc 0` then does `movs r0, #92` straight over it.

**What a fix needs**, and why it isn't done here: a real unwind, i.e. a
helper-vector slot that does for a bytecode trap what
`dispatch_abi.cpp`'s `runtimeBail` already does for `RESOURCE_ERROR` —
restore `Runtime::savedSp` and transfer to the landing, carrying the trap
code. That is a new reserved slot (§11's table), hand-written asm in
`runtime.S`, a new fixed constant, and a change to the dispatch ABI's own
contract about what may leave a compiled procedure. Worth doing
deliberately, not folded into a fuzzing pass.

**Meanwhile:** `fuzz/qemu_exec` sets these aside explicitly rather than
reporting them repeatedly — `vm.ts`'s `VmResult.trapDepth` says how many
frames below the entry the trap fired, and the harness skips anything above
0 with a named reason. Nothing else about such a program is exempt: it is
still translated, still run, and still checked for crashes and hangs.

## Whole-program procedure count

`ProcSlot`'s directory is sized by `procCount` with no ceiling of its own
beyond storage. `oracle_server.ts` caps it at 16
(`REALISTIC_MAX_PROC_COUNT`), the same kind of realistic-profile bound as
`REALISTIC_MAX_ARG_COUNT` above: a fuzzer left unbounded spends its budget
on hundred-procedure programs whose procedures are one instruction each,
which exercises `Runtime::init`'s loop and nothing else.
`harness.cpp` mirrors the constant, since its `Runtime` storage buffer is
sized off it.

## Adding another entry

When the fuzzer (or anything else) turns up another "validator says yes,
no real program would ever do this, and here's what breaks" case: measure
the actual hard limit the way the section above does (don't guess), pick a
realistic-profile cap comfortably under it, add both to the table pattern
above, and wire the cap into `oracle_server.ts`'s `withinRealisticProfile`
gate.
