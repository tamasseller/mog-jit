# jit-armv6m

A JIT compiler from the Generic Core ISA
(`mog-core/docs/isa-core.md`) to ARMv6-M Thumb, for bare-metal
Cortex-M0/M0+ firmware that receives bytecode at runtime and needs to
execute it natively, compiling and evicting procedures on demand under a
hard memory ceiling.

## Docs

- [docs/design.md](docs/design.md): the full design. Memory layout, register
  assignment, the 4-register TOS window, calling convention, dispatch and
  eviction ABI, the translation pipeline and its `acc` fusion state machine,
  position independence under compaction, and a worked hand-translation.
  §16 tracks what is verified on hardware and what is still open.
- `docs/armv6m-reference-manual.pdf`: ARM DDI 0419E, the architecture
  reference this design is checked against.

## Layout

| Path | What |
|---|---|
| `src/compiler/` | the native C++ translator, targeting `src/runtime/` — covers the full core instruction set, plus `ext.h`'s extension seam for wire bytes >= 128 (isa-core.md §11): decode, effect declarations and codegen, through `ExtSite`'s emission surface (design.md §18; only §18.1's per-excursion extension state is unbuilt). Three layers, each depending only downwards: `infra/` (`armv6.h`, the Thumb encoder adapted from tamasseller/sdvm, plus `instr.h`/`registers.h`/`effect.h`/`ext.h` and the assembler, decoder and body scanner), `emit/` (the emitters and the state they track — `arithmetic`, `window`, `abi_strategy`, `ext`, `accstate`/`shape`/`flagstate`), and `translate/` (the bytecode-driven logic on top) |
| `runtime/` | the real dispatch/eviction runtime in C++ and hand-written asm, layered: `runtime.S` (call/return helpers, translator trampoline, `enter_dispatch`) and `dispatch_abi.h`/`.cpp` (the ABI's own fixed conventions both sides agree on — the helper vector, `trampolineAddr`, `runtimeBail`, the sentinel offset, the `LANDING_*` tags) are the dispatch layer; `executor.{h,cpp}` (`Executor` — memory configuration once, then one `run()` per encoded program blob, and the only interface an application needs) is initialization; `resource_codes.h` is the failure vocabulary, `program_frame.h` the wire frame binding a program to the validator that produced it, and `stack_budget.h` the stack-cost constants; `runtime.h` (`Runtime`, composing `code_arena.h`'s `CodeArena` and `dispatch_table.h`'s `DispatchTable`/`ProcSlot` — arena, stack floor, procedure directory, eviction/compaction) is shared internal state; `translate_proc.cpp` (the real `translateProc`, the landing every dispatch through an uncompiled slot reaches) hands off to `src/compiler/infra/assembler.{h,cpp}` — the only thing that touches `Runtime` from inside a translation, owning arena growth/eviction and the literal pool |
| `src/**/*.mk` | what each directory contributes to a build: `SOURCES` and `INCLUDE_DIRS`, in paths resolved from the fragment's own location so any consumer can include it. `-all` is a component's own contribution and chains to exactly one fragment below it, so a fragment is never reached twice and none needs a guard: `compiler-all.mk` is the whole translator, `runtime-all.mk` the portable runtime state, `-headers` variants contribute include directories without objects, and `executor`/`dispatch`/`bytecode-default`/`ext-default` are additions a consumer names when it wants them. `tools/srclist.sh` resolves a fragment set for the plain-`g++` scripts under `fuzz/` and `bench/` |
| `support/` | what the three harnesses share, one component per directory, each with its own `.mk`: `qemu-image/` (vector table, C++ stubs, the minimal semihosting console and the ARM toolchain and flags every bare-metal image is built with), `bytecode/` (the hand-encoder, the shared program corpus, the whole-program envelope format), `ext-rawmem/` (a real §11 extension used as the subject wherever the seam itself is under test, target and host builds), `dump-code/` (translate an envelope on the host and write out the emitted Thumb, with whichever extension the consumer links) |
| `test/host` | unit tests: `armv6.h`'s raw encoders, and the translator end to end, against real libc (`--coverage`) |
| `test/qemu` | the same translator plus the real, unmodified `runtime/`, run on `qemu-system-arm` — a harness-sanity check in `main.cpp` plus one `test_*.cpp` per topic — whole encoded programs (real wire bytes), arena eviction, and the up-front stack budget. `vectors.S`/`linker.ld` (bare-metal startup) live here — test-only, never shipped |
| `fuzz/` | differential fuzzing, two halves: `src/driver/` runs the real translator on the host under ASan/UBSan on validator-approved whole programs (finds crashes); `src/qemu-exec/` runs the *emitted Thumb* on `qemu-system-arm` against `mog-core`'s reference VM (finds miscompilation, which the host half is structurally blind to). One directory per native output under `src/`, each with its own `Makefile`; `ts/` is one program per file with `ts/lib/` for what they share. See `fuzz/README.md`, and docs/design.md §17 for what each half has actually caught |
| `bench/` | measuring emitted Thumb against C on realistic DSP workloads: the `sampstream` extension (both halves, `check.sh` gating them against the reference VM on real emulated hardware) and a QEMU TCG plugin counting executed instructions per region. See `bench/README.md` for what is measured and what is not yet built |
| `vendor/` | submodules: `ultimate-makefile`, `1test` |

## Commands

```sh
make test        # everything below
make test-host   # test/host
make test-qemu   # test/qemu (needs qemu-system-arm)
```
