#!/usr/bin/env python3
"""Check the frozen architectural event interface against its three producers.

    python3 tools/check_event_contract.py --profile p0
    python3 tools/check_event_contract.py --profile p0 --negative

`config/contracts/event_v1.json` is the frozen V-008 interface. This tool proves
that the three things that must agree, do:

  * the SystemVerilog tap -- `rtl/core/mosaic_retire.sv`'s per-lane `ev_*` ports
    (combinational, one record per acknowledged lane, up to the retire width) and
    `sim/tb/mosaic_bringup_tb.sv`'s `c_evt_*` wrapper ports (registered, one
    record per commit);
  * the host record `sim/common/event_tap.h`'s `RetireEvent`;
  * the one serialiser `sim/common/event_codec.h`/`.cpp` -- its field list,
    widths, order, kinds and compiled schema version.

and that the schema itself is well formed: every field states its width, meaning,
validity rule and sampling instant; every field is either architectural or
observed and says which; no two fields claim the same identity semantics; every
architectural effect carries exactly one hart and one instruction; the byte
layout is the one the record actually has; and every rule in the file names a
check this tool implements.

Disagreements are reported as errors naming the offending field, never averaged
away. A declaration wider than the schema's field is a *host container* and must
be recorded with its exact declared width and a reason; a narrower one is a
truncation and is rejected. Where the schema and the RTL genuinely disagree about
*what a producer supplies* (the out-of-order retire path has no `insn`, no
`pc_after` and no per-lane `epc`; the memory endpoint has no event tap at all) the
schema records the required port and this tool prints it on every run, so a gap
is never implied and never silent.

Exit status is 0 only when the whole interface checks out. `--negative` additionally
mutates one input at a time in a sandbox and requires the mutations to be
rejected -- a negative control that is merely present is not evidence.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import sys
import tempfile
from typing import Any, Dict, List, Optional, Tuple

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

SCHEMA_REL = os.path.join("config", "contracts", "event_v1.json")
CODEC_REL = os.path.join("sim", "common", "event_codec.h")
RETIRE_REL = os.path.join("rtl", "core", "mosaic_retire.sv")
TAP_SV_REL = os.path.join("sim", "tb", "mosaic_bringup_tb.sv")
TAP_H_REL = os.path.join("sim", "common", "event_tap.h")

# The files a negative control may mutate. They are the interface itself: the
# schema, the two RTL producers and the two C++ views of it.
INTERFACE_FILES = [SCHEMA_REL, CODEC_REL, TAP_H_REL, TAP_SV_REL, RETIRE_REL]

# group -> the kinds a field of that group can be valid in, and whether the group
# is an architectural effect or an observed fact. These are the frozen semantics
# from docs/validation-plan.md#3: a trap does not pretend to have retired, a
# committed store's visibility is not its retirement, and a debug-only
# microarchitectural identity is not architectural state.
GROUP_KINDS = {
    "identity": ("RETIRE", "TRAP", "MEM_VISIBLE"),
    "timing": ("RETIRE", "TRAP", "MEM_VISIBLE"),
    "kind": ("RETIRE", "TRAP", "MEM_VISIBLE"),
    "instruction": ("RETIRE", "TRAP"),
    "microarch_identity": ("RETIRE", "TRAP"),
    "gpr": ("RETIRE",),
    "csr": ("RETIRE",),
    "memory": ("RETIRE", "MEM_VISIBLE"),
    "trap": ("TRAP",),
}
GROUP_CLASS = {
    "identity": "architectural",
    "timing": "observed",
    "kind": "architectural",
    "instruction": "architectural",
    "microarch_identity": "observed",
    "gpr": "architectural",
    "csr": "architectural",
    "memory": "architectural",
    "trap": "architectural",
}

# The invariants this tool implements. A rule in the schema that names one of
# these is enforced; a rule that names anything else, or an invariant no rule
# names, is a problem (INV-RULES).
INVARIANTS = (
    "INV-COMPLETE",
    "INV-ENCODING",
    "INV-VERSION",
    "INV-CPPFIELDS",
    "INV-SOURCE",
    "INV-TAPDECL",
    "INV-RTLWIDTH",
    "INV-IDENTITY",
    "INV-LOCATE",
    "INV-KINDS",
    "INV-VECTOR",
    "INV-RULES",
)


class Problem(object):
    __slots__ = ("where", "message", "invariant")

    def __init__(self, where: str, message: str, invariant: str = "INV-SOURCE") -> None:
        self.where = where
        self.message = message
        self.invariant = invariant

    def __str__(self) -> str:
        # The invariant is printed, not implied: a rejected interface says which
        # check rejected it, so the negative controls below are evidence about a
        # named check rather than about "the tool exited non-zero".
        return "[%s] %s: %s" % (self.invariant, self.where, self.message)


# ---------------------------------------------------------------------------
# expressions: the subset of SystemVerilog and of the schema's width language
# ---------------------------------------------------------------------------


class ExprError(Exception):
    pass


TOKEN_RE = re.compile(
    r"\s*(?:(?P<number>\d[\d_]*)|"
    r"(?P<name>\$?[A-Za-z_][A-Za-z0-9_]*)|"
    r"(?P<op>::|<=|>=|==|!=|&&|\|\||[-+*/%()?:!<>]))"
)


def tokenize(text: str) -> List[str]:
    tokens: List[str] = []
    position = 0
    while position < len(text):
        match = TOKEN_RE.match(text, position)
        if not match:
            raise ExprError("cannot tokenize %r at offset %d" % (text, position))
        tokens.append(match.group(1) or match.group(2) or match.group(3))
        position = match.end()
    return tokens


class Expression(object):
    """Recursive-descent parser for the expression subset the two sides use.

    Supports integers (underscores allowed), names, `pkg::NAME` qualified names,
    `$clog2`/`clog2`, `+ - * / %`, `!`, comparisons, `&&`, `||`, parentheses and
    the ternary `?:`. Nothing else: no calls, no attributes, no subscripts, so a
    width expression cannot reach out of its namespace.
    """

    def __init__(self, tokens: List[str], text: str) -> None:
        self.tokens = tokens
        self.text = text
        self.position = 0

    def peek(self) -> Optional[str]:
        return self.tokens[self.position] if self.position < len(self.tokens) else None

    def take(self) -> str:
        token = self.peek()
        if token is None:
            raise ExprError("unexpected end of %r" % self.text)
        self.position += 1
        return token

    def expect(self, token: str) -> None:
        got = self.take()
        if got != token:
            raise ExprError("expected %r in %r, got %r" % (token, self.text, got))

    def parse(self):
        node = self.ternary()
        if self.peek() is not None:
            raise ExprError("trailing %r in %r" % (self.peek(), self.text))
        return node

    def ternary(self):
        condition = self.binary(0)
        if self.peek() == "?":
            self.take()
            when_true = self.ternary()
            self.expect(":")
            when_false = self.ternary()
            return ("ternary", condition, when_true, when_false)
        return condition

    LEVELS = [
        ("||",),
        ("&&",),
        ("==", "!="),
        ("<", "<=", ">", ">="),
        ("+", "-"),
        ("*", "/", "%"),
    ]

    def binary(self, level: int):
        if level >= len(self.LEVELS):
            return self.unary()
        node = self.binary(level + 1)
        while self.peek() in self.LEVELS[level]:
            operator = self.take()
            right = self.binary(level + 1)
            node = ("bin", operator, node, right)
        return node

    def unary(self):
        token = self.peek()
        if token in ("!", "-", "+"):
            self.take()
            return ("unary", token, self.unary())
        return self.primary()

    def primary(self):
        token = self.take()
        if token.isdigit():
            return ("number", int(token.replace("_", "")))
        if token == "(":
            node = self.ternary()
            self.expect(")")
            return node
        if re.match(r"^\$?[A-Za-z_]", token):
            name = token
            while self.peek() == "::":
                self.take()
                name += "::" + self.take()
            if self.peek() == "(":
                if name not in ("clog2", "$clog2"):
                    raise ExprError("only clog2 may be called in %r" % self.text)
                self.take()
                argument = self.ternary()
                self.expect(")")
                return ("call", name, argument)
            return ("name", name)
        raise ExprError("unexpected %r in %r" % (token, self.text))


def clog2(value: int) -> int:
    if value <= 0:
        raise ValueError("clog2 requires a positive value, got %r" % value)
    return (value - 1).bit_length()


class Namespace(object):
    """Lazily evaluated names: file localparams may refer to each other."""

    def __init__(self, values: Dict[str, int]) -> None:
        self.values: Dict[str, int] = dict(values)
        self.texts: Dict[str, str] = {}
        self.resolving: List[str] = []

    def define(self, name: str, text: str) -> None:
        self.texts[name] = text

    def resolve(self, name: str) -> int:
        if name in self.values:
            return self.values[name]
        if name not in self.texts:
            raise ExprError("unknown name %r" % name)
        if name in self.resolving:
            raise ExprError("cyclic definition of %r" % name)
        self.resolving.append(name)
        try:
            value = evaluate(self.texts[name], self)
        finally:
            self.resolving.pop()
        self.values[name] = value
        return value


def evaluate(text: str, namespace: Namespace) -> int:
    tree = Expression(tokenize(text), text).parse()

    def walk(node) -> int:
        kind = node[0]
        if kind == "number":
            return node[1]
        if kind == "name":
            return namespace.resolve(node[1])
        if kind == "call":
            return clog2(walk(node[2]))
        if kind == "unary":
            value = walk(node[2])
            if node[1] == "!":
                return 0 if value else 1
            return -value if node[1] == "-" else value
        if kind == "bin":
            operator = node[1]
            left = walk(node[2])
            right = walk(node[3])
            if operator == "+":
                return left + right
            if operator == "-":
                return left - right
            if operator == "*":
                return left * right
            if operator == "/":
                if right == 0:
                    raise ExprError("division by zero")
                return left // right
            if operator == "%":
                return left % right
            if operator == "==":
                return 1 if left == right else 0
            if operator == "!=":
                return 1 if left != right else 0
            if operator == "<":
                return 1 if left < right else 0
            if operator == "<=":
                return 1 if left <= right else 0
            if operator == ">":
                return 1 if left > right else 0
            if operator == ">=":
                return 1 if left >= right else 0
            if operator == "&&":
                return 1 if (left and right) else 0
            if operator == "||":
                return 1 if (left or right) else 0
            raise ExprError("unsupported operator %r" % operator)
        if kind == "ternary":
            return walk(node[2]) if walk(node[1]) else walk(node[3])
        raise ExprError("unsupported expression node %r" % (kind,))

    return walk(tree)


# ---------------------------------------------------------------------------
# reading the producers
# ---------------------------------------------------------------------------

RETIRE_LOCALPARAM_RE = re.compile(
    r"^\s*localparam\s+int\s+unsigned\s+([A-Za-z_]\w*)\s*=\s*([^;]*);", re.M
)
RETIRE_PORT_RE = re.compile(
    r"^\s*output\s+logic\s*\[\s*(.+?)\s*-\s*1\s*:\s*0\s*\]\s*(ev_\w+)\s*,?\s*$"
)
SV_PORT_RE = re.compile(
    r"^\s*output\s+(?:wire|logic)\s*(?:\[\s*(\d+)\s*:\s*(\d+)\s*\])?\s*(\w+)\s*,?\s*$"
)
CPP_STRUCT_RE = re.compile(r"struct\s+(\w+)\s*\{(.*?)\n\};", re.S)
CPP_MEMBER_RE = re.compile(
    r"^\s*(bool|char|unsigned|int|long|uint8_t|uint16_t|uint32_t|uint64_t)\s+"
    r"(\w+)\s*(?:=[^;]*)?;",
    re.M,
)
CPP_WIDTHS = {
    "bool": 1,
    "char": 8,
    "uint8_t": 8,
    "uint16_t": 16,
    "unsigned": 32,
    "int": 32,
    "uint32_t": 32,
    "long": 64,
    "uint64_t": 64,
}
CODEC_VERSION_RE = re.compile(
    r"^\s*#define\s+MOSAIC_EVENT_SCHEMA_VERSION\s+(\d+)\s*$", re.M
)
CODEC_FIELD_RE = re.compile(r"X\(\s*(\w+)\s*,\s*(\d+)\s*\)")
KIND_RE = re.compile(r"K\(\s*(\w+)\s*,\s*(\d+)\s*\)")


def read_text(path: str) -> str:
    with open(path) as handle:
        return handle.read()


def parse_retire_ports(text: str, namespace: Namespace) -> Dict[str, int]:
    """Per-lane widths of the retire unit's event ports."""
    for match in RETIRE_LOCALPARAM_RE.finditer(text):
        namespace.define(match.group(1), match.group(2).strip())
    widths: Dict[str, int] = {}
    for line in text.splitlines():
        match = RETIRE_PORT_RE.match(line)
        if not match:
            continue
        total = evaluate(match.group(1), namespace)
        lanes = namespace.resolve("RET_WIDTH")
        if lanes <= 0 or total % lanes != 0:
            raise ExprError(
                "port %s is %d bits, not a whole number of %d lanes"
                % (match.group(2), total, lanes)
            )
        widths[match.group(2)] = total // lanes
    return widths


