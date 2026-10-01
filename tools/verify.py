#!/usr/bin/env python3
"""Run the registered simulation suites for one profile.

    python3 tools/verify.py --profile p0 --suite scalar-directed-v1
    python3 tools/verify.py --profile p0 --all

Suites are declared in `tests/suites/registry.json`. Three rules from
docs/implementation-plan.md section 2 are enforced literally:

  * a command that succeeds exits 0;
  * `--suite <name>` that is not registered **fails** -- running an empty set and
    reporting success is exactly the failure mode that is being ruled out, so an
    empty suite registry is a failure, not a pass;
  * every run writes a JSON manifest with the command, the seed, the tool
    versions and a per-case verdict.

Exit status is 0 only when every requested suite exists and every case in it
passes.
"""

from __future__ import annotations

import argparse
import json
import os
import shlex
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from mosaic import config_check  # noqa: E402

REPO_ROOT = config_check.REPO_ROOT
SUITE_REGISTRY = os.path.join(REPO_ROOT, "tests", "suites", "registry.json")


def load_suites() -> dict:
    if not os.path.exists(SUITE_REGISTRY):
        return {}
    with open(SUITE_REGISTRY) as handle:
        return json.load(handle).get("suites", {})


def tool_versions() -> dict:
    def version(cmd):
        try:
            out = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 timeout=30)
            lines = out.stdout.decode("utf-8", "replace").strip().splitlines()
            return lines[0] if lines else ""
        except (OSError, subprocess.SubprocessError):
            return ""

    return {
        "verilator": version(["verilator", "--version"]),
        "python": sys.version.split()[0],
    }


def run_suite(profile: str, name: str, suite: dict, seed: int, out_root: str) -> bool:
    """Runs one suite by invoking tools/run_unit.py for each of its cases."""
    cases = suite.get("cases", [])
    if not cases:
        print("FAIL suite %-28s declares no cases; an empty suite cannot pass" % name)
        return False

    runner = os.path.join(REPO_ROOT, "tools", "run_unit.py")
    out_dir = os.path.join(out_root, name)
    os.makedirs(out_dir, exist_ok=True)

    passed = True
    verdicts = []
    for case_id in cases:
        cmd = [sys.executable, runner, "--profile", profile, "--case", case_id,
               "--seed", str(seed), "--out", os.path.join(out_dir, case_id)]
        started = time.time()
        result = subprocess.run(cmd, cwd=REPO_ROOT, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT)
        log = result.stdout.decode("utf-8", "replace")
        with open(os.path.join(out_dir, case_id + ".log"), "w") as handle:
            handle.write("$ %s\n%s" % (" ".join(shlex.quote(part) for part in cmd), log))
        ok = result.returncode == 0 and "PASS" in log
        verdicts.append({
            "case": case_id,
            "verdict": "PASS" if ok else "FAIL",
            "exit_code": result.returncode,
            "seconds": round(time.time() - started, 3),
        })
        if not ok:
            passed = False
            print("FAIL %-28s %s" % (name, case_id))

    manifest = {
        "schema_version": 1,
        "profile": profile,
        "suite": name,
        "seed": seed,
        "cases": verdicts,
        "verdict": "PASS" if passed else "FAIL",
        "tool_versions": tool_versions(),
        "command": "python3 tools/verify.py --profile %s --suite %s" % (profile, name),
    }
    with open(os.path.join(out_dir, "manifest.json"), "w") as handle:
        json.dump(manifest, handle, indent=2, sort_keys=True)
        handle.write("\n")

    if passed:
        print("PASS %-28s %d case(s)" % (name, len(verdicts)))
    return passed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", default="p0")
    parser.add_argument("--suite", action="append", default=[])
    parser.add_argument("--all", action="store_true")
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--out", default=os.path.join(REPO_ROOT, "results", "suites"))
    args = parser.parse_args()

    suites = load_suites()

    if args.list:
        for name in sorted(suites):
            print("%-28s %s" % (name, ", ".join(suites[name].get("cases", []))))
        return 0

    if args.all:
        selected = sorted(suites)
    elif args.suite:
        selected = args.suite
    else:
        parser.error("one of --suite or --all is required")

    status = 0
    for name in selected:
        if name not in suites:
            print("FAIL unknown suite %r; registered suites: %s"
                  % (name, ", ".join(sorted(suites)) or "(none)"), file=sys.stderr)
            status = 1
            continue
        if not run_suite(args.profile, name, suites[name], args.seed, args.out):
            status = 1

    if args.all and not selected:
        print("FAIL no suites are registered for profile %s; an empty suite set must "
              "not report success" % args.profile, file=sys.stderr)
        status = 1

    return status


if __name__ == "__main__":
    sys.exit(main())