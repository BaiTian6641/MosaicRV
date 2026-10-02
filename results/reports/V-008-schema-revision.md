# V-008 schema revision — v1 → v2: the instruction length field (`insn_len`)

Revision of the frozen architectural event interface `config/contracts/event_v1.json`.
Companion to `results/reports/V-008-events.md` (the freeze record; its §8 is the
addendum for this revision). Written because `python3
tools/check_event_contract.py --profile p0` was RED after I-041 added `ev_insn`
and `ev_len` to the retire unit's event tap and the frozen contract had not
followed.

| Artifact | Role in this revision |
|---|---|
| `config/contracts/event_v1.json` | the interface: v2, 22 fields, 101 octets/record |
| `sim/common/event_codec.h` / `.cpp` | the one serialiser: `MOSAIC_EVENT_FIELDS(X)`, widths, order, kinds, compiled version |
| `sim/common/event_tap.h` | the de-facto host record `RetireEvent` |
| `rtl/core/mosaic_retire.sv` | the producer whose declarations were unmapped (not edited) |
| `tools/check_event_contract.py` | the checker: positive + negative controls |
| `results/reports/V-008-events.md` | the freeze record; §8 is this revision's addendum |
| `results/reports/V-008-schema-revision.md` | this file |

## STATUS: PASS — v2 reconciled across the three producers and the serialiser

| Gate | Observed | rc |
| --- | --- | --- |
| `python3 tools/check_event_contract.py --profile p0` | `profile p0: event contract OK -- 22 fields, 101 octets/record, kinds RETIRE/TRAP/MEM_VISIBLE`; gaps `ev_pc_after (pc_after), ev_trap_epc (trap_epc)` | 0 |
| `python3 tools/check_event_contract.py --profile p0 --negative` | `negative controls: 36/36 illegal interfaces rejected` | 0 |
| `make check PROFILE=p0` | green (includes both of the above) | 0 |
| `python3 tools/check_records.py` | `ok   records agree: 51 delivered package(s), 60 registered case(s), …` | 0 |
| `make lint-slang PROFILE=p0` | all tidy rules PASS | 0 |
| `make lint-cpp` | `lint-cpp: 55 file(s) clean` | 0 |
| six affected cases, each from a deleted `build/p0/unit/<case>` | all PASS (see §6) | 0 |

## 1. What was inconsistent

`tools/check_event_contract.py --profile p0` was RED before the change:

```
ERROR [INV-TAPDECL] producer retire_unit: rtl/core/mosaic_retire.sv declares the event declaration 'ev_insn' which the schema does not map to any field
ERROR [INV-TAPDECL] producer retire_unit: rtl/core/mosaic_retire.sv declares the event declaration 'ev_len' which the schema does not map to any field
profile p0: 2 event-contract problem(s)
```

The RV64C package I-041 (`results/reports/I-041-compressed.md` §2) made the
retire unit carry the committed instruction's **own bits** (`ev_insn`) and its
**own length** (`ev_len`, 3 bits per lane: 2 bytes for a 16-bit compressed
instruction, 4 otherwise) so that both travel to the architectural record, as the
V-008 card requires ("正常与 trap trace 用 original instruction PC/length"). The
frozen v1 schema was never updated:

* `insn_bits` was declared in v1 as a `pending_rtl` source — reconciliation F-3,
  "declared but no producer yet". I-041 is that field finally having a producer,
  so `ev_insn` is F-3 landing and not a new field.
* the instruction **length** was genuinely new: v1 listed no length field, and
  `ev_len` was a declaration the schema did not map.

The RTL is right; the contract lagged. No RTL was edited.

## 2. What changed

* `insn_bits`: source `{"kind":"pending_rtl", … "decl":"ev_insn"}` →
  `{"kind":"rtl_lane","file":"rtl/core/mosaic_retire.sv","decl":"ev_insn","relation":"exact"}`.
  `insn_bits` is no longer printed as a gap.
* New field `insn_len`: group `instruction`, `width_bits` 3, `width_expr` "3",
  `class` architectural, `identity_role` none, `valid_kinds` `["RETIRE","TRAP"]`,
  `sampled_at` `settled_pre_edge`, `encoding_octets` 1, inserted **after
  `insn_bits`** so it sits inside the instruction group. Sources: `rtl_lane
  ev_len` exact, and `sim/common/event_tap.h` `insn_len` as an 8-bit container.
