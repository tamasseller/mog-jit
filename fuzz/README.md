# jit-armv6m/fuzz

Two halves, because they find different bugs and neither can find the
other's.

| | what runs | what it catches |
|---|---|---|
| `harness.cpp` + `oracle_server.ts` | the real translator on the host, under ASan/UBSan with asserts live | **crashes**: an assert, UB, an out-of-range encoding — on input a validator already approved |
| `qemu_exec/` | the *emitted Thumb*, on `qemu-system-arm`, against the real unmodified `runtime/` | **miscompilation**: no crash, no bail, just the wrong number |

The host half cannot see a miscompilation, because nothing in it ever
executes anything. The QEMU half cannot see an assert, because a bare-metal
image is built `-DNDEBUG`. Run both.

## Host half — crashes

```sh
# once, in another terminal (keeps @ppl/machine warm behind a socket)
npx ts-node --transpile-only jit-armv6m/fuzz/oracle_server.ts

./build.sh
./fuzz_driver seeds                      # one worker; run several for real throughput
PPL_FUZZ_CORPUS_OUT=/tmp/corpus ./fuzz_driver seeds   # also export for qemu_exec/
```

Every candidate is gated through `validateProgram` first (over a Unix
socket, so Node starts once rather than per test case), so a crash is a
real "runtime+compiler stability" bug by construction: the JIT is never
supposed to see anything else. The other acceptable outcome is
`Assembler::fail()`'s `RESOURCE_ERROR` bail, which the harness treats as a
pass.

Input is one whole program envelope — `encodeJitProgram`'s
`max_call_depth`/`total_depth`/`proc_count` header, then each procedure.
Whole programs, not one procedure, because a lone procedure cannot legally
contain a `CALL` at all (§8.2 rejects self-recursion), which used to leave
the entire call path unreachable by the fuzzer.

Each input is translated twice: once with a *detached* `Assembler` (fixed
buffer), then again with *attached* ones against a small real arena at a
fixed low address, which is what reaches `Runtime::allocate`/
`findEvictionVictim`/`evict`'s compaction memmove and `finalize`'s dispatch
registration. `probe_arena.cpp` is a diagnostic that reports, per arena
size, whether a given program actually reaches eviction — worth running
after changing the sizing, since an arena that is too generous silently
exercises none of it.

`repro.cpp` replays one saved input; `build_afl.sh` is the coverage-guided
build for a machine that has AFL++ (this one doesn't, so `harness.cpp`'s own
`main()` is a mutation loop whose only feedback signal is validator
approval — enough to keep the corpus accumulating structure).

## QEMU half — miscompilation

```sh
cd qemu_exec && ./build.sh && cd ..
npx ts-node --transpile-only jit-armv6m/fuzz/qemu_exec/qemu_exec.ts <dir-or-file>...
npx ts-node --transpile-only jit-armv6m/fuzz/qemu_exec/minimize_exec.ts <file>
```

`qemu_exec.ts` computes each program's reference result with
`@ppl/machine`'s VM, runs the real emitted code on the emulated target, and
diffs. `minimize_exec.ts` shrinks a mismatching program by deleting whole
instructions and re-encoding, so every candidate stays validator-approved —
one QEMU boot per pass, not per candidate.

One thing it deliberately does not compare: a `RESOURCE_ERROR`, a
legitimate outcome with no reference counterpart. Everything else is
comparable, traps at any call depth included —
`ProgramResult.trapped`'s own `LANDING_*` tag distinguishes the three
outcomes, so nothing is encoded in the result value
(`docs/target-profile.md`).

## Seeds

`make_seeds.ts` owns `seeds/` — it is the only thing that writes there.
Every seed goes through `validateProgram` before being written, because a
seed that doesn't validate is silently discarded on every single execution:
the old hand-encoded `loop` seed had been doing exactly that.

`dump_seeds.sh` stages `test/corpus_programs.h`'s bodies (shared with
`test/qemu/fixtures.cpp`) into `seeds_raw/` for `make_seeds.ts` to wrap:

```sh
./dump_seeds.sh
TS_NODE_PROJECT=tsconfig.json npx ts-node --transpile-only make_seeds.ts
```

Beyond those, `make_seeds.ts` authors the multi-procedure/`CALL` shapes no
single-procedure format can express, and large shapes aimed at specific
*compiled-size* guards (a case body past the conditional-branch span, a
loop body past the back-edge's reach, a run of literal-pool constants, a
spill past `Uoff<2,8>`, deep nesting, a 100-case jump table). Blind
mutation from small seeds essentially never reaches any of those.
