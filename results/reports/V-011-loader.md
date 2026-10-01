# V-011 — ELF loading and memory boundaries

**Status: the case passes against the registered wrapper, with all controls
observed.** `loader.elf_boundaries` prints `PASS` and exits 0 (490 checks, 312 DUT
cycles, 14/14 re-exec controls), six compile-time mutants each exit 1 with the
named check they break, and the independent Python reader agrees with the loaded
image byte for byte. Three real loader defects were found and fixed on the way
(§2); no check was weakened for any of them.

| File | State |
|---|---|
| `sim/common/elf_loader.h` | `Segment::flags` added; `Segment::Contains` made overflow-safe; three statuses appended (`kSegmentOverflow`, `kNotExecutable`, `kEntryNotExecutable`) |
| `sim/common/elf_loader.cpp` | `e_type` check, vaddr/memsz and offset/filesz overflow checks, executable-entry check; six `-DMOSAIC_ELF_MUTANT_*` negative controls |
| `sim/unit/tb_loader.cpp` | the case; builder, memory checks, DUT checks, re-exec controls |
| `tools/check_elf_load.py` | independent ELF reader used as the second anchor for the manifest |
| `tools/run_loader_controls.py` | builds and observes the six mutants from empty directories |
| `results/unit/loader.elf_boundaries/` | case artefacts: `good.elf`, `image.txt`, `controls.txt`, `run.log`, `result.json` |
| `results/reports/V-011-loader.md` | this file |

Nothing under `tests/programs/` was read for its golden signatures and nothing
there was written: the malformed images are generated at run time into the case's
own output directory (`results/unit/loader.elf_boundaries/bad-*.elf`).

---

## 1. What the case drives, and what the expectation is

### 1.1 What it drives

The registered top is `mosaic_bringup_tb`. The case:

1. **Loader, in process.** Writes fourteen ELF images (one accepted, thirteen
   malformed) and asserts the exact `LoadStatus` for each; the one whose fault is
   against the memory map rather than the file is refused by `LoadImage`.
2. **Memory model, in process.** `MemoryModel::LoadImage` of the accepted image,
   then a byte-for-byte comparison of every segment's `memsz` bytes against the
   builder's description; little-endian word and byte loads/stores; the fault
   table at region edges; and the model's access-fault counter as the record of
   which probes were actually treated as faults.
3. **The DUT.** The accepted image is pushed into `mosaic_bringup_tb` through its
   own `h_img_*` port, reset is released, the core is run for 40 cycles, and its
   first retire (PC and instruction word), the readback of every image word, the
   zero gap between the segments and the BSS tails, and nine boundary probes
   through `h_rb_*` are checked. 312 DUT cycles of this are counted.
4. **The refusals, by re-execution.** For each malformed image the case
   `fork()`/`execv()`s *itself* with `--image <path>` and reads the child's exit
   status and its own line. This is the only way "exit 2 before any DUT cycle" is
   an observation rather than a claim.

### 1.2 Where the expectation comes from

* The expected segment list, byte values, BSS sizes and entry point are the
  **description this file hands to its own builder** (`SegSpec`/`ElfSpec`), not
  the loader's output. The checks compare the loader's `Image` against that
  description field by field, and the memory model's contents against it byte by
  byte. A loader that loaded half the image cannot satisfy that comparison even
  though it would satisfy a comparison against itself.
* The ELF layout the builder writes is the System V gABI layout the loader is
  written against; `tools/check_elf_load.py` re-parses the emitted file with
  `struct` and compares it with the manifest the case wrote from `LoadElf`'s
  return value. Two independent readers of the same bytes, one C++ and one
  Python.
* The memory map, TOHOST/FROMHOST, reset vector and signature addresses come from
  the generated `build/p0/sim/mosaic_platform.h`, never from literals in the
  testbench.
* The DUT's boundary behaviour is read back through the DUT's own readback port,
  so it is evidence about what the wrapper's memory model actually does.

### 1.3 What it explicitly does **not** read

* Not the golden signatures under `tests/programs/` (owned by I-007/V-012), and
  not `tests/programs/corpus.json`.
* Not any part of the RTL's internal state: the only DUT observations are the
  public ports (`c_evt_*`, `c_dbg_pc`, `h_rb_*`, `h_tohost_written`) and the
  image-load port contract.
* Not the loader's own output as the expectation for anything (§1.2), and not the
  manifest as the expectation for the DUT: the DUT is compared against the
  builder's bytes, and the manifest is compared against the Python reader.
* No external assembler, linker or `readelf` at run time: the bytes are built by
  this file, and the only external authority consulted is the Python parser.

---

## 2. Defects found in the loader and fixed

Each was found by a check that was already written to fail, not by inspection.

