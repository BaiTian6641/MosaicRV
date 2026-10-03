#!/usr/bin/env python3
"""Negative control for CASE=vec.selfcheck_corpus.

The case claims that the self-checking RVV programs under tests/programs/vec/
compare the DUT's result against constants derived by hand from the RVV
specification and *fail loudly* when the DUT disagrees.  A passing run alone
cannot separate that claim from "the programs merely do not trap", so this
control perturbs the oracle instead of the machine:

  * v07_control_wrong_addsub.S is v01_addsub_e64.S with one expected constant
    deliberately wrong (op1 element 1 written as ...FD instead of ...FE).  The
    DUT still computes the correct ...FE, so the program's own comparison must
    fail at the first differing byte -- byte 24 of its result block.

The control builds the case driver and the programs from deleted directories
(so a stale binary cannot be mistaken for a fresh one), runs the shipping case
over the six registered programs (which must PASS 6/6), then runs the same
driver over the wrong-constant program alone and requires it to exit 1, print
`CHECK FAILED`, and name the first failure as check 1 at byte offset 24.

Usage:
    python3 tools/run_vector_selfcheck_controls.py [--profile p2]
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import run_unit  # noqa: E402

REPO_ROOT = run_unit.REPO_ROOT
CASE_ID = "vec.selfcheck_corpus"
CONTROL_PROGRAM = "v07_control_wrong_addsub"
CONTROL_SOURCE = "tests/programs/vec/src/v07_control_wrong_addsub.S"
# The wrong constant the control file must actually contain, and the failure it
# must produce.
WRONG_CONSTANT = "0xFFFFFFFFFFFFFFFD"
EXPECTED_FAILURE = "check 1 failed at byte offset 24"
MAX_CYCLES = 500000


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


def case_entry(registry: dict) -> dict:
    """The case's build recipe: the registry entry when it exists, else the
    same sources derived from vec.mask_prefix_at_core (the vector engine's
    already-registered core-level case)."""
    entry = registry.get(CASE_ID)
    if entry is not None:
        print("case entry: from tests/unit/registry.json")
        return entry
    base = registry["vec.mask_prefix_at_core"]
    print("case entry: derived from vec.mask_prefix_at_core "
          "(vec.selfcheck_corpus is not in the registry yet)")
    return {
        "task": "V-060",
        "top": base["top"],
        "rtl": base["rtl"],
        "sv": base["sv"],
        "cpp": [
            "sim/unit/tb_core_vecselfcheck.cpp",
            "sim/common/elf_loader.cpp",
            "sim/common/memory_model.cpp",
            "sim/common/event_tap.cpp",
        ],
        "max_cycles": MAX_CYCLES,
        "seed": 1,
        "profiles": ["p2"],
    }


def build_case(entry: dict, profile: str, build_dir: str) -> str:
    """Build the case from a deleted directory; returns the binary path."""
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
    binary = os.path.join(build_dir, CASE_ID)
    cmd = ["verilator"] + run_unit.VERILATOR_FLAGS
    cmd += ["--top-module", entry["top"], "-Mdir", os.path.join(build_dir, "obj_dir")]
    cmd += ["-I%s" % os.path.join(REPO_ROOT, "build", profile, "sim")]
    cmd += ["-I%s" % os.path.join(REPO_ROOT, "rtl", "core")]
    cmd += ["-I%s" % os.path.join(REPO_ROOT, "rtl", "common")]
    cmd += ["-I%s" % os.path.join(REPO_ROOT, "build", profile, "rtl")]
    cmd += ["-CFLAGS", "-I%s" % run_unit.SIM_COMMON]
    cmd += ["-CFLAGS", "-I%s" % os.path.join(REPO_ROOT, "build", profile, "sim")]
    cmd += ["-o", binary]
    cmd += sources
    with open(os.path.join(build_dir, "build_command.txt"), "w") as handle:
        handle.write(" ".join(cmd))

    result = run_unit.run(cmd)
    log = result.stdout.decode("utf-8", "replace")
    if result.returncode != 0:
        raise RuntimeError("build failed\n%s" % log)
    return binary


def build_programs() -> None:
    """Rebuild tests/programs/vec/build from scratch."""
    build_dir = os.path.join(REPO_ROOT, "tests/programs/vec/build")
    shutil.rmtree(build_dir, ignore_errors=True)
    result = subprocess.run(["make", "-C", os.path.join(REPO_ROOT, "tests/programs/vec"),
                             "all"], cwd=REPO_ROOT, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    log = result.stdout.decode("utf-8", "replace")
    if result.returncode != 0:
        raise RuntimeError("building tests/programs/vec failed\n%s" % log)


def run_case(binary: str, out_dir: str, programs: str | None = None):
    os.makedirs(out_dir, exist_ok=True)
    cmd = [binary, "--case", CASE_ID, "--out", out_dir, "--seed", "1",
           "--max-cycles", str(MAX_CYCLES)]
    if programs is not None:
        cmd += ["--programs", programs]
    return subprocess.run(cmd, cwd=REPO_ROOT, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", default="p2",
                        help="the profile whose generated config the case builds "
                             "against (default p2: the lowest profile whose "
                             "geometry declares the vector engine)")
    args = parser.parse_args()

    registry = run_unit.load_registry()["cases"]
    entry = case_entry(registry)

    # The mutation must exist in the source before it is claimed to have been
    # built: a control source without the wrong constant would run the shipping
    # program under the control's name.
    source = os.path.join(REPO_ROOT, CONTROL_SOURCE)
    if not os.path.exists(source):
        print("FAIL: %s does not exist" % source)
        return 1
    with open(source) as handle:
        text = handle.read()
    if WRONG_CONSTANT not in text:
        print("FAIL: %s does not contain the wrong constant %s"
              % (source, WRONG_CONSTANT))
        return 1

    root = os.path.join(REPO_ROOT, "build", args.profile, "vec_selfcheck_controls")
    shipping_dir = os.path.join(root, "shipping")
    print("building the case from a deleted directory (profile %s)..." % args.profile)
    binary = build_case(entry, args.profile, shipping_dir)
    print("shipping driver sha256: %s" % sha256_of(binary))
    print("rebuilding tests/programs/vec/build from a deleted directory...")
    build_programs()

    # ---- baseline: the registered six must pass ----
    result = run_case(binary, os.path.join(root, "out-shipping"))
    log = result.stdout.decode("utf-8", "replace")
    print("  baseline exit=%d %s" % (result.returncode, first_result_line(log)))
    # Require "N/N" with N > 0: every listed program reached its pass trap.
    counts = re.findall(r"(\d+)/(\d+) self-checking RVV programs", log)
    all_passed = bool(counts) and counts[0][0] == counts[0][1] and int(counts[0][1]) > 0
    baseline_ok = result.returncode == 0 and "RESULT PASS" in log and all_passed
    if not baseline_ok:
        print("BASELINE FAILED: the shipping case must pass every program before "
              "the control means anything")
        return 1

    # ---- control: the wrong-constant program must fail, and name the failure ----
    control_elf = os.path.join(REPO_ROOT, "tests/programs/vec/build",
                               CONTROL_PROGRAM + ".elf")
    if not os.path.exists(control_elf):
        print("FAIL: %s was not built" % control_elf)
        return 1
    print("control program sha256:  %s" % sha256_of(control_elf))

    result = run_case(binary, os.path.join(root, "out-control"),
                      programs=CONTROL_PROGRAM)
    log = result.stdout.decode("utf-8", "replace")
    caught = (result.returncode == 1 and "CHECK FAILED" in log
              and EXPECTED_FAILURE in log)
    print()
    print("control: %s" % CONTROL_PROGRAM)
    print("  exit=%d %s" % (result.returncode, first_result_line(log)))
    print("  first failure: %s" % first_failure(log))
    if result.returncode != 1:
        print("  MISS: the control must exit 1 (got %d)" % result.returncode)
    if "CHECK FAILED" not in log:
        print("  MISS: no CHECK FAILED line")
    if EXPECTED_FAILURE not in log:
        print("  MISS: expected %r" % EXPECTED_FAILURE)
    if not caught:
        return 1
    print()
    print("OK: the wrong-constant control fails the case and names the first "
          "failure (%s)" % EXPECTED_FAILURE)
    return 0


if __name__ == "__main__":
    sys.exit(main())