* Byte layout: `total_octets` 100 → 101; every offset from `rob_id` on +1 (§3).
* `encoding`, `producers[retire_unit].fields_unavailable` (`insn_bits` removed,
  `insn_len` never listed), `producers[retire_unit].note`, a `revisions` block,
  reconciliation F-3 (`status` key) and reconciliation F-7 added.
* `sim/common/event_codec.h`: `X(insn_len, 3)` added after `X(insn_bits, 32)`;
  `MOSAIC_EVENT_SCHEMA_VERSION` 1 → 2; a header comment records the v2 delta.
* `sim/common/event_tap.h`: `RetireEvent` gains `uint8_t insn_len = 0` (mapped in
  the schema, so the struct cannot drift from the field). `Line()` is unchanged
  on purpose — see §5.
* `tools/check_event_contract.py`: three controls added; three existing controls
  re-pointed (§4).

### 2.1 Why the field is 3 bits, not the plan's `u8`

`docs/validation-plan.md` §3 names `insn_len:u8` in the future canonical event.
An RV64 instruction is 2 or 4 bytes; the RTL carries the value in `RET_SIZE_W` =
3 bits (`rtl/core/mosaic_retire.sv`), and `docs/validation-plan.md` §3's widths
are container widths for an interface that already diverges from the frozen one
(v1 froze `hart_id` at 1 bit and `retire_seq` at 8 bits, not the plan's `u32` /
`u64`). The checker's `no_truncated_field` rule forbids freezing a field wider
than its per-lane RTL producer, so v2 freezes the producer's own 3 bits. This is
recorded as reconciliation F-7. A consumer treats any value other than 2 or 4 as
invalid rather than re-deriving the length.

## 3. The v2 record layout

