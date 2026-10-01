# I-006 — Synchronous RAM abstraction and collision semantics

Task card: `docs/stage-0-contracts-bringup.md` §I-006 (line 177). Interface contract:
`docs/implementation-plan.md` §1.3. Cross-checked against hypothesis **H-004**
(`docs/platform-plan.md` §H-004, line 89): the observable behaviour of the wrapper must be
independent of the target RAM's mixed-port mode.

Files owned by this deliverable:

| File | What it is |
|---|---|
| `rtl/common/mosaic_ram.sv` | the RAM itself — portable RTL, no vendor primitive |
| `sim/tb/mosaic_ram_tb.sv` | `mosaic_ram_tb`, the Verilator top: two RAM banks + address permutation |
| `sim/unit/tb_ram.cpp` | `CASE=ram.collision_matrix`, the campaign and the independent shadow model |
| `results/reports/I-006-ram.md` | this file |

Toolchain actually used: **Verilator 5.052 2026-09-05**, Python 3.9.6, macOS/arm64
(Darwin 25.6.0). Yosys 0.69+post was used only for the inference evidence in §5.

---

## 1. The interface, and the timing contract in prose

```systemverilog
module mosaic_ram #(
  parameter int DATA_WIDTH        = 64,
  parameter int DEPTH             = 64,   // power of two
  parameter int BYTE_ENABLE_PORTS = 1,
  localparam int ADDR_WIDTH       = $clog2(DEPTH),
  localparam int NUM_BYTES        = DATA_WIDTH / 8,
  localparam int READ_LATENCY     = 1
) (
  input  logic clk, rst,
  input  logic [ADDR_WIDTH-1:0] waddr,
  input  logic [DATA_WIDTH-1:0] wdata,
  input  logic [NUM_BYTES-1:0]  wmask,
  input  logic                  we,
  input  logic [ADDR_WIDTH-1:0] raddr,
  output logic [DATA_WIDTH-1:0] rdata,
  output logic                  rvalid
);
```

`ADDR_WIDTH`, `NUM_BYTES` and `READ_LATENCY` are **localparams in the parameter port list**,
not parameters: they are derived, so no caller can create a geometry in which the address
width disagrees with the array depth or in which the read latency is anything but one.
Three `generate if` blocks turn a contract violation into an elaboration error (they
instantiate a module that does not exist) rather than into a silent misbuild:

```systemverilog
if (READ_LATENCY != 1)          begin : g_bad_read_latency  mosaic_ram_contract_violation u_read_latency(); end
if ((1 << ADDR_WIDTH) != DEPTH) begin : g_bad_depth          mosaic_ram_contract_violation u_depth();        end
if ((DATA_WIDTH % 8) != 0)      begin : g_bad_data_width    mosaic_ram_contract_violation u_data_width();    end
```

**In prose.** `rst` is synchronous. While `rst` is high the RAM is dead: the write port
accepts nothing and the read port produces neither data nor valid. Once `rst` is low, the
write port commits `wdata` to `waddr` on the next rising edge, lane `i` only if `wmask[i]`
is set — a lane whose mask bit is zero keeps the byte it had, and a write with `wmask == 0`
writes nothing at all. The read port has no enable and never stalls: an address presented in
cycle N produces its data in cycle N+1 and nothing else, one read per cycle in, one read per
cycle out, in order. `rvalid` is high in exactly those cycles whose predecessor cycle was a
non-reset cycle, and low otherwise; because the read port cannot stall, `rvalid` is exactly
`rst` delayed by one cycle and inverted, which is also the cheapest implementation and the one
that makes "asserted exactly one cycle later" structural rather than merely checked.
`rdata` is a register fed by the array on the clock edge, never a combinational path out of
the array.

## 2. The collision contract (chosen, not inherited)

**Same-address, same-cycle read and write resolves to READ-FIRST, byte by byte.** The read
issued in cycle N observes the array as it was at the *start* of cycle N. Concretely, when
`raddr == waddr` in cycle N:

