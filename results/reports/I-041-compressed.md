# I-041 — RV64C decompression and cross-boundary fetch

Case: **`CASE=compressed.cross_boundary`** (registered in `tests/unit/registry.json`,
task I-041, top `mosaic_core_tb`, driver `sim/unit/tb_core_compressed.cpp`).
Card: `docs/implementation-plan.md` § "I-041 — 实现 C 解压与跨界取指".

Status: the driver exists, the case builds from a deleted build directory and passes
on the shipping RTL, and the card's four named failure modes are each caught by a
`-D` control that fails with a named check and its own binary hash
(`tools/run_compressed_controls.py`). The registry entry is still
`"pending": true` — the integration lead clears it when the package is recorded, and
this lane did not edit the registry.

Two defects were found and fixed on the way, both of them the reason the package did
not deliver on its first attempt:

* a **decoder defect this case caught**: the CI-format immediate of `c.addi`,
  `c.addiw`, `c.li` and `c.andi` was read from the destination-register field
  (§6);
* the **inherited regression** in `core.mem_program` and three other corpus cases:
  a speculatively fetched reserved encoding stopped the machine before the branch
  that would have squashed it had redirected (§5);
* an **inherited defect in the device serializer** that this lane found while
  checking the tree: the PMA attribute read back for a non-serialized access was
  the *previous* held transaction's, so an ordinary RAM access was reported to the
  memory system as an MMIO access and `mmio.exactly_once` (I-038) failed (§7).

The last section is the honest account of what is *not* covered.

---

## 1. The rule

### 1.1 The length is the encoding's, not the PC's

RISC-V states the length of an instruction in its own first two bits. Fetch
therefore reads the **encoding**:

```
rsp_data[1:0] == 2'b11  ->  32-bit instruction, 4 bytes
otherwise               ->  16-bit instruction, 2 bytes
```

and *checks* the memory's reported byte count against it rather than trusting it: a
32-bit encoding returned in two bytes is a malformed response and is refused as
illegal, not delivered with half its bits missing. `out_len` is the instruction's own
length and `out_bits` are its own bits — for a 16-bit instruction the upper half of
the fetched word is **not** carried, because those bytes are the next instruction's
encoding, and an event record that showed them would be showing a different
instruction.

### 1.2 One decompressor, one control word

`mosaic_decoder` takes `(insn_raw[31:0], insn16)` and expands the 16-bit encoding to
the base-ISA instruction it is shorthand for; the existing 32-bit decode then runs
unchanged. There is one `decode_ctl_t` and one meaning for every field, and nothing
downstream of the decoder knows C exists. A second control-word producer for the
16-bit forms is the "second opinion about what the machine does" this project refuses.

Three outcomes per encoding, each stated in the RTL rather than implied:

* **defined** — the encoding expands, `ctl.valid` is set, the instruction executes;
* **reserved** — `ok` is 0, the compressed arm drives nothing, and `ctl` is exactly
  `CTL_ILLEGAL`: never a half-decoded word;
* **hint** — `c.nop`, `c.addi`/`c.li`/`c.lui`/`c.slli` x0, `c.srli`/`c.srai`/`c.andi`
  x0, `c.mv`/`c.add` x0: legal, expands to an x0-destination instruction, retires and
  writes nothing. A hint that trapped would be a wrong machine.

The reserved set implemented (all RV64C text, and all exercised or refused by this
case): `c.addi4spn` with `nzuimm == 0` (the all-zero halfword), `c.addiw` rd == x0,
`c.addi16sp` `nzimm == 0`, `c.lui` imm == 0, `c.lwsp`/`c.ldsp` rd == x0, `c.jr`
rs1 == x0, quadrant-0 funct3 `001`/`100`/`101`, quadrant-1 funct3-`100`/`11` reserved
sub-cases, and quadrant-2 funct3 `001`/`101` (the c.fld/c.fsd/c.fldsp/c.fsdsp forms:
refused because p0 has no floating point — an unimplemented extension, not a reserved
encoding).

### 1.3 The encodings the case drives

Every word below was emitted by this case's own encoders and independently checked
against binutils:

```
riscv64-elf-as -march=rv64imc -mabi=lp64  +  riscv64-elf-objdump -D -b binary -m riscv:rv64
```

