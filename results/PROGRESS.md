# MosaicRV implementation progress

Append-only log. Newest entries last. Every entry names the command that was run
and what it actually printed; nothing here is a claim that was not observed.

Format: `date — task — status — evidence — next`.

---

## 2026-09-30 — I-001 — DONE — frozen profile/ISA/PMA/CSR/geometry configuration

**Delivered.** `config/schema/{profile,memory_map,csr,geometry}.schema.json`,
`config/profiles/p{0..3}.json`, `config/capability_ladder.json`,
`config/memory/p{0..3}.json`, `config/geometry/p{0..3}.json`,
`config/csr/mode_m.json`, `config/status/implementation_status.json`.
`tools/mosaic/jsonschema_mini.py` (dependency-free draft-07 subset validator),
`tools/mosaic/config_check.py`, `tools/check_profile.py`.

**Design decisions worth recording.**

- `Zicsr`, `Zifencei`, `Zicntr` and `Zihpm` have **no `misa` bit** since those
  extensions were split out of the base ISA. Only `I` (bit 8) and `M` (bit 12)
  appear in the p0 `misa`, giving reset `0x8000000000001100`.
- `config/capability_ladder.json` is the single place that says which profile may
  claim which extension, and which `I-`/`V-` tasks back it. A capability is only
  **advertised** in `misa` / compiler `-march` / reference capability once every
  implementation task named for it is in `config/status/implementation_status.json`.
  Until then the manifest reports it under `not_yet_implemented`.
- Each CSR table lists the `profiles` that may use it, and a table may not define
  a privilege mode the profile does not claim.

**Evidence actually run.**

```
$ python3 tools/check_profile.py --profile p0
profile p0: configuration OK
  claimed      : I, M, Zicsr, Zifencei, Zicntr, Zihpm
  advertised   : (none yet)
  not yet impl : I, M, Zicsr, Zifencei, Zicntr, Zihpm

$ python3 tools/check_profile.py --profile p0 --negative
  ... 28 illegal configurations rejected
```

The negative controls mutate a throwaway copy of the real configuration and
require the checker to reject it: claiming an extension one rung too early,
unknown extension, cacheable-but-non-idempotent PMA, overlapping regions, a
writable ROM, an unimplemented device, a non-power-of-two or oversized atomic
granule, a reset vector outside executable memory, `Zicsr` without `Zifencei`,
`xlen != 64`, VLEN without V, an unknown schema field, invalid JSON, a CSR with
no `spec_clause`, a duplicate CSR address, an over-wide CSR reset, a read-only
CSR declared WARL, a `warl_wpri` CSR with no WPRI fields, overlapping writable
and WPRI fields, and a privilege mode with no CSR table. The controls refuse to
run at all if the unmodified configuration is already broken, so a control cannot
pass for the wrong reason.

**CSR table.** Authored against the ratified Privileged Specification v1.12
(tag `Priv-v1.12`, commit `9896426`), cross-checked three ways including a
`riscv64-elf-as`/`objdump` round trip. `mstatus` reset `0x1800`, `misa` reset
`0x8000000000001100`, `mhartid` `0xF14` = 3860. `mstatush`, `cycleh`, `timeh`,
`instreth` and the other RV32-only aliases are deliberately **absent**, which is
what makes "accessing an unimplemented CSR raises illegal instruction" a testable
negative case. `mcycle`/`minstret` are declared read-write rather than read-only:
the address table marks them `MRW` and the spec says they can be written, so
declaring them read-only would have been a factual error in our own config.

---

## 2026-09-30 — I-002 — DONE — packet/tag/credit/memory contracts

**Delivered.** `config/contracts/interfaces.json` (eight interfaces from
`docs/implementation-plan.md` §1.3), `config/contracts/counters.json`,
`tools/check_contracts.py`, and `render_sv_id_package()` in
`tools/gen_manifest.py` which emits `build/<profile>/rtl/mosaic_id_pkg.svh`.

**The central rule.** No tag, generation or age counter is a hand-typed constant.
Each is an expression over the profile geometry, and `tools/check_contracts.py`
evaluates the expression and proves the arithmetic for the real profile:

- every counter must exceed the number of values that can be live at once;
- a counter declared `comparison: "order"` (an age or sequence number, compared
  with "which is older?") must **also** strictly exceed twice the largest
  distance two live values can be separated by;
- a counter declared `comparison: "identity"` (a tag generation, only ever
  compared for equality) needs only the first inequality. Applying the age rule
  to a generation would force a generation bit wider than it can ever need —
  forcing that rule onto every counter was the first version's mistake, and the
  checker caught it.

Expressions are evaluated from a checked AST that admits only the declared names,
integer literals, arithmetic and `clog2`. A contract file cannot reach
`__import__`, an attribute, a subscript, or any builtin.

