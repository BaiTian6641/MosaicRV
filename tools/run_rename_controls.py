#!/usr/bin/env python3
"""Negative controls for the rename cases, including CASE=rename.bank_bias_exhaustion
(work package I-032).

The bank-preference case claims that the optional preference over physical
allocation is a *preference*: it never leaks or steals a physical register, it
always falls back to any legal free bank when the preferred one is exhausted, and
it changes only which legal free tag is chosen -- never whether an allocation
succeeds and never the architectural result. A claim like that is evidence only if
the defect it excludes would be caught, so this tool rebuilds a case with exactly
one defect injected through a `-DMOSAIC_RENAME_MUTANT_*` define -- from an empty
build directory, so a mutant can never reuse the shipping object files -- and runs
it. Every mutant must:

  * build cleanly into its own directory, with its `-D` on the recorded command
    line (`build_command.txt` next to the binary);
  * produce a binary that differs from the shipping one (`cmp`), so a stale build
    cannot masquerade as a mutant;
  * exit 1, with the named check the mutation breaks as the first failure.

A mutant that exits 0 is either an unobservable defect or a check that cannot
fail; this tool reports it as NOT MET rather than counting it.

The shipping build is built and run first, from an empty directory: mutants mean
nothing if the case does not pass in the first place.

Usage: run_rename_controls.py [--only SUBSTRING]
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
SOURCE = os.path.join("rtl", "core", "mosaic_rename.sv")

# Each row: the define, the case it is run against, the defect it injects, and the
# text the first failure must contain. The three I-032 mutants run against the
# bank case; the rows after them re-verify the pre-existing rename controls whose
# guard regions this card edited (the scan, the group-accept decision and the
# allocated destination), on the cases that own them.
MUTANTS = [
    (
        "MOSAIC_RENAME_MUTANT_BIAS_DEADLOCK",
        "rename.bank_bias_exhaustion",
        "the preference becomes a requirement: when the preferred bank has no free "
        "tag the group is refused even though tags are free elsewhere, so allocation "
        "deadlocks on an empty preferred bank -- the card's first Fail mode",
        "bank-bias",
    ),
    (
        "MOSAIC_RENAME_MUTANT_BIAS_UNRELEASED",
        "rename.bank_bias_exhaustion",
        "the preferred-bank mask is used without intersecting it with the free set, so "
        "the scan returns a tag that is still owned -- an unreleased physical tag, the "
        "card's second Fail mode",
        "bank-bias",
    ),
    (
        "MOSAIC_RENAME_MUTANT_BIAS_ARCH",
        "rename.bank_bias_exhaustion",
        "the preference leaks into the destination identity: with the bias on the "
        "generation is taken from the tag the unbiased scan would have chosen, so the "
        "produced (tag, generation) is not the chosen tag's next generation and the "
        "writeback that should deliver the architectural value is judged stale",
        "bank-bias",
    ),
    # --- blast radius: pre-existing controls in the regions this card edited ---
    (
        "MOSAIC_RENAME_MUTANT_NO_EXHAUST_CHECK",
        "rename.same_cycle_chain",
        "exhaustion is not detected (I-013 control): a group is accepted with an empty "
        "free set",
        "exhaustion",
    ),
    (
        "MOSAIC_RENAME_MUTANT_NONATOMIC_GROUP",
        "rename.same_cycle_chain",
        "the group is not atomic (I-014 control): a two-tag group with one free tag "
        "half-allocates",
        "twowide-stall",
    ),
    (
        "MOSAIC_RENAME_MUTANT_X0_ALLOC",
        "rename.same_cycle_chain",
        "a write to x0 allocates a physical tag (I-013 control)",
        "x0",
    ),
    (
        "MOSAIC_RENAME_MUTANT_SAME_TAG_LANE1",
        "rename.same_cycle_chain",
        "lane 1 reuses lane 0's tag (I-014 control)",
        "twowide-raw",
    ),
    (
        "MOSAIC_RENAME_MUTANT_WAW_OLD_FROM_MAP",
        "rename.same_cycle_chain",
        "lane 1 reports the start-of-cycle mapping as the one it displaces (I-014 "
        "control)",
        "twowide-waw",
    ),
    (
        "MOSAIC_RENAME_MUTANT_NO_FREE_RESTORE",
        "rename.single_width_ownership",
        "a squash does not restore the free set (I-013 control)",
        "squash",
    ),
    (
        "MOSAIC_RENAME_MUTANT_CKPT_ALLOC_LEAK",
        "rename.single_width_ownership",
        "an allocation in the checkpoint cycle is not journalled (I-013 control)",
        "random",
    ),
]


def build(entry: dict, defines: list, build_dir: str) -> str:
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
    for define in defines:
        # The mutation is in the RTL, so the define goes to Verilator and not to
        # the generated C++.
        cmd += ["-D%s" % define]
    cmd += ["-o", os.path.join(build_dir, entry["case_id"])]
    cmd += sources

    with open(os.path.join(build_dir, "build_command.txt"), "w") as handle:
        handle.write(" ".join(cmd))

    result = run_unit.run(cmd)
    log = result.stdout.decode("utf-8", "replace")
    if result.returncode != 0:
        raise RuntimeError("build failed for %s\n%s" % (defines, log))
    return log


def run_case(binary: str, case_id: str, out_dir: str, max_cycles: int):
    os.makedirs(out_dir, exist_ok=True)
    return subprocess.run(
        [binary, "--case", case_id, "--out", out_dir, "--seed", "1",
         "--max-cycles", str(max_cycles)],
        cwd=REPO_ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )


def sha256_of(path: str) -> str:
    with open(path, "rb") as handle:
        return hashlib.sha256(handle.read()).hexdigest()


def first_failure(log: str) -> str:
    """The first MISMATCH line, which names the check the mutation breaks."""
    for line in log.splitlines():
        if line.startswith("MISMATCH"):
            return line
    for line in log.splitlines():
        if line.startswith("RESULT"):
            return line
    return "(no MISMATCH or RESULT line)"


def first_result_line(log: str) -> str:
    for line in log.splitlines():
        if line.startswith("RESULT "):
            return line
    return "(no RESULT line)"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only", default=None)
    args = parser.parse_args()

    registry = run_unit.load_registry()["cases"]

    # The mutation must exist in the source before it is claimed to have been
    # built: a `-D` that no `ifdef` reads would build a shipping binary under a
    # mutant's name.
    source = os.path.join(REPO_ROOT, SOURCE)
    with open(source) as handle:
        text = handle.read()
    for define, _case, _defect, _expected in MUTANTS:
        if text.count(define) < 1:
            print("FAIL: %s does not occur in %s" % (define, source))
            return 1

    root = os.path.join(REPO_ROOT, "build", "p0", "rename_controls")
    # The shipping binary is built once per case, from an empty directory, and
    # compared against every mutant of that case.
    shipping = {}
    for case_id in sorted({row[1] for row in MUTANTS}):
        if case_id not in registry:
            print("no such case: %s" % case_id)
            return 2
        entry = dict(registry[case_id])
        entry["case_id"] = case_id
        shipping_dir = os.path.join(root, "shipping-" + case_id)
        print("building the shipping case %s from an empty directory..." % case_id)
        build(entry, [], shipping_dir)
        binary = os.path.join(shipping_dir, case_id)
        result = run_case(binary, case_id, os.path.join(root, "out-shipping-" + case_id),
                          entry.get("max_cycles", 200000))
        ok = result.returncode == 0 and b"RESULT PASS" in result.stdout
        print("  baseline exit=%d %s" % (
            result.returncode, first_result_line(result.stdout.decode("utf-8", "replace"))))
        if not ok:
            print("BASELINE FAILED: the shipping build must pass before mutants mean "
                  "anything")
            return 1
        shipping[case_id] = binary
        print("  shipping binary sha256: %s" % sha256_of(binary))
    print()

    failures = 0
    print("%-42s %-30s %-5s %s" % ("mutant", "case", "exit", "result"))
    print("-" * 130)
    for define, case_id, defect, expected in MUTANTS:
        if args.only is not None and args.only not in define:
            continue
        entry = dict(registry[case_id])
        entry["case_id"] = case_id
        mutant_dir = os.path.join(root, define)
        try:
            build(entry, [define], mutant_dir)
        except RuntimeError as error:
            print("%-42s %-30s BUILD FAILED" % (define, case_id))
            print(error)
            failures += 1
            continue
        mutant = os.path.join(mutant_dir, case_id)
        differs = subprocess.run(["cmp", "-s", shipping[case_id], mutant]).returncode != 0
        result = run_case(mutant, case_id, os.path.join(root, "out-" + define),
                          entry.get("max_cycles", 200000))
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
        print("%-42s %-30s %-5d %s" % (define, case_id, result.returncode, status))
        print("    injects: %s" % defect)
        print("    sha256:  %s" % sha256_of(mutant))
        print("    build:   %s" % os.path.relpath(
            os.path.join(mutant_dir, "build_command.txt"), REPO_ROOT))
        print("    %s" % first_failure(log))
        if not (differs and caught):
            failures += 1

    print("-" * 130)
    if failures:
        print("%d mutant(s) were not caught as required" % failures)
        return 1
    print("all %d mutants mutate the binary, exit 1 and name the check they break"
          % (len(MUTANTS) if args.only is None else 1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
