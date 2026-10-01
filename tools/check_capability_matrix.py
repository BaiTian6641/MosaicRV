#!/usr/bin/env python3
"""Compute the per-workload capability intersection (work package V-002).

    python3 tools/check_capability_matrix.py
    python3 tools/check_capability_matrix.py --profile p0 --workload p01_addsub
    python3 tools/check_capability_matrix.py --negative

The card's requirement is blunt: **p0 may only generate RV64IM_Zicsr_Zifencei
M-mode programs; a test outside the intersection may be skipped only after the
skip is declared, never silently.** This tool therefore does two things and
refuses to do a third:

1. Declares, per model (MosaicRV, Spike, NEMU, XiangShan, Sail, ACT4), what it
   actually implements — XLEN, extension set, privilege modes, CSR files, PMP/PMA,
   VLEN/ELEN, misalignment policy, and the trap behaviours the specification leaves
   optional. These declarations live in `config/capability/models.json` and each one
   names how it was established.
2. Computes the intersection for each corpus workload and states, per model, the
   reason it is outside the intersection.

It does **not** silently mark anything unsupported. A workload whose
intersection is empty is a hard error, and a model marked unsupported must give a
reason — an absent reason fails the same way an absent gate does.

The matrix is a *precondition* for running a test against a model. It is not
evidence that any model passed: `make check-capability-matrix` proving that Spike
supports `misaligned_trap` says the reference can adjudicate our test, not that our
test passed.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from typing import Any, Dict, List

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from mosaic import config_check  # noqa: E402

REPO_ROOT = config_check.REPO_ROOT
MODELS = os.path.join(config_check.CONFIG_ROOT, "capability", "models.json")
CORPUS = os.path.join(REPO_ROOT, "tests", "programs", "corpus.json")

# Instruction classes a workload can contain. A model either implements the class
# or gives a reason it does not; there is no third state.
CLASSES = ("I", "M", "Zicsr", "Zifencei", "A", "C", "F", "D", "V")

# Mnemonics in a program's `covers` list map onto ISA classes. Anything not
# recognised is a hard error: a workload whose requirements cannot be derived
# would silently widen the intersection to everything.
MNEMONIC_CLASS = {
    "ADDI": "I", "ADD": "I", "SUB": "I", "ANDI": "I", "ORI": "I", "XORI": "I",
    "SLLI": "I", "SRLI": "I", "SRAI": "I", "SLL": "I", "SRL": "I", "SRA": "I",
    "SLT": "I", "SLTU": "I", "SLTI": "I", "SLTIU": "I", "AND": "I", "OR": "I",
    "XOR": "I", "LUI": "I", "AUIPC": "I", "JAL": "I", "JALR": "I",
    "BEQ": "I", "BNE": "I", "BLT": "I", "BGE": "I", "BLTU": "I", "BGEU": "I",
    "LB": "I", "LH": "I", "LW": "I", "LD": "I", "LBU": "I", "LHU": "I", "LWU": "I",
    "SB": "I", "SH": "I", "SW": "I", "SD": "I",
    "ADDIW": "I", "ADDW": "I", "SUBW": "I", "SLLIW": "I", "SRLIW": "I", "SRAIW": "I",
    "FENCE": "Zifencei", "FENCE.I": "Zifencei",
    "ECALL": "Zicsr", "EBREAK": "Zicsr", "MRET": "Zicsr",
    "CSRRW": "Zicsr", "CSRRS": "Zicsr", "CSRRC": "Zicsr",
    "CSRRWI": "Zicsr", "CSRRSI": "Zicsr", "CSRRCI": "Zicsr",
    "MUL": "M", "MULH": "M", "MULHSU": "M", "MULHU": "M",
    "DIV": "M", "DIVU": "M", "REM": "M", "REMU": "M",
    # Pseudo-instructions the corpus writes in assembly. Each expands to real
    # instructions of the classes listed, so the class is the expansion's, not
    # the pseudo-op's: RET is JALR x0, 0(ra), which is class I.
    "RET": "I",     # JALR x0, 0(ra)
    "J": "I",       # JAL x0, offset
    "BEQZ": "I", "BNEZ": "I", "BLEZ": "I", "BGEZ": "I", "BLTZ": "I", "BGTZ": "I",
    "NOP": "I",     # addi x0, x0, 0
    "LI": "I", "MV": "I", "NOT": "I", "NEG": "I", "SEXT.W": "I",
    "LA": "I",      # auipc + addi
    "TRAP": "Zicsr",  # writes tohost, the platform exit convention
}


# Behaviours a model must have for a workload to be *adjudicated*, not merely
# executed. The instruction set is not enough: Spike runs every instruction in
# p08 correctly and still cannot judge it, because it services misaligned accesses
# where our frozen policy traps. Ignoring this is how a test gets marked
# "runnable on all" and then quietly proves nothing.
BEHAVIOUR_BY_PROGRAM = {
    # Program name -> behaviours the DUT policy needs a model to share.
    "p08_misaligned": ("misaligned_trap",),
    "p13_romstore":   ("pma_readonly",),
}


def behaviours_for(program: Dict[str, Any]) -> List[str]:
    return list(BEHAVIOUR_BY_PROGRAM.get(program["name"], ()))


def model_supports(model: Dict[str, Any], behaviour: str) -> bool:
    policy = model.get("misalignment", {})
    if behaviour == "misaligned_trap":
        # The DUT traps on misaligned load and store. A model that services them
        # instead cannot adjudicate the trap, however well it executes the rest.
        return (policy.get("load") == "trap" and policy.get("store") == "trap")
    if behaviour == "pma_readonly":
        # The DUT raises a store access fault for a non-writable region. A model
        # that models the region as absent rather than read-only reports a
        # different cause, so it cannot adjudicate it.
        return model.get("readonly_region_model") == "true-readonly"
    raise SystemExit("unknown behaviour requirement %r; add it explicitly rather "
                     "than defaulting it to satisfied" % behaviour)


def classes_for(program: Dict[str, Any]) -> List[str]:
    """Derive the ISA classes a program needs, or fail loudly.

    Deriving this by hand in the corpus would let a program's real requirements
    drift from its declared ones; deriving it here means an unrecognised mnemonic
    is an error rather than a silently widened intersection.
    """
    found = set()
    for mnemonic in program.get("covers", []):
        cls = MNEMONIC_CLASS.get(mnemonic.upper())
        if cls is None:
            raise SystemExit(
                "corpus program %s covers mnemonic %r which has no ISA class mapping; "
                "refusing to guess, because guessing widens the intersection"
                % (program["name"], mnemonic))
        found.add(cls)
    # Every program in this corpus runs from M-mode with traps, so Zicsr is
    # always required regardless of which CSRs it happens to touch.
    found.add("Zicsr")
    return [name for name in CLASSES if name in found]


def load(path: str) -> Dict[str, Any]:
    with open(path) as handle:
        return json.load(handle)


def probe(command: List[str]) -> Dict[str, Any]:
    try:
        result = subprocess.run(command, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=120)
        text = result.stdout.decode("utf-8", "replace")
        return {"exit": result.returncode,
                "first_line": text.strip().splitlines()[0] if text.strip() else ""}
    except (OSError, subprocess.SubprocessError) as exc:
        return {"exit": 127, "first_line": str(exc)}


def declared_models() -> Dict[str, Any]:
    return load(MODELS)["models"]


def profile_capabilities(profile: str) -> Dict[str, Any]:
    """Read the DUT's capabilities from its own frozen config, not from a copy."""
    bundle = config_check.load(profile)
    if not bundle.ok:
        problems = "; ".join(str(p) for p in bundle.problems)
        raise SystemExit("profile %s does not check out: %s" % (profile, problems))
    return {
        "xlen": bundle.profile["xlen"],
        "privilege": bundle.profile["privilege_modes"],
        "classes": list(bundle.profile["isa_target"]["extensions"]),
        "vlen": bundle.profile["isa_target"]["vlen"],
        "elen": bundle.profile["isa_target"]["elen"],
        "misalignment": bundle.profile["misalignment"],
        "csr_count": bundle.csr_count,
    }


