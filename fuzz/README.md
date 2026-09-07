# fuzz

Programs are generated at **DSL level** and checked by three engines that
must agree: an AST evaluator, `mog-core`'s reference VM, and the real
emitted Thumb on an emulated target. A fourth sink runs the translator on
the host under ASan/UBSan to catch what none of them can — a crash.

Generating at AST level rather than mutating wire bytes buys three things.
Validator approval becomes the normal outcome (~98%, against ~13% for byte
mutation), so the work is spent on programs that reach the translator at
all. Deep nests, wide jump tables and call graphs come out of the grammar
instead of having to be hand-authored. And the **lowerer** joins the axis:
the reference VM and the JIT both consume the same `RtlProgram`, so only an
engine that never sees the lowerer's output can disagree with it.

## Running it

```sh
./fuzz/fuzz.sh                  # until killed; Ctrl-C reports and exits
./fuzz/fuzz.sh --for 2h         # or 90m, 300s
./fuzz/fuzz.sh --rounds 20000
./fuzz/fuzz.sh --stop-on-finding
./fuzz/fuzz.sh --calibrate-only # the self-checks alone
```

That is the whole interface. It builds what it needs, checks its own oracles
before trusting them, measures what the seeds reach unmutated, runs the
campaign with the lanes interleaved, and reports where programs died and how
much of the target was reached. Non-zero exit on a finding or a failed
self-check. Redirect it and the progress ticker becomes ordinary log lines.

A finding is reported as **DSL source**, minimized, which is the whole point
of the tool: the output is a program you can read, and the same text is a
seed file.

Everything below is what that runs, and how to reach a piece on its own.
Each `npx` line is run from the repo root, matching that entry point's own
usage line; the shell scripts locate themselves and work from anywhere. A
bare `ts/...` path is what a `Cannot find module './driver.ts'` means.

## The pieces

| kind | what | when |
|---|---|---|
| **calibration** | `eval/eval_check`, `gen/ub_check`, `eval/diff`, `gen/stats` | once, before a campaign means anything |
| **lane** | `invalid`, `frame` | interleaved, every few batches |
| **main loop** | `driver` | continuously |
| **diagnostic** | `repro.sh`, `dump_code.sh`, `probe_arena.sh`, `gen/show`, `gen/minimize`, `qemu-exec` | by hand, on one program |
| **measurement** | `seed_value`, `coverage.sh` | by hand; `fuzz.sh` calls `seed_value` on the seeds it finds idle |

Each is a standalone script and still runs alone — `fuzz.sh` orchestrates
them rather than absorbing them.

## The campaign on its own

```sh
make -C fuzz/src/qemu-exec   # the target sink
make -C fuzz/src/driver      # the host sink
npx ts-node --transpile-only fuzz/ts/driver.ts --rounds 20000 --batch 200
```

Exits non-zero on a finding. `--no-qemu` / `--no-host` run one sink alone;
`--seed N` moves the whole campaign to a different sequence.

Every finding is written out as a program, readable as DSL source, under
`--save <dir>` (`/tmp/ppl-fuzz-findings` by default). That file is the repro.

The pair a finding is reported under — `descended from signed, round seed 6`
— names the *seed* it grew from, not the program that failed: the corpus
feeds novel programs back into itself, so by the time something fails its
entry is usually several generations deep. The pair is a breadcrumb, the
saved program is the artefact.

```sh
npx ts-node --transpile-only fuzz/ts/gen/minimize.ts /tmp/ppl-fuzz-findings/<file>.json
npx ts-node --transpile-only fuzz/ts/gen/minimize.ts <file>.json --jit   # target predicate
```

`--dbg-every` (default 4) also runs the batch through
`exec_runner_dbg.elf`, the same runner with asserts live. The shipped image
is `-DNDEBUG`, so before it nothing that *executed* emitted Thumb could see
an assert fire: the host sink asserts but never runs the code. An assert
comes back as an `A:` line in the failing program's own ordinal slot, so the
driver names it without bisecting.

`gen/minimize.ts` shrinks on the tree — deleting statements, collapsing an
expression into one of its children — so what comes out is source a person
reads, and every candidate it tries is well-formed by construction. To
replay one through the host crash sink instead:

