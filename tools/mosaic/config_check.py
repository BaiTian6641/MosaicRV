"""Load and check the frozen MosaicRV profile/ISA/PMA/CSR/geometry configuration.

Three layers of checking, all of which must pass:

1. **Schema** — each document validates against its JSON Schema
   (`config/schema/*.json`) using the dependency-free subset validator.
2. **Semantic** — rules a schema cannot express: PMA legality, region overlap,
   CSR field legality, geometry consistency, and the rule that a profile may
   never claim a capability above its own rung.
3. **Derivation** — `misa`, the ISA string and the build manifest are *derived*
   from the same profile + ladder + implementation-status inputs, so the DUT,
   the compiler flags and the reference model can never disagree.

Nothing here consults the implementation. A profile declares a target; whether
that target may be *advertised* is decided by
`config/status/implementation_status.json`, which records delivered work
packages. Advertising a capability whose implementation tasks have not delivered
is a hard failure, not a warning.
"""

from __future__ import annotations

import hashlib
import json
import os
from typing import Any, Dict, List, Optional, Tuple

from . import jsonschema_mini

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
CONFIG_ROOT = os.path.join(REPO_ROOT, "config")
SCHEMA_ROOT = os.path.join(CONFIG_ROOT, "schema")

PROFILE_ORDER = ["p0", "p1", "p2", "p3"]

# Devices the SoC actually implements. A memory map naming any other device is
# rejected: an unimplemented device must not appear in a published map, or the
# platform contract silently becomes a lie (docs/implementation-plan.md I-001).
KNOWN_DEVICES = frozenset(["uart", "test_harness", "clint", "msip"])

# RISC-V misa extension bit assignments (ISA manual, "misa" register table).
# Zicsr/Zifencei/Zicntr/Zihpm are deliberately absent: since those extensions were
# split out of the base ISA they have no misa bit.
MISA_BITS = {
    "A": 0,
    "B": 1,
    "C": 2,
    "D": 3,
    "F": 5,
    "H": 6,
    "I": 8,
    "J": 9,
    "M": 12,
    "N": 13,
    "P": 15,
    "Q": 16,
    "S": 18,
    "U": 20,
    "V": 21,
    "W": 22,
}
MISA_MXL_SHIFT = 62

# Architectural integer register count for RV64; x0 is never renamed.
ARCH_INT_REGS = 32

# Registers that hold a program counter. Their low bits are the instruction-
# address alignment (IALIGN) rule's, and IALIGN is a property of the profile:
# Priv v1.12 says "The low bit of mepc (mepc[0]) is always zero. On
# implementations that support only IALIGN=32, the two low bits (mepc[1:0]) are
# always zero", and the same for sepc. The alignment bits of these registers are
# therefore *derived* from the profile's claimed extensions (see `ialign`) and
# declared as such in the table (`ialign_bits`), never frozen as writable or
# unmodifiable -- freezing them is exactly the EX-034 defect.
PC_VALUED_CSRS = ("mepc", "sepc")


class Problem(object):
    __slots__ = ("where", "message")

    def __init__(self, where: str, message: str) -> None:
        self.where = where
        self.message = message

    def __str__(self) -> str:
        return "%s: %s" % (self.where, self.message)

    def as_dict(self) -> Dict[str, str]:
        return {"where": self.where, "message": self.message}


class Bundle(object):
    """Everything one profile needs, plus every problem found while gathering it."""

    def __init__(self, name: str) -> None:
        self.name = name
        self.problems: List[Problem] = []
        self.profile: Optional[Dict[str, Any]] = None
        self.ladder: Optional[Dict[str, Any]] = None
        self.memory: Optional[Dict[str, Any]] = None
        self.geometry: Optional[Dict[str, Any]] = None
        self.csr_tables: List[Dict[str, Any]] = []
        self.csr_rules: Optional[Dict[str, Any]] = None
        self.status: Optional[Dict[str, Any]] = None
        self.input_hashes: Dict[str, str] = {}

    def fail(self, where: str, message: str) -> None:
        self.problems.append(Problem(where, message))

    @property
    def ok(self) -> bool:
        return not self.problems

    @property
    def csr_count(self) -> int:
        return sum(len(block["csrs"]) for table in self.csr_tables for block in table["modes"])


# ---------------------------------------------------------------------------
# loading
# ---------------------------------------------------------------------------


def _load_json(path: str, bundle: Bundle, where: str) -> Optional[Dict[str, Any]]:
    try:
        with open(path, "r") as handle:
            raw = handle.read()
    except OSError as exc:
        bundle.fail(where, "cannot read %s: %s" % (os.path.relpath(path, REPO_ROOT), exc))
        return None
    bundle.input_hashes[os.path.relpath(path, REPO_ROOT)] = hashlib.sha256(
        raw.encode("utf-8")
    ).hexdigest()
    try:
        return json.loads(raw)
    except ValueError as exc:
        bundle.fail(where, "invalid JSON in %s: %s" % (os.path.relpath(path, REPO_ROOT), exc))
        return None


def _validate(bundle: Bundle, document: Any, schema_name: str, where: str) -> None:
    schema = _load_json(os.path.join(SCHEMA_ROOT, schema_name), bundle, "%s schema" % where)
    if schema is None:
        return
    for path, message in jsonschema_mini.validate(document, schema):
        bundle.fail("%s%s" % (where, path), message)


