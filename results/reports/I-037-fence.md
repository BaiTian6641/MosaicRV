# I-037 — FENCE and FENCE.I

Status: **registered case passes; four controls fail as required; gates green.**

    CASE=fence.code_and_data_order  ->  RESULT PASS fence.code_and_data_order
        checks=15 retires=33 fence_i_redirects=1 store_txns=5
        res_d=1 res_s=0x0000000000000022 cycles=156 seed=1   (exit 0)

Shipping binary `sha256 = 2d5b9e0474472e21afcf53f3cf5e63c225466f85a5bc724ecc52a2b6ae48717c`.

---

## 1. The rule, stated as a rule

This profile ships the **conservative** reading the work package asks for. For a
FENCE or FENCE.I *F* at the ROB head:

> **F may complete only when the memory path *F* orders is idle.** "Idle" is the
> conjunction of three facts about three structures, sampled in one cycle:
>
> * the **store queue holds nothing** — every store older than *F* has been
>   authorised (its instruction retired) *and* has left the queue, i.e. the
>   memory endpoint has accepted it;
> * the **load queue holds nothing** — every load older than *F* has produced its
>   result and that result has been handed to the writeback path;
> * the **memory endpoint has no transaction outstanding** — no accepted request
>   is in flight and no response is still held for a consumer.
>
> **While *F* is staged, no younger memory macro may enter a queue.** A load or
> store younger than *F* is refused at the single port both queues are fed from,
> so it cannot allocate until *F* has retired. Younger non-memory work still
> executes; it simply cannot retire past *F*, because *F* owns the ROB head.

FENCE and FENCE.I share the rule; FENCE.I adds the instruction-view
invalidation of §3. A FENCE that has completed but not yet retired still blocks
younger memory macros, so "the block" and "the drain" are two halves of one
statement about the same cycle window.

### 1.1 What the rule drains, and what it deliberately does not

| Structure | Drained? | Why |
|---|---|---|
| store queue | **yes** | It is the one place a *store* older than the fence can still be waiting to become visible. A fence that let a younger access proceed while an older store was queued would be exactly the card's second fail mode. |
| memory endpoint | **yes** | A store leaves the store queue when the endpoint *accepts* it, so "the store queue is empty" does not by itself mean the memory system has performed the store. The endpoint's own `busy` — accepted request in flight or response still held — is the missing conjunct, and it is why `o_mem_lsu_busy` was brought out of the core for the case to check. |
| load queue | **yes** | The strong reading of "every older access has completed": a load older than the fence must finish before any younger access becomes visible. Under this machine's single in-order endpoint a prior load cannot actually be reordered behind a younger store, so this conjunct is belt-and-braces — but the card asks for "every relevant older access", and a load queue that is not empty is a prior access that has not completed. |
| clusters' issue queues | **no** | An ALU/branch/mul-div uop in flight has no memory effect, and the ROB already prevents it from retiring past the fence. Draining them would cost cycles and buy nothing. |
| fetch, decode buffer, rename | **no** for FENCE | FENCE has no instruction-view obligation. Treating it as one would be the card's explicit error ("FENCE must not be equated with a cache flush, and must not be treated as an automatic cross-hart I-cache synchronisation"). |

### 1.2 "No longer ordering-safe", concretely, for each structure

* **Store queue.** An entry is ordering-safe when it is authorised and will leave
  in program order. A fence is complete only when the queue is *empty*, so no
  older store is left to become visible after a younger access. If a fence
  completed with any entry present, the queue would be holding a store whose
  visibility the fence has just promised — the promise would be false.
* **Endpoint.** The endpoint is ordering-safe when it owns no transaction: the
  module accepts one request at a time and refuses a second until the first
  response is taken, so "not busy" is exactly "the order the memory saw is the
  order it was given, and nothing is still in it".
* **Fetch (FENCE.I only).** The hart's instruction view is ordering-safe when
  every byte the front end has *already delivered* has been thrown away and any
  new fetch is issued under an epoch that post-dates the publishing store. §3.

### 1.3 What FENCE deliberately does not do

* It is **not** a cache flush. The p0 memory system has no data cache, so there
  is nothing to flush; the rule is stated in terms of the queues and the
  endpoint, which are the structures that exist.
