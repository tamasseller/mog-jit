# Bytecode reader interface — plan

Status: done. The interface as built is documented in design.md §1.2 and
§18; what follows is the plan it was built from, with the deviations
recorded below.

Two changes that ride together: a link-time-injected bytecode reader, and the
extension interface simplification that the reader's phase split exposes.

## Goal

- Bytecode reachable only through a link-time accessor, never a raw pointer.
- Memory-mapped storage (internal flash, XIP QSPI) stays the zero-cost case.
- Block-buffered external storage (SPI NOR, EEPROM, a host link) becomes
  possible with no JIT change.
- Caching and prefetch live entirely in the driver.
- `bodyPtr` becomes an opaque handle, so it can later become an offset — a
  prerequisite for shrinking `ProcSlot`.

Not a goal: raising the program-size ceiling. That ceiling is `ProcSlot` at
16 B/proc, resident for the whole run, and this change does not move it.

## Why it is cheap: the access pattern is already a cursor

Every bytecode read in the tree is strictly forward.

| site | pattern |
|---|---|
| `programFrameHash` | sequential over the whole payload |
| `parseProgramHeader`, `Runtime::loadProgram` | sequential, procedure by procedure |
| `GUARDED_scanBody` | forward; recurses on block nesting, `pc` shared by reference, only advances |
| `GUARDED_processUntilTerminator` | forward |
| `translateLoop` / `IfThen` / `IfThenElse` / `BrTableExh` / `Switch` | every recursive call resumes at `pc` or `term.next` |

`translateLoop`'s back edge is computed from `a.pc()` — an emitted-code
offset, not a bytecode one. Nothing rewinds the wire.

So `pc` is already a cursor position passed by hand. Threading a real cursor
removes it: `translateLoop(uint32_t pc, BranchWidth)` becomes
`translateLoop(BranchWidth)`, and `DecodedInstr::next` disappears. The only
non-read use of `pc` is `this->bytes + pc` for `ExtSite`, plus
`translateBody`'s closing `assert(decoded.next == this->bytesLen)`.

Two deviations from strict streaming, both solved JIT-side:

- **One-instruction lookahead.** `peekStoreFold` and the `CONST` lookahead in
  `translate_data_flow.cpp` decode ahead and may decline to consume. Fix by
  carrying the `DecodedInstr` forward into the next loop iteration instead of
  re-decoding at the top. Do not give the accessor a rewind — that puts a
  minimum-buffer-size constraint back into the driver.
- **Narrow→Wide restart.** `translateProc` runs `translateBody` twice from
  body offset 0. Needs `open(handle)`, nothing more.

## The interface

Link-time bound, matching the precedent already set by `extDecode`/`extEmit`
(`extern "C"`, no vtable, weak default in `ext_default.cpp`).

```c
typedef uint32_t BcHandle;   /* opaque; what ProcSlot::bodyPtr now holds */

extern "C" void     bcOpen(BcCursor *c, BcHandle h, uint32_t len);
extern "C" uint8_t  bcNext(BcCursor *c);          /* undefined past len */
extern "C" void     bcHint(BcHandle h, uint32_t len);  /* optional prefetch */
```

Byte at a time, deliberately. A span request would force the accessor to
handle a request straddling a block boundary — short-return or staging copy —
which is logic the memory-mapped case pays for in interface complexity and
needs none of.

`bcOpen` carries `len` so the cursor owns end-of-body; `bytesLen` stops being
threaded through every decode signature.

Cost: cold start is 21k–49k instructions per procedure (`bench/README.md`) for
81–231 bytes of bytecode, so roughly 200 instructions per bytecode byte
already. A non-inlined per-byte call is a few percent. Enable LTO if it shows
up in a measurement.

### Prefetch

`bcHint(handle, bodyBytes)` at `Ctx::Ctx`. `bodyBytes` is already in the
dispatch table.

The win is not DMA overlap. It is that the Narrow→Wide second pass and the
recompile-after-eviction pass become cache hits, with a buffer sized to the
largest procedure body. Note the first scan of a procedure cannot be hinted:
`bodyBytes` only exists after that scan produced it.

## Extension interface

`extDecode` returns a packed `decl` that rides through `Instr::extDecl` into
`handleExt`. Who consumes what:

| field | consumer | phase |
|---|---|---|
| `NEEDS_LR` | `triggersLRSave` → prologue push | scan |
| `CALL_SHAPED`, `tosDelta > 0` | rejection | scan |
| `tosDelta` | post-emit cross-check | translate |
| `halfwords` | emit budget check | translate |
| `poolWords`, `ATOMIC` | `AtomicBlock` reservation | translate |

