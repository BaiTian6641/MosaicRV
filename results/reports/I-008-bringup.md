# I-008 — Simplified scalar semantic path as bring-up comparison

**Status: the case passes; one acceptance criterion is not met and is not
claimed.** `core.bringup_vs_reference` prints `PASS` and exits 0, `lint_rtl.py`
is clean, and four of the five mutation controls are demonstrated to fail with
exit 1. `MOSAIC_BRINGUP_MUTANT_4` **cannot** fail, and the reason is a defect in
the control rather than in either implementation; §8 proves it. That one
criterion is therefore INCOMPLETE and this report does not paper over it.

| File | State |
|---|---|
| `rtl/core/mosaic_bringup_core.sv` | complete microarchitecture, `-Wall` clean in the shipping build and in all five mutant builds |
| `sim/tb/mosaic_bringup_tb.sv` | complete, `-Wall` clean, owns the memory model; **unchanged in this pass** — the missing cause-7 trap was not a PMA defect (§7.2) |
| `sim/unit/tb_bringup.cpp` | harness + embedded reference model; one reference defect (§7.3), three harness defects and two probe-fixture defects fixed (§8.2, §9) |
| `results/reports/I-008-bringup.md` | this file |

No commit was made. This pass changed `rtl/core/mosaic_bringup_core.sv`,
`sim/unit/tb_bringup.cpp` and this report, and regenerated the case artefacts
under `results/unit/core.bringup_vs_reference/`. The working tree also carries
unrelated modifications (`README.md`, `docs/*`, `tools/check_upstream.py`) that
this work package did not touch.

---

## 1. What this module is, and is not

`mosaic_bringup_core` is a single-issue, in-order, non-pipelined,
non-speculative machine-mode RV64IM_Zicsr_Zifencei core whose only product is an
architectural retire stream that can be compared, event by event, against an
independent model.

**It is not the MosaicRV processor.** There is no branch prediction, no
speculation, no out-of-order execution, no rename, no ROB, no cache, no store
buffer and no pipeline. `misa` reports MXL=2, I and M — and nothing else.
`docs/stage-0-contracts-bringup.md` `### I-008` names the blocking risk exactly:
reporting processor-RTL completion on the strength of a software or simplified
path. Nothing in this work package does that, and no performance, occupancy or
hazard number may be taken from this core.

### Deliberate simplifications, stated

* **No forwarding at all.** The register file is read combinationally in the
  execute state and written on the clock edge that closes the committing state,
  so every operand is the architecturally committed value. That is what makes the
  machine in-order by construction rather than by enforcement.
* **`fence` and `fence.i` retire as architectural no-ops.** This machine has no
  cache and no store buffer, so there is nothing for either to order. This is a
  simplification of the *ordering* machinery and is safe only because there is no
  reorder to suppress.
* **Direct-mode `mtvec` only.** `mtvec[1:0]` is WARL and is forced to `00` on
  every write, so a write of `01` (Vectored) reads back as `00`. Vectored mode is
  therefore **not** supported and **not** tested.
* **`time` reads as zero.** `config/csr/mode_m.json` declares it fixed at reset 0
  and p0 has no timer interrupt logic, so there is no `mtime` to shadow.
* **CLINT is a plain readable/writable region** in the testbench, for the same
  reason.
* **The M extension is implemented inline in the bring-up core**, completely
  (all eight operations, plus divide-by-zero and signed overflow), because
  `rtl/core/mosaic_muldiv.sv` does not exist in this tree. It is a combinational
  stand-in for the bring-up path, not the shared iterative unit p0's real fabric
  will use, and it is labelled as such in the module header. A reference path
  that shared its datapath with the DUT it checks would be a weaker reference.
* **Decode and the integer ALU are private to the bring-up core** rather than
  instantiated from `mosaic_decoder.sv` / `mosaic_alu.sv`. This was permitted
  explicitly and is a deliberate choice: the reference path has to be readable
  and auditable as one file, and a second, independently written decode is
  exactly what a differential comparison wants. It uses `mosaic_pkg`'s types
  (`decode_ctl_t`, `alu_op_e`, `md_op_e`, `mem_kind_e`, `csr_op_e`, the `EXC_*`
  codes) throughout, and it defines a duplicate of no `mosaic_pkg` type.

## 2. Microarchitecture

Five states, one instruction in flight, one commit point:

| State | Work |
|---|---|
| `S_FETCH` | post the fetch request at `pc`; take instruction-address-misaligned (cause 0) if `pc[1:0] != 0` |
| `S_FWAIT` | wait for `ifetch_ack`; a fetch fault is cause 1 |
| `S_EXEC` | decode + execute + commit, combinationally apart from the writes made on the closing edge |
| `S_DREQ` | post the data request for the load/store latched by `S_EXEC` |
| `S_DWAIT` | wait for `dmem_ack`; a fault is cause 5 (load) or 7 (store); on ack with no fault the instruction commits |

There is no `S_COMMIT` state: with one instruction in flight there is nothing to
wait for between "execute finished" and "commit". Every architectural register
has exactly one writer (the single `always_ff`), and every architectural event
is emitted from one place, so the commit point is unambiguous.

