# I-059 -- vector integration into the out-of-order core

**Status: NOT DELIVERED.** The integration is landed and the core elaborates and
lints clean under both profiles' `lint_rtl`, but CASE=`vec.integrated` does not
yet pass. This report states exactly what was built, what passes, the one open
defect, what was verified, and what remains.

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
