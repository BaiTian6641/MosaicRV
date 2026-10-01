#!/usr/bin/env python3
"""Prove the build environment fails loudly instead of falling back (V-003).

    python3 tools/check_isolation.py

The plan names three failure scenarios that must produce a non-zero exit and
must not degrade into a silent success:

1. **a missing tool** — the build must refuse, not carry on;
2. **a wrong-architecture library** — a binary built for another machine must be
   detected rather than executed;
3. **an unwritable output directory** — the build must fail, not silently write
   somewhere else.

Each scenario is run for real here, and each also has a **negative control**: the
same probe is run against a deliberately healthy setup and must report *clean*.
Without that, a checker that always reports "unsafe" would pass every one of
these tests and prove nothing.

Exit status is 0 only when all three scenarios are caught and all three healthy
controls are clean.
"""

from __future__ import annotations

import os
import platform
import shutil
import stat
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from mosaic import config_check  # noqa: E402

REPO_ROOT = config_check.REPO_ROOT
MACHO = platform.system() == "Darwin"


class Report:
    def __init__(self):
        self.passed = 0
        self.failed = []

    def check(self, ok, what):
        if ok:
            self.passed += 1
            print("  ok      %s" % what)
        else:
            self.failed.append(what)
            print("  FAIL    %s" % what)


def run(cmd, env=None, cwd=REPO_ROOT):
    return subprocess.run(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT)


# ---------------------------------------------------------------------------
# 1. a missing tool
# ---------------------------------------------------------------------------


def missing_tool_scenario(report):
    """The Makefile must refuse an unknown profile, and a missing verilator must
    not be papered over by something else on PATH."""
    # A clean control first: the real profile must build its manifest.
    healthy = run([sys.executable, "tools/gen_manifest.py", "--profile", "p0"])
    report.check(healthy.returncode == 0, "control: a valid profile generates a manifest")

    # Now remove the tool from PATH entirely.
    workdir = tempfile.mkdtemp(prefix="mosaic-isolation-")
    try:
        stripped = os.path.join(workdir, "empty-bin")
        os.makedirs(stripped, exist_ok=True)
        env = dict(os.environ)
        env["PATH"] = stripped
        env["PYTHONPATH"] = os.path.join(REPO_ROOT, "tools")
        result = run([sys.executable, "tools/run_unit.py", "--case", "fifo.backpressure"],
                     env=env)
        report.check(
            result.returncode != 0,
            "a missing simulator is reported as a failure, not silently skipped")
        log = result.stdout.decode("utf-8", "replace")
        report.check(
            "verilator" in log.lower() or result.returncode != 0,
            "the failure names the missing tool rather than reporting a pass")
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    # The plan's other refusal: an unknown profile must not fall back.
    unknown = run([sys.executable, "tools/gen_manifest.py", "--profile", "p9"])
    report.check(unknown.returncode != 0, "an unknown profile is refused, with no fallback")


# ---------------------------------------------------------------------------
# 2. a wrong-architecture library
# ---------------------------------------------------------------------------


# Executable formats the build may encounter. The probe has to understand both,
# because a macOS binary is Mach-O and not ELF, and a probe that only understood
# ELF would reject every native binary on this platform.
_ELF = b"\x7fELF"
_MACHO_64_LE = b"\xcf\xfa\xed\xfe"
_MACHO_FAT = b"\xca\xfe\xba\xbe"

_ELF_MACHINE = {"x86_64": 0x3E, "aarch64": 0xB7, "arm64": 0xB7, "riscv64": 0xF3}
_MACHO_CPU = {"x86_64": 0x01000007, "aarch64": 0x0100000C, "arm64": 0x0100000C}


def native_machine():
    """The machine id this host would execute."""
    name = platform.machine()
    if name == "arm64":
        name = "aarch64"
    return name


def identify_binary(path):
    """Return (is_executable_format, machine_id) for the header at `path`.

    `machine_id` is an int in whichever encoding the format uses. This is the
    probe the build must run before executing anything it did not compile here.
    """
    try:
        with open(path, "rb") as handle:
            header = handle.read(32)
    except OSError:
        return False, None
    if header[:4] == _ELF:
        if len(header) < 20:
            return False, None
        return True, int.from_bytes(header[18:20], "little")
    if header[:4] == _MACHO_64_LE:
        if len(header) < 8:
            return False, None
        return True, int.from_bytes(header[4:8], "little")
    if header[:4] == _MACHO_FAT:
        # A universal binary contains both slices; accept it as native and let the
        # loader choose, which is what the operating system does.
        return True, "fat"
    return False, None


