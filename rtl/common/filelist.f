# Ordered sources for rtl/common. Order matters: packages and typedefs are
# elaborated before the modules that use them, and Verilator and Yosys both
# honour file order.
mosaic_fifo.sv
mosaic_skid_buffer.sv
mosaic_ram.sv
