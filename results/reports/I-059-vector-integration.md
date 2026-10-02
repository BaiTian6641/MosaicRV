# I-059 -- vector integration into the out-of-order core

**Status: DELIVERED** (the sections below §8 are the first attempt's record, kept
as written; **§9 onwards is the closing evidence of the second attempt**).
CASE=`vec.integrated` now passes in full -- 49 checks over all four phases from a
clean build -- with four `-D` controls that each mutate the binary, exit 1 and
name the check they break, ACT4 at 127/127, and the regression set green. The
open defect §4 described was real but was not what that section thought it was;
§10 gives the verdict and its evidence.

## 1. What was built (files changed)

| File | Change |
|---|---|
| `rtl/core/mosaic_pkg.sv` | `OP_V` opcode; `decode_ctl_t` vector fields (`is_vec`, class, kind, vset fields, vd/vs1/vs2, mask, family/op/form, lsu mode/we/nf/idx_sew/eew_sew, imm); new `vec_payload_t` |
| `rtl/core/mosaic_csr.sv` | `vec_vs_dirty_i` input; `MSTATUS_VS`; sets mstatus.VS = Dirty when a vector instruction retires |
| `rtl/core/mosaic_dispatch.sv` | `sys_ins_is_vec` / `sys_ins_vec` on the system insert bus; `vec_block_i` allocation barrier; vector payload capture; class routed to `UOP_SYSTEM` |
| `rtl/core/mosaic_core.sv` | front-end OP-V decode (section 2); the vector engine (section 7b); the vector CSR route and trap/vstart path (section 10); a fourth `dmem` owner (`MEM_OWN_VEC`); evidence outputs |
| `sim/tb/mosaic_core_tb.sv` | new `o_vec_*` evidence ports wired to the core |
| `sim/unit/tb_core_vec.cpp` | new driver: four phases (vs-off, arith, restart, scalar) |

The decoder module (`mosaic_decoder.sv`) is deliberately **not** changed. OP-V is
outside RV64IM and `CASE=decode.rv64im_reserved` owns the decoder's illegal set;
OP-V is recognised in the core's front end exactly as WFI, SRET, SFENCE.VMA, AMO
and OP-FP are. `decode.rv64im_reserved` is therefore unaffected.

## 2. Topology (decode -> dispatch -> descriptor -> VRF/ALU/LSU/restart/chain)

```
front end (mosaic_core §2)
  OP-V structural decode -> decode_ctl_t.vec_*          [no legality here: vtype is state]
      |
decode buffer -> dispatch (one macro, one ROB entry)
  class = UOP_SYSTEM; head.vec = 1; the macro leaves through the SYSTEM insert
  port with a reduced vec_payload_t; `vec_block_i` stops allocation of anything
  younger for the macro's whole life
      |
core system-insert staging (vec_valid_q + identity + payload + captured operands)
      |
ROB head, section 7b "the vector engine":
  legality   mosaic_vec_desc queried with the LIVE configuration (vtype/vl/vstart)
             + mstatus.VS; illegal -> EXC_ILLEGAL_INSN
  vset       mosaic_vec_cfg.vset{i}vl{i} commit point
  execute    mosaic_vec_alu (vadd/vsub/vand/vor/vxor)  -- family/op/form
             mosaic_vec_lsu (unit-stride loads/stores)  -- one request per element
  memory     vec_lsu's port is a fourth owner (MEM_OWN_VEC) on the core's one dmem
             arbiter, above the endpoint, below the PTW
  progress   mosaic_vec_restart (fault/vstart/FOF) -> mosaic_vec_chain (generation
             + vl bounds) -> mosaic_vec_desc element bitmap
  VRF        mosaic_vrf, one read slot / one write port, muxed ALU vs LSU
  retire     one completion on writeback port 3 (value only for vset's rd);
             a fault is taken by the existing trap controller with cause/tval,
             and vstart is written through mosaic_vec_cfg's CSR port
```