def load(name: str, config_root: Optional[str] = None) -> Bundle:
    """Gather and check every configuration input for profile `name`."""
    bundle = Bundle(name)
    if name not in PROFILE_ORDER:
        bundle.fail(
            "profile",
            "unknown profile %r; known profiles are %s" % (name, ", ".join(PROFILE_ORDER)),
        )
        return bundle

    root = config_root or CONFIG_ROOT

    profile = _load_json(os.path.join(root, "profiles", "%s.json" % name), bundle, "profile")
    if profile is None:
        return bundle
    bundle.profile = profile
    _validate(bundle, profile, "profile.schema.json", "profile")

    bundle.ladder = _load_json(
        os.path.join(root, "capability_ladder.json"), bundle, "capability ladder"
    )

    memory_rel = profile.get("memory_map_file")
    if memory_rel:
        bundle.memory = _load_json(os.path.join(root, memory_rel), bundle, "memory map")
        if bundle.memory is not None:
            _validate(bundle, bundle.memory, "memory_map.schema.json", "memory map")

    geometry_rel = profile.get("geometry_file")
    if geometry_rel:
        bundle.geometry = _load_json(os.path.join(root, geometry_rel), bundle, "geometry")
        if bundle.geometry is not None:
            _validate(bundle, bundle.geometry, "geometry.schema.json", "geometry")

    for csr_rel in profile.get("csr_files", []):
        table = _load_json(os.path.join(root, csr_rel), bundle, "csr table %s" % csr_rel)
        if table is None:
            continue
        bundle.csr_tables.append(table)
        _validate(bundle, table, "csr.schema.json", "csr table %s" % csr_rel)

    status_path = os.path.join(root, "status", "implementation_status.json")
    if os.path.exists(status_path):
        bundle.status = _load_json(status_path, bundle, "implementation status")

    # The CSR legality-relationship rule ledger (V-017). It is additive to the
    # implementation table: the table says which bits are writable, the ledger
    # says which *relationship* each bit obeys, which clause licenses it, and the
    # positive and negative example the comparator is checked by. The case
    # csr.rule_ledger is generated from this file and must visit every rule, so a
    # rule with no reachable stimulus is a failure rather than a skip.
    ledger_path = os.path.join(root, "csr", "rule_ledger.json")
    if os.path.exists(ledger_path):
        ledger = _load_json(ledger_path, bundle, "csr rule ledger")
        if ledger is not None:
            bundle.csr_rules = ledger
            _validate(bundle, ledger, "csr_rules.schema.json", "csr rule ledger")

    _check_capabilities(bundle)
    _check_memory(bundle, name)
    _check_geometry(bundle, name)
    _check_csr(bundle, name)
    _check_csr_rules(bundle, name)
    _check_cross_references(bundle)
    return bundle


# ---------------------------------------------------------------------------
# capability ladder
# ---------------------------------------------------------------------------


def _ladder_index(bundle: Bundle) -> Dict[str, Any]:
    caps: Dict[str, Any] = {}
    if bundle.ladder:
        for cap in bundle.ladder.get("capabilities", []):
            caps[cap["name"]] = cap
    return caps


def _check_capabilities(bundle: Bundle) -> None:
    if not bundle.profile or not bundle.ladder:
        return
    caps = _ladder_index(bundle)
    profile_name = bundle.name
    profile_rank = PROFILE_ORDER.index(profile_name)
    claimed = bundle.profile["isa_target"]["extensions"]
    modes = bundle.profile["privilege_modes"]
    isa = bundle.profile["isa_target"]

    for name in claimed:
        cap = caps.get(name)
        if cap is None:
            bundle.fail(
                "profile %s" % profile_name,
                "claims capability %r which has no row in config/capability_ladder.json" % name,
            )
            continue
        if PROFILE_ORDER.index(cap["min_profile"]) > profile_rank:
            bundle.fail(
                "profile %s" % profile_name,
                "claims %r but that capability first exists at %s; advertising ahead of "
                "implementation is the exact failure docs/implementation-plan.md#1.1 forbids"
                % (name, cap["min_profile"]),
            )
        if not cap.get("impl_tasks"):
            bundle.fail("capability %s" % name, "has no implementation task; unbacked claim")
        if not cap.get("verify_tasks"):
            bundle.fail("capability %s" % name, "has no verification task; unbacked claim")

    for mode in modes:
        cap = caps.get(mode)
        if cap is None:
            bundle.fail(
                "profile %s" % profile_name,
                "claims privilege mode %r with no capability ladder row" % mode,
            )
            continue
        if PROFILE_ORDER.index(cap["min_profile"]) > profile_rank:
            bundle.fail(
                "profile %s" % profile_name,
                "claims privilege mode %r but that mode first exists at %s"
                % (mode, cap["min_profile"]),
            )

    # Zicsr and Zifencei are a pair in RVA23; neither may appear alone.
    has_zicsr = "Zicsr" in claimed
    has_zifencei = "Zifencei" in claimed
    if has_zicsr != has_zifencei:
        bundle.fail(
            "profile %s" % profile_name,
            "Zicsr and Zifencei must be claimed together (RVA23U64 mandatory pair), got "
            "zicsr=%s zifencei=%s" % (has_zicsr, has_zifencei),
        )

    has_v = "V" in claimed
    if has_v and not isinstance(isa["vlen"], int):
        bundle.fail("profile %s" % profile_name, "claims V but vlen is not an integer")
    if not has_v and isa["vlen"] is not None:
        bundle.fail("profile %s" % profile_name, "declares vlen without claiming V")
    if has_v and isa["elen"] not in (32, 64):
        bundle.fail("profile %s" % profile_name, "claims V but elen must be 32 or 64")
    if isinstance(isa["vlen"], int) and isa["vlen"] < 128:
        bundle.fail("profile %s" % profile_name, "VLEN must be at least 128 for RVA23")

    table_modes = set()
    for table in bundle.csr_tables:
        for block in table.get("modes", []):
            table_modes.add(block["mode"])
    for mode in modes:
        if mode not in table_modes:
            bundle.fail(
                "profile %s" % profile_name,
                "privilege mode %r is claimed but no CSR table defines it" % mode,
            )


