# I-046 — TLB and SFENCE.VMA: `tlb.sfence_vma`

Work package I-046 (`docs/implementation-plan.md` §3.1). Registered case
`tlb.sfence_vma`, top `mosaic_core_tb`, driver `sim/unit/tb_core_tlb.cpp`. Run
with `python3 tools/run_unit.py --profile p1 --case tlb.sfence_vma` (the cache
is a supervisor feature and p1 is the profile that declares S/U and `satp`). The
case stays registered `"pending": true`; the integration lead clears it when
recording.

**Verdict: PASS** — `comparisons=33 checks=0`, from a deleted build directory,
with five controls that each fail with a named first failure and a binary hash
different from the shipping build (table below).

The cache is **`rtl/core/mosaic_tlb.sv`**, a module that *instantiates* the
I-045 walker (`mosaic_ptw`) and presents the walker's own request/response
interface and PTE port, so the core drives the cache exactly where it drove the
walker. It is not inside the walker: the walker's case (`sv39.walk_and_faults`)
keeps a bare `mosaic_ptw` DUT to drive, and the cache needs its own DUT whose PTE
port the case can own.

## The structure

| property | this implementation |
|---|---|
| size | 16 entries, 8 sets x 2 ways (set index `va[15:13]`) |
| replacement | invalid way, then an in-place tag match, then a per-set round-robin bit |
| tag | `vpn = va[38:12]` (4 KiB granular) + `asid = satp.ASID[15:0]`, compared exactly; plus a `g` bit for global (PTE.G=1) leaves, which match any ASID |
| ASID width | 16 (`ASIDLEN = ASIDMAX` for Sv39; the satp field is now writable, `config/csr/mode_su.json`) |
| what is cached | the translation (`pa[55:12]`) **and** the leaf's `{x,w,r,u}` permission bits **and** its `a`/`d` bits and `g` |
| permission on a hit | re-evaluated for *this* access with `mosaic_pkg::leaf_perm_ok`, the same function the walker uses, so a cached permission result cannot drift from a walked one |
| page size | 4 KiB. A superpage walk produces a per-4 KiB physical address and that is what is cached; a second 4 KiB chunk of the same superpage walks once of its own. Stated, not hidden. |

The `g`/`a`/`d` bits come from a new 3-bit `xl_attr_o` output on `mosaic_ptw`
(`{g,a,d}` after any A/D update); the walker otherwise is I-045 unchanged, and
its A/D compare-and-set policy is kept verbatim.

## The SFENCE.VMA matrix (what each form invalidates here)

The instruction is decoded by the core's front end (funct7 `0001001`,
funct3 `000`, rd `00000`, opcode `1110011` — recognised from the raw word like
WFI and SRET, so the decoder's reserved-encoding enumeration is untouched),
carried through dispatch as a system macro with **two** renamed operands, and
resolved at the ROB head. The matrix is the specification's (priv v1.12 §4.2.1):

| form | addresses | ASIDs | globals | this implementation |
|---|---|---|---|---|
| `rs1=x0, rs2=x0` | all pages | all | flushed | **exact** (nothing is kept) |
| `rs1=x0, rs2≠x0` | all pages of `rs2` | `rs2` only | **kept** | **exact** on the ASID dimension (a different ASID and a global survive); over-fencing would be legal, we do not |
| `rs1≠x0, rs2=x0` | the `rs1` page | all | flushed | **exact** |
| `rs1≠x0, rs2≠x0` | the `rs1` page of `rs2` | `rs2` only | **kept** | **exact** |
| `rs1≠x0` non-canonical | — | — | — | no effect (the spec says so; nothing is invalidated) |

The case checks the conservative direction explicitly: after a per-ASID fence,
an entry for a *different* ASID and a **global** entry tagged with the fenced
ASID both still hit with **no PTE read**, while the matching non-global entries
walk.

A **privilege or `mstatus` (SUM/MXR) change is deliberately not an invalidation
source**: the translation is a property of the page table and the ASID, and the
privilege-dependent part (the permission decision) is re-evaluated on every hit.
The spec says those changes take effect immediately *without* an SFENCE.VMA, and
re-evaluating the decision is what makes that true. A **`satp` write** *does*
flush the whole cache here: the spec does not require it, but flushing
conservatively means no translation outlives the context that produced it.

