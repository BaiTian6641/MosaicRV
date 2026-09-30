#!/usr/bin/env python3
"""Check the frozen interface contracts against the real profile geometry.

    python3 tools/check_contracts.py --profile p0
    python3 tools/check_contracts.py --profile p0 --negative

Widths and counter moduli in `config/contracts/*.json` are *expressions* over the
geometry, never hand-typed constants. This tool evaluates them and proves the
arithmetic actually holds for the profile:

  * every expression uses only the declared namespace (no builtins, no attribute
    access, no calls other than `clog2`);
  * every field's width is at least the width its justification claims;
  * every tag, generation and age counter's modulus **strictly exceeds** twice
    the largest distance two live values can be separated by, and strictly
    exceeds the number of values live at once;
  * no interface identifies in-flight work with a wrapping index alone;
  * every interface states an owner, a transfer rule, a cancel rule, a response
    rule, a credit return path, resources with unique allocate and release
    events, and at least one ABA counterexample.

Exit status is 0 only when all of that holds for every checked profile.
"""

from __future__ import annotations

import argparse
import ast
import json
import os
import shutil
import sys
import tempfile
from typing import Any, Dict, List, Optional

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from mosaic import config_check  # noqa: E402

CONTRACT_ROOT = os.path.join(config_check.CONFIG_ROOT, "contracts")

# A field whose name ends in one of these is a wrapping identity and therefore
# needs a companion generation/epoch field in the same interface.
GENERATION_REQUIREMENTS = {
    "rob_index": "rob_gen",
    "prf_tag": "prf_gen",
    "req_id": "epoch",
    "tx_id": "tx_gen",
    "route_id": "route_gen",
}


class Problem(object):
    def __init__(self, where: str, message: str) -> None:
        self.where = where
        self.message = message

    def __str__(self) -> str:
        return "%s: %s" % (self.where, self.message)


def clog2(value: int) -> int:
    """Ceiling log2, with clog2(1) == 0 and negative/zero rejected by the caller."""
    if value <= 0:
        raise ValueError("clog2 requires a positive value, got %r" % value)
    return (value - 1).bit_length()


class ExpressionEvaluator(object):
    """Evaluates contract expressions against a fixed namespace.

    Expressions are evaluated from an AST that has been checked node by node, so
    a contract file cannot reach `__import__`, an attribute, a subscript, or any
    builtin. Only names from the namespace, integer literals, `+ - * / // % **`,
    comparisons, parentheses and `clog2` are accepted.
    """

    ALLOWED_NODES = (
        ast.Expression, ast.BinOp, ast.UnaryOp, ast.Name, ast.Load, ast.Call,
        ast.Add, ast.Sub, ast.Mult, ast.FloorDiv, ast.Div, ast.Mod, ast.Pow,
        ast.USub, ast.UAdd, ast.Mod, ast.Compare, ast.Lt, ast.LtE, ast.Gt,
        ast.GtE, ast.Eq, ast.NotEq, ast.IfExp,
    )

    def __init__(self, namespace: Dict[str, int], allowed_names: List[str]) -> None:
        self.namespace = namespace
        self.allowed_names = set(allowed_names)

    def evaluate(self, expression: str) -> int:
        try:
            tree = ast.parse(expression, mode="eval")
        except SyntaxError as exc:
            raise ValueError("cannot parse %r: %s" % (expression, exc))
        for node in ast.walk(tree):
            if not isinstance(node, self.ALLOWED_NODES):
                raise ValueError(
                    "expression %r uses disallowed syntax %s"
                    % (expression, type(node).__name__)
                )
            if isinstance(node, ast.Name) and node.id not in self.allowed_names:
                raise ValueError(
                    "expression %r uses unknown name %r" % (expression, node.id)
                )
            if isinstance(node, ast.Call):
                if not isinstance(node.func, ast.Name) or node.func.id != "clog2":
                    raise ValueError(
                        "expression %r calls %r; only clog2 is available"
                        % (expression, getattr(node.func, "id", "<expr>"))
                    )
                if node.keywords:
                    raise ValueError("expression %r passes a keyword argument" % expression)
        value = eval(compile(tree, "<contract>", "eval"), {"__builtins__": {}}, dict(self.namespace))
        if isinstance(value, bool) or not isinstance(value, int):
            raise ValueError("expression %r did not evaluate to an integer" % expression)
        return value