* every byte lane of `rdata` returns the byte value that was in the array before cycle N's
  write — the lanes `wmask` selects *and* the lanes it does not;
* so the returned word equals the pre-write word in full, and the write commits normally.

There is no write-first mode, no no-change mode, and no platform-dependent tie-break.

**Why this one, for this project's first consumer.** The integer PRF reads its two source
operands and writes its one destination physical register in the same cycle. `add x5, x5, x3`
addresses the same physical register through the read port and the write port in the same
cycle, and RISC-V requires the source operand to be the old value. Read-first delivers that
for free: no bypass network in the PRF, no special case for `rd == rs1` or `rd == rs2`, and
no dependence on what happened to that register in the previous cycle. Write-first would
silently corrupt every `rd == rs` instruction; no-change would make the result depend on the
array's previous contents, which a real board cannot reproduce deterministically.

**Portability claim being tested.** Read-first is exactly the read-during-write "old data"
mode that FPGA block RAM offers natively (`READ_FIRST` on AMD/Xilinx and the equivalent on
Gowin and Lattice), which is why the common RTL can state it without a bypass. A platform
macro whose only mixed-port mode is write-first or pass-through needs a **wrapper change and
nothing else**: inside `platform/<board>/` you add either an explicit bypass register feeding
`rdata` or an external arbiter that serialises the two ports. No consumer, no contract, and no
line of this unit test changes. That is the property H-004 asks for, and the portability of
this contract is what makes the whole core board-independent.

## 3. Semantics matrix

Rows are situations, columns are what the RAM observably does. "old" = the value before this
cycle's write, "new" = the value this cycle's write commits.

| # | Situation | `rdata` (cycle N+1) | `rvalid` | Array afterwards | Checked by |
|---|---|---|---|---|---|
| 1 | read, no write | word at `raddr` | 1 | unchanged | every phase |
| 2 | read + write, **different** address | word at `raddr`, post-write | 1 | `waddr` updated | edges-interleave |
| 3 | read + write, **same** address, `wmask = ff` | **old** word (read-first) | 1 | new word | `collision-every-address`, every address |
| 4 | read + write, same address, partial mask | **old** word, all 8 lanes | 1 | selected lanes new, others kept | mask `0x03` collision at every address, plus all 256 masks |
| 5 | read + write, same address, `wmask = 00` | **old** word | 1 | unchanged | `collision-every-address` (`Collide(a, v, 0x00)`) |
| 6 | write only, no read issued | undefined (no `rvalid` contract on it) | 1 | updated | every phase |
| 7 | any access while `rst` high | no data (`rdata` holds its previous value) | **0** | unchanged; the offered write is dropped | `init`, `reset-preserves-array` |
| 8 | back-to-back reads, different addresses | in order, one per cycle, never stalls | 1 | unchanged | `edges-interleave`, soak |
| 9 | back-to-back writes | — | 1 | both land, neither dropped nor duplicated | `edges-interleave`, soak |
| 10 | read-after-write (write in N, read of it in N+1) | new word | 1 | updated | byte-mask sweep, edges |
| 11 | write-after-read (read in N, write to it in N+1) | old word | 1 | updated | edges (`Collide` at 0, 31, 63) |
| 12 | two different addresses written on the same edge | — | 1 | both land | edges-interleave; the two banks write `a` and `a ^ 0x2a` every cycle of the campaign |
| 13 | entry never written since power-up | **undefined contents** | 1 | — | deliberately not compared until the array is initialised (§6) |
| 14 | reset at any time | array contents survive | 0 during reset | unchanged | `reset-preserves-array`, 2 rounds × full array |

Addresses at `0`, `DEPTH/2 - 1` (= 31) and `DEPTH - 1` (= 63) are exercised explicitly in
`edges-interleave` and in the corner-mask pass of `byte-mask-sweep`; all-address collisions are
swept over all 64 addresses.

## 4. Reset strategy