**2.1 A dynamic (ET_DYN) image was accepted.** `e_type` was never read. A PIE
image has a link-time `p_vaddr` and a run-time load address chosen by a loader
this harness does not have, so accepting it loads the image at an address nobody
will execute. Fixed: `e_type` must be `ET_EXEC`; anything else is
`not-executable`. Negative control: `MOSAIC_ELF_MUTANT_ACCEPT_DYNAMIC` (exit 1).

**2.2 A segment whose `vaddr + memsz` wraps the address space was accepted.**
Both bounds are sums of attacker-controlled 64-bit fields. `offset + filesz` was
compared against the file size *after* the addition could wrap below it, so a
segment that runs off the end of the file could look like it fits;
`vaddr + memsz` was never checked, and `Segment::Contains` computed it, so a
segment near `2**64` made a wild entry address look mapped. Fixed: both sums are
checked pre-addition (`kSegmentOverflow`), and `Contains` is written so it cannot
overflow (`address - vaddr < memsz`). Negative controls:
`MOSAIC_ELF_MUTANT_NO_SEGMENT_OVERFLOW_CHECK` (exit 1), and two distinct inputs
(`vaddr-overflow`, `file-overflow`) that reach the two different checks.

**2.3 An entry point in a segment that declares itself non-executable was
accepted.** `p_flags` was never read. Fixed: if the segment holding `e_entry`
declares any permission bits at all and they do not include `PF_X`, the load is
refused with `entry-not-executable`. **Compatibility rule, stated so it can be
argued with:** a segment whose `p_flags` is exactly zero declares nothing, and an
entry there is accepted. That carve-out exists because the I-004 case
(`sim/harness/tb_harness.cpp`) builds such an image and asserts it loads; taking
it at face value keeps that case green without editing another package's
testbench, and it is the image, not the loader, that is silent. If the project
would rather require `PF_X` unconditionally, the carve-out is one condition and
I-004's builder needs one line. Negative control:
`MOSAIC_ELF_MUTANT_ALLOW_NONEXEC_ENTRY` (exit 1).

Also made overflow-safe: `Segment::Contains`. `LoadStatusName` gained the three
names. The three enums were appended after the existing values so any stored
status number keeps its meaning.

---

## 3. Acceptance criteria, mapped to checks

| Card criterion | Check(s) |
|---|---|
| multiple PT_LOAD segments | `loader.accept.multi-segment`, `loader.accept.segment0/1.*` |
| BSS (`memsz - filesz`) is exactly zero | `memory.image.segment0/1.byte*` (offset >= filesz must read 0) |
| non-zero entry, used, not the lowest address | `loader.accept.entry-is-e_entry` (0x80000800, seg 1) |
| declared load addresses vs loaded image, byte for byte | `memory.image.segment*.byte*`, `dut.image.segment*.word*`, plus `tools/check_elf_load.py` |
| nothing spilled between/around segments | `dut.image.gap.*` over the whole 0x80000048..0x80000800 gap |
| bad magic | `loader.reject.magic`, control `magic` → exit 2 |
| wrong endianness | `loader.reject.endian`, control `endian` → exit 2 |
| truncation | `loader.reject.truncated`, control `truncated` → exit 2 |
| no PT_LOAD | `loader.reject.noload`, control `noload` → exit 2 |
| segment alignment | `loader.reject.align`, control `align` → exit 2 |
| `filesz > memsz` | `loader.reject.filesz-gt-memsz`, control `filesz-gt-memsz` → exit 2 |
| overlapping segments | `loader.reject.overlap`, control `overlap` → exit 2 |
| overflowing segment | `loader.reject.vaddr-overflow` / `.file-overflow`, controls `vaddr-overflow` / `file-overflow` → exit 2 |
| dynamic ELF | `loader.reject.dynamic`, control `dynamic` → exit 2 |
| out-of-range address | `memory.reject.out-of-range`, control `out-of-map` → exit 2 (the loader cannot know the map; the harness refuses at `LoadImage`) |
| non-executable entry | `loader.reject.entry-not-executable`, control `entry-not-executable` → exit 2 |
| refusal **before any DUT cycle** | every control line carries `dut_cycles=0`; the accepted image carries `dut_cycles=40` from the same code path |
| load/store byte order | `memory.store-load.size8.round-trip`, `memory.byte-order.byte0..7`, `memory.byte-store.one-byte-only`, `memory.image.segment*.word-byte-order`, `dut.image.segment*.word*` |
| boundary faults | `memory.boundary.*` (10 probes) and `dut.boundary.*` (9 probes + sentinel) |
| a wrong address never wraps RAM | `memory.boundary.ram.one-past-end`, `memory.boundary.no-wrap` (first word unchanged across the probes), `dut.boundary.ram.one-past-end`, `dut.boundary.ram.before-start`, `dut.boundary.no-wrap` (a sentinel at RAM base is still there after every probe) |

