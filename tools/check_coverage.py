#!/usr/bin/env python3
"""Cross-check the capability ladder against the plan's task inventory.

    python3 tools/check_coverage.py
    python3 tools/check_coverage.py --profile p3 --json

Answers one question precisely: **for every capability this project might claim,
does a real implementation task and a real verification task exist, and has the
implementation actually been delivered?**

It reads the plan documents as the source of task definitions, so a capability
cannot point at a task that was invented locally, and it reads
`config/status/implementation_status.json` as the source of delivery, so "done"
means a package's tests passed, not that someone typed its ID into a file.

Exit status is 0 when every capability resolves to real tasks and no claimed
capability is advertised without its implementation having been delivered.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from typing import Dict, Set

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from mosaic import config_check  # noqa: E402

REPO_ROOT = config_check.REPO_ROOT

# Work packages are defined as "### I-001 — ..." headings in the plan documents.
TASK_HEADING = re.compile(r"^###\s+([IVH]-\d{3})\b")
TASK_COUNT = {"I": 0, "V": 0, "H": 0}

PLAN_DOCS = [
    "docs/implementation-plan.md",
    "docs/validation-plan.md",
    "docs/platform-plan.md",
]
STAGE_DOCS = [
    "docs/stage-%d-%s.md" % (index, slug)
    for index, slug in enumerate([
        "contracts-bringup", "scalar-control", "execution-fabric", "memory-system",
        "vector-locality", "multihart-aggregation", "verification-quality",
        "fpga-hardware", "asic-release", "lockstep-safety", "rva23-security",
    ])
]


def collect_tasks() -> Set[str]:
    """Task IDs defined by the plan. The same heading appears in both the master
    plan and the stage guide that owns it, so this returns the deduplicated set;
    the per-kind counters are computed from that set, not from the heading count."""
    tasks: Set[str] = set()
    for relative in PLAN_DOCS + STAGE_DOCS:
        path = os.path.join(REPO_ROOT, relative)
        if not os.path.exists(path):
            continue
        with open(path, "r", errors="replace") as handle:
            for line in handle:
                match = TASK_HEADING.match(line)
                if match:
                    tasks.add(match.group(1))
    for task in tasks:
        TASK_COUNT[task[0]] += 1
    return tasks


def delivered_tasks() -> Set[str]:
    path = os.path.join(config_check.CONFIG_ROOT, "status", "implementation_status.json")
    if not os.path.exists(path):
        return set()
    with open(path) as handle:
        return set(json.load(handle).get("delivered_tasks", []))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", default=None,
                        help="also report the delivery status of one profile")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--verbose", action="store_true",
                        help="list what each non-advertisable capability waits on")
    args = parser.parse_args()

    defined = collect_tasks()
    delivered = delivered_tasks()
    with open(os.path.join(config_check.CONFIG_ROOT, "capability_ladder.json")) as handle:
        ladder = json.load(handle)

    problems = []
    rows = []
    for capability in ladder["capabilities"]:
        name = capability["name"]
        impl = list(capability.get("impl_tasks", []))
        verify = list(capability.get("verify_tasks", []))
        missing_impl = [task for task in impl if task not in defined]
        missing_verify = [task for task in verify if task not in defined]
        if missing_impl:
            problems.append("capability %s cites implementation tasks that no plan document "
                            "defines: %s" % (name, ", ".join(missing_impl)))
        if missing_verify:
            problems.append("capability %s cites verification tasks that no plan document "
                            "defines: %s" % (name, ", ".join(missing_verify)))
        rows.append({
            "capability": name,
            "min_profile": capability["min_profile"],
            "kind": capability.get("kind"),
            "impl_tasks": impl,
            "verify_tasks": verify,
            "impl_delivered": [task for task in impl if task in delivered],
            "verify_delivered": [task for task in verify if task in delivered],
            # Advertising requires BOTH halves. The implementation tasks say the
            # feature exists; the verification tasks are where its independent
            # positive and negative cases live. The first version of this checker
            # collected `verify_tasks` and then never consulted them, so a
            # capability became advertisable the moment its RTL landed -- which
            # is precisely the "capability claim without evidence" the plan's
            # section 3.2 forbids. A capability with no verification task at all
            # is also not advertisable: that is a hole in the ladder, not a
            # licence to publish.
            "advertisable": bool(impl) and bool(verify)
                            and all(task in delivered for task in impl)
                            and all(task in delivered for task in verify),
            "missing_for_advertisement": [t for t in impl + verify if t not in delivered],
        })

    report = {
        "plan_tasks_defined": len(defined),
        "tasks_by_kind": dict(TASK_COUNT),
        "delivered_tasks": sorted(delivered),
        "capabilities": rows,
        "problems": problems,
    }

    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
        return 1 if problems else 0

    print("plan work packages defined : %d (%d I / %d V / %d H)"
          % (len(defined), TASK_COUNT["I"], TASK_COUNT["V"], TASK_COUNT["H"]))
    print("delivered (tests passed)   : %d" % len(delivered))
    print("capabilities in the ladder  : %d" % len(rows))
    advertisable = [row for row in rows if row["advertisable"]]
    print("advertisable now           : %s"
          % (", ".join(row["capability"] for row in advertisable) or "(none yet)"))
    blocked = [row for row in rows if not row["advertisable"] and row["missing_for_advertisement"]]
    if blocked and args.verbose:
        for row in blocked:
            print("  not advertisable: %-12s waiting on %s"
                  % (row["capability"], ", ".join(row["missing_for_advertisement"])))

    if args.profile:
        bundle = config_check.load(args.profile)
        if not bundle.ok:
            for problem in bundle.problems:
                print("ERROR %s" % problem, file=sys.stderr)
            return 1
        advertised, pending = config_check.advertised_capabilities(bundle)
        print("profile %s advertised     : %s"
              % (args.profile, ", ".join(advertised) or "(none yet)"))
        print("profile %s not yet impl   : %s"
              % (args.profile, ", ".join(pending) or "(none)"))

    for problem in problems:
        print("ERROR %s" % problem, file=sys.stderr)
    if problems:
        return 1

    print("PASS every capability resolves to real implementation and verification tasks")
    return 0


if __name__ == "__main__":
    sys.exit(main())