# ---------------------------------------------------------------------------
# PMA / memory map
# ---------------------------------------------------------------------------


def _check_memory(bundle: Bundle, profile_name: str) -> None:
    memory = bundle.memory
    if memory is None:
        return

    regions = memory["regions"]
    seen_names = set()
    for index, region in enumerate(regions):
        where = "memory map region[%d] %s" % (index, region.get("name", "?"))
        name = region.get("name")
        if name in seen_names:
            bundle.fail(where, "duplicate region name %r" % name)
        seen_names.add(name)

        base = region["base"]
        size = region["size"]
        if base + size > (1 << 64):
            bundle.fail(where, "region wraps the 64-bit physical address space")
        if size & (size - 1) != 0:
            bundle.fail(where, "region size %d is not a power of two" % size)
        if base % size != 0:
            bundle.fail(where, "region base 0x%x is not aligned to its size 0x%x" % (base, size))

        # PMA legality from the RISC-V privileged specification: an
        # implementation must not treat non-idempotent memory as cacheable, must
        # not execute a region it cannot read, and must not promise atomicity it
        # cannot hold.
        if region["cacheable"] and not region["idempotent"]:
            bundle.fail(where, "cacheable region is not idempotent; illegal PMA combination")
        if region["executable"] and not region["readable"]:
            bundle.fail(where, "executable region is not readable; instruction fetch cannot work")
        if region["kind"] == "rom" and region["writable"]:
            bundle.fail(where, "rom region is writable")
        if region["kind"] == "mmio" and region["cacheable"]:
            bundle.fail(where, "mmio region must not be cacheable")
        if not (region["readable"] or region["writable"]) and region["error_response"] == "none":
            bundle.fail(where, "region denies both read and write but declares no error response")

        granule = region["atomic_granule"]
        if granule:
            if granule & (granule - 1) != 0:
                bundle.fail(where, "atomic_granule %d is not a power of two" % granule)
            elif granule > 8:
                bundle.fail(
                    where,
                    "atomic_granule %d exceeds the 8-byte maximum naturally aligned RISC-V "
                    "access; declare a smaller guarantee or serialize externally" % granule,
                )
            elif base % granule != 0:
                bundle.fail(
                    where,
                    "atomic_granule %d but base 0x%x is not aligned to it" % (granule, base),
                )
            elif size % granule != 0:
                bundle.fail(
                    where,
                    "atomic_granule %d but size 0x%x is not a multiple of it" % (granule, size),
                )

        if region["kind"] == "mmio":
            device = region.get("device")
            if not device:
                bundle.fail(where, "mmio region does not name a device")
            elif device not in KNOWN_DEVICES:
                bundle.fail(
                    where,
                    "mmio region names device %r which the SoC does not implement (known: %s)"
                    % (device, ", ".join(sorted(KNOWN_DEVICES))),
                )
        elif region.get("device"):
            bundle.fail(where, "non-mmio region must not name a device")

    ordered = sorted(regions, key=lambda r: r["base"])
    for left, right in zip(ordered, ordered[1:]):
        if left["base"] + left["size"] > right["base"]:
            bundle.fail(
                "memory map",
                "regions %s [0x%x,0x%x) and %s [0x%x,0x%x) overlap"
                % (
                    left["name"], left["base"], left["base"] + left["size"],
                    right["name"], right["base"], right["base"] + right["size"],
                ),
            )


# ---------------------------------------------------------------------------
# CSR tables
# ---------------------------------------------------------------------------


def _parse_bit_ranges(spec: List[str], width: int, field: str, where: str,
                      bundle: Bundle) -> List[Tuple[int, int]]:
    ranges: List[Tuple[int, int]] = []
    for item in spec:
        if ":" in item:
            msb_text, lsb_text = item.split(":", 1)
        else:
            msb_text = lsb_text = item
        try:
            msb = int(msb_text, 10)
            lsb = int(lsb_text, 10)
        except ValueError:
            bundle.fail(where, "field %s has non-numeric bit range %r" % (field, item))
            continue
        if msb < lsb:
            msb, lsb = lsb, msb
        if lsb < 0 or msb >= width:
            bundle.fail(
                where,
                "field %s range %r lies outside the declared width %d" % (field, item, width),
            )
            continue
        ranges.append((msb, lsb))
    return ranges


def _ranges_overlap(a: List[Tuple[int, int]], b: List[Tuple[int, int]]) -> bool:
    for a_hi, a_lo in a:
        for b_hi, b_lo in b:
            if a_lo <= b_hi and b_lo <= a_hi:
                return True
    return False