def parse_sv_ports(text: str, prefix: str) -> Dict[str, int]:
    widths: Dict[str, int] = {}
    for line in text.splitlines():
        match = SV_PORT_RE.match(line)
        if not match or not match.group(3).startswith(prefix):
            continue
        high, low = match.group(1), match.group(2)
        widths[match.group(3)] = (int(high) - int(low) + 1) if high and low else 1
    return widths


def parse_cpp_struct(text: str, name: str) -> Dict[str, int]:
    match = CPP_STRUCT_RE.search(text)
    if not match or match.group(1) != name:
        raise ExprError("struct %s not found in the host record" % name)
    widths: Dict[str, int] = {}
    for member in CPP_MEMBER_RE.finditer(match.group(2)):
        widths[member.group(2)] = CPP_WIDTHS[member.group(1)]
    return widths


def parse_codec(text: str) -> Tuple[int, List[Tuple[str, int]], Dict[str, int]]:
    version_match = CODEC_VERSION_RE.search(text)
    if not version_match:
        raise ExprError("event_codec.h declares no MOSAIC_EVENT_SCHEMA_VERSION")
    block = re.search(r"#define\s+MOSAIC_EVENT_FIELDS\(X\)(.*?)\n\n", text, re.S)
    if not block:
        raise ExprError("event_codec.h declares no MOSAIC_EVENT_FIELDS(X) list")
    fields = [(m.group(1), int(m.group(2))) for m in CODEC_FIELD_RE.finditer(block.group(1))]
    kinds_block = re.search(r"#define\s+MOSAIC_EVENT_KINDS\(K\)(.*?)\n\n", text, re.S)
    if not kinds_block:
        raise ExprError("event_codec.h declares no MOSAIC_EVENT_KINDS(K) list")
    kinds = {m.group(1): int(m.group(2)) for m in KIND_RE.finditer(kinds_block.group(1))}
    return int(version_match.group(1)), fields, kinds


