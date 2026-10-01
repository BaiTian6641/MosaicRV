#!/usr/bin/env python3
"""Guest-side NEMU ABI probe (runs inside the x86_64 Lima guest).

Usage:
    python3 nemu_guest_probe.py <ref-so> <offsets.json> <out.json>

Does the things the V-004 card demands of the *actual* reference library:
writes an independent architectural state into NEMU, reads it back,
steps the reference over two arithmetic instructions plus one store,
and checks the architectural results. Negative controls
(missing symbol, wrong layout size) are exercised by the host wrapper
in separate processes so a deliberate abort cannot mask a positive result.

Stdlib only (ctypes, json, subprocess, sys).
"""
import ctypes
import json
import sys

DIFFTEST_TO_DUT = 0
DIFFTEST_TO_REF = 1

REQUIRED_SYMBOLS = [
    "difftest_init",
    "difftest_init_v2",
    "difftest_memcpy",
    "difftest_memcpy_init",
    "difftest_regcpy",
    "difftest_exec",
    "difftest_raise_intr",
    "difftest_csrcpy",
    "difftest_store_commit",
]

MBASE = 0x80000000


def fail(out, msg):
    out["verdict"] = "FAIL"
    out["error"] = msg
    print(json.dumps(out, indent=2, sort_keys=True))
    return 1


def main():
    out = {"probe": "nemu_guest_probe", "checks": {}}
    if len(sys.argv) != 4:
        return fail(out, "usage: nemu_guest_probe.py <ref-so> <offsets.json> <out.json>")
    so_path, offsets_path, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
    with open(offsets_path) as handle:
        spec = json.load(handle)
    out["ref_so"] = so_path
    out["layout_spec"] = spec.get("name", "?")

    try:
        so = ctypes.CDLL(so_path)
    except OSError as exc:
        return fail(out, "dlopen refused: %s" % exc)
    out["checks"]["dlopen"] = "ok"

    missing = [name for name in REQUIRED_SYMBOLS if not hasattr(so, name)]
    if missing:
        return fail(out, "missing difftest symbols (fail-closed): %s" % ", ".join(missing))
    out["checks"]["symbols_present"] = "ok:%d" % len(REQUIRED_SYMBOLS)

    # Arity check: regcpy must be the 2-argument (dut, direction) form.
    # A caller compiled against the 3-argument LightQS form would corrupt
    # the stack here; ctypes validates argcount before the call.
    so.difftest_regcpy.restype = None
    so.difftest_regcpy.argtypes = [ctypes.c_void_p, ctypes.c_bool]
    so.difftest_memcpy.restype = None
    so.difftest_memcpy.argtypes = [ctypes.c_uint64, ctypes.c_void_p,
                                   ctypes.c_size_t, ctypes.c_bool]
    so.difftest_exec.restype = None
    so.difftest_exec.argtypes = [ctypes.c_uint64]
    so.difftest_init.restype = None
    so.difftest_init.argtypes = []
    out["checks"]["prototypes"] = "ok:regcpy(void*,bool),memcpy(u64,void*,size,bool),exec(u64)"

    try:
        reg_size = ctypes.c_uint.in_dll(so, "DIFFTEST_REG_SIZE").value
    except ValueError:
        return fail(out, "DIFFTEST_REG_SIZE not exported")
    out["ref_reg_size"] = reg_size
    if reg_size != spec["reg_size"]:
        return fail(out, "layout size mismatch: reference exports %d, layout derives %d"
                    % (reg_size, spec["reg_size"]))
    out["checks"]["reg_size"] = "ok:%d" % reg_size

    off = spec["offsets"]

    def put(buf, field, value):
        buf[off[field]:off[field] + 8] = int(value).to_bytes(8, "little")

    def get(buf, field):
        return int.from_bytes(buf[off[field]:off[field] + 8], "little")

    so.difftest_init()

    # -- state round-trip: independent values in, same values back ------------
    state = bytearray(reg_size)
    put(state, "mode", 3)  # MODE_M: PMP default-deny faults M-mode fetches at mode 0
    put(state, "gpr1", 0x0123456789ABCDEF)
    put(state, "gpr2", 0xFEDCBA9876543210)
    put(state, "gpr5", 0x80001000)
    put(state, "pc", MBASE)
    put(state, "mtvec", 0x80000100)
    back = bytearray(reg_size)
    so.difftest_regcpy((ctypes.c_char * reg_size).from_buffer(back), DIFFTEST_TO_DUT)
    for field, want in (("gpr1", 0x0123456789ABCDEF), ("gpr2", 0xFEDCBA9876543210),
                        ("gpr5", 0x80001000), ("pc", MBASE), ("mtvec", 0x80000100)):
        if get(back, field) != want:
            return fail(out, "round-trip corrupted %s: wrote %#x read %#x"
                        % (field, want, get(back, field)))
    if get(back, "gpr0") != 0:
        return fail(out, "x0 is not zero after round-trip")
    out["checks"]["state_roundtrip"] = "ok:gpr/pc/mtvec"
    so.difftest_regcpy((ctypes.c_char * reg_size).from_buffer(state), DIFFTEST_TO_REF)
    # CONFIG_SHARE builds assert n<=1 in cpu_exec: one architectural step per
    # call, which is also the granularity the card wants verified.
    for _ in range(3):
        so.difftest_exec(1)
    # ADD x3,x1,x2 = 0x002081b3 ; ADDI x4,x0,42 = 0x02a00213 ;
    # SD x3,0(x5) with x5 = 0x80001000 = 0x0032b023.
    code = bytes([0xB3, 0x81, 0x20, 0x00, 0x13, 0x02, 0xA0, 0x02,
                  0x23, 0xB0, 0x32, 0x00])
    cbuf = (ctypes.c_char * len(code)).from_buffer_copy(code)
    so.difftest_memcpy(MBASE, cbuf, len(code), DIFFTEST_TO_REF)
    state = bytearray(reg_size)
    put(state, "mode", 3)  # MODE_M (see above)
    put(state, "gpr1", 5)
    put(state, "gpr2", 7)
    put(state, "gpr5", 0x80001000)
    put(state, "pc", MBASE)
    so.difftest_regcpy((ctypes.c_char * reg_size).from_buffer(state), DIFFTEST_TO_REF)
    for _ in range(3):
        so.difftest_exec(1)
    got = bytearray(reg_size)
    so.difftest_regcpy((ctypes.c_char * reg_size).from_buffer(got), DIFFTEST_TO_DUT)
    if get(got, "gpr3") != 12:
        return fail(out, "reference ADD produced x3=%#x, want 12" % get(got, "gpr3"))
    if get(got, "gpr4") != 42:
        return fail(out, "reference ADDI produced x4=%#x, want 42" % get(got, "gpr4"))
    if get(got, "pc") != MBASE + 12:
        return fail(out, "reference pc=%#x after 3 steps, want %#x"
                    % (get(got, "pc"), MBASE + 12))
    out["checks"]["reference_step"] = "ok:ADD=12,ADDI=42,pc+=12"

    mem = bytearray(8)
    so.difftest_memcpy(0x80001000, (ctypes.c_char * 8).from_buffer(mem), 8,
                       DIFFTEST_TO_DUT)
    stored = int.from_bytes(mem, "little")
    if stored != 12:
        return fail(out, "reference store left %#x at 0x80001000, want 12" % stored)
    out["checks"]["store_direction"] = "ok:SD visible at 0x80001000"

    out["verdict"] = "PASS"
    with open(out_path, "w") as handle:
        json.dump(out, handle, indent=2, sort_keys=True)
    print(json.dumps(out, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
