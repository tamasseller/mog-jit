# bench — measuring the JIT against C

All three workloads are in and measured. Run:

```sh
bench/plugin/selftest.sh                              # is the counter honest?
bench/check.sh                                        # do the extension's halves agree?
npx ts-node --transpile-only bench/check-workload.ts  # do the workloads agree?
bench/build.sh                                        # 18 images
npx ts-node --transpile-only bench/bench.ts
```

## Results

2048 vs 4096 samples differenced. Every configuration of every workload
agrees with the reference VM on its return value and on every byte it wrote.

| workload | JIT cycles/sample | vs -Os | vs best level | emitted / bytecode |
|---|---|---|---|---|
| pulse-trigger | 27.79 | 1.03x | 1.61x | 150 B / 83 B |
| iq-preamble | 19.23 | 2.06x | 2.38x | 210 B / 111 B |
| median5 | 151.00 | 1.99x | 2.10x | 298 B / 240 B |

Against docs/design.md: §14 predicted throughput within "roughly 2-4x" of
`-Os` C, and the measured 1.03x-2.06x sits at or below the bottom of that
range; against each workload's best level it is 1.61x-2.38x. §15 estimated
4-10 KB of flash for the whole JIT, and ~9.8 KB is inside it. Cold start is
21k-49k instructions depending on program size, paid once per procedure.

The three workloads are chosen to be different shapes, and they behave like
it:

**pulse-trigger** — a two-state machine with hysteresis, on unsigned ADC
codes. Almost no arithmetic, and the closest result: 1.03x against `-Os`.

**iq-preamble** — quadrature demodulation of a tone sampled at four times
its frequency, where the mixer coefficients are [1,0,-1,0] and [0,1,0,-1],
so there is no multiply in the demodulator at all. `I^2 + Q^2` against a
squared threshold avoids a square root the ISA does not have. This is the
only workload that reaches a `MUL`, and the only one whose accumulators
need the DSL's signed types for `>>` to mean ASR.

**median5** — Knuth's nine-comparator sorting network, verified
exhaustively over all 120 permutations rather than eyeballed. Cortex-M0 has
no IT blocks, so each compare-exchange is a real branch on both sides;
neither gets to hide the work in a conditional move.

### Where the JIT loses, concretely

**Registers, on median5.** 35 of its 147 emitted instructions are
SP-relative spill and fill — the 4-register TOS window against seven live
locals (the validator reports `totalDepth` 9). GCC has eight low registers
here and spills far less. This is the workload with the worst ratio against
`-O2`, and that is why.

**Range fusion and hoisting, on pulse-trigger.** GCC fuses
`run >= 12 && run <= 60` into one `subs`/`cmp`/`bhi`, and hoists the event
ring's count into a stack slot across the whole loop where the emitted code
reloads it per trigger. Both visible in the disassembly, both legitimate.

Where the JIT wins is `-O0`, by better than 2x on every workload — which is
worth stating only because "received bytecode is slower than compiled C" is
not unconditionally true.

## Where the cycle figures come from

QEMU is not cycle-accurate, and the microbit model's Cortex-M0 has no DWT
cycle counter to read, so nothing here is a measured cycle. Two numbers are
reported instead, and they are different kinds of thing:

**Instructions are exact.** QEMU reports precisely what executed, and that
is already a fair comparison on its own terms — the JIT-emitted Thumb and
the compiled kernels run on the same emulated core, so whatever the model
omits it omits from both sides equally.

**Cycles are modelled**, by weighting that exact instruction stream with ARM
DDI 0432C's timings. This is worth doing on a Cortex-M0 specifically and
would not be on a bigger core: no cache, no branch prediction, no store
buffer, zero-wait-state memory, so the timing genuinely is a static table
plus one dynamic term. Loads and stores cost 2, `LDM`/`STM`/`PUSH`/`POP`
cost 1 per register plus 1, `BL` 4, `BX`/`BLX` 3, everything else 1.

The one dynamic term is the taken-branch pipeline refill, and it is
observed rather than guessed: a conditional branch is costed at 1
statically, and the +2 is added only when the next translated block does
not start at the previous block's fall-through address. A not-taken branch
lands exactly on fall-through and costs nothing extra.

It matters. The JIT materializes pooled literals where C keeps values in
registers, so it issues proportionally more loads; weighting them moved the
`-O2` ratio from 1.52x to 1.54x and the `-Os` ratio from 1.13x to 1.03x.

`MUL` is a knob (`mul=` plugin argument, default 1) because Cortex-M0
permits both a 1-cycle and a 32-cycle multiplier and the choice is
implementation defined. Only `iq-preamble` reaches one, twice per detection
window rather than per sample, so even the 32-cycle variant would move its
figure by well under a cycle per sample — but the report says which it
assumed rather than leaving it to be discovered.

## Why every number is a difference

Nothing is measured once and reported. Each phase runs the same work over
`N1` and again over `N2`, and the difference becomes the per-sample figure.
Everything that happens once per phase rather than once per sample — JIT
translation, `Executor` setup, the call in and out, the region markers' own
instructions — is identical in both runs and cancels exactly. That is what
makes a JIT excursion comparable to a compiled function whose fixed
overheads are completely different, without either side having to estimate
its own overhead. `bench-config.ts` carries the arithmetic.