# ---------------------------------------------------------------------------
# geometry
# ---------------------------------------------------------------------------


def geometry_namespace(profile: str) -> Namespace:
    profile_path = os.path.join(REPO_ROOT, "config", "profiles", "%s.json" % profile)
    geometry_path = os.path.join(REPO_ROOT, "config", "geometry", "%s.json" % profile)
    with open(profile_path) as handle:
        profile_doc = json.load(handle)
    with open(geometry_path) as handle:
        geometry = json.load(handle)
    isa = profile_doc.get("isa_target", {})
    values = {
        "xlen": profile_doc["xlen"],
        "harts": profile_doc["harts"],
        "clusters": profile_doc["clusters"],
        "rob_entries": geometry["rob"]["entries"],
        "max_uops_per_macro": geometry["rob"]["max_uops_per_macro"],
        "int_prf_entries": geometry["int_prf"]["entries"],
        "rename_width": geometry["rename"]["dispatch_width"],
        "fetch_outstanding": geometry["frontend"]["fetch_outstanding"],
        "iq_entries_per_cluster": geometry["fabric"]["iq_entries_per_cluster"],
        "result_fifo_per_cluster": geometry["fabric"]["result_fifo_per_cluster"],
        "lq_entries": geometry["lsu"]["lq_entries"],
        "sq_entries": geometry["lsu"]["sq_entries"],
    }
    if isa.get("vlen"):
        values["vlen"] = isa["vlen"]
        values["elen"] = isa["elen"]
    return Namespace(values)