Header: 4-byte little-endian `schema_version` (now `2`). Then, in declaration
order (which is the codec's `MOSAIC_EVENT_FIELDS(X)` order), each field
`ceil(bits/8)` octets, value little-endian in the low bytes.

| # | field | bits | octets | offset v2 | offset v1 | group | class | valid kinds | sampled at |
|---|---|---|---|---|---|---|---|---|---|
| 1 | `retire_seq` | 8 | 1 | 4 | 4 | identity | architectural | RETIRE/TRAP/MEM_VISIBLE | settled_pre_edge |
| 2 | `hart_id` | 1 | 1 | 5 | 5 | identity | architectural | RETIRE/TRAP/MEM_VISIBLE | producer_constant |
| 3 | `cycle` | 64 | 8 | 6 | 6 | timing | observed | RETIRE/TRAP/MEM_VISIBLE | producer_constant |
| 4 | `kind` | 2 | 1 | 14 | 14 | kind | architectural | RETIRE/TRAP/MEM_VISIBLE | settled_pre_edge |
| 5 | `pc_before` | 64 | 8 | 15 | 15 | instruction | architectural | RETIRE/TRAP | settled_pre_edge |
| 6 | `pc_after` | 64 | 8 | 23 | 23 | instruction | architectural | RETIRE/TRAP | settled_pre_edge |
| 7 | `insn_bits` | 32 | 4 | 31 | 31 | instruction | architectural | RETIRE/TRAP | settled_pre_edge |
| 8 | **`insn_len`** | **3** | **1** | **35** | — | instruction | architectural | RETIRE/TRAP | settled_pre_edge |
| 9 | `rob_id` | 14 | 2 | 36 | 35 | microarch_identity | observed | RETIRE/TRAP | settled_pre_edge |
| 10 | `gpr_write_valid` | 1 | 1 | 38 | 37 | gpr | architectural | RETIRE | settled_pre_edge |
| 11 | `rd` | 5 | 1 | 39 | 38 | gpr | architectural | RETIRE | settled_pre_edge |
| 12 | `rd_value` | 64 | 8 | 40 | 39 | gpr | architectural | RETIRE | settled_pre_edge |
| 13 | `csr_write_valid` | 1 | 1 | 48 | 47 | csr | architectural | RETIRE | settled_pre_edge |
| 14 | `csr_addr` | 12 | 2 | 49 | 48 | csr | architectural | RETIRE | settled_pre_edge |
| 15 | `csr_value` | 64 | 8 | 51 | 50 | csr | architectural | RETIRE | settled_pre_edge |
| 16 | `mem_is_store` | 1 | 1 | 59 | 58 | memory | architectural | RETIRE/MEM_VISIBLE | settled_pre_edge |
| 17 | `mem_addr` | 64 | 8 | 60 | 59 | memory | architectural | RETIRE/MEM_VISIBLE | settled_pre_edge |
| 18 | `mem_data` | 64 | 8 | 68 | 67 | memory | architectural | RETIRE/MEM_VISIBLE | settled_pre_edge |
| 19 | `mem_size` | 3 | 1 | 76 | 75 | memory | architectural | RETIRE/MEM_VISIBLE | settled_pre_edge |
| 20 | `trap_cause` | 64 | 8 | 77 | 76 | trap | architectural | TRAP | settled_pre_edge |
| 21 | `trap_tval` | 64 | 8 | 85 | 84 | trap | architectural | TRAP | settled_pre_edge |
| 22 | `trap_epc` | 64 | 8 | 93 | 92 | trap | architectural | TRAP | settled_pre_edge |

`total_octets`: **101** (v1: 100). 22 fields (v1: 21). The only semantic change
is `insn_len`; the v1 row records the offsets before the insertion, and the
`rob_id`-onward shift is the reason the version moves.

### 3.1 The new field's validity

`insn_len` is the number of bytes of the instruction that retired in that lane:
`2` for a 16-bit compressed instruction, `4` for a 32-bit one. It is valid
whenever the record is about an instruction (kind RETIRE or TRAP) and its length
is known; an instruction whose fetch faulted has no complete encoding, so the
field is zero and the TRAP record's `trap_cause` says why. It is the
instruction's own length as committed — not a fetch size, and not something a
consumer re-decodes from `insn_bits[1:0]`. I-041's structural invariant still
holds and can be cross-checked: for every lane, `len == 2` iff
`insn_bits[1:0] != 2'b11` (`results/reports/I-041-compressed.md` §3).

## 4. Negative controls (observed)

Before this revision the tool carried 33 controls. Now it carries **36**, all
rejected with the offending field/producer named (`36/36 illegal interfaces
rejected`). Three were added for the new field and observed rejected:

| # | new control | observed rejection |
|---|---|---|
| A1 | `retire ev_len renamed` — the schema's mapping is left pointing at a port that no longer exists | `[INV-TAPDECL] producer retire_unit: rtl/core/mosaic_retire.sv declares the event declaration 'ev_ilen' which the schema does not map to any field` |
| A2 | `codec insn_len encoded 4 bits wide` — the codec disagrees with the schema's 3 bits | `[INV-CPPFIELDS] field insn_len: sim/common/event_codec.h encodes it 4 bits wide, the schema says 3` |
| A3 | `host record insn_len renamed` — the host container is renamed without the schema | `[INV-TAPDECL] producer host_oracle_tap: sim/common/event_tap.h declares the event declaration 'insn_bytes' which the schema does not map to any field` |

The three pre-existing controls whose **mutation target moved** with the version
or the layout were re-pointed (not weakened — each is still a single mutation of
the interface that must be rejected):

| control | re-pointed because | observed rejection |
|---|---|---|
| `codec schema version bumped alone` | the compiled version is now 2, so the mutation is 2 → 3 | `[INV-VERSION] schema_version: the schema says 2 and sim/common/event_codec.h is compiled against 3; the encoder's version check would compare two different things` |
| `pending source with no reason recorded` | `insn_bits` no longer has a `pending_rtl` source; re-pointed at `pc_after`, which still does | `[INV-SOURCE] field pc_after: pending source without a reason` |
| `encoding offset drifted by one byte` | `mem_data` moved 67 → 68; the drift is now +1 from 68 | `[INV-ENCODING] field mem_data: encoding_offset_bytes is 69 but the layout puts it at 68` |

One pre-existing control now names a different field in its rejection without
being weakened: `retire ev_store_size narrowed to 2 bits` narrows the shared
localparam `RET_SIZE_W`, which the new `ev_len` port also uses, so the checker
reports the first field in order — `[INV-RTLWIDTH] field insn_len:
rtl/core/mosaic_retire.sv:ev_len is 2 bits per lane but the schema says 3`. The
mutation is still a real narrowing of a lane port and is still rejected.

## 5. Consumers of the record, and a stored record from before the change

* **`sim/common/event_codec.h` / `.cpp`** — rebuilt to v2; the field list, widths,
  order, kinds and compiled version agree with the schema field for field
  (checked by `INV-CPPFIELDS`, `INV-VERSION`). A throwaway program (not
  committed) round-tripped a record: 101 octets, header `2`, `insn_len = 2`
  survives encode→decode, a v1 header is refused (`kVersionMismatch`), and a
  value above the 3-bit field is refused (`kFieldOverflow`).
* **`sim/common/event_tap.h` `RetireEvent`** — gains `insn_len`, mapped as the
  8-bit container. Its `Line()` text form is **unchanged**: that format is
  byte-frozen by the I-008 reference model's `emit()` (embedded in
  `sim/unit/tb_bringup.cpp`) and compared line for line by the I-004 harness, so
  the I-004 text stream does not yet show the length. Extending it is the host
  record's migration onto `event_codec.h` (reconciliation F-5), a declared
  follow-up, not part of this revision.
