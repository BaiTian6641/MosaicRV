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
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from mosaic import config_check  # noqa: E402

BUILD_ROOT = os.path.join(config_check.REPO_ROOT, "build")
RTL_ROOT = os.path.join(config_check.REPO_ROOT, "rtl")

# Ordered source lists. Order matters: packages and typedefs must be elaborated
# before the modules that use them, and Verilator/Yosys both honour file order.
FILELIST_GROUPS = [
    ("rtl/common/filelist.f", "common"),
    ("rtl/core/filelist.f", "core"),
    ("rtl/fabric/filelist.f", "fabric"),
    ("rtl/vector/filelist.f", "vector"),
    ("rtl/soc/filelist.f", "soc"),
]


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

    add("  // Physical memory map")
    for region in sorted(bundle.memory["regions"], key=lambda r: r["base"]):
        base_name = "MOSAIC_%s_BASE" % region["name"].upper()
        size_name = "MOSAIC_%s_SIZE" % region["name"].upper()
        add("  localparam logic [63:0] %-34s = %s;" % (base_name, _hex64(region["base"])))
        add("  localparam logic [63:0] %-34s = %s;" % (size_name, _hex64(region["size"])))
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
    add("")
    add("package mosaic_id_pkg;")
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
    add("endpackage : mosaic_id_pkg")
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

    The one rule that is not a mask projection is the delegation registers: with
    no S or U mode there is no delegation target, so the table's "the whole
    register is a WARL field" collapses to "the only legal value is 0". That is
    applied here, from the profile's own privilege list, rather than being
    written into the RTL as a second opinion.
    """
    profile = bundle.profile
    less_privileged = [mode for mode in profile["privilege_modes"] if mode in ("S", "U")]

    csrs = []
    for table in bundle.csr_tables:
        for block in table["modes"]:
            for csr in block["csrs"]:
                csrs.append(csr)
    csrs.sort(key=lambda csr: csr["address"])

    seen = {}
    for csr in csrs:
        if csr["address"] in seen:
            raise SystemExit(
                "csr %s: address 0x%03x already emitted for %s; the generated package "
                "cannot hold two registers at one number"
                % (csr["name"], csr["address"], seen[csr["address"]])
            )
        seen[csr["address"]] = csr["name"]

    lines = []
    add = lines.append
    add("// GENERATED FILE - do not edit.")
    add("// Produced by tools/gen_manifest.py --profile %s from config/csr/mode_m.json." % bundle.name)
    add("//")
    add("// One address, one reset value and one write mask per CSR, decoded from the")
    add("// implementation table that tools/check_profile.py validates. The CSR file")
    add("// names these constants and carries no second copy of any of them.")
    add("//")
    add("//   MOSAIC_CSR_WMASK_<NAME>        bits software may change (writable_fields)")
    add("//   MOSAIC_CSR_WRITE_LEGAL_<NAME>  whether a write to the register is legal")
    add("//                                  at all (access mode != ro)")
    add("//")
    add("// Bits in a WARL/WPRI register that are neither listed as writable nor as")
    add("// write-preserve-zero are reset-only: they read back their reset value.")
    add("")
    add("`ifndef MOSAIC_CSR_PKG_SV_")
    add("`define MOSAIC_CSR_PKG_SV_")
    add("")
    add("package mosaic_csr_pkg;")
    add("")
    add("  localparam int unsigned MOSAIC_CSR_COUNT = %d;" % len(csrs))
    add("")

    for csr in csrs:
        name = csr["name"].upper()
        width = csr["width"]
        access = csr["access"]
        write_legal = access != "ro"
        if write_legal:
            wmask = _bit_mask(csr.get("writable_fields", []), width,
                              "csr %s writable_fields" % csr["name"])
        else:
            wmask = 0
        note = ""
        if csr["name"] in NO_TARGET_WITHOUT_LESS_PRIVILEGE and not less_privileged:
            if wmask != 0:
                note = ("  // %s: no S or U mode in profile %s, so there is no delegation\n"
                        "  // target and every bit is WARL whose only legal value is 0;\n"
                        "  // writes are accepted and canonicalise to 0."
                        % (csr["name"], bundle.name))
            wmask = 0
        add("  // ---------------------------------------------------------------- %s" % csr["name"])
        add("  // 0x%03X, %d-bit, access %s, %s, reset 0x%X"
            % (csr["address"], width, access, csr["behavior"], csr["reset"]))
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
    less_privileged = [mode for mode in profile["privilege_modes"] if mode in ("S", "U")]

    csrs = []
    for table in bundle.csr_tables:
        for block in table["modes"]:
            for csr in block["csrs"]:
                csrs.append(csr)
    csrs.sort(key=lambda csr: csr["address"])

    lines = []
    add = lines.append
    add("// GENERATED FILE - do not edit.")
    add("// Produced by tools/gen_manifest.py --profile %s from config/csr/mode_m.json." % bundle.name)
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
    add("} mosaic_csr_desc_t;")
    add("")
    add("static const mosaic_csr_desc_t MOSAIC_CSR_TABLE[MOSAIC_CSR_COUNT] = {")
    for csr in csrs:
        width = csr["width"]
        access = csr["access"]
        write_legal = access != "ro"
        wmask = _bit_mask(csr.get("writable_fields", []), width,
                          "csr %s writable_fields" % csr["name"]) if write_legal else 0
        if csr["name"] in NO_TARGET_WITHOUT_LESS_PRIVILEGE and not less_privileged:
            wmask = 0
        add('  { "%-11s", 0x%03x, UINT64_C(0x%016x), UINT64_C(0x%016x), %d },'
            % (csr["name"], csr["address"], csr["reset"], wmask, 1 if write_legal else 0))
    add("};")
    add("")
    add("#endif  // MOSAIC_CSR_TABLE_H_")
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
    sources = []
    for rel, _label in FILELIST_GROUPS:
        path = os.path.join(RTL_ROOT, rel)
        if not os.path.exists(path):
            continue
        with open(path) as handle:
            for raw in handle:
                line = raw.split("#", 1)[0].strip()
                if line:
                    sources.append(os.path.join(RTL_ROOT, os.path.dirname(rel), line))
    return sources


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