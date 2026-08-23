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
| `runtime/` | the real dispatch/eviction runtime in C++ and hand-written asm: `runtime.S` (call/return helpers, translator trampoline, `enter_dispatch`), `runtime_host.cpp`/`.h` (the `enter_program` family), `runtime_internal.h` (`Runtime`/`DispatchEntry`, arena/eviction/compaction), `semihosting.cpp` |
| `compiler/` | the native C++ translator, targeting `runtime/`. Covers the full instruction set (`EXT` excluded on both sides by design) |
| `test/host`, `test/qemu` | encoder unit tests, and a QEMU smoke test |
| `compiler/test/host`, `compiler/test/qemu` | the native compiler's own unit and QEMU tests |
| `vendor/` | submodules: `ultimate-makefile`, `1test` |

A TypeScript prototype (`prototype/`) existed earlier as a faster-iteration
blueprint for working out the translation algorithm before committing it to
C++ — `compiler/src/*.h`'s own header comments still note which prototype
file a given piece was originally ported from, as a historical record. It
was retired once `compiler/` reached full feature parity; `runtime/` (the
actual dispatch/eviction runtime, genuinely shared infrastructure rather than
prototype-specific) is what survived the move, relocated rather than
deleted.

## Commands

```sh
make test                   # everything below
make test-host              # test/host
make test-qemu              # test/qemu (needs qemu-system-arm)
make test-compiler-host     # compiler/test/host
make test-compiler-qemu     # compiler/test/qemu
```
