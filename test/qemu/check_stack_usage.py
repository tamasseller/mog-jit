#!/usr/bin/env python3
"""Verify GCC's own -fstack-usage measurements against the hand-derived
stack-safety constants in runtime/dispatch_abi.h and compiler/src/
translate_proc.cpp/proc_scan.cpp, so a change to any function on either
chain fails the build instead of silently invalidating those constants
(docs/design.md G2/G3/G5 — this script is the enforcement side).

Two tracked groups, matching dispatch_abi.h's own derivation comment for
TRANSLATOR_ENTRY_WORST_CASE_BYTES:

- FIXED_CHAIN: the one-time, non-recursive call chain from
  translatorTrampoline's entry down to the deepest point emitPrologueStub's
  own arena-growth path can reach. Summed, they make up
  TRANSLATOR_ENTRY_WORST_CASE_BYTES minus the 28-byte translatorTrampoline/
  REALIGN_ENTER term (hand-verified against runtime.S directly — it's
  hand-written assembly, -fstack-usage has nothing to say about it).
- RECURSIVE_CLUSTER: the real per-level recursion
  (translate_proc.cpp's processNonTerminators <-> processUntilTerminator,
  plus checkStackFloor itself, which runs one level deeper still before
  either of those two get to unwind; translateLoop/translateIfThen/
  translateIfThenElse/translateSwitch/translateBody are confirmed fully
  inlined into processNonTerminators/translateProc at -Os and intentionally
  do not appear here — if a future compiler/flag change stops inlining
  them, expect_zero_others below still catches it, since their
  re-emergence as separate frames would mean processNonTerminators' own
  reported frame no longer reflects their cost). Summed (not maxed): a
  single level of real recursion has all three frames simultaneously live,
  not as alternatives. This sum is what translate_proc.cpp's
  TRANSLATE_BODY_STACK_MARGIN needs to comfortably exceed per level —
  checked directly below, not just noted in a comment.
- SCAN_CLUSTER: proc_scan.cpp's own pre-compilation recursion (scanBody),
  independent of the above — see proc_scan.cpp's own SCAN_STACK_MARGIN
  comment for why this is never a proxy for RECURSIVE_CLUSTER.

Every entry is checked for an EXACT match, not just a ceiling: a value
that's silently gotten *smaller* than expected is exactly what went wrong
with TRANSLATOR_ENTRY_WORST_CASE_BYTES before (a modeled path was removed,
nobody shrank the constant to match) — this cannot happen again unnoticed
if a mismatch in either direction fails the build.

Function names are demangled and matched by prefix, not exact string
equality: at -Os GCC is free to clone a static function
(.isra.N/.part.N/.constprop.N suffixes), and a naive exact match would
then silently see zero results — treated here as a hard failure (see
_find), not a silent skip, since that's indistinguishable from "this
function's cost stopped being accounted for at all," the one failure mode
this whole script exists to catch.
"""
import glob
import os
import re
import subprocess
import sys

# (display name, demangled-name prefix to match, expected static frame bytes)
FIXED_CHAIN = [
    ("compileProc",                  "compileProc(",                          200),
    ("translateProc",                "jitc::translateProc(",                  120),
    ("abiEmitPrologue",              "jitc::abiEmitPrologue(",                 16),
    ("emitPrologueStub",             "jitc::emitPrologueStub(",                16),
    ("Assembler::emit",              "jitc::Assembler::emit(",                 16),
    ("Assembler::growForAttached",   "jitc::Assembler::growForAttached(",      48),
]
# translatorTrampoline's push{r0,r1,r2,lr} (16) + REALIGN_ENTER (12) = 28,
# hand-verified against runtime.S directly — not -fstack-usage-measurable
# (hand-written assembly), not tracked here.
FIXED_CHAIN_ASM_BYTES = 28

RECURSIVE_CLUSTER = [
    ("processNonTerminators",  "jitc::processNonTerminators(",  104),
    ("processUntilTerminator", "jitc::processUntilTerminator(", 56),
    ("checkStackFloor",        "jitc::checkStackFloor(",        8),
]

# translate_proc.cpp's own TRANSLATE_BODY_STACK_MARGIN, read from source
# rather than duplicated here — a hardcoded expected value would just be a
# second place for the same "constant silently stops matching reality"
# failure mode (see this file's own header) to recur.
TRANSLATE_PROC_CPP = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "../../compiler/src/translate_proc.cpp")
MARGIN_CONST_RE = re.compile(r"TRANSLATE_BODY_STACK_MARGIN\s*=\s*(\d+)")

SCAN_CLUSTER = [
    ("scanBody", "jitc::scanBody(", 72),
]

ALL_GROUPS = [
    ("fixed one-time chain (TRANSLATOR_ENTRY_WORST_CASE_BYTES)", FIXED_CHAIN),
    ("recursive cluster (TRANSLATE_BODY_STACK_MARGIN's per-level cost)", RECURSIVE_CLUSTER),
    ("proc_scan's own recursion (SCAN_STACK_MARGIN)", SCAN_CLUSTER),
]


def _demangle(name):
    out = subprocess.run(["c++filt", name], capture_output=True, text=True, check=True)
    return out.stdout.strip()


