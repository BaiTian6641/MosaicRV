# I-058 — vector chaining, element readiness and the two element-granular hazards

Work package **I-058** of `docs/implementation-plan.md` §3.1 (`V-059`), the card:

> **Inputs**: per-element producer readiness, source lifetime, VRF overlap.
> **Action**: element-by-element forwarding and readiness, with a safe no-chaining
> configuration as the control; run `CASE=rvv.chaining_hazards` over
> source-before-write and instruction cancellation.
> **Outputs**: the chaining network, an element scoreboard, and comparison counters.
> **Pass**: the defined architectural state is identical with chaining on and off,
> and a WAR source that has not finished reading cannot be released because some
> other lane completed.
> **Fail**: a macro-level ready bit masking element-level dependencies, or a packet
> from a wrong-path execution being accepted by a new descriptor.

## STATUS: COMPLETE (unit); NOT WIRED INTO THE CORE

| Acceptance item | Evidence |
| --- | --- |
| The chaining network exists as RTL and is element-granular | `rtl/core/mosaic_vec_chain.sv` (new); `sim/tb/mosaic_vec_tb.sv` instantiates it beside the I-051 descriptor, the I-056 packetizer, the I-057 restart controller and the VRF |
| A consumer begins element *i* as soon as its producer's element *i* is written | rule R1 in the module header; `c0_rdy_c = chain_en_i ? rdq_q[c0_req_index_i] : done_q`; in the on/off phase 7 of the 8 reads land before the producer's `done` with chaining (`pre_on=7`) and 0 of 8 without it (`pre_off=0`) |
| The producer's elements complete in ascending, descending and interleaved order, and the consumer reads the right values | phase `order ascending` / `order descending` / `order interleaved`; each element's read is compared against the host oracle `ChainElemVal(gen,i)`; the descending order writes the *last* element first |
| Fail criterion — a macro-level ready bit masking element-level dependencies | `MOSAIC_VEC_CHAIN_MUTANT_MACRO_READY`, exit 1 on `order descending: an element was read before its producer wrote it` |
| Fail criterion — a wrong-path packet accepted by a new descriptor | `MOSAIC_VEC_CHAIN_MUTANT_STALE_PACKET`, exit 1 on `cancel: a packet from the cancelled generation 30 was accepted by the new descriptor` |
| WAR: a source that has not finished reading is not released | phase `war A`: with the consumer read only `0..2`, an overwrite of element 4 is refused and of element 1 (already read) is granted; phase `war B`: another reader finishing does not release element 4 |
| WAR: "some other lane finished" does not release the source | phase `war B` — consumer 1 reads all six elements and finishes, consumer 0 has read `0..2`, the release of element 4 is still refused |
| Fail criterion — a WAR release that ignores an unfinished read | `MOSAIC_VEC_CHAIN_MUTANT_WAR_RELEASE`, exit 1 on `war A: element 4 was released while the consumer had read only elements 0..2` |
| The no-chaining control is switchable and the architectural state is identical | `chain_en_i = 0` waits for the whole macro; phase `on/off` runs the same program both ways and requires the read values field for field and the descriptor bitmap equal |
| Fail criterion — a chaining path that reorders elements within one macro | `MOSAIC_VEC_CHAIN_MUTANT_REORDER`, exit 1 on `order ascending: element 0 read 0x0000000000000000 expected 0x4121fefe2121fefe` |
| The defined architectural state advances without commit | the descriptor is allocated and never released during a run; its element bitmap follows the network's accepted packets (`chain_bind_i`) and reaches all eight elements while the macro is still in flight, so execution is decoupled from retire/release |
| `python3 tools/run_unit.py --profile p2 --case rvv.chaining_hazards` | `RESULT PASS rvv.chaining_hazards checks=184 orders=3 cycles_on=10 cycles_off=18 fwd_on=8 fwd_off=0 stall_on=1 stall_off=9 pkt_on=8 pkt_off=8 pre_on=7 pre_off=0 cycles=185` |
| `python3 tools/run_unit.py --profile p0 --case rvv.chaining_hazards` | PASS, identical 184 checks and identical counters |
| The same binary at seeds 2, 7 and 12345 | PASS, identical 184 checks and identical counters |
| `verilator --lint-only -Wall` on the module, shipping and all four `-D` controls | clean under every definition |
| `slang-tidy --std 1800-2017` on the module | exit 0 (only the tree-wide STYLE-13 unnamed-`always_comb` advisories the other vector files carry) |
| `python3 tools/run_vec_chain_controls.py --profile p0` | 4 of 4 mutants mutate the binary, exit 1 and name the check they break |
| Sibling cases unchanged | `rvv.integer_mask_permute`, `rvv.memory_modes`, `rvv.partial_fault_restart`, `rvv.fp_flags_reduction`, `rvv.descriptor_legality`, `rvv.vset_boundaries`, `rvv.vtype_layout`, `vrf.mapping_aliases` PASS under p0 and p2 |
| Lint | `tools/lint_rtl.py --profile p0` and `--profile p2`: 58 sources clean (57 + this module) |
| `make check` | green, exit 0, under p0 and p2 |
| `check_records` | green: 71 delivered packages, 82 registered cases |
| `check_exclusions` and `--negative` | green: 54 registered / 21 open / 33 covered, closure ok; negative controls 13/13 illegal ledgers rejected |
| ACT4 | 127/127 from the committed driver (reproduced); the working tree's 0/127 is the concurrent V-010 lane's uncommitted `BusResetGate`, proven by rebuilding the case with the committed driver — see the tree-state note |
| This report | — |