---

## 4. Control table (observed)

### 4.1 Command-line controls, observed by re-execution

Every row is one `execv` of the same binary with `--image <file>`; the exit code
and the first line of the child's output are printed by the parent and written to
`results/unit/loader.elf_boundaries/controls.txt`.

| Control | Input | Exit | First line |
|---|---|---|---|
| accepts-good-image | the accepted multi-segment image | **0** | `ACCEPT loader.elf_boundaries entry=0x0000000080000800 dut_cycles=40 dut_ok=1` |
| magic | `bad-magic.elf` | **2** | `REFUSE ... bad-magic: missing \x7fELF magic dut_cycles=0` |
| endian | `bad-endian.elf` | **2** | `REFUSE ... not-little-endian: ELF data encoding is not ELFDATA2LSB dut_cycles=0` |
| truncated | `bad-truncated.elf` | **2** | `REFUSE ... truncated: program header table runs past the end of the file dut_cycles=0` |
| noload | `bad-noload.elf` | **2** | `REFUSE ... no-loadable-segment: image contains no PT_LOAD segment dut_cycles=0` |
| align | `bad-align.elf` | **2** | `REFUSE ... segment-alignment: PT_LOAD vaddr 0x80000002 is not 8-byte aligned dut_cycles=0` |
| filesz-gt-memsz | `bad-filesz-gt-memsz.elf` | **2** | `REFUSE ... bad-program-header: PT_LOAD has p_filesz larger than p_memsz dut_cycles=0` |
| overlap | `bad-overlap.elf` | **2** | `REFUSE ... segment-overlap: PT_LOAD segments at 0x80000000 and 0x80000010 overlap dut_cycles=0` |
| vaddr-overflow | `bad-vaddr-overflow.elf` | **2** | `REFUSE ... segment-overflow: PT_LOAD vaddr 0xfffffffffffff000 + memsz 8192 wraps the address space dut_cycles=0` |
| file-overflow | `bad-file-overflow.elf` | **2** | `REFUSE ... segment-overflow: PT_LOAD p_offset + p_filesz wraps the file offset space dut_cycles=0` |
| dynamic | `bad-dynamic.elf` | **2** | `REFUSE ... not-executable: e_type is 3, expected 2 (ET_EXEC) dut_cycles=0` |
| entry-unmapped | `bad-entry-unmapped.elf` | **2** | `REFUSE ... entry-not-mapped: entry point 0x80100000 is not inside any PT_LOAD segment dut_cycles=0` |
| entry-not-executable | `bad-entry-not-executable.elf` | **2** | `REFUSE ... entry-not-executable: entry point 0x80000000 is in a PT_LOAD with p_flags 0x6, which is not executable dut_cycles=0` |
| out-of-map | `bad-out-of-map.elf` | **2** | `REFUSE ... image-outside-memory-map: image segment at 0x40000000 covers 0x40000000, which is outside the p0 memory map dut_cycles=0` |

The `accepts-good-image` row is what makes the `dut_cycles=0` rows mean something:
the same binary, the same function, drives the clock when the load succeeds.

### 4.2 Mutants of the code under test (compile-time, empty build directory)

`tools/run_loader_controls.py` rebuilds `sim/common/elf_loader.cpp` with one
`-D` define, from a directory wiped first, checks the binary differs from the
shipping one with `cmp`, and runs the case.

| Mutant (`-D…`) | Injected defect | Exit | First failure observed |
|---|---|---|---|
| `MOSAIC_ELF_MUTANT_LOAD_FIRST_SEGMENT_ONLY` | keeps only the first PT_LOAD | 1 | `loader.accept.multi-segment: expected ok, got entry-not-mapped (entry point 0x80000800 is not inside any PT_LOAD segment)` |
| `MOSAIC_ELF_MUTANT_IGNORE_ENTRY` | enters at the lowest address | 1 | `loader.accept.entry-is-e_entry: expected 0x0000000080000800, got 0x0000000080000000` |
| `MOSAIC_ELF_MUTANT_ALLOW_NONEXEC_ENTRY` | accepts a non-executable entry segment | 1 | `loader.reject.entry-not-executable: expected entry-not-executable, got ok` |
| `MOSAIC_ELF_MUTANT_NO_SEGMENT_OVERFLOW_CHECK` | lets a wrapping vaddr+memsz through | 1 | `loader.reject.vaddr-overflow: expected segment-overflow, got ok` |
| `MOSAIC_ELF_MUTANT_SKIP_OVERLAP_CHECK` | accepts overlapping segments | 1 | `loader.reject.overlap: expected segment-overlap, got ok` |
| `MOSAIC_ELF_MUTANT_ACCEPT_DYNAMIC` | accepts ET_DYN at its link-time address | 1 | `loader.reject.dynamic: expected not-executable, got ok` |

