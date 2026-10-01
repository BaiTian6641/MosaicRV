// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Design internal header
// See Vharness_tb.h for the primary calling header

#ifndef VERILATED_VHARNESS_TB___024ROOT_H_
#define VERILATED_VHARNESS_TB___024ROOT_H_  // guard

#include "verilated.h"


class Vharness_tb__Syms;

class alignas(VL_CACHE_LINE_BYTES) Vharness_tb___024root final {
  public:

    // DESIGN SPECIFIC STATE
    VL_IN8(clk,0,0);
    VL_IN8(rst,0,0);
    VL_IN8(step_valid,0,0);
    VL_OUT8(step_ready,0,0);
    VL_OUT8(mem_en,0,0);
    VL_OUT8(mem_byte_en,7,0);
    VL_OUT8(retire,0,0);
    VL_OUT8(illegal,0,0);
    VL_OUT8(rd_we,0,0);
    VL_OUT8(rd_index,4,0);
    CData/*0:0*/ harness_tb__DOT__u_probe__DOT__does_store;
    CData/*0:0*/ harness_tb__DOT__u_probe__DOT__write_rd;
    CData/*0:0*/ __Vtrigprevexpr___TOP__clk__0;
    CData/*0:0*/ __Vtrigprevexpr___TOP__rst__0;
    CData/*0:0*/ __Vtrigprevexpr___TOP__step_valid__0;
    CData/*0:0*/ __VicoDidInit;
    CData/*0:0*/ __Vtrigprevexpr___TOP__clk__1;
    VL_IN(insn_in,31,0);
    IData/*31:0*/ __Vtrigprevexpr___TOP__insn_in__0;
    VL_IN64(pc_in,63,0);
    VL_OUT64(mem_addr,63,0);
    VL_OUT64(mem_wdata,63,0);
    VL_OUT64(next_pc,63,0);
    VL_OUT64(rd_value_out,63,0);
    QData/*63:0*/ __Vtrigprevexpr___TOP__pc_in__0;
    VlUnpacked<QData/*63:0*/, 32> harness_tb__DOT__u_probe__DOT__xregs;
    VlUnpacked<QData/*63:0*/, 1> __VstlTriggered;
    VlUnpacked<QData/*63:0*/, 2> __VicoTriggered;
    VlUnpacked<QData/*63:0*/, 1> __VactTriggered;
    VlUnpacked<QData/*63:0*/, 1> __VnbaTriggered;

    // INTERNAL VARIABLES
    Vharness_tb__Syms* vlSymsp;
    const char* vlNamep;

    // CONSTRUCTORS
    Vharness_tb___024root(Vharness_tb__Syms* symsp, const char* namep);
    ~Vharness_tb___024root();
    VL_UNCOPYABLE(Vharness_tb___024root);

    // INTERNAL METHODS
    void __Vconfigure(bool first);
};


#endif  // guard
