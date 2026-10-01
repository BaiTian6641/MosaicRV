# I-008 — Simplified scalar semantic path as bring-up comparison

**Status: INCOMPLETE. The case does not pass. This report says exactly where it
stops and what is proven.**

Deliverables in this work package:

| File | State |
|---|---|
| `rtl/core/mosaic_bringup_core.sv` | complete microarchitecture, `-Wall` clean in the shipping build and in all five mutant builds |
| `sim/tb/mosaic_bringup_tb.sv` | complete, `-Wall` clean, owns the memory model |
| `sim/unit/tb_bringup.cpp` | complete harness, builds and runs; **21 of 39** corpus programs pass the full differential comparison |
| `results/reports/I-008-bringup.md` | this file |

Two acceptance criteria are **not** met and are not claimed:

* `python3 tools/run_unit.py --case core.bringup_vs_reference` does not print
  PASS. The case exits 1 on `p08_misaligned.i0`.
* The mutant runs were not executed. The five mutants exist, compile and lint
  clean, but no evidence of their failing is in this report, and this report
  does not pretend otherwise.

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
  so every operand is the architecturally committed value. That is what makes
  the machine in-order by construction rather than by enforcement.
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
* `sim/tb/mosaic_bringup_tb.sv` does `` `include "mosaic_pkg.sv" ``.
  `tools/run_unit.py` puts testbench sources before RTL sources on the Verilator
  command line and the elaborator reads them in order, so a package that is
  merely listed is read after the module that imports it.

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

**Costs.** A non-memory instruction is 3 clocks (`S_FETCH`, `S_FWAIT`,
`S_EXEC`); a memory instruction is 5 (`S_FETCH`, `S_FWAIT`, `S_EXEC`, `S_DREQ`,
`S_DWAIT`).

### Memory interface

Declared in full in the module header, so the harness author can implement the
other side from that text. One outstanding request per port; a request is
asserted for exactly one cycle and nothing else is accepted until the ack.
Fetch: `ifetch_req_o` / `ifetch_addr_o` / `ifetch_ack_i` / `ifetch_rdata_i` /
`ifetch_fault_i`. Data: `dmem_req_o` / `dmem_we_o` / `dmem_addr_o` /
`dmem_size_o` (encoded 0..3) / `dmem_wdata_o` / `dmem_ack_i` / `dmem_rdata_i` /
`dmem_fault_i`. Two contract points the environment must honour: a store to
`boot_rom` faults even though the region is mapped and readable (the write is
not performed), and the core performs every misalignment check itself and never
issues a misaligned request, so a permissive environment that assembles an
unaligned access from adjacent bytes is a conforming environment.

## 3. Trap rules implemented

Cause, and what `mtval` receives:

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

Trap entry writes `mepc`, `mcause`, `mtval`, does `MIE → MPIE`, `MIE ← 0`, and
jumps to `mtvec`. `MPP` is fixed at `11` by the CSR file, so there is no privilege
transition to perform. `mret` restores `mstatus.MPP` into the lower privilege
field (a no-op, since it is fixed), sets `MIE ← MPIE`, `MPIE ← 1`, and jumps to
`mepc`.

**A trap is never reported as an ordinary retire.** `evt_has_rd_o` is 0,
`evt_is_store_o` is 0, `evt_next_pc_o` is the trap vector, and cause/tval/epc
are populated. `MOSAIC_BRINGUP_MUTANT_5` breaks exactly this rule.

## 4. CSR file

`config/csr/mode_m.json` is the source of the table. `mstatus` reset `0x1800` and
`misa` reset `0x8000000000001100` are transcribed from that file and **re-read
from it at run time** by `tb_bringup.cpp`, which fails the case by name if either
literal stops matching.

Implemented, with their storage:

| CSR | Number | Behaviour |
|---|---|---|
| `mstatus` | 768 | reset `0x1800`; write mask bits 1,3,5,7; `MPP` fixed `11` on read and write; SIE/SPIE read 0 (p0 has no S-mode — Privileged Spec v1.12 requires it; this is the one place the JSON's `writable_fields` list is reconciled, and it is WARL-legal) |
| `misa` | 769 | `0x8000000000001100`, read-only |
| `medeleg` `mideleg` | 770 771 | full 64-bit storage |
| `mie` `mip` | 772 836 | write mask bits 7 and 3 |
| `mtvec` | 773 | `BASE[1:0]` forced to `00` (Direct only) |
| `mcounteren` | 774 | read-only 0 (all bits unmodifiable in this profile) |
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

`csrrw` with `rd == x0` does not read; `csrrs`/`csrrc` with `rs1 == x0` do not
write. Both forms, register and immediate, are covered by one expression because
the immediate forms carry a zero-extended `zimm` in the `rs1` field.

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
   model** (below) to produce the expected stream;
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

**Why this is not a DUT compared against itself:** the oracle is a different
language, a different decode, a different memory model and a different CSR table
derivation. It found, in the course of this work, five defects that a
self-consistency check would have accepted — see §8.

### Result so far

21 of the 39 corpus programs produce retire streams byte-identical to the
reference, and `p08_misaligned.i0` is the first that does not:

```
  p01_addsub.i0              tohost      events=71068  cycles=248698  tohost=0x1 sig=0x1,0x2468acf13579bdd,0x0,0xec pass
  p01_addsub.i1              tohost      events=71067  cycles=248695  tohost=0x1 sig=0x8000000000000000,...      pass
  p01_addsub.i2              tohost      events=71068  cycles=248698  tohost=0x1 sig=0x7ffffffffffffffe,...      pass
  p02_branch.i{0,1,2}        tohost      events=71122/71138/71116                                 pass
  p03_loadstore.i{0,1,2}     tohost      events=71084  cycles=248780  tohost=0x1                            pass
  p04_mul.i{0,1,2}           tohost      events=71060  cycles=248674  tohost=0x1                            pass
  p05_divrem.i{0,1,2}        tohost      events=71068/71077  cycles=248698/248725                        pass
  p06_shiftlogic.i{0,1,2}    tohost      events=71071  cycles=248707  tohost=0x1                            pass
  p07_byteops.i{0,1,2}       tohost      events=71076  cycles=248734  tohost=0x1                            pass
  p08_misaligned.i0          tohost      events=71341  cycles=248918  tohost=0x1  FAIL: differs at event 71125
