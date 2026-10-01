import ctypes
so=ctypes.CDLL('mosaic-ref/NEMU-f39e/build/riscv64-nemu-interpreter-so')
so.difftest_regcpy.restype=None
so.difftest_regcpy.argtypes=[ctypes.c_void_p,ctypes.c_bool,ctypes.c_bool]
try:
    so.difftest_regcpy(None,True,True)
except ctypes.ArgumentError:
    print('arity-rejected')
else:
    raise SystemExit('3-arg regcpy call was accepted')