**What is reset: one control bit.** `rvalid` is cleared by `rst`. That is the entire reset
footprint. `rdata` is a plain pipeline register that is not reset, because `rvalid` masks it
while it holds a meaningless value. **The array has no reset and no initial value.**

Why that is sufficient: validity is tracked *outside* the RAM. The owner of the array — the
PRF free list, the cache tag array, the boot ROM loader — holds a valid bit per entry and
clears it explicitly; it must never read an entry it has not written since reset. Row 13 of
the matrix makes the consequence explicit: an unwritten entry's contents are undefined, and
the testbench therefore does not pretend to know them.

Why it is not just permitted but required: resetting a `DEPTH × DATA_WIDTH` array would turn
an inferrable block RAM into `DEPTH × DATA_WIDTH` flip-flops each carrying a reset, which is
precisely the "unexpected full-FF reset" (意外全FF reset) that the card blocks. §5 shows what
that costs in cells.

Evidence that synthesis does not infer a reset on the array:

```sh
$ yosys -p 'read_verilog -sv rtl/common/mosaic_ram.sv; hierarchy -top mosaic_ram -check; \
    proc; memory; opt; memory_map; opt_clean; \
    select -assert-none t:$adff t:$aldff t:$sdff t:$sdffe t:$dffsr t:$dffsre'
...
End of script. Logfile hash: 88ec969ee0, time: 0.33s, user: 0.41s, sys: 0.02s
$ echo $?
0
```

No resettable flip-flop cell exists anywhere in the elaborated module, and the array is
inferred as memory (`Mapping memory \mem in module \mosaic_ram: created 64 $dff cells and 0
static cells of width 64. Extracted data FF from read port 0` — the read register is *outside*
the array, which is the register boundary the card asks to see). Yosys folds
`if (rst) rvalid <= 0; else rvalid <= 1;` into a constant-with-enable on a single flop, which
is why the assertion above passes even for the one register that does take a reset.

The contrast, same script, same array, with the reset mutant of §7 (mutant 4) compiled in:

| | shipping | `-DMOSAIC_RAM_MUTANT_ARRAY_RESET` |
|---|---|---|
| cells | 9 282 | 17 559 |
| `$mux` | 544 | 4 721 |
| `$reduce_bool` | 0 | 4 096 |
| yosys wall time | 0.33 s | 5.04 s |

One constant-zero multiplexer and one AND reduction appears **per array bit** — 4 096 of them
for a 64×64 array. That is the full-array reset showing up as per-bit reset logic, and it is
what stops the array from ever being inferred as a block RAM.

## 5. How the testbench avoids being its own oracle

`sim/tb/mosaic_ram_tb.sv` instantiates **two** `mosaic_ram` banks. Bank A takes the stimulus
address; bank B takes the same address XOR `0x2a`. Because XOR by a non-zero constant is a
bijection, `raddr == waddr` holds in bank B exactly when it holds in bank A, so the collision
cases are covered in both — while in the same cycle the two write ports always address two
*different* locations, which is how row 12 is exercised on every cycle of the campaign rather
than in one scripted corner.

`sim/unit/tb_ram.cpp` keeps an **independent C++ shadow array per bank** with little-endian
byte packing (`MergeBytes()`: for each mask bit, replace `old & ~maskbyte` with
`data & maskbyte`). The shadow is written from the stimulus the testbench generated, and it
models the *contract*, never the RTL. The ordering that makes the oracle independent:

1. at the start of a cycle, `rdata`/`rvalid` still belong to the **previous** cycle's read, so
   they are compared against what was promised for that cycle;
2. the expectation for *this* cycle's read is captured from the shadow **before** this cycle's
   write is applied — that is the read-first rule expressed in the oracle;
3. only then is the write merged into the shadow, and only then does the clock edge fire.

Two checks therefore run on **every** cycle of the campaign, plus named checks in each phase:

* **latency check** — `rvalid` in cycle N must equal `rst` in cycle N-1, for both banks;
* **data check** — `rdata` must equal the shadow's pre-write value, for both banks.