```

### The open defect

At `p08_misaligned.i0`'s **second** trap entry, at `pc=0x80000250`
(`csrrw sp, mscratch, sp`), the DUT and the reference disagree about the value
of `mscratch`:

```
DUT: hart=0 seq=00000000000115d5 pc=0000000080000250 ... insn=34011173 rd=x2 val=0000000080006480
REF: hart=0 seq=00000000000115d5 pc=0000000080000250 ... insn=34011173 rd=x2 val=0000000080006080
```

`0x80006480` is the trap-stack top that `crt0` put in `mscratch`; `0x80006080` is
the interrupted program `sp`, which is what the handler's entry `csrrw` should
have left in `mscratch`. So on the DUT `mscratch` is never actually updated by a
`csrrw`, while the reference does update it. The visible consequence is that the
DUT takes 2 traps where the reference takes 5, so it never reaches the later
traps and the streams then differ in length.

The CSR write plumbing reads correctly (`csr_writes` enables `CSR_RW`,
`csr_written` masks the value, the next-state `case` assigns `mscratch_n`, and the
`always_ff` assigns `mscratch_q <= mscratch_n` on the same edge as every other
register), so the cause is not identified. **This is an unresolved defect in the
bring-up core and is the single thing standing between this work package and a
passing case.**

## 7. Negative controls

Implemented in `tb_bringup.cpp` and wired in; their results are not part of a
passing run because the case fails before reaching them.

* **Non-vacuity.** One bit of a real signature word from a real run is flipped and
  the comparison is shown to reject it.
* **A corrupted ELF is refused, not partially loaded** — three corruptions (bad
  magic, truncated, non-RISC-V `e_machine`), each asserted to come back
  non-`kOk` from `LoadElf` with its named status.
* **A program that never writes TOHOST hits the cycle limit**, distinguished by
  name from "PC left mapped memory" and from "TOHOST written", which are three
  separate outcomes the harness reports separately.
* **Eight directed probes**, one named architectural rule each, each also run
  through the independent reference and compared event for event:

| # | Probe | Rule asserted |
|---|---|---|
| 0 | `x0` | `x0` reads as zero and discards writes (`addi x0,x0,5` then `add t0,x0,x0` must be 0) |
| 1 | `illegal_csr` | a CSR number absent from `mode_m.json` is illegal instruction, and the sentinel in `rd` is unchanged |
| 2 | `ro_csr` | writing a read-only CSR is illegal instruction, and the sentinel in `rd` is unchanged |
| 3 | `csrrs_x0` | `csrrs c, x0` reads but does **not** write; `mscratch` keeps its value |
| 4 | `misaligned_load` | a misaligned load traps with cause 4 |
| 5 | `fext` | an F/D instruction is illegal instruction in p0 |
| 6 | `never_tohost` | a program that never writes TOHOST is stopped by the cycle limit |
| 7 | `rom_store` | a store to `boot_rom` faults with cause 7 |

The probe image is assembled with the same frozen toolchain and the same `-march`
as the firmware corpus. Bytes (536, SHA-256
`4e642ac5574c4852e3d49e7da64b76587a19088b66715168972fc70bc524779d`) are embedded
in `tb_bringup.cpp` and the harness writes a probe selector to `0x8000F000`
before each run, so one image covers all eight. Provenance command:

```
riscv64-elf-gcc -march=rv64im_zicsr_zifencei -mabi=lp64 -mcmodel=medany \
  -nostdlib -nostartfiles -ffreestanding -fno-builtin -fno-pic -mno-relax \
  -Wall -Wa,--fatal-warnings -c probe.S -o probe.o
