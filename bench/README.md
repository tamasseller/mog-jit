# bench — measuring the JIT against C

Status: end to end on one workload. The harness is complete; two of the
three workloads are not, and neither is cycle weighting.

```sh
bench/plugin/selftest.sh   # is the counter counting what it claims?
bench/check.sh             # do the extension's two halves agree?
bench/build.sh             # six images, one per optimization level
npx ts-node --transpile-only bench/bench.ts
```

## Results so far

`pulse-trigger`, 2048 vs 4096 samples differenced, all seven configurations
agreeing with the reference VM on the return value and on every trigger
event:

| level | insns/sample | vs JIT | kernel .text | stack (GCC) |
|---|---|---|---|---|
| **JIT** | **19.05** | 1.00x | 150 emitted + 81 bytecode | — |
| O0 | 36.28 | 0.52x | 148 | 32 |
| O1 | 12.59 | 1.51x | 156 | 40 |
| O2 | 12.54 | 1.52x | 156 | 32 |
| O3 | 12.54 | 1.52x | 156 | 32 |
| Os | 16.88 | 1.13x | 116 | 32 |
| Og | 14.21 | 1.34x | 112 | 20 |

Cold start is 21218 instructions, paid once per procedure. The fixed
footprint is ~9.8 KB of `.text` for translator plus runtime.

Against docs/design.md, on this one workload:

- §14 predicts throughput within "roughly 2-4x" of `-Os` C. Measured 1.13x
  against `-Os`, 1.52x against `-O2`/`-O3` — better than the estimate.
- §14 predicts 4-6x opcode expansion amortized with control flow. This
  workload is almost entirely control flow and expands 1.85x by bytes.
- §15 estimates 4-10 KB flash for the whole JIT. Measured ~9.8 KB, at the
  top of the range but inside it.

Two things GCC does here that the JIT structurally cannot, both visible in
the disassembly and both legitimate: it fuses `run >= 12 && run <= 60` into
a single `subs`/`cmp`/`bhi` range test, and it hoists the event ring's count
into a stack slot across the whole loop where the emitted code reloads it
per trigger.

## Why instructions, not cycles

QEMU is not cycle-accurate, and the microbit model's Cortex-M0 has no DWT
cycle counter to read. What QEMU can report exactly is how many
instructions executed, which is a fair comparison on its own terms: the
JIT-emitted Thumb and the compiled kernels run on the same emulated core, so
whatever the model omits, it omits from both sides equally.

A weighted Cortex-M0 cycle estimate on top of the count is a later
refinement, and one that has to state its assumptions — notably which
multiplier variant the core has, since the architecture permits both a
1-cycle and a 32-cycle `MUL`.

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

**The stack figures are not comparable as printed.** The JIT excursion peaks
at 1544 bytes and a translate-only run reaches exactly the same depth, so
that number is the *translator's* peak, not the cost of running the program
— the program's own operand stack is 6 words. The compiled kernel's 128
bytes is an ordinary call frame. Separating the JIT's steady-state
execution depth from its translation depth would need a repaint from inside
`Executor::run`, which there is no hook for.

**The fixed-footprint figure is approximate.** It sums `.text` by symbol
name prefix, not from a link map.

**One workload proves one thing.** This is the branchy, comparison-heavy
case. The arithmetic-heavy case (IQ demodulation) and the compare-heavy
nonlinear case (median filter) are the two that would move these ratios,
and both need the DSL's signed types.

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
`inline` for the same reason, and its `&` is deliberately not `&&`.

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

`plugin/selftest.sh` gates the counter: a region around a loop of exactly
known length against a region around nothing, so the marker bias cancels
rather than being assumed. It is 2 instructions per region, measured.

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