The run reports 5 410 such comparisons over 5 487 cycles. Nothing is compared until every entry
of both arrays has been written, which is what keeps Verilator's `--x-initial unique`
randomisation (and a vendor model's X behaviour) from either creating or masking a result:
after the init sweep the shadow predicts every value the RAM can legally return.

## 6. The campaign

`CASE=ram.collision_matrix`, 5 487 cycles of the 20 000 the registry allows:

| Phase | What it does |
|---|---|
| `init` | 4 reset cycles (with a write offered on every one), one 64-address sweep to write a distinct pattern into both banks, then the **first compared cycle is a same-address read/write** — the headline demonstration of the register boundary — then a full read-back |
| `collision-every-address` | at all 64 addresses: three full-mask same-address collisions back to back, a `0x03` partial-mask collision, a `0x00` no-write collision, then a clean read-back |
| `byte-mask-sweep` | all 256 byte-mask values at a rotating address (so each address sees four different masks), each a same-address collision followed by a read-back; then the corner masks `00/ff/0f/f0` at addresses 0, 31, 63 |
| `edges-interleave` | at 0, 31, 63: back-to-back reads across three addresses, back-to-back writes, write-after-read, read-after-write, and two distinct addresses written on the same edge |
| `reset-preserves-array` | 2 rounds × (fill all 64 entries, 4 reset cycles with a write offered to address 0 on each, read the whole array back) |
| `soak` | 4 000 seeded random cycles: random address, random byte mask, random write enable, random data, and the read address deliberately equal to the write address half the time |

## 7. Mutation table — the negative controls

The mutants are `-D` variants of `rtl/common/mosaic_ram.sv`, each confined to an `ifdef` that
is **off in the shipping build**. None of them is reachable from `tools/run_unit.py`, which
passes no `-D`; they are built by hand with the command below.

| # | Define | Injected defect | Which check catches it | Observed failure |
|---|---|---|---|---|
| 1 | `MOSAIC_RAM_MUTANT_COMB_READ` | `always_comb rdata = mem[raddr];` — a combinational read, i.e. "simulation reads in zero cycles, the board reads in one" | the named collision check `Collide(0, …)`: the read must return the value from before the write | `init: cycle 70` — returned `0xa5a5f00d45a40000` (post-write) where the shadow held `0xa5a5f00d5a5a0000` (pre-write) |
| 2 | `MOSAIC_RAM_MUTANT_NO_BYTE_ENABLE` | the write port ignores `wmask` and writes all 8 lanes | the `wmask = 0x00` collision at address 0: a write with no byte selected must leave the word alone | `collision-every-address: cycle 139` — the word came back rewritten although no lane was selected |
| 3 | `MOSAIC_RAM_MUTANT_EARLY_VALID` | `assign rvalid = ~rst;` — valid asserted in the cycle the address is presented, one cycle before the data | the per-cycle latency check, which requires `rvalid(N) == rst(N-1)` | `init: cycle 5` — the first non-reset cycle: `rvalid_a: expected low, got high` |
| 4 | `MOSAIC_RAM_MUTANT_ARRAY_RESET` | `if (rst) for (i…) mem[i] <= '0;` — the whole array is cleared on reset | `reset-preserves-array`: fill the array, reset, read the pattern back | `reset-preserves-array: cycle 1165` — `expected 0xa5a5f00d4a5a0000, got 0x0000000000000000` |

All four make the case **FAIL with exit status 1**, quoted verbatim in §9. For comparison,
mutant 2's build also emits two `-Wall` warnings of its own (`wmask` and `BYTE_ENABLE_PORTS`
become unused) — which is why the mutant builds below pass `-Wno-fatal`; the shipping build
needs no such flag.

## 8. What was deliberately left out, and why

* **No platform wrapper was written.** The card's deliverable for I-006 is the generic
  contract plus the semantics matrix; `platform/{gowin,amd,asic}/` belongs to H-003/H-004 and
  does not exist yet. §2 states precisely what a wrapper has to contain if a macro cannot do
  read-first, which is the interface the later work needs.