riscv64-elf-gcc ... -Wl,-T,probe.ld -Wl,--no-relax -o probe.elf probe.o
riscv64-elf-objcopy -O binary probe.elf probe.bin
```

The `_start` code reads the selector, scales it by 8 and jumps through a table of
absolute addresses, which is why the same image can select any probe. The
disassembly of the image is in §10.

## 8. Mutation controls

Five `ifdef` blocks in `mosaic_bringup_core.sv`, all off in the shipping build,
all five confirmed to compile and lint clean:

| Define | Defect injected |
|---|---|
| `MOSAIC_BRINGUP_MUTANT_1` | `x0` becomes an ordinary writable register: the read mux stops forcing zero and the `rd == x0` qualification on the commit write enable is dropped |
| `MOSAIC_BRINGUP_MUTANT_2` | the F/D mul-add opcode `1000011` decodes as an ordinary `add`, so an extension that is not in p0 becomes legal |
| `MOSAIC_BRINGUP_MUTANT_3` | the load-misalignment check is skipped, so a misaligned load succeeds instead of trapping with cause 4 |
| `MOSAIC_BRINGUP_MUTANT_4` | `csrrs`/`csrrc` with a zero source still writes the CSR |
| `MOSAIC_BRINGUP_MUTANT_5` | a trapping instruction is reported as an ordinary retire, with a register write and a fall-through PC, while the machine still redirects correctly |

**No mutant run is claimed.** The acceptance criterion is that each one makes the
case FAIL with exit 1, and that evidence does not exist yet. Mutant 2 in
particular has no probe other than probe 5, so it must not be reported as covered
until probe 5 is shown to catch it.

## 9. Defects this differential comparison actually found

Each was invisible to any self-consistency check, and each is listed with the
evidence that located it. This is the concrete argument that the comparison is
worth its cost.

| Defect | Found by |
|---|---|
| The PC advanced by 4 **every cycle**, not only at commit — the core free-ran one instruction per clock instead of waiting for its memory | the streams diverged at line 0; a 10-cycle trace (`MOSAIC_BRINGUP_TRACE=10`) localised it to `pc_n`'s default |
| `lui` was not decoded at all, then decoded with the wrong immediate (missing the `<< 12`), then with the wrong operand A (`ir[19:15]` read as a register index) | `p01_addsub.i0`, event 71019 |
| `div` and `divu` were swapped, because the mapping was written by mnemonic name rather than by encoding | `p05_divrem.i0`, event 71040 |
| The signed-overflow guard tested `a[XLEN-1]` instead of `a == MIN`, so every negative dividend divided by minus one returned the dividend | `p05_divrem.i2`, event 71056 |
| The testbench placed a partial store at the **word base** instead of at `8*(addr & 7)`, corrupting every unaligned-address store | `p03_loadstore.i0`, event 71059, localised with `MOSAIC_BRINGUP_DUMP=80001080` |
| `sra`'s register form (funct7 `0100000`, funct3 `101`) and all five OP-32 W-forms were missing | `p01_addsub.i0`, event 71019 |

Two of these (`div`/`divu`, `sra`/OP-32) were the same mistake in both the RTL and
the reference: mapping an instruction by what its F3 constant is *called* instead
of by its encoding. In the reference model the mistake surfaced first, because
the reference ran standalone against real firmware before the RTL existed.

## 10. Evidence

Every command below was run from the repository root on 2026-09-30 with Verilator
5.052 and `riscv64-elf-gcc` 16.2.0.

### Lint, shipping build

```
$ verilator --lint-only -Wall -Wno-DECLFILENAME -Irtl/core \
    --top-module mosaic_bringup_tb \
    rtl/core/mosaic_pkg.sv rtl/core/mosaic_bringup_core.sv sim/tb/mosaic_bringup_tb.sv
