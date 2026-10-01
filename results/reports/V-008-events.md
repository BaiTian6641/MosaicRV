# V-008 — the frozen architectural event interface

Work package V-008 (`docs/validation-plan.md` §5 V-008, `docs/stage-0-contracts-bringup.md`
§V-008): freeze the architectural event interface — per-field width, validity,
boundary order and sampling instant — hand-derive the events of two same-cycle
instructions plus a trap and of a store drain, and validate the three producers
(SV tap, host record, one serialiser) against the schema.

| Artifact | Role |
|---|---|
| `config/contracts/event_v1.json` | the frozen interface: schema version 1, 21 fields, 100 octets/record, kinds RETIRE/TRAP/MEM_VISIBLE |
| `sim/common/event_codec.h` / `.cpp` | the one serialiser; its `MOSAIC_EVENT_FIELDS(X)` is the encoding order and the single source of truth |
| `sim/common/event_tap.h` | the de-facto host record `RetireEvent` (I-004 harness) |
| `rtl/core/mosaic_retire.sv` | the out-of-order per-lane `ev_*` producer |
| `sim/tb/mosaic_bringup_tb.sv` | the single-lane, registered `c_evt_*` producer |
| `tools/check_event_contract.py` | the checker wired into `make check`; 33 built-in negative controls |
| `results/reports/V-008-events.md` | this file |

## STATUS: PASS — interface frozen, checker wired and negative-controlled

| Check | Result |
| --- | --- |
| positive check | `event contract OK -- 21 fields, 100 octets/record, kinds RETIRE/TRAP/MEM_VISIBLE`, exit 0 |
| negative controls | `33/33 illegal interfaces rejected`, exit 0 |
| `make check` with the target wired in | exit 0 |
| fields with no RTL producer yet | exactly three, named on every run (`ev_insn`, `ev_pc_after`, `ev_trap_epc`; rules F-3/F-4) |

---

## 1. What the interface is

One record is **one architectural event**: one hart, one instruction — or one
memory visibility of exactly one previously retired store. The record is per
instruction, never per cycle: `EventRecord` in `sim/common/event_codec.h` has no
"the cycle's snapshot" member, so a shared retirement snapshot is not
representable. A record is emitted for each retiring lane in the cycle the
reorder buffer acknowledges the pop (the event follows the acknowledgement, not
the request), for each trapping instruction in the cycle it traps, and for each
architectural memory visibility. A two-wide retirement is two records, oldest
first, and lane *i*'s `retire_seq` is the counter plus *i*.

Encoding: a 4-byte little-endian `schema_version` header, then the 21 fields in
declaration order, each `ceil(width_bits/8)` octets, value little-endian in the
low bytes with high bits zero. 100 octets in total. The text form is the same
fields, same order, each value hexadecimal with `2*ceil(bits/8)` digits.

The 21 fields, resolved for p0 (`xlen` 64, `harts` 1, `rob_entries` 64,
`int_prf_entries` 96, `rename_width` 2):