`halfwords` is a survivor of the branch-span budget removed in `ac6ab46`
("returned a byte figure to charge against a branch-span budget that no longer
exists"). The two-pass Narrow/Wide retry replaced any need to predict an
instruction's emitted size. `poolWords` and `ATOMIC` remain only because the
core opens the `AtomicBlock` on the extension's behalf and must be told the
size first.

`Assembler::AtomicBlock` is public and `ExtSite::a` is a public `Assembler&`,
so the extension can open its own as its first act. Doing that removes:

- `halfwords` and `poolWords` from the declaration;
- the emit budget check in `Ctx::handleExt`, which exists only to catch a
  declaration disagreeing with what `extEmit` emitted;
- the double bookkeeping — `ext_rawmem.cpp` states `halfwords=16` in
  `extDecode` and separately emits sixteen halfwords in `emitMemmove`, policed
  at run time by `RESOURCE_PROGRAM_EXT_UNSUPPORTED`.

`extEmit` already re-derives what it needs: `const uint8_t opcode =
*site.opcode();` then re-dispatches. It never reads the `decl` it was handed.

Resulting shape — two calls on the two phases, instead of one call straddling
both and passing an IR between them through `Instr`'s union:

```c
/* scan phase: wire length + what the core cannot get anywhere else */
extern "C" uint32_t extDescribe(BcCursor *c, uint32_t *desc);  /* -> length */
/* translate phase: cursor at the instruction, extension owns everything else */
extern "C" void     extEmit(ExtSite &site);
```

`desc` carries `NEEDS_LR`, `CALL_SHAPED`, `tosDelta`. Half the current word.

Keep it a word. `Instr`'s union is what keeps `DecodedInstr` off the sret path
in `proc_scan`'s recursion, where the margin against `SCAN_STACK_MARGIN` is
~56 bytes; a description returned by value would spend it.

Deliberate trade: the budget check is the only guard against a buggy extension
letting the literal pool flush mid-sequence. Moving `AtomicBlock` into
`extEmit` makes that the extension's problem — consistent with register
discipline and `EXT_SCRATCH_MASK`, which nothing enforces either, but it is a
guardrail coming out.

## Work items

In order. Each step keeps the tree green.

1. Carry the lookahead instruction forward in
   `GUARDED_processUntilTerminator`; delete `peekStoreFold`'s re-decode.
2. Thread a cursor through `decode_instr` and `proc_scan`. Signatures lose
   `(bytes, bytesLen, offset)`, gain `(cursor)`. `decodeLeb128Checked`'s
   overrun test becomes a cursor-end test.
3. Thread the cursor through `translate_*`. Drop `pc` parameters and
   `DecodedInstr::next`.
4. `programFrameOk` and `parseProgramHeader` onto the cursor.
5. `ProcSlot::bodyPtr` → `BcHandle`. Add the weak default accessor
   (memory-mapped, `bcOpen`/`bcNext` inline-trivial) in a new
   `bytecode_default.cpp` beside `ext_default.cpp`.
6. Split `extDecode` into `extDescribe`; move `AtomicBlock` into the three
   `extEmit` implementations; drop `halfwords`/`poolWords`/`ATOMIC` and the
   budget check.
7. `bcHint`, and a buffered reference accessor to prove the interface.

## As built

Deviations from the above, and what they cost.

- **`bcTell` is a fourth accessor call.** `Runtime::loadProgram` walks the
  whole directory off one cursor and has to pin each body it passes; with
  an opaque handle it cannot do the arithmetic itself. `bcTell(c)` names
  the byte the cursor will read next, and is the only place a `bodyHandle`
  comes from.
- **`BcReader` owns end-of-body, not `BcCursor`.** The core wraps the
  driver's cursor with the length it was opened with; `atEnd()` bounds every
  walk, and reading past it is asserted rather than tracked — a malformed
  program is the validator's business, so nothing is carried per byte for
  it. `bcOpen` still carries `len`, for prefetch sizing.
- **`extDescribe` returns a bool, not a length.** With the cursor threaded
  through, a returned length is a second opinion on a position the core
  already has — exactly the double bookkeeping item 6 exists to remove. It
  also retires the "no forward progress" hazard structurally: the core
  consumes the opcode byte itself, so a walk always advances.
- **`extEmit` is handed no description.** The two calls are on the two
  phases, so nothing carries a description from the scan into translation.
  Beyond the emit budget check the plan already gives up, that also drops
  the post-emit `tosDelta` cross-check and `EXT_FLAG_KILLS_ACC`, and with
  them four host tests. An emitter whose window effect disagrees with its
  own `extDescribe` is now a miscompilation rather than a diagnostic —
  keeping the check would have meant either a cursor copy per instruction
  in the hot loop, or a description the emitter supplies itself, which
  checks nothing. `EXT_FLAG_NEEDS_LR`'s helper-reach assertion survives,
  retargeted onto the prologue's own `savesLR`, which is the fact that
  actually matters at the site.
- **`Op::EXT` carries the opcode byte, not the description.** `decodeInstr`
  never consults the extension: it hands back the opcode with the operands
  unread, and whichever phase asked consumes them. That keeps one decoder
  for both phases and takes `ext.h` out of `decode_instr`'s dependencies.

### Measured

`make test` (225 host, 118 QEMU), `stack-usage-check`, all 52 fuzz seeds
through `qemu_exec` (50 matched, 0 mismatched, 2 legitimate bails), 1.5M
executions of `fuzz/build.sh`'s ASan+UBSan driver and 21.4M of
`build_afl.sh`'s coverage-guided one (101 corpus entries past the 52 seeds,
0 crashes, 0 hangs), `bench/check.sh`, and the three-workload bench —
every image agreeing with the reference VM.