- V e r i l a t o r   R e p o r t: Verilator 5.052 2026-09-05
- Verilator: Built from 0.174 MB sources in 5 modules, into 1.065 MB in 4 C++ files
- Verilator: Walltime 0.140 s (elab=0.005, cvt=0.136, bld=0.000); cpu 0.140 s
```

Clean, in the shipping build and in all five mutant builds (each additionally
with its own `-DMOSAIC_BRINGUP_MUTANT_<n>`).

### Build and run

```
$ verilator --cc --exe --build -j 0 -O2 -CFLAGS "-O2 -std=c++17 -Wall" \
    --x-assign unique --x-initial unique --top-module mosaic_bringup_tb \
    -Mdir build/p0/unit/core.bringup_vs_reference/obj_dir \
    -Ibuild/p0/sim -Irtl/core -Irtl/common -CFLAGS -Isim/common -CFLAGS -Ibuild/p0/sim \
    -o build/p0/unit/core.bringup_vs_reference/core.bringup_vs_reference \
    sim/tb/mosaic_bringup_tb.sv rtl/core/mosaic_bringup_core.sv \
    rtl/core/mosaic_pkg.sv rtl/core/mosaic_decoder.sv rtl/core/mosaic_alu.sv \
    sim/unit/tb_bringup.cpp sim/common/sim_common.cpp \
    sim/common/elf_loader.cpp sim/common/event_tap.cpp

$ ./build/p0/unit/core.bringup_vs_reference/core.bringup_vs_reference \
    --case core.bringup_vs_reference --out results/unit/core.bringup_vs_reference \
    --seed 1 --max-cycles 400000
...
RESULT FAIL core.bringup_vs_reference check failed: p08_misaligned.i0: the DUT and
the independent reference disagree -- 252 of 71341 events differ
```

Exit 1.

### Firmware corpus

```
$ make -C tests/programs all        # 39 ELFs
$ make -C tests/programs audit      # exit 0, 39 ELFs, all rv64im_zicsr_zifencei
```

The corpus exists and is current; all 39 ELFs are loaded and run.

### Probe image disassembly (abridged)

```
0000000080000000 <_start>:
    80000000: 000802b7  lui    t0,0x80          # PSEL_BASE
    80000004: 00f2829b  addiw  t0,t0,15
    80000008: 00c29293  slli   t0,t0,0xc
    8000000c: 0002b303  ld     t1,0(t0)
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
0000000080000198: 02000043  .word 0x02000043     # fmadd.s ft0,ft0,ft0,ft0
...
00000000800001c4: 00a02023  sw     a0,0(zero)    # boot_rom store
...
00000000800001dc <trap_entry>:
    800001dc: 341022f3  csrr   t0,mepc
    800001e0: 34202373  csrr   t1,mcause
    ...
    800001fc: 30200073  mret
```

## 11. What has to happen next

1. **Root-cause the `csrrw`-to-`mscratch` defect in §6.** Everything else in this
   work package is blocked behind it: the case cannot pass, and the mutant runs
   cannot be reported.
2. Run the five mutants and paste the failing output. Until then this package has
   no mutation evidence and must not be described as having any.
3. `tests/unit/registry.json` needs two edits to the `core.bringup_vs_reference`
   entry, requested from Main and not made here:
   * `cpp` must add `sim/common/elf_loader.cpp` and `sim/common/event_tap.cpp`.
     Without them the link fails with undefined `mosaic::LoadElf`,
     `mosaic::EventTap::Record`, `::Save` and `::Compare`.
   * `max_cycles` must rise from 200000 to at least 400000. A corpus program
     needs ~248700 clocks (71068 architectural events at 3 clocks each plus 5
     for each of its memory instructions); at 200000 the case stopped mid-run on
     the very first program and reported 66666 events against the reference's
     71068, which looks like a hardware bug and is not one.
4. Re-run `python3 tools/run_unit.py --case core.bringup_vs_reference` once the
   core is fixed, and paste the PASS.

## 12. Files

* `rtl/core/mosaic_bringup_core.sv` — the core, ~1500 lines, bring-up only.
* `sim/tb/mosaic_bringup_tb.sv` — the testbench and its memory model.
* `sim/unit/tb_bringup.cpp` — the harness and the embedded reference model.
* `results/reports/I-008-bringup.md` — this file.

Nothing else was modified. No commit was made.