| # | field | width (bits) | width_expr | group | class | valid kinds | sampled at |
|---|---|---|---|---|---|---|---|
| 1 | `retire_seq` | 8 | `clog2(2*rob_entries+1)` | identity | architectural | RETIRE/TRAP/MEM_VISIBLE | settled_pre_edge |
| 2 | `hart_id` | 1 | `harts` | identity | architectural | RETIRE/TRAP/MEM_VISIBLE | producer_constant |
| 3 | `cycle` | 64 | `xlen` | timing | observed | RETIRE/TRAP/MEM_VISIBLE | producer_constant |
| 4 | `kind` | 2 | `2` | kind | architectural | RETIRE/TRAP/MEM_VISIBLE | settled_pre_edge |
| 5 | `pc_before` | 64 | `xlen` | instruction | architectural | RETIRE/TRAP | settled_pre_edge |
| 6 | `pc_after` | 64 | `xlen` | instruction | architectural | RETIRE/TRAP | settled_pre_edge |
| 7 | `insn_bits` | 32 | `32` | instruction | architectural | RETIRE/TRAP | settled_pre_edge |
| 8 | `rob_id` | 14 | `2*clog2(int_prf_entries)` | microarch_identity | observed | RETIRE/TRAP | settled_pre_edge |
| 9 | `gpr_write_valid` | 1 | `1` | gpr | architectural | RETIRE | settled_pre_edge |
| 10 | `rd` | 5 | `5` | gpr | architectural | RETIRE | settled_pre_edge |
| 11 | `rd_value` | 64 | `xlen` | gpr | architectural | RETIRE | settled_pre_edge |
| 12 | `csr_write_valid` | 1 | `1` | csr | architectural | RETIRE | settled_pre_edge |
| 13 | `csr_addr` | 12 | `12` | csr | architectural | RETIRE | settled_pre_edge |
| 14 | `csr_value` | 64 | `xlen` | csr | architectural | RETIRE | settled_pre_edge |
| 15 | `mem_is_store` | 1 | `1` | memory | architectural | RETIRE/MEM_VISIBLE | settled_pre_edge |
| 16 | `mem_addr` | 64 | `xlen` | memory | architectural | RETIRE/MEM_VISIBLE | settled_pre_edge |
| 17 | `mem_data` | 64 | `xlen` | memory | architectural | RETIRE/MEM_VISIBLE | settled_pre_edge |
| 18 | `mem_size` | 3 | `3` | memory | architectural | RETIRE/MEM_VISIBLE | settled_pre_edge |
| 19 | `trap_cause` | 64 | `xlen` | trap | architectural | TRAP | settled_pre_edge |
| 20 | `trap_tval` | 64 | `xlen` | trap | architectural | TRAP | settled_pre_edge |
| 21 | `trap_epc` | 64 | `xlen` | trap | architectural | TRAP | settled_pre_edge |

Two fields are `observed`, not architectural: `cycle` (the harness's clock, never
comparable across microarchitectures — validation-plan §3 rule 4) and `rob_id`
(the retiring entry's `{generation, destination tag}`, a debug identity no
reference model has). They can never enter an architectural-equality comparison,
and neither can be a locating field. The locating fields are `hart_id` and
`retire_seq`; both are architectural and valid on every kind, so a retirement, a
trap and a store visibility are all attributable through the same two fields.

### 1.1 Validity by kind

This is the hand-derived core of the freeze: which fields a consumer may read for
each kind. Blank means the field is **outside the record's validity** and is
carried as zero; reading it anyway is the defect the rules name.

| group / field | RETIRE | TRAP | MEM_VISIBLE |
|---|---|---|---|
| `retire_seq`, `hart_id`, `cycle`, `kind` | defined | defined | defined |
| `pc_before`, `pc_after`, `insn_bits` | defined | defined (target = handler PC) | — |
| `rob_id` | defined (OoO producer) | defined (OoO producer) | — |
| `gpr_write_valid`, `rd`, `rd_value` | defined iff the instruction writes a GPR, and `rd`/`rd_value` iff `gpr_write_valid` | — (a trapping instruction writes no register) | — |
| `csr_write_valid`, `csr_addr`, `csr_value` | defined iff the instruction writes a CSR | — (writes no CSR) | — |
| `mem_is_store`, `mem_addr`, `mem_data`, `mem_size` | defined iff a store (zero for load/non-memory) | — | defined for the store |
| `trap_cause`, `trap_tval`, `trap_epc` | — (a trap emitted as a retire is a phantom instruction) | defined | — |

---

## 2. Hand-derived event table

The sequence below is derived from the RISC-V rules and from the record's own
`record` block (`lane_rule`, `trap_rule`, `memory_rule`) — not read back from the
RTL. Program (p0, hart 0, M-mode, `x7 = 0x8000_1000`, `mtvec = 0x8000_0200`);
the per-hart retirement counter is 7 before the window opens.

| cycle | lane | instruction (pc) | note |
|---|---|---|---|
| C1 | 0 | `addi x5, x0, 1` @ `0x8000_0000` | two same-cycle retirements |
| C1 | 1 | `addi x6, x5, 2` @ `0x8000_0004` | |
| C2 | 0 | `sd x6, 8(x7)` @ `0x8000_0008` | store retires; drain is later |
| C2+*k* | — | — | store becomes visible (drain) |
| C3 | 0 | `ecall` @ `0x8000_000C` | trap at the head |
| C3 | 1 | `addi x8, x0, 9` @ `0x8000_0010` | younger: squashed by the trap, **no record** |

The five records this produces, with **every** field the interface defines
(fields outside validity shown as `—`):

