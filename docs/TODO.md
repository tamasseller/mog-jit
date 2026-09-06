# TODO

## Open

**The frame does not bind the extension set.** §1.1's frame seeds on
`PROGRAM_CONTRACT_VERSION` alone, so a program built against one extension
and run on an image linking another (or none) passes it. That is why
`RESOURCE_PROGRAM_EXT_UNKNOWN` and `RESOURCE_PROGRAM_EXT_UNSUPPORTED` are
still runtime checks where every other `PROGRAM`-class wire check is now an
assert (§12). Folding an extension identity into the frame seed retires both
and costs no flash — the check already exists. Needs an identity: `Extension`
(mog-core `extension.ts`) has no name field, and the C++ side would need a
weak `extIdentity()` beside `extDescribe`/`extEmit`. Contract change, so a
`PROGRAM_CONTRACT_VERSION` bump.

**Nothing host-side rejects a call-shaped extension.** mog-core's validator
supports `effect.calleeOf`; this backend cannot compile it
(`RESOURCE_PROGRAM_EXT_UNSUPPORTED`, and `Executor::run`'s stack budget rests
on its absence). The rejection belongs in `encodeJitProgram`, which is
already mog-jit-specific, rather than on the target.

**`translate_control_flow.cpp`'s back-edge TOS restore is unreached.** The
`window.tos != entryTos` arm after a loop's condition block is covered by
neither `test/host` nor any fuzz campaign. If validated RTL cannot leave the
operand stack unbalanced there, it is an assert; establish which before
removing it.

**A long-lived target service is possible but no longer justified.**
`fuzz/src/readc-spike/` settles the open question: SYS_READC does work on
this QEMU/machine, but only through an explicit `-chardev`, and it is
non-blocking and lossy — an empty buffer reads 0, and interleaving SYS_WRITE0
on the same chardev drops every other input byte. All three reasons to build it have
since gone. The 24KB batch window that forced chunking is now 128KB
(`BATCH_LIMIT`, one chunk per 250-program batch instead of four). Coverage
says the campaign is reach-bound rather than throughput-bound — 400
candidates reach 93.6% of translator lines and 30000 reach 95.5%, while
target edge coverage plateaus at ~620 of 2048 bits after about 1000. And the
20s hang budget now costs nothing: the one hang a campaign ever recorded was
fuzzing-campaign.md §10, and 40000 candidates since the fix have produced
none. What a SysTick watchdog would still buy is the *diagnostic* — a hang
would come back as `LANDING_CANCELLED` naming its program, rather than a
dead batch the driver attributes by counting result lines.
