// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Design internal header
// See Vmosaic_ram_tb.h for the primary calling header

#ifndef VERILATED_VMOSAIC_RAM_TB___024ROOT_H_
#define VERILATED_VMOSAIC_RAM_TB___024ROOT_H_  // guard

#include "verilated.h"


class Vmosaic_ram_tb__Syms;

class alignas(VL_CACHE_LINE_BYTES) Vmosaic_ram_tb___024root final {
  public:

    // DESIGN SPECIFIC STATE
    VL_IN8(clk,0,0);
    VL_IN8(rst,0,0);
    VL_IN8(waddr,5,0);
    VL_IN8(wmask,7,0);
    VL_IN8(we,0,0);
    VL_IN8(raddr,5,0);
    VL_OUT8(rvalid_a,0,0);
    VL_OUT8(rvalid_b,0,0);
    CData/*0:0*/ __Vtrigprevexpr___TOP__clk__0;
    CData/*0:0*/ __Vtrigprevexpr___TOP__rst__0;
    CData/*5:0*/ __Vtrigprevexpr___TOP__waddr__0;
    CData/*7:0*/ __Vtrigprevexpr___TOP__wmask__0;
    CData/*0:0*/ __Vtrigprevexpr___TOP__we__0;
    CData/*5:0*/ __Vtrigprevexpr___TOP__raddr__0;
    CData/*0:0*/ __VicoDidInit;
    CData/*0:0*/ __Vtrigprevexpr___TOP__clk__1;
    VL_IN64(wdata,63,0);
    VL_OUT64(rdata_a,63,0);
    VL_OUT64(rdata_b,63,0);
    QData/*63:0*/ __Vtrigprevexpr___TOP__wdata__0;
    VlUnpacked<QData/*63:0*/, 64> mosaic_ram_tb__DOT__u_bank_b__DOT__mem;
    VlUnpacked<QData/*63:0*/, 64> mosaic_ram_tb__DOT__u_bank_a__DOT__mem;
    VlUnpacked<QData/*63:0*/, 1> __VstlTriggered;
    VlUnpacked<QData/*63:0*/, 2> __VicoTriggered;
    VlUnpacked<QData/*63:0*/, 1> __VactTriggered;
    VlUnpacked<QData/*63:0*/, 1> __VnbaTriggered;

    // INTERNAL VARIABLES
    Vmosaic_ram_tb__Syms* vlSymsp;
    const char* vlNamep;

    // CONSTRUCTORS
    Vmosaic_ram_tb___024root(Vmosaic_ram_tb__Syms* symsp, const char* namep);
    ~Vmosaic_ram_tb___024root();
    VL_UNCOPYABLE(Vmosaic_ram_tb___024root);

    // INTERNAL METHODS
    void __Vconfigure(bool first);
};


#endif  // guard