# ---------------------------------------------------------------------------
# the check
# ---------------------------------------------------------------------------


def load_schema(root: str) -> Dict[str, Any]:
    with open(os.path.join(root, SCHEMA_REL)) as handle:
        return json.load(handle)


def check(root: str, profile: str) -> List[Problem]:
    problems: List[Problem] = []

    def fail(where: str, message: str) -> None:
        problems.append(Problem(where, message, invariant))

    try:
        schema = load_schema(root)
    except (OSError, ValueError) as exc:
        return [Problem(SCHEMA_REL, "cannot read the frozen schema: %s" % exc)]

    namespace = geometry_namespace(profile)
    try:
        for name, expression in schema.get("rtl_constants", {}).items():
            namespace.values[name] = evaluate(expression, namespace)
    except ExprError as exc:
        fail("rtl_constants", str(exc))
        return problems

    kinds: Dict[str, int] = {
        name: entry["value"] for name, entry in schema.get("event_kinds", {}).items()
    }
    all_kinds = tuple(kinds)
    fields = schema.get("fields", [])

    # ------------------------------------------------------------ INV-COMPLETE
    invariant = "INV-COMPLETE"
    seen = set()
    for field in fields:
        name = field.get("name", "<unnamed>")
        where = "field %s" % name
        if name in seen:
            fail(where, "duplicate field name")
        seen.add(name)
        if not isinstance(field.get("width_bits"), int) or field["width_bits"] < 1:
            fail(where, "no positive width_bits")
        if not field.get("meaning"):
            fail(where, "no meaning recorded")
        if not field.get("valid_when") or len(field["valid_when"]) < 20:
            fail(where, "no validity rule (valid_when)")
        if field.get("sampled_at") not in schema.get("sampling_instants", {}):
            fail(
                where,
                "sampling instant %r is not one of %s"
                % (field.get("sampled_at"), sorted(schema.get("sampling_instants", {}))),
            )
        if field.get("group") not in GROUP_KINDS:
            fail(where, "group %r is not a declared group" % field.get("group"))
            continue
        expected_kinds = GROUP_KINDS[field["group"]]
        if tuple(field.get("valid_kinds", ())) != expected_kinds:
            fail(
                where,
                "group %s is valid in %s but the field claims %s"
                % (field["group"], list(expected_kinds), field.get("valid_kinds")),
            )
        if field.get("class") != GROUP_CLASS[field["group"]]:
            fail(
                where,
                "group %s is %s but the field claims %s"
                % (field["group"], GROUP_CLASS[field["group"]], field.get("class")),
            )
        try:
            resolved = evaluate(str(field.get("width_expr", "")), namespace)
        except ExprError as exc:
            fail(where, "width expression %r: %s" % (field.get("width_expr"), exc))
            continue
        if resolved != field.get("width_bits"):
            fail(
                where,
                "width_expr %r resolves to %d for profile %s but width_bits is %r"
                % (field.get("width_expr"), resolved, profile, field.get("width_bits")),
            )

    # ------------------------------------------------------------ INV-ENCODING
    invariant = "INV-ENCODING"
    encoding = schema.get("encoding", {})
    offset = int(encoding.get("header", {}).get("bits", 0)) // 8
    if offset <= 0:
        fail("encoding.header", "no positive header width")
    for field in fields:
        name = field.get("name", "?")
        octets = (int(field.get("width_bits", 0)) + 7) // 8
        if field.get("encoding_octets") != octets:
            fail("field %s" % name,
                 "encoding_octets is %r but %r bits need %d octets"
                 % (field.get("encoding_octets"), field.get("width_bits"), octets))
        if field.get("encoding_offset_bytes") != offset:
            fail("field %s" % name,
                 "encoding_offset_bytes is %r but the layout puts it at %d"
                 % (field.get("encoding_offset_bytes"), offset))
        offset += octets
    if encoding.get("total_octets") != offset:
        fail("encoding", "total_octets is %r but the field layout is %d octets wide"
             % (encoding.get("total_octets"), offset))

    # ------------------------------------------------------------- INV-VERSION
    invariant = "INV-VERSION"
    codec_text = read_text(os.path.join(root, CODEC_REL))
    try:
        codec_version, codec_fields, codec_kinds = parse_codec(codec_text)
    except ExprError as exc:
        fail(CODEC_REL, str(exc))
        codec_version, codec_fields, codec_kinds = -1, [], {}
    if codec_version != schema.get("schema_version"):
        fail("schema_version",
             "the schema says %r and %s is compiled against %d; the encoder's "
             "version check would compare two different things"
             % (schema.get("schema_version"), CODEC_REL, codec_version))
    if codec_kinds != kinds:
        fail("event_kinds",
             "%s declares %s but the schema declares %s"
             % (CODEC_REL, codec_kinds, kinds))

    # ----------------------------------------------------------- INV-CPPFIELDS
    invariant = "INV-CPPFIELDS"
    schema_names = [f.get("name") for f in fields]
    cpp_names = [name for name, _ in codec_fields]
    if cpp_names != schema_names:
        only_schema = [n for n in schema_names if n not in cpp_names]
        only_cpp = [n for n in cpp_names if n not in schema_names]
        if only_schema or only_cpp:
            for name in only_schema:
                fail("field %s" % name, "in the schema but not in %s" % CODEC_REL)
            for name in only_cpp:
                fail("field %s" % name, "in %s but not in the schema" % CODEC_REL)
        else:
            fail("field order",
                 "%s lists %s but the schema orders them %s"
                 % (CODEC_REL, cpp_names, schema_names))
    else:
        for field, (_, bits) in zip(fields, codec_fields):
            if field["width_bits"] != bits:
                fail("field %s" % field["name"],
                     "%s encodes it %d bits wide, the schema says %d"
                     % (CODEC_REL, bits, field["width_bits"]))

    # ------------------------------------------------------------- INV-SOURCE
    invariant = "INV-SOURCE"
    schema_fields = {f["name"]: f for f in fields if "name" in f}
    declared_per_file: Dict[str, Dict[str, int]] = {}
    pending: List[str] = []
    for field in fields:
        name = field.get("name", "?")
        sources = field.get("sources", [])
        if not sources:
            fail("field %s" % name, "declares no source at all")
            continue
        for source in sources:
            kind = source.get("kind")
            if kind in ("rtl_lane", "tap", "rtl_derived"):
                relative = source.get("file")
                if not relative or not os.path.exists(os.path.join(root, relative)):
                    fail("field %s" % name, "source file %r does not exist" % relative)
                    continue
                decls = []
                if source.get("decl"):
                    decls.append(source["decl"])
                decls.extend(source.get("from", []))
                if not decls:
                    fail("field %s" % name, "source %s names no declaration" % kind)
                for decl in decls:
                    declared_per_file.setdefault(relative, {})[decl] = name
            elif kind == "pending_rtl":
                if not source.get("why") or len(source["why"]) < 20:
                    fail("field %s" % name, "pending source without a reason")
                pending.append("%s -> %s (%s)" % (name, source.get("decl"), relative))
            elif kind == "harness_binding":
                if not source.get("why") or len(source["why"]) < 20:
                    fail("field %s" % name, "harness binding without a reason")
            else:
                fail("field %s" % name, "unknown source kind %r" % kind)

    for entry in schema.get("reconciliation", []):
        for name in [n.strip() for n in str(entry.get("field", "")).split(",")]:
            if name and name not in schema_fields:
                fail("reconciliation %s" % entry.get("id"),
                     "names field %r which is not in the field list" % name)

    # ------------------------------------------------------------- INV-TAPDECL
    invariant = "INV-TAPDECL"
    producer_decls: Dict[str, Dict[str, int]] = {}
    for producer in schema.get("producers", []):
        relative = producer["file"]
        selector = producer.get("declaration_selector", {})
        selector_kind = selector.get("kind")
        try:
            if selector_kind == "sv_output_port_prefix":
                actual = parse_sv_ports(read_text(os.path.join(root, relative)),
                                        selector["prefix"])
            elif selector_kind == "sv_lane_port_prefix":
                # A per-lane port is declared as one flat vector of
                # retire_width * field_width bits whose width is an expression
                # over the file's own localparams, so it is evaluated rather
                # than read as a literal.
                actual = parse_retire_ports(read_text(os.path.join(root, relative)),
                                            namespace)
            elif selector_kind == "cpp_struct_members":
                actual = parse_cpp_struct(read_text(os.path.join(root, relative)),
                                          selector["struct"])
            else:
                fail("producer %s" % producer.get("id"),
                     "unknown declaration_selector %r" % selector_kind)
                continue
        except (OSError, ExprError) as exc:
            fail("producer %s" % producer.get("id"), str(exc))
            continue
        producer_decls[relative] = actual
        declared = declared_per_file.get(relative, {})
        for decl in sorted(set(actual) - set(declared)):
            fail("producer %s" % producer.get("id"),
                 "%s declares the event declaration %r which the schema does not "
                 "map to any field" % (relative, decl))
        for decl in sorted(set(declared) - set(actual)):
            fail("field %s" % declared[decl],
                 "the schema says %s declares %r but it does not"
                 % (relative, decl))

    # ------------------------------------------------------------ INV-RTLWIDTH
    invariant = "INV-RTLWIDTH"
    for field in fields:
        name = field.get("name", "?")
        for source in field.get("sources", []):
            decl = source.get("decl")
            if not decl:
                continue
            relative = source.get("file")
            actual = producer_decls.get(relative, {}).get(decl)
            if actual is None:
                continue  # already reported by INV-TAPDECL / INV-SOURCE
            if source.get("kind") == "rtl_lane":
                if actual != field["width_bits"]:
                    fail("field %s" % name,
                         "%s:%s is %d bits per lane but the schema says %d"
                         % (relative, decl, actual, field["width_bits"]))
            elif source.get("kind") == "tap":
                if source.get("declared_bits") != actual:
                    fail("field %s" % name,
                         "the schema records %s:%s as %r bits but it is %d"
                         % (relative, decl, source.get("declared_bits"), actual))
                if actual < field["width_bits"]:
                    fail("field %s" % name,
                         "%s:%s is %d bits, narrower than the %d-bit field: a "
                         "producer that cannot carry the field is a truncation"
                         % (relative, decl, actual, field["width_bits"]))
                if actual > field["width_bits"] and not source.get("why"):
                    fail("field %s" % name,
                         "%s:%s is wider than the field and no container reason "
                         "is recorded" % (relative, decl))

    # ----------------------------------------------------------- INV-IDENTITY
    invariant = "INV-IDENTITY"
    roles: Dict[str, List[str]] = {}
    for field in fields:
        roles.setdefault(field.get("identity_role", "none"), []).append(field["name"])
    for role, names in roles.items():
        if role != "none" and len(names) > 1:
            fail("identity", "%s claim the %r identity; a field may not claim an "
                             "identity another field already carries" % (names, role))

    # ------------------------------------------------------------- INV-LOCATE
    # The clause the card makes non-negotiable: every architectural effect
    # locates to exactly one hart and one instruction. The two locating fields
    # carry the identities, are architectural, and are valid on every kind -- so
    # a store's visibility and a trap are attributable through the same two
    # fields as a retirement is, with no debug-only side channel.
    invariant = "INV-LOCATE"
    locating = schema.get("locating", {})
    for key, role in (("hart_field", "hart"), ("order_field", "retire_order")):
        field = schema_fields.get(locating.get(key, ""))
        if field is None:
            fail("locating.%s" % key, "names %r, which is not a field in the list"
                 % locating.get(key))
            continue
        if field.get("identity_role") != role:
            fail("field %s" % field["name"],
                 "is named by locating.%s but claims the %r identity, not %r"
                 % (key, field.get("identity_role"), role))
        if field.get("class") != "architectural":
            fail("field %s" % field["name"],
                 "is a locating field and must be architectural, not %s"
                 % field.get("class"))
        if tuple(field.get("valid_kinds", ())) != all_kinds:
            fail("field %s" % field["name"],
                 "is a locating field and must be valid on every kind, not %s"
                 % field.get("valid_kinds"))

    # -------------------------------------------------------------- INV-KINDS
    invariant = "INV-KINDS"
    kind_field = None
    for field in fields:
        if field.get("group") == "kind":
            kind_field = field
    if kind_field is None:
        fail("fields", "no field carries the event kind, so the record cannot say "
                       "whether an instruction retired or trapped")
    else:
        width = kind_field["width_bits"]
        if width < 64 and max(kinds.values()) >= (1 << width):
            fail("field %s" % kind_field["name"],
                 "is %d bits wide and cannot hold kind value %d"
                 % (width, max(kinds.values())))
        if set(kinds.values()) != {1, 2, 3} or len(kinds) < 2:
            fail("event_kinds", "the kind table %s does not carry retirements and "
                                "traps as distinct values" % kinds)
    for group, expected in GROUP_KINDS.items():
        if not any(f.get("group") == group for f in fields):
            fail("fields", "no field of group %s is listed" % group)

    # ------------------------------------------------------------- INV-VECTOR
    invariant = "INV-VECTOR"
    for field in fields:
        name = field["name"]
        if re.match(r"^(vec|vbody|vreg)", name):
            if "vlen" not in str(field.get("width_expr", "")):
                fail("field %s" % name,
                     "a vector body must take its width from vlen, not the literal "
                     "%r: a fixed window truncates the body" % field.get("width_expr"))
    for entry in schema.get("deferred", []):
        if not entry.get("why") or not entry.get("must_not"):
            fail("deferred %s" % entry.get("id"), "without a reason or a prohibition")
        if entry.get("enforced_by") not in INVARIANTS:
            fail("deferred %s" % entry.get("id"),
                 "is enforced by %r, which is not an implemented invariant"
                 % entry.get("enforced_by"))
        if any(f["name"] == entry.get("id") for f in fields):
            fail("deferred %s" % entry.get("id"), "is also an active field")

    # -------------------------------------------------------------- INV-RULES
    invariant = "INV-RULES"
    rules = schema.get("rules", [])
    if not rules:
        fail("rules", "the schema records no rule, so the failure modes it was "
                      "written to design out are not written down")
    used = set()
    for rule in rules:
        identifier = rule.get("id", "<unnamed>")
        if not rule.get("text") or len(rule["text"]) < 20:
            fail("rule %s" % identifier, "has no text")
        enforced_by = rule.get("enforced_by")
        if enforced_by not in INVARIANTS:
            fail("rule %s" % identifier,
                 "names %r as its check, which this tool does not implement"
                 % enforced_by)
        else:
            used.add(enforced_by)
    for invariant in INVARIANTS:
        if invariant not in used:
            fail("rules", "%s is implemented but no rule names it, so the check "
                          "has no stated reason" % invariant)

    return problems


