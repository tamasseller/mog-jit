# jit-armv6m

A JIT compiler from the Generic Core ISA
(`packages/machine/docs/isa-core.md`) to ARMv6-M Thumb, for bare-metal
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
| `compiler/src/` | the native C++ translator, targeting `runtime/` — covers the full core instruction set, plus `ext.h`'s extension seam for wire bytes >= 128 (isa-core.md §11) — decode and effect declarations are in, codegen for an extension op is not yet (design.md §18), plus `armv6.h` (the Thumb instruction encoder it's built on, adapted from tamasseller/sdvm) |
| `runtime/` | the real dispatch/eviction runtime in C++ and hand-written asm, layered: `runtime.S` (call/return helpers, translator trampoline, `enter_dispatch`) and `dispatch_abi.h`/`.cpp` (the ABI's own fixed conventions both sides agree on — the helper vector, `trampolineAddr`, `runtimeBail`, the sentinel offset, the `LANDING_*` tags) are the dispatch layer; `executor.{h,cpp}` (`Executor` — memory configuration once, then one `run()` per encoded program blob, and the only interface an application needs) is initialization; `resource_codes.h` is the failure vocabulary, `program_frame.h` the wire frame binding a program to the validator that produced it, and `stack_budget.h` the stack-cost constants; `runtime.h` (`Runtime`, composing `code_arena.h`'s `CodeArena` and `dispatch_table.h`'s `DispatchTable`/`ProcSlot` — arena, stack floor, procedure directory, eviction/compaction) is shared internal state; `translate_proc.cpp` (the real `translateProc`, the landing every dispatch through an uncompiled slot reaches) hands off to `compiler/src/assembler.{h,cpp}` — the only thing that touches `Runtime` from inside a translation, owning arena growth/eviction and the literal pool |
| `test/host` | unit tests: `armv6.h`'s raw encoders, and the translator end to end, against real libc (`--coverage`) |
| `test/qemu` | the same translator plus the real, unmodified `runtime/`, run on `qemu-system-arm` — a harness-sanity check in `main.cpp` plus one `test_*.cpp` per topic — whole encoded programs (real wire bytes), arena eviction, and the up-front stack budget. `vectors.S`/`linker.ld` (bare-metal startup) live here — test-only, never shipped |
| `fuzz/` | differential fuzzing, two halves: `harness.cpp` runs the real translator on the host under ASan/UBSan on validator-approved whole programs (finds crashes); `fuzz/qemu_exec/` runs the *emitted Thumb* on `qemu-system-arm` against `@ppl/machine`'s reference VM (finds miscompilation, which the host half is structurally blind to). See `fuzz/README.md`, and docs/design.md §17 for what each half has actually caught |
| `vendor/` | submodules: `ultimate-makefile`, `1test` |

## Commands

```sh
make test        # everything below
make test-host   # test/host
make test-qemu   # test/qemu (needs qemu-system-arm)
```
