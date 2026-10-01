#!/usr/bin/env python3
"""Build and run unit testbench cases listed in tests/unit/registry.json.

    python3 tools/run_unit.py --case fifo.backpressure
    python3 tools/run_unit.py --profile p0 --all

Each case is a self-contained Verilator build: a SystemVerilog top-level
wrapper plus a C++ testbench, compiled into ``build/<profile>/unit/<case-id>``.

Exit status is 0 only when every requested case builds, runs, prints a matching
``RESULT`` line, and exits 0. A case that hangs, crashes, prints no RESULT line,
or prints FAIL is a failure -- never a silent pass.
"""

from __future__ import annotations

import argparse
import json
import os
import shlex
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from mosaic import config_check  # noqa: E402

REPO_ROOT = config_check.REPO_ROOT
REGISTRY = os.path.join(REPO_ROOT, "tests", "unit", "registry.json")
SIM_COMMON = os.path.join(REPO_ROOT, "sim", "common")

# Support code every testbench needs. It is added by the runner rather than
# listed per case, so a new case cannot forget it and end up with a link error,
# and cannot list it twice and end up with duplicate symbols.
SHARED_CPP = ["sim/common/sim_common.cpp"]

VERILATOR_FLAGS = [
    "--cc",
    "--exe",
    "--build",
    "-j",
    "0",
    "-O2",
    # Only -Wall reaches the simulator build: Verilator's own runtime headers are
    # compiled with the same flags and are not warning-clean under -Wextra. Our
    # sources are held to the stricter standard by the `lint-cpp` make target,
    # which compiles them on their own without the generated runtime.
    "-CFLAGS",
    "-O2 -std=c++17 -Wall",
    "--x-assign",
    "unique",
    "--x-initial",
    "unique",
]


def load_registry() -> dict:
    with open(REGISTRY) as handle:
        return json.load(handle)


def run(cmd, cwd=REPO_ROOT):
    return subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def build_case(profile: str, case_id: str, entry: dict) -> str:
    """Compile one case; returns the log text. Raises RuntimeError on failure."""
    build_dir = os.path.join(REPO_ROOT, "build", profile, "unit", case_id)
    binary = os.path.join(build_dir, case_id)
    os.makedirs(build_dir, exist_ok=True)

    # Packages are listed before the files that use them. Verilator reads sources
    # in command-line order, so a package declared after the module that imports it
    # makes every package type look undeclared -- the same failure the linter had,
    # and for the same reason.
    rtl = entry.get("rtl", [])
    packages = [p for p in rtl if os.path.basename(p) == "mosaic_pkg.sv"]
    rest = [p for p in rtl if p not in packages]
    sources = [os.path.join(REPO_ROOT, p)
               for p in (packages + rest + entry.get("sv", [])
                         + entry.get("cpp", []) + SHARED_CPP)]
    missing = [p for p in sources if not os.path.exists(p)]
    if missing:
        raise RuntimeError("case %s lists missing sources: %s" % (case_id, ", ".join(missing)))

    cmd = ["verilator"] + VERILATOR_FLAGS
    cmd += ["--top-module", entry["top"], "-Mdir", os.path.join(build_dir, "obj_dir")]
    cmd += ["-I%s" % os.path.join(REPO_ROOT, "build", profile, "sim")]
    # Every core testbench includes mosaic_pkg.sv; it is part of the shared
    # sources rather than of any one case, for the same reason sim_common.cpp is.
    cmd += ["-I%s" % os.path.join(REPO_ROOT, "rtl", "core")]
    cmd += ["-I%s" % os.path.join(REPO_ROOT, "rtl", "common")]
    # The generated RTL config package. Verilator does NOT search the including
    # file's own directory, so an RTL file that does `include "mosaic_cfg_pkg.svh"`
    # cannot find it unless the generated directory is on the include path. The
    # linter and lint-slang already pass this directory; the runner did not, which
    # pushed RTL toward hardcoding sizes the geometry file controls. Reported by an
    # agent rather than diagnosed here.
    cmd += ["-I%s" % os.path.join(REPO_ROOT, "build", profile, "rtl")]
    cmd += ["-CFLAGS", "-I%s" % SIM_COMMON]
    cmd += ["-CFLAGS", "-I%s" % os.path.join(REPO_ROOT, "build", profile, "sim")]
    cmd += ["-o", binary]
    cmd += sources

    # A stale build directory is a verification hazard, not a speed feature.
    # Verilator generates its own makefile for the C++ half, and when only a
    # `-D` changes -- which is exactly what a mutant run does -- the generated
    # sources can look up to date and the *previous* binary is kept. One lane
    # saw a mutant's behaviour survive what looked like a clean rebuild, and a
    # mutant table built that way is not evidence. So: record the command that
    # produced the directory and wipe the directory when the command differs.
    # Cheap to compute, and it makes it impossible for a mutant run to be
    # confused with the shipping build.
    stamp_path = os.path.join(build_dir, "build_command.txt")
    stamp = " ".join(cmd)
    previous = None
    if os.path.exists(stamp_path):
        with open(stamp_path) as handle:
            previous = handle.read().strip()
    if previous != stamp:
        if os.path.isdir(os.path.join(build_dir, "obj_dir")):
            shutil.rmtree(os.path.join(build_dir, "obj_dir"), ignore_errors=True)
        if os.path.exists(binary):
            os.remove(binary)
        with open(stamp_path, "w") as handle:
            handle.write(stamp)

    result = run(cmd)
    log = result.stdout.decode("utf-8", "replace")
    if result.returncode != 0 or not os.path.exists(binary):
        raise RuntimeError("build failed for case %s\n%s" % (case_id, log))
    return log