def build_namespace(bundle: config_check.Bundle) -> Dict[str, int]:
    geometry = bundle.geometry or {}
    frontend = geometry.get("frontend", {})
    prf = geometry.get("int_prf", {})
    fabric = geometry.get("fabric", {})
    lsu = geometry.get("lsu", {})
    rob = geometry.get("rob", {})
    isa = bundle.profile["isa_target"] if bundle.profile else {}

    vector = geometry.get("vector")
    if vector:
        elems_per_reg = max(1, (vector["vlen"] // vector["elen"]))
    else:
        elems_per_reg = max(1, (isa.get("vlen") or 128) // (isa.get("elen") or 64))

    return {
        "xlen": bundle.profile["xlen"] if bundle.profile else 64,
        "harts": bundle.profile["harts"] if bundle.profile else 1,
        "clusters": geometry.get("fabric", {}).get("clusters", 1),
        "rob_entries": rob.get("entries", 64),
        "max_uops_per_macro": rob.get("max_uops_per_macro", 8),
        "int_prf_entries": prf.get("entries", 96),
        "prf_banks": prf.get("banks", 4),
        "iq_entries_per_cluster": fabric.get("iq_entries_per_cluster", 8),
        "lq_entries": lsu.get("lq_entries", 8),
        "sq_entries": lsu.get("sq_entries", 8),
        "fetch_outstanding": frontend.get("fetch_outstanding", 4),
        "result_fifo_per_cluster": fabric.get("result_fifo_per_cluster", 2),
        "mshrs": (geometry.get("caches") or {}).get("mshrs", 2),
        "lsu_units": lsu.get("units", 1),
        "mul_div_units": fabric.get("mul_div_units", 1),
        "fetch_bandwidth": 2,
        "elems_per_reg": elems_per_reg,
    }


def check_profile(profile: str, config_root: Optional[str] = None) -> List[Problem]:
    problems: List[Problem] = []
    bundle = config_check.load(profile, config_root=config_root)
    if not bundle.ok:
        problems.extend(Problem("config", str(p)) for p in bundle.problems)
        return problems

    contract_root = os.path.join(config_root or config_check.CONFIG_ROOT, "contracts")
    contracts = load_contract("interfaces.json", contract_root)
    counters_doc = load_contract("counters.json", contract_root)
    if contracts is None:
        return [Problem("contracts", "config/contracts/interfaces.json is missing")]
    if counters_doc is None:
        return [Problem("contracts", "config/contracts/counters.json is missing")]

    namespace = build_namespace(bundle)
    allowed = contracts.get("expression_namespace", [])
    evaluator = ExpressionEvaluator(namespace, allowed)

    # ---- counters -------------------------------------------------------
    counter_exprs = set()
    for index, counter in enumerate(counters_doc.get("counters", [])):
        where = "counter %s" % counter.get("name", index)
        for key in ("expression", "max_live_expr", "max_compare_distance_expr"):
            if key not in counter:
                problems.append(Problem(where, "missing %s" % key))
        try:
            modulus = evaluator.evaluate(counter["expression"])
            max_live = evaluator.evaluate(counter["max_live_expr"])
            max_distance = evaluator.evaluate(counter["max_compare_distance_expr"])
        except (KeyError, ValueError) as exc:
            problems.append(Problem(where, str(exc)))
            continue
        counter_exprs.add(counter["expression"])

        if modulus <= max_live:
            problems.append(
                Problem(where, "modulus %d does not exceed the %d values that can be live"
                        % (modulus, max_live))
            )
        if modulus <= 2 * max_distance:
            problems.append(
                Problem(where,
                        "modulus %d does not strictly exceed twice the maximum compare "
                        "distance %d; a wrapped counter would alias with a live one"
                        % (modulus, max_distance))
            )
        if not counter.get("purpose"):
            problems.append(Problem(where, "no purpose recorded"))

    # ---- interfaces -----------------------------------------------------
    seen = set()
    for interface in contracts.get("interfaces", []):
        name = interface.get("name", "<unnamed>")
        where = "interface %s" % name
        if name in seen:
            problems.append(Problem(where, "duplicate interface name"))
        seen.add(name)

        for field in ("owner", "transfer", "cancel", "response", "credit_return_path"):
            if not interface.get(field):
                problems.append(Problem(where, "missing %s" % field))

        fields = interface.get("identity_fields", [])
        by_name = {f.get("name"): f for f in fields}
        for field in fields:
            fwhere = "%s field %s" % (where, field.get("name", "?"))
            if not field.get("why"):
                problems.append(Problem(fwhere, "no justification recorded"))
            try:
                width = evaluator.evaluate(field["expr"])
            except (KeyError, ValueError) as exc:
                problems.append(Problem(fwhere, str(exc)))
                continue
            if width < field.get("min_bits", 0):
                problems.append(
                    Problem(fwhere, "expression yields %d bits but the field needs at least %d"
                            % (width, field["min_bits"]))
                )
            if width < 1:
                problems.append(Problem(fwhere, "expression yields a non-positive width"))

        for index_name, gen_name in GENERATION_REQUIREMENTS.items():
            if index_name in by_name and gen_name not in by_name:
                problems.append(
                    Problem(where,
                            "carries %s without %s; a wrapping index alone cannot identify "
                            "in-flight work" % (index_name, gen_name))
                )

        resources = interface.get("resources", [])
        if not resources:
            problems.append(Problem(where, "declares no resource, so nothing has a "
                                           "unique allocate/release event"))
        for resource in resources:
            rwhere = "%s resource %s" % (where, resource.get("name", "?"))
            if not resource.get("allocate"):
                problems.append(Problem(rwhere, "no allocate event"))
            if not resource.get("release"):
                problems.append(Problem(rwhere, "no release event"))
            if "count_expr" in resource:
                try:
                    count = evaluator.evaluate(resource["count_expr"])
                except (KeyError, ValueError) as exc:
                    problems.append(Problem(rwhere, str(exc)))
                    continue
                if count < 1:
                    problems.append(Problem(rwhere, "count_expr evaluates to %d" % count))

        if not interface.get("aba_counterexamples"):
            problems.append(
                Problem(where, "no ABA counterexample; an interface with no worked "
                               "aliasing failure has not been thought about")
            )

    return problems


class NegativeControls(object):
    def __init__(self, profile: str, config_root: str) -> None:
        self.profile = profile
        self.config_root = config_root
        self.failures: List[str] = []
        self.total = 0

    def case(self, name: str, mutate) -> None:
        self.total += 1
        workdir = tempfile.mkdtemp(prefix="mosaic-contract-negative-")
        try:
            sandbox = os.path.join(workdir, "config")
            shutil.copytree(self.config_root, sandbox)
            saved = CONTRACT_ROOT
            try:
                mutate(sandbox)
                problems = _check_with_root(self.profile, sandbox)
            finally:
                del saved
            if not problems:
                self.failures.append(name)
                print("  NOT REJECTED : %s" % name, file=sys.stderr)
            else:
                print("  rejected: %-48s %s" % (name, problems[0]))
        finally:
            shutil.rmtree(workdir, ignore_errors=True)

    def run(self) -> int:
        # A negative control that trips on an unrelated pre-existing problem is a
        # vacuous pass. Require the real configuration to be clean first.
        baseline = check_profile(self.profile)
        if baseline:
            print(
                "negative controls skipped: profile %s does not check out (%d problem(s)); "
                "fix the configuration before using these controls as evidence"
                % (self.profile, len(baseline)),
                file=sys.stderr,
            )
            return 1

        def edit_contract(sandbox: str, edit) -> None:
            path = os.path.join(sandbox, "contracts", "interfaces.json")
            with open(path) as handle:
                doc = json.load(handle)
            edit(doc)
            with open(path, "w") as handle:
                json.dump(doc, handle, indent=2)

        def edit_counters(sandbox: str, edit) -> None:
            path = os.path.join(sandbox, "contracts", "counters.json")
            with open(path) as handle:
                doc = json.load(handle)
            edit(doc)
            with open(path, "w") as handle:
                json.dump(doc, handle, indent=2)

        def drop_field(name: str):
            def mutate(sandbox: str) -> None:
                def edit(doc):
                    for interface in doc["interfaces"]:
                        interface["identity_fields"] = [
                            f for f in interface["identity_fields"] if f["name"] != name
                        ]
                edit_contract(sandbox, edit)
            return mutate

        self.case("rob_index without rob_gen", drop_field("rob_gen"))
        self.case("prf_tag without prf_gen", drop_field("prf_gen"))
        self.case("tx_id without tx_gen", drop_field("tx_gen"))
        self.case("req_id without epoch", drop_field("epoch"))
        self.case("empty cancel rule",
                  lambda s: edit_contract(s, lambda d: d["interfaces"][0].update({"cancel": ""})))
        self.case("resource without a release event",
                  lambda s: edit_contract(
                      s, lambda d: d["interfaces"][0]["resources"][0].pop("release")))
        self.case("interface with no resources",
                  lambda s: edit_contract(s, lambda d: d["interfaces"][0].update({"resources": []})))
        self.case("interface with no ABA counterexample",
                  lambda s: edit_contract(
                      s, lambda d: d["interfaces"][0].update({"aba_counterexamples": []})))
        self.case("field with no justification",
                  lambda s: edit_contract(
                      s, lambda d: d["interfaces"][0]["identity_fields"][0].update({"why": ""})))
        self.case("counter modulus equal to twice the compare distance",
                  lambda s: edit_counters(
                      s, lambda d: d["counters"][0].update(
                          {"expression": "2*%s" % d["counters"][0]["max_compare_distance_expr"]})))
        self.case("counter modulus below the live count",
                  lambda s: edit_counters(
                      s, lambda d: d["counters"][0].update({"expression": "rob_entries-1"})))
        self.case("field narrower than its justification claims",
                  lambda s: edit_contract(
                      s, lambda d: d["interfaces"][0]["identity_fields"][0].update(
                          {"expr": "1", "min_bits": 32})))
        self.case("expression referencing an unknown name",
                  lambda s: edit_contract(
                      s, lambda d: d["interfaces"][0]["identity_fields"][0].update(
                          {"expr": "quantum_of_the_future"})))
        self.case("expression calling something other than clog2",
                  lambda s: edit_contract(
                      s, lambda d: d["interfaces"][0]["identity_fields"][0].update(
                          {"expr": "len('rtl')"})))
        self.case("expression reaching for a builtin",
                  lambda s: edit_contract(
                      s, lambda d: d["interfaces"][0]["identity_fields"][0].update(
                          {"expr": "__import__('os').getcwd() and 8"})))

        if self.failures:
            print("negative controls: %d of %d wrongly accepted: %s"
                  % (len(self.failures), self.total, ", ".join(self.failures)), file=sys.stderr)
            return 1
        print("negative controls: %d/%d illegal contracts rejected" % (self.total, self.total))
        return 0


def _check_with_root(profile: str, config_root: str) -> List[Problem]:
    """check_profile, reading contracts from a sandboxed config root."""
    return check_profile(profile, config_root=config_root)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", default="p0")
    parser.add_argument("--all", action="store_true")
    parser.add_argument("--negative", action="store_true")
    args = parser.parse_args()

    status = 0
    for profile in (config_check.PROFILE_ORDER if args.all else [args.profile]):
        problems = check_profile(profile)
        if problems:
            for problem in problems:
                print("ERROR %s" % problem, file=sys.stderr)
            print("profile %s: %d contract problem(s)" % (profile, len(problems)), file=sys.stderr)
            status = 1
        else:
            print("profile %s: contracts OK" % profile)

    if args.negative:
        profile = args.profile if not args.all else config_check.PROFILE_ORDER[0]
        status |= NegativeControls(profile, config_check.CONFIG_ROOT).run()

    return status


if __name__ == "__main__":
    sys.exit(main())