* **No vendor simulation models were compared**, and no `MOSAIC_RAM_MUTANT` variant was run
  through one. The models do not exist in the tree, and inventing a stand-in would be a
  fabricated comparison. The inference evidence in §4 is real; the multi-vendor half of H-004
  remains open for H-004 itself and is not claimed here.
* **`BYTE_ENABLE_PORTS = 0` is supported but not unit-tested.** The shipping configuration is
  1 and the parameter exists only so a future no-byte-enable memory can be wrapped; testing
  the degenerate mode would test a configuration this project does not use. Its lint-clean-ness
  in that mode is not claimed either (the unused `wmask` is flagged, correctly).
* **Only one write port.** The card's "simultaneous writes to two different addresses" case is
  covered by two RAM instances in the wrapper rather than by a second write port in the RAM,
  because the PRF's four banks are four RAM instances, not one RAM with four write ports. Row
  12 of the matrix and §5 explain the arrangement.

## 9. Commands and their real output

### 9.1 Lint (warnings are errors)

```sh
$ cd /Users/flare/MosaicRV
$ verilator --lint-only -Wall --top-module mosaic_ram_tb \
      sim/tb/mosaic_ram_tb.sv rtl/common/mosaic_ram.sv
- V e r i l a t i o n   R e p o r t: Verilator 5.052 2026-09-05 rev vUNKNOWN-built20260905
- Verilator: Built from 0.055 MB sources in 5 modules, into 0.035 MB in 3 C++ files needing 0.000 MB
- Verilator: Walltime 0.006 s (elab=0.001, cvt=0.002, bld=0.000); cpu 0.006 s on 1 threads; allocated 10.469 MB
$ echo $?
0
```

Zero warnings, zero errors. The geometry guard is not vacuous — temporarily setting the
wrapper's `DEPTH` to 32 gives:

```sh
%Error-MODMISSING: sim/tb/mosaic_ram_tb.sv:93:7: Cannot find file containing module: 'mosaic_ram_tb_geometry_mismatch'
%Error: Exiting due to 1 error(s), 4 warning(s)
```

(and was reverted immediately; the lint above is the reverted, shipping state).

### 9.2 The case, through the project's own runner

```sh
$ cd /Users/flare/MosaicRV
$ python3 tools/run_unit.py --case ram.collision_matrix
PASS ram.collision_matrix         task=I-006
$ echo $?
0
```

`results/unit/ram.collision_matrix/run.log`:

```
$ /Users/flare/MosaicRV/build/p0/unit/ram.collision_matrix/ram.collision_matrix --case ram.collision_matrix --out /Users/flare/MosaicRV/results/unit/ram.collision_matrix --seed 1 --max-cycles 20000
RESULT PASS ram.collision_matrix collision matrix complete: 5410 shadow comparisons, 5487 cycles, seed 1
```

The soak is seeded from `--seed`; it passes on 1, 2, 3, 7 and 12345:

```
seed 1      RESULT PASS ram.collision_matrix collision matrix complete: 5410 shadow comparisons, 5487 cycles, seed 1
seed 2      RESULT PASS ram.collision_matrix collision matrix complete: 5410 shadow comparisons, 5487 cycles, seed 2
seed 3      RESULT PASS ram.collision_matrix collision matrix complete: 5410 shadow comparisons, 5487 cycles, seed 3
seed 7      RESULT PASS ram.collision_matrix collision matrix complete: 5410 shadow comparisons, 5487 cycles, seed 7
seed 12345  RESULT PASS ram.collision_matrix collision matrix complete: 5410 shadow comparisons, 5487 cycles, seed 12345
```

### 9.3 The four mutants, each built by hand and run against the same case

Build command (`$D` is the mutant define; the runner's own flags are reproduced, plus
`-Wno-fatal` because mutant 2 is *supposed* to leave `wmask` unused):