Derived for p0: `rob_generation` modulus 128 over 64 live; `uop_sequence`
modulus 1024 over a maximum compare distance of 512; `prf_generation` 192 over 96;
`memory_transaction_generation` 64 over 22; `retire_sequence` 128 over 64.

**Other rules enforced.** No interface may carry `rob_index` without `rob_gen`,
`prf_tag` without `prf_gen`, `req_id` without `epoch`, or `tx_id` without
`tx_gen` — a wrapping index alone can never identify in-flight work. Every
interface must state an owner, a transfer rule, a cancel rule, a response rule, a
credit return path, resources with unique allocate **and** release events, and at
least one worked ABA counterexample. `route_id` was deliberately **removed** from
that rule after the checker flagged it: a static cluster index never wraps and
needs no generation.

**Evidence actually run.**

```
$ python3 tools/check_contracts.py --profile p0
profile p0: contracts OK

$ python3 tools/check_contracts.py --profile p0 --negative
  rejected: rob_index without rob_gen      ... interface macro_dispatch: carries
            rob_index without rob_gen; a wrapping index alone cannot identify in-flight work
  rejected: prf_tag without prf_gen        ... rejected
  rejected: tx_id without tx_gen           ... rejected
  rejected: req_id without epoch           ... rejected
  rejected: empty cancel rule              ... interface fetch_decode: missing cancel
  rejected: resource without a release event
  rejected: interface with no resources
  rejected: interface with no ABA counterexample
  rejected: field with no justification
  rejected: ordered counter modulus equal to twice the compare distance
  rejected: identity counter modulus below its live count
  rejected: counter with no declared comparison kind
  rejected: field narrower than its justification claims
  rejected: expression referencing an unknown name
  rejected: expression calling something other than clog2
  rejected: expression reaching for a builtin
negative controls: 16/16 illegal contracts rejected
```

---

## 2026-09-30 — I-003 — DONE — reproducible build with capability rejection

**Delivered.** `Makefile`, `tools/run_unit.py`, `tools/gen_manifest.py`,
`tools/verify.py`, `tools/synth_check.py`, `tests/unit/registry.json`,
`tests/suites/registry.json`.

**Contract, enforced rather than documented.**

- `make` targets that succeed exit 0; a mismatch, timeout or unimplemented
  configuration exits non-zero.
- `make <target> PROFILE=<unknown>` exits 2 with a message and **no** fallback.
  There is no code path that silently builds a different configuration.
- A configuration that does not check out produces **no** manifest at all;
  `tools/gen_manifest.py` refuses to write one.
- `tools/run_unit.py` adds the shared harness support (`sim/common/sim_common.cpp`)
  itself rather than trusting each case to list it. This came from a real link
  failure: two packages listed it in neither, and a later fix would have had one
  of them list it and break the other.
- `tools/verify.py --all` with an empty suite registry **fails**. Running nothing
  and reporting success is the specific outcome `docs/implementation-plan.md` §2
  rules out, so the p0 gate is correctly red until I-008 lands.
- Warnings are errors: Verilator runs with `-Wall`, and `make lint-cpp` holds the
  project's own C++ to `-Wall -Wextra -Wshadow`. Only `-Wall` reaches the
  simulator build, because Verilator's own runtime headers are compiled with the
  same `CFLAGS` and are not `-Wextra` clean.

---

## 2026-09-30 — I-004 — DONE — Verilator harness, loader, memory model, event stream

**Delivered.** `sim/common/elf_loader.{h,cpp}`,
`sim/common/memory_model.{h,cpp}`, `sim/common/event_tap.{h,cpp}`,
`sim/common/sim_common.{h,cpp}` (earlier commit), `sim/tb/harness_probe.sv`,
`sim/tb/harness_tb.sv`, `sim/harness/tb_harness.cpp`, plus the generated
`build/<profile>/sim/mosaic_platform.h`.

**Why the DUT is a test double.** The card requires the harness to be proven
before any real core exists. `harness_probe` implements LUI/ADDI/SD/JAL over a
stepping interface and is labelled throughout as *not* the processor. The harness
itself owns instruction fetch, all memory accesses, PMA faults, the cycle limit
and the event capture, so the memory model exists in exactly one place.

**Frozen test protocol.** `test_protocol` in every profile: TOHOST `0x00102000`
(bit 0 = pass), FROMHOST `0x00102008`, signature area `0x80000400` (4 words).
`tools/check_profile.py` rejects a configuration where TOHOST/FROMHOST do not land
in the `test_harness` device region, where the signature address is not writable,
or where four signature words would run past the region. Firmware, harness and
RTL therefore cannot disagree about where the result goes.

**Evidence actually run.**

