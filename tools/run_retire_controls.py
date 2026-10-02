#!/usr/bin/env python3
"""Negative controls for CASE=retire.width_and_order (work package V-013).

The case claims the retirement path is exact: older before younger, a dense
`retire_order` with no duplicate, `x0` never written, and a slot-0 trap blocking
the younger slot. That claim is only evidence if a machine (or a checker) that
got one of those wrong would be caught, so this tool rebuilds the case with
exactly one defect injected, from an empty build directory, and runs it. Every
mutant must:

  * build into its own directory, deleted first, with its `-D` in that build's
    own command line (written next to the binary as `build_command.txt`);
  * produce a binary whose SHA-256 differs from the shipping build's, so a stale
    object file cannot masquerade as a mutant;
  * exit 1, with the named check the mutation breaks as the first failure.

The RTL mutants are the retire, ROB and rename module controls the project
already documents. Three of the card's four fail modes -- "a cycle's end state
contaminating an earlier slot", "slot order established by callback accident"
and "an unreported trace gap" -- are properties of the *checker*, not of one
gate, so those three are injected into this case's own comparison with
`-CFLAGS -D...`. They are labelled DRIVER in the table and in
results/reports/V-013-retire.md rather than passed off as RTL mutants.

Usage: run_retire_controls.py [--only NAME]
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import run_unit  # noqa: E402

REPO_ROOT = run_unit.REPO_ROOT
CASE = "retire.width_and_order"

# (define, kind, defect, text the first failure must contain)
# kind: "rtl" -> -D to Verilator, "driver" -> -CFLAGS -D to the C++ half.
#
# The card names four fail modes: "a cycle's end state contaminating an earlier
# slot", "slot order established by callback accident", "x0 modified" and "an
# unreported trace gap". The first three are properties of the *checker*, and the
# fourth is a property of the stream the checker watches, so those four are
# injected into this case's own comparison with -CFLAGS and are labelled DRIVER
# here and in results/reports/V-013-retire.md rather than passed off as RTL
# mutants. The one RTL mutant is the retire unit's own trap control.
MUTANTS = [
    (
        "MOSAIC_RETIRE_MUTANT_TRAP_AS_NORMAL",
        "rtl",
        "the trapping instruction takes the ordinary retirement path, so its "
        "payload reaches the stream as a retirement and its event carries no "
        "trap flag -- the phantom instruction the event contract warns about, and "
        "the entry behind it is free to retire in the same cycle.",
        "the trap event occupies lane 0 of the stream",
    ),
    (
        "MOSAIC_RETIRE_CHECKER_CYCLE_END",
        "driver",
        "the checker takes the older slot's expected value from the cycle's end "
        "state instead of from the model's own per-slot step, so the "
        "two-writes-to-one-rd cycle is the one it gets wrong.",
        "value",
    ),
    (
        "MOSAIC_RETIRE_CHECKER_LANE_REVERSED",
        "driver",
        "the checker consumes the model in the order the slots happened to be "
        "read rather than in lane order, which is the card's 'slot order "
        "established by callback accident'.",
        "pc",
    ),
    (
        "MOSAIC_RETIRE_CHECKER_X0_WRITE",
        "driver",
        "the expectation describes a write to x0 as a real destination write, so "
        "the comparison has to reject the machine's (correct) report at the "
        "per-slot destination check -- which is the check a machine that really "
        "modified x0 would trip.",
        "we=1, got x0 we=0",
    ),
    (
        "MOSAIC_RETIRE_CHECKER_DROP_EVENT",
        "driver",
        "the checker loses one event on the way to the comparison. Nothing about "
        "the machine changed; the trace gap detector is the only thing that can "
        "report it, and the card requires it to.",
        "retire_order is dense and strictly increasing",
    ),
]

# Mutants of the modules under the integrated core that were tried for a control
# and are *invisible* through it: each leaves the top-level architectural result
# bit-for-bit unchanged, so no case built on the integrated core can catch them,
# and this tool records them as an observation instead of pretending they are
# controls. The reason is required, because a mutant that passes is either a
# gap in the case or a property of the machine, and the difference matters.
INVISIBLE = [
    (
        "MOSAIC_RETIRE_MUTANT_SECOND_LANE_UNORDERED",
        "rtl",
        "the order rule is removed, but the acknowledgement filter below it "
        "still drops an ack for lane 1 with no ack for lane 0, so no out-of-order "
        "event is ever published; the module reports the fault on "
        "`o_order_fault`, which the integrated core does not expose.",
    ),
    (
        "MOSAIC_ROB_MUTANT_RETIRE_OVER_EXCEPTION",
        "rtl",
        "an exceptional macro is retirable, but the retire unit decides the trap "
        "from the entry's own exception flag, so the trap event is still taken "
        "and the entry still does not retire.",
    ),
    (
        "MOSAIC_RENAME_MUTANT_X0_ALLOC",
        "rtl",
        "a write to x0 allocates a physical tag, but the source-read path "
        "hardwires x0 to zero and the commit path excludes rd 0, so no "
        "architectural value changes; the defect is a tag leak, visible only as "
        "exhaustion after enough x0 writes.",
    ),
]


def build(entry: dict, case_id: str, mutants: list, build_dir: str) -> str:
    """Build one configuration from an empty directory; returns the log."""
    shutil.rmtree(build_dir, ignore_errors=True)
    os.makedirs(build_dir, exist_ok=True)

    rtl = entry.get("rtl", [])
    packages = [p for p in rtl if os.path.basename(p) == "mosaic_pkg.sv"]
    rest = [p for p in rtl if p not in packages]
    sources = [
        os.path.join(REPO_ROOT, p)
        for p in (packages + rest + entry.get("sv", []) + entry.get("cpp", [])
                  + run_unit.SHARED_CPP)
    ]
    cmd = ["verilator"] + run_unit.VERILATOR_FLAGS
    cmd += ["--top-module", entry["top"], "-Mdir", os.path.join(build_dir, "obj_dir")]
    cmd += ["-I%s" % os.path.join(REPO_ROOT, "build", "p0", "sim")]
    cmd += ["-I%s" % os.path.join(REPO_ROOT, "rtl", "core")]
    cmd += ["-I%s" % os.path.join(REPO_ROOT, "rtl", "common")]
    cmd += ["-I%s" % os.path.join(REPO_ROOT, "build", "p0", "rtl")]
    cmd += ["-CFLAGS", "-I%s" % run_unit.SIM_COMMON]
    cmd += ["-CFLAGS", "-I%s" % os.path.join(REPO_ROOT, "build", "p0", "sim")]
    for define, kind, _defect, _expected in mutants:
        if kind == "rtl":
            cmd += ["-D%s" % define]
        else:
            cmd += ["-CFLAGS", "-D%s" % define]
    cmd += ["-o", os.path.join(build_dir, case_id)]
    cmd += sources

    # The directory was deleted above, so this build cannot reuse a shipping
    # object; the command is written next to the binary so the mutant's `-D` is
    # on the record for whoever reads the evidence later.
    with open(os.path.join(build_dir, "build_command.txt"), "w") as handle:
        handle.write(" ".join(cmd))

    result = run_unit.run(cmd)
    log = result.stdout.decode("utf-8", "replace")
    if result.returncode != 0:
        raise RuntimeError("build failed for %s\n%s" % (mutants, log))
    return log


def run_case(binary: str, out_dir: str, max_cycles: int) -> subprocess.CompletedProcess:
    os.makedirs(out_dir, exist_ok=True)
    return subprocess.run(
        [binary, "--case", CASE, "--out", out_dir, "--seed", "1",
         "--max-cycles", str(max_cycles)],
        cwd=REPO_ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )


def sha256(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 16), b""):
            digest.update(chunk)
    return digest.hexdigest()


def first_line(log: str, prefix: str) -> str:
    for line in log.splitlines():
        if line.startswith(prefix):
            return line
    return "(no %s line)" % prefix


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only", default=None)
    args = parser.parse_args()

    registry = run_unit.load_registry()["cases"]
    if CASE not in registry:
        print("no such case: %s" % CASE)
        return 2
    entry = registry[CASE]
    max_cycles = entry.get("max_cycles", 4000000)

    root = os.path.join(REPO_ROOT, "build", "p0", "retire_controls")
    shipping_dir = os.path.join(root, "shipping")
    print("building the shipping case from an empty directory...")
    build(entry, CASE, [], shipping_dir)
    shipping = os.path.join(shipping_dir, CASE)
    result = run_case(shipping, os.path.join(root, "out-shipping"), max_cycles)
    baseline_ok = result.returncode == 0 and b"RESULT PASS" in result.stdout
    print("  baseline exit=%d %s" % (
        result.returncode, first_line(result.stdout.decode("utf-8", "replace"),
                                      "RESULT ")))
    if not baseline_ok:
        print("BASELINE FAILED: the shipping build must pass before mutants mean "
              "anything")
        return 1

    shipping_hash = sha256(shipping)
    print("shipping binary sha256: %s" % shipping_hash)
    print()
    print("%-46s %-7s %-5s %s" % ("mutant", "kind", "exit", "result"))
    print("-" * 118)
    failures = 0
    for define, kind, defect, expected in MUTANTS:
        if args.only is not None and args.only not in define:
            continue
        mutant_dir = os.path.join(root, define)
        try:
            build(entry, CASE, [(define, kind, defect, expected)], mutant_dir)
        except RuntimeError as error:
            print("%-46s %-7s BUILD FAILED" % (define, kind))
            print(error)
            failures += 1
            continue
        mutant = os.path.join(mutant_dir, CASE)
        differs = subprocess.run(["cmp", "-s", shipping, mutant]).returncode != 0
        result = run_case(mutant, os.path.join(root, "out-" + define), max_cycles)
        log = result.stdout.decode("utf-8", "replace")
        caught = result.returncode == 1 and expected in log
        status = "OK" if (differs and caught) else "MISS"
        if not differs:
            status += " (binary identical to shipping)"
        if result.returncode == 0:
            status += " (exit 0: unobservable or no check can fail)"
        elif result.returncode != 1:
            status += " (exit %d)" % result.returncode
        if result.returncode == 1 and not caught:
            status += " (expected %r)" % expected
        print("%-46s %-7s %-5d %s" % (define, kind, result.returncode, status))
        print("    injects: %s" % defect)
        print("    sha256:  %s" % sha256(mutant))
        print("    build:   %s" % os.path.relpath(
            os.path.join(mutant_dir, "build_command.txt"), REPO_ROOT))
        print("    %s" % first_line(log, "RESULT "))
        if not (differs and caught):
            failures += 1

    print("-" * 118)
    if failures:
        print("%d mutant(s) were not caught as required" % failures)
        return 1
    print("all %d mutants mutate the binary, exit 1 and name the check they break"
          % len(MUTANTS))

    # ------------------------------------------------------------------ probes
    # Mutants of the modules under the core that change nothing at the top. They
    # are run and their result printed, because "this defect is invisible here"
    # is a fact about the machine that a reader of the report needs, and because
    # a probe that suddenly starts failing would mean the core changed.
    print()
    if args.only is not None:
        print("--only: skipping the %d invisible-mutant probes" % len(INVISIBLE))
        return 0
    print("%-46s %-7s %-5s %s" % ("invisible-mutant probe", "kind", "exit", "result"))
    print("-" * 118)
    probes = 0
    for define, kind, reason in INVISIBLE:
        probe_dir = os.path.join(root, define)
        try:
            build(entry, CASE, [(define, kind, "", "")], probe_dir)
        except RuntimeError as error:
            print("%-46s %-7s BUILD FAILED" % (define, kind))
            print(error)
            probes += 1
            continue
        probe = os.path.join(probe_dir, CASE)
        differs = subprocess.run(["cmp", "-s", shipping, probe]).returncode != 0
        result = run_case(probe, os.path.join(root, "out-" + define), max_cycles)
        log = result.stdout.decode("utf-8", "replace")
        caught = result.returncode != 0
        if not differs:
            status = "PROBE INVALID (binary identical to shipping)"
            probes += 1
        elif caught:
            status = "NOW CAUGHT (the core changed; this must be re-triaged)"
            probes += 1
        else:
            status = "invisible, as recorded"
        print("%-46s %-7s %-5d %s" % (define, kind, result.returncode, status))
        print("    reason: %s" % reason)
        print("    sha256: %s" % sha256(probe))
    print("-" * 118)
    if probes:
        print("%d probe(s) need attention" % probes)
        return 1
    print("all %d probes are invisible through the integrated core, as recorded"
          % len(INVISIBLE))
    return 0


if __name__ == "__main__":
    sys.exit(main())
