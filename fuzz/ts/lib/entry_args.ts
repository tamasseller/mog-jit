// The entry procedure's argument vector, for every part of the harness that
// needs one.
//
// Its own module, deliberately: qemu_exec.ts and minimize_exec.ts are both
// scripts whose top-level body runs a whole sweep, so importing the
// generator *from* either of them would launch one as a side effect of the
// import. Three consumers have to agree exactly — oracle_server.ts (which
// computes the reference result for the crash campaign), qemu_exec.ts
// (which computes the reference result AND writes the guest batch) and
// minimize_exec.ts (which must shrink towards the same program that
// failed). A generator that differed between any two of them would
// manufacture mismatches indistinguishable from real miscompilations.
export function entryArgsFor(argCount: number): number[]
{
    // Distinct and non-zero per index. An all-zero vector hides exactly the
    // bug this exists to catch: an uninitialized window register still
    // "matches" the reference when the word it happens to read is also zero.
    // Anything that swaps two arguments, reads the wrong window register,
    // or reverses the spill order now changes the result.
    //
    // Small, though — 0x11, 0x22, ... rather than something wide and
    // eye-catching. A large value here is actively harmful: plenty of
    // programs use their first argument as a loop counter, and seeding one
    // with 0xa5a50001 turns a two-iteration countdown into 2.7 billion
    // steps, which the reference VM's own watchdog then reports as "does
    // not terminate" and the sweep discards. Six seeds silently stopped
    // being compared that way. Distinctness is what catches a permuted
    // window; magnitude buys nothing.
    return Array.from({ length: argCount }, (_, i) => 0x11 * (i + 1))
}
