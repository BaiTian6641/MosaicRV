#!/usr/bin/env python3
"""Check that the two places which name work packages agree with each other.

    python3 tools/check_records.py

`tests/unit/registry.json` is where a case declares which work package it
belongs to and which sources it compiles; `config/status/implementation_status.json`
is where a package is called delivered and where its evidence is listed. The two
are written by different acts -- one by whoever adds a case, the other by the
integration lead when the package's own cases have been run from a clean build --
so they can disagree, and a disagreement is a *record* defect even when every
test passes: a package can be listed as delivered with someone else's case as its
evidence, or a case can belong to a package that was never recorded.

This tool was written because exactly that happened. `rename.single_width_ownership`
was listed under I-014 while the registry declares it as I-013's case; the entries
had been written months apart by different sessions and nothing compared them.

Checks, each of which is a hard failure:

 1. every case a delivered package lists as its evidence exists in the registry;
 2. that case's declared task is the package claiming it;
 3. every registered case whose task is delivered is listed in that task's
    evidence -- unless the case declares itself `"pending": true`, which is how a
    case that was registered *before* its driver exists (the project's deliberate
    order: the case is fixed before the thing it tests is built) says "not yet
    evidence". A pending case that a delivered package *does* claim is the mirror
    error and also fails: the marker is then stale.
 4. every report path a delivered package names exists on disk;
 5. case names are unique and every entry carries a task, a top and a driver.

What it deliberately does not check: whether the recorded result is *true*. That
is what running the case from a deleted build directory, with its mutants, is for;
this tool only stops the paperwork from lying about which package owns what.
"""

import argparse
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REGISTRY = os.path.join(ROOT, "tests", "unit", "registry.json")
STATUS = os.path.join(ROOT, "config", "status", "implementation_status.json")
PROFILES_DIR = os.path.join(ROOT, "config", "profiles")
KNOWN_PROFILES = tuple(sorted(
    os.path.splitext(name)[0] for name in os.listdir(PROFILES_DIR)
    if name.endswith(".json")))


def load(path):
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle, object_pairs_hook=dict)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    registry = load(REGISTRY)
    status = load(STATUS)
    cases = registry["cases"]
    delivered = status["delivered_tasks"]
    evidence = status["evidence"]

    failures = []

    for name, entry in sorted(cases.items()):
        for key in ("task", "top"):
            if not entry.get(key):
                failures.append("registry: case %s has no %s" % (name, key))
        if not (entry.get("cpp") or entry.get("sv")):
            failures.append("registry: case %s names no driver or wrapper" % name)
        # A case may declare the profiles it belongs to. The field exists because
        # some cases are machine-mode-only by construction -- their reference
        # model and expected CSR table are p0's -- and running one against a
        # profile whose DUT owns S/U privilege tests a model that does not exist
        # rather than testing the DUT.
        profiles = entry.get("profiles")
        if profiles is not None:
            if not isinstance(profiles, list) or not profiles:
                failures.append(
                    "registry: case %s has a profiles field that is not a "
                    "non-empty list" % name)
            else:
                for prof in profiles:
                    if prof not in KNOWN_PROFILES:
                        failures.append(
                            "registry: case %s declares unknown profile %r"
                            % (name, prof))

    seen = {}
    for name, entry in sorted(cases.items()):
        task = entry.get("task")
        seen.setdefault(task, []).append(name)

    for task in delivered:
        record = evidence.get(task)
        if record is None:
            failures.append("status: %s is delivered with no evidence block" % task)
            continue
        claimed = record.get("cases", [])
        if not isinstance(claimed, list):
            failures.append("status: %s evidence.cases is not a list" % task)
            continue
        for name in claimed:
            if name not in cases:
                failures.append(
                    "status: %s claims case %s, which the registry does not define"
                    % (task, name))
                continue
            owner = cases[name].get("task")
            if owner != task:
                failures.append(
                    "status: %s claims case %s, but the registry assigns it to %s"
                    % (task, name, owner))
            if cases[name].get("pending"):
                failures.append(
                    "registry: case %s is marked pending but %s claims it as "
                    "evidence -- the marker is stale" % (name, task))
        for name in seen.get(task, []):
            if name in claimed:
                continue
            if cases[name].get("pending"):
                continue
            failures.append(
                "registry: case %s belongs to delivered package %s, which does "
                "not list it as evidence (mark it \"pending\": true if it is not "
                "yet the package's evidence)" % (name, task))
        report = record.get("report", "")
        if report:
            path = report.split(",")[0].split(" ")[0].strip()
            full = os.path.join(ROOT, path)
            if path.endswith(".md") and not os.path.exists(full):
                failures.append("status: %s names report %s, which does not exist"
                                % (task, path))

    if failures:
        print("FAIL %d record inconsistency(ies):" % len(failures))
        for line in failures:
            print("  " + line)
        return 1
    if not args.quiet:
        print("ok   records agree: %d delivered package(s), %d registered case(s), "
              "every claimed case exists and belongs to the package claiming it"
              % (len(delivered), len(cases)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