| halfword | binutils says | the case uses it for |
|---|---|---|
| `0000` | `unimp` | reserved (c.addi4spn nzuimm == 0; the all-zero halfword) |
| `2001` | `.insn` (unknown) | reserved (c.addiw rd == x0) |
| `4002` | `.insn` | reserved (c.lwsp rd == x0) |
| `6002` | `.insn` | reserved (c.ldsp rd == x0) |
| `8002` | `.insn` | reserved (c.jr rs1 == x0) |
| `8000` | `.insn` | reserved (quadrant 0, funct3 100) |
| `9c41` | `.insn` | reserved (quadrant 1 funct3 100, RV64 marker, `c[6:5] == 10`) |
| `0305` | `addi t1,t1,1` (c.addi x6,1) | the PC-advance-by-two case |
| `040d` | `addi s0,s0,3` (c.addi x8,3) | a compressed instruction before a straddle |
| `8526` | `mv a0,s1` (c.mv x10,x9) | compressed register moves |
| `9522` | `add a0,a0,s0` (c.add x10,x8) | |
| `55fd` | `li a1,-1` (c.li x11,-1) | sign extension of the 6-bit immediate |
| `0015` | `c.nop 5` (c.addi x0,5) | a hint: legal, retires, writes nothing |
| `c111` | `beqz a0,+4` | a **not-taken** compressed branch: the fall-through is PC+2 |
| `e111` | `bnez a0,+4` | a taken compressed branch |
| `a011` | `j +4` | a compressed unconditional jump |
| `9602` | `jalr a2` (c.jalr) | the **link** register must be PC+2 |
| `00100293` | `li t0,1` | 32-bit, 4 bytes |
| `00740493` | `addi s1,s0,7` | the 32-bit instruction placed at PC 2 mod 4 |
| `00000617` | `auipc a2,0x0` | builds the c.jalr's target |
| `00008713` | `mv a4,ra` | consumes the link value |

## 2. How the length and the original bits reach the event record

The two facts the card names — "正常与 trap trace 用 original instruction PC/length" —
travel as data, and nothing is re-derived from a PC or from a previous instruction:

```
mosaic_fetch        out_len / out_bits   (the encoding's own length and bits)
  + o_rsp_live, o_rsp_len                (the accepted response's own length)
mosaic_core         fetch_out_len / fetch_out_bits -> dbuf_len/dbuf_bits
                    uop_meta_t.insn_len is set from the same register
mosaic_dispatch     dec_len0/dec_bits0 -> desc_wr_len/desc_wr_insn
mosaic_macro_desc   wr_len/wr_insn -> len_q/insn_q -> rd_len0/rd_len1, rd_insn0/rd_insn1
mosaic_retire       rob_len/rob_insn -> ev_len / ev_insn (per valid lane)
mosaic_core         ev_len / ev_insn ports -> mosaic_core_tb ev_len_o / ev_insn_o
```

