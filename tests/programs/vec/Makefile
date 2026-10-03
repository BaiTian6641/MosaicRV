# tests/programs/vec/Makefile
#
# Self-checking RVV programs (see README.md).  These are NOT the p01..p13
# scalar corpus: they are not in tests/programs/corpus.json, they are not built
# by `make -C tests/programs all`, and they do not affect tests/programs/
# golden.json or tools/host_oracle.py.
#
#   make -C tests/programs/vec all     build every ELF + print the entry points
#   make -C tests/programs/vec dis     disassemble every program
#   make -C tests/programs/vec clean
#
# The toolchain, the object model and the linker script style are the same ones
# the corpus uses; the ISA string carries the `v` extension because these
# programs execute vector instructions, and they link their own crt0.S instead
# of the corpus's (see crt0.S for why).

PYTHON  ?= python3
CROSS   ?= riscv64-elf-
CC      := $(CROSS)gcc
OBJDUMP := $(CROSS)objdump
READELF := $(CROSS)readelf

# The vector extension is required by the program text itself; the integer/core
# profile the DUT advertises is unchanged (rv64im_zicsr_zifencei plus the
# delivered vector families).
ARCHFLAGS := -march=rv64imv_zicsr_zifencei -mabi=lp64 -mcmodel=medany
INCLUDES  := -I . -I ../src

CFLAGS := $(ARCHFLAGS) -nostdlib -nostartfiles -ffreestanding -fno-builtin \
          -fno-pic -mno-relax -fno-asynchronous-unwind-tables \
          -fno-stack-protector -fno-common $(INCLUDES) -Wall -Wa,--fatal-warnings

LDSCRIPT := vec_selfcheck.ld
LDFLAGS  := $(ARCHFLAGS) -nostdlib -nostartfiles \
            -Wl,--no-relax -Wl,--build-id=none \
            -Wl,-T,$(LDSCRIPT) -Wl,--fatal-warnings -Wl,--no-warn-rwx-segments

SRCDIR   := src
BUILDDIR := build

# The seven programs the registered case runs: every one must reach its PASS trap.
PROGRAMS := v01_addsub_e64 v02_addsub_e32 v03_logic_e16 \
            v04_masklog_e8 v05_maskpfx_e8 v06_loadstore v08_masked_addsub_e32

# The negative control: a copy of v01 with one expected constant deliberately
# wrong.  It is built here but is NOT in PROGRAMS, so the registered run never
# sees it; tools/run_vector_selfcheck_controls.py runs it on its own and
# requires it to FAIL with a named first failure.
CONTROL  := v07_control_wrong_addsub

ALL_PROGRAMS := $(PROGRAMS) $(CONTROL)
BOOT     := $(BUILDDIR)/crt0.o

ELFS     := $(addprefix $(BUILDDIR)/,$(addsuffix .elf,$(ALL_PROGRAMS)))

.PHONY: all map dis clean
.DELETE_ON_ERROR:

all: $(ELFS) map

$(BUILDDIR)/%.o: $(SRCDIR)/%.S vec.h crt0.S $(LDSCRIPT)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BOOT): crt0.S vec.h $(LDSCRIPT)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/%.elf: $(BUILDDIR)/%.o $(BOOT) $(LDSCRIPT)
	$(CC) $(LDFLAGS) -o $@ $(BUILDDIR)/$*.o $(BOOT)

map: $(ELFS)
	@echo "=== MosaicRV self-checking RVV programs ==="
	@for elf in $(ELFS); do \
	    printf '%-40s ' "$$elf"; \
	    $(READELF) -h $$elf | grep -o 'Entry point address:.*'; \
	done

dis: $(ELFS)
	@for elf in $(ELFS); do \
	    echo "--- $$elf ---"; \
	    $(OBJDUMP) -d --no-show-raw-insn $$elf | sed -n '/<_start>:/,/^$$/p'; \
	done

clean:
	rm -rf $(BUILDDIR)