def _check_csr(bundle: Bundle, profile_name: str) -> None:
    claimed_modes = set(bundle.profile["privilege_modes"]) if bundle.profile else set()

    # A profile may leave a CSR address range deliberately unimplemented. That is a
    # legitimate, common state -- a profile that does not implement two-stage
    # translation has no hypervisor CSRs at all -- but it must be *declared*, so
    # the absence can be checked rather than merely asserted in prose, and so the
    # CSR unit can be told which numbers must raise illegal instruction.
    absent = []
    for table in bundle.csr_tables:
        for span in table.get("absent_csr_ranges", []):
            first, last = span["first"], span["last"]
            if first > last:
                bundle.fail("csr table absent range",
                            "range 0x%03x..0x%03x has first > last" % (first, last))
                continue
            absent.append((first, last, span.get("reason", "")))

    for table in bundle.csr_tables:
        if profile_name not in table.get("profiles", []):
            bundle.fail(
                "csr table",
                "table does not list profile %r in its `profiles` array, so it may not be "
                "used by that profile" % profile_name,
            )
        if not table["modes"] and not table.get("absent_csr_ranges"):
            bundle.fail(
                "csr table",
                "defines no CSRs and declares no absent ranges; an empty table must say "
                "what it is deliberately leaving out and why",
            )
        for mode_block in table["modes"]:
            mode = mode_block["mode"]
            if mode not in claimed_modes:
                bundle.fail(
                    "csr table mode %s" % mode,
                    "profile %r does not claim privilege mode %s, so this table may not "
                    "define it" % (profile_name, mode),
                )
            by_address: Dict[int, Any] = {}
            for csr in mode_block["csrs"]:
                where = "csr %s.%s" % (mode, csr.get("name", "?"))
                address = csr["address"]
                width = csr["width"]
                behavior = csr["behavior"]
                access = csr["access"]

                if address in by_address:
                    bundle.fail(
                        where,
                        "address 0x%03x already used by %r in the same privilege mode"
                        % (address, by_address[address]),
                    )
                by_address[address] = csr.get("name")

                if csr["reset"] >= (1 << width):
                    bundle.fail(
                        where,
                        "reset value 0x%x does not fit in the declared %d-bit width"
                        % (csr["reset"], width),
                    )
                if access == "reserved":
                    bundle.fail(where, "a published CSR table may not declare a reserved address")
                if behavior == "fixed" and access != "ro":
                    bundle.fail(where, "fixed-behaviour CSR must be read-only, got %r" % access)
                if behavior == "fixed" and csr.get("writable_fields"):
                    bundle.fail(where, "fixed-behaviour CSR must not declare writable fields")

                writable = _parse_bit_ranges(
                    csr.get("writable_fields", []), width, "writable_fields", where, bundle
                )
                wpri = _parse_bit_ranges(
                    csr.get("wpri_fields", []), width, "wpri_fields", where, bundle
                )
                fixed = _parse_bit_ranges(
                    csr.get("unmodifiable_bits", []), width, "unmodifiable_bits", where, bundle
                )
                ialign_bits = _parse_bit_ranges(
                    csr.get("ialign_bits", []), width, "ialign_bits", where, bundle
                )
                if _ranges_overlap(writable, wpri):
                    bundle.fail(where, "writable_fields and wpri_fields overlap")
                if _ranges_overlap(writable, fixed) or _ranges_overlap(wpri, fixed):
                    bundle.fail(where, "declared fields overlap unmodifiable_bits")
                if _ranges_overlap(ialign_bits, writable) or _ranges_overlap(ialign_bits, wpri) \
                        or _ranges_overlap(ialign_bits, fixed):
                    bundle.fail(
                        where,
                        "ialign_bits overlaps a declared writable, WPRI or unmodifiable "
                        "field; a bit's writability is either declared here or derived "
                        "from the profile's IALIGN, never both",
                    )

                # The IALIGN rule. A program-counter register must say which of
                # its low bits follow the profile's instruction-address alignment,
                # and the generator widens or masks them from the profile's
                # claimed extensions. Freezing bit 1 either way in the table is
                # the inconsistency EX-034 recorded: a profile that claims C has
                # IALIGN=16 and mepc[1]/sepc[1] writable, one that does not has
                # IALIGN=32 and both low bits read-only zero.
                if csr.get("name") in PC_VALUED_CSRS:
                    if sorted(ialign_bits) != [(1, 0)]:
                        bundle.fail(
                            where,
                            "a program-counter register must declare ialign_bits "
                            '["1:0"] so its low bits follow the profile\'s IALIGN '
                            "instead of being frozen in the table",
                        )
                    if (1, 1) in fixed or (1, 1) in writable:
                        bundle.fail(
                            where,
                            "bit 1 of a program-counter register is IALIGN-derived "
                            "(read-only zero exactly when the profile does not claim C) "
                            "and must not be declared writable or unmodifiable",
                        )

                if behavior == "warl_wpri" and not (writable and wpri):
                    bundle.fail(
                        where,
                        "behavior warl_wpri requires both writable_fields and wpri_fields "
                        "to be non-empty",
                    )
                if behavior in ("warl", "warl_wpri") and access == "ro":
                    bundle.fail(where, "WARL behaviour requires a writable CSR")
                if behavior == "wpri" and not wpri:
                    bundle.fail(where, "behavior wpri requires a non-empty wpri_fields")
                if not csr.get("spec_clause"):
                    bundle.fail(where, "missing spec_clause; an unproven CSR must not ship")

                # Extension-state fields. The privileged specification states that
                # mstatus.FS shall not be read-only zero when the F extension is
                # implemented, and the same for VS and the V extension. Only that
                # direction is enforced: these CSR tables are shared across all four
                # profiles, so a field that is writable but reads zero in a profile
                # without F is the correct single description of "no floating-point
                # state exists", not a violation. The reverse -- claiming F while
                # declaring FS read-only zero -- hides real dirty state and is rejected.
                #
                # `writable` and `fixed` above are *parsed* (msb, lsb) tuples, so
                # membership against declared field names must use the raw lists.
                declared_fixed = csr.get("unmodifiable_bits", [])
                for field, capability in (("14:13", "F"), ("10:9", "V")):
                    if field not in declared_fixed:
                        continue
                    if capability in bundle.profile["isa_target"]["extensions"]:
                        bundle.fail(
                            where,
                            "field %s is read-only zero but this profile claims %s; the "
                            "specification requires the field not to be read-only zero "
                            "when the extension is implemented" % (field, capability),
                        )

                for first, last, reason in absent:
                    if first <= address <= last:
                        bundle.fail(
                            where,
                            "address 0x%03x lies inside a range this profile declares "
                            "unimplemented (%s)" % (address, reason),
                        )


