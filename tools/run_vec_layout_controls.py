#!/usr/bin/env python3
"""Negative controls for CASE=rvv.vtype_layout (work package I-051).

The case claims that `vtype` carries its fields at the ratified v1.0 positions
(`vlmul[2:0]`, `vsew[5:3]`, `vta[6]`, `vma[7]`, `vill[63]`) and that both the
configuration unit and the descriptor decode them there. A claim like that is
evidence only if the defect it excludes would be caught, so this tool rebuilds
the case with exactly one defect injected through a `-D` define -- from an empty
build directory, so a mutant can never reuse the shipping object files -- and
runs it. Every mutant must:

  * build cleanly into its own directory, with its `-D` on the recorded command
    line (`build_command.txt` next to the binary);
  * produce a binary that differs from the shipping one (`cmp`), so a stale
    build cannot masquerade as a mutant;
  * exit 1, and its log must contain the named check the mutation breaks.

The shipping build is built and run first, from an empty directory: mutants mean
nothing if the case does not pass in the first place.

The three mutants are the fail modes the layout card names: the field put back
at the pre-ratification 7:5 position (so the round-trip fails); SEW decoded from
the wrong bits (so a legal configuration is misread); and a software CSR write
to `vtype` accepted (so software can write `vill`, breaking the URO rule).

Usage: run_vec_layout_controls.py [--only SUBSTRING] [--profile p2]
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
CASE_ID = "rvv.vtype_layout"

# Each mutant: the define, the source files that must read it, the defect it
# injects, and the text the failing check must contain.
MUTANTS = [
    (
        "MOSAIC_VEC_MUTANT_VTYPE_LEGACY_75",
        ["rtl/core/mosaic_vec_desc.sv", "rtl/core/mosaic_vec_cfg.sv"],
        "the pre-ratification layout -- vsew back at bits 7:5 with vta/vma at "
        "4/3 -- in both the descriptor and the configuration unit, so a "
        "specification-encoded vtype no longer round-trips",
        "vtype-roundtrip",
    ),
    (
        "MOSAIC_VEC_MUTANT_VTYPE_SEW_WRONG",
        ["rtl/core/mosaic_vec_desc.sv"],
        "the descriptor decodes SEW from bits 4:2 instead of 5:3, so a legal "
        "configuration is misread as a different SEW",
        "decoded from the wrong bits",
    ),
    (
        "MOSAIC_VEC_MUTANT_VTYPE_SW_WRITE",
        ["rtl/core/mosaic_vec_cfg.sv"],
        "a software CSR write to vtype is accepted, so software can write vill "
        "instead of the URO rule's illegal-instruction",
        "software set vill",
    ),
]


def build(entry: dict, profile: str, defines: list, build_dir: str) -> str:
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
    cmd += ["-o", os.path.join(build_dir, CASE_ID)]
    cmd += sources

    with open(os.path.join(build_dir, "build_command.txt"), "w") as handle:
        handle.write(" ".join(cmd))

    result = run_unit.run(cmd)
    log = result.stdout.decode("utf-8", "replace")
    if result.returncode != 0:
        raise RuntimeError("build failed for %s\n%s" % (defines, log))
    return log


def run_case(binary: str, out_dir: str, max_cycles: int):
    os.makedirs(out_dir, exist_ok=True)
    return subprocess.run(
        [binary, "--case", CASE_ID, "--out", out_dir, "--seed", "1",
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
    parser.add_argument("--profile", default="p2")
    args = parser.parse_args()

    registry = run_unit.load_registry()["cases"]
    if CASE_ID not in registry:
        print("no such case: %s" % CASE_ID)
        return 2
    entry = registry[CASE_ID]
    max_cycles = entry.get("max_cycles", 200000)

    # The mutation must exist in the source before it is claimed to have been
    # built: a `-D` that no `ifdef` reads would build a shipping binary under a
    # mutant's name.
    for define, source_rels, _defect, _expected in MUTANTS:
        for source_rel in source_rels:
            source = os.path.join(REPO_ROOT, source_rel)
            if source_rel not in entry.get("rtl", []):
                print("FAIL: %s is not in the case's RTL list" % source_rel)
                return 1
            with open(source) as handle:
                if define not in handle.read():
                    print("FAIL: %s does not occur in %s" % (define, source))
                    return 1

    root = os.path.join(REPO_ROOT, "build", args.profile, "vec_layout_controls")
    shipping_dir = os.path.join(root, "shipping")
    print("building the shipping case from an empty directory (profile %s)..." % args.profile)
    build(entry, args.profile, [], shipping_dir)
    shipping = os.path.join(shipping_dir, CASE_ID)
    result = run_case(shipping, os.path.join(root, "out-shipping"), max_cycles)
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
    print("%-42s %-5s %s" % ("mutant", "exit", "result"))
    print("-" * 118)
    for define, _source_rels, defect, expected in MUTANTS:
        if args.only is not None and args.only not in define:
            continue
        mutant_dir = os.path.join(root, define)
        try:
            build(entry, args.profile, [define], mutant_dir)
        except RuntimeError as error:
            print("%-42s BUILD FAILED" % define)
            print(error)
            failures += 1
            continue
        mutant = os.path.join(mutant_dir, CASE_ID)
        differs = subprocess.run(["cmp", "-s", shipping, mutant]).returncode != 0
        mutant_hash = sha256_of(mutant)
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
        print("%-42s %-5d %s" % (define, result.returncode, status))
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
          % (len(MUTANTS) if args.only is None else 1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