```sh
npx ts-node --transpile-only fuzz/ts/gen/show.ts <entry> <seed> --emit /tmp/p.bin
./fuzz/repro.sh /tmp/p.bin
```

## The two tiers

| | what runs | what it catches |
|---|---|---|
| inner (in process) | mutate → UB filter → AST evaluator → lower → validate → reference VM | **lowerer and VM bugs** — the two engines disagreeing, on a value, a trap code or the extension buffer |
| outer (batched) | `src/qemu-exec/` on `qemu-system-arm`, the same image again with asserts live, and `src/driver/` under ASan/UBSan | **miscompilation** (the wrong number, or no answer at all, from real emitted Thumb) and **crashes** (an assert, UB, an out-of-range encoding) |

The inner tier costs no emulator and runs on everything; the outer tier only
ever sees programs that already survived it. A batch is retained on a
structural signature — procedure count, nesting depth, the validator's own
`totalDepth`/`maxCallDepth`, compiled size — so a QEMU boot is spent on
shapes it has not run yet rather than on many mutants of one.

A program with a shape nothing has produced before also **joins the
corpus**. Without that, every candidate is one generation from a
hand-written seed and the shapes that only exist several mutations deep are
never reached at all. Retention is bounded by size, because mutation adds
far more often than it deletes and an unbounded corpus drifts until
everything in it sits against `PROGRAM_MAX`.

Every candidate goes through `parse(print(ast))` and is lowered from the
**reparsed** tree, so the program that runs is provably the one a report
prints. This used to be a sampled sweep over the corpus instead, because
parsing dominated the inner loop and cost roughly 2x per level of expression
nesting — which turned out to be two grammar rules parsing their operand
twice rather than anything about the parser generator. Fixed in mog-core, it
now costs a few percent of a campaign, and the sampling is gone.