# ---------------------------------------------------------------------------
# negative controls: mutate one input, require the mutation to be rejected
# ---------------------------------------------------------------------------


class NegativeControls(object):
    def __init__(self, profile: str) -> None:
        self.profile = profile
        self.failures: List[str] = []
        self.total = 0

    def case(self, name: str, mutator) -> None:
        self.total += 1
        workdir = tempfile.mkdtemp(prefix="mosaic-event-negative-")
        try:
            for relative in INTERFACE_FILES:
                target = os.path.join(workdir, relative)
                os.makedirs(os.path.dirname(target), exist_ok=True)
                shutil.copyfile(os.path.join(REPO_ROOT, relative), target)
            try:
                mutator(workdir)
            except AssertionError as exc:
                # A mutation whose target has vanished is a broken control, not a
                # passing one: it would otherwise silently stop testing anything.
                self.failures.append("%s (mutation does not apply: %s)" % (name, exc))
                print("  BROKEN CONTROL: %s (%s)" % (name, exc), file=sys.stderr)
                return
            problems = check(workdir, self.profile)
            if not problems:
                self.failures.append(name)
                print("  NOT REJECTED : %s" % name, file=sys.stderr)
            else:
                first = problems[0]
                print("  rejected: %-44s %s" % (name, first))
        finally:
            shutil.rmtree(workdir, ignore_errors=True)

    # -- the mutators -------------------------------------------------------

    def _edit(self, workdir: str, relative: str, old: str, new: str) -> None:
        path = os.path.join(workdir, relative)
        text = read_text(path)
        if old not in text:
            raise AssertionError("mutation target %r not present in %s" % (old, relative))
        with open(path, "w") as handle:
            handle.write(text.replace(old, new, 1))

    def _edit_schema(self, workdir: str, mutator) -> None:
        path = os.path.join(workdir, SCHEMA_REL)
        with open(path) as handle:
            document = json.load(handle)
        mutator(document)
        with open(path, "w") as handle:
            json.dump(document, handle, indent=2)

    def _field(self, document: Dict[str, Any], name: str) -> Dict[str, Any]:
        for field in document["fields"]:
            if field["name"] == name:
                return field
        raise AssertionError("no field %r" % name)

    def run(self) -> int:
        baseline = check(REPO_ROOT, self.profile)
        if baseline:
            print("negative controls skipped: the interface does not check out "
                  "(%d problem(s)); fix it before using these controls as evidence"
                  % len(baseline), file=sys.stderr)
            return 1

        def swap_codec_fields(workdir: str) -> None:
            # Swap the first two field entries' names, keeping the macro's line
            # layout, so the *order* is what changed and nothing else.
            path = os.path.join(workdir, CODEC_REL)
            text = read_text(path)
            pattern = re.compile(r"(?P<a>X\(retire_seq,\s*8\))(?P<mid>.*?)"
                                 r"(?P<b>X\(hart_id,\s*1\))", re.S)
            replaced, count = pattern.subn(
                lambda m: m.group("b") + m.group("mid") + m.group("a"), text, count=1)
            if count != 1:
                raise AssertionError("could not swap the codec's first two fields")
            with open(path, "w") as handle:
                handle.write(replaced)

        def rename_field_everywhere(workdir: str, old: str, new: str) -> None:
            self._edit(workdir, CODEC_REL, "X(%s," % old, "X(%s," % new)
            self._edit_schema(
                workdir, lambda d: self._field(d, old).update({"name": new}))

        # -- the RTL producers -------------------------------------------------
        self.case("retire ev_store_size narrowed to 2 bits",
                  lambda w: self._edit(w, RETIRE_REL, "RET_SIZE_W = 3;", "RET_SIZE_W = 2;"))
        self.case("retire ev_store_data renamed",
                  lambda w: self._edit(w, RETIRE_REL, "ev_store_data,", "ev_stdata,"))
        self.case("retire ev_seq widened past the schema",
                  lambda w: self._edit(w, RETIRE_REL, "$clog2(2 * RET_ROB + 1);",
                                       "$clog2(2 * RET_ROB + 1) + 4;"))
        # -- the bring-up tap --------------------------------------------------
        self.case("bringup c_evt_pc narrowed to 32 bits",
                  lambda w: self._edit(w, TAP_SV_REL, "output wire  [63:0] c_evt_pc,",
                                       "output wire  [31:0] c_evt_pc,"))
        self.case("bringup c_evt_next_pc renamed",
                  lambda w: self._edit(w, TAP_SV_REL, "output wire  [63:0] c_evt_next_pc,",
                                       "output wire  [63:0] c_evt_npc,"))
        # -- the host record ---------------------------------------------------
        self.case("host record store_size removed",
                  lambda w: self._edit(w, TAP_H_REL, "  unsigned store_size = 0;", ""))
        self.case("host record rd widened to 16 bits",
                  lambda w: self._edit(w, TAP_H_REL, "  uint8_t rd = 0;",
                                       "  uint16_t rd = 0;"))
        # -- the serialiser ----------------------------------------------------
        self.case("codec pc_before encoded 32 bits wide",
                  lambda w: self._edit(w, CODEC_REL, "X(pc_before, 64)", "X(pc_before, 32)"))
        self.case("codec trap_epc dropped from the field list",
                  lambda w: self._edit(w, CODEC_REL, "  X(trap_epc, 64)", ""))
        self.case("codec field order changed", swap_codec_fields)
        self.case("codec field renamed without the schema",
                  lambda w: self._edit(w, CODEC_REL, "X(rd_value,", "X(rdval,"))
        self.case("codec schema version bumped alone",
                  lambda w: self._edit(w, CODEC_REL,
                                       "MOSAIC_EVENT_SCHEMA_VERSION 1",
                                       "MOSAIC_EVENT_SCHEMA_VERSION 2"))
        self.case("codec MEM_VISIBLE value changed",
                  lambda w: self._edit(w, CODEC_REL, "K(MEM_VISIBLE, 3)", "K(MEM_VISIBLE, 9)"))
        # -- the schema's own completeness -------------------------------------
        self.case("field without a validity rule",
                  lambda w: self._edit_schema(
                      w, lambda d: self._field(d, "rd_value").pop("valid_when")))
        self.case("field without a sampling instant",
                  lambda w: self._edit_schema(
                      w, lambda d: self._field(d, "pc_after").pop("sampled_at")))
        self.case("field with no group at all",
                  lambda w: self._edit_schema(
                      w, lambda d: self._field(d, "insn_bits").pop("group")))
        self.case("width expression that does not resolve to the width",
                  lambda w: self._edit_schema(
                      w, lambda d: self._field(d, "csr_addr").update({"width_expr": "1"})))
        # -- sources -----------------------------------------------------------
        self.case("field without any source",
                  lambda w: self._edit_schema(
                      w, lambda d: self._field(d, "csr_value").update({"sources": []})))
        self.case("pending source with no reason recorded",
                  lambda w: self._edit_schema(
                      w, lambda d: self._field(d, "insn_bits")["sources"][-1].pop("why")))
        # -- identity and the locating clause ----------------------------------
        self.case("second field claiming the retirement order",
                  lambda w: self._edit_schema(
                      w, lambda d: self._field(d, "cycle").update(
                          {"identity_role": "retire_order"})))
        self.case("locating order field is a debug-only identity",
                  lambda w: self._edit_schema(
                      w, lambda d: d["locating"].update({"order_field": "rob_id"})))
        self.case("locating hart field named but absent",
                  lambda w: self._edit_schema(
                      w, lambda d: d["locating"].update({"hart_field": "hart_number"})))
        # -- kinds -------------------------------------------------------------
        self.case("trap payload marked valid on a retirement",
                  lambda w: self._edit_schema(
                      w, lambda d: self._field(d, "trap_cause").update(
                          {"valid_kinds": ["RETIRE", "TRAP"]})))
        self.case("store visibility without its identity",
                  lambda w: self._edit_schema(
                      w, lambda d: self._field(d, "retire_seq").update(
                          {"valid_kinds": ["RETIRE", "TRAP"]})))
        self.case("architectural next PC marked observed",
                  lambda w: self._edit_schema(
                      w, lambda d: self._field(d, "pc_after").update({"class": "observed"})))
        self.case("event kind field too narrow for the kinds it must carry",
                  lambda w: [self._edit(w, CODEC_REL, "X(kind, 2)", "X(kind, 1)"),
                             self._edit_schema(
                                 w, lambda d: self._field(d, "kind").update(
                                     {"width_bits": 1, "width_expr": "1"}))])
        # -- the vector rule ---------------------------------------------------
        self.case("active field named as a vector body in a fixed window",
                  lambda w: rename_field_everywhere(w, "rd", "vbody"))
        self.case("deferred vector entry names an active field",
                  lambda w: self._edit_schema(
                      w, lambda d: d["deferred"][0].update({"id": "insn_bits"})))
        # -- the encoding ------------------------------------------------------
        self.case("encoding offset drifted by one byte",
                  lambda w: self._edit_schema(
                      w, lambda d: self._field(d, "mem_data").update(
                          {"encoding_offset_bytes": 68})))
        self.case("record size disagrees with the field layout",
                  lambda w: self._edit_schema(
                      w, lambda d: d["encoding"].update({"total_octets": 104})))
        # -- rules -------------------------------------------------------------
        self.case("rule with no implemented check",
                  lambda w: self._edit_schema(
                      w, lambda d: d["rules"][0].update({"enforced_by": "INV-VIBES"})))
        self.case("rule deleted from the schema",
                  lambda w: self._edit_schema(w, lambda d: d["rules"].pop()))
        self.case("reconciliation naming a field that does not exist",
                  lambda w: self._edit_schema(
                      w, lambda d: d["reconciliation"][0].update({"field": "pc_future"})))

        if self.failures:
            print("negative controls: %d of %d were wrongly accepted: %s"
                  % (len(self.failures), self.total, ", ".join(self.failures)),
                  file=sys.stderr)
            return 1
        print("negative controls: %d/%d illegal interfaces rejected"
              % (self.total, self.total))
        return 0


