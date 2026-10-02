#!/usr/bin/env python3
"""Negative controls for the mask-prefix cases.

Two cases share this tool: ``CASE=rvv.mask_prefix_vstart`` (work package I-057,
the illegal-instruction rule at a non-zero ``vstart``) and the semantics proper,
``CASE=rvv.mask_prefix_semantics`` (the three rules at the first set bit, plus
the all-zero and position-0 boundaries).  ``--case`` selects which; the default
is the vstart case.

The vstart case claims that the mask-prefix operations (`vmsbf.m`, `vmsif.m`,
`vmsof.m`) raise an illegal-instruction exception when `vstart` is non-zero --
they cannot be restarted part-way -- while `vstart == 0` still executes
normally, and that the refused instruction neither reads nor writes the register
file nor changes the destination. A claim like that is evidence only if the
defect it excludes would be caught, so this tool rebuilds the case with exactly
one defect injected through a `-D` define -- from an empty build directory, so a
mutant can never reuse the shipping object files -- and runs it. Every mutant
must:

  * build cleanly into its own directory, with its `-D` on the recorded command
    line (`build_command.txt` next to the binary);
  * produce a binary that differs from the shipping one (`cmp`), so a stale
    build cannot masquerade as a mutant;
  * exit 1, and its log must contain the named check the mutation breaks.

The shipping build is built and run first, from an empty directory: mutants mean
nothing if the case does not pass in the first place.

Usage: run_vec_maskpfx_controls.py [--only SUBSTRING] [--profile p0]
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
DEFAULT_CASE = "rvv.mask_prefix_vstart"

# Each mutant: the define, the source file that must read it, the defect it
# injects, and the text the failing check must contain.  Two cases share this
# tool: the `vstart` rule (I-057) and the semantics proper (the registered
# CASE=rvv.mask_prefix_semantics).
MUTANTS = [
    (
        "MOSAIC_VEC_ALU_MUTANT_MASKPFX_NO_TRAP",
        "rtl/core/mosaic_vec_alu.sv",
        "the rule is not implemented at all: vmsbf/vmsif/vmsof execute from a "
        "non-zero vstart instead of raising an illegal-instruction exception",
        "vstart=1: a non-zero vstart did not raise an illegal instruction",
    ),
    (
        "MOSAIC_VEC_ALU_MUTANT_MASKPFX_TRAP_VSTART0",
        "rtl/core/mosaic_vec_alu.sv",
        "the rule fires at vstart == 0 too, so a mask-prefix instruction that "
        "must execute normally is wrongly refused",
        "vstart=0: the instruction was refused as illegal",
    ),
    (
        "MOSAIC_VEC_ALU_MUTANT_MASKPFX_WRONG_OP",
        "rtl/core/mosaic_vec_alu.sv",
        "vmsof is exempted, so only two of the three mask-prefix operations "
        "raise on a non-zero vstart",
        "vmsof vstart=1: a non-zero vstart did not raise an illegal instruction",
    ),
]

# CASE=rvv.mask_prefix_semantics: the defect the previous package missed
# (`vmsbf` sharing `vmsif`'s expression) and the two boundaries where a wrong
# all-zero or position-0 rule hides.
MUTANTS_SEMANTICS = [
    (
        "MOSAIC_VEC_ALU_MUTANT_MASKPFX_SAME_EXPR",
        "rtl/core/mosaic_vec_alu.sv",
        "vmsbf and vmsif share the set-including-first expression again, so "
        "vmsbf is wrong at the first set bit of every source that has one",
        "spec-anchor vmsbf src=148 k-bit2: 1 expected 0",
    ),
    (
        "MOSAIC_VEC_ALU_MUTANT_MASKPFX_ALLZERO",
        "rtl/core/mosaic_vec_alu.sv",
        "an all-zero active source is cleared for vmsbf/vmsif instead of the "
        "specification's all-ones",
        "all-zero vmsbf bit15: 0 expected 1",
    ),
    (
        "MOSAIC_VEC_ALU_MUTANT_MASKPFX_POS0",
        "rtl/core/mosaic_vec_alu.sv",
        "vmsbf reports a bit set before the first element when element 0 is "
        "itself the first set bit (k = 0)",
        "spec-anchor vmsbf src=149 k-bit0: 1 expected 0",
    ),
]

CASES = {
    "rvv.mask_prefix_vstart": MUTANTS,
    "rvv.mask_prefix_semantics": MUTANTS_SEMANTICS,
}


def build(entry: dict, profile: str, defines: list, build_dir: str, case_id: str) -> str:
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
    cmd += ["-I%s" % os.path.join(REPO_ROOT, "build", profile, "sim")]
    cmd += ["-I%s" % os.path.join(REPO_ROOT, "rtl", "core")]
    cmd += ["-I%s" % os.path.join(REPO_ROOT, "rtl", "common")]
    cmd += ["-I%s" % os.path.join(REPO_ROOT, "build", profile, "rtl")]
    cmd += ["-CFLAGS", "-I%s" % run_unit.SIM_COMMON]
    cmd += ["-CFLAGS", "-I%s" % os.path.join(REPO_ROOT, "build", profile, "sim")]
    for define in defines:
        # The mutation is in the RTL, so the define goes to Verilator, not to the
        # generated C++.
        cmd += ["-D%s" % define]
    cmd += ["-o", os.path.join(build_dir, case_id)]
    cmd += sources

    with open(os.path.join(build_dir, "build_command.txt"), "w") as handle:
        handle.write(" ".join(cmd))

    result = run_unit.run(cmd)
    log = result.stdout.decode("utf-8", "replace")
    if result.returncode != 0:
        raise RuntimeError("build failed for %s\n%s" % (defines, log))
    return log


def run_case(binary: str, out_dir: str, max_cycles: int, case_id: str):
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
    for line in log.splitlines():
        if line.startswith("CHECK FAILED"):
            return line
    for line in log.splitlines():
        if line.startswith("RESULT"):
            return line
    return "(no CHECK FAILED or RESULT line)"


def first_result_line(log: str) -> str:
    for line in log.splitlines():
        if line.startswith("RESULT "):
            return line
    return "(no RESULT line)"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only", default=None)
    parser.add_argument("--profile", default="p0")
    parser.add_argument("--case", default=DEFAULT_CASE, choices=sorted(CASES))
    args = parser.parse_args()

    case_id = args.case
    mutants = CASES[case_id]
    registry = run_unit.load_registry()["cases"]
    if case_id not in registry:
        print("no such case: %s" % case_id)
        return 2
    entry = registry[case_id]
    max_cycles = entry.get("max_cycles", 200000)

    # The mutation must exist in the source before it is claimed to have been
    # built: a `-D` that no `ifdef` reads would build a shipping binary under a
    # mutant's name.
    for define, source_rel, _defect, _expected in mutants:
        source = os.path.join(REPO_ROOT, source_rel)
        if source_rel not in entry.get("rtl", []):
            print("FAIL: %s is not in the case's RTL list" % source_rel)
            return 1
        with open(source) as handle:
            if define not in handle.read():
                print("FAIL: %s does not occur in %s" % (define, source))
                return 1

    root = os.path.join(REPO_ROOT, "build", args.profile, "vec_maskpfx_controls")
    shipping_dir = os.path.join(root, "shipping")
    print("building the shipping case from an empty directory (profile %s)..." % args.profile)
    build(entry, args.profile, [], shipping_dir, case_id)
    shipping = os.path.join(shipping_dir, case_id)
    result = run_case(shipping, os.path.join(root, "out-shipping"), max_cycles, case_id)
    baseline_ok = result.returncode == 0 and b"RESULT PASS" in result.stdout
    print("  baseline exit=%d %s" % (
        result.returncode, first_result_line(result.stdout.decode("utf-8", "replace"))))
    if not baseline_ok:
        print("BASELINE FAILED: the shipping build must pass before mutants mean "
              "anything")
        return 1

    failures = 0
    print("shipping binary sha256: %s" % sha256_of(shipping))
    print()
    print("%-46s %-5s %s" % ("mutant", "exit", "result"))
    print("-" * 118)
    for define, _source_rel, defect, expected in mutants:
        if args.only is not None and args.only not in define:
            continue
        mutant_dir = os.path.join(root, define)
        try:
            build(entry, args.profile, [define], mutant_dir, case_id)
        except RuntimeError as error:
            print("%-46s BUILD FAILED" % define)
            print(error)
            failures += 1
            continue
        mutant = os.path.join(mutant_dir, case_id)
        differs = subprocess.run(["cmp", "-s", shipping, mutant]).returncode != 0
        mutant_hash = sha256_of(mutant)
        result = run_case(mutant, os.path.join(root, "out-" + define), max_cycles, case_id)
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
        print("%-46s %-5d %s" % (define, result.returncode, status))
        print("    injects: %s" % defect)
        print("    sha256:  %s" % mutant_hash)
        print("    build:   %s" % os.path.relpath(
            os.path.join(mutant_dir, "build_command.txt"), REPO_ROOT))
        print("    %s" % first_failure(log))
        if not (differs and caught):
            failures += 1

    print("-" * 118)
    if failures:
        print("%d mutant(s) were not caught as required" % failures)
        return 1
    print("all %d mutants mutate the binary, exit 1 and name the check they break"
          % (len(mutants) if args.only is None else 1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