## The files, and what each change is

* **new** `rtl/core/mosaic_vec_chain.sv` (448 lines) — the chaining network: the
  element readiness scoreboard, the forwarded element buffer, the packet
  generation check, and the source-lifetime (WAR) arbiter.
* **modified** `sim/tb/mosaic_vec_tb.sv` — additive only: the network's ports
  (producer, two consumers, WAR, read-back, counters), one instance
  `u_vec_chain`, and a `chain_bind_i` arm in the descriptor's element-progress
  mux. The I-057 arm (`rst_bind_i`) keeps priority, so the restart case's
  ownership is unchanged; when neither is set the driver owns the ports as before.
  Every existing port and instance is untouched.
* **modified** `sim/unit/tb_vec.cpp` — additive only: the `Chain` harness, the
  four phases of `rvv.chaining_hazards`, the host oracle, the coverage struct and
  the case's `RESULT` fields. Existing phase sets, harnesses and the default case
  are untouched; the file still serves all eight cases.
* **new** `tools/run_vec_chain_controls.py` — the four controls.

`tests/unit/registry.json` and `config/status/implementation_status.json` were
**not** edited. The case stays marked `"pending": true`; the integration lead
clears that marker when the package is recorded. (The entry already lists
`rtl/core/mosaic_vec_chain.sv`, so the recorded `rtl` list is complete for this
package.)

`mosaic_vec_desc.sv` and `mosaic_vec_restart.sv` were **not** touched: the
scoreboard that chaining needs is a different fact from the architectural
progress those files keep, which is the design decision the next section states.

## The chaining rule, as a rule

The producing macro's element *i* is available the moment it is written, and a
consumer takes a source element only when that element -- not the macro -- is
ready. This is what the network implements:

* **R1 — element readiness.** A consumer C of the produced group may read element
  *i* when the producer's element *i* has been written and buffered (`rdq[i]`).
  With `chain_en_i = 0` -- the safe no-chaining control -- C waits for the
  producer's whole macro (`done_q`) and reads the same values, later. Readiness
  never depends on the producer retiring or on the descriptor's release: the
  network has no connection to `release_i`, and the phase set never releases the
  descriptor while reads are in flight.
* **R2 — element identity.** A producer element packet is accepted only when it
  carries the group's current producer generation, its index is inside `vl`, the
  macro has not faulted, and the element is not already written. A packet from a
  cancelled or superseded generation is refused, and because the descriptor's
  element progress is taken from the network's accepted-packet pulse
  (`chain_bind_i`), a stale packet enters *neither* the forwarding scoreboard
  *nor* the descriptor's bitmap.
