#!/usr/bin/env python3
"""Per-step measurement for CASE=fabric.integrated (work package I-090).

The report `results/reports/I-090-fabric.md` states what each integration step
did to the measured numbers. That table is measured here: each configuration is
built from an empty directory with one or more `-DMOSAIC_FAB_STEP_NO_*` defines,
which disable one step of the *dynamic* configuration while leaving the others
on, and the case is run once per configuration.

These are instrumentation, not controls: a partial fabric is not a defect, but it
is also not the full fabric, so the case's "the dynamic configuration changed
something measurable" and "the router granted" checks fail on the partial
configurations by construction. Only the numbers are read from them; the
configuration with no step disabled is the registered case and passes.

  +-----------------------+---------------------------------------------------+
  | step disabled         | the dynamic configuration that results            |
  +-----------------------+---------------------------------------------------+
  | no defines            | steering + bank preference + bypass (registered)  |
  | NO_BYPASS             | steering + bank preference                        |
  | NO_BANK, NO_BYPASS    | steering only                                     |
  | NO_STEERING, NO_BANK, | nothing: the "dynamic" run is the fixed machine    |
  | NO_BYPASS             |                                                   |

Usage: run_fabric_steps.py [--out DIR]
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import run_unit  # noqa: E402

REPO_ROOT = run_unit.REPO_ROOT
CASE_ID = "fabric.integrated"

CONFIGS = [
    ("no step (= fixed baseline)",
     ["MOSAIC_FAB_STEP_NO_STEERING", "MOSAIC_FAB_STEP_NO_BANK",
      "MOSAIC_FAB_STEP_NO_BYPASS"]),
    ("+steering", ["MOSAIC_FAB_STEP_NO_BANK", "MOSAIC_FAB_STEP_NO_BYPASS"]),
    ("+steering +bank", ["MOSAIC_FAB_STEP_NO_BYPASS"]),
    ("+steering +bank +bypass", []),
    ("bypass only (fixed affinity)",
     ["MOSAIC_FAB_STEP_NO_STEERING", "MOSAIC_FAB_STEP_NO_BANK"]),
]


def build(entry: dict, defines: list, build_dir: str) -> str:
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
        cmd += ["-D%s" % define]
    cmd += ["-o", os.path.join(build_dir, CASE_ID)]
    cmd += sources

    with open(os.path.join(build_dir, "build_command.txt"), "w") as handle:
        handle.write(" ".join(cmd))
    result = run_unit.run(cmd)
    if result.returncode != 0:
        raise RuntimeError("build failed for %s\n%s"
                           % (defines, result.stdout.decode("utf-8", "replace")))
    return os.path.join(build_dir, CASE_ID)


def run(binary: str, out_dir: str, max_cycles: int) -> str:
    os.makedirs(out_dir, exist_ok=True)
    return subprocess.run(
        [binary, "--case", CASE_ID, "--out", out_dir, "--seed", "1",
         "--max-cycles", str(max_cycles)],
        cwd=REPO_ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    ).stdout.decode("utf-8", "replace")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default=os.path.join(REPO_ROOT, "build", "p0",
                                                      "fabric_steps"))
    args = parser.parse_args()

    entry = run_unit.load_registry()["cases"][CASE_ID]
    max_cycles = entry.get("max_cycles", 200000)
    # The switches must exist before a build can be claimed to have used them.
    source = os.path.join(REPO_ROOT, "rtl", "core", "mosaic_core.sv")
    with open(source) as handle:
        text = handle.read()
    for define in ["MOSAIC_FAB_STEP_NO_STEERING", "MOSAIC_FAB_STEP_NO_BANK",
                   "MOSAIC_FAB_STEP_NO_BYPASS"]:
        if text.count(define) < 1:
            print("FAIL: %s does not occur in %s" % (define, source))
            return 1

    print("%-28s %s" % ("dynamic configuration", "dynamic run"))
    print("-" * 120)
    for label, defines in CONFIGS:
        build_dir = os.path.join(args.out, re.sub(r"[^A-Za-z0-9]+", "_", label))
        binary = build(entry, defines, build_dir)
        log = run(binary, os.path.join(build_dir, "out"), max_cycles)
        match = re.search(r"  dynamic  (.*)", log)
        print("%-28s %s" % (label, match.group(1) if match else "(no dynamic line)"))
    print("-" * 120)
    print("the numbers above are the report's step table; the partial "
          "configurations fail the case's dynamic-activity checks by "
          "construction and only their numbers are read")
    return 0


if __name__ == "__main__":
    sys.exit(main())