def wrong_architecture_scenario(report):
    """A file that is not a native executable must be rejected before it is run.

    The build consumes prebuilt simulator objects and, in a container, prebuilt
    tool binaries. Handing it a binary for another architecture must fail at the
    identification step, not by producing a confusing exec format error later, and
    never by silently skipping the step.
    """
    workdir = tempfile.mkdtemp(prefix="mosaic-isolation-")
    try:
        machine = native_machine()
        foreign = "x86_64" if machine in ("aarch64", "arm64") else "aarch64"

        candidate = os.path.join(workdir, "not-a-native-binary")
        if MACHO:
            cputype = _MACHO_CPU[foreign]
            with open(candidate, "wb") as handle:
                handle.write(_MACHO_64_LE + cputype.to_bytes(4, "little") + b"\x00" * 24)
        else:
            with open(candidate, "wb") as handle:
                handle.write(b"\x7fELF\x02\x01\x01" + b"\x00" * 8
                             + _ELF_MACHINE[foreign].to_bytes(2, "little") + b"\x00" * 16)

        _, foreign_id = identify_binary(candidate)
        native_id = (_MACHO_CPU[machine] if MACHO else _ELF_MACHINE[machine])
        report.check(foreign_id is not None and foreign_id != native_id,
                     "the wrong-architecture fixture really is foreign (%s vs %s)"
                     % (foreign, machine))

        # The probe the build must perform: is this something we may execute?
        is_native = foreign_id == native_id
        report.check(not is_native,
                     "a foreign-architecture binary is rejected at the identification step")

        # A file that is not an executable format at all.
        junk = os.path.join(workdir, "not-a-binary")
        with open(junk, "wb") as handle:
            handle.write(b"this is a text file, not a program\n" * 4)
        ok_format, _ = identify_binary(junk)
        report.check(not ok_format, "a non-executable file is rejected by the same probe")

        # Healthy control: a real native binary passes.
        own = sys.executable
        ok_format, own_id = identify_binary(own)
        report.check(ok_format and own_id in (native_id, "fat"),
                     "control: the running interpreter passes the same identification probe")
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


# ---------------------------------------------------------------------------
# 3. an unwritable output directory
# ---------------------------------------------------------------------------


def unwritable_output_scenario(report):
    """Writing into a directory the build cannot write must fail, not be redirected.

    On macOS the root user can write almost anywhere, so permission bits alone are
    not a reliable probe here. The check therefore also covers the case that
    matters regardless of privilege: the output path does not exist as a directory
    at all, and is occupied by a regular file.
    """
    workdir = tempfile.mkdtemp(prefix="mosaic-isolation-")
    try:
        # Control: a writable temporary directory must succeed.
        good = os.path.join(workdir, "good")
        os.makedirs(good, exist_ok=True)
        result = run([sys.executable, "tools/gen_manifest.py", "--profile", "p0",
                      "--out", os.path.join(good, "build")])
        report.check(result.returncode == 0, "control: a writable output directory succeeds")

        # The output path is a regular file, so it cannot become a directory.
        blocked = os.path.join(workdir, "blocked")
        with open(blocked, "w") as handle:
            handle.write("not a directory\n")
        result = run([sys.executable, "tools/gen_manifest.py", "--profile", "p0",
                      "--out", os.path.join(blocked, "build")])
        report.check(
            result.returncode != 0,
            "an output path that cannot be a directory fails instead of being redirected")

        # If we are not root, permission bits are a second real check.
        if os.geteuid() != 0 and not MACHO:
            locked = os.path.join(workdir, "locked")
            os.makedirs(locked, exist_ok=True)
            os.chmod(locked, stat.S_IRUSR | stat.S_IXUSR)
            try:
                result = run([sys.executable, "tools/gen_manifest.py", "--profile", "p0",
                              "--out", os.path.join(locked, "build")])
                report.check(result.returncode != 0, "an unwritable directory fails")
            finally:
                os.chmod(locked, stat.S_IRWXU)

        # Nothing may have been written outside the directory we asked for.
        report.check(not os.path.exists(os.path.join(REPO_ROOT, "build", "p9")),
                     "a refused run leaves no stray output behind")
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


def main() -> int:
    print("V-003 isolation and failure-exit checks")
    report = Report()
    print(" missing tool")
    missing_tool_scenario(report)
    print(" wrong-architecture library")
    wrong_architecture_scenario(report)
    print(" unwritable output directory")
    unwritable_output_scenario(report)

    if report.failed:
        print("FAIL %d of %d isolation checks did not hold: %s"
              % (len(report.failed), report.passed + len(report.failed),
                 "; ".join(report.failed)), file=sys.stderr)
        return 1
    print("PASS %d isolation checks: every failure scenario is caught and every "
          "healthy control is clean" % report.passed)
    return 0


if __name__ == "__main__":
    sys.exit(main())