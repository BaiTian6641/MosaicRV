# V-064 — multihart.context_isolation

**Case:** `multihart.context_isolation` (task `V-064`), profile **p3** (the only
profile with two harts; `config/profiles/p3.json` `"harts": 2`).
**Command:** `python3 tools/run_unit.py --profile p3 --case multihart.context_isolation`
from a deleted `build/p3/unit/multihart.context_isolation` (full rebuild).
**RESULT (verbatim):**

```
RESULT PASS multihart.context_isolation h0=157 h1=12100 retires, fcsr=0x0000000000000010/0x0000000000000028 vl=1/3 seed=1
```

Shipping binary `build/p3/unit/multihart.context_isolation/multihart.context_isolation`
sha256 `26486c13bea91c5921c886b97239f97418076fbc4473ee52a582f9b08eb61015`
(the same hash the controls tool built as its shipping baseline).

Sources (all additive): `sim/tb/mosaic_multihart_ctx_tb.sv`,
`sim/unit/tb_multihart_ctx.cpp`, `tools/run_multihart_context_controls.py`,
`tests/unit/registry.json` (one entry); plus the `mhartid` wiring described at the
bottom. The harness is the same two-hart machine `multihart.isolation` uses
(`mosaic_multihart`); the new wrapper only changes the two reset PCs to be equal
and taps per-hart context evidence.

## 1. What the case runs

Both harts reset to the **same physical PC** (`0x8000_2000`) and fetch the **same
instruction stream**. The first thing the program reads is `mhartid`; every later
per-hart difference (ASID, FP flags/result, vector length, whether the translated
load faults, whether an interrupt is taken, whether the hart halts in WFI) is
derived by the same instructions from that one per-hart value. That is what makes
the interleaving real: the same PC retires with different state on the two harts.

Three runs from a clean reset, each with its own fresh memory image:

| phase | `hart_en_i` | meaning |
|---|---|---|
| A | `01` | hart 0 alone — **reference 0** |
| B | `10` | hart 1 alone — **reference 1** |
| C | `11` | both together |

The reference is deliberately **two separate observations** (one per hart), not one
shared register state; the concurrent run's stream for each hart must equal *that
hart's own* solo stream, field for field.

Program (one image, both harts; hart `h` = `mhartid`):

* M-mode prologue: read `mhartid` twice (stability); `mtvec`/PMP; hart 0 raises a
  machine software interrupt on itself (`mie.MSIE`+`mstatus.MIE`+`mip.MSIP`) and
  takes it; `mstatus` = MPP=S + FS/VS=Initial + hart's own SUM bit; `fcsr.frm =
  hart`; `fcvt.d.wu f0, mhartid`; `fcvt.d.wu f3, x0`; `fdiv.d f2, f0, f3`;
  `fmv.x.d` + store; `vsetvli` with AVL `2*hart+1`; `vle32`/`vadd.vv`/`vse32`;
  `satp` = Sv39 | ASID `1+hart` | root PPN `Pp(0x80004000)+hart`; `mret`.
* S-mode body: read `satp` back and store it through a translated VA; a translated
  load of VA `0x4000_0000 + hart*0x1000` (hart 0's L0 entry is invalid → load page
  fault; hart 1's is mapped → returns its page's value); then hart 0 clears `sie`
  and executes `wfi` (halts), hart 1 runs a bounded work loop and then parks.
* M-mode trap handler: records `mcause`/`mtval` per hart into a per-hart page
  (sync at +0/+8, interrupt at +16/+24), clears the pending bit, advances `mepc`,
  preserves MPP, `mret`.

## 2. Pass clause → check

| Pass clause (card) | Check (exact `Check` text) | Can fail when |
|---|---|---|
| each hart's **PC** independent | `both harts reset to the same PC and their first retire PC is equal` | a hart fetches a different stream |
| **rename** independent | `hart 1's concurrent stream equals its own reference` + `hart 0's concurrent stream equals its own reference` | a redirect/rename recovery crosses harts |
| **commit** independent | same per-hart reference checks, plus `hart 1 kept committing after hart 0 halted` | one hart's halt stops the other |
| **trap** independent | `hart 0's translated load faulted with the page-fault cause and its own VA`; `hart 1 took no trap: the same load at the same PC is mapped for it`; `the faulting load retired as a trap on hart 0`; `the same load retired normally on hart 1` | a trap leaks to the other hart |
| **CSR** state independent | `each hart's satp carries its own ASID (1 and 2)`; `the two satp values differ`; `each hart's S-mode read-back of satp matches its own port`; `the two harts' mstatus values differ`; `each hart's mstatus carries its own SUM bit`; `each hart's fcsr is its own (flags from its own divide)`; `the two harts' fcsr values differ`; `the same fdiv.d produced each hart's own FP result (qNaN vs +inf)`; `the same vsetvli selected each hart's own vl (1 and 3)`; `the two harts' vl values differ`; `each hart's vector store holds its own doubled source elements` | one shared register/CSR/vector state |
| **`mhartid` stable** | `both harts read mhartid twice`; `each hart's two mhartid reads agree (it is stable)`; `hart 0 reads mhartid 0 and hart 1 reads mhartid 1`; `the two harts report different mhartid values` | `mhartid` is a shared constant (distinctness) or drifts (stability) |
| **one hart stopping does not stop the others** | `hart 0 halted in WFI`; `hart 1 never halted in WFI`; `hart 1 retired its program and parked`; `hart 1 kept committing after hart 0 halted`; `hart 0 halted in WFI with hart 1 held in reset`; `hart 1 parked with hart 0 held in reset` | a halt/stop propagates |
| interleaving: same PC, different GPR | `the same csrr mhartid at the same PC retires with a different GPR value per hart` | the two harts share state |
| IRQ per hart | `hart 0 took the interrupt it raised on itself`; `hart 1 took no interrupt`; `hart 0's recorded interrupt cause is the machine software interrupt` | an interrupt is delivered to the wrong hart |
| ownership / sharing (fabric) | `the shared service detected no ownership mismatch`; `every packet's own identity named its hart (h0->0, h1->1)`; `both harts used the one shared memory service (h0=186, h1=20134)`; `both harts had a request outstanding at the same time (294 cycles)` | a response is misrouted |