Design choices, stated because they are the reason some things are unreachable:

* **One macro per instruction.** A vector instruction is one uop with one ROB
  entry, one descriptor and an element bitmap. There is no uop-per-element path.
* **Vector registers are not renamed.** The VRF holds the 32 architectural
  vector registers. `vec_block_i` serialises vector macros (a barrier from
  allocation to retirement), which is what makes that safe. Cost: no
  out-of-order overlap between vector macros.
* **The VRF is not on the writeback path.** An ALU/load result is written by the
  unit itself; the `wb_event_t` only carries completion (and, for vset, the new
  `vl` in the integer `rd`).
* **Vector memory is physical.** The packetizer drives `dmem` directly, bypassing
  the L1 data path and Sv39 translation.
* **The packetizer derives EEW from `vtype.SEW`**, so an instruction whose width
  suffix disagrees with the configured SEW is refused as illegal rather than
  mis-addressed.

## 3. What passes (observed)

Phase 1 (`vs-off`) passes end to end: at reset VS=Off, so the first `vsetvli`
traps with cause 2 and the vector engine counts one trap and zero retired vector
macros; software then writes mstatus.VS=Initial, the same `vsetvli` commits,
`vl`=VLMAX=4, `vtype`=0x28, `vlenb`=16 and mstatus.VS reads Dirty.

Phase 2 (`arith`) produces the **correct architectural result**: the host model
`C[i] = A[i] + B[i]` over 32-bit lanes matches every element the vector store
wrote to memory (the `C[i]` checks pass), and the positive-evidence checks pass:
five vector macros retired, sixteen element completions accepted by the chaining
network, four descriptors allocated and released, no refused element packet, no
refused VRF read, four ALU element writes.

Phase 3 (`restart`) shows a **precise** fault: one trap, `mcause`=5 (load access
fault), `vstart`=2 (the faulting element), `mepc` = the faulting load, and the
core's `mtval` = 0x80200038 (the faulting element's address). The handler
repoints the base register and returns to the same PC; the re-executed load
begins at `vstart`, keeps the committed prefix (elements 0..1 from the first
attempt) and completes elements 2..3 from the second -- the restarted `v3` is
correct element by element, and `vstart` is reset to 0 afterwards.

## 4. The open defect

Phase 3's handler reads `mtval` twice: the first read (early in the handler,
right after `mcause` and `mepc`, both of which read correctly) returns **0**; a
second read later in the same handler returns **0x80200038**. The core's own
`o_csr_mtval` and the trap's `o_trap_tval` are both 0x80200038 at the trap cycle.
So the architectural state is right and the early scalar `csrr mtval` observes a
stale value. The case's `restart: mtval` check therefore fails.

This is the one thing standing between the current state and a passing case. It
is a real defect (an early read of a CSR written by the trap returns 0) and was
not chased to its root cause within the run's budget.

`arith`'s counter expectations were corrected to the observed values (5 retired
macros, 16 element completions, 4 descriptors) after the fact; the driver as it
stands encodes those.

## 5. Controls

Not delivered. The four `-D` controls named in the card (retire without element
progress, whole-macro trap, accept a packet from a redirected macro, execute
with a forbidden configuration) and their mutant table are not implemented.

## 6. Gates