The same subtraction against an `N=0` phase gives the cold-start cost.

## What is not being claimed

**The stack figures are not comparable as printed.** On every workload the
JIT excursion and a translate-only run reach exactly the same depth, so
those 1.1-1.5 KB are the *translator's* peak and not the cost of running
the program — the programs' own operand stacks are 6 to 9 words. The
compiled kernels' 128 bytes is an ordinary call frame. Separating the JIT's
steady-state execution depth from its translation depth would need a
repaint from inside `Executor::run`, which there is no hook for.

**The fixed-footprint figure is approximate.** It sums `.text` by symbol
name prefix, not from a link map.

**Three workloads on one core.** Every number here is Cortex-M0 with
zero-wait-state memory and the single-cycle multiplier assumed. Nothing has
been measured on silicon, and nothing has been measured on a core with a
cache or a branch predictor, where the model this suite uses would not
apply at all.

## The sample-stream extension

Three opcodes, `sampstream_ext.ts` (reference half, carries the
specification) and `ext_sampstream.{h,cpp}` (target half):

| op | shape | emitted |
|---|---|---|
| `sample_at(i)` | index in acc → sign-extended `i16` in acc | 4 instructions |
| `out_at(i, v)` | value in acc, index popped | 4 instructions |
| `trigger(kind, i)` | index in acc, `kind` a literal; acc preserved | 11 instructions |

Every op emits inline. That is the point rather than a convenience: the
suite compares emitted Thumb against C doing the same work, and an op
reaching its data through `ExtSite::cHelperCall` would spend a dozen
instructions on the seam that the C side spends none on — the numbers would
then describe the seam, not the JIT. `kernels_ref.cpp`'s accessors are
`inline` for the same reason, and its `&` is deliberately not `&&` — both
comparisons run on both sides, so the ratio does not depend on how often
the first test happens to fail.

**The index lives in the program, not in the extension.** A stateful
`next_sample()` would keep its cursor in extension memory, costing a pooled
base plus a load and a store on every access, where C keeps the same cursor
in a register across the whole loop. An indexed ring hands the JIT's own
register allocator the cursor as an ordinary local, and look-back is then
`sample_at(i - n)` with no second op.

Indices are masked, never bounds-checked, exactly as in
`fuzz/rawmem_ext.ts`: the mask *is* the buffer size, so there is no trap
path for the two halves to disagree about.

## Correctness gates

`check.sh` gates the extension: it generates one program and its samples
from the reference half, links them with the real translator and the real
unmodified runtime, runs the emitted Thumb under QEMU, and compares the
return value, a hash of all 1024 output samples, the trigger count and every
event-ring slot against `@ppl/machine`'s VM.

`bench.ts` gates every measurement the same way, on every one of the six
images, before printing anything. A number from two sides that disagree
about the answer is not a measurement.

`check-workload.ts` gates a workload's DSL body against a TypeScript
transcription of the same machine, at both sample counts — a workload that
agreed at one length and not the other would give a per-sample figure with
no meaning.

`plugin/selftest.sh` gates both counters, against two hand-written
sequences whose cost is exact by construction, each differenced against a
region around nothing so the marker bias cancels rather than being assumed.
One covers the branch model — 401 instructions and 799 cycles, being `movs`
once, `subs` 200 times, then 199 taken `bne` at 3 and one untaken at 1,
which is the split a model charging every conditional branch alike would
get wrong. The other covers the memory and multi-register weights — 6
instructions and 10 cycles across `sub sp`/`push`/`ldr`/`str`/`pop`/`add
sp` — because those are what actually moved the reported ratios, and the
loop touches no memory at all.

## Reproducibility

Only `kernels_ref.cpp`'s optimization level varies across the six images, so
every JIT-side figure must come out identical in all of them, and `bench.ts`
fails if it does not.

That check earned its place immediately. `g_sampIn` originally sat in
ordinary `.rodata`, whose address moves with `.text`, which moves with the
reference kernels' optimization level — and `Assembler::materializeImm32`
chooses between a pooled literal and a two-instruction immediate sequence
*by the value*, so the JIT's own per-sample cost differed by a whole
instruction between images that were supposed to differ only in the C side
(19.05 against 18.05). `linker.ld` now pins the samples to a fixed address
clear of the image.

## Region markers

`bench_marks.h`'s `BENCH_REGION_MARKERS(name)` defines a pair of no-inline
functions; the driver reads their addresses out of the ELF with `nm` and
passes them to the plugin, which registers a callback at those two addresses
and nowhere else. The mechanism costs nothing elsewhere and is identical for
a compiled kernel and for code the JIT emitted moments earlier — the markers
bracket the `Executor::run` call, never anything inside the translated
program.

The plugin header is vendored (QEMU stable-7.2, plugin API version 1) and
the version is asserted, so a QEMU whose plugin ABI moved refuses to load it
rather than reporting a wrong number.
