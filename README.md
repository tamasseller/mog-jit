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
| `src/armv6.h` | Thumb instruction encoder (adapted from tamasseller/sdvm) |
| `src/vectors.S`, `src/linker.ld` | bare-metal startup for the QEMU test targets |
| `prototype/` | the translation algorithm in TypeScript, emitting real Thumb, run on `qemu-system-arm`. Same per-procedure logic as the envisioned JIT, run eagerly over a whole program so the translator can be iterated on without the arena/eviction runtime |
| `prototype/qemu/` | the real dispatch/eviction runtime in C++ and hand-written asm: `runtime.S` (call/return helpers, translator trampoline, `enter_dispatch`), `runtime_host.cpp` (the `enter_program` family), `compile_proc.cpp` (a mock translator that copies precompiled blobs, so dispatch and eviction are testable independently of compiler correctness) |
| `compiler/` | the native C++ port of the translator, targeting the real dispatch/eviction runtime only. Covers a straight-line opcode slice so far (docs/design.md §16 item 8) |
| `test/host`, `test/qemu` | encoder unit tests, and a QEMU smoke test |
| `compiler/test/host`, `compiler/test/qemu` | the native compiler's own unit and QEMU tests |
| `vendor/` | submodules: `ultimate-makefile`, `1test` |

`prototype` and `compiler` are two implementations of one algorithm, kept
deliberately eyeball-diffable: `compiler/src/*.h` name the `prototype/src/*.ts`
file each was ported from, and preserve its formulas verbatim.

## Commands

```sh
make test                   # everything below
make test-host              # test/host
make test-qemu              # test/qemu (needs qemu-system-arm)
make test-compiler-host     # compiler/test/host
make test-compiler-qemu     # compiler/test/qemu
```

The TypeScript prototype builds and runs through the monorepo workspace:

```sh
npm test --workspace @ppl/jit-armv6m-prototype
```