def execute_case(profile: str, case_id: str, entry: dict, out_root: str,
                  seed_override=None) -> bool:
    build_dir = os.path.join(REPO_ROOT, "build", profile, "unit", case_id)
    binary = os.path.join(build_dir, case_id)
    out_dir = os.path.join(out_root, case_id)
    os.makedirs(out_dir, exist_ok=True)

    seed = seed_override if seed_override is not None else entry.get("seed", 1)
    max_cycles = entry.get("max_cycles", 100000)
    cmd = [binary, "--case", case_id, "--out", out_dir,
           "--seed", str(seed), "--max-cycles", str(max_cycles)]
    log_path = os.path.join(out_dir, "run.log")

    try:
        result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=max(60, int(max_cycles / 1000)))
    except subprocess.TimeoutExpired as exc:
        with open(log_path, "wb") as handle:
            handle.write((exc.output or b"") + b"\nHARNESS TIMEOUT\n")
        print("FAIL %-28s harness timed out" % case_id)
        return False

    log = result.stdout.decode("utf-8", "replace")
    with open(log_path, "w") as handle:
        handle.write("$ %s\n%s" % (" ".join(shlex.quote(part) for part in cmd), log))

    verdict = None
    for line in log.splitlines():
        if line.startswith("RESULT "):
            parts = line.split(None, 3)
            verdict = parts[1] if len(parts) > 1 else None
            if len(parts) > 2 and parts[2] != case_id:
                print("FAIL %-28s RESULT line names %r, expected %r"
                      % (case_id, parts[2], case_id))
                return False

    if verdict is None:
        print("FAIL %-28s printed no RESULT line (exit %d)" % (case_id, result.returncode))
        return False
    if result.returncode != 0 or verdict != "PASS":
        print("FAIL %-28s verdict=%s exit=%d  see %s"
              % (case_id, verdict, result.returncode, os.path.relpath(log_path, REPO_ROOT)))
        return False

    print("PASS %-28s task=%s" % (case_id, entry.get("task", "?")))
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", action="append", default=[])
    parser.add_argument("--profile", default="p0")
    parser.add_argument("--all", action="store_true")
    parser.add_argument("--seed", type=int)
    parser.add_argument("--out", default=os.path.join(REPO_ROOT, "results", "unit"))
    parser.add_argument("--list", action="store_true")
    args = parser.parse_args()

    registry = load_registry()
    cases = registry["cases"]

    if args.list:
        for case_id in sorted(cases):
            print("%-28s %s" % (case_id, cases[case_id].get("task", "?")))
        return 0

    if args.all:
        selected = sorted(cases)
    elif args.case:
        selected = args.case
    else:
        parser.error("one of --case or --all is required")

    status = 0
    for case_id in selected:
        if case_id not in cases:
            print("FAIL %-28s not registered in tests/unit/registry.json" % case_id)
            status = 1
            continue
        try:
            build_case(args.profile, case_id, cases[case_id])
        except RuntimeError as exc:
            print("FAIL %-28s %s" % (case_id, exc))
            status = 1
            continue
        if not execute_case(args.profile, case_id, cases[case_id], args.out, args.seed):
            status = 1

    return status


if __name__ == "__main__":
    sys.exit(main())