* It is **not** an automatic cross-hart instruction-cache synchronisation.
  `FENCE.I` invalidates *this hart's* instruction view and nothing else; there is
  no snoop, no broadcast and no other hart in p0.
* It **ignores the `fm`/`pred`/`succ` fields**. A full fence is strictly stronger
  than any of the weak orderings those fields can describe, so ignoring them is
  conservative, not permissive. In particular **`fence.tso` is treated as
  `fence`** and is not distinguished — see §7.

---

## 2. How FENCE is wired in

* `mosaic_decoder` already decoded `OP-MISC-MEM` funct3 000/001 into
  `is_miscmem`/`is_fence_i`; no decoding change was needed.
* `mosaic_dispatch` no longer refuses `is_miscmem`. A fence is allocated into the
  ROB like any macro and leaves through the **system insert port**, carrying the
  two class bits `sys_ins_is_fence`/`sys_ins_is_fence_i`. (The system port is the
  one place a macro is resolved at the architectural boundary, which is exactly
  where a fence's ordering is decidable.)
* `mosaic_core` stages the two bits with the rest of the system payload
  (`sys_fence_q`, `sys_fence_i_q`), and the system unit gains the drain gate:
  `sys_exec` for a fence-like macro is additionally gated on `mem_path_idle`, and
  the memory insert port (`disp_mem_ready`) is refused while a fence is staged.
* The completion path is unchanged: the fence's completion is an ordinary
  writeback event with no destination, so the ROB marks the entry done and the
  fence retires as an ordinary instruction with no register write.

---

## 3. FENCE.I: the instruction-view invalidation

FENCE.I is implemented as the drain rule **plus a system redirect to its own
`pc + 4`**.

* **What is invalidated.** On the cycle FENCE.I retires, the redirect arbiter
  acts on the system request; the registered redirect that follows drives
  `fetch_redir_valid`, `dbuf_purge`, the cluster flush, the ROB flush, both
  memory queues' flush and the rename restore. That single redirect:
  * advances the fetch unit's **epoch** and marks every outstanding fetch slot
    `cancelled`, so any response still in flight for a request issued before the
    store is classified `stale`/`squashed` and dropped rather than delivered —
    the same authoritative mechanism `CASE=fetch.redirect_late_response` tests
    directly;
  * purges the decode buffer and the whole ROB at and above the head, discarding
    every instruction the front end had already delivered;
  * restores the rename state to the committed map (`sys_redirect_kill`/full
    restore, the MRET/trap path), so retired state is untouched and speculative
    state is gone.
  The fetch that follows re-requests `pc + 4` under the new epoch.
* **Why it cannot execute a stale byte.** Two independent reasons:
  1. the publishing store is *older* than FENCE.I and the drain rule (§1) means
     the store has been accepted by the endpoint — the backing memory already
     holds the new bytes — before FENCE.I completes;
  2. every instruction fetched before the invalidation is discarded by the
     redirect (pipeline purge + fetch-slot cancel), so the only bytes that can
     execute at or after `FENCE.I + 4` are bytes fetched *after* the store was
     visible.
* **The lane-1 barrier.** `mosaic_core` already kept lane 1 from retiring behind a
  taken branch whose redirect was pending (`head_pending_taken`). FENCE.I is the
  same hazard with a sharper consequence — the instruction in lane 1 is very
  often the *stale* one the fence exists to invalidate — so `head_fence_i_pending`
  suppresses lane 1 while a FENCE.I is the head. Without it, the instruction after
  FENCE.I retired in the same cycle (the ROB retires two per cycle) before the
  registered redirect could flush it, and the stale byte committed. This was
  observed, not hypothesised: the first version of the case executed `0x11` at
  FIXPC.

---

## 4. The case

`CASE=fence.code_and_data_order`, driver `sim/unit/tb_core_fence.cpp`, top
`mosaic_core_tb`. Two hand-encoded programs run from the reset vector in one
image; instruction fetch and data access read and write **one shared memory**, so
a store to a code address is visible to the next fetch.

**Data-ordering program.** A device register at `0x80003000` answers a read with
the number of store transactions the memory system has performed so far (a
counter in this driver's own memory model — the expectation is this file's
model, not the DUT's). The program reads it, performs a store, executes `FENCE`,
reads it again, and publishes the difference. The difference is 1 only if the
fence let the older store complete before the younger load did. To make the
ordering *value-observable* rather than merely a timing property, the store is
preceded by an independent `div`: the divide is iterative (64 steps) and the ROB
retires in order, so the store cannot retire — and so cannot drain — until the
divide writes back, while a younger load can allocate and issue immediately. A
younger load that got past the fence therefore reads the device before the
publish and the difference is 0.

**Self-modifying program.** Stores a new word (`addi t2, x0, 0x22`) over the word
immediately after FENCE.I, executes FENCE.I, then executes that word. The
original word (`addi t2, x0, 0x11`) was fetched before the store took effect,
because the front end runs ahead of the ROB by the depth of the decode buffer and
dispatch queue. A FENCE.I that does not invalidate the delivered view executes the
stale `0x11`; a correct one executes `0x22`.

**Expectations, none from the DUT:** (i) the two published results — 1 and 0x22 —
are this driver's model's; (ii) the per-instruction retirement stream is compared
against a small independent RV64IM interpreter in the same file, which decodes
the same words from the ISA text, executes loads/stores against its own memory,
models the device itself, and treats FENCE/FENCE.I as ordering no-ops (it always
reads current code, so it predicts the patched instruction).

**What is checked:** the retirement stream (pc, rd, value) in order; the two
published results; **that every FENCE/FENCE.I retires with the store queue empty,
the load queue empty and the endpoint not busy** (the card's second fail mode);
that FENCE.I issues exactly one redirect and that the first redirect of the
program is FENCE.I's and names `FENCE.I + 4`; that the program reached its exit
protocol (`TOHOST = 1`); and that no macro was refused as unsupported and no
exception trap was taken.

---

## 5. Mutants

All built from a **deleted** build directory with the `-D` on that build's
Verilator command line, by `python3 tools/run_fence_controls.py`. Shipping binary
`sha256 = 2d5b9e0474472e21afcf53f3cf5e63c2…`.

| Mutant (`-DMOSAIC_CORE_MUTANT_…`) | Defect | sha256 (first 16) | Exit | First failure |
|---|---|---|---|---|
| `FENCE_EARLY` | FENCE completes at the ROB head instead of waiting for the drain | `0feffc15c9eb8139` | 1 | `FENCE retires with the memory path drained: at the retirement: store queue=0 load queue=0 endpoint busy=1` |
| `FENCEI_NO_INVALIDATE` | FENCE.I completes like a plain fence and never redirects, so the delivered instruction view survives | `6b45390360f9d7ac` | 1 | `retire 24 at 0x80000060 value for x10 expected 0x22, got 0x11` (the stale byte executes) |
| `FENCEI_SKIP` | FENCE.I resumes at `pc + 8`, so the instruction after it is lost | `61d8b960a91af00f` | 1 | `the first redirect is FENCE.I's and names the instruction after it: got a redirect to 0x80000064, … next instruction is 0x80000060` |
| `FENCE_ACCESS_PAST` | younger **loads** are not blocked while a fence is staged | `750432eb616eee87` | 1 | `retire 13 at 0x80000034 value for x30 expected 0x1, got 0x0` (the younger load read the device before the publish) |

The four binaries differ from the shipping one and each exits 1 on its named
first failure. `FENCE_ACCESS_PAST` blocks younger *stores* as well as loads in the
shipping rule; the mutant releases only loads, because releasing stores as well
makes the fence's own "store queue empty" conjunct unsatisfiable and the machine
deadlocks (a real finding: the block is load-bearing for *liveness*, not only for
ordering). The first draft of this mutant released both and deadlocked at
`head_pc=0x80000028` with two stores queued.

---

## 6. Defect found while building the case

**A memory macro inserted in the same cycle as a redirect survived the flush.**
The store queue's squash covers the entries *resident when it is applied*; an
entry appended in that same cycle is appended after the survivors are computed
and is kept. The macro at the dispatch head is always younger than the
redirecting instruction, so it is dead work, but its ROB entry is freed by the
flush while its store-queue entry stays. It is then never authorised, so it
blocks the queue head for ever (the drain requires a non-empty authorised
prefix). The FENCE.I redirect exposed it: `sq` grew to 3, `commit`/`drain`
stopped at 3, and the case stalled.

Fixed in the core by refusing the memory insert in the flush cycle
(`!rob_flush_pulse` conjunct in `disp_mem_ready`) — the same cycle window the
`recovering` gate already covers for later cycles. This is a pre-existing
integration defect in the memory path, not a fence defect; it is reachable
whenever a redirect and a memory insert coincide. It is recorded here rather than
in I-033/I-034/I-035 because this case found it.

---

## 7. NOT verified

* **Cross-hart ordering is not exercised — it cannot be.** p0 is one hart. There
  is no second hart, no snoop and no shared bus, so "FENCE is not an automatic
  cross-hart I-cache synchronisation" is a statement about what the implementation
  does *not* contain, and this case can only confirm the absence by construction
  (FENCE touches the fetch view for nothing; FENCE.I invalidates exactly this
  hart's view). A multi-hart case cannot exist at this profile.
* **`fence.tso` is not distinguished.** The decoder accepts `funct3 = 000` for
  any `fm`, dispatch stages it as a plain fence, and the rule ignores
  `fm`/`pred`/`succ`. A `fence.tso` therefore drains and blocks exactly like a
  full fence — stronger than TSO requires, never weaker — but the case does not
  contain one and does not measure the difference.
* **Instruction-fetch ordering of `FENCE.I` is not measured through a stale
  instruction that arrives from the *memory model* rather than the core's own
  pipeline.** The stale bytes in this case come from the front end's delivered
  view (the decode buffer/dispatch queue), which is the hazard the mechanism
  addresses. A responder that answered a fresh `pc + 4` request with pre-store
  bytes would be a harness fault, not a hart fault.
* **The fetch unit's outstanding-request table is exercised only indirectly.**
  The invalidation drives the fetch unit's authoritative `redirect_valid`, which
  is the same path (epoch advance + per-slot `cancelled`) that
  `CASE=fetch.redirect_late_response` examines directly; this case does not read
  the fetch unit's cancellation counters, so "a slot was outstanding at the
  invalidation" is `[INFERENCE]` from the front end's run-ahead, not measured
  here. The instruction-invalidation claim itself does not depend on it: the
  decode buffer and ROB purge is what discards the delivered word.
* **The fence's drain rule is exercised with a store that is *older*, never with
  a fence that must order a prior *load* against a younger store**, because the
  single in-order endpoint makes an older load complete before a younger store
  can be authorised. The load-queue conjunct of the rule is therefore checked for
  emptiness but its ordering effect is not independently stressed.
* **No MMIO/LRSC/AMO interaction.** The device register lives in ordinary RAM in
  the driver's model; the profile has no LR/SC, no AMO and no cache, and the
  rule's treatment of them is not tested.

---

## 8. Gates

| Command | Result |
|---|---|
| `python3 tools/lint_rtl.py --profile p0` | 33 source files clean |
| `slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl $(find rtl -name '*.sv' \| sort)` | zero errors (only the pre-existing `mosaic_skid_buffer.sv` STYLE notices) |
| `python3 tools/run_unit.py --profile p0 --case fence.code_and_data_order` | PASS, exit 0 |
| `python3 tools/run_fence_controls.py` | all four observable controls OK |

Regression cases re-run and passing: `fabric.fixed_two_cluster`,
`core.corpus_branch`, `core.unwritten_reg_read`, `core.mem_program`,
`core.trap_csr_program`, `store.wrong_path_visibility`, `lsu.byte_forwarding`,
`lsu.size_fault_boundaries`.

---

## 9. How to reproduce

```sh
# the case (builds, runs, prints one RESULT line)
python3 tools/run_unit.py --profile p0 --case fence.code_and_data_order

# shipping and the four mutants, each from a deleted build directory
python3 tools/run_fence_controls.py
python3 tools/run_fence_controls.py --only FENCEI_SKIP

# the gates
python3 tools/lint_rtl.py --profile p0
slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl $(find rtl -name '*.sv' | sort)
```

Files touched: `rtl/core/mosaic_dispatch.sv`, `rtl/core/mosaic_core.sv`,
`sim/tb/mosaic_core_tb.sv`, `sim/unit/tb_core_fence.cpp`,
`tools/run_fence_controls.py`.