## 3. Interleaving matrix (observed, concurrent run)

| cell (same PC) | hart 0 (context A) | hart 1 (context B) |
|---|---|---|
| `csrr s1, mhartid` @ `0x8000_2000` | s1 = 0 | s1 = 1 |
| `csrr s2, mhartid` @ `0x8000_2004` | s2 = 0 (stable) | s2 = 1 (stable) |
| `bne s1, x0, skip_irq` @ `0x8000_2044` | not taken → takes MS interrupt | taken → skips |
| `csrw fcsr` / `fdiv.d` | frm=0; 0.0/0.0 → **qNaN** `0x7ff8…`, NV; fcsr = `0x10` | frm=1; 1.0/0.0 → **+inf** `0x7ff0…`, DZ; fcsr = `0x28` |
| `vsetvli` with AVL `2*hart+1` | vl = **1** | vl = **3** |
| `vle32/vadd.vv/vse32` | stores `2*0x11111111` (1 elem) | stores `2*1,2*2,2*3` (3 elems) |
| `csrw satp` | Sv39, ASID **1**, root `0x8000_4000` | Sv39, ASID **2**, root `0x8000_5000` |
| `ld a2, 0(a1)` @ trap cell (VA `0x4000_0000 + hart*0x1000`) | **load page fault** cause 13, tval `0x4000_0000` | retires normally, a2 = `0xdeadbeefcafef00d` |
| `wfi` @ WFI cell | **halts** (wfi_halt high) | skipped |
| work loop + park | — | **parks** after 12100 retires |

Observed end state: `priv = 1/1` (both S-mode), `satp = 0x8000100000080004 /
0x8000200000080005`, `mstatus` differ (hart 0 SUM=0, hart 1 SUM=1),
`trap_irq_ctr = 1/0`, hart 0 trap page: sync cause `0xd`/tval `0x40000000`,
interrupt cause `0x8000000000000003`; hart 1 trap page all zero.
Cycles: solo0 = 1161, solo1 = 112570, both = 112588. Both harts are genuinely
interleaved on the shared service (186 and 20134 accepted requests, 294 cycles
with both outstanding, 0 ownership mismatches).

## 4. Controls — each rebuilt from a deleted directory with its `-D` in the build

`tools/run_multihart_context_controls.py` (shipping baseline PASS first):

| control | kind | sha256 | exit | first failure named |
|---|---|---|---|---|
| `MOSAIC_MUTANT_MHARTID_SHARED` | RTL `-D` in `rtl/core/mosaic_core.sv`: both harts report the shared `MOSAIC_CSR_RESET_MHARTID` | `520c54d2cd4a9b27ce5a4dbdb942195cbd34b0d3794a9068e320c9eba543a945` | 1 | `CHECK FAILED: hart 0 reads mhartid 0 and hart 1 reads mhartid 1` (then `the two harts report different mhartid values`, `the same csrr mhartid at the same PC retires with a different GPR value per hart`) |
| `MOSAIC_MH_CTX_MUTANT_HALT_ALL` | RTL `-D` in `rtl/core/mosaic_multihart.sv`: hart 0's `wfi_halt` resets hart 1 | `487e9b16a4efedcc1e8294352d6f1fda20043866823e2da2f7d16c16713a8ef5` | 1 | `CHECK FAILED: hart 1 retired its program and parked` (also `hart 1 kept committing after hart 0 halted`, `hart 1's concurrent stream equals its own reference`) |
| `--control-share-ref` | driver-side: hart 1 compared against hart 0's reference (one shared reference state) | shipping binary | 1 | `MISMATCH stream/h1 …` + `CHECK FAILED: hart 1's concurrent stream equals its own reference (12100 retires)` |

