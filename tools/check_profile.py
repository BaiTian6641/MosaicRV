#!/usr/bin/env python3
"""Validate the frozen MosaicRV profile configuration.

    python3 tools/check_profile.py --profile p0
    python3 tools/check_profile.py --all
    python3 tools/check_profile.py --profile p0 --negative

Exit status is 0 only when every requested check passes. Any problem is a hard
failure: this is the gate that stops the build from advertising an extension the
hardware does not implement.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from mosaic import config_check  # noqa: E402


def _report(bundle: config_check.Bundle) -> int:
    if not bundle.ok:
        for problem in bundle.problems:
            print("ERROR %s" % problem, file=sys.stderr)
        print("profile %s: %d problem(s)" % (bundle.name, len(bundle.problems)), file=sys.stderr)
        return 1
    advertised, pending = config_check.advertised_capabilities(bundle)
    print("profile %s: configuration OK" % bundle.name)
    print("  claimed      : %s" % ", ".join(bundle.profile["isa_target"]["extensions"]))
    print("  advertised   : %s" % (", ".join(advertised) or "(none yet)"))
    print("  not yet impl : %s" % (", ".join(pending) or "(none)"))
    return 0


def _write_json(path: str, document: object) -> None:
    with open(path, "w") as handle:
        json.dump(document, handle, indent=2, sort_keys=True)
        handle.write("\n")


def _read_json(path: str) -> object:
    with open(path) as handle:
        return json.load(handle)


class NegativeControls(object):
    """Prove the checker actually rejects illegal configurations.

    Each control mutates a throwaway copy of the real config and requires the
    checker to reject it. A control that is wrongly accepted means the checker
    is vacuous, which is worse than having no checker at all.
    """

    def __init__(self, profile: str, config_root: str) -> None:
        self.profile = profile
        self.config_root = config_root
        self.failures = []
        self.total = 0

    def case(self, name: str, mutate) -> None:
        self.total += 1
        workdir = tempfile.mkdtemp(prefix="mosaic-negative-")
        try:
            sandbox = os.path.join(workdir, "config")
            shutil.copytree(self.config_root, sandbox)
            mutate(sandbox)
            bundle = config_check.load(self.profile, config_root=sandbox)
            if bundle.ok:
                self.failures.append(name)
                print("  NOT REJECTED : %s" % name, file=sys.stderr)
            else:
                print("  rejected: %-46s %s" % (name, bundle.problems[0]))
        finally:
            shutil.rmtree(workdir, ignore_errors=True)

    # -- mutators ---------------------------------------------------------
    def _profile_path(self, sandbox: str) -> str:
        return os.path.join(sandbox, "profiles", "%s.json" % self.profile)

    def add_extension(self, ext: str):
        def mutate(sandbox: str) -> None:
            path = self._profile_path(sandbox)
            document = _read_json(path)
            document["isa_target"]["extensions"].append(ext)
            _write_json(path, document)
        return mutate

    def drop_extension(self, ext: str):
        def mutate(sandbox: str) -> None:
            path = self._profile_path(sandbox)
            document = _read_json(path)
            document["isa_target"]["extensions"].remove(ext)
            _write_json(path, document)
        return mutate

    def patch_profile(self, edit):
        def mutate(sandbox: str) -> None:
            path = self._profile_path(sandbox)
            document = _read_json(path)
            edit(document)
            _write_json(path, document)
        return mutate

    def patch_memory(self, edit):
        def mutate(sandbox: str) -> None:
            document = _read_json(self._profile_path(sandbox))
            path = os.path.join(sandbox, document["memory_map_file"])
            memory = _read_json(path)
            edit(memory)
            _write_json(path, memory)
        return mutate

    def patch_first_csr(self, edit):
        def mutate(sandbox: str) -> None:
            document = _read_json(self._profile_path(sandbox))
            path = os.path.join(sandbox, document["csr_files"][0])
            table = _read_json(path)
            edit(table)
            _write_json(path, table)
        return mutate

    def write_invalid_json(self, sandbox: str) -> None:
        with open(self._profile_path(sandbox), "w") as handle:
            handle.write('{"profile": "p0",,, }\n')

    # -- the controls -----------------------------------------------------
    def run(self) -> int:
        ladder = _read_json(os.path.join(self.config_root, "capability_ladder.json"))
        future = sorted({
            cap["name"] for cap in ladder["capabilities"]
            if cap["min_profile"] != self.profile and cap["kind"] != "constraint"
        })

        for ext in future:
            self.case("claims %s one rung too early" % ext, self.add_extension(ext))

        self.case("unknown extension name", self.add_extension("NotAnExtension"))
        self.case(
            "cacheable but non-idempotent PMA",
            self.patch_memory(lambda m: m["regions"][0].update({"cacheable": True, "idempotent": False})),
        )
        self.case(
            "overlapping regions",
            self.patch_memory(lambda m: m["regions"][1].update({"base": m["regions"][0]["base"]})),
        )
        self.case(
            "rom region is writable",
            self.patch_memory(lambda m: m["regions"][0].update({"writable": True})),
        )
        self.case(
            "mmio names an unimplemented device",
            self.patch_memory(lambda m: m["regions"][1].update({"device": "gpu_mailbox"})),
        )
        self.case(
            "atomic granule is not a power of two",
            self.patch_memory(lambda m: m["regions"][1].update({"atomic_granule": 3})),
        )
        self.case(
            "atomic granule exceeds an RISC-V access width",
            self.patch_memory(lambda m: m["regions"][1].update({"atomic_granule": 16})),
        )
        self.case(
            "reset vector outside executable memory",
            self.patch_profile(lambda d: d["reset"].update({"reset_vector": 0x7FFFFFFF})),
        )
        self.case("Zicsr claimed without Zifencei", self.drop_extension("Zifencei"))
        self.case("xlen is not 64", self.patch_profile(lambda d: d.update({"xlen": 32})))
        self.case(
            "VLEN declared without claiming V",
            self.patch_profile(lambda d: d["isa_target"].update({"vlen": 128})),
        )
        self.case(
            "unknown schema field in the profile",
            self.patch_profile(lambda d: d.update({"vlen_bits": 128})),
        )
        self.case("profile file is not valid JSON", self.write_invalid_json)
        self.case(
            "CSR without a spec clause",
            self.patch_first_csr(lambda t: t["modes"][0]["csrs"][0].pop("spec_clause")),
        )
        self.case(
            "CSR with a duplicate address",
            self.patch_first_csr(
                lambda t: t["modes"][0]["csrs"][1].update({"address": t["modes"][0]["csrs"][0]["address"]})
            ),
        )
        self.case(
            "CSR reset value wider than the register",
            self.patch_first_csr(lambda t: t["modes"][0]["csrs"][0].update({"reset": (1 << 64) + 1})),
        )
        self.case(
            "read-only CSR declaring WARL behaviour",
            self.patch_first_csr(lambda t: t["modes"][0]["csrs"][0].update({"access": "ro"})),
        )
        self.case(
            "warl_wpri CSR with no WPRI fields",
            self.patch_first_csr(
                lambda t: t["modes"][0]["csrs"][0].update({"behavior": "warl_wpri", "wpri_fields": []})
            ),
        )
        self.case(
            "overlapping writable and WPRI fields",
            self.patch_first_csr(lambda t: t["modes"][0]["csrs"][0].update({"wpri_fields": ["5:4"]})),
        )
        self.case(
            "privilege mode with no CSR table",
            self.patch_profile(lambda d: d["privilege_modes"].append("U")),
        )

        if self.failures:
            print(
                "negative controls: %d of %d wrongly accepted: %s"
                % (len(self.failures), self.total, ", ".join(self.failures)),
                file=sys.stderr,
            )
            return 1
        print("negative controls: %d/%d illegal configurations rejected"
              % (self.total, self.total))
        return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", help="profile name, e.g. p0")
    parser.add_argument("--all", action="store_true", help="check every profile")
    parser.add_argument(
        "--negative",
        action="store_true",
        help="additionally prove the checker rejects illegal configurations",
    )
    args = parser.parse_args()

    if not args.profile and not args.all:
        parser.error("one of --profile or --all is required")

    status = 0

    if args.negative:
        targets = [args.profile] if args.profile else config_check.PROFILE_ORDER[:1]
        for name in targets:
            print("negative controls for %s:" % name)
            status |= NegativeControls(name, config_check.CONFIG_ROOT).run()
        unknown = config_check.load("p9")
        if unknown.ok:
            print("NOT REJECTED : unknown profile name", file=sys.stderr)
            status |= 1
        else:
            print("  rejected: %-46s %s" % ("unknown profile name", unknown.problems[0]))

    for name in (config_check.PROFILE_ORDER if args.all else [args.profile]):
        status |= _report(config_check.load(name))

    return status


if __name__ == "__main__":
    sys.exit(main())