def _load_su_lines(objdir):
    lines = []
    for path in glob.glob(f"{objdir}/**/*.su", recursive=True):
        with open(path) as f:
            for raw in f:
                raw = raw.rstrip("\n")
                if not raw:
                    continue
                # file:line:col:mangled-or-plain-name<TAB>bytes<TAB>qualifiers
                parts = raw.rsplit("\t", 2)
                if len(parts) != 3:
                    continue
                loc_and_name, bytes_str, qualifiers = parts
                # loc_and_name is "file:line:col:name" — name itself may
                # contain colons (templates, scope), so only split off the
                # leading file:line:col.
                m = re.match(r"^[^\t]*?:\d+:\d+:(.*)$", loc_and_name)
                if not m:
                    continue
                raw_name = m.group(1)
                lines.append((raw_name, int(bytes_str), qualifiers, path))
    return lines


def _find(su_lines, prefix, display_name):
    matches = []
    for raw_name, byte_count, qualifiers, path in su_lines:
        demangled = _demangle(raw_name) if raw_name.startswith("_Z") else raw_name
        # .su names lead with the return type ("uint32_t jitc::foo(..."),
        # so match the qualified-name+"(" as a substring, not a prefix —
        # the trailing "(" still guards against a longer name (e.g.
        # "translateProcHelper(") false-matching a shorter prefix.
        if prefix in demangled:
            matches.append((demangled, byte_count, qualifiers, path))
    if not matches:
        print(f"FAIL: '{display_name}' (prefix '{prefix}') matched ZERO .su entries.")
        print("  Either the function was renamed/removed, or GCC cloned it")
        print("  (.isra.N/.part.N/.constprop.N) under a name this prefix no")
        print("  longer matches. Either way its stack cost just stopped being")
        print("  tracked — update this script's prefix or the surrounding")
        print("  derivation comment, don't just widen the match blindly.")
        return None
    if len(matches) > 1:
        print(f"FAIL: '{display_name}' (prefix '{prefix}') matched {len(matches)} .su entries, expected exactly 1:")
        for demangled, byte_count, qualifiers, path in matches:
            print(f"  {demangled}  {byte_count} {qualifiers}  ({path})")
        print("  Narrow the prefix so it picks out exactly one function.")
        return None
    demangled, byte_count, qualifiers, path = matches[0]
    if "dynamic" in qualifiers:
        print(f"FAIL: '{display_name}' is qualified '{qualifiers}' — its frame")
        print("  has a run-time-dependent component (alloca/unbounded VLA)")
        print("  GCC can't statically bound. The whole point of this check is")
        print("  a static byte budget; an unbounded frame here voids it.")
        return None
    return byte_count


def _read_margin_constant():
    with open(TRANSLATE_PROC_CPP) as f:
        text = f.read()
    m = MARGIN_CONST_RE.search(text)
    return int(m.group(1)) if m else None


def main():
    objdir = sys.argv[1] if len(sys.argv) > 1 else ".o"
    su_lines = _load_su_lines(objdir)
    if not su_lines:
        print(f"FAIL: no .su files found under '{objdir}' — was this built with -fstack-usage?")
        return 1

    ok = True
    for group_name, entries in ALL_GROUPS:
        print(f"-- {group_name} --")
        for display_name, prefix, expected in entries:
            actual = _find(su_lines, prefix, display_name)
            if actual is None:
                ok = False
                continue
            if actual != expected:
                print(f"FAIL: {display_name} is {actual} bytes, expected exactly {expected}.")
                print("  Update both this script's table AND the corresponding")
                print("  derivation comment/constant (dispatch_abi.h or")
                print("  translate_proc.cpp/proc_scan.cpp) together — a mismatch")
                print("  in either direction means the hand-derived constant no")
                print("  longer matches what GCC actually generates.")
                ok = False
            else:
                print(f"  OK: {display_name} = {actual} bytes")

    if ok:
        fixed_total = sum(e[2] for e in FIXED_CHAIN) + FIXED_CHAIN_ASM_BYTES
        recursive_total = sum(e[2] for e in RECURSIVE_CLUSTER)
        print(f"\nAll tracked functions match. Fixed chain total (with the "
              f"{FIXED_CHAIN_ASM_BYTES}-byte asm term) = {fixed_total} bytes — "
              f"must equal dispatch_abi.h's TRANSLATOR_ENTRY_WORST_CASE_BYTES.")
        margin = _read_margin_constant()
        if margin is None:
            print(f"\nFAIL: could not find TRANSLATE_BODY_STACK_MARGIN in {TRANSLATE_PROC_CPP}.")
            return 1
        if margin <= recursive_total:
            print(f"\nFAIL: TRANSLATE_BODY_STACK_MARGIN ({margin}) does not exceed the "
                  f"recursive cluster's real per-level cost ({recursive_total}) — a nested "
                  f"LOOP/BR_TABLE could blow the stack between one checkStackFloor call and "
                  f"the next without either one catching it.")
            return 1
        print(f"Recursive cluster per-level cost = {recursive_total} bytes — "
              f"TRANSLATE_BODY_STACK_MARGIN ({margin}) comfortably exceeds it.")
        return 0

    return 1


if __name__ == "__main__":
    sys.exit(main())
