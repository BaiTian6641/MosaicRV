# V-020 — Forbidding silent exclusions and reference skips

Work package V-020. Inputs: the comparison rules per source/profile, ACT4's default
exclusions, and the Difftest waive/skip paths. Outputs: a machine-readable exclusion
ledger and a count-closure report. Pass: zero unregistered exclusions; no committed
feature can be accepted merely by a waiver; every new rule must pass a negative control.

This report describes the ledger (`config/validation/exclusion_ledger.json`), the checker
that gates it (`tools/check_exclusions.py`, wired into `make check`), the closure counts,
the control table with observed rejections, and — the paragraph this package exists to
produce — the largest exclusions the project is currently carrying.

Reproduce:

```
python3 tools/check_exclusions.py
python3 tools/check_exclusions.py --negative
make check-exclusions
make check
python3 tools/check_records.py
```

---

## 1. What the ledger is, and what it deliberately is not

The ledger is the machine-readable form of everything this project does **not** check. The
"not covered" lists that each delivered package's report carries are, in effect, this
project's exclusions; this file is where they stop being footnotes. An exclusion that is
not written here is an *unregistered* exclusion, and the checker treats that as a failure.

It deliberately does **not** judge whether a reason is true. That is what the cited report,
the cited case and the case's own mutants are for. The checker stops the ledger from
disagreeing with the registry and the recorded results; it cannot tell whether a
well-written reason is a good one.

## 2. The ledger's shape

One entry per exclusion, validated against `config/schema/exclusion_ledger.schema.json` by
the project's own dependency-free validator (`tools/mosaic/jsonschema_mini.py`), the same
one `config_check` uses for every other config document.

| field | meaning |
|---|---|
| `id` | stable identifier, `EX-NNN`, unique |
| `kind` | one of the six kinds below |
| `subjects` | the concrete things excluded (checker names, ACT4 suite names, file paths, field names). A grouped entry still enumerates every member here. |
| `reason` | why the exclusion exists — an exclusion with no reason is a silent skip |
| `spec_clause` | the clause that makes the behaviour a requirement, **or an explicit statement that no clause applies and why** |
| `alternative_verification` | `{status, cases, checks, note}`. `covered` names registry cases (which must exist **and have a recorded PASS**) or tool paths (which must exist, and a `tools/check_*.py` must be wired into the Makefile). `open` names nothing and says so in the note. |
| `affected_cases` | `[{case, fields}]`; every `case` must exist in `tests/unit/registry.json` |
| `affected_scope` | files, targets, suites or models, so a global exclusion still says what it concerns |
| `end_condition` | what would let the exclusion be removed — an empty end condition is an exclusion with no exit |

The six kinds are defined once in the ledger's `kind_semantics` block and once in the
schema; the checker refuses a ledger whose two definitions disagree, and refuses a kind
outside the enum:

- `disabled_checker` — a check that exists but is switched off or short-circuited;
- `skip` — a case, suite, phase, profile, seed or stimulus that is not run;
- `waiver` — a comparison deliberately not made for a named field, which **must** name
  independent verification that passes;
- `unimplemented_reference_feature` — a feature the DUT or a reference model lacks;
- `timeout` — a budget that ends a run and stands in for a verdict (honest only when a
  timeout is a FAIL, never a pass);
- `generation_failure` — a step that produces the tests and can drop one from the total.

## 3. How the checker decides pass or fail

`tools/check_exclusions.py` reads the ledger, the schema, `tests/unit/registry.json`,
`results/unit/<case>/result.json` and the Makefile, and applies eight rules:

1. the ledger validates against its schema, and the schema's kind enum, the ledger's
   `kind_semantics` and the checker's own list of kinds agree exactly;
2. every entry carries a reason, a specification basis, an affected scope and an end
   condition (empty fields are rejected by the schema's `minLength` and again by the
   checker);
3. `id`s are unique and match `EX-NNN`;
4. every `affected_cases` name exists in the registry;
5. every `alternative_verification` **case** exists in the registry *and* its recorded
   `result.json` verdict is `PASS` — an alternative verification that does not pass covers
   nothing; a missing result is treated as not passing;