* **The reference adapter** (route A's C++→reference step) is not built in the
  tree, so nothing consumes the binary codec yet; when it is built it must be
  compiled against v2.
* **A future waveform decoder or route-B generator** must be regenerated against
  v2; v1 artifacts are not forward-compatible.
* **A stored binary record from before the change** carries header `1` and the
  v1 layout; the v2 decoder rejects it at the header. There is no migration
  reader, and reinterpreting a v1 record as v2 would misread every field from
  `rob_id` onward. Stored **text** streams (`RetireEvent::Line()` files under
  `results/unit/...`) are unaffected — they are not this codec's format.

## 6. Re-verification (commands and observed results)

```
$ python3 tools/check_event_contract.py --profile p0
profile p0: event contract OK -- 22 fields, 101 octets/record, kinds RETIRE/TRAP/MEM_VISIBLE
profile p0: fields with no RTL producer yet: ev_pc_after (pc_after), ev_trap_epc (trap_epc) (see rules F-3/F-4)
                                       # rc 0

$ python3 tools/check_event_contract.py --profile p0 --negative
  ... 36 lines, each "rejected: <name> [INV-...] ..."
negative controls: 36/36 illegal interfaces rejected      # rc 0

$ make check PROFILE=p0                                    # rc 0 (includes the two above)
$ python3 tools/check_records.py
ok   records agree: 51 delivered package(s), 60 registered case(s), every claimed case exists and belongs to the package claiming it   # rc 0
$ make lint-slang PROFILE=p0                               # rc 0
$ make lint-cpp                                            # rc 0 (55 files clean)

$ rm -rf build/p0/unit/<case> && python3 tools/run_unit.py --profile p0 --case <case>
PASS core.event_payload           task=I-017
PASS core.trap_csr_program        task=I-023
PASS compressed.cross_boundary    task=I-041
PASS core.corpus_sweep            task=I-023
PASS trap.precise_state           task=V-014
PASS retire.width_and_order       task=V-013
```

## 7. Not covered

* **No live DUT round trip of the binary codec.** The codec is still not driven
  by a DUT event stream (no in-tree consumer); its v2 behaviour is proved by the
  throwaway encode→decode smoke test and by the checker's structural agreement,
  not by a unit case. That remains V-010/V-013 work.
* **The host record's text form does not carry `insn_len`.** `RetireEvent::Line()`
  is unchanged (see §5), so the I-004/I-008 text streams stay byte-identical; the
  struct member is present and mapped but no in-tree harness populates it yet.
  This is the F-5 migration.
* **No migration reader for v1 records.** A stored v1 binary record is
  undecodable by v2 and there is no converter. None is stored in the tree today
  (the codec's binary form is not written by any case), so nothing in the
  repository breaks; a consumer holding one must keep a v1 decoder.
* **Only p0 is checked.** `check_event_contract.py` is run for `--profile p0`;
  the field widths are resolved against p0's geometry.
* **The plan's aspirational widths are not reconciled beyond `insn_len`.** v2
  still narrows `hart_id`, `retire_seq`, `kind` and `insn_len` relative to
  `docs/validation-plan.md` §3's future-interface widths; that divergence
  predates this revision and is unchanged (documented in V-008-events.md §1 and
  reconciliation F-7 for the new field only).
* **The delivery ledger text is stale.** `config/status/implementation_status.json`'s
  V-008 evidence note still describes "schema version 1, 21 fields, 100 octets"
  and "33 built-in negative controls". That file is the delivery ledger, outside
  this revision's ownership; it needs a one-line update by whoever owns it (the
  checker does not read it, so it does not gate anything).
