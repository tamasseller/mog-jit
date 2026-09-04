// The two sample counts every measurement is taken at.
//
// Nothing is measured at one length and reported. Each phase runs the same
// work over N1 and again over N2, and the *difference* is what becomes a
// per-sample figure:
//
//     perSample = (insns(N2) - insns(N1)) / (N2 - N1)
//
// Everything that happens once per phase rather than once per sample —
// JIT translation, the Executor's own setup, the call in and out, the
// region markers' own instructions — is identical in both runs and cancels
// exactly. That is what makes the number comparable to a C kernel whose
// fixed overheads are completely different, without either side having to
// estimate its own overhead.
//
// The same subtraction gives the translation cost: a phase run at N=0 is
// everything except the per-sample work.
//
// Both stay inside SAMP_IN_SAMPLES so the input index never wraps, and N2
// is double N1 so the difference is as large as the input window allows.

export const BENCH_N0 = 0
export const BENCH_N1 = 2048
export const BENCH_N2 = 4096
