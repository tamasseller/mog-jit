/**
 * Test entry point for @ppl/jit-armv6m-prototype.
 * Run via: npm test (from this package) or npm test (from root)
 */
import "./leb128.test"
import "./algorithms.test"
import "./call.test"
import "./rotation.test"
import "./deep-args.test"
import "./br-table.test"
import "./loop.test"
import "./loop-merge.test"
import "./case-terminator-close.test"
import "./branch-range.test"
import "./peek-peek-inplace.test"
import "./unary-and-comparison-values.test"
import "./last-arg-fold.test"
import "./bytecodeReader-decode.test"
import "./procDirectory.test"
import "./comparison-fusion.test"
import "./abi-dispatch.test"
import "./eviction.test"
import "./enter-program-variants.test"
import "./le-condition-encoding.test"
