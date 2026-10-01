#!/usr/bin/env python3
"""Cross-check the MosaicRV p0 firmware corpus against Spike.

    python3 tests/programs/audit/spike_crosscheck.py \
        --spike $HOME/mosaic-ref/install/bin/spike

This is a THIRD opinion, not a second one.  The host oracle
(tools/host_oracle.py) computes the expected signature from the declared
inputs in pure Python.  Spike executes the actual ELF and reports the
architectural memory writes it performed.  Comparing the two closes the loop
between "what the firmware should produce" and "what the firmware really
produces", without either side being derived from the other:

  * Spike reads the RTL-free ISA semantics, not this repository's code.
  * The oracle reads corpus.json, not this repository's code.

Spike is configured here to present the frozen p0 memory map:

  -m0x00100000:0x4000   covers the uart (0x00100000) and the test_harness
                       device (0x00102000), so TOHOST/FROMHOST accesses are
                       ordinary memory as the protocol requires.
  -m0x80000000:0x200000 the 2 MiB of RAM.
  boot_rom at 0x0 is deliberately NOT mapped, so a store there raises an
  access fault exactly as config/memory/p0.json requires of a read-only
  region.

Exit status 0 only when Spike's signature words equal the oracle's for every
(program, input) pair and Spike exits 0 (tohost == 1, PASS) for all of them.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
CORPUS = os.path.join(REPO, "tests", "programs", "corpus.json")
BUILD = os.path.join(REPO, "tests", "programs", "build")

MEMORY = "0x00100000:0x4000,0x80000000:0x200000"
COMMIT_RE = re.compile(
    r"^core\s+0:\s+3\s+0x([0-9a-f]+)\s+\(0x[0-9a-f]+\)\s+mem\s+"
    r"0x([0-9a-f]+)\s+0x([0-9a-f]+)\s*$")


def load_corpus() -> dict:
    with open(CORPUS) as handle:
        return json.load(handle)


def oracle_signature(document: dict, program: dict, inputs: dict) -> list:
    sys.path.insert(0, os.path.join(REPO, "tools"))
    import host_oracle  # noqa: E402  (path set above; not a firmware import)
    signature, _traps = host_oracle.evaluate(program, inputs)
    return signature


def run_spike(spike: str, elf: str, timeout: int) -> tuple:
    cmd = [spike, "--isa=rv64im_zicsr_zifencei", "-m" + MEMORY,
           "--log-commits", elf]
    proc = subprocess.run(cmd, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=timeout)
    return proc.returncode, proc.stdout.decode("utf-8", "replace")


def signature_from_trace(trace: str, base: int, words: int) -> list:
    """Recover the signature words from the last full-width store to each.

    The signature area is written with 8-byte stores, so the last
    `mem <base + 8*i> 0x<16 hex>` line for each slot is its final value.
    """
    values = {}
    for line in trace.splitlines():
        match = COMMIT_RE.match(line)
        if match is None:
            continue
        address = int(match.group(2), 16)
        value = int(match.group(3), 16)
        if base <= address < base + 8 * words and (address - base) % 8 == 0:
            values[(address - base) // 8] = value
    return [values.get(index) for index in range(words)]


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--spike",
                        default=os.path.join(os.path.expanduser("~"),
                                             "mosaic-ref", "install", "bin",
                                             "spike"))
    parser.add_argument("--corpus", default=CORPUS)
    parser.add_argument("--build", default=BUILD)
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--program", default=None)
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)

    if not os.path.exists(args.spike):
        sys.stderr.write("spike not found at %s\n" % args.spike)
        return 2

    with open(args.corpus) as handle:
        document = json.load(handle)
    protocol = document["protocol"]
    base = protocol["signature"]
    words = protocol["signature_words"]

    failures = []
    checked = 0
    for program in document["programs"]:
        if args.program and program["name"] != args.program:
            continue
        for index, inputs in enumerate(program["inputs"]):
            elf = os.path.join(args.build, "%s.i%d.elf" % (program["name"], index))
            if not os.path.exists(elf):
                failures.append("%s: missing" % elf)
                continue
            code, trace = run_spike(args.spike, elf, args.timeout)
            got = signature_from_trace(trace, base, words)
            want = oracle_signature(document, program, inputs)
            label = "%s.i%d" % (program["name"], index)
            if code != 0:
                failures.append("%s: spike exit %d (tohost was not 1)" % (label, code))
            if any(word is None for word in got):
                failures.append("%s: spike never stored the whole signature" % label)
            elif got != want:
                failures.append("%s: spike %s, oracle %s"
                                % (label,
                                   ["0x%016x" % w for w in got],
                                   ["0x%016x" % w for w in want]))
            else:
                checked += 1
                if args.verbose:
                    print("ok   %-22s exit=%d %s"
                          % (label, code, ["0x%016x" % w for w in got]))

    print("spike cross-check: %d/%d case(s) agree between Spike's "
          "architectural memory writes and the host oracle"
          % (checked, checked + len(failures)))
    if failures:
        sys.stderr.write("\n=== spike cross-check FAILED ===\n")
        for item in failures:
            sys.stderr.write("  %s\n" % item)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))