* **R3 — source lifetime (WAR).** A consumer holding the group as a source
  registers a per-element hold over `[0, vl)`. The hold on element *i* is
  discharged only by that consumer reading element *i*, or by that consumer
  cancelling/finishing (the `c*_finish_i` input means "this consumer no longer
  needs the group"). An overwrite of group G at element *i* (`war_grant_o`) and
  the release of G as a whole (`src_release_ok_o`) are granted only when no hold
  remains on that element / on any element of that group. A reader that finished
  does not discharge another reader's hold.
* **R4 — element order.** Reading element *i* returns the producer's element *i*.
  The forwarding buffer is indexed by the element index, so a grant for index *i*
  can never return another element's slot: element order within a macro is the
  contract the case's `MOSAIC_VEC_CHAIN_MUTANT_REORDER` control attacks.
* **R5 — fault and cancel.** A producer fault freezes the scoreboard: no element
  at or after the fault is accepted. A producer cancel empties it, so a cancelled
  macro's readiness cannot be consumed by anyone.

## The scoreboard's shape — parallel, and why

The descriptor's element bitmap (I-051, bound by I-057) is *not* extended; the
network keeps a **parallel** element scoreboard. Three facts made that the right
call:

1. **Different meaning.** The descriptor's bitmap is *architectural* progress --
   it is what the restart controller (I-057) resumes from and what a trap's
   committed prefix names. Chaining readiness must advance on an element *write*
   (microarchitectural), which is earlier than architectural commit, and must
   never record a speculative or wrong-path packet.
2. **Different payload.** The forwarding path needs the *element data* and the
   *generation the packet came from*; the descriptor holds neither.
3. **They cannot disagree anyway.** When the network drives the descriptor
   (`chain_bind_i`), the descriptor's `elem_done_*` *is* the network's accepted
   packet pulse, and a packet is accepted only when its generation matches the
   group's, its index is inside `vl`, and the macro has not faulted. The parallel
   scoreboard is therefore a strictly earlier view of the same accepted set; the
   case checks the descriptor's bitmap against the host's accepted set, and the
   two agree in every phase.

Where readiness comes from, concretely: `rdq_q` is set by an accepted
`p_wr_*` packet or by a granted `war_*` overwrite. It is read by
`c0_rdy_o`/`c1_rdy_o` (chaining), and compared against the host oracle, never the
other way round. `o_ready_o` exposes the whole 128-bit bitmap and the case reads
it back (the cancellation phase asserts that a stale packet did *not* set bit 3).

## The on/off comparison, measured

`chain_en_i` is a runtime input, so the control is the same RTL with one bit
clear -- not a second implementation and not a rebuild. The phase runs one
program (an 8-element macro, ascending completion, one consumer reading in
element order) twice:

| configuration | read values | descriptor bitmap | accepted packets | cycles | element-granular forwards | stalled cycles | reads before the producer finished |
| --- | --- | --- | --- | --- | --- | --- | --- |
| chaining on (`chain_en_i = 1`) | oracle | `0x00…ff` | 8 | **10** | **8** | 1 | 7 of 8 |
| chaining off (`chain_en_i = 0`) | identical, field for field | identical | 8 | **18** | 0 | 9 | 0 of 8 |

Read honestly: **on this program chaining is 8 cycles faster** (10 vs 18, a 44 %
reduction in the loop's cycles) and every one of the eight reads is served by
element-level forwarding rather than by the whole-macro gate. The control cannot
read anything until the producer's whole macro is done, so its nine stalled
cycles are the mechanism, and 7 of the 8 chained reads are observed before the
producer's `done` — the eighth lands in the same cycle the producer's completion
becomes visible. The architectural result is identical field for field, which is
the card's pass criterion; the numbers are the performance difference, measured
rather than assumed.

Two honest caveats. The cycle count is the driver's loop count for this program
(8 elements), not a throughput claim for a real machine. And the card's "sustained
vector throughput" is I-059/I-060 territory that this package deliberately leaves
(see *Not covered*): the network moves one element per cycle and models no
parallel lanes.

## The hazard construction, element by element

**The macro-level ready bit (the card's first fail mode).** The producer's
elements complete in *descending* order (`7,6,…,0`) and in an *interleaved* order
(`0,2,4,6,1,3,5,7`), while the consumer requests elements in ascending order. In
the descending run the consumer asks for element 0 while only element 7 has been
written; the network must refuse the read. A design with a macro-level ready bit
(`|rdq_q` -- "one element written, the macro is ready") grants it and hands back
an unwritten slot. The mutant build fails on exactly that:
`order descending: an element was read before its producer wrote it`, and the
value it would have delivered is caught by the per-element value check
(`element 0 read 0x0000000000000000 expected 0x4121fefe2121fefe` in the reorder
control, which exercises the same comparison). The host oracle fixes element *i*'s
value as an element-wise 64-bit add of two host arrays, `ChainElemVal(gen,i)`; the
network never computes it, so the comparison is a real check of the forwarding
path.

**The WAR hazard.** Phase `war A`: one consumer has read `0..k` (k = 2) of six
elements. An overwrite of element 4 -- not read -- is refused
(`war A: element 4 was released while the consumer had read only elements 0..2`);
an overwrite of element 1 -- read -- is granted, and the granted overwrite is
shown to land (a later consumer reads the new value out of element 1); the group
as a whole (`src_release_ok_o`) stays held while `3..5` are unread and is released
once they are read. The same phase shows the hold is *scoped*: a writer to a
different group is not blocked, so the hold is not a spurious global lock.

Phase `war B` is the sentence the card turns on: two consumers register on the
same group; consumer 1 reads all six elements and finishes; consumer 0 has read
`0..2`. The release of element 4 is *still refused* -- `war B: element 4 was
released because the other reader finished, while consumer 0 had not read it`.
The `MOSAIC_VEC_CHAIN_MUTANT_WAR_RELEASE` control grants whenever some reader is
free (`!(h0 && h1)` instead of `!(h0 || h1)`) and is caught by the `war A`
assertion first, which is the actual first failure rather than the intended one.

**The wrong-path packet (the card's second fail mode).** Phase `cancel`: a macro
of generation 30 writes elements 0..2 and is then cancelled; its descriptor is
released and a new macro of generation 31 takes the slot. A packet still carrying
generation 30 is offered for element 3. It must be refused, it must not mark
element 3 ready, and the new descriptor's bitmap must stay empty — the check
reads all three. The new producer then writes `0,1,2,4,5,3` while the consumer
reads `0..5`; the consumer's element 3 read must be generation 31's value, and it
must not have been accepted before generation 31 wrote element 3. With the
identity check in place the stale packet is refused, the consumer waits, and the
descriptor's bitmap ends at exactly generation 31's six elements. The
`MOSAIC_VEC_CHAIN_MUTANT_STALE_PACKET` control drops the generation comparison and
is caught on the first of those checks.

The same phase also covers a producer **fault** at element 3: elements 3, 4 and 5
are then refused, the faulted macro's committed elements `0..2` remain readable,
and element 3 is never readable. That is R5 (fault/kill propagated to dependent
packets).

## Coverage, asserted rather than implied

| phase | cells | what must hold |
| --- | --- | --- |
| `order` | 3 orders x 8 elements | every element read after its write, with the oracle's value; the descriptor bitmap is the written set |
| `on/off` | 2 runs x 8 elements | values and bitmap identical; forwards and stalls differ in the intended direction; the control is faster |
| `war` | 2 cells | element-granular release timing, group-scoped holds, another reader's completion is irrelevant |
| `cancel` | 1 cell (+ 1 fault cell) | a stale-generation packet is refused and does not advance the descriptor; a fault stops the elements after it |

The counts are checks at the end of the case (`coverage: producer order …
never ran`, `… WAR cells ran, expected 2`, …), so a phase that silently stopped
running fails instead of being implied.

## The host oracle's identity

`sim/unit/tb_vec.cpp` computes, on the host and from the operation's rules,
element *i*'s payload (`ChainElemVal(gen,i)` = an element-wise add of two host
arrays) and the expected accepted-element set. The case never derives an
expectation from `chain_ready_lo_o`/`chain_ready_hi_o`, from the counters, or
from `o_elem_bitmap_lo/hi` — it only *compares* them. The "read after write"
property is checked independently of the values: the driver records which element
indices it saw accepted as writes and requires an accepted read of element *i* to
post-date it.

## The mutant table

`python3 tools/run_vec_chain_controls.py --profile p0` — the shipping build is
built first from an empty directory and required to pass, then each mutant is
built from its own empty directory with its `-D` on the recorded command line.

| mutant | injects | sha256 | first failing check |
| --- | --- | --- | --- |
| shipping | — | `6667a4a6866851f36ca4ede63af2d6406c8516518649962f9348395f059c9d76` | — (PASS) |
| `MOSAIC_VEC_CHAIN_MUTANT_MACRO_READY` | a macro-level ready bit: one element written makes the whole macro look ready (the card's first fail mode) | `3f4fbc59b509c407f34ebdc2d927c44da1501df78102be0940872f3d32b09d7f` | `order descending: an element was read before its producer wrote it` |
| `MOSAIC_VEC_CHAIN_MUTANT_STALE_PACKET` | the packet identity check is dropped (the card's second fail mode) | `ee36b357f6c92ce994fffafcd4f19ecdc90501257ce09c4df3663b004e8a6eb5` | `cancel: a packet from the cancelled generation 30 was accepted by the new descriptor` |
| `MOSAIC_VEC_CHAIN_MUTANT_WAR_RELEASE` | a WAR release fires because some other reader is free, ignoring the outstanding one | `29c4dde62583a44bf7c404944ecb0a8c6baa27c24ce5f011c67a21f61d044f37` | `war A: element 4 was released while the consumer had read only elements 0..2` |
| `MOSAIC_VEC_CHAIN_MUTANT_REORDER` | the forwarding path returns the neighbouring element's data | `afe823180aaeed601a25c5b9343d744add01b78ae491bca8a58b76716ea0db66` | `order ascending: element 0 read 0x0000000000000000 expected 0x4121fefe2121fefe` |

Each mutant's binary differs from the shipping one (`cmp`), each exits 1, and
each names the check its defect breaks. The `MACRO_READY` mutant is caught by the
read-after-write assertion before the value assertions it would also break — the
table states the actual first failure rather than the intended one. The
`WAR_RELEASE` mutant is caught by the `war A` assertion rather than by the `war B`
one, for the same reason. Verilator embeds a source-line mapping, so a
comment-only edit to any file in the case's compilation changes the binary hash;
the hashes above are from the tree state at the time of writing, and the
*shipping-vs-mutant* inequality is the property the control checks.

## Not covered — stated deliberately

* **The vector unit is still not wired into the core.** `mosaic_core.sv` and
  `mosaic_dispatch.sv` contain zero references to any `mosaic_vec*` module: no
  decoded vector instruction reaches this network, and the descriptor is not
  driven from a real decode. Until that integration exists the vector capability
  is not an advertisement. This package is a unit-level chaining network, exactly
  as I-051..I-057 were unit-level blocks.
* **I-059's territory — runtime lane quota reallocation — is deliberately left.**
  There is no lane broker, no 2/4/8-lane share switch, no `rvv.lane_resize_boundary`
  case, and no lane ownership generation here. The network moves one element per
  cycle and has no notion of a physical lane: chaining is modelled as a readiness
  handshake, not as parallel lane occupancy.
* **I-060's territory — the lane-local buffer (LLB) — is deliberately left.**
  There is no clean-copy locality buffer, no invalidation protocol and no
  `llb.stale_copy_invalidation` case in this package.
* **Sustained throughput is not claimed.** The card lists it as a `Covers` item,
  and the on/off counter is the only throughput-shaped measurement here: a cycle
  delta on an 8-element, single-consumer, one-element-per-cycle program. No
  instruction-per-cycle, no steady-state loop, no bottleneck analysis.
* **The generation identity is driver-supplied.** The packets carry the ROB
  generation, the same identity discipline the store queue, the TLB and the MSHR
  use, and the case keeps the generation it drives equal to the descriptor's
  `alloc_rob_gen` (and checks the descriptor's generation back). The RTL does not
  *take* the generation from the descriptor's `o_macro_rob_gen_o`; a full
  integration would wire that, and this package does not.
* **The WAR arbiter is a separate port with no generation.** `war_*` models the
  register-file's "may I overwrite this element" permission and is scoped by the
  source group; a stale WAR request from a cancelled writer is not refused by a
  generation check (the packet path is where identity is enforced). A real
  machine's source lifetime would be driven by rename/VRF, not by a port.
* **Only two consumer slots are tracked.** A group read by more than two
  concurrent younger macros is not modelled; the holds are two bitmaps, not a set.
* **A consumer reads one group.** The overlap the network knows is
  `consumer_source_group == producer_destination_group`; a consumer reading
  several groups, or a producer writing several destination groups, is not
  modelled. The I-051 descriptor already rules the register-overlap legality
  (its `RSN_OVERLAP_SRC` etc.), and this package relies on that rather than
  repeating it.
* **Interrupts are not modelled.** I-057's restart controller owns the element-
  boundary interrupt handshake; this network has no trap path and the case
  asserts none.
* **`vstart`-relevant chaining is not modelled.** A macro restarted at a non-zero
  `vstart` (I-057) chaining into a successor, or a successor beginning at a
  non-zero element, is not exercised: every run here starts at element 0.
* **Fault-only-first, vector AMO, segments, whole-register order and the `v0`
  mask alias** remain as I-054/I-055/I-056/I-057 left them; the network's
  `p_fault_*` input is a stop, not a fault classification.
* **No power, area, timing or formal claim.** The mutants are dynamic; there is no
  equivalence proof that the chaining and no-chaining configurations reach the
  same architectural state, only the enforced field-for-field comparison of the
  eight elements and the descriptor bitmap on one program.

## Tree state at the time of writing

This package's files are `rtl/core/mosaic_vec_chain.sv` (new),
`sim/tb/mosaic_vec_tb.sv`, `sim/unit/tb_vec.cpp` and
`tools/run_vec_chain_controls.py` (new).

* `python3 tools/run_unit.py --profile p2 --case rvv.chaining_hazards` →
  `RESULT PASS rvv.chaining_hazards checks=184 orders=3 cycles_on=10 cycles_off=18
  fwd_on=8 fwd_off=0 stall_on=1 stall_off=9 pkt_on=8 pkt_off=8 pre_on=7 pre_off=0 cycles=185`, exit 0;
  the same under p0 and at seeds 2, 7 and 12345.
* The seven sibling vector cases and `vrf.mapping_aliases` PASS under p0 and p2
  with the shared wrapper and driver changed.
* `tools/lint_rtl.py --profile p0` and `--profile p2`: 58 sources clean.
  `make lint-slang PROFILE=p2`: exit 0.
* `make check PROFILE=p0` and `PROFILE=p2`: green (exit 0), `check-exclusions`
  included. `tools/check_records.py`: 71 delivered packages, 82 registered cases.
  `tools/check_exclusions.py`: `ok exclusions: 54 registered, 21 open, 33 covered`
  and `ok closure`; `--negative`: `negative controls: 13/13 illegal ledgers
  rejected`.
* **ACT4 (`core.act_dut`, p1) is the one gate this package cannot show green in
  the tree, and the tree is not this package's.** The committed result records
  `applicable=127 generated=127 run=127 passed=127 failed=0` at
  `2026-10-02T15:15:09Z`, and a re-run from the committed driver reproduces it
  exactly:
  `RESULT PASS core.act_dut applicable=127 generated=127 run=127 passed=127 failed=0`.
  Against the *working tree* the same case fails `passed=0 failed=127` (every ELF
  stops with an early trap), because the concurrent V-010 reset-traffic lane has
  uncommitted `BusResetGate` edits in the tree — a new `sim/common/bus_reset_gate.h`
  and that gate applied to `sim/unit/tb_core_act.cpp` and every other
  `sim/unit/tb_core*.cpp`, plus `sim/common/sim_common.h`. The provenance is
  proven, not asserted: `core.act_dut`'s source list has **zero** overlap with
  this package's files, the case is built from exactly the registry's list, and
  rebuilding it with the *committed* `sim/unit/tb_core_act.cpp` (nothing else
  changed) passes 127/127. The same experiment on another case the lane touches,
  `fabric.integrated` with the committed `tb_core_fabric.cpp`, gives
  `RESULT PASS fabric.integrated … arch_identical=yes`. Between those two builds
  the same uncommitted edits made four core cases record `FAIL` and tripped
  `check-exclusions` (which treats a registered case whose latest result is not
  PASS as an unregistered exclusion); that state has since been reverted and the
  gate is green again, which is why the line above reads as it does. I-058 owns
  none of the files involved and its own case is PASS throughout.
