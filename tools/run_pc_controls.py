#!/usr/bin/env python3
"""Negative controls for the PC-transfer and fetch-visibility case (V-016).

CASE=pc.branch_and_fetch_visibility says what the program counter and the fetch
view must do. That claim is only evidence if a machine that got it wrong would be
caught, so this tool rebuilds the case with exactly one defect injected -- from a
deleted build directory -- and runs it. Every mutant must:

  * build cleanly into its own directory, wiped first (a mutant must never reuse
    the shipping object files);
  * produce a binary that differs from the shipping one (`cmp`), so a stale build
    cannot masquerade as a mutant;
  * exit 1, with the named check the mutation breaks somewhere in its output.

A mutant that exits 0 is either an unobservable defect or a check that cannot
fail; this tool reports it as MISS rather than counting it.

Two defects are in the RTL, so their define goes to Verilator (`-D`):

  * MOSAIC_CORE_MUTANT_FENCEI_NO_INVALIDATE  FENCE.I does not invalidate the
    delivered fetch view (I-037's mutant, reused -- this case does not invent a
    second one);
  * MOSAIC_CORE_MUTANT_NO_PURGE            a redirect does not purge the
    undecodable macro a wrong-path fetch left in the decode buffer (I-023's
    mutant, reused).

Two cannot be RTL. In p0 there is no cache, the core asks for four bytes at the
instruction's own PC, and *this harness's memory model* is what assembles them,
so their define goes to the generated C++ (`-CFLAGS -D`), exactly as I-041's
wrong-line control does:

  * MOSAIC_IMEM_MUTANT_MASK_PC    the memory serves the fetch of a PC that is not
    on a line front from the line front -- the PC masked at a boundary -- so the
    machine executes a word that is not the one at its own PC;
  * MOSAIC_IMEM_MUTANT_JALR_ODD   the memory serves the fetch of a JALR's target
    from the uncleared (odd) address, the architectural consequence of a JALR
    that does not clear bit 0. This has to be a harness defect: the one RTL
    mutant for that defect, MOSAIC_BRANCH_TARGET_MUTANT_1, is *unobservable* in
    the integrated core. mosaic_cluster recomputes the JALR target itself
    (`br_target_eff = is_jalr ? {br_sum[63:1], 1'b0} : br_target`) and never reads
    the branch-target unit's `target` output, so the mutant binary differs from
    shipping while behaving identically (verified: identical RESULT line). The
    report records that as a finding.

Usage: run_pc_controls.py [--case pc.branch_and_fetch_visibility] [--only NAME]
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

# Each mutant: the define, where it is injected ("rtl" or "cpp"), the defect it
# injects, and the check that must catch it.
MUTANTS = [
    (
        "MOSAIC_IMEM_MUTANT_MASK_PC",
        "cpp",
        "the harness's instruction memory answers a PC that is not on a line "
        "front with the line front -- the PC masked at a boundary -- so the "
        "machine executes a word that is not the one at its own PC",
        "the event record carries the instruction's own bits",
    ),
    (
        "MOSAIC_IMEM_MUTANT_JALR_ODD",
        "cpp",
        "the harness's instruction memory answers the fetch of a JALR's target "
        "with the bytes the uncleared (odd) address names -- the architectural "
        "consequence of a JALR that does not clear bit 0. It has to be a "
        "harness defect: the one RTL mutant for that defect "
        "(MOSAIC_BRANCH_TARGET_MUTANT_1) is unobservable in the integrated core, "
        "because mosaic_cluster recomputes the JALR target itself",
        "the event record carries the instruction's own bits",
    ),
    (
        "MOSAIC_CORE_MUTANT_FENCEI_NO_INVALIDATE",
        "rtl",
        "FENCE.I completes like a plain fence and never redirects, so the front "
        "end keeps the instruction view it delivered before the publishing store "
        "and the stale bytes execute",
        "after FENCE.I the new bytes are the ones executed",
    ),
    (
        "MOSAIC_CORE_MUTANT_NO_PURGE",
        "rtl",
        "a redirect does not purge the younger work it discards, so the "
        "undecodable macro a wrong-path fetch left in the decode buffer is "
        "dispatched and the machine stops",
        "the machine never stops on a wrong-path fetch fault",
    ),
]


def build(entry: dict, case_id: str, define: str, where: str, build_dir: str) -> str:
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
    if define:
        if where == "rtl":
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
        raise RuntimeError("build failed for %s\n%s" % (define or "(shipping)", log))
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


def first_result_line(log: str) -> str:
    for line in log.splitlines():
        if line.startswith("RESULT "):
            return line
    return "(no RESULT line)"


def first_failure_line(log: str) -> str:
    for line in log.splitlines():
        if line.startswith("CHECK FAILED") or line.startswith("MISMATCH"):
            return line
    return "(no failing check line)"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", default="pc.branch_and_fetch_visibility")
    parser.add_argument("--only", default=None)
    args = parser.parse_args()

    registry = run_unit.load_registry()["cases"]
    if args.case not in registry:
        print("no such case: %s" % args.case)
        return 2
    entry = registry[args.case]
    max_cycles = entry.get("max_cycles", 200000)

    root = os.path.join(REPO_ROOT, "build", "p0", "pc_controls")
    shipping_dir = os.path.join(root, "shipping")
    print("building the shipping case from an empty directory...")
    build(entry, args.case, "", "rtl", shipping_dir)
    shipping = os.path.join(shipping_dir, args.case)
    result = run_case(shipping, args.case, os.path.join(root, "out-shipping"),
                      max_cycles)
    baseline_ok = result.returncode == 0 and b"RESULT PASS" in result.stdout
    print("  baseline exit=%d %s" % (
        result.returncode,
        first_result_line(result.stdout.decode("utf-8", "replace"))))
    if not baseline_ok:
        print("BASELINE FAILED: the shipping build must pass before mutants mean "
              "anything")
        return 1

    failures = 0
    selected = [m for m in MUTANTS if args.only is None or args.only in m[0]]
    print("shipping binary sha256: %s" % sha256_of(shipping))
    print()
    print("%-42s %-5s %s" % ("mutant", "exit", "result"))
    print("-" * 112)
    for define, where, defect, expected in selected:
        mutant_dir = os.path.join(root, define)
        try:
            build(entry, args.case, define, where, mutant_dir)
        except RuntimeError as error:
            print("%-42s BUILD FAILED" % define)
            print(error)
            failures += 1
            continue
        mutant = os.path.join(mutant_dir, args.case)
        differs = subprocess.run(["cmp", "-s", shipping, mutant]).returncode != 0
        result = run_case(mutant, args.case, os.path.join(root, "out-" + define),
                          max_cycles)
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
        print("%-42s %-5d %s" % (define, result.returncode, status))
        print("    injects: %s (%s)" % (defect, "RTL" if where == "rtl" else
                                        "the harness's memory model"))
        print("    sha256:  %s" % sha256_of(mutant))
        print("    build:   %s" % os.path.relpath(
            os.path.join(mutant_dir, "build_command.txt"), REPO_ROOT))
        print("    %s" % first_failure_line(log))
        print("    %s" % first_result_line(log))
        if not (differs and caught):
            failures += 1

    print("-" * 112)
    if failures:
        print("%d mutant(s) were not caught as required" % failures)
        return 1
    print("all %d mutants mutate the binary, exit 1 and name the check they break"
          % len(selected))
    return 0


if __name__ == "__main__":
    sys.exit(main())
