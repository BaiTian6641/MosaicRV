#!/usr/bin/env python3
"""Check the exclusion ledger: every thing this project does not check is registered.

    python3 tools/check_exclusions.py
    python3 tools/check_exclusions.py --negative

config/validation/exclusion_ledger.json is the machine-readable form of every
disabled checker, skipped case or suite, waived field, unimplemented reference
feature, timeout that stands in for a verdict, and generation step that can drop
a test. This tool decides whether that file can be trusted, and it decides it by
reading the two things that can contradict it: tests/unit/registry.json (the
cases that exist) and results/unit/<case>/result.json (what each case recorded).

What it proves:

 1. the ledger validates against config/schema/exclusion_ledger.schema.json,
    with the project's own dependency-free validator;
 2. every exclusion carries a reason, a specification basis (or an explicit
    statement that none applies), an affected scope, and an end condition;
 3. every `affected_cases` name exists in the registry;
 4. every `alternative_verification` case exists in the registry *and* has a
    recorded PASS -- an alternative verification that does not pass covers
    nothing;
 5. every named alternative-verification check exists on disk, and a check that
    is itself a gate (`tools/check_*.py`) is wired into the Makefile;
 6. a `waiver` names independent verification: a waiver may never be the only
    cover for a committed feature;
 7. every registered case that has no recorded PASS is named in the ledger --
    a case with no result is an unregistered exclusion, and that is the failure
    this tool exists to produce;
 8. the closure counts are printed: checks disabled, cases skipped, fields
    waived, open versus covered, so the number is visible rather than implied.

`--negative` builds a sandbox copy of the ledger, the schema, the registry, the
recorded results and the Makefile, mutates one input at a time, and requires
every mutation to be rejected. A control that is merely present is not evidence;
each one must be observed to fail. The controls include the card's deliberate
unregistered exclusion: a registry case with no result that the ledger does not
name.

What it deliberately does not check: whether an exclusion's reason is *true*.
That is what the cited report, the cited case and the case's own mutants are for.
This tool stops the ledger from disagreeing with the registry and the recorded
results; it cannot tell whether a well-written reason is a good one.

Exit status is 0 only when the whole ledger checks out. `--negative` additionally
requires every mutation to be rejected.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import tempfile
from typing import Any, Dict, List, Optional, Tuple

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(HERE)

sys.path.insert(0, HERE)

from mosaic import jsonschema_mini  # noqa: E402

SCHEMA_REL = os.path.join("config", "schema", "exclusion_ledger.schema.json")
LEDGER_REL = os.path.join("config", "validation", "exclusion_ledger.json")
REGISTRY_REL = os.path.join("tests", "unit", "registry.json")
RESULTS_REL = os.path.join("results", "unit")
MAKEFILE_REL = "Makefile"

# The kinds the ledger may use. Kept here as well as in the schema so that a kind
# added to one and not the other is caught rather than silently accepted.
KINDS = (
    "disabled_checker",
    "skip",
    "waiver",
    "unimplemented_reference_feature",
    "timeout",
    "generation_failure",
)

# A waiver must name something other than itself: the pass criterion is that no
# committed feature can be accepted merely by a waiver. The rule is enforced in
# check(); the checker refuses a waiver whose alternative verification is open.


def load(path: str) -> Any:
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle, object_pairs_hook=dict)


class Problem(object):
    __slots__ = ("where", "message")

    def __init__(self, where: str, message: str) -> None:
        self.where = where
        self.message = message

    def __str__(self) -> str:
        return "[%s] %s" % (self.where, self.message)


def read_text(path: str) -> str:
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


def recorded_verdict(root: str, case: str) -> Optional[str]:
    """The verdict in results/unit/<case>/result.json, or None if unrecorded."""
    path = os.path.join(root, RESULTS_REL, case, "result.json")
    if not os.path.exists(path):
        return None
    try:
        document = load(path)
    except (ValueError, OSError):
        return "UNREADABLE"
    verdict = document.get("verdict")
    return verdict if isinstance(verdict, str) else "MALFORMED"


def check(root: str) -> Tuple[List[Problem], Dict[str, int]]:
    """Return (problems, counts). counts is only meaningful when problems is empty."""
    problems: List[Problem] = []

    schema = load(os.path.join(root, SCHEMA_REL))
    ledger = load(os.path.join(root, LEDGER_REL))
    registry = load(os.path.join(root, REGISTRY_REL))
    cases = registry["cases"]
    makefile = read_text(os.path.join(root, MAKEFILE_REL))

    # -- 1. schema -----------------------------------------------------------
    for path, message in jsonschema_mini.validate(ledger, schema):
        problems.append(Problem("schema", "%s: %s" % (path, message)))
    if problems:
        # The structural checks below assume the document's shape.
        return problems, {}

    # The schema's kind enum and this tool's must agree.
    schema_kinds = tuple(
        sorted(schema["properties"]["exclusions"]["items"]["properties"]["kind"]["enum"]))
    if schema_kinds != tuple(sorted(KINDS)):
        problems.append(Problem(
            "schema", "the schema's kind enum %s does not match the checker's %s"
            % (list(schema_kinds), list(KINDS))))
    semantics = tuple(sorted(ledger["kind_semantics"].keys()))
    if semantics != tuple(sorted(KINDS)):
        problems.append(Problem(
            "ledger", "kind_semantics defines %s, expected exactly %s"
            % (list(semantics), list(KINDS))))

    exclusions = ledger["exclusions"]

    # -- 2. identity and per-entry rules -------------------------------------
    seen_ids: Dict[str, int] = {}
    for index, entry in enumerate(exclusions):
        where = entry.get("id") or ("exclusion #%d" % index)
        if where in seen_ids:
            problems.append(Problem(where, "duplicate id (also at index %d)"
                                    % seen_ids[where]))
        seen_ids[where] = index

        kind = entry["kind"]
        alternative = entry["alternative_verification"]
        status = alternative["status"]

        if status == "covered" and not (
                alternative["cases"] or alternative["checks"]):
            problems.append(Problem(
                where, "covered but names neither a case nor a check; an "
                "alternative verification that names nothing covers nothing"))

        if status == "open" and (alternative["cases"] or alternative["checks"]):
            problems.append(Problem(
                where, "open but names %s; an open exclusion must not claim "
                "coverage -- say so in the note instead"
                % (alternative["cases"] + alternative["checks"])))

        if kind == "waiver" and status != "covered":
            problems.append(Problem(
                where, "a waiver must name independent verification that passes "
                "(the pass criterion forbids accepting a committed feature by a "
                "waiver alone)"))

        if not entry["end_condition"].strip():
            problems.append(Problem(where, "empty end condition"))
        if not entry["reason"].strip():
            problems.append(Problem(where, "empty reason"))
        if not entry["spec_clause"].strip():
            problems.append(Problem(where, "empty spec_clause"))
        if not entry["affected_scope"].strip():
            problems.append(Problem(where, "empty affected_scope"))

    # -- 3/4. cases the entry names ------------------------------------------
    for entry in exclusions:
        where = entry.get("id", "?")
        alternative = entry["alternative_verification"]
        for name in alternative["cases"]:
            if name not in cases:
                problems.append(Problem(
                    where, "alternative verification names case %s, which the "
                    "registry does not define" % name))
                continue
            verdict = recorded_verdict(root, name)
            if verdict != "PASS":
                problems.append(Problem(
                    where, "alternative verification names case %s, whose "
                    "recorded result is %s, not PASS" % (name, verdict or "absent")))
        for check_path in alternative["checks"]:
            full = os.path.join(root, check_path)
            if not os.path.exists(full):
                problems.append(Problem(
                    where, "alternative verification names check %s, which does "
                    "not exist" % check_path))
                continue
            if os.path.basename(check_path).startswith("check_") and \
                    check_path not in makefile:
                problems.append(Problem(
                    where, "alternative verification names the gate %s, which "
                    "the Makefile does not invoke" % check_path))
        for affected in entry["affected_cases"]:
            if affected["case"] not in cases:
                problems.append(Problem(
                    where, "affected_cases names case %s, which the registry "
                    "does not define" % affected["case"]))

    # -- 7. every case without a recorded PASS must be registered ------------
    named = set()
    for entry in exclusions:
        for affected in entry["affected_cases"]:
            named.add(affected["case"])
        named.update(entry["alternative_verification"]["cases"])

    unregistered = []
    for name in sorted(cases):
        if recorded_verdict(root, name) != "PASS" and name not in named:
            unregistered.append(name)
    for name in unregistered:
        problems.append(Problem(
            "unregistered", "case %s has no recorded PASS and no ledger entry "
            "names it; that is an unregistered exclusion" % name))

    if problems:
        return problems, {}

    # -- 8. closure counts ---------------------------------------------------
    counts = {
        "total": len(exclusions),
        "open": sum(1 for e in exclusions
                    if e["alternative_verification"]["status"] == "open"),
        "covered": sum(1 for e in exclusions
                       if e["alternative_verification"]["status"] == "covered"),
    }
    for kind in KINDS:
        counts[kind] = sum(1 for e in exclusions if e["kind"] == kind)

    skipped_cases = set()
    for entry in exclusions:
        if entry["kind"] == "skip":
            for affected in entry["affected_cases"]:
                skipped_cases.add(affected["case"])
    counts["cases_skipped"] = len(skipped_cases)

    waived_fields = set()
    for entry in exclusions:
        if entry["kind"] != "waiver":
            continue
        for affected in entry["affected_cases"]:
            for field in affected["fields"]:
                waived_fields.add((affected["case"], field))
    counts["fields_waived"] = len(waived_fields)

    counts["checks_disabled"] = counts["disabled_checker"]
    return [], counts


# ---------------------------------------------------------------------------
# negative controls: mutate one input, require the mutation to be rejected
# ---------------------------------------------------------------------------


class NegativeControls(object):
    def __init__(self, root: str) -> None:
        self.root = root
        self.failures: List[str] = []
        self.total = 0

    def _sandbox(self) -> str:
        workdir = tempfile.mkdtemp(prefix="mosaic-exclusions-negative-")
        for relative in (SCHEMA_REL, LEDGER_REL, REGISTRY_REL, MAKEFILE_REL):
            target = os.path.join(workdir, relative)
            os.makedirs(os.path.dirname(target), exist_ok=True)
            shutil.copyfile(os.path.join(self.root, relative), target)
        # The alternative-verification checks must resolve, or every control would
        # be rejected for the wrong reason (a missing tool) instead of its defect.
        shutil.copytree(os.path.join(self.root, "tools"),
                        os.path.join(workdir, "tools"),
                        ignore=shutil.ignore_patterns("__pycache__"))
        results = os.path.join(self.root, RESULTS_REL)
        if os.path.isdir(results):
            for name in sorted(os.listdir(results)):
                result = os.path.join(results, name, "result.json")
                if os.path.isfile(result):
                    target = os.path.join(workdir, RESULTS_REL, name, "result.json")
                    os.makedirs(os.path.dirname(target), exist_ok=True)
                    shutil.copyfile(result, target)
        return workdir

    def case(self, name: str, mutator) -> None:
        self.total += 1
        workdir = self._sandbox()
        try:
            try:
                mutator(workdir)
            except AssertionError as exc:
                # A mutation whose target has vanished is a broken control, not a
                # passing one: it would silently stop testing anything.
                self.failures.append("%s (mutation does not apply: %s)" % (name, exc))
                print("  BROKEN CONTROL: %s (%s)" % (name, exc), file=sys.stderr)
                return
            problems, _ = check(workdir)
            if not problems:
                self.failures.append(name)
                print("  NOT REJECTED : %s" % name, file=sys.stderr)
            else:
                print("  rejected: %-52s %s" % (name, problems[0]))
        finally:
            shutil.rmtree(workdir, ignore_errors=True)

    # -- helpers -------------------------------------------------------------

    def _edit_ledger(self, workdir: str, mutator) -> None:
        path = os.path.join(workdir, LEDGER_REL)
        document = load(path)
        mutator(document)
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(document, handle, indent=2)

    def _entry(self, document: Dict[str, Any], index: int = 0) -> Dict[str, Any]:
        exclusions = document["exclusions"]
        if index >= len(exclusions):
            raise AssertionError("ledger has no exclusion at index %d" % index)
        return exclusions[index]

    def _covered_entry(self, document: Dict[str, Any]) -> Dict[str, Any]:
        for entry in document["exclusions"]:
            alternative = entry["alternative_verification"]
            if alternative["status"] == "covered" and alternative["cases"]:
                return entry
        raise AssertionError("no covered entry names a case")

    def _set_verdict(self, workdir: str, case: str, verdict: str) -> None:
        path = os.path.join(workdir, RESULTS_REL, case, "result.json")
        if not os.path.exists(path):
            raise AssertionError("no recorded result for case %r to mutate" % case)
        document = load(path)
        document["verdict"] = verdict
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(document, handle, indent=2)

    # -- the controls --------------------------------------------------------

    def run(self) -> int:
        baseline, counts = check(self.root)
        if baseline:
            print("negative controls skipped: the ledger does not check out "
                  "(%d problem(s)); fix it before using these controls as evidence"
                  % len(baseline), file=sys.stderr)
            return 1

        # Each mutator is a separate function so a reader can see every rule that
        # has a control; they are applied one at a time below.

        def drop_reason(document: Dict[str, Any]) -> None:
            self._entry(document)["reason"] = ""

        def empty_end_condition(document: Dict[str, Any]) -> None:
            self._entry(document)["end_condition"] = ""

        def unknown_kind(document: Dict[str, Any]) -> None:
            self._entry(document)["kind"] = "vibes"

        def duplicate_id(document: Dict[str, Any]) -> None:
            document["exclusions"][1]["id"] = document["exclusions"][0]["id"]

        def empty_subjects(document: Dict[str, Any]) -> None:
            self._entry(document)["subjects"] = []

        def missing_alternative(document: Dict[str, Any]) -> None:
            entry = self._covered_entry(document)
            entry["alternative_verification"]["cases"] = []
            entry["alternative_verification"]["checks"] = []

        def alternative_names_ghost(document: Dict[str, Any]) -> None:
            entry = self._covered_entry(document)
            entry["alternative_verification"]["cases"] = ["no.such_case"]

        def alternative_names_ghost_check(document: Dict[str, Any]) -> None:
            entry = self._covered_entry(document)
            entry["alternative_verification"]["checks"] = ["tools/check_ghost.py"]

        def waiver_without_cover(document: Dict[str, Any]) -> None:
            entry = self._covered_entry(document)
            entry["kind"] = "waiver"
            entry["alternative_verification"]["status"] = "open"
            entry["alternative_verification"]["cases"] = []
            entry["alternative_verification"]["checks"] = []

        def affected_case_ghost(document: Dict[str, Any]) -> None:
            entry = self._entry(document)
            entry["affected_cases"] = [{"case": "ghost.case", "fields": []}]

        def open_claims_coverage(document: Dict[str, Any]) -> None:
            entry = self._entry(document)
            if entry["alternative_verification"]["status"] != "open":
                for candidate in document["exclusions"]:
                    if candidate["alternative_verification"]["status"] == "open":
                        entry = candidate
                        break
            entry["alternative_verification"]["cases"] = [
                self._covered_entry(document)["alternative_verification"]["cases"][0]]

        # The observed mutations.
        self.case("exclusion with no reason",
                  lambda w: self._edit_ledger(w, drop_reason))
        self.case("empty end condition",
                  lambda w: self._edit_ledger(w, empty_end_condition))
        self.case("unknown kind",
                  lambda w: self._edit_ledger(w, unknown_kind))
        self.case("duplicate id",
                  lambda w: self._edit_ledger(w, duplicate_id))
        self.case("exclusion with no subjects",
                  lambda w: self._edit_ledger(w, empty_subjects))
        self.case("covered alternative naming no case or check",
                  lambda w: self._edit_ledger(w, missing_alternative))
        self.case("alternative verification names a case that does not exist",
                  lambda w: self._edit_ledger(w, alternative_names_ghost))
        self.case("alternative verification names a check that does not exist",
                  lambda w: self._edit_ledger(w, alternative_names_ghost_check))
        self.case("waiver with no independent verification",
                  lambda w: self._edit_ledger(w, waiver_without_cover))
        self.case("affected case that is not in the registry",
                  lambda w: self._edit_ledger(w, affected_case_ghost))
        self.case("open exclusion that claims coverage",
                  lambda w: self._edit_ledger(w, open_claims_coverage))

        def failing_case() -> Tuple[str, str]:
            document = load(os.path.join(self.root, LEDGER_REL))
            entry = self._covered_entry(document)
            return entry["id"], entry["alternative_verification"]["cases"][0]

        def alternative_names_failing_case(workdir: str) -> None:
            # Point the sandbox's result for the named case at FAIL. The ledger
            # is not mutated: the case's own record is.
            entry_id, case = failing_case()
            self._set_verdict(workdir, case, "FAIL")
        self.case("alternative verification names a case that fails",
                  alternative_names_failing_case)

        def unregistered_exclusion(workdir: str) -> None:
            # The card's deliberate trigger: a registered case with no recorded
            # result that the ledger does not name must be rejected.
            path = os.path.join(workdir, REGISTRY_REL)
            registry = load(path)
            registry["cases"]["ghost.unregistered"] = {
                "task": "V-020", "top": "ghost_tb", "sv": ["sim/tb/ghost_tb.sv"]}
            with open(path, "w", encoding="utf-8") as handle:
                json.dump(registry, handle, indent=2)
        self.case("registered case with no result that the ledger does not name",
                  unregistered_exclusion)

        if self.failures:
            print("negative controls: %d of %d were wrongly accepted: %s"
                  % (len(self.failures), self.total, ", ".join(self.failures)),
                  file=sys.stderr)
            return 1
        print("negative controls: %d/%d illegal ledgers rejected"
              % (self.total, self.total))
        return 0


# ---------------------------------------------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=REPO_ROOT,
                        help="root to read the ledger, registry and results from "
                             "(tests only)")
    parser.add_argument("--negative", action="store_true")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    problems, counts = check(args.root)
    if problems:
        print("FAIL %d exclusion-ledger problem(s):" % len(problems))
        for problem in problems:
            print("  " + str(problem))
        return 1

    if not args.quiet:
        print("ok   exclusions: %d registered, %d open, %d covered -- "
              "disabled checker=%d, skip=%d, waiver=%d, "
              "unimplemented reference feature=%d, timeout=%d, generation failure=%d"
              % (counts["total"], counts["open"], counts["covered"],
                 counts["disabled_checker"], counts["skip"], counts["waiver"],
                 counts["unimplemented_reference_feature"], counts["timeout"],
                 counts["generation_failure"]))
        # The closure counts the card asks to be visible rather than implied.
        print("ok   closure: checks disabled=%d, cases skipped=%d, fields waived=%d"
              % (counts["checks_disabled"], counts["cases_skipped"],
                 counts["fields_waived"]))

    status = 0
    if args.negative:
        status = NegativeControls(args.root).run()
    return status


if __name__ == "__main__":
    sys.exit(main())