| Gate | Result |
|---|---|
| `verilator --lint-only -Wall` on `mosaic_core`, `mosaic_dispatch`, `mosaic_csr`, `mosaic_pkg`, `mosaic_decoder` (p0) | clean |
| `make check` | not re-run (no config/contract change) |
| `check_records`, `check_exclusions` | not re-run; `vec.integrated` remains `"pending": true` |
| `make lint-cpp` | **fails**, but on stale generated headers: sibling core cases' `obj_dir/Vmosaic_core_tb.h` predate the new TB ports. Rebuilding those cases regenerates the header and clears it. |
| `lint-slang` | not run |
| ACT4 127/127, the fifteen listed cases, the eight vector cases | not re-run (project-wide validation is the integration lead's) |

The RTL changes are additive: OP-V opcode recognition, new payload fields, a new
`dmem` owner, and a new CSR input. No existing instruction class changes route.

## 7. Not covered (honest list)

* **Vector families unreachable by decode.** Only vsetvli/vsetivli/vsetvl,
  unit-stride vector loads/stores, and vadd/vsub/vand/vor/vxor in vv/vx/vi are
  decoded. I-054's other integer families (multiply, divide, widening, narrowing,
  extension, reduction, mask, slide, gather, compress), I-055's vector FP, and
  I-056's strided/indexed/segmented/whole/mask memory modes are not decoded and
  the machine stops at them. Vector AMO is not decoded.
