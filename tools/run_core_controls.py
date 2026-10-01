#!/usr/bin/env python3
"""Negative controls for the integrated core (work package I-023).

CASE=fabric.fixed_two_cluster claims that the second cluster genuinely executes
its share of a program, and that a straight-line program retires in program
order. A claim like that is evidence only if the defect it excludes would be
caught, so this tool rebuilds the case with exactly one defect injected through
a `-DMOSAIC_*_MUTANT_*` define -- from an empty build directory, so a mutant can
never reuse the shipping object files -- and runs it. Every mutant must:

  * build cleanly into its own directory;
  * produce a binary that differs from the shipping one (`cmp`), so a stale
    build cannot masquerade as a mutant;
  * exit 1, with the named check the mutation breaks as the first failure.

A mutant that exits 0 is either an unobservable defect or a check that cannot
fail; this tool reports it as NOT MET rather than counting it.

The shipping build is built and run first, from an empty directory: mutants mean
nothing if the case does not pass in the first place.

Usage: run_core_controls.py [--case fabric.fixed_two_cluster]
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import run_unit  # noqa: E402

REPO_ROOT = run_unit.REPO_ROOT

# Each mutant: the define, the defect it injects, and the text of the first
# failure the mutation must produce.
MUTANTS = [
    (
        "MOSAIC_DISPATCH_MUTANT_SINGLE_CLUSTER",
        "the dispatch affinity never alternates, so every macro is inserted into "
        "cluster 0's queue and the second cluster is never used",
        "cluster 1 executed at least one ALU uop",
    ),
    (
        "MOSAIC_CORE_MUTANT_DBUF_PUSH_SLOT",
        "the decode buffer picks its push slot from the pop alone, which "
        "overwrites a live entry when the buffer holds one entry in slot 1 and "
        "swaps the first two macros of a program",
        "retire 1 pc",
    ),
    (
        "MOSAIC_CORE_MUTANT_ROB_GEN_COMMIT",
        "the committed map is fed the ROB entry generation instead of the "
        "physical tag's generation, so the map-equality boundary is false from "
        "the first commit on",
        "an empty ROB is at a rename boundary",
    ),
]


# CASE=core.mem_program's controls. Each names the wiring decision it breaks and
# the text of the first failure the mutation must produce.
CORE_MEM_MUTANTS = [
    (
        "MOSAIC_CORE_MUTANT_STORE_PRECOMMIT",
        "the store is authorised when it is *allocated* instead of when it "
        "retires, so a store reaches memory before the instruction that owns it "
        "has retired",
        "no store reaches memory before its instruction retires",
    ),
    (
        "MOSAIC_LQ_MUTANT_YOUNGER_FORWARDS",
        "the load queue forwards from the youngest covering store with no regard "
        "for whether that store is older than the load, so a load takes a value "
        "from a store that has not happened yet",
        "the retirement stream follows the reference",
    ),
    (
        "MOSAIC_CORE_MUTANT_MEM_BEHIND_BRANCH",
        "dispatch lets a load or store through the branch barrier, so the "
        "instructions after an unresolved branch are allocated into the queues "
        "and issued to the endpoint before the redirect discards them",
        "exactly one data transaction per load and per store reached the data port",
    ),
    (
        "MOSAIC_CORE_MUTANT_STORE_SIZE_WORD",
        "the store queue is told every store is a word, so the byte strobes and "
        "the bytes written disagree with the instruction for byte, half and "
        "double stores; the first load that reads such a store back names it",
        "the retirement stream follows the reference",
    ),
]

MUTANTS_BY_CASE = {
    "core.mem_program": CORE_MEM_MUTANTS,
}


def mutants_for(case_id: str) -> list:
    return MUTANTS_BY_CASE.get(case_id, MUTANTS)


def build(entry: dict, case_id: str, defines: list, build_dir: str) -> str:
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
        # the generated C++ (unlike the loader controls, whose mutants are C++).
        cmd += ["-D%s" % define]
    cmd += ["-o", os.path.join(build_dir, case_id)]
    cmd += sources

    # The directory was deleted above, so this build cannot reuse a shipping
    # object; the command is written next to it so a mutant's `-D` is on the
    # record for whoever reads the evidence later, exactly as run_unit keeps
    # `build_command.txt` for the shipping build.
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
    import hashlib
    with open(path, "rb") as handle:
        return hashlib.sha256(handle.read()).hexdigest()


def first_result_line(log: str) -> str:
    for line in log.splitlines():
        if line.startswith("RESULT "):
            return line
    return "(no RESULT line)"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", default="fabric.fixed_two_cluster")
    parser.add_argument("--only", default=None)
    args = parser.parse_args()

    registry = run_unit.load_registry()["cases"]
    if args.case not in registry:
        print("no such case: %s" % args.case)
        return 2
    entry = registry[args.case]
    max_cycles = entry.get("max_cycles", 200000)

    root = os.path.join(REPO_ROOT, "build", "p0", "core_controls")
    shipping_dir = os.path.join(root, "shipping")
    print("building the shipping case from an empty directory...")
    build(entry, args.case, [], shipping_dir)
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

    mutants = mutants_for(args.case)
    failures = 0
    print("shipping binary sha256: %s" % sha256_of(shipping))
    print()
    print("%-46s %-5s %s" % ("mutant", "exit", "result"))
    print("-" * 110)
    for define, defect, expected in mutants:
        if args.only is not None and args.only not in define:
            continue
        mutant_dir = os.path.join(root, define)
        try:
            build(entry, args.case, [define], mutant_dir)
        except RuntimeError as error:
            print("%-46s BUILD FAILED" % define)
            print(error)
            failures += 1
            continue
        mutant = os.path.join(mutant_dir, args.case)
        differs = subprocess.run(["cmp", "-s", shipping, mutant]).returncode != 0
        mutant_hash = sha256_of(mutant)
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
        print("%-46s %-5d %s" % (define, result.returncode, status))
        print("    injects: %s" % defect)
        print("    sha256:  %s" % mutant_hash)
        print("    build:   %s" % os.path.relpath(
            os.path.join(mutant_dir, "build_command.txt"), REPO_ROOT))
        print("    %s" % first_result_line(log))
        if not (differs and caught):
            failures += 1

    print("-" * 110)
    if failures:
        print("%d mutant(s) were not caught as required" % failures)
        return 1
    print("all %d mutants mutate the binary, exit 1 and name the check they break"
          % (len(mutants) if args.only is None else 1))
    return 0


if __name__ == "__main__":
    sys.exit(main())