# ---------------------------------------------------------------------------
# CSR rule ledger (V-017)
# ---------------------------------------------------------------------------


_RULE_KINDS = frozenset(["strict", "warl_allowed", "read_only", "alias", "permission"])
_EXAMPLE_OPS = frozenset(["csrrw", "csrrs", "csrrc", "csrr"])


def _ledger_bits_mask(spec: str, width: int, where: str, bundle: Bundle) -> Optional[int]:
    """Parse a ledger `bits` string ("3", "14:13", "63,35:32") into a mask."""
    mask = 0
    for item in spec.split(","):
        if ":" in item:
            hi_text, lo_text = item.split(":", 1)
        else:
            hi_text = lo_text = item
        try:
            hi, lo = int(hi_text, 10), int(lo_text, 10)
        except ValueError:
            bundle.fail(where, "bit spec %r is not numeric" % item)
            return None
        if hi < lo:
            hi, lo = lo, hi
        if lo < 0 or hi >= width:
            bundle.fail(where, "bit spec %r lies outside the %d-bit register" % (item, width))
            return None
        mask |= ((1 << (hi - lo + 1)) - 1) << lo
    return mask


def _check_csr_rules(bundle: Bundle, profile_name: str) -> None:
    """Cross-check the V-017 rule ledger against the profile's implementation table.

    The schema says the ledger is well-formed. This says it is *about this
    profile*: every rule names a CSR the table implements (or is explicitly about
    an address it does not), every implemented CSR carries at least one rule, the
    example's target addresses exist and are writable where a write is expected,
    every rule states the adjacent illegal result it rejects, and the address
    encoded permission of every implemented CSR agrees with the profile's mode
    list -- the part of the permission rule that no M-only stimulus can reach.
    """
    ledger = bundle.csr_rules
    if ledger is None:
        return
    if ledger.get("profile") != profile_name:
        # The ledger is written against one profile's implementation table; it is
        # loaded (and schema-checked) for every profile so a malformed file cannot
        # hide, but its addresses are only cross-checked against the table it names.
        return

    where = "csr rule ledger"
    merged: Dict[str, Dict[str, Any]] = {}
    by_address: Dict[int, str] = {}
    for table in bundle.csr_tables:
        for block in table.get("modes", []):
            for csr in block["csrs"]:
                merged.setdefault(csr["name"], csr)
                by_address[csr["address"]] = csr["name"]

    # The address-encoded permission rule: csr[9:8] is the lowest privilege level
    # that may access the register and csr[11:10] == 3 marks a read-only register,
    # so a write needs csr[9:8] (or M when the address is read-only). No
    # implemented CSR may require a privilege the profile does not implement.
    claimed = set(bundle.profile["privilege_modes"]) if bundle.profile else set()
    least = min(({ "U": 0, "S": 1, "H": 2, "M": 3 }[m] for m in claimed), default=3)
    for name, csr in sorted(merged.items()):
        address = csr["address"]
        min_r = (address >> 8) & 0x3
        min_w = 3 if ((address >> 10) & 0x3) == 0x3 else min_r
        if min_r > least:
            bundle.fail(
                "csr %s" % name,
                "address 0x%03x encodes a minimum privilege %d above the least mode "
                "profile %s implements (%d); the permission precondition is unmet"
                % (address, min_r, profile_name, least),
            )
        if csr["access"] != "ro" and min_w > least:
            bundle.fail(
                "csr %s" % name,
                "address 0x%03x requires privilege %d to write but profile %s implements "
                "only down to %d" % (address, min_w, profile_name, least),
            )

    seen_ids = set()
    covered: Dict[str, int] = {}
    for index, rule in enumerate(ledger["rules"]):
        rid = rule["id"]
        rule_where = "%s rule %s" % (where, rid)
        if rid in seen_ids:
            bundle.fail(rule_where, "duplicate rule id")
        seen_ids.add(rid)

        kind = rule["kind"]
        if kind not in _RULE_KINDS:
            bundle.fail(rule_where, "unknown rule kind %r" % kind)

        csr_name = rule["csr"]
        csr = merged.get(csr_name)
        if csr_name == "unimplemented":
            if rule["address"] in by_address:
                bundle.fail(
                    rule_where,
                    "declared 'unimplemented' but address 0x%03x implements %s"
                    % (rule["address"], by_address[rule["address"]]),
                )
            width = 64
        else:
            if csr is None:
                bundle.fail(rule_where, "names CSR %r which profile %s does not implement"
                            % (csr_name, profile_name))
                continue
            if csr["address"] != rule["address"]:
                bundle.fail(
                    rule_where,
                    "address 0x%03x disagrees with the table's 0x%03x for %s"
                    % (rule["address"], csr["address"], csr_name),
                )
            width = csr["width"]
            covered[csr_name] = covered.get(csr_name, 0) + 1

        mask = _ledger_bits_mask(rule["bits"], width, rule_where, bundle)

        for tag in ("positive", "negative"):
            example = rule[tag]
            ex_where = "%s %s" % (rule_where, tag)
            if example["op"] not in _EXAMPLE_OPS:
                bundle.fail(ex_where, "unknown op %r" % example["op"])
            target = int(example.get("target", "0x%03x" % rule["address"]), 16)
            write_target = int(example.get("write_target", "0x%03x" % target), 16)
            if example["op"] != "csrr":
                if write_target not in by_address:
                    # Only a permission rule may write an address the table does
                    # not implement, and it must expect the write to trap.
                    if kind != "permission" or example["traps"] < 1:
                        bundle.fail(
                            ex_where,
                            "write target 0x%03x is not an implemented CSR and the rule "
                            "does not expect a trap" % write_target,
                        )
                else:
                    written = merged[by_address[write_target]]
                    if written["access"] == "ro" and example["traps"] < 1:
                        bundle.fail(
                            ex_where,
                            "writes read-only CSR %s without expecting a trap"
                            % written["name"],
                        )
                    if csr is not None and written["name"] != csr_name and kind != "alias":
                        bundle.fail(
                            ex_where,
                            "writes %s but the rule is about %s (only an alias rule may "
                            "write a different register)" % (written["name"], csr_name),
                        )
            if target not in by_address:
                # Only a permission rule may aim at an address the table does not
                # implement, and it must expect the access to trap.
                if kind != "permission" or example["traps"] < 1:
                    bundle.fail(
                        ex_where,
                        "read target 0x%03x is not implemented and the rule does not "
                        "expect a trap" % target,
                    )

        # The card's central requirement: every rule carries the adjacent illegal
        # result it rejects, or it is not a rule the comparator can be wrong about.
        if "forbid" not in rule["positive"] and "forbid" not in rule["negative"]:
            bundle.fail(
                rule_where,
                "neither example names the adjacent illegal result (no `forbid`); the "
                "comparator would accept every difference, which is the auto-waiver "
                "fail mode",
            )

    for name in sorted(merged):
        if name not in covered:
            bundle.fail(
                where,
                "implemented CSR %s has no rule; the ledger must cover every implemented "
                "CSR, not only the interesting ones" % name,
            )


