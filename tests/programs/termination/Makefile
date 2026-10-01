# tests/programs/termination/Makefile
#
# V-012 exit-protocol programs (see exit.h).  These are NOT the p01..p13
# firmware corpus: they are not in corpus.json, they are not built by
# `make -C tests/programs all`, and they do not affect
# tests/programs/golden.json or tools/host_oracle.py.
#
#   make -C tests/programs termination        build all seven ELFs + print the map
#   make -C tests/programs termination dis    disassemble every program
#   make -C tests/programs termination audit  ISA audit of the six programs that do
#                                      not deliberately execute WFI
#   make -C tests/programs termination clean
#
# The toolchain, the p0 ISA string and the linker script are the same frozen
# ones the corpus uses; the only difference is that these programs are their
# own reset path, so crt0.o and trap.o are not linked.

PYTHON  ?= python3
CROSS   ?= riscv64-elf-
CC      := $(CROSS)gcc
OBJDUMP := $(CROSS)objdump
READELF := $(CROSS)readelf

ARCHFLAGS := -march=rv64im_zicsr_zifencei -mabi=lp64 -mcmodel=medany
INCLUDES  := -I . -I ../src

CFLAGS := $(ARCHFLAGS) -nostdlib -nostartfiles -ffreestanding -fno-builtin \
          -fno-pic -mno-relax -fno-asynchronous-unwind-tables \
          -fno-stack-protector -fno-common $(INCLUDES) -Wall -Wa,--fatal-warnings

LDSCRIPT := ../linker/mosaic_p0.ld
LDFLAGS  := $(ARCHFLAGS) -nostdlib -nostartfiles \
            -Wl,--no-relax -Wl,--build-id=none \
            -Wl,-T,$(LDSCRIPT) -Wl,--fatal-warnings -Wl,--no-warn-rwx-segments

SRCDIR   := src
BUILDDIR := ../build/termination

PROGRAMS := x01_normal x02_fail x03_spin x04_wfi x05_signature_mismatch \
            x06_signature_overrun x07_pending_store x08_runaway

# The six programs that stay inside the p0 ISA string's audited set.  x04_wfi
# deliberately executes WFI, which tests/programs/audit/check_p0_isa.py's
# M-mode allow-list (currently {"mret"}) does not contain; the audit target
# below names that exclusion rather than hiding it.
AUDITABLE := x01_normal x02_fail x03_spin x05_signature_mismatch \
             x06_signature_overrun x07_pending_store x08_runaway

ELFS := $(addprefix $(BUILDDIR)/,$(addsuffix .elf,$(PROGRAMS)))
AUDIT_ELFS := $(addprefix $(BUILDDIR)/,$(addsuffix .elf,$(AUDITABLE)))

.PHONY: all map dis audit clean
.DELETE_ON_ERROR:

all: $(ELFS) map

$(BUILDDIR)/%.o: $(SRCDIR)/%.S exit.h ../src/platform.h $(LDSCRIPT)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/%.elf: $(BUILDDIR)/%.o $(LDSCRIPT)
	$(CC) $(LDFLAGS) -o $@ $<

map: $(ELFS)
	@echo "=== MosaicRV V-012 exit-protocol programs ==="
	@for elf in $(ELFS); do \
	    printf '%-34s ' "$$elf"; \
	    $(READELF) -h $$elf | grep -o 'Entry point address:.*'; \
	done

dis: $(ELFS)
	@for elf in $(ELFS); do \
	    echo "--- $$elf ---"; \
	    $(OBJDUMP) -d --no-show-raw-insn $$elf | sed -n '/<_start>:/,/^$$/p'; \
	done

audit: $(AUDIT_ELFS)
	@echo "audit: $(AUDITABLE) (x04_wfi excluded: it deliberately executes WFI)"
	$(PYTHON) ../audit/check_p0_isa.py --objdump $(OBJDUMP) --readelf $(READELF) \
	    --arch 'rv64i2p1_m2p0_zicsr2p0_zifencei2p0_zmmul1p0' $(AUDIT_ELFS)

clean:
	rm -rf $(BUILDDIR)
