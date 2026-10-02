#!/usr/bin/env python3
"""Negative controls for CASE=trap.precise_state (work package V-014).

The case claims that a synchronous trap leaves precise state: the faulting
instruction does not retire as a success, older effects are complete, younger
effects are invisible, and every specified CSR and PC is exactly right. That
claim is only evidence if a machine (or a checker) that got one of those wrong
would be caught, so this tool rebuilds the case with exactly one defect
injected, from an empty build directory, and runs it. Every mutant must:

  * build into its own directory, deleted first, with its `-D` in that build's
    own command line (written next to the binary as `build_command.txt`);
  * produce a binary whose SHA-256 differs from the shipping build's, so a stale
    object file cannot masquerade as a mutant;
  * exit 1, with the named check the mutation breaks as the first failure.

The card names four fail modes. Two of them are properties of the machine and
are injected into the RTL modules this project already documents as controls
(`MOSAIC_RETIRE_MUTANT_TRAP_AS_NORMAL` for "an exception's writeback polluting
`rd` first", and `MOSAIC_CORE_MUTANT_MRET_PC_WRONG` for "an instruction being
lost on replay after the trap"). The other two are properties of the *checker*
-- "a wrong `mtval` being masked wholesale" and "the reference model taking one
instruction too many" -- and no RTL mutant can express them, because they are
statements about what this case's own comparison expects; those two are injected
into this case's comparison with `-CFLAGS -D...` and are labelled DRIVER below
and in results/reports/V-014-trapstate.md.

Two further driver controls are included because they make the card's first and
fourth rules fail at the *snapshot* and *replay* level rather than only at the
retirement stream, which is the layer the case is really about.

Usage: run_trapstate_controls.py [--only NAME]
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
CASE = "trap.precise_state"

# (define, kind, fail mode, defect, text the first failure must contain)
# kind: "rtl" -> -D to Verilator, "driver" -> -CFLAGS -D to the C++ half.
MUTANTS = [
    (
        "MOSAIC_RETIRE_MUTANT_TRAP_AS_NORMAL",
        "rtl",
        "1. an exception's writeback polluting `rd` first",
        "the trapping entry takes the ordinary retirement path, so its payload "
        "reaches the committed map and the event stream as a retirement and its "
        "writeback is applied -- the phantom instruction the event contract "
        "records, and the pollution the card names.",
        "retire",
    ),
    (
        "MOSAIC_TRAPSTATE_MUTANT_MTVAL_MASKED",
        "driver",
        "2. a wrong `mtval` being masked wholesale",
        "the checker is mutated to the wholesale-masked `mtval` the fail mode "
        "describes: it expects zero for every trap, so a machine that reported "
        "the real faulting address fails the per-field `mtval` comparison.",
        "mtval",
    ),
    (
        "MOSAIC_TRAPSTATE_MUTANT_REF_EXTRA_INSN",
        "driver",
        "3. the reference model taking one instruction too many",
        "the reference's own stream is mutated to treat each trap as an ordinary "
        "retirement, so it expects the faulting instruction to retire where the "
        "machine publishes a trap; the per-event comparison fails at that PC.",
        "the retirement stream follows the reference",
    ),
    (
        "MOSAIC_CORE_MUTANT_MRET_PC_WRONG",
        "rtl",
        "4. an instruction being lost on replay after the trap",
        "`mret` returns to `mepc + 4` instead of `mepc`, so the instruction after "
        "the faulting one is skipped on the replay path: an instruction is lost, "
        "and the retirement stream and the final state both say so.",
        "retire",
    ),
    (
        "MOSAIC_TRAPSTATE_MUTANT_RD_POLLUTED",
        "driver",
        "1 (snapshot view). an exception's writeback polluting `rd` first",
        "the checker is mutated to the expectation of a machine that polluted "
        "the load's destination: it expects `x20` to hold the faulting load's "
        "(zero) result at trap entry, which the correct machine's frame fails.",
        "x20",
    ),
    (
        "MOSAIC_TRAPSTATE_MUTANT_REPLAY_LOST",
        "driver",
        "4 (replay view). an instruction being lost on replay after the trap",
        "the checker is mutated to believe the younger store behind the trap was "
        "lost on replay, so it expects the probe to still hold its pre-value; the "
        "store actually re-executed, so the replay check fails.",
        "the post-trap store reached memory",
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
    for define, kind, _mode, _defect, _expected in mutants:
        if kind == "rtl":
            cmd += ["-D%s" % define]
        else:
            cmd += ["-CFLAGS", "-D%s" % define]
    cmd += ["-o", os.path.join(build_dir, case_id)]
    cmd += sources

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

    root = os.path.join(REPO_ROOT, "build", "p0", "trapstate_controls")
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
    ran = 0
    for define, kind, mode, defect, expected in MUTANTS:
        if args.only is not None and args.only not in define:
            continue
        ran += 1
        mutant_dir = os.path.join(root, define)
        try:
            build(entry, CASE, [(define, kind, mode, defect, expected)], mutant_dir)
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
        print("    fail mode: %s" % mode)
        print("    injects:   %s" % defect)
        print("    sha256:    %s" % sha256(mutant))
        print("    build:     %s" % os.path.relpath(
            os.path.join(mutant_dir, "build_command.txt"), REPO_ROOT))
        print("    %s" % first_line(log, "RESULT "))
        print("    first:     %s" % first_line(log, "MISMATCH "))
        if not (differs and caught):
            failures += 1

    print("-" * 118)
    if failures:
        print("%d of %d mutant(s) were not caught as required" % (failures, ran))
        return 1
    print("all %d mutants mutate the binary, exit 1 and name the check they break"
          % ran)
    return 0


if __name__ == "__main__":
    sys.exit(main())