**Where a stall can come from — exhaustively:** `ifetch_ack_i` not asserted
(`S_FWAIT`), `dmem_ack_i` not asserted (`S_DWAIT`), or `rst_i`. There is no
hazard unit, no structural interlock, no backpressure path and no arbitration,
because there is exactly one requester per port and one instruction per machine.

**Costs.** A non-memory instruction is 3 clocks; a memory instruction is 5.

### Memory interface

Declared in full in the module header, so the harness author can implement the
other side from that text. One outstanding request per port; a request is
asserted for exactly one cycle and nothing else is accepted until the ack. Two
contract points the environment must honour: a store to `boot_rom` faults even
though the region is mapped and readable (the write is not performed), and the
core performs every misalignment check itself and never issues a misaligned
request, so a permissive environment that assembles an unaligned access from
adjacent bytes is a conforming environment.

## 3. Trap rules implemented

| Cause | Raised by | `mtval` |
|---|---|---|
| 0 instruction address misaligned | `pc[1:0] != 0` at fetch | `pc` |
| 1 instruction access fault | `ifetch_fault_i` | `pc` |
| 2 illegal instruction | 16-bit instruction; reserved encoding; CSR number absent from `config/csr/mode_m.json`; write to a read-only CSR entry | instruction bits (`ir[15:0]` for a 16-bit instruction) |
| 3 breakpoint | `ebreak` | `pc` |
| 4 load address misaligned | load whose address is not a multiple of its width | the address |
| 5 load access fault | `dmem_fault_i` on a load | the address |
| 6 store/AMO address misaligned | store whose address is not a multiple of its width | the address |
| 7 store/AMO access fault | `dmem_fault_i` on a store, including a store to `boot_rom` | the address |
| 11 ecall from M-mode | `ecall` | 0 |

Trap entry writes `mepc`, `mcause`, `mtval`, does `MPIE ← MIE`, `MIE ← 0`, and
jumps to `mtvec`. `MPP` is fixed at `11` by the CSR file, so there is no privilege
transition to perform. `mret` sets `MIE ← MPIE`, `MPIE ← 1`, leaves `MPP` at its
WARL value and jumps to `mepc`.

**A trap is never reported as an ordinary retire.** `evt_has_rd_o` is 0,
`evt_is_store_o` is 0, `evt_next_pc_o` is the trap vector, and cause/tval/epc
are populated. `MOSAIC_BRINGUP_MUTANT_5` breaks exactly this rule.

## 4. CSR file

`config/csr/mode_m.json` is the source of the table. `mstatus` reset `0x1800` and
`misa` reset `0x8000000000001100` are transcribed from that file and **re-read
from it at run time** by `tb_bringup.cpp`, which fails the case by name if either
literal stops matching.