All six differ from the shipping binary and all six exit 1. **No redundancy
probes:** every mutant changes the verdict, and no control passes identically to
the baseline.

### 4.3 The independent reader's own control

| Invocation | Exit |
|---|---|
| `check_elf_load.py good.elf image.txt` | 0 — `CHECKER PASS: 2 PT_LOAD segment(s), entry 0x80000800, 40 payload bytes, byte-identical to an independent parse` |
| same, with one payload byte of the manifest flipped | 1 — `CHECKER FAIL: segment 0 bytes: file len 8, loader len 8, first difference at 0` |

---

## 5. Exact commands and observed output

```
$ python3 tools/run_unit.py --case loader.elf_boundaries
PASS loader.elf_boundaries        task=V-011
```
(the case's own line, from `results/unit/loader.elf_boundaries/run.log`:
`RESULT PASS loader.elf_boundaries 490 checks, 312 DUT cycles, 14/14 controls observed`)

```
$ python3 tools/run_loader_controls.py
building the shipping case from an empty directory...
  baseline exit=0 RESULT PASS loader.elf_boundaries 490 checks, 312 DUT cycles, 14/14 controls observed
... (table as §4.2) ...
all 6 mutants mutate the binary, exit 1 and name the check they break
```

```
$ python3 tools/check_elf_load.py results/unit/loader.elf_boundaries/good.elf \
      results/unit/loader.elf_boundaries/image.txt
CHECKER PASS: 2 PT_LOAD segment(s), entry 0x80000800, 40 payload bytes, byte-identical to an independent parse
```

Regressions for the files this package changed (the loader is on their path):

```
$ python3 tools/run_unit.py --case harness.bad_image
PASS harness.bad_image            task=I-004

$ python3 tools/run_unit.py --case core.bringup_vs_reference
PASS core.bringup_vs_reference    task=I-008
```

And a direct check that the strengthened loader still accepts the real corpus
(39 ELFs, all consumed by I-008), built from `elf_loader.cpp` alone:

```
$ /tmp/load_corpus tests/programs/build
corpus: 39 ELF(s), 0 rejected
```

Lint on the changed C++ (the project's `lint-cpp` flags):

```
$ clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Wshadow -Isim/common sim/common/elf_loader.cpp
elf_loader.cpp: clean under -Wall -Wextra -Wshadow
```
`sim/unit/tb_loader.cpp` is likewise clean under those flags; the only warnings it
produces come from Verilator's own headers, which the project's `lint-cpp` also
compiles.

---

## 6. Not verified, and stated limits

* **The first-pass build was against a temporary copy of the wrapper.** While this
  package was being finished, the V-009 lane held
  `sim/tb/mosaic_bringup_tb.sv` mid-edit (line 121 had `output wire h_tohost_value`
  at one bit against the 64-bit `m_tohost_value` on line 390, which Verilator
  refuses). Every number in §4 and §5 above was produced **after** that lane
  restored the file. Revision exercised: `sim/tb/mosaic_bringup_tb.sv`
  sha256 `78c27dfef350cc94e6ac87c7631488823f9309bccdc61eddc7d436f6dfca4f36`,
  repository `HEAD` `29bb23d`. The intermediate runs used a `/tmp` copy with only
  that one port width restored and are not quoted as evidence here.
* **The `p_flags == 0` carve-out in §2.3 is a judgement call, not a proof.** An
  image that declares no permissions is accepted at its entry; only an image that
  explicitly omits `PF_X` is refused. §2.3 says what would have to change to
  tighten it.
* **`e_type` is required to be exactly `ET_EXEC`.** An `ET_NONE`/`ET_REL` image is
  refused as `not-executable`; no such image exists in the corpus, so this is
  untested against a real one.
* **Segment alignment is checked as 8-byte, not page-aligned.** The loader's rule
  is the one the harness needs for word-granular image loading; `p_align` is read
  but not enforced. An image with `p_align` 4096 and a 4-byte-aligned vaddr is
  refused by the 8-byte rule, not by a page rule.
* **The DUT checks are about delivery, not about execution semantics.** The case
  proves the core retires the instruction the ELF placed at the reset vector and
  that every loaded word reads back byte-identical; it does not compare a retire
  stream (that is I-008) and it does not run a program to TOHOST.
* **The 40-cycle DUT run is a self-loop.** Nothing here constrains the core beyond
  the first retire and the memory contents, by design.
* **`MemoryModel` was tested, not changed.** It is not in this package's ownership
  list; where its documented precedence (alignment before region) disagreed with
  a probe I had written, the probe was corrected, and §3 records the precedence
  as pinned rather than assumed.
