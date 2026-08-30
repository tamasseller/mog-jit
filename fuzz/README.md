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
`Assembler::fail()`'s bail, which the harness treats as a pass (it ignores
the `RESOURCE_*` code — only whether control escaped matters here).

Input is one whole program envelope — `encodeJitProgram`'s
`max_call_depth`/`total_depth`/`proc_count` header, then each procedure.
Whole programs, not one procedure, because a lone procedure cannot legally
contain a `CALL` at all (§8.2 rejects self-recursion), which used to leave
the entire call path unreachable by the fuzzer.

Each input is translated twice, both passes through the one entry point
there is (`translateProc(procIdx, Runtime&, lruTick)` — every translation
emits into a real `Runtime`'s arena; there is no buffer-only variant).
Pass 1 gives each procedure the whole 64K arena and re-initialises the
`Runtime` between procedures, so one procedure's bail can't hide the ones
after it. Pass 2 re-runs them over an arena deliberately sized to the
program, several rounds, compiling only cold slots — that is what reaches
`findEvictionVictim`, `evict`'s compaction memmove and codePtr slides, and
`finalize`'s dispatch registration.

The drivers build `-m32`, because `Runtime` addresses its arena and every
`ProcSlot::bodyPtr` as a bare `uint32_t`; the arena is ordinary static
storage, so ASan catches an emitted halfword landing past `arenaEnd`.

`probe_arena.sh` reports, per arena size, whether a given program actually
reaches eviction — worth running after changing the sizing, since an arena
that is too generous silently exercises none of it. `repro.sh` builds and
replays one saved input (`last_input.bin` after a crash) through the same
harness. `build_afl.sh` is the coverage-guided build for a machine that has
AFL++ (this one doesn't, so `harness.cpp`'s own `main()` is a mutation loop
whose only feedback signal is validator approval — enough to keep the
corpus accumulating structure).

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

One thing it deliberately does not compare: a resource error, a legitimate
outcome with no reference counterpart. It is still counted and now bucketed
by its own `RESOURCE_*` code (design.md §12), which is what tells an arena
that wants growing from a corpus this target cannot compile at any size.
Everything else is comparable, traps at any call depth included —
`ProgramResult.trapped`'s own `LANDING_*` tag distinguishes the three
outcomes, so nothing is encoded in the result value for the two that are
compared (`docs/target-profile.md`).

## Seeds

`make_seeds.ts` owns `seeds/` — it is the only thing that writes there.
Every seed goes through `validateProgram` before being written, because a
seed that doesn't validate is silently discarded on every single execution:
the old hand-encoded `loop` seed had been doing exactly that.

`dump_seeds.sh` stages `test/corpus_programs.h`'s bodies (shared with
`test/qemu/test_nested_blocks.cpp`) into `seeds_raw/` for `make_seeds.ts` to wrap:

```sh
./dump_seeds.sh
TS_NODE_PROJECT=tsconfig.json npx ts-node --transpile-only make_seeds.ts
```

Every seed is run with a real argument vector, not zeros —
`fuzz/entry_args.ts` owns the one generator all three consumers share
(`oracle_server.ts`, `qemu_exec.ts`, `minimize_exec.ts`), because a
disagreement between any two of them would manufacture mismatches
indistinguishable from miscompilations. Values are distinct per index so a
permuted window changes the answer, and deliberately small: a wide value in
a slot some program uses as a loop counter turns a short countdown into
billions of steps, which the reference VM's watchdog then reports as "does
not terminate" and the sweep silently discards.

Beyond those, `make_seeds.ts` authors the multi-procedure/`CALL` shapes no
single-procedure format can express, and large shapes aimed at specific
*compiled-size* guards (a case body past the conditional-branch span, a
loop body past the back-edge's reach, a run of literal-pool constants, a
spill past `Uoff<2,8>`, deep nesting, a 100-case jump table). Blind
mutation from small seeds essentially never reaches any of those.