| CSR | Number | Behaviour |
|---|---|---|
| `mstatus` | 768 | reset `0x1800`; write mask bits 3 and 7; `MPP` fixed `11` on read and write; SIE/SPIE read 0 (p0 has no S-mode — Privileged Spec v1.12 requires it; this is the one place the JSON's `writable_fields` list is reconciled, and it is WARL-legal) |
| `misa` | 769 | `0x8000000000001100`, read-only (`writable_fields` is empty) |
| `medeleg` `mideleg` | 770 771 | full 64-bit storage |
| `mie` `mip` | 772 836 | write mask bits 7 and 3 |
| `mtvec` | 773 | `BASE[1:0]` forced to `00` (Direct only) |
| `mcounteren` | 774 | read-only 0 |
| `mscratch` `mepc` `mcause` `mtval` | 832–835 | full 64-bit storage; `mepc[1:0]` forced 0 |
| `mvendorid` `marchid` `mimpid` `mhartid` | 3857–3860 | read-only fixed 0 |
| `mcycle` `minstret` | 2816 2818 | writable counters; `mcycle` increments every clock, `minstret` every committed retire |
| `cycle` `instret` | 3072 3074 | read-only shadows of `mcycle` / `minstret` |
| `time` | 3073 | read-only 0 |

**Illegal-CSR rule.** The legal set is exactly the table's keys. An access to any
number outside it raises illegal instruction (cause 2) with `mtval` = the
instruction bits. A register is writable exactly when its entry has at least one
writable field and is not `ro`; writing a read-only register raises illegal
instruction. Both rules are exercised by directed probes (§7).

**Zicsr decode.** `funct3` in the CSR space is *not* the OP/OP-IMM `funct3`, so
the decode table uses its own literals (`001/010/011` register forms,
`101/110/111` immediate forms, `100` reserved) rather than the `mosaic_pkg`
`F3_*` names. That distinction is not cosmetic: it was the cause of defect 9.1.

**Read/write suppression.** `csrrw` with `rd == x0` does not read; `csrrs`/`csrrc`
with `rs1 == x0` do not write. Both forms, register and immediate, are covered by
one expression because the immediate forms carry a zero-extended `zimm` in the
`rs1` field.

## 5. Event format

Exactly `mosaic::RetireEvent::Line()`, so `EventTap::Compare` and a plain `diff`
both work:

```
hart=0 seq=0000000000011590 pc=... next_pc=... insn=...
  [rd=x<n> val=...]
  [TRAP cause=... tval=... epc=...]
  [STORE addr=... data=... size=<bytes>]
```

`seq` is per-hart and restarts at 0 for every run. `size` is the access width **in
bytes**; the memory interface carries the encoded form, and both spellings exist
so that neither is guessed at by a consumer.

## 6. Differential comparison method

`sim/unit/tb_bringup.cpp` runs every ELF in `tests/programs/build` and, for each:

1. loads it with `sim/common/elf_loader.h` and **refuses any non-`kOk` status**;
2. checks the entry point equals the profile's reset vector;
3. pushes the image through the testbench's word-granular loader and runs the
   DUT, recording every architectural event into a `mosaic::EventTap`;
4. writes the same segments to a manifest and runs the **independent reference
   model** to produce the expected stream;
5. compares the two with `EventTap::Compare` — event count, then line for line;
6. compares `TOHOST` and the four signature words against the reference, not
   against the DUT.

**The oracle.** A second implementation of RV64IM_Zicsr_Zifencei machine mode,
written from the RISC-V ISA manual and Privileged Specification v1.12, in Python,
reading `config/memory/p0.json`, `config/csr/mode_m.json` and
`config/profiles/p0.json` directly. It shares no code with the design under test,
including `mosaic_pkg`; its opcode constants are written out by hand. It has no
access to the RTL, to the simulator or to the DUT's outputs. Its memory model is
byte-granular and sparse; an address no region covers is an access fault, not a
silent zero; it honours no misalignment policy, because the frozen profile assigns
that to the hart. Its CSR legality and writability come from the JSON, not from a
hand-written list.

It is embedded as a string in `tb_bringup.cpp` and materialised into the case
output directory, so the oracle cannot drift away from the harness that depends
on it.

**Result.**

```
$ python3 tools/run_unit.py --case core.bringup_vs_reference ; echo "EXIT=$?"
PASS core.bringup_vs_reference    task=I-008
EXIT=0

$ grep '^RESULT' results/unit/core.bringup_vs_reference/run.log
RESULT PASS core.bringup_vs_reference corpus 39 programs, 2774346 architectural events, every stream identical to the independent reference
```

All 39 streams are byte-identical, verified independently of the harness:

```
$ python3 - <<'EOF'
import glob
d='results/unit/core.bringup_vs_reference'
n=bad=0
for f in sorted(glob.glob(d+'/p??_*.events.txt')):
    if '.reference.' in f: continue
    n+=1
    if open(f,'rb').read()!=open(f.replace('.events.txt','.reference.events.txt'),'rb').read():
        bad+=1; print('DIFFERS', f)
print(f"{n} corpus streams compared byte-for-byte, {bad} differ")
EOF
39 corpus streams compared byte-for-byte, 0 differ
```

The comparison has now surfaced **twelve** defects that a self-consistency check
would have accepted: six in the core, one in the testbench's memory model,
three in the reference model (two of them the same
mapping-by-mnemonic-name-instead-of-by-encoding mistake as in the core, and the
`mret` omission of §7.3), one in the probe fixture, and three in the harness.
The full list, with the evidence that located each, is §9.

---

## 7. The two blocking defects, root-caused

### 7.1 `csrrw sp, mscratch, sp` never wrote `mscratch` — **the DUT was wrong**

**Symptom.** `p08_misaligned.i0` diverged from the reference at event 71125 and
then took 2 traps where the reference took 5:

```
$ diff results/unit/core.bringup_vs_reference/p08_misaligned.i0.events.txt \
       results/unit/core.bringup_vs_reference/p08_misaligned.i0.reference.events.txt | head -4
71126c71126
< hart=0 seq=00000000000115d5 pc=0000000080000250 ... insn=34011173 rd=x2 val=0000000080006480
---
> hart=0 seq=00000000000115d5 pc=0000000080000250 ... insn=34011173 rd=x2 val=0000000080006080
```

`34011173` is `csrrw sp, mscratch, sp` (`funct3` 001, `rd = x2`, `rs1 = x2`). It
appears twice in `tests/programs/src/trap.S`, at the handler's entry (line 115)
and its exit (line 185); both encode identically.

**Discriminator (as proposed).** `Dut::ReadCsr(0x340)` was called immediately
after each `csrrw` retire:

```
PROBE pc=0000000080000160 mscratch_after=0000000080006480
PROBE pc=0000000080000250 mscratch_after=0000000080006480
PROBE pc=0000000080000160 mscratch_after=0000000080006580
PROBE pc=0000000080000250 mscratch_after=0000000080006580
```

`mscratch` still held `0x80006480` — the value `crt0` wrote at boot — after the
handler's **entry** `csrrw`. So **the write did not land at all**; it did not
land with a wrong operand.

**Which side was wrong, and how that was established.** `riscv64-elf-nm` on
`tests/programs/build/p08_misaligned.i0.elf` gives
`mosaic_trapstack_top = 0x80006480` and `mosaic_stack_top = 0x80006080`;
`tests/programs/src/crt0.S` does `csrw mscratch, t0` with `t0 =
mosaic_trapstack_top`. The interrupted `sp` at the first trap was `0x80006080`
(both streams agree up to the divergence, and the reference's exit `csrrw` reads
back exactly that). So `csrrw sp, mscratch, sp` must leave `mscratch = 0x80006080`.
The DUT left it at `0x80006480` and the reference produced `0x80006080`:
**the DUT is wrong.**

**Root cause.** A `$display` on the commit edge, gated on `csr_addr == 0x340`,
settled it in one run:

```
I008DBG pc=0000000080000160 op=2 w=1 wd=0000000080006480 rs1v=0000000080006080 cw=0000000080006480 mq=0000000080006480 mn=0000000080006480
```

`rs1v` is the correct operand (`0x80006080`) and `csr_writes` is 1, but `csr_wdata`
is `0x80006480` — the *old* CSR contents. `op=2` is `CSR_RS`, and
`CSR_RS` computes `csr_wdata = csr_rdata | csr_operand`, which is
`mscratch | sp = 0x80006480 | 0x80006080 = 0x80006480`: writing the register back
onto itself.

`csrrw` decoded as `csrrs`. The cause is the SYSTEM `funct3` table:

```systemverilog
// before
} else if (funct3 == F3_SLTU) begin
  d = illegal_op();     // comment claims funct3 == 100 is reserved
end else begin
  case (funct3)
    F3_ADD_SUB: d.csr_op = CSR_RW;   // 000
    F3_SLL:     d.csr_op = CSR_RS;   // 001  <- csrrw lands here
    F3_SLT:     d.csr_op = CSR_RC;   // 010  <- csrrs lands here
    F3_SLTU:    ... csr_imm_form = 1; // 011  <- csrrc is trapped as illegal
    F3_XOR:     ... csr_imm_form = 1; // 100  <- the reserved encoding is accepted
    F3_SRL_SRA: ... csr_imm_form = 1; // 101  <- csrrwi lands here
    default:    d = illegal_op();     // 110, 111 -> illegal
```

`F3_*` are the **OP/OP-IMM** names in `mosaic_pkg`
(`F3_ADD_SUB = 000, F3_SLL = 001, F3_SLT = 010, F3_SLTU = 011, F3_XOR = 100,
F3_SRL_SRA = 101`), and the Zicsr space is offset by one from them. The whole
table was shifted: `csrrw` became `csrrs`, `csrrs` became `csrrc`, `csrrc`
(funct3 011) was trapped as illegal, the reserved encoding 100 was accepted, and
`csrrsi`/`csrrci` were illegal. The guard clause also tested `F3_SLTU` (011)
while its own comment said 100 — the same off-by-one in a second place.

The reference decodes the same instructions correctly
(`csr_op = {1: CSR_RW, 2: CSR_RS, 3: CSR_RC}[funct3 & 3]`), which independently
confirms the intended table.

**Fix.** Six localparams named for the Zicsr encoding, used to select the table,
so no shared OP/OP-IMM constant can shift it again:

```systemverilog
localparam logic [2:0] F3_CSR_RESERVED = 3'b100;
localparam logic [2:0] F3_CSRRW = 3'b001;  F3_CSRRS = 3'b010;  F3_CSRRC = 3'b011;
localparam logic [2:0] F3_CSRRWI = 3'b101; F3_CSRRSI = 3'b110; F3_CSRRCI = 3'b111;
```

**Why this one defect produced all three symptoms.** `csrrw` was a no-op write,
so `mscratch` never changed and the handler's exit `csrrw sp, mscratch, sp`
restored `sp` to `0x80006480` instead of the interrupted `0x80006080`. The
handler's 17 register restores then read from `0x80006480` rather than from the
frame it had just written at `0x80006400`, so every caller-saved register came
back as zero. `main` resumed with `t3 = 0`, its "misaligned word load" became an
aligned load of ROM address 0 and did not trap, and execution never reached the
`sh` at `scratch+7`, the `sd` at `scratch+12` or the `sw` to `boot_rom`.

### 7.2 The missing store-access fault — **neither the testbench nor the
reference was wrong; it was a consequence of 7.1**

The handoff's hypothesis was that `sim/tb/mosaic_bringup_tb.sv` failed to model
PMA. It does not: `pma_region`, `region_readable`, `region_writable` and
`access_ok` are already there, `boot_rom` is the one region `region_writable`
rejects, and `m_d_fault <= 1'b1` is raised for a store that `access_ok` refuses
without performing the write. Cause 7 appears in `p08_misaligned.i0` as soon as
the Zicsr decode is fixed, with `tval = 0`, which is `MOSAIC_BOOT_ROM_BASE`:

```
$ grep -o "TRAP cause=[0-9a-f]* tval=[0-9a-f]*" \
    results/unit/core.bringup_vs_reference/p08_misaligned.i0.events.txt
TRAP cause=4 tval=0000000080001089
TRAP cause=4 tval=000000008000108a
TRAP cause=6 tval=0000000080001087
TRAP cause=6 tval=000000008000108c
TRAP cause=7 tval=0000000000000000
```

That is the exact trace `tests/programs/src/p08_misaligned.S` documents — 4, 4,
6, 6, 7 — and `p08_misaligned.i0` reports
`sig=0xef,0x1234567,0x123,0x101018181c000003`, whose low bits fold the five
logged causes. The `boot_rom` store path is independently covered by
`p13_romstore.i{0,1,2}` (`sig3 = 0x106`) and by probe 7.

**No change was made to the testbench.**

### 7.3 A third disagreement, on `mret` — **the reference was wrong**

With 7.1 fixed, four lines still differed, all of them `csrr t0, mstatus`
(`300022f3`) in the handler's exit path: the DUT read `0x1888`, the reference
`0x1808`.

**Which side was wrong.** Privileged Specification v1.12, §2.1.6.1:

> When executing an *x*RET instruction, supposing *x*PP holds the value *y*,
> *x*IE is set to *x*PIE; the privilege mode is changed to *y*; *x*PIE is set to
> 1; and *x*PP is set to the least-privileged supported mode.

and, for trap entry, the same section:

> When a trap is taken from privilege mode *y* into privilege mode *x*, *x*PIE is
> set to the value of *x*IE; *x*IE is set to 0; and *x*PP is set to *y*.

The DUT implements both. The reference implemented trap entry (`take_trap` swaps
`MIE` and `MPIE`) but treated `mret` as a bare `next_pc = self.mepc`, leaving
`mstatus` untouched. From the handler-written `0x1880` the DUT's `mret` yields
`0x1888` (`MIE ← MPIE = 1`, `MPIE ← 1`) and the reference's yields `0x1800`; the
next trap entry then reads back `0x1888` and `0x1808` respectively. **The DUT is
right and the reference is wrong**, and the four differing lines are exactly that
difference.

**This is the one edit made to the reference model, and it is flagged here
because the standing rule is not to bend the oracle to match the DUT.** The
edit implements the clause above in the reference's own style and cites it:

```python
elif ir == 0x30200073:
    is_mret = True
    next_pc = self.mepc
    # MRET is not just "pc = mepc".  Privileged Specification v1.12, section
    # 2.1.6.1: "When executing an xRET instruction, supposing xPP holds the
    # value y, xIE is set to xPIE; the privilege mode is changed to y; xPIE is
    # set to 1; and xPP is set to the least-privileged supported mode."
    old_mstatus = self.mstatus
    self.mstatus = ((old_mstatus & ~0x88) |
                    (0x08 if (old_mstatus & 0x80) else 0) |
                    0x80) | 0x1800
```

The justification is the specification, not the DUT's output: the same argument
would condemn any implementation that leaves `mstatus` alone on `mret`. Nothing
else in the reference was touched, and the two implementations stay independent
in every other respect.

---

## 8. Negative controls and mutation controls

### 8.1 Harness-level negative controls

* **Non-vacuity.** One bit of a real signature word from a real run is flipped and
  the comparison is shown to reject it.
* **A corrupted ELF is refused, not partially loaded** — bad magic, truncated, and
  non-RISC-V `e_machine`, each asserted to come back non-`kOk` from `LoadElf` with
  its named status.
* **A program that never writes TOHOST hits the cycle limit**, distinguished by
  name from "PC left mapped memory" and from "TOHOST written".

### 8.2 Eight directed probes

Each probe is also run through the independent reference on the same image and
compared event for event.

| # | Probe | Rule asserted |
|---|---|---|
| 0 | `x0` | `x0` reads as zero and discards writes |
| 1 | `illegal_csr` | a CSR number absent from `mode_m.json` is illegal instruction, and the sentinel in `rd` is unchanged |
| 2 | `ro_csr` | writing a read-only CSR is illegal instruction, and the sentinel in `rd` is unchanged |
| 3 | `csrrs_x0` | `csrrs c, x0` reads but does **not** write: `mscratch` keeps its value, and a read-only CSR with a zero source is a plain read rather than an illegal write |
| 4 | `misaligned_load` | a misaligned load traps with cause 4 |
| 5 | `fext` | an F/D instruction is illegal instruction in p0 |
| 6 | `never_tohost` | a program that never writes TOHOST is stopped by the cycle limit |
| 7 | `rom_store` | a store to `boot_rom` faults with cause 7 |

```
$ grep '  probe' results/unit/core.bringup_vs_reference/run.log
  probe x0               cycle_limit tohost=0x0000000000000000 sig=0x0,0x0,0x0,0x0
  probe illegal_csr      cycle_limit tohost=0x0000000000000000 sig=0x5a5a5a5a5a5a5a5a,0x0,0x0,0x2
  probe ro_csr           cycle_limit tohost=0x0000000000000000 sig=0xbadc0de,0x0,0x0,0x2
  probe csrrs_x0         cycle_limit tohost=0x0000000000000000 sig=0xaaaa5555aaaa5555,0x8000000000001100,0x0,0x0
  probe misaligned_load  cycle_limit tohost=0x0000000000000000 sig=0x0,0x0,0x0,0x4
  probe fext             cycle_limit tohost=0x0000000000000000 sig=0xbadc0de,0x0,0x0,0x2
  probe never_tohost     cycle_limit tohost=0x0000000000000000 sig=0x0,0x0,0x0,0x0
  probe rom_store        cycle_limit tohost=0x0000000000000000 sig=0x55,0x0,0x0,0x7
```

**The probe image was changed, twice, and both changes are recorded here.**
Its `probe.S` is not in this repository, so both edits were made as exact
instruction substitutions, each checked against
`riscv64-elf-as -march=rv64im_zicsr_zifencei` and then against the run:

| Offset | Was | Now | Why |
|---|---|---|---|
| `0x154`–`0x15c` (probe 4) | `lui t3,0x80` / `addiw t3,t3,1` / `slli t3,t3,12` → `t3 = 0x80001000` | `lui t3,0x80002` / `addiw t3,t3,0` / `slli t3,t3,0` → `t3 = 0x80002000` | probe 4 used its scratch address as the store target, and `config/profiles/p0.json` names `0x80001000` as `tohost`; the store ended the run before the misaligned load it exists to test, so probe 4 asserted nothing |
| `0x128` (probe 3) | `csrrs t1, mscratch, zero` (`34002373`) | `csrrs t1, misa, zero` (`30102373`) | adds the read-only half of the same suppression rule: a read-only CSR with `rs1 == x0` must be a plain read, not an illegal write |

Bytes 536, SHA-256 `2b15606206adc4aefa92555a658db90665ead779151ed76c442923ba4264c1dd`
(previously `4e642ac5574c4852e3d49e7da64b76587a19088b66715168972fc70bc524779d`).
`probe 4` now reports `sig3 = 4`, which it never could before.

### 8.3 Mutation controls

Five `ifdef` blocks in `mosaic_bringup_core.sv`, all off in the shipping build:

| Define | Defect injected | Result |
|---|---|---|
| `MOSAIC_BRINGUP_MUTANT_1` | `x0` becomes an ordinary writable register | **FAILS, exit 1** |
| `MOSAIC_BRINGUP_MUTANT_2` | the F/D mul-add opcode `1000011` decodes as an ordinary `add` | **FAILS, exit 1** (probe `fext`) |
| `MOSAIC_BRINGUP_MUTANT_3` | the load-misalignment check is skipped | **FAILS, exit 1** |
| `MOSAIC_BRINGUP_MUTANT_4` | `csrrs`/`csrrc` with a zero source still writes the CSR | **DOES NOT FAIL, exit 0 — see 8.4** |
| `MOSAIC_BRINGUP_MUTANT_5` | a trap is reported as an ordinary retire | **FAILS, exit 1** |

Each mutant is built by re-running `tools/run_unit.py`'s own Verilator command
line — same flags, same source order, same include paths — with the single extra
`-D`, and the resulting binary is run with the registered `--seed 1
--max-cycles 4000000`. Verbatim output:

```
=============== MOSAIC_BRINGUP_MUTANT_1
build: ok
exit code: 1
RESULT FAIL core.bringup_vs_reference check failed: p01_addsub.i0: the DUT and the independent reference disagree -- event count differs: run has 88751, reference has 71068

=============== MOSAIC_BRINGUP_MUTANT_2
build: ok
exit code: 1
RESULT FAIL core.bringup_vs_reference corpus 39 programs, 2774346 architectural events, every stream identical to the independent reference
CHECK FAILED: probe fext: retire stream matches the independent reference (19 of 6664 events differ)
CHECK FAILED: probe fext: mcause is 2, got 0 -- an F/D instruction is illegal instruction in p0

=============== MOSAIC_BRINGUP_MUTANT_3
build: ok
exit code: 1
RESULT FAIL core.bringup_vs_reference check failed: p08_misaligned.i0: the DUT and the independent reference disagree -- event count differs: run has 71445, reference has 71593

=============== MOSAIC_BRINGUP_MUTANT_4
build: ok
exit code: 0
RESULT PASS core.bringup_vs_reference corpus 39 programs, 2774346 architectural events, every stream identical to the independent reference

=============== MOSAIC_BRINGUP_MUTANT_5
build: ok
exit code: 1
RESULT FAIL core.bringup_vs_reference check failed: p01_addsub.i0: the DUT and the independent reference disagree -- 53250 of 71068 events differ
```

Mutant 5 is caught at the fourth event, which is the `csrw mscratch, t0` at
`0x8000010f` being dressed up as an ordinary retire:

```
MISMATCH p01_addsub.i0 retire stream: ... event 4: reference
  "hart=0 seq=0000000000000004 pc=0000000080000010 ... insn=34029073" vs run
  "hart=0 seq=0000000000000004 pc=0000000080000010 ... insn=34029073 rd=x0 val=0000000000000000"
```

### 8.4 `MOSAIC_BRINGUP_MUTANT_4` is a vacuous control — this is the open finding

**It is not a defect in the DUT or in the reference; both are correct.** It is a
defect in the control.

The injected condition is:

```systemverilog
csr_writes = (ctl.csr_op == CSR_RW) ||
             (((ctl.csr_op == CSR_RS) || (ctl.csr_op == CSR_RC)) &&
              ((csr_operand != {XLEN{1'b0}}) ||
               (csr_is_legal(ctl.csr_addr) && csr_is_writable(ctl.csr_addr))));
```

For `csrrs`/`csrrc` with `rs1 == x0` this admits a write exactly when the target
is **legal and writable**, and `csr_wdata = csr_rdata | csr_operand` with a zero
operand is `csr_rdata`. So the mutant's only effect is to write a CSR's own
contents back into it. That is unobservable if every writable CSR's WARL write
is idempotent — and in this profile every one of them is:

| CSR | `csr_written` | Idempotent? |
|---|---|---|
| `mstatus` | `(v & 0x88) \| 0x1800` | yes — `mstatus` is always stored in that form |
| `mtvec` | `{v[63:2], 2'b00}` | yes — bits 1:0 are always 0 |
| `mepc` | `{v[63:2], 2'b00}` | yes |
| `mip`, `mie` | `v & 0x88` | yes — stored masked |
| `mscratch`, `mcause`, `mtval`, `medeleg`, `mideleg` | `v` | yes |
| `mcycle`, `minstret` | `v` | **no** — but nothing in the corpus or any probe reads them |

The one architectural case where the suppressed write *is* visible is a
**read-only** CSR — a core that wrongly wrote there would raise illegal
instruction — but the mutant's own `csr_is_writable` guard excludes exactly that
case, so the mutant cannot produce it either.

This is measured, not argued: the mutant build runs the whole case, all 39 corpus
programs and all 8 probes, and prints `RESULT PASS` with exit 0, quoted above.

**What would make it observable.** Either of these, both of which need a rebuilt
probe image (`probe.S` is not in this repository, and the dispatch table has no
room for a ninth entry):

1. inject the defect as an unconditional `csr_writes = 1` for `CSR_RS`/`CSR_RC`
   with a zero source. Probe 3, as strengthened in §8.2, then catches it: with the
   mutant, `csrrs misa, x0` raises illegal instruction and `sig[1]` becomes 0
   instead of `0x8000000000001100`;
2. or add a probe that executes `csrrs <read-only CSR>, x0, x0`, which the
   current guard would let through as a write and turn into an illegal
   instruction.

Until one of those happens, `MOSAIC_BRINGUP_MUTANT_4` provides no mutation
evidence for the Zicsr write-suppression rule, and **acceptance criterion 3 is
not met for it**. That is the one criterion this report leaves open.

---

## 9. Defects this work actually found

Each was invisible to any self-consistency check.

| # | Side | Defect | Found by |
|---|---|---|---|
| 1 | DUT | the PC advanced by 4 **every cycle**, not only at commit — the core free-ran one instruction per clock instead of waiting for its memory | the streams diverged at line 0; a 10-cycle trace (`MOSAIC_BRINGUP_TRACE=10`) localised it to `pc_n`'s default |
| 2 | DUT | `lui` was not decoded at all, then decoded with the wrong immediate (missing the `<< 12`), then with the wrong operand A (`ir[19:15]` read as a register index) | `p01_addsub.i0`, event 71019 |
| 3 | DUT and reference | `div` and `divu` were swapped, because the mapping was written by mnemonic name rather than by encoding | `p05_divrem.i0`, event 71040 (the reference's copy surfaced first, because the reference ran standalone against real firmware before the RTL existed) |
| 4 | DUT | the signed-overflow guard tested `a[XLEN-1]` instead of `a == MIN`, so every negative dividend divided by minus one returned the dividend | `p05_divrem.i2`, event 71056 |
| 5 | testbench | a partial store was placed at the **word base** instead of at `8*(addr & 7)`, corrupting every store at an unaligned address | `p03_loadstore.i0`, event 71059, localised with `MOSAIC_BRINGUP_DUMP=80001080` |
| 6 | DUT and reference | `sra`'s register form (funct7 `0100000`, funct3 `101`) and all five OP-32 W-forms were missing | `p01_addsub.i0`, event 71019 |
| 7 | DUT | **the Zicsr `funct3` table was selected with the OP/OP-IMM `F3_*` names, so `csrrw` decoded as `csrrs` and wrote each CSR back onto itself** (§7.1) | `p08_misaligned.i0`, event 71126 |
| 8 | **reference** | **`mret` did not perform the `MIE ← MPIE`, `MPIE ← 1` update required by Privileged Spec v1.12 §2.1.6.1** (§7.3) | `p08_misaligned.i0`, four events, once 7 was fixed |
| 9 | harness | `Reporter::Check` failures did not affect the verdict, so the case could print `RESULT PASS` over 22 failed checks | only visible once the corpus passed and the probe section ran at all — it never had |
| 10 | harness | the reference was never given the probe selector, so all 8 probes were compared against selector 0's stream | first probe run after the corpus passed |
| 11 | harness | the probe reference got a *cycle* budget where the DUT got a *step* budget, so every probe comparison reported a spurious length mismatch | same |
| 12 | probe fixture | probe 4's scratch address is the `tohost` register, so the probe ended before the instruction it exists to test | probe 4 reported `sig3 = 0` |

Defects 3, 6 and 7 are the same mistake three times: mapping an instruction by
what its constant is *called* instead of by its encoding. Defect 7 is the Zicsr
version of it, and it is the one this pass root-caused.

Defect 8 is the first one the oracle found in itself. It is listed because it is
part of the answer to "what is the reference worth": a second implementation
disagreed with it, and the specification settled which side was wrong.

---

## 10. Evidence

```
$ python3 tools/run_unit.py --case core.bringup_vs_reference ; echo "EXIT=$?"
PASS core.bringup_vs_reference    task=I-008
EXIT=0

$ grep '^RESULT' results/unit/core.bringup_vs_reference/run.log
RESULT PASS core.bringup_vs_reference corpus 39 programs, 2774346 architectural events, every stream identical to the independent reference

$ grep -c 'CHECK FAILED' results/unit/core.bringup_vs_reference/run.log
0

$ python3 tools/lint_rtl.py ; echo "EXIT=$?"
ok   rtl/common/mosaic_fifo.sv: clean as mosaic_fifo
ok   rtl/common/mosaic_ram.sv: clean as mosaic_ram
ok   rtl/common/mosaic_skid_buffer.sv: clean as mosaic_skid_buffer
ok   rtl/core/mosaic_alu.sv: clean as mosaic_alu
ok   rtl/core/mosaic_branch_cmp.sv: clean as mosaic_branch_cmp
ok   rtl/core/mosaic_branch_target.sv: clean as mosaic_branch_target
ok   rtl/core/mosaic_bringup_core.sv: clean as mosaic_bringup_core
ok   rtl/core/mosaic_decoder.sv: clean as mosaic_decoder
lint: 9 source file(s) clean
EXIT=0

$ riscv64-elf-nm tests/programs/build/p08_misaligned.i0.elf | grep -E 'trapstack|stack_top'
0000000080006480 B __trapstack_section_end
0000000080006080 B __trapstack_section_start
0000000080006080 B mosaic_stack_top
0000000000000010 a MOSAIC_TRAPSTACK_ALIGN
0000000080006080 B mosaic_trapstack_bottom
0000000000000400 a MOSAIC_TRAPSTACK_SIZE
0000000080006480 B mosaic_trapstack_top

$ riscv64-elf-objdump -d -M no-aliases --start-address=0x80000160 \
      --stop-address=0x80000170 tests/programs/build/p08_misaligned.i0.elf
    80000160:	34011173          	csrrw	sp,mscratch,sp
    80000164:	f8010113          	addi	sp,sp,-128
    ...
    80000250:	34011173          	csrrw	sp,mscratch,sp
    ...
    80000294:	08010113          	addi	sp,sp,128
    80000298:	30200073          	mret

$ make -C tests/programs all
$ make -C tests/programs audit      # exit 0, 39 ELFs, all rv64im_zicsr_zifencei
```

### Probe image disassembly (abridged, current)

```
0000000080000000 <_start>:
    80000000: 000802b7  lui    t0,0x80          # PSEL_BASE
    80000004: 00f2829b  addiw  t0,t0,15
    80000008: 00c29293  slli   t0,t0,0xc
    8000000c: 0002b303  ld     t1,0(t0)         # probe selector
    80000010: 00331313  slli   t1,t1,0x3
    80000014: 00000397  auipc  t2,0x0
    80000018: 01438393  addi   t2,t2,20         # probe_table
    8000001c: 00730333  add    t1,t1,t2
    80000020: 00033e03  ld     t3,0(t1)
    80000024: 000e0067  jr     t3
0000000080000068 <probe_x0>:
    80000068: 00500013  li     zero,5
    8000006c: 000002b3  add    t0,zero,zero
...
0000000080000128 <probe 3, the csrrs x0 read>:   # §8.2, changed
    80000124: 34029073  csrrw  zero,mscratch,t0
    80000128: 30102373  csrrs  t1,misa,zero
    8000012c: 34002e73  csrrs  t3,mscratch,zero
0000000080000148 <probe 4, the misaligned load>:  # §8.2, changed
    80000154: 80002e37  lui    t3,0x80002       # 0x80002000, not tohost
    80000158: 000e0e1b  addiw  t3,t3,0
    8000015c: 000e1e13  slli   t3,t3,0x0
    80000160: 112232b7  lui    t0,0x11223
    80000164: 3442829b  addiw  t0,t0,836        # 0x11223344
    80000168: 005e2023  sw     t0,0(t3)
    8000016c: 001e1503  lh     a0,1(t3)         # misaligned -> cause 4
...
00000000800001c4: 00a02023  sw     a0,0(zero)    # boot_rom store -> cause 7
...
00000000800001dc <trap_entry>:
    800001dc: 341022f3  csrr   t0,mepc
    800001e0: 34202373  csrr   t1,mcause
    ...
    800001fc: 30200073  mret
0000000080000200 <tail>:                     # writes 0 to tohost, then parks
    80000200: 00000513  li    a0,0
    80000204: 00100593  li    a1,1
    80000208: 001023b7  lui   t2,0x102
    8000020c: 00a3b023  sd    a0,0(t2)
    80000210: 0000006f  j     .
```

---

## 11. Status against the acceptance criteria

| # | Criterion | Result |
|---|---|---|
| 1 | `python3 tools/run_unit.py --case core.bringup_vs_reference` prints PASS, exit 0, all 39 corpus ELFs agreeing line for line | **MET** — quoted in §10 and verified byte-for-byte outside the harness |
| 2 | `python3 tools/lint_rtl.py` clean | **MET** — 9 source files clean, exit 0 |
| 3 | all five mutants demonstrated to FAIL with exit 1 | **NOT MET for `MOSAIC_BRINGUP_MUTANT_4`** — four demonstrated in §8.3, the fifth is provably vacuous and proved so in §8.4 |
| 4 | the report names the root cause of the `csrrw` defect and the missing cause-7 trap, states which side was wrong and how that was established, and quotes real command output | **MET** — §7 and §10 |

The remaining work is one line of RTL and, for option 2 in §8.4, a `probe.S`
that is not in this repository. It is deliberately left to the reviewer rather
than done here: redefining a documented mutation control so that it fails is not
a change a package should make on its own authority.

## 12. Files

* `rtl/core/mosaic_bringup_core.sv` — the core, bring-up only.
* `sim/tb/mosaic_bringup_tb.sv` — the testbench and its memory model.
  **Unchanged in this pass**; the PMA model it already had was correct.
* `sim/unit/tb_bringup.cpp` — the harness and the embedded reference model.
* `results/reports/I-008-bringup.md` — this file.