# ---------------------------------------------------------------------------
# geometry
# ---------------------------------------------------------------------------


def _check_geometry(bundle: Bundle, profile_name: str) -> None:
    geometry = bundle.geometry
    if geometry is None or bundle.profile is None:
        return

    if geometry.get("profile") != profile_name:
        bundle.fail(
            "geometry",
            "geometry declares profile %r but is used by %r"
            % (geometry.get("profile"), profile_name),
        )

    rob = geometry["rob"]
    prf = geometry["int_prf"]
    frontend = geometry["frontend"]
    fabric = geometry["fabric"]
    rename = geometry["rename"]
    lsu = geometry["lsu"]

    def power_of_two(value: int) -> bool:
        return value > 0 and (value & (value - 1)) == 0

    if not power_of_two(rob["entries"]):
        bundle.fail(
            "geometry",
            "rob.entries=%d is not a power of two; the ROB pointer and wrap comparison "
            "would need an explicit terminator" % rob["entries"],
        )
    # The PRF is allocated from an explicit free list rather than a wrapping
    # pointer, so its depth need not be a power of two; docs/implementation-plan.md
    # section 1.2 deliberately picks 96. What must hold is that a bank index can be
    # decoded from a physical tag without ambiguity.
    if prf["entries"] % prf["banks"] != 0:
        bundle.fail(
            "geometry",
            "int_prf.entries=%d does not divide evenly into %d banks; bank index decoding "
            "would be ambiguous" % (prf["entries"], prf["banks"]),
        )
    if prf["entries"] <= ARCH_INT_REGS:
        bundle.fail(
            "geometry",
            "int_prf.entries=%d leaves no room for renaming above the %d architectural "
            "registers" % (prf["entries"], ARCH_INT_REGS),
        )
    if rename["rename_width"] > rename["dispatch_width"]:
        bundle.fail(
            "geometry",
            "rename_width=%d exceeds dispatch_width=%d; a macro could be allocated a "
            "physical register it can never issue" % (rename["rename_width"], rename["dispatch_width"]),
        )
    if fabric["iq_entries_per_cluster"] < rename["dispatch_width"]:
        bundle.fail(
            "geometry",
            "iq_entries_per_cluster=%d cannot hold one cycle of dispatch_width=%d"
            % (fabric["iq_entries_per_cluster"], rename["dispatch_width"]),
        )
    if lsu["lq_entries"] < 2 or lsu["sq_entries"] < 2:
        bundle.fail("geometry", "LQ and SQ must hold at least two entries to make progress")
    if fabric["result_fifo_per_cluster"] < fabric["alu_per_cluster"]:
        bundle.fail(
            "geometry",
            "result_fifo_per_cluster=%d cannot accept completions from %d ALUs in one cycle"
            % (fabric["result_fifo_per_cluster"], fabric["alu_per_cluster"]),
        )

    xlen = bundle.profile["xlen"]
    # The fetch target buffer is sized in bits and must be able to hold at least
    # one instruction of the widest supported length.
    quantum = frontend["target_quantum"]
    if quantum % 16 != 0 or quantum > xlen or quantum < 16:
        bundle.fail(
            "geometry",
            "frontend.target_quantum=%d bits is not a 16-bit-aligned window no wider than "
            "XLEN=%d" % (quantum, xlen),
        )

    # The rename stage holds as many physical registers in flight as the PRF has
    # allocatable entries, and its undo journal is bounded by how many allocations
    # can be outstanding at once, which is the ROB. For that bound to be sound
    # rather than hopeful:
    #
    #     allocatable tags  >=  ROB entries
    #
    # At p0 this is 96 - 32 = 64 = ROB_ENTRIES exactly, with nothing to spare. A
    # deeper ROB on the same PRF makes the journal the limiting structure and
    # reports spurious overflow on a machine that squashes correctly, so the
    # relationship is checked rather than assumed.
    allocatable = prf["entries"] - ARCH_INT_REGS
    if allocatable < rob["entries"]:
        bundle.fail(
            "geometry",
            "int_prf.entries=%d leaves %d allocatable tags after %d architectural "
            "registers, fewer than rob.entries=%d; the rename undo journal would "
            "overflow on a correctly-squashing machine. Deepen the PRF or shrink the ROB"
            % (prf["entries"], allocatable, ARCH_INT_REGS, rob["entries"]),
        )

    if bundle.profile.get("clusters") != fabric["clusters"]:
        bundle.fail(
            "geometry",
            "profile declares %r clusters but the fabric geometry declares %d"
            % (bundle.profile.get("clusters"), fabric["clusters"]),
        )

    if "V" in bundle.profile["isa_target"]["extensions"]:
        vector = geometry.get("vector")
        if vector is None:
            bundle.fail("geometry", "profile claims V but the geometry has no vector block")
        else:
            isa = bundle.profile["isa_target"]
            if vector["vlen"] != isa["vlen"]:
                bundle.fail(
                    "geometry",
                    "vector.vlen=%d disagrees with the profile's isa_target.vlen=%s"
                    % (vector["vlen"], isa["vlen"]),
                )
            if vector["elen"] != isa["elen"]:
                bundle.fail(
                    "geometry",
                    "vector.elen=%d disagrees with the profile's isa_target.elen=%s"
                    % (vector["elen"], isa["elen"]),
                )
            # A vector register group is LMUL*VLEN bits wide. The VRF as a whole is
            # vrf_banks * vrf_bank_bytes * 8 bits, which is the total architectural
            # state; the group is the widest window any one instruction can name.
            group_capacity_bits = vector["vrf_banks"] * vector["vrf_bank_bytes"] * 8
            widest_group_bits = vector["vlen"] * vector["lane_groups_large"]
            if group_capacity_bits < widest_group_bits:
                bundle.fail(
                    "geometry",
                    "vector register file holds %d bits but the widest register group "
                    "(LMUL=%d over VLEN=%d) needs %d bits"
                    % (group_capacity_bits, vector["lane_groups_large"], vector["vlen"],
                       widest_group_bits),
                )
            expected_groups = (vector["vrf_banks"] * vector["vrf_bank_bytes"] * 8) // vector["vlen"]
            if expected_groups < vector["lane_groups_large"]:
                bundle.fail(
                    "geometry",
                    "vector register file provides LMUL up to %d but the largest lane "
                    "quota claims %d" % (expected_groups, vector["lane_groups_large"]),
                )
            if not (
                vector["lane_groups_small"] <= vector["lane_groups_medium"]
                <= vector["lane_groups_large"]
            ):
                bundle.fail(
                    "geometry",
                    "lane group quotas must be ordered small <= medium <= large, got %d/%d/%d"
                    % (
                        vector["lane_groups_small"],
                        vector["lane_groups_medium"],
                        vector["lane_groups_large"],
                    ),
                )
    elif geometry.get("vector"):
        bundle.fail("geometry", "geometry declares vector resources but the profile does not claim V")

    if geometry.get("caches"):
        if not bundle.memory:
            return
        cacheable = [r for r in bundle.memory["regions"] if r["cacheable"]]
        if not cacheable:
            bundle.fail(
                "geometry",
                "profile declares caches but its memory map has no cacheable region",
            )


