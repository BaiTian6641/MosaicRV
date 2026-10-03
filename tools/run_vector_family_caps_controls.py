#!/usr/bin/env python3
"""Negative control for the vector-ALU family advertisement.

CASE=vec.selfcheck_corpus claims that each program under tests/programs/vec/src/
exercises one ALU family end to end and checks the result.  A passing run alone
cannot separate that claim from "the family is refused and the program does not
run", so this control perturbs the *machine* by clearing the capability bits of
the families advertised by this package:

  * the shipping DUT advertises them (VEC_ALU_CAPS bit per family), and every
    new program must PASS;
  * the mutant DUT clears all twelve newly advertised family bits
    (-DMOSAIC_CORE_MUTANT_VEC_CAPS_NEW_OFF), so the ALU refuses each family and
    each new program must exit 1 with `CHECK FAILED: <program>`.

Each program uses exactly one newly advertised family (plus vsetvli and vector
loads/stores, which are not ALU families), so running it alone against the
all-cleared mutant isolates that family's capability bit: a program that still
passed would mean its family was reachable without the bit.

Both binaries are built from deleted directories and their sha256 must differ,
so the mutant cannot be a stale copy of the shipping build.

Usage:
    python3 tools/run_vector_family_caps_controls.py [--profile p2]
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
MUTANT_DEFINE = "MOSAIC_CORE_MUTANT_VEC_CAPS_NEW_OFF"
MAX_CYCLES = 500000

# (program, family name) for each newly advertised family.  Each program uses
# only its own family among the ALU families.
FAMILY_PROGRAMS = [
    ("v10_wide_e32", "WIDE"),
    ("v11_mul_e32", "MUL"),
    ("v12_mulw_e16", "MULW"),
    ("v13_shift_e32", "SHIFT"),
    ("v15_minmax_e32", "MINMAX"),
    ("v16_cmp_e32", "CMP"),
    ("v17_sat_e32", "SAT"),
    ("v18_slide_e32", "SLIDE"),
    ("v19_gather_e32", "GATHER"),
    ("v20_compress_e8", "COMPRESS"),
    ("v21_reduce_e32", "REDUCE"),
    ("v22_redwide_e16", "REDWIDE"),
]


def sha256_of(path: str) -> str:
    with open(path, "rb") as handle:
        return hashlib.sha256(handle.read()).hexdigest()


def case_entry(registry: dict) -> dict:
    entry = registry.get(CASE_ID)
    if entry is None:
        raise RuntimeError("%s is not registered" % CASE_ID)
    return entry


def build_case(entry: dict, profile: str, build_dir: str, defines: list) -> str:
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
    for define in defines:
        # The mutation is in the RTL, so the define goes to Verilator.
        cmd += ["-D%s" % define]
    cmd += ["-o", binary]
    cmd += sources
    with open(os.path.join(build_dir, "build_command.txt"), "w") as handle:
        handle.write(" ".join(cmd))

    result = run_unit.run(cmd)
    log = result.stdout.decode("utf-8", "replace")
    if result.returncode != 0:
        raise RuntimeError("build failed for %s\n%s" % (defines, log))
    return binary


def build_programs() -> None:
    build_dir = os.path.join(REPO_ROOT, "tests/programs/vec/build")
    shutil.rmtree(build_dir, ignore_errors=True)
    result = subprocess.run(["make", "-C", os.path.join(REPO_ROOT, "tests/programs/vec"),
                             "all"], cwd=REPO_ROOT, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    if result.returncode != 0:
        raise RuntimeError("building tests/programs/vec failed\n%s"
                           % result.stdout.decode("utf-8", "replace"))


def run_case(binary: str, out_dir: str, programs: str | None = None):
    os.makedirs(out_dir, exist_ok=True)
    cmd = [binary, "--case", CASE_ID, "--out", out_dir, "--seed", "1",
           "--max-cycles", str(MAX_CYCLES)]
    if programs is not None:
        cmd += ["--programs", programs]
    return subprocess.run(cmd, cwd=REPO_ROOT, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT)


def first_failure(log: str) -> str:
    for line in log.splitlines():
        if line.startswith("CHECK FAILED"):
            return line
    for line in log.splitlines():
        if line.startswith("RESULT "):
            return line
    return "(no CHECK FAILED or RESULT line)"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", default="p2",
                        help="the profile whose generated config the case builds "
                             "against (default p2)")
    args = parser.parse_args()

    registry = run_unit.load_registry()["cases"]
    entry = case_entry(registry)

    root = os.path.join(REPO_ROOT, "build", args.profile, "vec_family_caps_controls")
    print("building the shipping case from a deleted directory (profile %s)..."
          % args.profile)
    shipping = build_case(entry, args.profile, os.path.join(root, "shipping"), [])
    shipping_hash = sha256_of(shipping)
    print("shipping driver sha256: %s" % shipping_hash)
    print("building the mutant (%s) from a deleted directory..." % MUTANT_DEFINE)
    mutant = build_case(entry, args.profile, os.path.join(root, "mutant"),
                        [MUTANT_DEFINE])
    mutant_hash = sha256_of(mutant)
    print("mutant   driver sha256: %s" % mutant_hash)
    if shipping_hash == mutant_hash:
        print("FAIL: the mutant binary is identical to the shipping binary -- the "
              "define did not reach the build")
        return 1
    print("rebuilding tests/programs/vec/build from a deleted directory...")
    build_programs()

    # ---- baseline: the shipping case must pass every program ----
    result = run_case(shipping, os.path.join(root, "out-shipping"))
    log = result.stdout.decode("utf-8", "replace")
    counts = re.findall(r"(\d+)/(\d+) self-checking RVV programs", log)
    all_passed = bool(counts) and counts[0][0] == counts[0][1] and int(counts[0][1]) > 0
    print("baseline: exit=%d %s" % (result.returncode,
                                   counts[0] if counts else "(no count)"))
    if result.returncode != 0 or "RESULT PASS" not in log or not all_passed:
        print("BASELINE FAILED: the shipping case must pass every program before "
              "the controls mean anything")
        return 1

    # ---- one control per advertised family ----
    print()
    print("%-18s %-9s %-26s %s" % ("program", "family", "shipping", "mutant"))
    failures = 0
    for program, family in FAMILY_PROGRAMS:
        ok = run_case(shipping, os.path.join(root, "out-%s-ship" % program),
                      programs=program)
        ok_log = ok.stdout.decode("utf-8", "replace")
        ship_ok = ok.returncode == 0 and "RESULT PASS" in ok_log \
            and ("1/1 self-checking" in ok_log)
        bad = run_case(mutant, os.path.join(root, "out-%s-mut" % program),
                       programs=program)
        bad_log = bad.stdout.decode("utf-8", "replace")
        named = ("CHECK FAILED: %s" % program) in bad_log
        mut_ok = bad.returncode == 1 and named
        print("%-18s %-9s %-26s %s" % (
            program, family,
            "PASS" if ship_ok else "FAIL(shipping)",
            "exit=%d %s" % (bad.returncode,
                            ("named: %s" % first_failure(bad_log)) if named
                            else "NOT NAMED")))
        if not ship_ok:
            print("  MISS: the program must PASS on the shipping DUT")
        if not mut_ok:
            print("  MISS: the mutant must exit 1 and name CHECK FAILED: %s" % program)
        failures += 0 if (ship_ok and mut_ok) else 1

    print()
    if failures:
        print("FAIL: %d of %d family controls did not isolate their family"
              % (failures, len(FAMILY_PROGRAMS)))
        return 1
    print("OK: all %d advertised families are gated by their capability bit; each "
          "program passes on the shipping DUT and fails, named, on the mutant that "
          "clears the family bits." % len(FAMILY_PROGRAMS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
