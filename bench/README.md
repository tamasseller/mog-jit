# bench — measuring the JIT against C

Status: the extension and the instruction counter are in and verified. The
workloads, the C reference matrix and the report are not.

Done:
- sample-stream extension, both halves, agreeing on emitted Thumb under QEMU
- QEMU TCG plugin counting executed instructions per region, calibrated

Open:
- the three workloads and their matched C kernels (blocked on the DSL's
  signed/sized primitive types)
- the C build matrix (`-O0 -O1 -O2 -O3 -Os -Og`) and the report table
- code-size and stack reporting, both of which have existing mechanisms to
  reuse (`test/qemu/measure_proc.h`, `fuzz/dump_code.cpp`,
  `test/qemu/stack_paint.cpp`, `tools/stack-margin.ts`)
- Cortex-M0 cycle weighting on top of the instruction count

## Why instructions, not cycles

QEMU is not cycle-accurate, and the microbit model's Cortex-M0 has no DWT
cycle counter to read. What QEMU can report exactly is how many
instructions executed, which is a fair comparison on its own terms: the
JIT-emitted Thumb and the C kernels run on the same emulated core, so
whatever the model omits, it omits from both sides equally.

A weighted Cortex-M0 cycle estimate on top of that count is a later
refinement, and one that has to state its assumptions — notably which
multiplier variant the core has, since the architecture permits both a
1-cycle and a 32-cycle `MUL`.

## The sample-stream extension

Three opcodes, `sampstream_ext.ts` (reference half, carries the
specification) and `ext_sampstream.{h,cpp}` (target half):

| op | shape | emitted |
|---|---|---|
| `sample_at(i)` | index in acc → sign-extended `i16` in acc | 4 instructions |
| `out_at(i, v)` | value in acc, index popped | 4 instructions |
| `trigger(kind, i)` | index in acc, `kind` a literal; acc preserved | 11 instructions |

Every op emits inline. That is the whole point rather than a convenience:
the suite compares emitted Thumb against C doing the same work, and an op
reaching its data through `ExtSite::cHelperCall` would spend a dozen
instructions on the seam that the C side spends none on — the numbers would
then describe the seam, not the JIT.

**The index lives in the program, not in the extension.** A stateful
`next_sample()` would keep its cursor in extension memory, costing a pooled
base plus a load and a store on every access, where C keeps the same cursor
in a register across the whole loop. An indexed ring hands the JIT's own
register allocator the cursor as an ordinary local, and look-back is then
`sample_at(i - n)` with no second op.

Indices are masked, never bounds-checked, exactly as in
`fuzz/rawmem_ext.ts`: the mask *is* the buffer size, so there is no trap
path for the two halves to disagree about. Mask and scale fold into two
shifts, one fewer than `ext_rawmem`'s `maskAndAlign` needs, because nothing
here aligns down from an arbitrary byte offset.

## Running what exists

```sh
bench/plugin/selftest.sh   # is the counter counting what it claims?
bench/check.sh             # do the extension's two halves agree?
bench/dump-emitted.sh      # what did the emitters actually encode?
```

`check.sh` is the gate: it generates one program and its input samples from
the reference half, links them into a bare-metal image with the real
translator and the real unmodified runtime, runs the emitted Thumb under
QEMU, and compares the return value, a hash of all 1024 output samples, the
trigger count and every event-ring slot against what `@ppl/machine`'s VM
computed for the same program. No benchmark number should be believed
before it passes.

`selftest.sh` is the same idea for the counter: a region around a loop of
exactly known length against a region around nothing, so the marker bias
cancels rather than being assumed. It is 2 instructions per region, and
measured, not reasoned about.

## Region markers

`bench_marks.h`'s `BENCH_REGION_MARKERS(name)` defines a pair of no-inline
functions; the host driver reads their addresses out of the ELF with `nm`
and passes them to the plugin, which registers a callback at those two
addresses and nowhere else. The mechanism costs nothing elsewhere and is
identical for a compiled C kernel and for code the JIT emitted moments
earlier — the markers bracket the `Executor::run` call, never anything
inside the translated program.

The plugin header is vendored (QEMU stable-7.2, plugin API version 1) and
the version is asserted, so a QEMU whose plugin ABI moved refuses to load it
rather than reporting a wrong number.
