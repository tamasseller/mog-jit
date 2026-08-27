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

## argCount vs. the window's stack-reclaim encoding

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
| Hard ABI ceiling | `argCount ≤ 131` | above this, `discardWindow`'s single-instruction reclaim can't encode the byte count at all — a real bug (§"Open question" below), not a policy choice |
| Realistic-profile cap | `argCount ≤ 16` | `oracle_server.ts`'s own extra gate (`REALISTIC_MAX_ARG_COUNT`) — no real procedure needs more than a handful of parameters; keeping the fuzz search inside this band means every crash it finds is worth investigating on its own terms, not "well, nobody would ever call it with 900 arguments anyway" |

**Open question, not fixed here:** the hard ceiling (131) is itself a real
gap — `Runtime::init()`/`ProcSlot::MAX_ARG_COUNT` (2047) let far more
through than `discardWindow` can ever encode, and nothing between the two
currently turns "argCount is technically representable but this specific
ABI sequence can't reclaim it" into a `RESOURCE_ERROR` bailout instead of an
assert. Worth its own fix (either a real multi-instruction reclaim, or a
compile-time-checked cap enforced before `translateProc` ever runs) —
tracked here rather than patched reflexively, since the *realistic-profile*
gate below already keeps the fuzzer productive without it.

## Non-canonical (overlong) LEB128 fields

**The crash this documents:** a fuzzed input decoded to `argCount=0`, body
`[SHR IMM_ACC 466312]` — but the *interesting* part wasn't the huge shift
amount (see below, a real bug, now fixed in `binops.cpp`); it's that
`466312` itself decoded from a wire-format immediate at all. Nothing about
that value is unrealistic on its own — a legitimate procedure could compute
a shift amount that large at runtime — the actual gap is in the encoding
question this section is about.

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

## Adding another entry

When the fuzzer (or anything else) turns up another "validator says yes,
no real program would ever do this, and here's what breaks" case: measure
the actual hard limit the way the section above does (don't guess), pick a
realistic-profile cap comfortably under it, add both to the table pattern
above, and wire the cap into `oracle_server.ts`'s `withinRealisticProfile`
gate.