`ev_len`/`ev_insn` are set for **every** valid lane, including a trap lane, because
the event stream is the architectural record: a consumer must not have to infer an
instruction's length from its PC (alignment does not decide it) or from its
predecessor (a compressed instruction's PC is not the previous PC plus four).

The length is also what the **front end** and the **branch unit** use:

* `mosaic_core` advances the program counter by `fetch_rsp_len` at the cycle the
  response is accepted (`fetch_pc_q + o_rsp_len`), and issues the next request from
  the same expression, so the front end still sustains one instruction per cycle. It
  holds **one request in flight**: `imem_req_valid` requires
  `outstanding_count == 0 || o_rsp_live`, so there is never a second unanswered
  request whose PC would have to be guessed.
* `mosaic_branch_target` takes `insn_len` as an input: `link = pc + insn_len` and the
  sequential next PC is `pc + insn_len`, so a `c.jalr` links to PC+2 and a *not-taken*
  compressed branch falls through to PC+2.

## 3. What the case asserts, and where

`sim/unit/tb_core_compressed.cpp` builds eleven runs, each with its own hand-built
image and its own reference:

| run | image | what it pins down |
|---|---|---|
| `mixed` | 16/32 mixed straight line, exit protocol, park | the whole stream: PC, length, bits, destination, value; the c.jalr link; the not-taken compressed branch's fall-through; a 32-bit instruction at PC 2 mod 4 |
| `boundary` | as above, with a 32-bit instruction crossing the harness's eight-byte line, ending with an instruction whose **second halfword is outside the image** | the fault is reported at the instruction's *own start address*; the second halfword is never fetched as an instruction; the faulting instruction never retires |
| `reserved.<name>` ×7 | three instructions, then one reserved encoding | the reserved encoding leaves **no** architectural trace: nothing retires at its PC, the decoder's illegal counter moved, and the machine stops there |
| `trap` | `csrw mtvec`, then a misaligned `c.lwsp` at PC 0x8000001a (2 mod 4) | the **trap record** names the compressed instruction's own PC, length, bits, cause and tval |

The expectation is this file's own **RV64IC interpreter** (`RunReference`), written
from the ISA text: it decodes 16-bit encodings itself (it does not call, mirror or
import the RTL's decompressor), executes the image, and produces the architectural
stream. Every retired lane is compared against it: PC, length, encoding, destination,
write enable, value, and for a trap lane the cause and tval.

Three checks do not need the reference and are stated as structural facts:

1. **the record's length agrees with the encoding it carries** — for *every* lane,
   `len == 2` if and only if `insn[1:0] != 11`;
2. **the program counter advances by the delivered instruction's own length** — the
   fetch the core issues in the cycle a response is accepted is compared with the
   reference's length for that instruction;
3. **every address the core requests is an instruction's start address** (or an
   address the image cannot serve at all, which is how a runaway fetch ends) — a PC in
   the middle of an instruction is a PC advanced by the wrong amount;
4. at the second-halfword fault **the delivered PC is the faulting instruction's own
   start address**.

## 4. Boundary cases

**Where the boundary is.** p0 has no cache and no MMU: the core requests four bytes at
the instruction's own PC and the harness's memory model assembles them. The
"line" is therefore the harness's own model (eight bytes), and a *page* boundary
cannot be exercised at all in p0 — see §9.

**The straddle.** `boundary` places a 32-bit instruction at PC 0x80000016 so its four
bytes cross the eight-byte line at 0x80000018, and it retires with `ev_len == 4` and
its own four bytes; `mixed` places one at 0x80000012 (2 mod 4) so the case also shows
a 32-bit instruction at a two-byte-aligned PC, which is exactly what an "aligned link"
rule would get wrong.

**The second-halfword fault.** The image ends in the middle of a 32-bit instruction:
its first halfword (`0x8513`) is present and its low two bits say the instruction is a
32-bit one, so the four bytes cannot be assembled and the request is answered with
`imem_rsp_fault`. The case asserts that the fetch unit reports the fault at
`0x8000001e` — the instruction's **own start address**, not `0x80000020` (its second
halfword) — that the core never *requests* the second halfword as though it were an
instruction, and that the instruction never retires. In p0 a fetch fault is a **stop**,
not a synchronous trap (dispatch refuses the undecodable macro), so the trap-record
assertion comes from the `trap` run instead.

**The trap record.** The `trap` run arms `mtvec`, materialises a word-misaligned
address in `x2`, and executes a two-byte `c.lwsp x6, 0(x2)` **at address 0x8000001a,
which is 2 mod 4** — the placement where an "instructions are four bytes" assumption
shows. The trap record must name the compressed instruction's own PC `0x8000001a`,
length **2**, its own bits, cause 4 (load address misaligned) and tval the misaligned
address. A machine that derived the PC from the payload, advanced it by four, or
reported the length of the instruction *after* it, fails here.

`mepc` is checked to its low bit only, and the run prints what it reads:

```
[note] trap: the trapping compressed instruction is at 0x000000008000001a;
       mepc reads 0x0000000080000018 (the p0 CSR config declares IALIGN=32)
```

`mepc[1:0]` are read-only zero because p0's CSR config says so
(`config/csr/mode_m.json`, generated into `MOSAIC_CSR_WMASK_MEPC = …fffc`), so a trap
taken on a 2-mod-4 PC records that PC rounded down. That is the IALIGN=32 rule, it is
**not** what the C extension needs, and it is recorded here as a known gap rather than
asserted by the case — see §9 for why this lane did not change it.

## 5. The device-attribute defect this lane also had to fix

`CASE=mmio.exactly_once` (I-038) passed on the pre-I-041 tree and failed with the
restored A-extension work in place:

```
every access carries the device attribute its region demands -- cycle 36:
access to 0x0000000080001400 (ordinary memory) presented with device attribute 1
```