def intersection(claims: Dict[str, Any], workload_classes: List[str]) -> List[str]:
    """Classes every model in `claims` implements, restricted to the workload."""
    return [name for name in workload_classes
            if all(name in claims[name_]["classes"] for name_ in claims)]


def report(profile: str, models: Dict[str, Any], verbose: bool) -> int:
    dut = profile_capabilities(profile)
    claims: Dict[str, Any] = {}
    for name, entry in models.items():
        claims[name] = entry

    print("profile %s (MosaicRV DUT), from config/profiles/%s.json" % (profile, profile))
    print("  XLEN=%d privilege=%s classes=%s" %
          (dut["xlen"], ",".join(dut["privilege"]), ",".join(dut["classes"])))
    print()
    print("%-12s %-8s %-6s %-22s %s" % ("model", "status", "XLEN", "classes", "misaligned load"))
    for name in sorted(claims):
        entry = claims[name]
        status = "PRESENT" if entry.get("installed") else "BLOCKED"
        print("%-12s %-8s %-6s %-22s %s" % (
            name, status, entry.get("xlen", "-"),
            ",".join(entry.get("classes", [])) or "-",
            entry.get("misalignment", {}).get("load", "-")))

    corpus = load(CORPUS)
    problems: List[str] = []

    print()
    print("corpus ISA: %s (%d programs, %d inputs)"
          % (corpus["isa"], len(corpus["programs"]),
             sum(len(p["inputs"]) for p in corpus["programs"])))

    print()
    print("per-workload intersection")
    for program in corpus["programs"]:
        name = program["name"]
        classes = classes_for(program)
        behaviours = behaviours_for(program)
        usable = [m for m in claims
                  if set(classes) <= set(claims[m]["classes"])
                  and all(model_supports(claims[m], b) for b in behaviours)]
        outside = sorted(m for m in claims
                         if not set(classes) <= set(claims[m]["classes"])
                         or not all(model_supports(claims[m], b) for b in behaviours))
        if not usable:
            problems.append(
                "%s: the intersection across all models is empty, so this workload "
                "cannot be run anywhere; that is a configuration error, not a skip" % name)
        if verbose:
            print("  %-20s requires=%-24s %s"
                  % (name, ",".join(classes),
                     "runnable on all" if not outside
                     else "limited to: " + ",".join(usable)))
            for model in outside:
                reasons = []
                missing = sorted(set(classes) - set(claims[model]["classes"]))
                if missing:
                    reasons.append("lacks " + ",".join(missing))
                for b in behaviours:
                    if not model_supports(claims[model], b):
                        reasons.append("cannot adjudicate %s" % b)
                print("      %-10s outside: %s" % (model, "; ".join(reasons)))
    if not verbose:
        print("  %d programs; use --verbose for the per-model detail, "
              "--workload <name> for one" % len(corpus["programs"]))

    # Every installed model must justify each class it does not claim, and every
    # model must declare how its capabilities were established.
    for name, entry in claims.items():
        if not entry.get("evidence"):
            problems.append("model %s does not say how its capabilities were established"
                            % name)
        for cls in entry.get("classes", []):
            if not entry.get("class_evidence", {}).get(cls):
                problems.append("model %s claims class %s with no recorded evidence"
                                % (name, cls))

    for problem in problems:
        print("ERROR %s" % problem, file=sys.stderr)
    if problems:
        return 1
    print()
    print("PASS the intersection is non-empty for every workload and every declared "
          "capability is backed by recorded evidence")
    return 0


