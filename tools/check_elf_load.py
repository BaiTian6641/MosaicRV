#!/usr/bin/env python3
"""Independently re-read an ELF and compare it with the loader's manifest.

Work package V-011. The C++ case (sim/unit/tb_loader.cpp) checks the loader
against the description its own builder produced. That is necessary but not
sufficient: if the builder and the loader shared a wrong idea of the ELF layout,
both would agree and both would be wrong. This tool is the second reader.

It parses the file from scratch with `struct`, reads the program headers out of
the raw bytes, and compares, for every PT_LOAD segment:

  * the segment's virtual address, file size, memory size and flags;
  * the segment's file bytes, byte for byte;
  * the image's entry point.

against `image.txt`, which the testbench writes from what LoadElf returned.
A disagreement is printed with the field name and exits 1. It deliberately does
not import anything from the project, so a bug in the project cannot hide here.

Usage: check_elf_load.py <elf> <manifest> [--allow-reject]
"""

from __future__ import annotations

import argparse
import struct
import sys

ELF_MAGIC = b"\x7fELF"
ET_EXEC = 2
EM_RISCV = 243
PT_LOAD = 1


def parse_elf(path: str) -> dict:
    with open(path, "rb") as handle:
        raw = handle.read()
    if len(raw) < 64 or raw[:4] != ELF_MAGIC:
        raise ValueError("not an ELF file")
    if raw[4] != 2:
        raise ValueError("not ELFCLASS64")
    if raw[5] != 1:
        raise ValueError("not little-endian")
    (e_type, e_machine, e_version) = struct.unpack_from("<HHI", raw, 16)
    (e_entry,) = struct.unpack_from("<Q", raw, 24)
    (e_phoff,) = struct.unpack_from("<Q", raw, 32)
    (e_phentsize, e_phnum) = struct.unpack_from("<HH", raw, 54)

    segments = []
    for index in range(e_phnum):
        base = e_phoff + index * e_phentsize
        if base + 56 > len(raw):
            raise ValueError("program header %d runs past the end of the file" % index)
        (p_type, p_flags) = struct.unpack_from("<II", raw, base)
        (p_offset,) = struct.unpack_from("<Q", raw, base + 8)
        (p_vaddr,) = struct.unpack_from("<Q", raw, base + 16)
        (p_filesz,) = struct.unpack_from("<Q", raw, base + 32)
        (p_memsz,) = struct.unpack_from("<Q", raw, base + 40)
        if p_type != PT_LOAD:
            continue
        if p_offset + p_filesz > len(raw):
            raise ValueError("PT_LOAD %d runs past the end of the file" % index)
        segments.append(
            {
                "vaddr": p_vaddr,
                "filesz": p_filesz,
                "memsz": p_memsz,
                "flags": p_flags,
                "data": raw[p_offset : p_offset + p_filesz],
            }
        )
    segments.sort(key=lambda segment: segment["vaddr"])
    return {
        "type": e_type,
        "machine": e_machine,
        "version": e_version,
        "entry": e_entry,
        "segments": segments,
    }


def parse_manifest(path: str) -> dict:
    entry = None
    segments = []
    with open(path) as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            parts = line.split()
            if parts[0].startswith("#"):
                continue
            if parts[0] == "entry":
                entry = int(parts[1], 16)
            elif parts[0] == "seg":
                segments.append(
                    {
                        "vaddr": int(parts[1], 16),
                        "filesz": int(parts[2], 10),
                        "memsz": int(parts[3], 10),
                        "flags": int(parts[4], 10),
                        "data": bytes.fromhex(parts[5]) if len(parts) > 5 else b"",
                    }
                )
    if entry is None:
        raise ValueError("manifest has no entry line")
    segments.sort(key=lambda segment: segment["vaddr"])
    return {"entry": entry, "segments": segments}


def compare(elf: dict, manifest: dict) -> list:
    problems = []
    if elf["type"] != ET_EXEC:
        problems.append("e_type is %d, not ET_EXEC" % elf["type"])
    if elf["machine"] != EM_RISCV:
        problems.append("e_machine is %d, not EM_RISCV" % elf["machine"])
    if elf["entry"] != manifest["entry"]:
        problems.append(
            "entry: file says 0x%x, loader said 0x%x" % (elf["entry"], manifest["entry"])
        )
    if len(elf["segments"]) != len(manifest["segments"]):
        problems.append(
            "segment count: file has %d PT_LOAD, loader reported %d"
            % (len(elf["segments"]), len(manifest["segments"]))
        )
    for index, (want, got) in enumerate(zip(elf["segments"], manifest["segments"])):
        for field in ("vaddr", "filesz", "memsz", "flags"):
            if want[field] != got[field]:
                problems.append(
                    "segment %d %s: file says 0x%x, loader said 0x%x"
                    % (index, field, want[field], got[field])
                )
        if want["data"] != got["data"]:
            first = next(
                (
                    i
                    for i, (a, b) in enumerate(zip(want["data"], got["data"]))
                    if a != b
                ),
                min(len(want["data"]), len(got["data"])),
            )
            problems.append(
                "segment %d bytes: file len %d, loader len %d, first difference at %d"
                % (index, len(want["data"]), len(got["data"]), first)
            )
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf")
    parser.add_argument("manifest")
    args = parser.parse_args()

    try:
        elf = parse_elf(args.elf)
        manifest = parse_manifest(args.manifest)
    except (OSError, ValueError) as error:
        print("CHECKER FAIL: %s" % error)
        return 1

    problems = compare(elf, manifest)
    if problems:
        for problem in problems:
            print("CHECKER FAIL: %s" % problem)
        return 1

    print(
        "CHECKER PASS: %d PT_LOAD segment(s), entry 0x%x, %d payload bytes, "
        "byte-identical to an independent parse"
        % (
            len(elf["segments"]),
            elf["entry"],
            sum(len(segment["data"]) for segment in elf["segments"]),
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