Both sinks read the **same batch file** (`exec_runner.cpp`'s format), so a
campaign feeds them from one artefact. A crashing batch is bisected down to
the single program responsible.

## The generator (`ts/gen/`)

`mutate.ts` is a scoped rewrite walk, not mutate-then-repair: it carries
what is legal at each site — visible variables, callable procedures, whether
a `break` would still be its case's closer — and only picks mutations that
site accepts. Four invariants are load-bearing, each with a real trap behind
it, and they are documented at the top of that file.

`print.ts` turns the mutated tree back into DSL source, which is how it
re-enters the toolchain (`ir` consumes text). That round trip is also a
check: `parse(print(ast))` must equal `ast` structurally, asserted on every
candidate, so every mutant exercises the real parser.

Mutation adds a statement at a time against a corpus bounded well below a
branch's reach, so the translator's out-of-range paths cannot be reached
from it however long a campaign runs. `--bulk-every` (default 32) therefore
runs a size-directed lane instead: `inflate` builds one construct with no
`else` over a run of 80–320 statements, so the branch across it has to clear
the whole thing. A conditional branch reaches ±254 bytes of code and the
wide form the translator retries in reaches ±2046; the `do`-`while` shape is
in the ladder because a `for`'s exit test branches forward over the body and
so fails before its back edge can. These programs are not fed back into the
corpus — they are a directed probe, and feeding them back would grow every
later generation by a fixed pad.

`corpus.ts` holds the seeds, authored as source. A corpus entry is a
*procedure graph*, since the grammar has no function-declaration node;
procedure `i` is named `p<i>` and may only call a higher index, which is
isa-core.md §8.2's acyclicity held as a representation invariant.

A seed earns its place by coverage, so `fuzz.sh` judges every seed at
startup and says `keep` or `drop` per name. Target edges first, one QEMU
boot each and no `gcovr`, which is what makes it affordable every run. Only
the seeds that axis finds idle are put to the host axis, which is the
expensive one — the two genuinely disagree, `ext_memcmp` having no unique
host line and four unique target edges. Leave-one-out is how the host axis
measures, so a mutually-redundant *pair* hides from it: each covers the
other, both score zero, and dropping both loses what the pair reached. One
further run over all the zero-scoring seeds at once catches that, and the
verdict becomes `keep one of`. `ts/seed_value.ts` is the same measurement
per seed rather than per suspect, plus a greedy minimal covering subset.

`ub.ts` drops programs whose operands are unsequenced with conflicting
effects. The DSL leaves a binary operator's operands unsequenced exactly as
C does, and `pickBinaryOrder` really does reorder them, so such a program
has no single right answer and comparing engines on it manufactures
mismatches that are not bugs. `ub_check.ts` calibrates it in both
directions — missing a real conflict is as wrong as flagging a defined one.

## The invalid lane (`ts/gen/invalid.ts`, `ts/invalid.ts`)

The valid lane asks whether a well-formed program computes the same answer
everywhere. This one asks the opposite, and it is the question the design
rests on: an application composing DSL fragments is allowed to get it wrong,
and something must catch it before a program reaches the wire. Since
design.md §12 turned every wire-validity check in the JIT into an assert
that `-DNDEBUG` strips, what catches it is `lowerProgram`, `signature.ts` or
`validateProgram` — and nothing else.

```sh
npx ts-node --transpile-only fuzz/ts/invalid.ts --rounds 1000
```

Eleven breakers, each violating exactly one named invariant, so a program
that gets through names the gate that was asleep. Calibrating it took three
rounds of finding that an escape was the breaker's fault, and each of those
is a fact worth keeping:

- A `default:` written **last** needs no `break` — `caseCloser` closes any
  clause with nothing after it. Only a default that runs on into another
  case is a violation.
- Statements after one that always terminates are **dropped, not rejected**,
  and a procedure the entry cannot reach is **never lowered**. A breaker
  that strikes either proves nothing, so site selection is restricted to
  live, reachable code.
- A shift amount of 32 or more is a *static* violation only where it lowers
  to the immediate form; reached through a register it is a runtime value
  §4.1 leaves unspecified, which the reference VM raises instead. Sites for
  that breaker exclude short-circuit right-hand sides and ternary arms,
  where an evaluation may never arrive.

The graph is built through `toProceduresUnchecked`, not `toProcedures`:
`checkGraph` is this fuzzer's own representation invariant, and a recursion
it refuses never reaches `validateProgram`'s §8.2 rejection, which is the
thing under test.

## The frame lane (`ts/frame.ts`)

§1.1's frame is the whole binding between the validator and the JIT, and
since the demotions it is the only runtime check standing between a corrupt
buffer and a translator that trusts its input. So it gets a lane:

```sh
npx ts-node --transpile-only fuzz/ts/frame.ts --rounds 2000
```

Seven kinds of damage to a program the valid lane accepted; the target must
answer `RESOURCE_PROGRAM_FRAME` and nothing else. A folded 16-bit hash
misses about one corruption in 65536 by construction — those are computed
here rather than discovered on the target, and are **not** run, because past
the frame the program is garbage the translator would take on trust.

## The evaluator (`ts/eval/`)

`ast-eval.ts` is the third engine, and its whole value is being
independent: it restates the type, signedness and narrowing rules rather
than importing `desugar`, `types.ts`'s `annotate`, or `vm.ts`'s
`evalBinary`/`evalUnary`. A bug in any of those would otherwise sit on both
sides of the comparison and cancel out.

The one deliberate exception is the extension: `lib/rawmem_ext.ts` *is* the
specification of those opcodes, so its own `exec` is driven through a small
`ExecState` adapter. A second MEMMOVE could only invent disagreements.

`eval_check.ts` is its gate — cases chosen to be awkward rather than random,
because random mutants turn out not to reach several of them. Signed
comparison in particular needs a narrow variable holding a high-bit value
against a small literal, which is a narrow target to hit by chance. Sabotage
a rule in `ast-eval.ts` and this must fail; if it does not, the case is not
doing what it claims.

`diff.ts` runs the inner tier alone, over as many mutants as asked, with no
emulator.

## Replaying saved programs (`ts/qemu-exec.ts`)

Runs encoded programs on the target against the reference VM, one QEMU boot
per batch, over any directory given on the command line:

```sh
npx ts-node --transpile-only fuzz/ts/qemu-exec.ts <dir>
```

Used for a finding under investigation. There is deliberately no standing
corpus of past findings: a fixed bug is pinned by a test in `test/host` or
`test/qemu`, where it is checked on every build and cannot rot into a file
nobody reads. What the seed corpus (`ts/gen/corpus.ts`) is for is coverage,
not history — see `ts/seed_value.ts`.

## The extension

`lib/rawmem_ext.ts` is a 1KB sandbox with six load/store widths, a MEMMOVE
whose three operands all come off the operand stack, and two comparisons.
It exists to exercise the JIT's extension seam end to end — `ExtSite`'s
window and acc services, the hand-written MEMMOVE helper, `extThunkHelper`'s
AAPCS reach — not to be useful. Every offset is masked rather than
bounds-checked, so there is no trap path for the two halves to disagree
about.

Its `rules()` hook is what makes any of that reachable from DSL source;
without it the seam is reachable only from hand-written RTL. The target half
is `../support/ext-rawmem/`.

Both sides compare the **buffer** as well as the result, through an
FNV-1a-32 digest the runner prints per program: a store to a wrongly
computed address is otherwise invisible unless the program happens to read
that slot back.

## Other tools

**Target-side coverage** answers the half the host build cannot see — the
dispatch path, `runtime.S`, the landing sequences:

```sh
make -C fuzz/src/qemu-exec COV=1
npx ts-node --transpile-only fuzz/ts/driver.ts --rounds 3000 \
    --qemu-elf fuzz/src/qemu-exec/exec_runner_cov.elf
```

`-fsanitize-coverage=trace-pc` calls `cov_rt.cpp` once per basic block.
AFL's mechanism, none of AFL's plumbing — and without AFL's collisions.
AFL hashes the return address into a small bitmap and lives with the
aliasing; a 32KB rom does not have to. Every instrumented block opens with a
4-byte `bl`, so no two return addresses are closer than four bytes and one
bit per four bytes of rom indexes every block uniquely, in 1KB of `.bss`.

That makes the map exact in both directions. `ts/lib/cov_map.ts` turns a bit
number back into an address, a function and a source line, so a campaign
reports `710 of 740 basic blocks` and then names the 30 — which is the
number worth acting on. The `-g` on the COV build is what resolves the
lines, and costs the image nothing.

`ts/cov_check.ts` calibrates the two halves against each other before any
campaign trusts them: the index is injective on this image, every block's
bit fits `g_covBitmap`, and every bit a live boot sets belongs to a block
the ELF knows. Nothing is mirrored by hand — the bitmap's size comes from
the symbol table and the block addresses from the disassembly — so an image
that moves is followed rather than misreported.

Two things the map cannot show. Anything that runs after `covReport` dumps
the bitmap (`semihostExit`) can never be recorded. And `trace-pc-guard`,
the usual way to get a dense per-block index, is Clang-only; this is GCC,
which offers `trace-pc` and `trace-cmp` and nothing else.

`coverage.sh` runs a campaign against the coverage build of the host sink
(`make -C src/driver COV=1`) and reports which translator lines it never
reached. What to read is the uncovered set, not the percentage: 400
candidates already reach 93.6% of translator lines and 3000 reach 94.9%, so
running more programs over the same branches buys nothing, and the cold
regions name what the generator cannot currently build. That is what the
size-directed lane, the empty `if` and the fall-through-into-`default`
mutation came from.

What stays cold is worth knowing. `proc_scan.cpp`'s rejection paths take
malformed wire input, which a generator whose every program the validator
approves cannot produce by construction. `window.cpp`'s `ExtSite` window
services and the spill and reclaim bails belong to `test/host`, which covers
them fully. `translate_control_flow.cpp`'s TOS restore at a loop's back edge
wants a condition block that leaves the operand stack where the body did
not, which no well-formed DSL loop does.

`probe_arena.sh` reports, per arena size, whether a program actually reaches
eviction — worth running after changing the sizing, since an arena that is
too generous silently exercises none of it. `dump_code.sh` disassembles what
the translator emitted. `gen/stats.ts` reports what fraction of generated
programs survives each stage; anything materially below ~98% means a
generator invariant has drifted, and the bucket it lands in names which.
