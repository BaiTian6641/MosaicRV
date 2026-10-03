#!/usr/bin/env python3
"""Before/after measurement for the memory-subsystem scale deliverable.

Rebuilds CASE=perf.equal_resource_compare four ways and reads the `stream`
(baseline configuration) and `alu_chain` rows from each run's table:

  before     256 B direct-mapped L1, blocking read path   -DMOSAIC_MEM_SMALL_CACHE -DMOSAIC_CACHE_BLOCKING
  size       8 KB L1, blocking read path                  -DMOSAIC_CACHE_BLOCKING
  mshr       256 B L1, non-blocking read path             -DMOSAIC_MEM_SMALL_CACHE
  both       8 KB L1, non-blocking read path              (the shipping build)

`MOSAIC_MEM_SMALL_CACHE` and `MOSAIC_CACHE_BLOCKING` are measurement switches,
not configurations: they are never defined in a shipping build (the four
controls prove the shipping build's behaviour).

The I-084 harness already asserts equal resources and architectural identity per
configuration, so every number here is a measurement of *this* geometry on
*these* programs with *this* seed, not a general claim.

Usage: run_memscale_measure.py [--out results/reports/memory-scale-measure.txt]
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import run_unit  # noqa: E402

REPO_ROOT = run_unit.REPO_ROOT
CASE_ID = "perf.equal_resource_compare"

# label -> list of -D defines
ARMS = [
    ("before", ["MOSAIC_MEM_SMALL_CACHE", "MOSAIC_CACHE_BLOCKING"]),
    ("size",   ["MOSAIC_CACHE_BLOCKING"]),
    ("mshr",   ["MOSAIC_MEM_SMALL_CACHE"]),
    ("both",   []),
]


def build(entry: dict, defines: list, build_dir: str) -> None:
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
    cmd += ["-I%s" % os.path.join(REPO_ROOT, "build", "p1", "sim")]
    cmd += ["-I%s" % os.path.join(REPO_ROOT, "rtl", "core")]
    cmd += ["-I%s" % os.path.join(REPO_ROOT, "rtl", "common")]
    cmd += ["-I%s" % os.path.join(REPO_ROOT, "build", "p1", "rtl")]
    cmd += ["-CFLAGS", "-I%s" % run_unit.SIM_COMMON]
    cmd += ["-CFLAGS", "-I%s" % os.path.join(REPO_ROOT, "build", "p1", "sim")]
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


def run_case(binary: str, out_dir: str, max_cycles: int) -> str:
    os.makedirs(out_dir, exist_ok=True)
    result = subprocess.run(
        [binary, "--case", CASE_ID, "--out", out_dir, "--seed", "1",
         "--max-cycles", str(max_cycles)],
        cwd=REPO_ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=max(300, int(max_cycles / 1000)),
    )
    return result.stdout.decode("utf-8", "replace")


def baseline_row(log: str, workload: str):
    """Return (cycles, insns, ipc, dmem, imem) for the baseline config row."""
    in_workload = False
    header = re.compile(r"^\s+config\s+cycles\s+insns\s+IPC\s+dmem\s+imem")
    row = re.compile(r"^\s+baseline\s+(\d+)\s+(\d+)\s+([\d.]+)\s+(\d+)\s+(\d+)")
    for line in log.splitlines():
        if line.startswith("  workload "):
            in_workload = line.split()[1] == workload
            continue
        if in_workload and header.match(line):
            continue
        if in_workload:
            m = row.match(line)
            if m:
                return (int(m.group(1)), int(m.group(2)), m.group(3),
                        int(m.group(4)), int(m.group(5)))
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default=None)
    args = parser.parse_args()

    registry = run_unit.load_registry()["cases"]
    entry = registry[CASE_ID]
    max_cycles = entry.get("max_cycles", 4000000)

    root = os.path.join(REPO_ROOT, "build", "p1", "memscale_measure")
    results = {}
    for label, defines in ARMS:
        print("building %-8s %s" % (label, " ".join(defines) or "(shipping)"))
        build_dir = os.path.join(root, label)
        build(entry, defines, build_dir)
        log = run_case(os.path.join(build_dir, CASE_ID),
                       os.path.join(root, "out-" + label), max_cycles)
        with open(os.path.join(root, "out-" + label, "run.log"), "w") as handle:
            handle.write(log)
        stream = baseline_row(log, "stream")
        alu = baseline_row(log, "alu_chain")
        if stream is None or alu is None:
            print("  could not read the table; RESULT line:")
            for line in log.splitlines():
                if line.startswith("RESULT"):
                    print("  " + line)
            return 1
        results[label] = (stream, alu)
        print("  stream baseline: cycles=%d insns=%d IPC=%s dmem=%d imem=%d"
              % stream)
        print("  alu_chain baseline: cycles=%d insns=%d IPC=%s dmem=%d imem=%d"
              % alu)

    lines = []
    lines.append("CASE=perf.equal_resource_compare, profile p1, seed 1")
    lines.append("stream workload, baseline configuration (all policy switches off)")
    lines.append("")
    lines.append("%-8s %-8s %8s %8s %7s %8s %6s" %
                 ("arm", "L1", "cycles", "insns", "IPC", "dmem", "imem"))
    l1 = {"before": "256 B", "size": "8 KB", "mshr": "256 B", "both": "8 KB"}
    nb = {"before": "no", "size": "no", "mshr": "yes", "both": "yes"}
    for label, _ in ARMS:
        c, n, ipc, dm, im = results[label][0]
        lines.append("%-8s %-8s %8d %8d %7s %8d %6d" %
                     (label, l1[label], c, n, ipc, dm, im))
    lines.append("")
    lines.append("alu_chain workload, baseline configuration (must be unchanged:")
    lines.append("no memory change may move an ALU-only chain)")
    lines.append("")
    lines.append("%-8s %8s %8s %7s %8s %6s" %
                 ("arm", "cycles", "insns", "IPC", "dmem", "imem"))
    for label, _ in ARMS:
        c, n, ipc, dm, im = results[label][1]
        lines.append("%-8s %8d %8d %7s %8d %6d" % (label, c, n, ipc, dm, im))
    lines.append("")
    lines.append("non-blocking read path: " +
                 ", ".join("%s=%s" % (k, nb[k]) for k, _ in ARMS))
    text = "\n".join(lines) + "\n"
    print()
    print(text)
    if args.out:
        with open(args.out, "w") as handle:
            handle.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
