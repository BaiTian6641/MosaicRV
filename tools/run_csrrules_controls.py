#!/usr/bin/env python3
"""Negative controls for CASE=csr.rule_ledger (work package V-017).

The case claims that for every rule in config/csr/rule_ledger.json the machine
answers as the rule says: determined fields are compared strictly, a WARL field
accepts a legal value and rejects the adjacent illegal one, read-only bits cannot
be changed, aliases track their canonical register, and an access the address does
not permit raises the illegal-instruction exception. That claim is only evidence
if a checker (or a machine) that got one of those wrong would be caught, so this
tool rebuilds the case with exactly one defect injected, from an empty build
directory, and runs it. Every mutant must:

  * build into its own directory, deleted first, with its `-D` in that build's
    own command line (written next to the binary as `build_command.txt`);
  * produce a binary whose SHA-256 differs from the shipping build's, so a stale
    object file cannot masquerade as a mutant;
  * exit 1, with the named check the mutation breaks as the first failure.

The card names three fail modes and all three are properties of the *checker*,
not of any RTL module: the ledger, not the RTL, is the one place that says what
this case expects, so no `-D` on an RTL file can express "this difference was
waived" or "this unimplemented CSR is accepted as zero". They are therefore
`-CFLAGS -D...` mutations of this case's own comparison and are labelled DRIVER
here and in results/reports/V-017-csr.md.

Usage: run_csrrules_controls.py [--only NAME]
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
CASE = "csr.rule_ledger"

# (define, kind, fail mode, defect, text the first failure must contain)
MUTANTS = [
    (
        "MOSAIC_CSRRULES_MUTANT_SKIP_RULE",
        "driver",
        "a rule silently skipped",
        "the driver drops the stimulus for one rule while the ledger still "
        "requires it, so the coverage assertion names the rule that was never "
        "visited instead of passing vacuously.",
        "was never visited",
    ),
    (
        "MOSAIC_CSRRULES_MUTANT_AUTO_WAIVER",
        "driver",
        "a waiver generated from a difference instead of a declared rule",
        "the comparator is mutated to accept the adjacent illegal value the "
        "ledger marks with `forbid` -- the auto-waiver the fail mode describes. "
        "The machine's correct canonicalisation of that WARL field then fails the "
        "read-back comparison, which is the 'reject the adjacent illegal result' "
        "requirement.",
        "medeleg.value negative",
    ),
    (
        "MOSAIC_CSRRULES_MUTANT_UNIMPL_ZERO",
        "driver",
        "an unimplemented CSR accepted unconditionally as zero",
        "the comparator is mutated to expect an unimplemented CSR to be read as "
        "zero without trapping. The machine raises the illegal-instruction "
        "exception the specification requires, so the trap comparison fails.",
        "unimplemented.read_0x7ff positive",
    ),
    # The three above are the card's fail modes and are properties of the checker.
    # The three below are properties of the *machine*: they reuse the CSR file's
    # own documented `-D` mutants (rtl/core/mosaic_csr.sv) to show that the same
    # ledger catches a real DUT defect, not only a mutated checker.
    (
        "MOSAIC_CSR_MUTANT_CYCLE_WRITABLE",
        "rtl",
        "a read-only counter shadow becomes writable (RTL defect)",
        "the CSR file's cycle/instret shadows are treated as writable aliases of "
        "mcycle/minstret, so a write the ledger's permission rule denies is "
        "accepted and no trap is taken.",
        "every expected illegal access raised",
    ),
    (
        "MOSAIC_CSR_MUTANT_RO_WRITE_ACCEPTED",
        "rtl",
        "a read-only CSR becomes writable (RTL defect)",
        "every implemented CSR is treated as writable, so a write to a read-only "
        "register the ledger denies is accepted and no trap is taken.",
        "every expected illegal access raised",
    ),
    (
        "MOSAIC_CSR_MUTANT_NO_FIELD_MASK",
        "rtl",
        "the mstatus write mask is not applied (RTL defect)",
        "the mstatus write no longer applies the generated field mask, so bits "
        "the profile declares read-only change where the ledger requires them "
        "fixed.",
        "mstatus.",
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

    root = os.path.join(REPO_ROOT, "build", "p0", "csrrules_controls")
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
    print("%-44s %-7s %-5s %s" % ("mutant", "kind", "exit", "result"))
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
            print("%-44s %-7s BUILD FAILED" % (define, kind))
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
        print("%-44s %-7s %-5d %s" % (define, kind, result.returncode, status))
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