```
$ python3 tools/run_unit.py --case harness.reset_load_exit   -> PASS
$ python3 tools/run_unit.py --case harness.bad_image         -> PASS
$ python3 tools/run_unit.py --case harness.timeout           -> PASS
$ python3 tools/run_unit.py --case harness.injected_mismatch -> PASS
$ make lint-cpp PROFILE=p0
lint-cpp: 7 file(s) clean
```

`harness.bad_image` builds a well-formed ELF64 header and corrupts exactly one
field per case, asserting the loader's named reason: bad magic, ELF32 class,
big-endian, x86-64 machine, truncated program header table, no `PT_LOAD`,
misaligned `PT_LOAD` vaddr, entry outside every segment, overlapping segments —
and then asserts a *good* image is still accepted, so the rejections only mean
something. `harness.timeout` proves a non-terminating program is stopped and
reported, not passed. `harness.injected_mismatch` flips one bit in a recorded
reference stream, proves exactly one event differs and that the report names
event 1, then re-compares the unmodified stream to prove the comparison is not
simply always reporting a difference.

**Four real defects the harness work found, all now fixed.**

1. `lui` was built as `{imm[31:12], 12'd0, 32'd0}`, putting the immediate in bits
   `[63:44]` instead of `[31:12]`. The first symptom was a store to
   `0x8000000000000400` instead of `0x80000400`.
2. Stores were decoded with the **I-type** immediate layout. S-type splits the
   immediate as `[31:25]` and `[11:7]`; the I-type layout silently folded `funct3`
   and `rs2` into the address.
3. The store's data register was read from the **`rd` field** instead of `rs2`;
   `rd` is not a destination for a store.
4. `sd` was accepted at `funct3 == 2`, which is `sw`. `sd` is `funct3 == 3`.
   `riscv64-elf-as`/`objdump` was used as the authority; two "corrections" made by
   hand were wrong and the assembler settled it.

Each was found by running the harness, not by reading it.

---

## 2026-09-30 — I-006 — DONE — synchronous RAM abstraction (independently verified)

Verified here rather than accepted on report. Lint clean with `-Wall`; the
registered case passes; and all four mutants were rebuilt and re-run in this
session and each **fails** the case:

| mutant | defect | result |
|---|---|---|
| `MOSAIC_RAM_MUTANT_COMB_READ` | `rdata` taken combinationally | exit 1, names the cycle |
| `MOSAIC_RAM_MUTANT_NO_BYTE_ENABLE` | byte write ignores `wmask` | exit 1, names the cycle |
| `MOSAIC_RAM_MUTANT_EARLY_VALID` | `rvalid` one cycle early | exit 1, `rvalid_a: expected low, got high` |
| `MOSAIC_RAM_MUTANT_ARRAY_RESET` | reset clears the array | exit 1, `expected 0xa5a5f00d4a5a0000, got 0x0` |

**Collision contract: READ-FIRST, byte by byte.** The read issued in cycle N
observes the array as it was at the start of cycle N, including in the lanes
`wmask` selects. This is the contract the integer PRF needs: `add x5,x5,x3`
reads and writes the same physical register in one cycle and needs the *old*
value, which read-first gives with no bypass network and no `rd == rs` special
case. It is also the mode FPGA block RAM offers natively, so a platform RAM that
cannot do it needs a wrapper change and nothing else — which is the portability
claim being tested.

**Reset strategy.** Only `rvalid`, a single control bit, is reset. The array has
no reset and no initial value; validity is tracked outside it. The agent's Yosys
evidence for this is the right kind of evidence and is worth keeping: with no
resettable flop cell anywhere in the design, synthesis stays at 9282 cells,
544 `$mux`; with the array-reset mutant compiled in it becomes 17559 cells, 4721
`$mux`, +4096 `$reduce_bool`, and 0.33 s becomes 5.04 s.

Full detail in `results/reports/I-006-ram.md`.

---

## 2026-09-30 — I-005 — IN PROGRESS — FIFO and skid buffer

`mosaic_fifo` and `mosaic_skid_buffer` are written and lint-clean. The FIFO's own
conservation and payload-stability checks pass (97069 checks over 3189 cycles).
The skid-buffer expectations added after the package started were still failing at
the time of this entry; the package is not marked done until they pass.

---

## Open / blocked

- **I-007** firmware corpus: in progress.
- **Stages 1-10**: not started. `make sim` correctly fails today because no
  simulation suite is registered; that is the honest state, not a defect.
- **Spike / NEMU / Difftest / SymbiYosys**: not yet attempted. The mandatory
  fallback is an independent host oracle in Python, which must never share a code
  path with the DUT.
- **FPGA and ASIC flows**: no vendor tool or PDK on this machine. Those gates stay
  `BLOCKED` naming the missing dependency; they cannot become `PASS`.