The A-extension lane had changed the serializer's attribute from `ser_hold_valid_q`
("a transaction is held, and only a device was ever held") to a separate latch,
`ser_hold_dev_q`, so that an AMO to RAM is held without being reported as a device —
the right intent. But it read the latch *alone*: an access that is not held at all
(only an ordinary, non-serialized one can be) then reported whatever attribute the
last held transaction had left behind.

The attribute is now the held transaction's and nothing else:

```
assign ser_out_dev_c = ser_hold_valid_q && ser_hold_dev_q;
```

A held device reports 1, a held AMO to RAM reports 0, and a direct access reports 0 —
which is correct by construction, because only a device or an AMO is ever held.
`mmio.exactly_once` and `amo.linearization` both pass, and the mutants around the
serializer (`MOSAIC_CORE_MUTANT_DEV_AS_RAM`, `MOSAIC_CORE_MUTANT_DEV_SPECULATIVE`) are
untouched.

## 6. The core regression this package inherited, and the fix

**Symptom.** With I-041's RTL in place, `core.mem_program` stopped the machine at
cycle 4146 (*"stalled ... allocated=50 retired=50 stopped=1"*), and three more corpus
cases failed the same way: `core.corpus_branch`, `core.corpus_sweep` (all 39 runs) and
`core.trap_csr_program`.

**Diagnosis.** Temporary per-cycle instrumentation on the case's own observation ports
(the fetch output register's PC, the illegal/unsupported counters, the branch barrier,
the redirect bundle) showed the same signature at every stop:

```
TEMPSTOP cyc=139 commit=46 dvalid=1 dpc=00000000800012ce dbits=00000073
         illegal=1 unsup=1 fetch_pc=... brinf=1 recov=0 wfi=0 redir=0
```

In `p03_loadstore`, the text ends with an unconditional jump at `0x800012c0` followed
by zero padding. The front end speculatively fetches the padding while the jump is
still unresolved; the all-zero halfword is a *reserved* compressed encoding
(`c.addi4spn` with `nzuimm == 0`), the decoder correctly refuses it, dispatch saw an
undecodable macro and stopped the machine — **before** the jump, which was still in
flight (`brinf=1`), could redirect and squash it. The deadlock is complete because the
redirect for that jump is itself gated on the machine not being stopped.

The pre-I-041 fetch dropped every non-32-bit encoding on the floor (`out_illegal`,
never pushed into the decode buffer), so the machine never stopped on padding — it
*skipped* it, which is the hole the card asks I-041 to close. Fixing the hole exposed
that p0's stop was not gated on the instruction being on the architectural path.

**Fix.** `mosaic_dispatch` gained one input, `branch_in_flight` (wired to the core's
`br_inflight`), and the unsupported refusal is now

```
l0_refused = l0_unsupported && !branch_in_flight;   // stop_q, counters, o_take
```

While a control transfer is unresolved, a macro this stage cannot decode is **held**:
neither allocated, nor taken, nor stopped. Both of the other choices are wrong —
taking it would silently drop an undecodable instruction (the hole again), and
stopping on it would halt the machine on an instruction that never executes
architecturally. The transfer's redirect purges the decode buffer and the machine
continues; if the transfer turns out not to be taken, the barrier drops and the
instruction stops the machine exactly as before.

This is not a weakening of the stop: every *architecturally next* undecodable
instruction still stops the machine, which the case suite confirms —
`core.ready_*` still stops at its refused ECALL (the ECALL is on the architectural
path), and `compressed.cross_boundary`'s reserved runs still stop at their reserved
encodings. What changed is that a *speculative* one no longer does.

**Result.** `core.mem_program`, `core.corpus_branch`, `core.corpus_sweep` and
`core.trap_csr_program` pass again, from deleted build directories, with I-041's C
support in the tree rather than reverted.

## 7. The defect this case found in the decompressor

The first run of the case failed its own reference on values, not on structure:

```
mixed: retire 2 at 0x80000008 value for x6 expected 0x1, got 0x6
```

`c.addi x6, 1` (halfword `0x0305`) added **6**. The CI-format immediate was built as
`{{6{c[12]}}, c[12:7]}` — a slice of the encoding — but the CI immediate is
`imm[5] = c[12]`, `imm[4:0] = c[6:2]`, and the destination register sits **between**
the two halves. The same expression was used for `c.addi`, `c.addiw`, `c.li` and
`c.andi`, so every compressed load-immediate form computed its immediate from its own
`rd` field. It is now one named value in `c_expand`:

```
ci_imm = {{6{c[12]}}, c[12], c[6:2]};   // sign-extended imm[5:0]
```

Checked against binutils, not against the DUT: `0x0305` disassembles as
`addi t1,t1,1` and `0x55fd` as `li a1,-1`. This was invisible to every other case in
the tree because no other case executes a compressed instruction.

## 8. Controls

`python3 tools/run_compressed_controls.py` rebuilds the case with one defect injected
from an empty directory, requires the mutant binary to differ from the shipping one,
requires exit 1, and requires the named check to appear in the mutant's output.

| define | injected into | defect | the check that names it |
|---|---|---|---|
| `MOSAIC_CORE_MUTANT_FETCH_PC_PLUS4` | RTL (`mosaic_core`) | the counter advances by four whatever the encoding said | *the instruction at 0x8000000a is 2 bytes long, but the fetch issued when it was answered went to 0x8000000e* |
| `MOSAIC_FETCH_MUTANT_DELIVER_16BIT` | RTL (`mosaic_fetch`) | a 16-bit instruction is delivered as 32 bits, length four, the next instruction's bytes as its upper half | *the instruction at 0x80000008 is 2 bytes long, but the fetch issued when it was answered went to 0x8000000c* |
| `MOSAIC_DECODER_MUTANT_C_RESERVED_EXEC` | RTL (`mosaic_decoder`) | a reserved encoding is expanded and executed instead of refused | *the reserved encoding at 0x80000008 leaves no architectural trace (nothing retires at its PC)* |
| `MOSAIC_IMEM_MUTANT_WRONG_LINE` | the harness's memory model (C++, `-CFLAGS -D`) | a straddling instruction's four bytes are assembled from the line *after* the one it starts in | *the event record carries the instruction's own bits: retire 8 at 0x80000016 …* |

The first three mutants are RTL defects. The fourth cannot be: in p0 there is no
cache, so the *memory* assembles the four bytes, and that is where "assembled from the
wrong line" lives. It is a `-CFLAGS` mutant for the same reason the loader controls
are (`tools/run_loader_controls.py`).

Mutant hashes and the shipping hash are printed by the tool and recorded below; a
mutant whose binary did not differ from the shipping build is reported as MISS rather
than counted.

```
shipping                              885e29a792bf85e9d1061457fce97cf31b0fa42710e4fccd467aae9f3c44495f
MOSAIC_CORE_MUTANT_FETCH_PC_PLUS4     1  OK  edcecc59b91101a61baa03a49e71b87d5fd548669761317bf84ad7304a69b23e
MOSAIC_FETCH_MUTANT_DELIVER_16BIT     1  OK  eddd9c452b4e7e20a4e6d21ec0a9f6556d6fa8d6b76051ebc4b3277425df11cb
MOSAIC_DECODER_MUTANT_C_RESERVED_EXEC 1  OK  d4189a217cf21eb241f078d409d42e52a6d9a7061cdf8466301ff9c2ef4269eb
MOSAIC_IMEM_MUTANT_WRONG_LINE         1  OK  ecd5b1a4ea752e5d5a84006fe9088c0f3a1435f798b20f79d98d67717563b0ff
all 4 mutants mutate the binary, exit 1 and name the check they break
```

## 9. Verification actually run

| what | command | result |
|---|---|---|
| the case | `python3 tools/run_unit.py --profile p0 --case compressed.cross_boundary` | **PASS**: `checks=98`, 11 runs, 66 retires, 14 of them compressed, 1022 cycles, seed 1, from a deleted build directory |
| the controls | `python3 tools/run_compressed_controls.py` | 4 of 4 mutants differ from shipping, exit 1, named (above) |
| the acceptance cases | `python3 tools/run_unit.py --profile p0 --case core.mem_program --case core.corpus_sweep --case core.corpus_branch --case fabric.fixed_two_cluster --case amo.linearization --case arbiter.forward_progress --case fetch.redirect_late_response --case decode.rv64im_reserved --case alu.boundaries --case fence.code_and_data_order --case core.trap_csr_program --case compressed.cross_boundary` | all **PASS** |
| RTL lint | `python3 tools/lint_rtl.py --profile p0` | clean, 38 sources |
| RTL elaboration | `slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl $(find rtl -name '*.sv' \| sort)` | exit 0, every lint check PASS |
| records | `python3 tools/check_records.py` | green: 43 delivered packages, 53 registered cases |
| the whole unit suite | `python3 tools/run_unit.py --profile p0 --all` (from deleted build directories) | 51 of 53 PASS, including every acceptance case; the two failures are not this package's (below) |