| # | cycle | kind | retire_seq | pc_before | pc_after | insn_bits | gpr (valid,rd,value) | csr (valid,addr,value) | mem (is_store,addr,data,size) | trap (cause,tval,epc) |
|---|---|---|---|---|---|---|---|---|---|---|
| R1 | C1 | RETIRE | 7 | `0x8000_0000` | `0x8000_0004` | `0x0010_0293` | (1, 5, 1) | (0, —, —) | (0, —, —, —) | — |
| R2 | C1 | RETIRE | 8 | `0x8000_0004` | `0x8000_0008` | `0x0022_8313` | (1, 6, 3) | (0, —, —) | (0, —, —, —) | — |
| R3 | C2 | RETIRE | 9 | `0x8000_0008` | `0x8000_000C` | `0x0063_B423` | (0, —, —) | (0, —, —) | (1, `0x8000_1008`, `0x3`, 3) | — |
| R4 | C2+*k* | MEM_VISIBLE | 9 | — | — | — | — | — | (1, `0x8000_1008`, `0x3`, 3) | — |
| R5 | C3 | TRAP | 10 | `0x8000_000C` | `0x8000_0200` | `0x0000_0073` | — | — | — | (11, 0, `0x8000_000C`) |

`hart_id` is 0 on every record (p0 has one hart); `kind` is the record's
discriminator (RETIRE = 1, TRAP = 2, MEM_VISIBLE = 3); `cycle` and `rob_id` are
observed fields supplied per record (the harness's clock; the retiring entry's
`{generation, tag}`), and are not compared architecturally. `insn_bits` values:
`addi` and `sd` as encoded, `ecall` = `0x0000_0073`; `trap_cause` 11 is ECALL
from M-mode, `trap_epc` = `pc_before` for a synchronous trap, `pc_after` =
`mtvec`.

The derivation, step by step:

* **R1/R2 — two lanes, one cycle, two records, no shared snapshot.** `rob_ack =
  2'b11`. `lane_rule` gives lane order = event order; `retire_seq` is the
  counter (7) plus *i*, so the younger instruction carries the larger number
  (R2 = 8). Each lane's PC and payload come from that lane's own ports — the
  record type has no cycle-wide members, so R1 cannot contaminate R2.
* **R3 — the store retires.** `mem_is_store` is 1 and the memory payload is the
  store's; `gpr_write_valid` is 0 because `sd` writes no register. The
  retirement is *not* the visibility (`store_drain_is_separate`).
* **R4 — the drain, a different kind, later.** The store buffer draining is a
  separate event: kind `MEM_VISIBLE`, carrying the store's own `retire_seq` (9),
  the same address/data/size. It has no `pc_before`/`pc_after`/`insn_bits` and no
  register/CSR payload — it is not an instruction. Attribution needs no
  knowledge of the store buffer's internal state, only `retire_seq`. The drain
  latency *k* is a property of the producer, not of the record; the ordering
  between R4 and a later retirement is not architecturally fixed.
* **R5 — the trap is one record and drops the younger lane.** `ev_trap[0]` is
  asserted and `ev_valid[0]` is set by the trap itself; the trap path drops
  everything younger, so lane 1 produces **no record at all** — a squashed
  instruction that is present but not retired contributes nothing (that single
  gate is what makes "a squashed instruction never touches committed state"
  true by construction). The `TRAP` record carries the cause and faulting value
  and nothing else: no register, no CSR, no store payload, even though the
  payload bus may have been driving one. A trap is never emitted as a RETIRE
  (ABA counterexample 1 in `config/contracts/interfaces.json`).

---

## 3. Three producers, one schema

The interface must hold across **three** producers and **one** serialiser, and
disagreement must be reported, never averaged:

1. `rtl/core/mosaic_retire.sv` — the out-of-order, per-lane `ev_*` producer;
   combinational from the buffer's head view, one record per acknowledged lane,
   gated on `ev_valid`. Per-lane widths are expressions over the file's own
   localparams (`RET_SEQ_W = $clog2(2*RET_ROB+1)` = 8, `RET_SIZE_W` = 3, …).
2. `sim/tb/mosaic_bringup_tb.sv` — the single-lane, **registered** `c_evt_*`
   wrapper; its ports are host-facing containers.
3. `sim/common/event_tap.h` — the host record `RetireEvent` used by the I-004
   harness.
4. `sim/common/event_codec.h` / `.cpp` — the one serialiser: the field list, the
   widths, the order, the kinds and the compiled schema version.

`tools/check_event_contract.py` proves the agreement structurally:

* every declaration of every producer file is parsed and **mapped to a schema
  field**; a declaration the schema does not map, or a schema source the file
  does not declare, is rejected (`INV-TAPDECL`, `INV-SOURCE`);
* the schema's `width_expr` is evaluated in the profile geometry namespace and
  checked against `width_bits` (`INV-COMPLETE`);
* a per-lane RTL port must be **exactly** the frozen width (`INV-RTLWIDTH`); a
  producer declaration **wider** than the field is a container and must record
  its exact declared width and a reason, a **narrower** one is a truncation and
  is rejected;
* the codec's field list, widths and order must equal the schema's and its
  compiled `MOSAIC_EVENT_SCHEMA_VERSION` must equal `schema_version`
  (`INV-CPPFIELDS`, `INV-VERSION`);
* the byte layout is recomputed from the field list and compared with the
  recorded offsets and total (`INV-ENCODING`);
* identities, kinds, the vector rule, the locating clause and the rules table
  are checked (`INV-IDENTITY`, `INV-KINDS`, `INV-VECTOR`, `INV-LOCATE`,
  `INV-RULES`).

The two recorded disagreements are containers, not drift: `c_evt_store_size`
is `[3:0]` where the field is 3 bits (reconciliation F-2 — the low three bits
are the field) and the host `store_size` is `unsigned` (32 bits). The retire
unit carries no `insn`, no `pc_after` and no per-lane `epc`; those are the
declared gaps of §5.

The check is itself under test: `--negative` copies the interface into a sandbox,
mutates **one input at a time**, and requires every mutation to be rejected — a
control that is merely present is not evidence. 33 mutations are rejected, e.g.
`retire ev_store_size narrowed to 2 bits` → `[INV-RTLWIDTH] field mem_size …
is 2 bits per lane but the schema says 3`, and `codec field order changed` →
`[INV-CPPFIELDS] field order: …`. A mutation whose target has vanished is
reported as a **broken control**, not silently skipped.

---

## 4. Integration-route freeze

`docs/validation-plan.md` §3 offers two integration routes, and V-008's job is to
pick one and freeze the interface both must share:

* **Route A (default, frozen here):** SV top-level retire probe → C++ canonical
  event → reference adapter. The SV stays synthesizable; the probe is enabled in
  the verification build; the harness carries the fixed-size record or explicit
  DPI parameters, never unstable hierarchical names. The canonical event is *not*
  the upstream ABI.
* **Route B (optional, not switched to):** freeze the Difftest generator →
  generate matching DPI Verilog/C++ → SV wrapper. Switching to B requires V-004
  and V-008's byte-layout/timing/field tests to pass first, and B must keep the
  **same** canonical event for audit.

The frozen artifact is `config/contracts/event_v1.json`: `id`
`mosaicrv.event.v1`, `schema_version` 1, `reference_profile` p0, `frozen`
2026-10-01, spec anchored to validation-plan §3 and §5 V-008. The freeze is
enforced, not declared: the codec refuses to encode a record whose caller-declared
version differs from the version it was compiled against, the decoder rejects a
header that differs, and the checker proves the compiled constant equals the
schema's `schema_version`. A route-B artifact must therefore be regenerated
against schema version 1 (or a new, versioned schema) — there is exactly one
serialiser and one byte layout, so a route-B producer cannot quietly encode a
different record.

---

## 5. Fields with no RTL producer yet

Printed by the tool on **every** run, so the gap is never implied and never
silent:

```
profile p0: fields with no RTL producer yet: ev_insn (insn_bits), ev_pc_after (pc_after), ev_trap_epc (trap_epc) (see rules F-3/F-4)
```

The out-of-order retire stream is complete for the effects it owns (register,
CSR, store, trap cause/tval, identity) but carries no architectural next PC, no
instruction bits and no per-lane epc. `insn_bits`, `pc_after` and `trap_epc` are
declared as `pending_rtl` sources with a reason each (reconciliation F-3): the
freeze declares the ports `mosaic_retire.sv` must grow (`ev_insn`, `ev_pc_after`,
`ev_trap_epc`). Until they exist those fields are supplied by the driving
scenario through the bring-up tap and are listed as **not verified through the
out-of-order producer**. F-4 is the same class one layer down: the memory
endpoint exports counters and last-fault values but no event tap, so the
`MEM_VISIBLE` record has no DUT-side producer yet (its producer today is the
harness memory side effect). F-5 records that the de-facto host record has no
`rob_id`/`cycle`/CSR fields and no memory-visibility message, so it must migrate
to the canonical record and codec.