# ---------------------------------------------------------------------------
# cross references
# ---------------------------------------------------------------------------


def _check_cross_references(bundle: Bundle) -> None:
    if not bundle.profile or bundle.memory is None:
        return
    regions = bundle.memory["regions"]
    reset_vector = bundle.profile["reset"]["reset_vector"]
    boot_bytes = bundle.profile["reset"]["boot_rom_bytes"]

    executable = [r for r in regions if r["executable"] and r["readable"]]
    if not any(r["base"] <= reset_vector < r["base"] + r["size"] for r in executable):
        bundle.fail(
            "profile reset",
            "reset vector 0x%x is not inside any readable+executable region" % reset_vector,
        )

    protocol = bundle.profile.get("test_protocol")
    if protocol:
        def region_of(address):
            for r in regions:
                if r["base"] <= address < r["base"] + r["size"]:
                    return r
            return None

        for key in ("tohost", "fromhost"):
            address = protocol[key]
            region = region_of(address)
            if region is None:
                bundle.fail("test protocol", "%s address 0x%x is not inside any mapped region"
                            % (key, address))
            elif address % 8 != 0:
                bundle.fail("test protocol", "%s address 0x%x is not 8-byte aligned" % (key, address))
            elif not region["writable"] or region.get("device"):
                # HTIF, and therefore Spike, can only poll a tohost that is real
                # memory; a device address cannot be used as one.
                bundle.fail(
                    "test protocol",
                    "%s address 0x%x is in region %r, which is %s; the test protocol must "
                    "be an ordinary writable memory word so an external reference model "
                    "can poll it" % (key, address, region["name"],
                                     "a device" if region.get("device") else "not writable"),
                )
        if abs(protocol["fromhost"] - protocol["tohost"]) < 8:
            bundle.fail("test protocol", "fromhost at 0x%x overlaps tohost at 0x%x"
                        % (protocol["fromhost"], protocol["tohost"]))
        signature = protocol["signature"]
        region = region_of(signature)
        if region is None:
            bundle.fail("test protocol", "signature address 0x%x is not inside any mapped region"
                        % signature)
        elif not region["writable"]:
            bundle.fail("test protocol", "signature address 0x%x is in region %r which is not "
                        "writable; the program could never record its result"
                        % (signature, region["name"]))
        span = protocol["signature_words"] * 8
        if region is not None and signature + span > region["base"] + region["size"]:
            bundle.fail("test protocol", "signature area of %d words at 0x%x runs past the end "
                        "of region %r" % (protocol["signature_words"], signature, region["name"]))

    roms = [r for r in regions if r["kind"] == "rom"]
    if not roms:
        bundle.fail("profile reset", "memory map declares no rom region to hold the boot vector")
    elif not any(r["size"] == boot_bytes for r in roms):
        bundle.fail(
            "profile reset",
            "reset.boot_rom_bytes=%d matches no rom region size (%s)"
            % (boot_bytes, ", ".join(str(r["size"]) for r in roms)),
        )