The full-suite run was taken before the one-line device-attribute fix, which was then
re-verified by re-running `mmio.exactly_once`, `amo.linearization`, `core.mem_program`
and `compressed.cross_boundary` (all PASS). Two suite failures remain, both inherited
and neither this package's:

* `reconfigure.drain_and_generation` cannot build: the registry lists
  `rtl/core/mosaic_owner_fsm.sv`, `sim/tb/mosaic_owner_fsm_tb.sv` and
  `sim/unit/tb_owner_fsm.cpp`, none of which exist in the tree. It is a registered case
  whose sources another lane has not landed.
* `mmio.exactly_once` failed in that run for the device-attribute defect fixed in §5;
  it passes now, which is the re-run recorded above.

## 10. Not covered (honest list)

* **A page boundary.** p0 has no MMU, so "cross-page" cannot be exercised: the case
  crosses the harness's *line*, which is the only boundary a cacheless core can see.
  The second-halfword fault is modelled as "the memory cannot assemble the four
  bytes", which is what a page-crossing fault looks like from the core's side, but
  the *cause* the DUT would report for a real page fault is Sv39's (I-045).
* **The cache-line refill case.** I-042's L1I does not exist yet, so "a straddling
  instruction assembled across a refill" is not exercised; the memory model does the
  assembly directly.
* **A fetch fault as a *trap*.** In p0 a fetch access fault is a stop (dispatch
  refuses the undecodable macro). The trap-record assertions therefore come from a
  memory-path exception on a compressed instruction, not from a fetch fault. When a
  trap path for instruction-access faults exists, the second-halfword fault should
  become a trap record with the same PC and length.
* **c.lw/c.sw/c.ld/c.sd (quadrant 0), c.addi16sp, c.srli/c.srai/c.andi,
  c.sub/xor/or/and, c.subw/addw and c.lui are decoded but not executed by any run** —
  they are in the decompressor and in the decoder's own reserved/hint rules, but this
  case only *refuses* reserved encodings and *executes* the forms listed in §1.3.
* **The F/D compressed forms** are refused (one encoded at a time is one of the seven
  reserved runs would need `c.fld`/`c.fsd` variants); the case checks that one such
  encoding leaves no trace, not that each of the four is refused.
* **A trap lane's payload bus** (rd/value) is not compared — only its PC, length,
  bits, cause and tval — because a trapping instruction publishes a trap, not an
  architectural write.
* **`c.ebreak`** and the `c.jr`/`c.ldsp`/`c.lwsp` *non-reserved* forms are not
  executed.
* **The compressed instruction's `misa` bit.** `config/capability_ladder.json` still
  gates the C extension on this package; the case does not check `misa` reporting, and
  nothing here advertises the extension to software.
* **`mepc`'s alignment.** With the C extension, IALIGN is 16 and only `mepc[0]` may be
  read-only zero; p0's CSR config (`config/csr/mode_m.json`) still declares
  `unmodifiable_bits: ["1:0"]` for the IALIGN=32 rule and generates
  `MOSAIC_CSR_WMASK_MEPC = 64'hfffffffffffffffc`, so a trap taken on a 2-mod-4 PC
  records that PC rounded down (observed: a trap at 0x8000001a reads back as
  0x80000018, and the case prints that as a note rather than asserting it). Moving the
  mask to `mepc[1]`-writable is the IALIGN=16 change: the C extension is gated to
  **p1** in `config/capability_ladder.json` (`impl_tasks: ["I-041"]`), the p0 CSR
  config is what generates the mask, and `CASE=core.event_payload` — another
  package's driver, which this lane must not edit — pins the p0 rule
  (`mepc's low two bits are read-only zero`). It is therefore a cross-package item for
  the lead: it needs the CSR config, its generated package and that case's expectation
  to move together.
* **The `pending` flag** in `tests/unit/registry.json` is still set; clearing it is the
  integration lead's action when the package is recorded, and this lane did not edit
  that file.