## Hit and miss, proved by the memory port

A hit is proved the only honest way: **the second access reads no PTE**. The
case owns the cache's PTE port in a memory model, counts the reads, and requires
`≥3` on the first access (the three-level walk) and **`=0`** on the second. The
miss counter and hit counter are also required to be non-zero. The integrated
run repeats it through the core: the second load of the same page adds **no**
PTE read on the core's `dmem` port.

## A walk never installs a stale entry

Every invalidation bumps a generation counter; a walk is tagged with the
generation in force when the walker accepted it, and a result may be installed
only while that generation still holds. This is the spec's rule that an implicit
walk that began before an SFENCE.VMA must not create an entry that fence would
have invalidated (priv v1.12 §4.2.1). It is checked twice: a walk cancelled in
flight installs nothing (and the next access re-walks), and a walk that spans a
fence installs nothing (`o_stale_ctr` counts it; the next access re-walks).

## The walker's A/D update is what the entry carries

An entry whose cached `D` is clear is **not** used for a store: the store misses,
walks, and the walker's compare-and-set sets `D` in the PTE; the re-installed
entry is dirty and the next store hits. A load installs with `D` as the leaf had
it, so a load never marks a page dirty. The case reads the PTE back and requires
`D=1` after the store and unchanged after the load.

## The instruction, through the core

An M-mode setup programs `satp = Sv39 | ASID 1 | root`, a PMP TOR covering RAM,
and `mret`s to S-mode. The stub loads twice (the second a hit), patches the PTE
with an ordinary store, executes a real `SFENCE.VMA x0, x0`, and loads a third
time. The third load must observe the **new** mapping (value `0x2222BBBB` rather
than the old `0x1111AAAA`); the core's TLB counters require the fence to have
executed and the cache to have installed at least two translations and served at
least one hit. No trap is allowed. A cache that treats the fence as a no-op
returns the old value and fails here (the `SFENCE_NOOP` control).

## Mutants (controls)

`python3 tools/run_tlb_controls.py --profile p1`. Each is built from a deleted
build directory with its `-D` on the Verilator command line; the binary hash
differs from the shipping one; the run exits 1 with a named first failure.

| control | `-D` | sha256 | exit | first failure |
|---|---|---|---|---|
| shipping | — | `2116994c77ed` | 0 | — |
| SFENCE_NOOP | `MOSAIC_TLB_MUTANT_SFENCE_NOOP` | `ef17df04b4dc` | 1 | `directed/stale-after-fence`: the fence did nothing, the stale mapping survived |
| FENCE_WRONG_ADDR | `MOSAIC_TLB_MUTANT_FENCE_WRONG_ADDR` | `a9b2ce24cb3e` | 1 | `directed/fence-by-addr`: the by-address fence invalidated the wrong page |
| CANCEL_IGNORED | `MOSAIC_TLB_MUTANT_CANCEL_IGNORED` | `699a0abd0d11` | 1 | `directed/cancel`: a cancelled walk still installed |
| SATP_NO_FLUSH | `MOSAIC_TLB_MUTANT_SATP_NO_FLUSH` | `55f3183c3be0` | 1 | `directed/satp-write`: an entry survived the satp write |
| STORE_IGNORES_DIRTY | `MOSAIC_TLB_MUTANT_STORE_IGNORES_DIRTY` | `60aa3dadb0da` | 1 | `directed/ad-dirty`: a store was served from an entry whose D was clear |

The first is the card's central failure verbatim; the second is a fence that
wins the wrong page (over-fencing is legal, but this is not over-fencing); the
last three are the cancelled walk, the satp write and the A/D rule.

## RTL revisions compiled (sha256, first 16 hex)

| file | hash |
|---|---|
| `rtl/core/mosaic_tlb.sv` | `4d866c1a1dfdeb0f` |
| `rtl/core/mosaic_ptw.sv` | `4cc2995a3eef659d` |
| `rtl/core/mosaic_pkg.sv` | `215588f22cde73b8` |
| `rtl/core/mosaic_core.sv` | `4f4c3b9eef95b29c` |
| `rtl/core/mosaic_dispatch.sv` | `b34a034144d7b0b4` |
| `rtl/core/mosaic_csr.sv` | `246d1b37d201a46e` |
| `sim/tb/mosaic_core_tb.sv` | `2b5991e40e39e5eb` |
| `sim/unit/tb_core_tlb.cpp` | `1c6bdc24238a0408` |
| `config/csr/mode_su.json` | `19509f05a13fc593` |

