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

## Running a campaign

```sh
make -C src/qemu-exec        # the target sink
make -C src/driver           # the host sink
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

`gen/minimize.ts` shrinks on the tree — deleting statements, collapsing an
expression into one of its children — so what comes out is source a person
reads, and every candidate it tries is well-formed by construction. To
replay one through the host crash sink instead:

```sh
npx ts-node --transpile-only fuzz/ts/gen/show.ts <entry> <seed> --emit /tmp/p.bin
./repro.sh /tmp/p.bin
```

## The two tiers

| | what runs | what it catches |
|---|---|---|
| inner (in process) | mutate → UB filter → AST evaluator → lower → validate → reference VM | **lowerer and VM bugs** — the two engines disagreeing, on a value, a trap code or the extension buffer |
| outer (batched) | `src/qemu-exec/` on `qemu-system-arm`, and `src/driver/` under ASan/UBSan | **miscompilation** (the wrong number from real emitted Thumb) and **crashes** (an assert, UB, an out-of-range encoding) |

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

Parsing dominates the inner loop — by measurement, essentially all of it,
and roughly 2x per level of expression nesting, so a deeply nested body can
cost hundreds of milliseconds on its own. The print/reparse round trip is
therefore checked out of band: at each batch boundary the driver spends
`--verify-ms` (default 250) walking the corpus round-robin. The corpus is
the better population for it than a uniform sample of candidates, being
where the shapes that only exist several mutations deep accumulate.

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

`ub.ts` drops programs whose operands are unsequenced with conflicting
effects. The DSL leaves a binary operator's operands unsequenced exactly as
C does, and `pickBinaryOrder` really does reorder them, so such a program
has no single right answer and comparing engines on it manufactures
mismatches that are not bugs. `ub_check.ts` calibrates it in both
directions — missing a real conflict is as wrong as flagging a defined one.

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

## Regression corpus (`seeds/`)

`ts/qemu-exec.ts` runs saved programs on the target against the reference
VM — one QEMU boot per batch. `seeds/` keeps one program per fixed finding
from the earlier byte-level campaign (docs/fuzzing-campaign.md), so

```sh
npx ts-node --transpile-only fuzz/ts/qemu-exec.ts seeds
```

is a standing check on all of them. `ts/make_seeds.ts` owns that directory
and is the only thing that writes there; every seed goes through
`validateProgram` before being written, because one that does not validate
is silently discarded on every execution.

`ts/minimize-exec.ts` is the instruction-level counterpart to
`gen/minimize.ts`, for a finding that arrives as an encoded program rather
than as a tree.

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