# ---------------------------------------------------------------------------
# derivation
# ---------------------------------------------------------------------------


def misa_value(claimed: List[str], xlen: int) -> int:
    """Compute the misa reset value for a set of claimed extensions."""
    value = {32: 1, 64: 2, 128: 3}[xlen] << MISA_MXL_SHIFT
    for name in claimed:
        if name in MISA_BITS:
            value |= 1 << MISA_BITS[name]
    return value


def _ladder_tokens(config_root: Optional[str] = None) -> Dict[str, str]:
    root = config_root or CONFIG_ROOT
    path = os.path.join(root, "capability_ladder.json")
    if not os.path.exists(path):
        return {}
    with open(path) as handle:
        ladder = json.load(handle)
    return {
        cap["name"]: cap["isa_string_token"]
        for cap in ladder.get("capabilities", [])
        if cap.get("isa_string_token")
    }


def isa_string(claimed: List[str], xlen: int, config_root: Optional[str] = None) -> str:
    """Build the compiler/reference/DUT ISA string from one capability list.

    Single-letter extensions come first in sorted order, then multi-letter ones
    in sorted order, so `rv64imafdc_zicsr_zifencei`-style strings come out
    identical from the manifest generator, the Makefile and the test corpora.
    """
    tokens = _ladder_tokens(config_root)
    singles = []
    multis = []
    for name in claimed:
        token = tokens.get(name, name.lower())
        (singles if len(token) == 1 else multis).append(token)
    singles = sorted(set(singles))
    multis = sorted(set(multis))
    body = "".join(singles) + ("_" + "_".join(multis) if multis else "")
    return "rv%d%s" % (xlen, body)


def advertised_capabilities(bundle: Bundle) -> Tuple[List[str], List[str]]:
    """Split claimed capabilities into (advertised, not-yet-implemented).

    Advertising is gated on `config/status/implementation_status.json`: a
    capability is publishable in `misa` / compiler `-march` / reference
    capability only once **both** the implementation tasks and the verification
    tasks the ladder names for it have delivered.

    Both halves matter and they are not interchangeable: the implementation tasks
    say the feature exists, and the verification tasks are where its independent
    positive and negative cases live. Gating on implementation alone is how a
    capability gets published with no evidence that it behaves, which is the
    class of claim this project exists to refuse. A capability that names no
    verification task at all is likewise not advertisable -- that is a gap in the
    ladder to fix, not a licence to publish.
    """
    if not bundle.profile or not bundle.ladder:
        return [], []
    caps = _ladder_index(bundle)
    delivered = set()
    if bundle.status:
        delivered = set(bundle.status.get("delivered_tasks", []))

    advertised: List[str] = []
    pending: List[str] = []
    for name in bundle.profile["isa_target"]["extensions"]:
        cap = caps.get(name)
        impl_tasks = list(cap.get("impl_tasks", [])) if cap else []
        verify_tasks = list(cap.get("verify_tasks", [])) if cap else []
        if (impl_tasks and verify_tasks
                and all(task in delivered for task in impl_tasks)
                and all(task in delivered for task in verify_tasks)):
            advertised.append(name)
        else:
            pending.append(name)
    return advertised, pending


def ialign(bundle: Bundle) -> int:
    """The instruction-address alignment the profile's claimed extensions imply.

    IALIGN is a property of the profile, not of one CSR: an implementation that
    supports the C extension executes 16-bit instructions and has IALIGN=16, and
    one that does not has IALIGN=32 (RISC-V Unprivileged ISA, "Instruction
    Length"; Priv v1.12 mepc/sepc: the two low bits are read-only zero "on
    implementations that support only IALIGN=32"). This is the one place the
    choice is made, and it is derived from the claimed extension list rather than
    held in a second flag that can drift from the capability ladder -- a drift
    that produced EX-034. tools/gen_manifest.py widens the generated write masks
    of the PC-valued CSRs from it, and tools/check_profile.py's table check
    rejects a profile that claims C while its table freezes the alignment bits.
    """
    extensions = bundle.profile["isa_target"]["extensions"] if bundle.profile else []
    return 16 if "C" in extensions else 32