6. every `alternative_verification` **check** exists on disk, and a check that is itself a
   gate (`tools/check_*.py`) is invoked by the Makefile;
7. a `covered` entry names at least one case or check; an `open` entry names neither; a
   `waiver` must be `covered` (the pass criterion forbids accepting a committed feature by
   a waiver alone);
8. **every registered case with no recorded PASS must be named by some entry.** A case with
   no result is an unregistered exclusion. This is the rule that makes "zero unregistered
   exclusions" mechanical rather than aspirational: `tlb.sfence_vma` is the one such case
   today, and `EX-014` carries it.

On success it prints the closure counts (below), so the number is visible rather than
implied. Exit status is 0 only when all eight rules hold.

### Closure counts (current)

```
ok   exclusions: 52 registered, 20 open, 32 covered -- disabled checker=4, skip=21,
     waiver=7, unimplemented reference feature=15, timeout=3, generation failure=2
ok   closure: checks disabled=4, cases skipped=30, fields waived=14
```

"checks disabled" counts `disabled_checker` entries; "cases skipped" counts the distinct
registry cases named by `skip` entries; "fields waived" counts the distinct
`(case, field)` pairs named by `waiver` entries. The counts are computed from the ledger,
so they cannot drift from it.

## 4. The control table (observed rejections)

`--negative` copies the ledger, schema, registry, recorded results, Makefile and `tools/`
into a sandbox, mutates **one input at a time**, and requires every mutation to be
rejected. A control that is merely present is not evidence; each line below was observed.
The sandbox copies `tools/` deliberately: without it every control would be rejected for a
missing tool rather than for its own defect, which would be a false pass.

```
negative controls: 13/13 illegal ledgers rejected

  rejected: exclusion with no reason                            [schema] $.exclusions[0].reason: shorter than minLength 1
  rejected: empty end condition                                 [schema] $.exclusions[0].end_condition: shorter than minLength 1
  rejected: unknown kind                                        [schema] $.exclusions[0].kind: 'vibes' not in enum
  rejected: duplicate id                                        [EX-001] duplicate id (also at index 0)
  rejected: exclusion with no subjects                          [schema] $.exclusions[0].subjects: array has 0 items
  rejected: covered alternative naming no case or check         [EX-001] covered but names neither a case nor a check
  rejected: alternative verification names a case that does not exist  [EX-001] names case no.such_case, which the registry does not define
  rejected: alternative verification names a check that does not exist [EX-001] names check tools/check_ghost.py, which does not exist
  rejected: waiver with no independent verification             [EX-001] a waiver must name independent verification that passes
  rejected: affected case that is not in the registry           [EX-001] affected_cases names case ghost.case, which the registry does not define
  rejected: open exclusion that claims coverage                 [EX-002] open but names ['fifo.backpressure']
  rejected: alternative verification names a case that fails    [EX-001] names case fifo.backpressure, whose recorded result is FAIL, not PASS
  rejected: registered case with no result that the ledger does not name  [unregistered] case ghost.unregistered has no recorded PASS and no ledger entry names it
```

The last control is the card's deliberate trigger: a registry case with no recorded result
that the ledger does not name is rejected as an unregistered exclusion. The
"names a case that fails" control mutates the *result* in the sandbox (the ledger is not
changed), so it proves the checker reads the recorded verdict rather than trusting the
ledger's claim.

## 5. The largest open exclusions the project is carrying

This is the useful paragraph: it says what this machine does not yet prove. Twenty of the
52 entries are `open` — covered by nothing that passes. Ranked by what they put at risk:

1. **No ACT4 ELF executes for any advertised p1 extension (`EX-012`).** The runner executes
   only the p0 applicable suites (I/M/Zicsr/Zifencei). A, C, Zicond, Zmmul and CMO are
   advertised at p1 because their implementation tasks are delivered, but the only evidence
   for them is the DUT's **own** directed unit cases (`amo.linearization`,
   `lrsc.reservation_progress`, `compressed.cross_boundary`, `muldiv.kill_and_edges`,
   `cache.refill_evict_fault`). There is no independent corpus for a single advertised p1
   extension. `V-043` is the package that closes this and it is not delivered.