| | master | with the reader |
|---|---|---|
| cycles/sample, all three workloads | 27.79 / 19.23 / 151.00 | unchanged |
| emitted bytes, all three workloads | 150 / 210 / 298 | unchanged |
| cold start, pulse-trigger | 21990 | 22320 (+1.5%) |
| cold start, iq-preamble | 27828 | 28323 (+1.8%) |
| cold start, median5 | 51742 | 54367 (+5.1%) |
| fixed .text, translator + runtime | 9530 B | 9292 B (−238 B) |
| translator stack peak (deepest workload) | 1464 B | 1304 B |
| `GUARDED_processUntilTerminator` | 416 B | 384 B (limit 432) |
| `GUARDED_scanBody` | 160 B | 128 B (limit 160) |
| `scanProcBody` | 144 B | 112 B (limit 488, shared with `translateProc`'s 472) |

Cold start is the per-byte call, and it lands inside the "few percent" this
plan predicted for the two smaller workloads. **LTO is not the answer here:**
`test/qemu`'s stack-usage gate reads GCC's per-translation-unit callgraph
(`-fcallgraph-info`), and LTO moves inlining past the point those are
written — the bound that keeps the translator's recursion safe would stop
being derivable. The steady state is untouched, since none of this is on a
path compiled code takes.

## Risks

- **Downstream build scripts.** Every fuzz driver builds from its own
  hand-maintained source list, not `make test`: `fuzz/build.sh`,
  `build_afl.sh`, `dump_code.sh`, `probe_arena.sh`, `qemu_exec/build.sh`,
  `bench/build.sh`. A new `bytecode_default.cpp` must be added to each, and
  each rebuilt and positively sanity-checked. This is where the time goes.
- **Extension ABI break.** `ExtSite::opcode()` now returns the byte and
  `operands()` is `operand()`, one byte at a time off the cursor. Three
  implementations in-tree (`test/ext_rawmem.cpp`, `test/host/ext_stub.cpp`,
  `bench/ext_sampstream.cpp`) plus `ext_default.cpp`. Any out-of-tree
  extension breaks.
- **Test surface.** 18 files construct programs or call `Executor::run`
  directly; `test/corpus_programs.h`, `test/qemu/run_program.h` and
  `measure_proc.h` are the shared ones to change first.
- **Stack budget.** `TRANSLATOR_ENTRY_CPP_BYTES` and
  `TRANSLATE_LEVEL_STACK_MARGIN` are measured from GCC's callgraph by
  `tools/stack-margin.ts` and gated by `test/qemu`'s stack-usage-check. A
  cursor adds a frame per decode; re-derive, do not adjust by hand.

## Not doing: dropping CALL

Measured, in case it comes up again. Cortex-M0 `-Os`, link map attribution,
translator plus runtime, no libc:

| variant | bytes | delta |
|---|---|---|
| current | 11305 | — |
| eager (all procs compiled up front, CALL kept) | 10887 | −418 |
| single procedure, no CALL at all | 9961 | −1344 |

12% for the whole call/dispatch/laziness story, against a ~9.0 KB floor that
is just the expression and control-flow translator. Not worth a variant.

The RAM argument runs the other way: on-demand compilation makes the arena
O(working set) rather than O(program), because nothing is pinned (§8) and the
`(procIdx, resume offset)` record re-enters an evicted caller. Emitted code is
1.29–1.93× its bytecode (`bench/README.md`), so the arena floor is one
procedure. That is the property this reader plan extends to storage.