`mosaic_core.sv` swaps its `mosaic_ptw` instance for `mosaic_tlb`, feeds it
`satp.ASID[15:0]` and the SFENCE.VMA/satp pulses, and exports the cache's
counters; `mosaic_dispatch.sv` carries the second operand and the two `has`
bits; `mosaic_csr.sv` makes `satp.ASID` writable (ASIDLEN = 16); `mosaic_pkg.sv`
gains `leaf_perm_ok` (shared by walker and cache) and the three decode bits.
The `sv39.walk_and_faults` interface `xl_attr_o` connection in the core and TB
was pre-existing on this revision (added by the I-045 lane).

## Cases re-run after the change

p1: `sv39.walk_and_faults`, `privilege.permission_matrix`, `core.mem_program`,
`core.corpus_sweep`, `store.wrong_path_visibility`, `lsu.byte_forwarding`,
`mmio.exactly_once`, `mem.visibility_provenance`, `fence.code_and_data_order` —
all PASS. p0: `privilege.permission_matrix`, `core.mem_program`,
`core.corpus_sweep`, `core.trap_csr_program`, `sv39.walk_and_faults` — all PASS.

`core.trap_csr_program` is a **p0** case: its directed check hardcodes the
p0 `mstatus` read-back `0x7eaa`, so under p1 (which makes the S-mode `mstatus`
bits writable, giving `0x7e7faa`) it fails on that constant. That is a
profile mismatch in the case's own expectation, not a translation regression;
it passes under p0, the profile I-045's report re-ran it under.

`sv39.walk_and_faults` did **not** need re-pointing. Its directed half drives
the standalone `mosaic_ptw`, which is untouched; its integrated half performs
each translated load/store once, so the cache still walks exactly once per
access and every architectural check (PA, cause, tval per row) is unchanged.

## Gates

`python3 tools/lint_rtl.py --profile p0` green (45 source files clean);
`slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl $(find rtl -name '*.sv' | sort)`
errors clean and `rtl/core/mosaic_tlb.sv` introduces no new style warning (its
`always_comb` blocks are named; the `STYLE-7`/`STYLE-16` warnings the run prints
are the project-wide ones the baseline carries);
`python3 tools/check_records.py` green; `make check` green (rc 0);
`python3 tools/check_event_contract.py --profile p0 --negative` 36/36.

## Not covered (honest list)

* **Instruction fetch is not translated** (I-045's report says so, and this
  package does not change it), so the cache is on the data path only. The
  question "is the TLB shared between fetch and data" is therefore not yet
  answerable: there is no fetch translation to share with. What was done instead
  is to make the cache a *single* instance on the one translation path that
  exists (`mosaic_tlb`, instantiated once by the core), so when the fetch path
  arrives it can share this instance rather than build a second one; the
  integration point is the arbiter that already serialises the one walker.
* **Single hart.** There is no cross-hart shootdown: the cache is private to the
  hart, as the spec requires, and an ASID is local to it. The multi-hart
  protocol is I-046's later work.
* **The caches are not in the path** (I-042's L1s are module-level), so no PTE or
  data access goes through a cache here; the PTE port is the walker's, merged
  onto `dmem`.
* **Implicit (PTE) accesses are still not checked against PMP** — I-045's stated
  gap, unchanged.
* **A store's translation is resolved at commit** (I-045's rule), and an SFENCE.VMA
  is fence-like: it drains the memory path, pulses the invalidation and then
  redirects, flushing everything younger. A walk already in flight is not
  terminated by the fence, only prevented from installing; that is one of the
  two behaviours the spec permits.
* **The A/D compare-and-set is not truly atomic** (one hart, one walker), as in
  I-045.
* **A store to a page whose cached entry has `D=0` misses and re-walks every
  time until the walk sets `D`** — correct, and observable, but not free.