Each mutant binary differs from the shipping binary (`cmp`) and each has its `-D`
on the recorded `build_command.txt`. All three exit 1 and name the check they
break. The first mutant is the card's "treating several software threads as one
hart" made concrete; the second is the card's "one hart stopping implicitly stops
the others"; the driver control is the card's "shared reference register state".
(A control that dies in the compiler or is inert would be reported as `MISS`; none
were.)

## 5. Defect found and fixed (recorded here because it moved other cases)

The case was first written expecting each hart to read its own `mhartid`, and it
found that **both harts read `mhartid` 0**: `mosaic_csr` read the single generated
constant `MOSAIC_CSR_RESET_MHARTID`, and `mosaic_core` did not wire `HART_ID` into
it. Two harts a machine cannot tell apart is a bug, so (with the integration lead's
approval) this was fixed:

* `rtl/core/mosaic_csr.sv`: new parameter
  `MHARTID_VALUE = mosaic_csr_pkg::MOSAIC_CSR_RESET_MHARTID`; the `mhartid` read
  returns it. The generated constant remains the **default**, so every single-hart
  instantiation is bit-identical.
* `rtl/core/mosaic_core.sv`: passes `CORE_XLEN'(HART_ID)` (and
  `MOSAIC_MUTANT_MHARTID_SHARED` forces the constant back for the control).
* `mosaic_multihart` is the only two-hart consumer; `mosaic_csr_tb` and the
  bringup core use the default.

Re-verified after the change (each from a deleted build directory):

| gate | verdict |
|---|---|
| `multihart.isolation` (p3) | `RESULT PASS multihart.isolation h0=102 h1=99 retires, shared_requests=307 contend=221 seed=1` |
| `core.act_dut` (p1) | PASS |
| `core.corpus_sweep` (p1) | PASS |
| `csr.precise_trap_mret` (p0, direct `mosaic_csr` consumer) | PASS |
| `core.trap_csr_program` (p0) | PASS |
| `python3 tools/lint_rtl.py --profile p0` | 65 source file(s) clean |
| `python3 tools/lint_rtl.py --profile p1` | 65 source file(s) clean |
| `python3 tools/check_records.py` | ok (99 registered cases) |
| `python3 tools/check_exclusions.py` | ok |

No ACT4 program or corpus program reads `mhartid` (checked in `tests/`), and
single-hart `HART_ID` is 0, so the change is inert outside the two-hart machine.

## 6. Not covered (explicit)

* **A 4-hart configuration.** The RTL is a two-hart construction:
  `mosaic_multihart` hardcodes `MH_HARTS = 2` and instantiates two `mosaic_core`s;
  p3 declares `"harts": 2`. No four-hart machine exists to run.
* **Interrupt-controller topologies.** The interrupt is raised by software through
  `mip`/`mie` on the hart itself; the CLINT/PLIC routing (per-hart external lines,
  MSIP fan-out, timer compare) is not driven and is not this case's subject.
* **Instruction-fetch translation.** Fetch translation is not integrated in this
  core (I-046/I-048 own it); both harts fetch physical code, and only the S-mode
  data path is translated. So "the same PC" is a physical PC, and the case does
  not claim per-hart fetch translation state.
* **Shared-memory ordering/coherence across harts.** The two harts use disjoint
  physical pages and no cross-hart visibility is claimed; that is V-065/V-066/V-067.
  The shared service here only proves ownership and contention.
* **FP/vector register files as such.** FP and vector coverage is at the
  architectural level: `fcsr`/flags/result via `fmv.x.d`, and `vl`/`vtype`/the
  stored vector bytes. Vector register-to-register state is not read out except
  through the store; `vmv.x.s` is not wired in this integration.
* **`mhartid` beyond p3.** The per-hart value is verified only in the two-hart p3
  machine; the ACT4/corpus suites run single-hart and read 0 as before.
* **Halt semantics beyond WFI + park.** `o_stopped` (an unsupported instruction)
  is checked to be false on both harts; other halt causes are not driven.
* **Randomised/seeded stress.** Every cell is directed; `--seed` is accepted and
  unused (a random program would be a weaker test than the directed cells above).