# ---------------------------------------------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", default="p0")
    parser.add_argument("--negative", action="store_true")
    parser.add_argument("--root", default=REPO_ROOT,
                        help="root to read the interface files from (tests only)")
    args = parser.parse_args()

    problems = check(args.root, args.profile)
    if problems:
        for problem in problems:
            print("ERROR %s" % problem, file=sys.stderr)
        print("profile %s: %d event-contract problem(s)" % (args.profile, len(problems)),
              file=sys.stderr)
        return 1

    schema = load_schema(args.root)
    fields = schema["fields"]
    pending: List[str] = []
    for field in fields:
        for source in field.get("sources", []):
            if source.get("kind") == "pending_rtl":
                pending.append("%s (%s)" % (source["decl"], field["name"]))
    print("profile %s: event contract OK -- %d fields, %d octets/record, kinds %s"
          % (args.profile, len(fields), schema["encoding"]["total_octets"],
             "/".join(schema["event_kinds"])))
    if pending:
        # Printed on every run, not implied: these fields have a canonical
        # producer that does not exist yet, and the freeze says so out loud.
        print("profile %s: fields with no RTL producer yet: %s (see rules F-3/F-4)"
              % (args.profile, ", ".join(sorted(pending))))

    status = 0
    if args.negative:
        status = NegativeControls(args.profile).run()
    return status


if __name__ == "__main__":
    sys.exit(main())
