#!/usr/bin/env python3
"""Generate the build manifest and the generated SystemVerilog config package.

    python3 tools/gen_manifest.py --profile p0

Produces, under ``build/<profile>/``:

* ``manifest.json``   — profile, ISA string, misa, memory map, CSR inventory,
                        input hashes and tool versions. This is the single file the
                        DUT build, the compiler flags and the reference model all
                        quote, so they cannot drift apart.
* ``rtl/mosaic_cfg_pkg.svh`` — synthesisable ``localparam`` constants derived
                        from exactly the same inputs.
* ``rtl/mosaic_csr_pkg.svh`` — the CSR implementation table: one address, reset
                        value, write mask and write-legality flag per CSR, decoded
                        from config/csr/mode_m.json. The CSR file includes it and
                        carries no second copy of any of those numbers.
* ``rtl/filelist.f``         — the ordered source list handed to Verilator/Yosys.

If the configuration does not check out, **no** manifest is written and the exit
status is non-zero. A failed configuration must never leave behind a manifest
that a later build could mistake for a good one.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from mosaic import config_check  # noqa: E402

BUILD_ROOT = os.path.join(config_check.REPO_ROOT, "build")
RTL_ROOT = os.path.join(config_check.REPO_ROOT, "rtl")

# The ordered source list is derived from the tree, not from a hand-maintained
# list per directory. The hand-written lists were a second copy of a fact the
# tree already states, and they had gone stale: `rtl/core/filelist.f` named 9 of
# the 20 modules in that directory, and the generator's own path join made the
# generated file empty, so the manifest reported "rtl sources: 0" while the
# per-directory lists looked populated. One source of truth is the directory.
#
# Order matters: a package must precede every module that refers to its types,
# and a package that `` `include ``s another must follow it. Packages are
# therefore emitted first, in name order (which satisfies the inclusion order for
# this tree), then the modules.
def _is_package_source(path: str) -> bool:
    try:
        with open(path, errors="replace") as handle:
            text = handle.read()
    except OSError:
        return False
    has_package = re.search(r"^\s*package\s+[A-Za-z_]", text, re.MULTILINE) is not None
    has_module = re.search(r"^\s*module\s+[A-Za-z_]", text, re.MULTILINE) is not None
    return has_package and not has_module


def _tool_version(command) -> str:
    try:
        out = subprocess.run(
            command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30
        )
        text = out.stdout.decode("utf-8", "replace").strip().splitlines()
        return text[0] if text else ""
    except (OSError, subprocess.SubprocessError):
        return ""


def tool_versions() -> dict:
    return {
        "verilator": _tool_version(["verilator", "--version"]),
        "yosys": _tool_version(["yosys", "-V"]),
        "riscv_gcc": _tool_version(["riscv64-elf-gcc", "--version"]),
        "python": sys.version.split()[0],
        "make": _tool_version(["make", "--version"]),
        "slang_tidy": _tool_version(["slang-tidy", "--version"]),
    }


def _hex64(value: int) -> str:
    return "64'h%016x" % value


def render_sv_package(bundle: config_check.Bundle, advertised) -> str:
    profile = bundle.profile
    geometry = bundle.geometry
    xlen = profile["xlen"]
    misa = config_check.misa_value(advertised, xlen)
    lines = []
    add = lines.append
    add("// GENERATED FILE - do not edit.")
    add("// Produced by tools/gen_manifest.py --profile %s" % bundle.name)
    add("// Source of truth: config/profiles/%s.json and everything it references." % bundle.name)
    add("// Any edit here is lost on the next build and will not match the manifest.")
    add("")
    add("// Several RTL files include this header, so it needs a guard. Without one the")
    add("// package is defined once per including file; Verilator dedupes packages and")
    add("// tolerates it, while slang correctly reports a duplicate definition. That")
    add("// difference is exactly why more than one tool reads this source.")
    add("`ifndef MOSAIC_CFG_PKG_SV_")
    add("`define MOSAIC_CFG_PKG_SV_")
    add("")
    add("package mosaic_cfg_pkg;")
    add("")
    add("  // This package is compiled into every file that includes it, and a warning is")
    add("  // a failure in this project. No consumer names every constant -- the RAM base")
    add("  // is unused in the frontend, the fetch depth in the CSR file -- so the")
    add("  // unused-parameter check is silenced here, once, rather than by a workaround")
    add("  // in every including module.")
    add("  /* verilator lint_off UNUSEDPARAM */")
    add("")
    add("  localparam int unsigned MOSAIC_PROFILE_INDEX = %d;" % config_check.PROFILE_ORDER.index(bundle.name))
    add("  localparam int unsigned MOSAIC_XLEN           = %d;" % xlen)
    add("  localparam int unsigned MOSAIC_HARTS          = %d;" % profile["harts"])
    add("  localparam int unsigned MOSAIC_CLUSTERS       = %d;" % geometry["fabric"]["clusters"])
    add("  localparam int unsigned MOSAIC_ARCH_INT_REGS  = 32;")
    add("")
    add("  // Derived from the advertised capability list, not from the claimed list:")
    add("  // an extension whose implementation has not delivered is not in misa.")
    add("  localparam logic [63:0] MOSAIC_MISA_RESET = %s;" % _hex64(misa))
    add("  localparam logic [63:0] MOSAIC_RESET_VECTOR = %s;" % _hex64(profile["reset"]["reset_vector"]))
    add("")
    add("  // Frontend")
    add("  localparam int unsigned MOSAIC_FETCH_OUTSTANDING = %d;" % geometry["frontend"]["fetch_outstanding"])
    add("  localparam int unsigned MOSAIC_BTB_ENTRIES       = %d;" % geometry["frontend"]["btb_entries"])
    add("  localparam int unsigned MOSAIC_BPU_ENTRIES       = %d;" % geometry["frontend"]["bpu_entries"])
    add("  localparam int unsigned MOSAIC_RAS_ENTRIES       = %d;" % geometry["frontend"]["ras_entries"])
    add("  localparam int unsigned MOSAIC_TARGET_QUANTUM    = %d;" % geometry["frontend"]["target_quantum"])
    add("")
    add("  // Rename / commit")
    add("  localparam int unsigned MOSAIC_RENAME_WIDTH = %d;" % geometry["rename"]["rename_width"])
    add("  localparam int unsigned MOSAIC_DISPATCH_WIDTH = %d;" % geometry["rename"]["dispatch_width"])
    add("  localparam int unsigned MOSAIC_RETIRE_WIDTH = %d;" % geometry["rename"]["dispatch_width"])
    add("  localparam int unsigned MOSAIC_ROB_ENTRIES = %d;" % geometry["rob"]["entries"])
    add("  localparam int unsigned MOSAIC_ROB_INDEX_W = $clog2(%d);" % geometry["rob"]["entries"])
    add("  localparam int unsigned MOSAIC_MAX_UOPS_PER_MACRO = %d;" % geometry["rob"]["max_uops_per_macro"])
    add("")
    add("  // Integer PRF: allocated from a free list, so the depth need not be a")
    add("  // power of two; only the bank decode has to be unambiguous.")
    add("  localparam int unsigned MOSAIC_INT_PRF_ENTRIES = %d;" % geometry["int_prf"]["entries"])
    add("  localparam int unsigned MOSAIC_INT_PRF_TAG_W   = $clog2(%d);" % geometry["int_prf"]["entries"])
    add("  localparam int unsigned MOSAIC_PRF_BANKS       = %d;" % geometry["int_prf"]["banks"])
    add("  localparam int unsigned MOSAIC_PRF_BANK_W      = $clog2(%d);" % geometry["int_prf"]["banks"])
    add("")
    add("  // Execution fabric")
    add("  localparam int unsigned MOSAIC_IQ_ENTRIES      = %d;" % geometry["fabric"]["iq_entries_per_cluster"])
    add("  localparam int unsigned MOSAIC_ALU_PER_CLUSTER = %d;" % geometry["fabric"]["alu_per_cluster"])
    add("  localparam int unsigned MOSAIC_MULDIV_UNITS    = %d;" % geometry["fabric"]["mul_div_units"])
    add("  localparam int unsigned MOSAIC_RESULT_FIFO     = %d;" % geometry["fabric"]["result_fifo_per_cluster"])
    add("  localparam int unsigned MOSAIC_REMOTE_LATENCY  = %d;" % geometry["fabric"]["remote_link_latency"])
    add("")
    add("  // LSU")
    add("  localparam int unsigned MOSAIC_LSU_UNITS  = %d;" % geometry["lsu"]["units"])
    add("  localparam int unsigned MOSAIC_LQ_ENTRIES = %d;" % geometry["lsu"]["lq_entries"])
    add("  localparam int unsigned MOSAIC_SQ_ENTRIES = %d;" % geometry["lsu"]["sq_entries"])
    add("")

    if "V" in advertised:
        vector = geometry["vector"]
        add("  // Vector (advertised: V is in the advertised capability list)")
        add("  localparam int unsigned MOSAIC_VLEN        = %d;" % vector["vlen"])
        add("  localparam int unsigned MOSAIC_ELEN        = %d;" % vector["elen"])
        add("  localparam int unsigned MOSAIC_VRF_BANKS   = %d;" % vector["vrf_banks"])
        add("  localparam int unsigned MOSAIC_VRF_BANK_B  = %d;" % vector["vrf_bank_bytes"])
        add("")

    # PMP: the entry count and the grain are a platform decision, so they come
    # from the profile's geometry file and never from the RTL. A profile with no
    # `pmp` block implements no entries at all (Priv v1.12: "Implementations may
    # implement zero, 16, or 64 PMP entries"), which is the honest description of
    # an M-only profile that has no less-privileged mode to protect.
    pmp = geometry.get("pmp")
    entries = int(pmp["entries"]) if pmp else 0
    grain = int(pmp["granularity_bytes"]) if pmp else 4
    g = 0
    while (1 << (g + 2)) < grain:
        g += 1
    if (1 << (g + 2)) != grain:
        raise SystemExit("geometry pmp.granularity_bytes must be a power of two >= 4")
    add("  // PMP: entries and grain from the profile geometry (Priv v1.12 2.7.1).")
    add("  localparam int unsigned MOSAIC_PMP_ENTRIES     = %d;" % entries)
    add("  localparam int unsigned MOSAIC_PMP_GRAIN_BYTES = %d;" % grain)
    add("  localparam int unsigned MOSAIC_PMP_G           = %d;" % g)
    add("  localparam int unsigned MOSAIC_PMP_CFG_COUNT   = %d;" % ((entries + 7) // 8))
    add("  // Flat observability widths for the entry arrays. One bit when the profile")
    add("  // implements no entries, because a zero-width vector is not a vector.")
    add("  localparam int unsigned MOSAIC_PMP_ENTRY_CFG_W  = %d;"
        % (1 if entries == 0 else entries * 8))
    add("  localparam int unsigned MOSAIC_PMP_ENTRY_ADDR_W = %d;"
        % (1 if entries == 0 else entries * 64))
    add("  // The CSR numbers are fixed by the ISA (priv-csrs.tex, \"CSR number")
    add("  // assignments\": 0x3A0 & MRW & pmpcfg0, 0x3B0 & MRW & pmpaddr0), so they")
    add("  // are emitted here rather than written into the RTL. On RV64 only the")
    add("  // even-numbered pmpcfg registers are legal, so entry group k is 0x3A0+2k.")
    add("  localparam logic [11:0] MOSAIC_PMPCFG_ADDR_BASE  = 12'h3a0;")
    add("  localparam logic [11:0] MOSAIC_PMPADDR_ADDR_BASE = 12'h3b0;")
    add("")

    add("  // Physical memory map")
    for region in sorted(bundle.memory["regions"], key=lambda r: r["base"]):
        base_name = "MOSAIC_%s_BASE" % region["name"].upper()
        size_name = "MOSAIC_%s_SIZE" % region["name"].upper()
        add("  localparam logic [63:0] %-34s = %s;" % (base_name, _hex64(region["base"])))
        add("  localparam logic [63:0] %-34s = %s;" % (size_name, _hex64(region["size"])))
    add("")
    add("  /* verilator lint_on UNUSEDPARAM */")
    add("")
    add("endpackage : mosaic_cfg_pkg")
    add("")
    add("`endif  // MOSAIC_CFG_PKG_SV_")
    add("")
    return "\n".join(lines)


def render_sv_id_package(bundle: config_check.Bundle) -> str:
    """Emit the identity package from the frozen contracts.

    Every width here comes from evaluating `config/contracts/interfaces.json`
    against this profile's geometry. If the geometry shrinks, a width shrinks
    with it and check_contracts.py proves the modulus still exceeds twice the
    maximum compare distance -- nothing here is a hand-typed constant.
    """
    import check_contracts

    contract_root = os.path.join(config_check.CONFIG_ROOT, "contracts")
    contracts = check_contracts.load_contract("interfaces.json", contract_root)
    counters_doc = check_contracts.load_contract("counters.json", contract_root)
    if contracts is None or counters_doc is None:
        raise SystemExit("config/contracts is missing; refusing to emit an identity package")

    namespace = check_contracts.build_namespace(bundle)
    evaluator = check_contracts.ExpressionEvaluator(
        namespace, contracts.get("expression_namespace", [])
    )

    widths: dict = {}
    justified: dict = {}
    for interface in contracts["interfaces"]:
        for field in interface["identity_fields"]:
            name = field["name"]
            width = evaluator.evaluate(field["expr"])
            if name in widths and widths[name] != width:
                raise SystemExit(
                    "contract field %s is used with two different widths (%d and %d); "
                    "one identity must have one width" % (name, widths[name], width)
                )
            widths[name] = width
            justified[name] = field.get("why", "")

    lines = []
    add = lines.append
    add("// GENERATED FILE - do not edit.")
    add("// Produced by tools/gen_manifest.py --profile %s from config/contracts/." % bundle.name)
    add("// Widths are evaluated from the profile geometry; tools/check_contracts.py")
    add("// proves every counter modulus exceeds twice the maximum compare distance.")
    add("//")
    add("// Several RTL files include this header in one compilation unit, so it needs an")
    add("// include guard. Without one the package is defined once per including file;")
    add("// the simulator dedupes packages and tolerates it, while slang reports a")
    add("// duplicate definition, and both tools read this source precisely because")
    add("// they disagree.")
    # Note for whoever edits these lines: a comment line whose first word is
    # `Verilator` (or `verilator`) is a *metacomment* to Verilator and is parsed
    # as a pragma, so `// Verilator dedupes ...` in generated RTL is a hard
    # error (BADVLTPRAGMA) in every file that includes this header. The word may
    # appear mid-line; it may not begin the comment.
    add("")
    add("`ifndef MOSAIC_ID_PKG_SV_")
    add("`define MOSAIC_ID_PKG_SV_")
    add("")
    add("package mosaic_id_pkg;")
    add("")
    add("  // This package is compiled into every file that includes it, and a warning")
    add("  // is a failure in this project. The identity functions below use only some")
    add("  // of their arguments' fields, and a consumer names only some of the")
    add("  // constants, so the unused-signal and unused-parameter checks are silenced")
    add("  // here rather than at every include site.")
    add("  /* verilator lint_off UNUSEDSIGNAL */")
    add("  /* verilator lint_off UNUSEDPARAM */")
    add("")
    add("  // ---- identity field widths ----")
    for name in sorted(widths):
        add("  localparam int unsigned MOSAIC_ID_W_%-12s = %d;" % (name.upper(), widths[name]))
    add("")
    add("  // ---- counter moduli (expressions from config/contracts/counters.json) ----")
    for counter in counters_doc["counters"]:
        bits = evaluator.evaluate(counter["expression"])
        add("  // %s: modulus %d, at most %s live"
            % (counter["name"], bits, counter["max_live_expr"]))
        add("  localparam int unsigned MOSAIC_CNT_W_%-16s = %d;" % (counter["name"].upper(), bits))
    add("")
    add("  // ---- composite identities ----")
    add("  typedef struct packed {")
    add("    logic [MOSAIC_ID_W_HART-1:0]     hart;")
    add("    logic [MOSAIC_ID_W_ROB_INDEX-1:0] rob_index;")
    add("    logic [MOSAIC_ID_W_ROB_GEN-1:0]   rob_gen;")
    add("    logic [MOSAIC_ID_W_UOP_INDEX-1:0] uop_index;")
    add("  } macro_id_t;")
    add("")
    add("  typedef struct packed {")
    add("    logic [MOSAIC_ID_W_HART-1:0]   hart;")
    add("    logic [MOSAIC_ID_W_PRF_TAG-1:0] prf_tag;")
    add("    logic [MOSAIC_ID_W_PRF_GEN-1:0] prf_gen;")
    add("  } prf_id_t;")
    add("")
    add("  // A macro identity matches only when the generation agrees. Comparing the")
    add("  // wrapping ROB index alone would let a squashed macro's late completion act")
    add("  // on the macro that has since taken the same entry.")
    add("  function automatic logic macro_id_eq(input macro_id_t a, input macro_id_t b);")
    add("    return (a.hart == b.hart) && (a.rob_gen == b.rob_gen) &&")
    add("           (a.rob_index == b.rob_index) && (a.uop_index == b.uop_index);")
    add("  endfunction")
    add("")
    add("  function automatic logic macro_id_live(input macro_id_t a, input macro_id_t b);")
    add("    return (a.hart == b.hart) && (a.rob_gen == b.rob_gen) && (a.rob_index == b.rob_index);")
    add("  endfunction")
    add("")
    add("  // Age comparison. The sequence modulus strictly exceeds twice the largest")
    add("  // distance two live uops can be separated by, so the sign of the modular")
    add("  // difference is unambiguous and no extra tag bit is needed.")
    add("  function automatic logic seq_older(input logic [MOSAIC_CNT_W_UOP_SEQUENCE-1:0] a,")
    add("                                        input logic [MOSAIC_CNT_W_UOP_SEQUENCE-1:0] b);")
    add("    logic [MOSAIC_CNT_W_UOP_SEQUENCE-1:0] diff;")
    add("    begin")
    add("      diff = a - b;")
    add("      seq_older = (diff != '0) && (diff[MOSAIC_CNT_W_UOP_SEQUENCE-1]);")
    add("    end")
    add("  endfunction")
    add("")
    add("  /* verilator lint_on UNUSEDPARAM */")
    add("  /* verilator lint_on UNUSEDSIGNAL */")
    add("")
    add("endpackage : mosaic_id_pkg")
    add("")
    add("`endif  // MOSAIC_ID_PKG_SV_")
    add("")
    return "\n".join(lines)


def _bit_mask(specs, width: int, where: str) -> int:
    """Turn the table's "msb:lsb" / "bit" strings into one integer mask.

    The table is the only place these numbers live; this function is a decoder,
    not a second copy. A range that does not fit the declared width is a hard
    error rather than a silent truncation, because a truncated mask would make
    the RTL accept a write it should refuse.
    """
    mask = 0
    for spec in specs or []:
        if ":" in spec:
            msb_text, lsb_text = spec.split(":", 1)
            msb, lsb = int(msb_text), int(lsb_text)
        else:
            msb = lsb = int(spec)
        if not (0 <= lsb <= msb < width):
            raise SystemExit(
                "csr %s: bit range %r does not fit a %d-bit register" % (where, spec, width)
            )
        for bit in range(lsb, msb + 1):
            mask |= 1 << bit
    return mask


# Registers whose only legal value is zero because this profile implements no
# privilege mode for the value to name. The clause is in each row's spec_clause:
# "p0 is M-only ... so no synchronous exception can occur in a less privileged
# mode and every bit is WARL whose only legal value is 0" (medeleg) and "With no
# S or U mode there is no delegation target, so every bit is WARL whose only
# legal value is 0" (mideleg). The rule is keyed on the *profile*, not on a
# hand-edited mask, so a profile that adds S or U gets the writable register back
# from the same table without anyone editing this file.
NO_TARGET_WITHOUT_LESS_PRIVILEGE = ("medeleg", "mideleg")

# Fields that only exist when a less-privileged mode exists. The CSR tables are
# shared by every profile, so a field whose whole meaning is "the previous/next
# privilege of a trap, and the memory-privilege override that only a mode below
# M can be overridden to" is described once and narrowed here for a profile that
# has no such mode. Narrowing rather than forbidding keeps one table and one
# rule; the alternative -- a second p0-only table -- is the second copy of a
# fact this generator exists to prevent.
#
#   mstatus.MPP [12:11]  previous privilege: read-only 3 when the only mode is M
#                        (Priv v1.12: "MPP ... implementations that support only
#                        M-mode may hardwire MPP to 3").
#   mstatus.SPP [8]      previous privilege for a trap into S-mode; there is no
#                        S-mode to return to.
#   mstatus.MPRV [17]    "MPRV is read-only 0 if U-mode is not supported."
#   mstatus.SUM [18]     "The SUM ... bit ... permissions apply only to S-mode";
#                        there is no S-mode.
#   mstatus.MXR [19]     same argument as SUM: it selects S-mode's ability to
#                        read a page with execute-only permission.
#   mstatus.TVM [20]     traps S-mode's satp access; no S-mode.
#   mstatus.TW [21]      traps WFI in a less-privileged mode; none exists.
#   mstatus.TSR [22]     traps SRET in S-mode; there is no S-mode.
#   mie/mip bits 9, 5, 1 (SEIE/STIE/SSIE and SEIP/STIP/SSIP) are the supervisor
#                        interrupt enables and pending bits; with no S-mode the
#                        only legal value of each is 0.
#   mcounteren [2:0]     gates counter access *from U-mode* (Priv v1.12: "When
#                        the CY, TM, IR ... bit in mcounteren is clear, attempts
#                        to read ... while executing in U-mode will cause an
#                        illegal instruction exception"). With no U-mode there is
#                        no access to gate and the register is read-only zero.
LESS_PRIVILEGE_ONLY_FIELDS = {
    "mstatus": ("12:11", "8", "17", "18", "19", "20", "21", "22"),
    "mie": ("9", "5", "1"),
    "mip": ("9", "5", "1"),
    "mcounteren": ("2", "1", "0"),
}


def _bit_list_mask(specs) -> int:
    """Mask of a raw "msb:lsb"/"bit" list, without any width checking.

    ``_bit_mask`` validates a range against a register width and is the right
    function for a table's own fields; this one is used to subtract bits that
    the table declares from a mask that has already been validated.
    """
    mask = 0
    for spec in specs or []:
        if ":" in spec:
            msb_text, lsb_text = spec.split(":", 1)
            msb, lsb = int(msb_text), int(lsb_text)
        else:
            msb = lsb = int(spec)
        for bit in range(lsb, msb + 1):
            mask |= 1 << bit
    return mask


def collect_csrs(bundle: config_check.Bundle) -> list:
    """Every CSR this profile implements, once, with its privilege visibility.

    The two CSR tables answer the same question from two sides: mode_m.json lists
    a register in the block of each *privilege mode from which it may be
    accessed*, so a register several modes share (the cycle/time/instret counter
    shadows are readable from M, S and U) appears in more than one block. The
    register itself is one register, so the blocks are merged here by name, and
    two entries for one address that disagree about anything else are a hard
    error rather than a silent last-wins.

    The merged record carries the set of modes, which is what the RTL needs and
    what the testbench checks. The access *rule*, though, is not taken from this
    list: the ISA encodes the minimum privilege of a CSR in its own address
    (bits [9:8] for reads, [11:10] for writes), so the generated package states
    that too, decoded from the address, and the model and the RTL both use it.
    The mode blocks are the implementation statement; the address is the rule.
    """
    profile = bundle.profile
    order = {name: level for level, name in enumerate(("U", "S", "H", "M"))}

    merged = {}
    for table in bundle.csr_tables:
        for block in table["modes"]:
            mode = block["mode"]
            if mode not in order:
                raise SystemExit("csr table declares unknown privilege mode %r" % mode)
            for csr in block["csrs"]:
                key = csr["name"]
                record = merged.get(key)
                if record is None:
                    record = dict(csr)
                    record["modes"] = set()
                    merged[key] = record
                else:
                    for field in ("address", "width", "access", "behavior", "reset",
                                  "writable_fields", "wpri_fields",
                                  "unmodifiable_bits"):
                        if record.get(field) != csr.get(field):
                            raise SystemExit(
                                "csr %s: the %s block and an earlier block disagree "
                                "about %s (%r vs %r); one register has one definition"
                                % (key, mode, field, record.get(field), csr.get(field))
                            )
                record["modes"].add(mode)

    csrs = sorted(merged.values(), key=lambda csr: csr["address"])
    for csr in csrs:
        address = csr["address"]
        # Priv v1.12, "CSR Address Mapping Conventions": csr[9:8] is the lowest
        # privilege level that can access the register, and csr[11:10] is 11 for
        # a read-only register and something else for a read/write one. So the
        # read privilege and the write privilege (when a write is allowed at all)
        # are both csr[9:8]: a write is illegal from a mode below it *and*
        # illegal everywhere when the address says read-only.
        csr["min_priv_r"] = (address >> 8) & 0x3
        csr["min_priv_w"] = 3 if ((address >> 10) & 0x3) == 0x3 else csr["min_priv_r"]
        read_only_addr = csr["min_priv_w"] == 3 and csr["min_priv_r"] != 3
        for mode in csr["modes"]:
            if order[mode] < csr["min_priv_r"]:
                raise SystemExit(
                    "csr %s (0x%03x): declared accessible from %s, but the address "
                    "reserves access to privilege %d and above"
                    % (csr["name"], address, mode, csr["min_priv_r"])
                )
        if csr["access"] != "ro":
            if read_only_addr:
                raise SystemExit(
                    "csr %s (0x%03x): the table declares it writable but the address "
                    "encodes a read-only register" % (csr["name"], address)
                )
            if order[max(csr["modes"], key=lambda m: order[m])] < csr["min_priv_w"]:
                raise SystemExit(
                    "csr %s (0x%03x): declared writable from a mode below the address's "
                    "access privilege %d" % (csr["name"], address, csr["min_priv_w"])
                )
    return csrs


def effective_csr_wmask(csr, less_privileged: bool) -> int:
    """The write mask a profile actually gets for one CSR.

    Two rules narrow the table, both keyed on the profile and not on a
    hand-edited mask: a delegation register whose target mode does not exist has
    no legal value but zero, and a field whose whole meaning is a less-privileged
    mode is read-only when there is no such mode.
    """
    if csr["access"] == "ro":
        return 0
    width = csr["width"]
    wmask = _bit_mask(csr.get("writable_fields", []), width,
                      "csr %s writable_fields" % csr["name"])
    if less_privileged:
        return wmask
    if csr["name"] in NO_TARGET_WITHOUT_LESS_PRIVILEGE:
        return 0
    narrowed = _bit_list_mask(LESS_PRIVILEGE_ONLY_FIELDS.get(csr["name"], ()))
    return wmask & ~narrowed


def render_sv_csr_package(bundle: config_check.Bundle) -> str:
    """Emit the CSR implementation table as a synthesisable package.

    Every address, reset value, writable-bit mask and write-legality flag the CSR
    file needs comes from ``config/csr/mode_m.json`` -- the same file
    tools/check_profile.py validates -- so the RTL and the configuration checker
    cannot disagree about which CSRs exist or which bits of them software may
    change. The RTL names these constants; it does not repeat one of the numbers.

    Access modes map to a mask and a legality flag, and nothing else is derived
    here:

      * ``ro`` / ``fixed``      -> write mask 0, write illegal
      * ``rw`` / ``rwr``        -> write mask = writable_fields, write legal
      * ``wpri_fields``         -> never writable and read back zero, so they are
                                   absent from the write mask by construction.

    The one rule that is not a mask projection is the delegation registers and the
    fields that only exist for a less-privileged mode: with no S or U mode there
    is no delegation target and no less-privileged mode for MPP/SPP/MPRV/SUM/MXR/
    TVM/TW/TSR to name, so those bits collapse to "the only legal value is 0".
    That is applied by ``effective_csr_wmask``, from the profile's own privilege
    list, rather than being written into the RTL as a second opinion.

    Three further constants are emitted per register and they are the ones the
    access check reads:

      * ``MOSAIC_CSR_MODES_<NAME>``      the modes the table declares this
                                         register accessible from, bit i = level i
                                         (U=0, S=1, M=3);
      * ``MOSAIC_CSR_MINPRIV_R/W_<NAME>`` the ISA's own minimum privilege for a
                                         read and for a write, decoded from the
                                         register's address bits [9:8]/[11:10].
    """
    profile = bundle.profile
    less_privileged = bool([mode for mode in profile["privilege_modes"] if mode in ("S", "U")])
    csrs = collect_csrs(bundle)
    modes_list = list(profile["privilege_modes"])

    lines = []
    add = lines.append
    add("// GENERATED FILE - do not edit.")
    add("// Produced by tools/gen_manifest.py --profile %s from the profile's CSR tables." % bundle.name)
    add("//")
    add("// One address, one reset value and one write mask per CSR, decoded from the")
    add("// implementation tables that tools/check_profile.py validates. The CSR file")
    add("// names these constants and carries no second copy of any of them.")
    add("//")
    add("//   MOSAIC_CSR_WMASK_<NAME>        bits software may change (writable_fields)")
    add("//   MOSAIC_CSR_WRITE_LEGAL_<NAME>  whether a write to the register is legal")
    add("//                                  at all (access mode != ro)")
    add("//   MOSAIC_CSR_MODES_<NAME>        modes the table declares it accessible from,")
    add("//                                  bit i = privilege level i")
    add("//   MOSAIC_CSR_MINPRIV_R/W_<NAME>  the minimum privilege a read/write needs,")
    add("//                                  decoded from the register's own address")
    add("//")
    add("// Bits in a WARL/WPRI register that are neither listed as writable nor as")
    add("// write-preserve-zero are reset-only: they read back their reset value.")
    add("")
    add("`ifndef MOSAIC_CSR_PKG_SV_")
    add("`define MOSAIC_CSR_PKG_SV_")
    add("")
    # The RTL has to *compile out* the registers a profile does not have: a
    # reference to a constant the package does not declare is a compile error,
    # not a zero. So which modes exist also travels as a preprocessor flag, from
    # the same profile list the masks above are narrowed from.
    add("// Which privilege modes this profile implements. Both the flags and the")
    add("// MOSAIC_CSR_HAS_S/HAS_U localparams below come from the same list, so the")
    add("// RTL cannot be built for a profile whose registers it names.")
    if "S" in modes_list:
        add("`define MOSAIC_CSR_HAS_S")
    if "U" in modes_list:
        add("`define MOSAIC_CSR_HAS_U")
    add("")
    add("package mosaic_csr_pkg;")
    add("")
    add("  // This package is compiled into every file that includes it, and a warning is")
    add("  // a failure in this project. The CSR file names only the constants its own")
    add("  // roles need and the testbench table uses the rest, so both the unused-")
    add("  // parameter and unused-signal checks are silenced here rather than at each")
    add("  // include site.")
    add("  /* verilator lint_off UNUSEDPARAM */")
    add("  /* verilator lint_off UNUSEDSIGNAL */")
    add("")
    add("  localparam int unsigned MOSAIC_CSR_COUNT = %d;" % len(csrs))
    add("")
    add("  // Privilege levels, and which of them this profile implements. The RTL reads")
    add("  // \"is there a mode below M\" from here rather than naming a profile.")
    add("  localparam logic [1:0] MOSAIC_PRIV_U = 2'd0;")
    add("  localparam logic [1:0] MOSAIC_PRIV_S = 2'd1;")
    add("  localparam logic [1:0] MOSAIC_PRIV_M = 2'd3;")
    add("  localparam logic MOSAIC_CSR_HAS_S = 1'b%d;" % (1 if "S" in modes_list else 0))
    add("  localparam logic MOSAIC_CSR_HAS_U = 1'b%d;" % (1 if "U" in modes_list else 0))
    add("  localparam logic [1:0] MOSAIC_PRIV_LEAST = 2'd%d;"
        % (0 if "U" in modes_list else (1 if "S" in modes_list else 3)))
    add("")

    for csr in csrs:
        name = csr["name"].upper()
        width = csr["width"]
        access = csr["access"]
        write_legal = access != "ro"
        wmask = effective_csr_wmask(csr, less_privileged)
        note = ""
        table_wmask = 0
        if write_legal:
            table_wmask = _bit_mask(csr.get("writable_fields", []), width,
                                    "csr %s writable_fields" % csr["name"])
        if table_wmask != wmask:
            note = ("  // %s: the table declares bits 0x%X writable, but profile %s has no\n"
                    "  // less-privileged mode for them to name, so every bit is WARL whose\n"
                    "  // only legal value is 0; writes are accepted and canonicalise to 0."
                    % (csr["name"], table_wmask & ~wmask, bundle.name))
        modes_mask = 0
        for mode in csr["modes"]:
            modes_mask |= {"U": 1 << 0, "S": 1 << 1, "H": 1 << 2, "M": 1 << 3}[mode]
        add("  // ---------------------------------------------------------------- %s" % csr["name"])
        add("  // 0x%03X, %d-bit, access %s, %s, reset 0x%X, modes %s"
            % (csr["address"], width, access, csr["behavior"], csr["reset"],
               "/".join(sorted(csr["modes"]))))
        if note:
            add(note)
        add("  localparam logic [11:0] MOSAIC_CSR_ADDR_%-11s = 12'h%03x;"
            % (name, csr["address"]))
        add("  localparam logic [63:0] MOSAIC_CSR_RESET_%-10s = %s;"
            % (name, _hex64(csr["reset"])))
        add("  localparam logic [63:0] MOSAIC_CSR_WMASK_%-10s = %s;"
            % (name, _hex64(wmask)))
        add("  localparam logic        MOSAIC_CSR_WRITE_LEGAL_%-2s = 1'b%d;"
            % (name, 1 if write_legal else 0))
        add("  localparam logic [3:0]  MOSAIC_CSR_MODES_%-7s = 4'b%s;"
            % (name, format(modes_mask, "04b")))
        add("  localparam logic [1:0]  MOSAIC_CSR_MINPRIV_R_%-5s = 2'd%d;"
            % (name, csr["min_priv_r"]))
        add("  localparam logic [1:0]  MOSAIC_CSR_MINPRIV_W_%-5s = 2'd%d;"
            % (name, csr["min_priv_w"]))
        add("")

    add("  /* verilator lint_on UNUSEDSIGNAL */")
    add("  /* verilator lint_on UNUSEDPARAM */")
    add("")
    add("endpackage : mosaic_csr_pkg")
    add("")
    add("`endif  // MOSAIC_CSR_PKG_SV_")
    add("")
    return "\n".join(lines)


def render_csr_header(bundle: config_check.Bundle) -> str:
    """Emit the CSR implementation table for the C++ testbench.

    The unit test's independent model needs the same addresses, reset values and
    write masks the RTL uses. Hand-copying them into sim/unit/tb_csr.cpp would be
    a second copy of a configured number -- the thing this project refuses -- so
    they arrive from the same decode of the same table. What stays independent is
    the model's *behaviour*: the WARL canonicalisation, the trap/MRET field
    transitions, the boundary priority and the counter arithmetic are written
    from the contract prose in rtl/core/mosaic_csr.sv and share no code with it.
    """
    profile = bundle.profile
    less_privileged = bool([mode for mode in profile["privilege_modes"] if mode in ("S", "U")])
    csrs = collect_csrs(bundle)

    lines = []
    add = lines.append
    add("// GENERATED FILE - do not edit.")
    add("// Produced by tools/gen_manifest.py --profile %s from the profile's CSR tables." % bundle.name)
    add("//")
    add("// The unit test's shadow model reads its table from here so there is exactly")
    add("// one copy of every CSR address, reset value and write mask in the tree; the")
    add("// model's behaviour is written independently from the RTL's contract.")
    add("")
    add("#ifndef MOSAIC_CSR_TABLE_H_")
    add("#define MOSAIC_CSR_TABLE_H_")
    add("")
    add("#include <stdint.h>")
    add("")
    add("#define MOSAIC_CSR_COUNT %d" % len(csrs))
    add("")
    add("typedef struct {")
    add("  const char *name;")
    add("  uint16_t    addr;         /* 12-bit CSR number */")
    add("  uint64_t    reset;        /* reset value */")
    add("  uint64_t    wmask;        /* bits software may change */")
    add("  uint8_t     write_legal;  /* 0 = a write is an illegal CSR access */")
    add("  uint8_t     min_priv_r;   /* minimum privilege a read needs */")
    add("  uint8_t     min_priv_w;   /* minimum privilege a write needs */")
    add("} mosaic_csr_desc_t;")
    add("")
    add("static const mosaic_csr_desc_t MOSAIC_CSR_TABLE[MOSAIC_CSR_COUNT] = {")
    for csr in csrs:
        access = csr["access"]
        write_legal = access != "ro"
        wmask = effective_csr_wmask(csr, less_privileged)
        add('  { "%s", 0x%03x, UINT64_C(0x%016x), UINT64_C(0x%016x), %d, %d, %d },'
            % (csr["name"], csr["address"], csr["reset"], wmask,
               1 if write_legal else 0, csr["min_priv_r"], csr["min_priv_w"]))
    add("};")
    add("")
    add("#endif  // MOSAIC_CSR_TABLE_H_")
    add("")
    return "\n".join(lines)


def render_csr_rules_header(bundle: config_check.Bundle) -> str:
    """Emit the V-017 rule ledger as a C++ table for CASE=csr.rule_ledger.

    The case is *generated* from the ledger: the driver walks this table, emits
    one stimulus per example, and asserts at the end that every rule was visited.
    A rule therefore cannot be silently skipped -- it would leave the coverage
    count short and fail the case -- and the ledger, not the driver, is the one
    place that says what each relationship is.
    """
    ledger = bundle.csr_rules
    lines = []
    add = lines.append
    add("// GENERATED FILE - do not edit.")
    add("// Produced by tools/gen_manifest.py --profile %s from config/csr/rule_ledger.json." % bundle.name)
    add("//")
    add("// One rule per legality relationship and two examples per rule (positive and")
    add("// negative). The driver builds its program from MOSAIC_CSR_EXAMPLES and its")
    add("// coverage assertion from MOSAIC_CSR_RULE_IDS, so a rule that is added to the")
    add("// ledger without a reachable example fails the case.")
    add("")
    add("#ifndef MOSAIC_CSR_RULES_H_")
    add("#define MOSAIC_CSR_RULES_H_")
    add("")
    add("#include <stdint.h>")
    add("")

    rules = []
    examples = []
    if ledger is not None and ledger.get("profile") == bundle.name:
        rules = ledger["rules"]
        for rindex, rule in enumerate(rules):
            for negative, tag in ((0, "positive"), (1, "negative")):
                examples.append((rindex, negative, rule, rule[tag]))

    add("#define MOSAIC_CSR_RULE_COUNT %d" % len(rules))
    add("#define MOSAIC_CSR_EXAMPLE_COUNT %d" % len(examples))
    add("")
    add("enum { MOSAIC_RULE_STRICT = 0, MOSAIC_RULE_WARL_ALLOWED = 1,")
    add("       MOSAIC_RULE_READ_ONLY = 2, MOSAIC_RULE_ALIAS = 3,")
    add("       MOSAIC_RULE_PERMISSION = 4 };")
    add("enum { MOSAIC_EX_CSRRW = 0, MOSAIC_EX_CSRRS = 1, MOSAIC_EX_CSRRC = 2,")
    add("       MOSAIC_EX_CSRR = 3 };")
    add("enum { MOSAIC_EXP_READ = 0, MOSAIC_EXP_RANGE = 1, MOSAIC_EXP_ADVANCE = 2,")
    add("       MOSAIC_EXP_CANARY = 3 };")
    add("")
    add("typedef struct {")
    add("  uint16_t address;")
    add("  uint8_t  kind;")
    add("  const char *field;")
    add("  const char *clause;")
    add("} mosaic_csr_rule_desc_t;")
    add("")
    add("typedef struct {")
    add("  uint16_t rule;          /* index into MOSAIC_CSR_RULE_IDS */")
    add("  uint8_t  negative;      /* 0 = positive example, 1 = negative */")
    add("  uint8_t  op;            /* MOSAIC_EX_* */")
    add("  uint8_t  traps;         /* illegal-instruction exceptions required */")
    add("  uint8_t  expect_mode;   /* MOSAIC_EXP_* */")
    add("  uint8_t  pre_read;")
    add("  uint8_t  has_forbid;")
    add("  uint16_t target;        /* the CSR the read is aimed at */")
    add("  uint16_t write_target;  /* the CSR the write is aimed at */")
    add("  uint64_t write;")
    add("  uint64_t expect_lo;     /* read/range-min/canary, or advance min_delta */")
    add("  uint64_t expect_hi;     /* range max */")
    add("  uint16_t advance_gap;   /* instructions between the two advance reads */")
    add("  uint64_t forbid;")
    add("} mosaic_csr_example_t;")
    add("")
    if rules:
        add("static const char *const MOSAIC_CSR_RULE_IDS[MOSAIC_CSR_RULE_COUNT] = {")
        for rule in rules:
            add('  "%s",' % rule["id"])
        add("};")
        add("")
        add("static const mosaic_csr_rule_desc_t MOSAIC_CSR_RULES[MOSAIC_CSR_RULE_COUNT] = {")
        kind_index = {"strict": 0, "warl_allowed": 1, "read_only": 2, "alias": 3,
                      "permission": 4}
        for rule in rules:
            add('  { 0x%03x, %d, "%s",'
                % (rule["address"], kind_index[rule["kind"]], rule["field"]))
            add('    "%s" },' % rule["clause"].replace("\\", "\\\\").replace('"', '\\"'))
        add("};")
        add("")
        add("static const mosaic_csr_example_t MOSAIC_CSR_EXAMPLES[MOSAIC_CSR_EXAMPLE_COUNT] = {")
        op_index = {"csrrw": 0, "csrrs": 1, "csrrc": 2, "csrr": 3}
        for rindex, negative, rule, example in examples:
            target = int(example.get("target", "0x%03x" % rule["address"]), 16)
            write_target = int(example.get("write_target", "0x%03x" % target), 16)
            exp = example["expect"]
            if "read" in exp:
                mode, lo, hi, gap = 0, int(exp["read"], 16), 0, 0
            elif "range" in exp:
                mode, lo, hi, gap = 1, int(exp["range"][0], 16), int(exp["range"][1], 16), 0
            elif "advance" in exp:
                mode, lo, hi, gap = 2, int(exp["advance"]["min_delta"]), 0, int(exp["advance"]["gap"])
            else:
                mode, lo, hi, gap = 3, int(exp["canary"], 16), 0, 0
            forbid = int(example["forbid"], 16) if "forbid" in example else 0
            add("  { %d, %d, %d, %d, %d, %d, %d, 0x%03x, 0x%03x,"
                % (rindex, negative, op_index[example["op"]], example["traps"], mode,
                   1 if example.get("pre_read") else 0, 1 if "forbid" in example else 0,
                   target, write_target))
            add("    UINT64_C(0x%016x), UINT64_C(0x%016x), UINT64_C(0x%016x), %d, UINT64_C(0x%016x) },"
                % (int(example["write"], 16), lo, hi, gap, forbid))
        add("};")
    else:
        # No ledger for this profile: the case must fail rather than pass
        # vacuously, so the tables exist but the counts are zero.
        add("static const char *const MOSAIC_CSR_RULE_IDS[1] = { \"(no ledger)\" };")
        add("static const mosaic_csr_rule_desc_t MOSAIC_CSR_RULES[1] = { { 0, 0, \"\", \"\" } };")
        add("static const mosaic_csr_example_t MOSAIC_CSR_EXAMPLES[1] = { { 0, 0, 0, 0, 0, 0, 0, 0, 0,")
        add("  UINT64_C(0), UINT64_C(0), UINT64_C(0), 0, UINT64_C(0) } };")
    add("")
    add("#endif  // MOSAIC_CSR_RULES_H_")
    add("")
    return "\n".join(lines)


def render_platform_header(bundle: config_check.Bundle) -> str:
    """Emit the C-visible platform contract for the simulation harness.

    The memory map and the test protocol reach the harness as generated constants
    rather than as a JSON file parsed at run time, so the harness, the firmware
    and the RTL cannot disagree about where TOHOST lives.
    """
    protocol = bundle.profile["test_protocol"]
    lines = []
    add = lines.append
    add("// GENERATED FILE - do not edit.")
    add("// Produced by tools/gen_manifest.py --profile %s." % bundle.name)
    add("// The memory map and test protocol come from the frozen profile configuration.")
    add("#ifndef MOSAIC_PLATFORM_H_")
    add("#define MOSAIC_PLATFORM_H_")
    add("")
    add("#include <stdint.h>")
    add("")
    add("#define MOSAIC_PROFILE_NAME \"%s\"" % bundle.name)
    add("#define MOSAIC_RESET_VECTOR UINT64_C(0x%x)" % bundle.profile["reset"]["reset_vector"])
    add("")
    add("// ---- physical memory map ----")
    for region in sorted(bundle.memory["regions"], key=lambda r: r["base"]):
        upper = "MOSAIC_%s_BASE" % region["name"].upper()
        add("#define %-34s UINT64_C(0x%016x)" % (upper, region["base"]))
        add("#define MOSAIC_%-30s UINT64_C(0x%016x)" % (region["name"].upper() + "_SIZE", region["size"]))
        add("#define MOSAIC_%-30s %d" % (region["name"].upper() + "_CACHEABLE",
                                        1 if region["cacheable"] else 0))
        add("#define MOSAIC_%-30s %d" % (region["name"].upper() + "_ATOMIC_GRANULE",
                                        region["atomic_granule"]))
    add("")
    add("// ---- test protocol (must match tests/programs and rtl/soc) ----")
    add("#define MOSAIC_TOHOST          UINT64_C(0x%x)" % protocol["tohost"])
    add("#define MOSAIC_FROMHOST        UINT64_C(0x%x)" % protocol["fromhost"])
    add("#define MOSAIC_SIGNATURE_ADDR  UINT64_C(0x%x)" % protocol["signature"])
    add("#define MOSAIC_SIGNATURE_WORDS %d" % protocol["signature_words"])
    add("#define MOSAIC_TEST_PASS_CODE  UINT32_C(%d)" % protocol["pass_code"])
    add("")
    add("#endif  // MOSAIC_PLATFORM_H_")
    add("")
    return "\n".join(lines)


def _collect_filelist() -> list:
    """Every RTL source under rtl/, packages first, then modules.

    Derived from the tree rather than from a hand-maintained list: see the note
    above `_is_package_source`. The old implementation joined each group's
    repo-relative path onto RTL_ROOT, so `rtl/core/filelist.f` became
    `rtl/rtl/core/filelist.f`, every group was skipped, and the generated list
    was silently empty -- a manifest that reports zero sources while the design
    has twenty is worse than no manifest, because downstream tools trust it.
    """
    found = []
    for base, dirs, files in os.walk(RTL_ROOT):
        dirs[:] = [d for d in dirs if d != "build"]
        for name in files:
            if name.endswith((".sv", ".v")):
                found.append(os.path.join(base, name))
    found.sort()
    packages = [path for path in found if _is_package_source(path)]
    modules = [path for path in found if path not in packages]
    return packages + modules


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", required=True)
    parser.add_argument("--out", help="output directory (default build/<profile>)")
    args = parser.parse_args()

    bundle = config_check.load(args.profile)
    if not bundle.ok:
        for problem in bundle.problems:
            print("ERROR %s" % problem, file=sys.stderr)
        print(
            "refusing to write a build manifest for a configuration that does not check out",
            file=sys.stderr,
        )
        return 1

    advertised, pending = config_check.advertised_capabilities(bundle)
    out_dir = args.out or os.path.join(BUILD_ROOT, args.profile)
    rtl_out = os.path.join(out_dir, "rtl")
    os.makedirs(rtl_out, exist_ok=True)

    manifest = {
        "schema_version": 1,
        "profile": args.profile,
        "xlen": bundle.profile["xlen"],
        "isa_string": config_check.isa_string(advertised, bundle.profile["xlen"]),
        "isa_string_claimed": config_check.isa_string(
            bundle.profile["isa_target"]["extensions"], bundle.profile["xlen"]
        ),
        "misa_reset": config_check.misa_value(advertised, bundle.profile["xlen"]),
        "claimed_capabilities": bundle.profile["isa_target"]["extensions"],
        "advertised_capabilities": advertised,
        "not_yet_implemented": pending,
        "privilege_modes": bundle.profile["privilege_modes"],
        "harts": bundle.profile["harts"],
        "misalignment": bundle.profile["misalignment"],
        "reset_vector": bundle.profile["reset"]["reset_vector"],
        "memory_map": [
            {
                "name": region["name"],
                "base": region["base"],
                "size": region["size"],
                "kind": region["kind"],
                "cacheable": region["cacheable"],
                "atomic_granule": region["atomic_granule"],
            }
            for region in sorted(bundle.memory["regions"], key=lambda r: r["base"])
        ],
        "csr_count": bundle.csr_count,
        "geometry": bundle.geometry,
        "tool_versions": tool_versions(),
        "input_hashes": bundle.input_hashes,
    }

    manifest_path = os.path.join(out_dir, "manifest.json")
    with open(manifest_path, "w") as handle:
        json.dump(manifest, handle, indent=2, sort_keys=True)
        handle.write("\n")

    sv_path = os.path.join(rtl_out, "mosaic_cfg_pkg.svh")
    with open(sv_path, "w") as handle:
        handle.write(render_sv_package(bundle, advertised))

    id_path = os.path.join(rtl_out, "mosaic_id_pkg.svh")
    with open(id_path, "w") as handle:
        handle.write(render_sv_id_package(bundle))

    csr_path = os.path.join(rtl_out, "mosaic_csr_pkg.svh")
    with open(csr_path, "w") as handle:
        handle.write(render_sv_csr_package(bundle))

    sim_out = os.path.join(out_dir, "sim")
    os.makedirs(sim_out, exist_ok=True)
    with open(os.path.join(sim_out, "mosaic_platform.h"), "w") as handle:
        handle.write(render_platform_header(bundle))

    with open(os.path.join(sim_out, "mosaic_csr_table.h"), "w") as handle:
        handle.write(render_csr_header(bundle))

    with open(os.path.join(sim_out, "mosaic_csr_rules.h"), "w") as handle:
        handle.write(render_csr_rules_header(bundle))

    sources = _collect_filelist()
    list_path = os.path.join(rtl_out, "filelist.f")
    with open(list_path, "w") as handle:
        for source in sources:
            handle.write("%s\n" % os.path.relpath(source, config_check.REPO_ROOT))

    print("profile %s: wrote %s" % (args.profile, os.path.relpath(manifest_path, config_check.REPO_ROOT)))
    print("  isa_string      : %s" % manifest["isa_string"])
    print("  misa reset      : 0x%016x" % manifest["misa_reset"])
    print("  advertised      : %s" % (", ".join(advertised) or "(none yet)"))
    print("  not yet impl    : %s" % (", ".join(pending) or "(none)"))
    print("  rtl sources     : %d" % len(sources))
    return 0


if __name__ == "__main__":
    sys.exit(main())