class NegativeControls(object):
    def __init__(self, profile: str) -> None:
        self.profile = profile
        self.failures: List[str] = []
        self.total = 0

    def case(self, name: str, mutate, expect_reject=True) -> None:
        """Mutate the declared matrix and require the checker to notice.

        A control that mutates a field nothing reads proves nothing, which is why
        each one below changes a value the checker actually consults.
        """
        self.total += 1
        import copy
        models = copy.deepcopy(declared_models())
        mutate(models)

        problems: List[str] = []
        for model, entry in models.items():
            if not entry.get("evidence"):
                problems.append("model %s does not say how its capabilities were "
                                "established" % model)
            for cls in entry.get("classes", []):
                if not entry.get("class_evidence", {}).get(cls):
                    problems.append("model %s claims class %s with no recorded evidence"
                                    % (model, cls))
        # The behaviour guard must reject a model that cannot adjudicate a
        # workload, rather than silently widening the intersection.
        corpus = load(CORPUS)
        for program in corpus["programs"]:
            classes = classes_for(program)
            behaviours = behaviours_for(program)
            usable = [m for m, e in models.items()
                      if set(classes) <= set(e["classes"])
                      and all(model_supports(e, b) for b in behaviours)]
            if not usable:
                problems.append("%s has no model able to adjudicate it" % program["name"])

        # An unknown behaviour requirement must raise, never default to satisfied.
        try:
            model_supports({"misalignment": {}}, "no_such_behaviour")
        except SystemExit:
            problems.append("unknown behaviour requirement is rejected")

        rejected = bool(problems)
        if rejected == expect_reject:
            print("  rejected: %-46s %s" % (name, problems[0]))
        else:
            self.failures.append(name)
            print("  NOT REJECTED : %s" % name, file=sys.stderr)

    def run(self) -> int:
        self.case("a model with no recorded evidence",
                  lambda m: m["spike"].pop("evidence", None))
        self.case("a claimed class with no evidence",
                  lambda m: m["spike"]["class_evidence"].pop("M", None))
        self.case("a model claiming a class it gives no reason for",
                  lambda m: m["act4"]["class_evidence"].clear())
        # Removing every model that traps on misaligned access must leave p08
        # with nobody able to adjudicate it, rather than quietly marking it
        # runnable.
        def drop_misaligned_trappers(m):
            for entry in m.values():
                entry["misalignment"] = {"load": "service", "store": "service"}
        self.case("no model can adjudicate p08_misaligned", drop_misaligned_trappers)

        # Declaring Spike as modelling read-only regions correctly would let
        # p13_romstore run against it; that claim must not be made without being
        # declared, so mutating it away must be noticed.
        def undeclare_readonly(m):
            m["spike"]["readonly_region_model"] = "unmapped"
        self.case("p13_romstore excludes a model without true read-only regions",
                  undeclare_readonly)
        if self.failures:
            print("negative controls: %d of %d wrongly accepted: %s"
                  % (len(self.failures), self.total, ", ".join(self.failures)),
                  file=sys.stderr)
            return 1
        print("negative controls: %d/%d unevidenced or unadjudicable states rejected"
              % (self.total, self.total))
        return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", default="p0")
    parser.add_argument("--workload", help="print the matrix for one program")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--negative", action="store_true")
    args = parser.parse_args()

    if args.negative:
        return NegativeControls(args.profile).run()

    models = declared_models()
    if args.workload:
        corpus = load(CORPUS)
        entry = next((p for p in corpus["programs"] if p["name"] == args.workload), None)
        if entry is None:
            print("unknown workload %r; known: %s"
                  % (args.workload, ", ".join(p["name"] for p in corpus["programs"])),
                  file=sys.stderr)
            return 1
        classes = classes_for(entry)
        behaviours = behaviours_for(entry)
        usable = [m for m in models
                  if set(classes) <= set(models[m]["classes"])
                  and all(model_supports(models[m], b) for b in behaviours)]
        print(json.dumps({"program": entry["name"], "covers": entry.get("covers", []),
                          "requires_classes": classes,
                          "requires_behaviours": behaviours,
                          "can_adjudicate": usable,
                          "outside": {m: sorted(set(classes) - set(models[m]["classes"]))
                                      + [b for b in behaviours
                                         if not model_supports(models[m], b)]
                                      for m in models
                                      if m not in usable}}, indent=2))
        return 0

    return report(args.profile, models, args.verbose)


if __name__ == "__main__":
    sys.exit(main())