2. **`mepc[1:0]` is read-only zero while p1 claims C (`EX-034`).** `config/csr/mode_m.json`
   encodes the IALIGN=32 rule and the generated mask zeroes both low bits. With IALIGN=16
   only `mepc[0]` may be read-only zero, so a trap taken on a 2-mod-4 PC is recorded rounded
   down. `compressed.cross_boundary` prints that rounding as a note instead of asserting it,
   and `core.event_payload` pins the p0 rule. A committed capability's trap state is wrong
   for the profile that claims it; the CSR config, the generated package and that case's
   expectation must move together.

3. **No independent reference has ever been compared against this core (`EX-041`, `EX-043`,
   `EX-044`).** NEMU is built but no MosaicRV Difftest adapter exists; Sail's capability is
   claimed from its shipped coverage, not a differential run; XiangShan is a second DUT used
   only for upstream calibration. The project's advertised capabilities rest on its own
   directed cases, its own shadow models and the host oracle. The harness can detect an
   injected mismatch (`harness.injected_mismatch`), but it injects the mismatch itself.

4. **The event interface has holes (`EX-035`, `EX-036`, `EX-037`, `EX-031`).** `MEM_VISIBLE`
   has no RTL producer; an asynchronous interrupt publishes no lane-0 event; `ev_trap_epc`,
   `ev_insn` and `ev_pc_after` are declared but absent; `ev_store_addr`/`ev_store_data` are
   unwired. A consumer watching only the event stream sees a PC jump with no record when an
   interrupt is taken, and the store payload is verified only through the memory image.

5. **The p13 store-to-read-only trap has no independent oracle (`EX-040`).** The DUT is the
   only model that treats `boot_rom` as read-only; Spike reports cause 6 where the
   specification requires cause 7 because the cross-check maps the region as unmapped. The
   DUT's own behaviour is checked, but the comparison is against itself.

6. **The campaign is one profile and one seed (`EX-015`, `EX-016`), `tlb.sfence_vma` has no
   result yet (`EX-014`), and there is no device watchdog (`EX-050`).** Most cases ran under
   `--profile p0` only; every randomised case ran at its registered seed 1; `tlb.sfence_vma`
   is registered and pending; a non-responding device hangs the memory path because p0 has
   no timeout on the serializer.

What the machine does prove is bounded by these: the p0 integer core and its unit cases are
verified from a clean build, but no advertised p1 extension has independent corpus
evidence, the event stream cannot yet describe every architectural effect, and the only
oracles this project has compared against are its own. That is the honest state V-020 is
supposed to make legible, and the ledger now carries it where a checker can refuse to let
it grow silently.

## 6. What the ledger does not claim

- It does not claim every `covered` entry is *sufficient*; it claims the named case exists
  and passes, and that the entry says what it covers.
- It does not re-derive the "not covered" prose of 56 reports; it carries the
  mechanism-level exclusions (checkers, skips, waivers, timeouts, generation steps) in full
  and the material declared capability gaps. A gap that no report declared and no mechanism
  creates is not in the ledger because nothing creates it.
- It does not replace the reports. The ledger is the index a checker can enforce; the
  reports remain the argument.

## 7. Gates

| gate | command | result |
|---|---|---|
| ledger checks out | `python3 tools/check_exclusions.py` | PASS — 52 entries, 0 unregistered |
| controls observed | `python3 tools/check_exclusions.py --negative` | PASS — 13/13 rejected |
| wired into the gate | `make check` | PASS (exit 0) |
| records still agree | `python3 tools/check_records.py` | PASS — 56 packages, 64 cases |

## 8. Revision

- The ledger covers 52 exclusions: 4 disabled checkers, 21 skips, 7 waivers, 15
  unimplemented reference features, 3 timeouts, 2 generation failures; 20 open, 32 covered.
- Two entries (`EX-031`, `EX-034`) were reclassified from `waiver` to
  `unimplemented_reference_feature`: an *open* waiver would be exactly the fail mode the
  card names (a committed feature accepted by a waiver alone), so the checker refuses it and
  the entries are carried as unimplemented features instead.
- `config/status/implementation_status.json` and `tests/unit/registry.json` were not
  edited; the registry is read, never written.