---

## 6. What this does not cover

* **No runtime round-trip yet.** The checker proves the field list, widths,
  order, kinds and version agree across the schema, the two SV declarations and
  the codec source; `event_codec.cpp` also compiles syntax-clean under the flags
  `make lint-cpp` uses (`c++ -std=c++17 -fsyntax-only -Wall -Wextra -Wshadow
  -Isim/common sim/common/event_codec.cpp`). The
  encode→decode→text round trip of a *live DUT event stream* is not exercised by
  a unit case yet — that belongs to V-010 (clock/sampling calibration) and
  V-013 (multi-retire comparison), which will drive the codec from real
  records. `RetireEvent::Line()` (the text form) is driven end-to-end by
  `tb_bringup.cpp` and `tb_harness.cpp`, but the binary codec is not.
* **Caches, MMIO, interrupts, multi-hart.** p0 has none: no cache and no MSHR,
  no MMIO event tap, no asynchronous-interrupt record (the `kind` table carries
  RETIRE/TRAP/MEM_VISIBLE only; validation-plan §3 lists SYNC_TRAP,
  ASYNC_INTERRUPT, VECTOR_PARTIAL_TRAP, EXTERNAL_INPUT, HALT as future kinds),
  and one hart (`hart_id` is one bit and always zero).
* **Vectors.** Deferred: p0 implements RV64IM_Zicsr_Zifencei, so no vector field
  is listed; the `deferred` entry `vector_body` forbids ever adding a vector
  field with a literal window width (`INV-VECTOR` rejects a fixed window such as
  128 bits).
* **The reference adapter.** Route A's C++→reference step is not built here;
  only the canonical event and its serialiser are frozen.

---

## 7. Commands and observed output

Wired into `make check` as `check-event-contract` (positive pass, then the
negative controls):

```
$ python3 tools/check_event_contract.py --profile p0
profile p0: event contract OK -- 21 fields, 100 octets/record, kinds RETIRE/TRAP/MEM_VISIBLE
profile p0: fields with no RTL producer yet: ev_insn (insn_bits), ev_pc_after (pc_after), ev_trap_epc (trap_epc) (see rules F-3/F-4)

$ python3 tools/check_event_contract.py --profile p0 --negative
profile p0: event contract OK -- 21 fields, 100 octets/record, kinds RETIRE/TRAP/MEM_VISIBLE
profile p0: fields with no RTL producer yet: ev_insn (insn_bits), ev_pc_after (pc_after), ev_trap_epc (trap_epc) (see rules F-3/F-4)
  rejected: retire ev_store_size narrowed to 2 bits      [INV-RTLWIDTH] field mem_size: rtl/core/mosaic_retire.sv:ev_store_size is 2 bits per lane but the schema says 3
  ... (33 mutations) ...
  rejected: reconciliation naming a field that does not exist [INV-SOURCE] reconciliation F-1: names field 'pc_future' which is not in the field list
negative controls: 33/33 illegal interfaces rejected

$ make check PROFILE=p0
...
python3 tools/check_event_contract.py --profile p0
profile p0: event contract OK -- 21 fields, 100 octets/record, kinds RETIRE/TRAP/MEM_VISIBLE
python3 tools/check_event_contract.py --profile p0 --negative
profile p0: event contract OK -- 21 fields, 100 octets/record, kinds RETIRE/TRAP/MEM_VISIBLE
...
  rejected: ...
negative controls: 33/33 illegal interfaces rejected
...
PASS 11 isolation checks: every failure scenario is caught and every healthy control is clean
$ echo $?
0
```

`file statuses in this run`:

| gate | rc |
|---|---|
| `tools/check_event_contract.py --profile p0` | 0 (`21 fields, 100 octets/record, kinds RETIRE/TRAP/MEM_VISIBLE`) |
| `tools/check_event_contract.py --profile p0 --negative` | 0 (`33/33 illegal interfaces rejected`) |
| `make check PROFILE=p0` with the target wired in | 0 |

Files changed by this work package: `Makefile` (added the `check-event-contract`
target, the `.PHONY` entry, the help line and the `check` dependency),
`results/reports/V-008-events.md` (this file). No RTL, schema, codec, registry or
status file was modified; the schema/codec/tap and the checker were already in
the tree and are frozen by this freeze.