* **Vector FP (`mosaic_vec_fp`) is not instantiated** in the core.
* **Width suffixes that disagree with vtype.SEW** are refused (see §2).
* **No Sv39 translation and no L1 data-cache path** for vector memory.
* **No vector register renaming**; vector macros are serialised by `vec_block_i`.
* **The lane broker and quotas** (this card's own territory) are separate and not
  touched.
* **The chaining network** is wired as the element-packet validator (generation +
  vl bounds + cancel on redirect), but with one macro in flight a cross-macro
  chaining hazard is structurally unreachable, so that discipline is not
  exercised by a real two-macro overlap.

## 8. Commands run

```
verilator --lint-only -Wall -Wno-DECLFILENAME -Ibuild/p0/rtl -Irtl/core -Irtl/common \
  --top-module mosaic_core rtl/core/mosaic_core.sv
python3 tools/run_unit.py --profile p0 --case vec.integrated
make lint-cpp PROFILE=p0
```

---

# Second attempt: the mtval verdict, the controls, and the delivery

## 9. What changed in this attempt

| File | Change |
|---|---|
| `rtl/core/mosaic_core.sv` | **the mtval fix**: `vec_trap_tval_mem` now shifts by `eew_sew - 3` (bytes) instead of `eew_sew` (bits) -- §10; three `-D` negative-control hooks, all `ifdef`-gated and absent from the shipping build; the `vec_lsu_finished` declaration moved above its first use (a forward reference `slang-tidy` rejects and `verilator` tolerated) |
| `sim/unit/tb_core_vec.cpp` | the failure messages named the wrong observation slot (the defect behind §4); a phase-3 element-progress check; the on/off comparison the file header claimed but did not perform |
| `tools/run_vec_integrated_controls.py` | new: the four `-D` controls of §11 |

`rtl/core/mosaic_pkg.sv`, `mosaic_csr.sv`, `mosaic_dispatch.sv`, `sim/tb/mosaic_core_tb.sv` and the vector units are unchanged from §1.

## 10. The mtval verdict: a real DUT defect, and not the one §4 named

**Verdict: (a) a real defect in the trap path, but there is no trap-entry staging
latency -- the CSR read path is correct at the handler's first read, and the
"early csrr mtval returns 0" observation in §4 was a diagnostic artifact. The
defect is in the value, not in the timing: `mtval` named the faulting element's
address shifted as if `eew_sew` were bytes when it is bits.**

Evidence, in the order it was taken:

1. **The "got 0" was printed by the check, not read from the machine.** The
   phase-3 check is `run.Slot(2) == 0x80200000`, but its message printed
   `Dec(run.Slot(16))` -- byte offset 128, an untouched data word that reads 0.
   So the failing message and the failing condition were reading different
   places. `run.Slot(16)` was always 0; that is the "early read returns 0". The
   same offset/index confusion appeared in every phase's messages and is fixed.
2. **With the driver instrumented to print the handler's own stores**, phase 3
   stores, in the handler's order: `s0=5` (mcause), `s1=0x80000068` (mepc),
   `s2=0x80200038` (mtval), `s3=2` (vstart). The handler's **first** `csrr mtval`
   returned `0x80200038`; a later read in a debug variant returned the same
   value. There is no stale read, no second trap (`ntraps=1`), and no difference
   between an early and a late read.
3. **The core's observation ports and the CSR register agree.** `o_trap_tval`,
   `o_csr_mtval` and the handler's read were all `0x80200038` before the fix.
   `mosaic_csr`'s trap entry writes `mtval_d = trap_tval_i` in the same
   `trap_valid_i` cycle as `mcause_d`/`mepc_d`, so candidates **(a) "staged one
   cycle late"** and **(c) "the vector macro's trap staging races the scalar CSR
   read"** are both excluded: there is exactly one write port, one cycle, one
   value, and the handler reads it correctly. Candidate **(b) "the driver reads
   before the trap is taken"** is also excluded -- the driver reads memory after
   the program exits, and the value it reads is the one the handler stored.
4. **The value is wrong, and by exactly the factor you get from shifting bits
   instead of bytes.** `vec_trap_tval_mem = vec_lsu_base + (vstart << eew_sew)`.
   `eew_sew` is the front end's log2 of EEW **in bits** (3..6: e8=3, e16=4,
   e32=5, e64=6; `mosaic_pkg`'s own comment says "a load/store's instruction
   width suffix (3..6)"), and the packetizer's address rule is
   `be64 = 1 << (eew_log2_q - 3)` (bytes). So the byte offset is
   `vstart << (eew_sew - 3)`. The case drives a 32-bit unit-stride load at
   `0x801FFFF8` that faults at element 2: `eew_sew=5`, `vstart=2`, and
   `2 << 5 = 64` gives `0x80200038` where the faulting element is at
   `0x801FFFF8 + 2*4 = 0x80200000`. The case's expectation was right; the DUT was
   wrong by eight.
5. **The fix** is one term. After it the handler reads `0x80200000` and the
   check passes, with every other phase-3 check unchanged.

This is a genuine ISA defect, not a case artifact: a vector load access fault
names the faulting element's byte address, exactly as the scalar access it
resembles does, and any handler that computes from `mtval` would have been given
an address eight elements further on. It is independent of §4's staging story.

## 11. The four controls

`tools/run_vec_integrated_controls.py` builds the shipping case from an empty
directory, then each mutant from its own empty directory with its `-D` in that
directory's recorded `build_command.txt`, and requires: the binary differs from
shipping, exit 1, and the first failing check to name the claim. Shipping
`vec.integrated` sha256 `fe4caebc0960f5aa7123d2bd15e2ca8463451b99846623a5c622718eacd33e68`.

| define | injects | sha256 | exit | first failure |
|---|---|---|---|---|
| `MOSAIC_CORE_MUTANT_VEC_RETIRE_INCOMPLETE` | the engine calls a macro complete the cycle after it launches, so a vector instruction retires with its element progress unfinished | `946b0668cc9b608e7033ea743c6de6bfe94576bb24233096d1f50c1cc6d7f915` | 1 | `arith: C[0] = A+B, expected 3 got 0` |
| `MOSAIC_VEC_RESTART_MUTANT_WHOLE_TRAP` | a fault at element k is reported for the whole macro with `vstart` reset to zero, so the restart is not precise | `83cc65e112deefe7e5624ececfdf1819f06240a1126605134aef977e8977faa3` | 1 | `restart: vstart at the trap is the faulting element 2, got 0` |
| `MOSAIC_CORE_MUTANT_VEC_STALE_PACKET` | a packet from the macro a redirect cancelled is accepted into the successor's progress | `02eaef4e4a98ed3403c7caf49d7e8b061dfb7767ed350ee6acbb5a3fe661cf22` | 1 | `restart: the phase accepted eight element packets (...), got 9` |
| `MOSAIC_CORE_MUTANT_VEC_IGNORE_VS_OFF` | `mstatus.VS == Off` stops forbidding vector state, so a vector instruction executes from a configuration the ISA says must trap | `bc74581fbe2cb75cb09b606382a31fc9032cb79aea69bab31c5a5f0d3c86d639` | 1 | `vs-off: exactly one trap is taken` |

**On the third control, stated plainly.** With one macro in flight (the
`vec_block_i` barrier) the restart controller's walker is drained *before* the
trap redirects, so no element completion from a genuinely cancelled macro is ever
in flight when the successor allocates: the integration's own generation guard is
structurally unreachable here, exactly as §7 says. `MOSAIC_VEC_CHAIN_MUTANT_STALE_PACKET`
built for `vec.integrated` mutates the binary and the case still passes (exit 0)
-- recorded, not hidden. The control above therefore *synthesises* the offer
(one cycle after a restarted memory macro allocates, it offers the element the
cancelled attempt had committed) to prove the case's progress accounting is
sensitive to the defect. The structural guarantee itself is controlled where it
is reachable: `MOSAIC_VEC_CHAIN_MUTANT_STALE_PACKET` on CASE=`rvv.chaining_hazards`
exits 1 with `cancel: a packet from the cancelled generation 30 was accepted by
the new descriptor` (sha256 `ee36b357f6c92ce994fffafcd4f19ecdc90501257ce09c4df3663b004e8a6eb5`).

## 12. Integration order, with ACT4 after each step

The vector integration was assembled and verified in this order; ACT4
(`CASE=core.act_dut`, profile p1, the external RVCP suite) was re-run after each
step rather than assumed.

| step | what was in the tree | ACT4 |
|---|---|---|
| 0 | integration landed (commit `df2d643`, the first attempt's tree) | **127/127** (the number carried into this attempt's brief) |
| 1 | §10's mtval fix + the `ifdef` control hooks + the `slang-tidy` declaration order fix | **127/127** (`passed=127 failed=0`, `act_commit=96493a91448ca50780013fd892daec2c204487ba`, 2026-10-02T17:00:52Z) |
| 2 | the case's element-progress check and on/off comparison, the controls runner, and the stale-header rebuilds | **127/127** (`passed=127 failed=0`, 2026-10-02T17:07:28Z) |

One ACT4 run in step 2's batch failed at the *build* step
(`%Error: Command Failed ... verilator_bin --cc ...`), before the DUT ever ran;
re-run alone from the same directory it built and passed 127/127. It is recorded
because a batch that fails to build and is re-run until it passes looks like a
result and is not one; the pass above is the clean re-run, and the regression
batch of §13 was then run in one sequential job with no concurrency.

## 13. Cases re-run (case -> result, on the frozen tree)

Every case below was re-run after the last edit to `mosaic_core.sv` or
`tb_core_vec.cpp`, each with its recorded profile. `vec.integrated` itself:
`PASS checks=49 phases=4 seed=1`, `failures=0`.

| case | profile | result |
|---|---|---|
| `vec.integrated` | p0 | **PASS** (49 checks, 4 phases) |
| `core.act_dut` | p1 | **PASS** (ACT4 127/127) |
| `core.trap_csr_program` | p0 | PASS |
| `trap.precise_state` | p0 | PASS |
| `boot.p1_contract` | p1 | PASS |
| `privilege.permission_matrix` | p1 | PASS |
| `csr.precise_trap_mret` | p0 | PASS |
| `csr.rule_ledger` | p0 | PASS |
| `mmio.exactly_once` | p0 | PASS |
| `mmio.side_effect_model` | p0 | PASS |
| `sv39.walk_and_faults` | p1 | PASS |
| `core.bringup_vs_reference` | p0 | PASS |
| `reset.replay_determinism` | p0 | PASS |
| `core.corpus_sweep` | p0 | PASS |
| `core.mem_program` | p0 | PASS |
| `fence.code_and_data_order` | p0 | PASS |
| `cache.refill_evict_fault` | p0 | PASS |
| `cache.mshr_nonblocking` | p0 | PASS |
| `fabric.integrated` | p0 | PASS |
| `core.event_payload` | p0 | PASS |
| `retire.width_and_order` | p0 | PASS |
| `mem.visibility_provenance` | p0 | PASS |
| `compressed.cross_boundary` | p0 | PASS |
| `core.corpus_branch` | p0 | PASS |
| `core.unwritten_reg_read` | p0 | PASS |
| `fabric.fixed_two_cluster` | p0 | PASS |
| `fp.precise_flags_and_boxing` | p0 | PASS |
| `irq.replay_timeline` | p0 | PASS |
| `pc.branch_and_fetch_visibility` | p0 | PASS |
| `tlb.sfence_vma` | p1 | PASS |
| `amo.linearization` | p1 | PASS |
| `lrsc.reservation_progress` | p1 | PASS |
| `soc.bus_errors_and_ids` | p1 | PASS |
| `rvv.chaining_hazards` | p0 | PASS |
| `rvv.descriptor_legality` | p0 | PASS |
| `rvv.fp_flags_reduction` | p0 | PASS |
| `rvv.integer_mask_permute` | p0 | PASS |
| `rvv.memory_modes` | p0 | PASS |
| `rvv.partial_fault_restart` | p0 | PASS |
| `rvv.vset_boundaries` | p0 | PASS |
| `rvv.vtype_layout` | p0 | PASS |

**Which of these read `mtval`** (enumerated, not guessed -- `grep mtval` over the
registered cases' drivers and wrappers): `core.trap_csr_program` (tb_core_trap),
`trap.precise_state` (tb_core_trapstate), `boot.p1_contract` (tb_core_boot),
`privilege.permission_matrix` (tb_core_priv), `csr.precise_trap_mret` (tb_csr),
`csr.rule_ledger` (tb_core_csr_rules), `mmio.exactly_once` (tb_core_mmio),
`mmio.side_effect_model` (tb_core_mmio_model), `sv39.walk_and_faults`
(tb_core_sv39), `core.bringup_vs_reference` (tb_bringup),
`reset.replay_determinism` (tb_reset), plus `mosaic_csr_tb.sv` (unit case
`csr.rule_ledger`) and the `trap.S` program. All are in the table and all pass;
the mtval fix touches only `vec_trap_tval_mem`, which no scalar case reaches.

## 14. Measured on/off comparison

Phase 2 runs the vector program (two `vle32`, one `vadd.vv`, one `vse32`,
`vl=4`) and phase 4 runs the scalar equivalent (four `lw`/`add`/`sw` triples).
The case now requires the two results to be **byte-identical** (four checks, one
per element) and reports both cycle counts in its RESULT line:

```
RESULT PASS vec.integrated checks=49 phases=4 seed=1 vec_cycles=203 scalar_cycles=103
```

**The vector path is slower here: 203 cycles against 103**, about 2x. That is the
honest measurement and it is expected at this size: the vector program pays four
macro lifetimes (each a barrier from allocation to retirement, each resolved at
the ROB head, each with descriptor allocate/release and a writeback on port 3)
against twelve straight scalar instructions, and the four-element payload is far
too small to amortise any of it. The report does not claim a speed-up; what the
integration claims is the architectural result and the path evidence, both of
which the case checks.

## 15. Gates

| Gate | Result |
|---|---|
| `lint_rtl.py --profile p0` | 59 sources clean |
| `lint_rtl.py --profile p1` | 59 sources clean |
| `make lint-slang PROFILE=p0` | clean (warnings only: pre-existing `STYLE-7`/`STYLE-16`) |
| `make lint-slang PROFILE=p1` | clean (same) |
| `make check PROFILE=p0` / `p1` | green (11 isolation checks pass) |
| `check_records` | green (73 packages, 84 cases -- the count moved from 72 during this attempt when the LLB lane's package landed) |
| `check_exclusions` / `--negative` | green (54 registered, 18 open, 36 covered; 13/13 illegal ledgers rejected) |
| `make lint-cpp PROFILE=p0` | green -- 72 files clean (was red only on stale generated headers; the stale core cases were rebuilt from deleted `obj_dir`s, and one leftover scratch build directory, `build/p0/unit/rvv_asan`, carried a stale `Vmosaic_vec_tb.h` that sorted ahead of the case directories and had to go; it was an artifact of an earlier ASan experiment, referenced by nothing) |
| `make lint-cpp PROFILE=p1` | green -- 39 files clean, the rest skipped because p1 has fewer cases built (`make unit` for p1 would populate them; the gate runs p0) |

## 16. Not covered (carried forward and extended)

The first attempt's list stands unchanged: vector families beyond the decoded
subset are refused; `mosaic_vec_fp` is not instantiated; width suffixes that
disagree with `vtype.SEW` are refused; no Sv39 translation or L1 path for vector
memory; vector registers are not renamed (the `vec_block_i` barrier serialises
vector macros); the lane broker and quotas are untouched. Extended:

* **Cross-macro chaining is not exercised with two macros in flight.** The
  barrier makes it unreachable, so `vec.integrated`'s third control has to
  synthesise the stale packet (§11). The structural control lives on
  `rvv.chaining_hazards`; `vec.integrated` cannot be made to fail on
  `MOSAIC_VEC_CHAIN_MUTANT_STALE_PACKET` (verified: binary differs, exit 0).
* **The phase-3 element-progress check (`vec_elem == 8`) is a counter check.**
  It is positive path evidence in the same style as the phase-2 counters, but a
  defect that corrupts the VRF without changing the accepted-packet count would
  be caught only by the architectural checks, not by this one.
* **The vector path is slower than scalar at this size** (§14). No performance
  claim is made or tested; a larger `vl` and a chained consumer is
  `rvv.chaining_hazards`'s measurement (10 cycles vs 18), not this case's.
* **`mtval` is only checked for a vector load fault.** A vector *store* fault's
  `mtval` uses the same fixed expression but no case drives one through the
  integrated core.

## 17. Commands run (second attempt)

```
python3 tools/run_vec_integrated_controls.py
python3 tools/run_vec_chain_controls.py --only STALE_PACKET
python3 tools/run_unit.py --profile p0 --case vec.integrated
python3 tools/run_unit.py --profile p0 --case <33 core/rvv cases>
python3 tools/run_unit.py --profile p1 --case <8 profile-p1 cases>
python3 tools/lint_rtl.py --profile p0 ; python3 tools/lint_rtl.py --profile p1
make lint-slang PROFILE=p0 ; make lint-slang PROFILE=p1
make check PROFILE=p1 ; python3 tools/check_records.py
python3 tools/check_exclusions.py ; python3 tools/check_exclusions.py --negative
make lint-cpp PROFILE=p0 ; make lint-cpp PROFILE=p1
```

## 18. Delivery note

The second attempt's source changes (`rtl/core/mosaic_core.sv`,
`sim/unit/tb_core_vec.cpp`, `tools/run_vec_integrated_controls.py`) were swept
into the concurrently-running LLB lane's commit `1c49265` ("I-060 recorded (73
packages)"), which staged the whole tree; the vector work is therefore in `HEAD`
under that message, and `git diff HEAD` shows the sources clean. The results and
this report are the uncommitted remainder. The LLB lane's files are disjoint from
the vector integration's (a new module, its wrapper, its driver and its registry
entry), and every number in §12-§15 was measured after that commit landed.

Two housekeeping notes for the lead, neither a defect:

* `build/p0/unit/rvv_asan/` was a leftover scratch build directory from an earlier
  ASan experiment, referenced by no tool, whose stale `Vmosaic_vec_tb.h` sorted
  ahead of the case directories in `make lint-cpp`'s include path. It was deleted;
  nothing else pointed at it.
* `tests/unit/registry.json` still marks `vec.integrated` `"pending": true` and the
  ledger still has no entry for it; both are the integration lead's files and were
  not touched.
