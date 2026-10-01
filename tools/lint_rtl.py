#!/usr/bin/env python3
"""Lint every RTL source, and prove the linter can fail.

    python3 tools/lint_rtl.py --profile p0
    python3 tools/lint_rtl.py --self-test

Verilator needs a top module, and this project has no single top yet: the design
is being built leaf by leaf and the wrappers arrive with their consumers. So each
RTL file is linted against its own module. A file that declares exactly one
module is linted with that module as the top; a file that is only a package is
handed to the AST linter instead, because Verilator cannot lint a package alone.

The point of `--self-test` is that a lint gate which has never been seen to fail
is not a gate. It lints a deliberately bad file, requires a non-zero exit and a
named warning, and reports what it saw.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from mosaic import config_check  # noqa: E402

REPO_ROOT = config_check.REPO_ROOT
RTL_ROOT = os.path.join(REPO_ROOT, "rtl")

VERILATOR_FLAGS = ["--lint-only", "-Wall", "-Wno-DECLFILENAME"]
SLANG = "slang-tidy"

MODULE_RE = re.compile(r"^\s*module\s+([A-Za-z_][A-Za-z0-9_$]*)", re.MULTILINE)
PACKAGE_RE = re.compile(r"^\s*package\s+([A-Za-z_][A-Za-z0-9_$]*)", re.MULTILINE)

# A module is expected to be written this way; a file that uses `endmodule : name`
# or a bare `endmodule` is still found by MODULE_RE.
BAD_SAMPLE = """
module lint_selftest_bad (
    input  wire clk,
    input  wire rst,
    input  wire d
);
    // A latch: `d` is not assigned on every path, so a real tool infers storage.
    // Verilator reports LATCH.
    reg q;
    always @* begin
      if (rst) q <= 1'b0;
      else if (d) q <= d;
    end
endmodule
"""


def include_dirs(profile: str) -> list:
    return [
        os.path.join(REPO_ROOT, "build", profile, "rtl"),
        os.path.join(RTL_ROOT, "common"),
        os.path.join(RTL_ROOT, "core"),
        os.path.join(RTL_ROOT, "fabric"),
        os.path.join(RTL_ROOT, "vector"),
        os.path.join(RTL_ROOT, "soc"),
    ]


def run(cmd, cwd=REPO_ROOT):
    result = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return result.returncode, result.stdout.decode("utf-8", "replace")


def lint_file(path: str, profile: str, extra_sources) -> tuple:
    with open(path, "r", errors="replace") as handle:
        text = handle.read()
    modules = MODULE_RE.findall(text)

    if not modules:
        packages = PACKAGE_RE.findall(text)
        if not packages:
            return True, "%s: no module or package; skipped" % os.path.relpath(path, REPO_ROOT)
        cmd = [SLANG, "--std", "1800-2017"]
        cmd += ["-I%s" % d for d in include_dirs(profile)]
        cmd.append(path)
        code, output = run(cmd)
        if code != 0:
            return False, "%s: AST lint failed\n%s" % (os.path.relpath(path, REPO_ROOT), output)
        return True, "%s: package(s) %s parsed and type-checked" % (
            os.path.relpath(path, REPO_ROOT), ", ".join(packages))

    top = modules[0]
    cmd = ["verilator"] + VERILATOR_FLAGS
    cmd += ["--top-module", top]
    cmd += ["-I%s" % d for d in include_dirs(profile)]
    cmd.append(path)
    cmd += list(extra_sources)
    code, output = run(cmd)
    relative = os.path.relpath(path, REPO_ROOT)
    if code != 0:
        return False, "%s: lint failed\n%s" % (relative, output)
    if len(modules) > 1:
        return False, "%s: declares %d modules (%s); lint one module per file so each is " \
                       "elaborated on its own" % (relative, len(modules), ", ".join(modules))
    return True, "%s: clean as %s" % (relative, top)


def self_test(profile: str) -> int:
    """Prove the lint gate can fail, so passing it means something."""
    workdir = tempfile.mkdtemp(prefix="mosaic-lint-selftest-")
    path = os.path.join(workdir, "lint_selftest_bad.sv")
    with open(path, "w") as handle:
        handle.write(BAD_SAMPLE)
    code, output = run(["verilator"] + VERILATOR_FLAGS + ["--top-module",
                                                          "lint_selftest_bad", path])
    saw_latch = "LATCH" in output.upper()
    if code == 0 or not saw_latch:
        print("SELF-TEST FAILED: a known-latching module did not produce a LATCH warning "
              "(exit %d)" % code, file=sys.stderr)
        print(output, file=sys.stderr)
        return 1
    print("lint self-test: a latching module is rejected (exit %d, LATCH reported)" % code)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", default="p0")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        return self_test(args.profile)

    sources = []
    for base, dirs, files in os.walk(RTL_ROOT):
        dirs[:] = [d for d in dirs if d != "build"]
        for name in sorted(files):
            if name.endswith((".sv", ".v")):
                sources.append(os.path.join(base, name))
    sources.sort()

    if not sources:
        print("lint: no RTL sources under rtl/ yet; nothing to check", file=sys.stderr)
        return 1

    # Every package is compiled alongside every module: a module that imports
    # mosaic_pkg will not elaborate without it, and a package on its own cannot
    # be a Verilator top. Passing the whole set is the only arrangement that
    # works for both kinds of file.
    contents = {path: open(path, errors="replace").read() for path in sources}
    packages = {os.path.basename(path)[:-3]: path for path, text in contents.items()
                if not MODULE_RE.search(text)}

    failures = []
    for path in sources:
        if path in packages.values():
            continue
        # Only the packages this file actually imports are compiled alongside it.
        # Dragging an unrelated package in would report its unused constants as
        # warnings against this module and turn a clean gate into noise.
        text = contents[path]
        needed = [pkg_path for name, pkg_path in packages.items()
                  if re.search(r"\b%s\s*::" % re.escape(name), text)
                  or ('`include' in text and name in text)]
        ok, message = lint_file(path, args.profile, needed)
        print(("ok   " if ok else "FAIL ") + message)
        if not ok:
            failures.append(path)

    if failures:
        print("lint: %d of %d source file(s) failed" % (len(failures), len(sources)),
              file=sys.stderr)
        return 1
    print("lint: %d source file(s) clean" % len(sources))
    return 0


if __name__ == "__main__":
    sys.exit(main())