```sh
$ verilator --cc --exe --build -j 0 -O2 --x-assign unique --x-initial unique -Wno-fatal \
    -D$DEFINE --top-module mosaic_ram_tb -Mdir $D/obj \
    -CFLAGS -I$REPO/sim/common -CFLAGS "-O2 -std=c++17 -Wall -Wextra" \
    -o $D/ram.collision_matrix \
    sim/tb/mosaic_ram_tb.sv rtl/common/mosaic_ram.sv sim/unit/tb_ram.cpp sim/common/sim_common.cpp
$ $D/ram.collision_matrix --case ram.collision_matrix --seed 1 --max-cycles 20000
```

Mutant 1 — `-DMOSAIC_RAM_MUTANT_COMB_READ` (combinational read):

```
MISMATCH init: cycle 70: same-address read/write at addr 0 with mask 0xff returned 0xa5a5f00d45a40000 (post-write), shadow holds 0xa5a5f00d5a5a0000 (pre-write): expected contract holds, got contract violated
CHECK FAILED: init: cycle 70: same-address read/write at addr 0 with mask 0xff returned 0xa5a5f00d45a40000 (post-write), shadow holds 0xa5a5f00d5a5a0000 (pre-write)
CHECK FAILED: no contract violation in any phase
RESULT FAIL ram.collision_matrix contract violated: init: cycle 70: same-address read/write at addr 0 with mask 0xff returned 0xa5a5f00d45a40000 (post-write), shadow holds 0xa5a5f00d5a5a0000 (pre-write)
exit=1
```

Mutant 2 — `-DMOSAIC_RAM_MUTANT_NO_BYTE_ENABLE` (byte write ignores `wmask`):

```
MISMATCH collision-every-address: cycle 139: same-address read/write at addr 0 with mask 0x00 returned 0xa5a5f00d5e5a0000 (post-write), shadow holds 0xa5a5f00d585e0000 (pre-write): expected contract holds, got contract violated
CHECK FAILED: collision-every-address: cycle 139: same-address read/write at addr 0 with mask 0x00 returned 0xa5a5f00d5e5a0000 (post-write), shadow holds 0xa5a5f00d585e0000 (pre-write)
CHECK FAILED: no contract violation in any phase
RESULT FAIL ram.collision_matrix contract violated: collision-every-address: cycle 139: same-address read/write at addr 0 with mask 0x00 returned 0xa5a5f00d5e5a0000 (post-write), shadow holds 0xa5a5f00d585e0000 (pre-write)
exit=1
```

Mutant 3 — `-DMOSAIC_RAM_MUTANT_EARLY_VALID` (`rvalid` one cycle early):

```
MISMATCH init: cycle 5: rvalid_a: expected low, got high
CHECK FAILED: init: cycle 5: rvalid_a
CHECK FAILED: no contract violation in any phase
RESULT FAIL ram.collision_matrix contract violated: init: cycle 5: rvalid_a
exit=1
```

Mutant 4 — `-DMOSAIC_RAM_MUTANT_ARRAY_RESET` (reset clears the whole array):

```
MISMATCH reset-preserves-array: cycle 1165: bank A read data: expected 0xa5a5f00d4a5a0000, got 0x0000000000000000
CHECK FAILED: reset-preserves-array: cycle 1165: bank A read data
CHECK FAILED: no contract violation in any phase
RESULT FAIL ram.collision_matrix contract violated: reset-preserves-array: cycle 1165: bank A read data
exit=1
```

## 10. Handoff to the next module

* Instantiate `mosaic_ram` per PRF bank / per cache tag array. Read port issues in cycle N and
  the value is usable in cycle N+1; there is no enable and no stall, so any flow control has to
  live above the RAM.
* Track validity outside the array — a free list or a valid bitmap — and never read an entry
  that has not been written since reset. The array's power-up contents are undefined.
* Same-address read/write is already resolved for you: the source operand of an `rd == rs`
  instruction is correct without a bypass path. Do not add one on the assumption that the RAM
  does not guarantee it; it does, and the guarantee is tested.
* A platform whose macro cannot do read-first needs a wrapper and nothing else — see §2.
