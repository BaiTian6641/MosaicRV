// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Design internal header
// See Vmosaic_fifo_tb.h for the primary calling header

#ifndef VERILATED_VMOSAIC_FIFO_TB___024ROOT_H_
#define VERILATED_VMOSAIC_FIFO_TB___024ROOT_H_  // guard

#include "verilated.h"


class Vmosaic_fifo_tb__Syms;

class alignas(VL_CACHE_LINE_BYTES) Vmosaic_fifo_tb___024root final {
  public:

    // DESIGN SPECIFIC STATE
    // Anonymous structures to workaround compiler member-count bugs
    struct {
        VL_IN8(clk,0,0);
        VL_IN8(rst,0,0);
        VL_IN8(d1_in_valid,0,0);
        VL_OUT8(d1_in_ready,0,0);
        VL_OUT8(d1_out_valid,0,0);
        VL_IN8(d1_out_ready,0,0);
        VL_OUT8(d1_count,0,0);
        VL_IN8(d2_in_valid,0,0);
        VL_OUT8(d2_in_ready,0,0);
        VL_OUT8(d2_out_valid,0,0);
        VL_IN8(d2_out_ready,0,0);
        VL_OUT8(d2_count,1,0);
        VL_IN8(d3_in_valid,0,0);
        VL_OUT8(d3_in_ready,0,0);
        VL_OUT8(d3_out_valid,0,0);
        VL_IN8(d3_out_ready,0,0);
        VL_OUT8(d3_count,1,0);
        VL_IN8(d8_in_valid,0,0);
        VL_OUT8(d8_in_ready,0,0);
        VL_OUT8(d8_out_valid,0,0);
        VL_IN8(d8_out_ready,0,0);
        VL_OUT8(d8_count,3,0);
        VL_IN8(sk_in_valid,0,0);
        VL_OUT8(sk_in_ready,0,0);
        VL_OUT8(sk_out_valid,0,0);
        VL_IN8(sk_out_ready,0,0);
        CData/*0:0*/ mosaic_fifo_tb__DOT__u_sk__DOT__valid_r;
        CData/*2:0*/ mosaic_fifo_tb__DOT__u_d8__DOT__rd_ptr;
        CData/*2:0*/ mosaic_fifo_tb__DOT__u_d8__DOT__wr_ptr;
        CData/*0:0*/ mosaic_fifo_tb__DOT__u_d8__DOT__push;
        CData/*0:0*/ mosaic_fifo_tb__DOT__u_d8__DOT__pop;
        CData/*1:0*/ mosaic_fifo_tb__DOT__u_d3__DOT__rd_ptr;
        CData/*1:0*/ mosaic_fifo_tb__DOT__u_d3__DOT__wr_ptr;
        CData/*0:0*/ mosaic_fifo_tb__DOT__u_d3__DOT__push;
        CData/*0:0*/ mosaic_fifo_tb__DOT__u_d3__DOT__pop;
        CData/*0:0*/ mosaic_fifo_tb__DOT__u_d2__DOT__rd_ptr;
        CData/*0:0*/ mosaic_fifo_tb__DOT__u_d2__DOT__wr_ptr;
        CData/*0:0*/ mosaic_fifo_tb__DOT__u_d2__DOT__push;
        CData/*0:0*/ mosaic_fifo_tb__DOT__u_d2__DOT__pop;
        CData/*0:0*/ mosaic_fifo_tb__DOT__u_d1__DOT__rd_ptr;
        CData/*0:0*/ mosaic_fifo_tb__DOT__u_d1__DOT__wr_ptr;
        CData/*0:0*/ mosaic_fifo_tb__DOT__u_d1__DOT__push;
        CData/*0:0*/ mosaic_fifo_tb__DOT__u_d1__DOT__pop;
        CData/*0:0*/ __Vtrigprevexpr___TOP__clk__0;
        CData/*0:0*/ __Vtrigprevexpr___TOP__rst__0;
        CData/*0:0*/ __Vtrigprevexpr___TOP__d1_in_valid__0;
        CData/*0:0*/ __Vtrigprevexpr___TOP__d1_out_ready__0;
        CData/*0:0*/ __Vtrigprevexpr___TOP__d2_in_valid__0;
        CData/*0:0*/ __Vtrigprevexpr___TOP__d2_out_ready__0;
        CData/*0:0*/ __Vtrigprevexpr___TOP__d3_in_valid__0;
        CData/*0:0*/ __Vtrigprevexpr___TOP__d3_out_ready__0;
        CData/*0:0*/ __Vtrigprevexpr___TOP__d8_in_valid__0;
        CData/*0:0*/ __Vtrigprevexpr___TOP__d8_out_ready__0;
        CData/*0:0*/ __Vtrigprevexpr___TOP__sk_in_valid__0;
        CData/*0:0*/ __Vtrigprevexpr___TOP__sk_out_ready__0;
        CData/*0:0*/ __VicoDidInit;
        CData/*0:0*/ __Vtrigprevexpr___TOP__clk__1;
        VL_IN(d1_in_payload,31,0);
        VL_OUT(d1_out_payload,31,0);
        VL_IN(d2_in_payload,31,0);
        VL_OUT(d2_out_payload,31,0);
        VL_IN(d3_in_payload,31,0);
        VL_OUT(d3_out_payload,31,0);
        VL_IN(d8_in_payload,31,0);
    };
    struct {
        VL_OUT(d8_out_payload,31,0);
        VL_IN(sk_in_payload,31,0);
        VL_OUT(sk_out_payload,31,0);
        IData/*31:0*/ mosaic_fifo_tb__DOT__u_sk__DOT__payload_r;
        IData/*31:0*/ mosaic_fifo_tb__DOT__u_d3__DOT____Vxrand___0;
        IData/*31:0*/ mosaic_fifo_tb__DOT__u_d1__DOT____Vxrand___0;
        IData/*31:0*/ __Vtrigprevexpr___TOP__d1_in_payload__0;
        IData/*31:0*/ __Vtrigprevexpr___TOP__d2_in_payload__0;
        IData/*31:0*/ __Vtrigprevexpr___TOP__d3_in_payload__0;
        IData/*31:0*/ __Vtrigprevexpr___TOP__d8_in_payload__0;
        IData/*31:0*/ __Vtrigprevexpr___TOP__sk_in_payload__0;
        VlUnpacked<IData/*31:0*/, 8> mosaic_fifo_tb__DOT__u_d8__DOT__mem;
        VlUnpacked<IData/*31:0*/, 3> mosaic_fifo_tb__DOT__u_d3__DOT__mem;
        VlUnpacked<IData/*31:0*/, 2> mosaic_fifo_tb__DOT__u_d2__DOT__mem;
        VlUnpacked<IData/*31:0*/, 1> mosaic_fifo_tb__DOT__u_d1__DOT__mem;
        VlUnpacked<QData/*63:0*/, 1> __VstlTriggered;
        VlUnpacked<QData/*63:0*/, 2> __VicoTriggered;
        VlUnpacked<QData/*63:0*/, 1> __VactTriggered;
        VlUnpacked<QData/*63:0*/, 1> __VnbaTriggered;
    };

    // INTERNAL VARIABLES
    Vmosaic_fifo_tb__Syms* vlSymsp;
    const char* vlNamep;

    // CONSTRUCTORS
    Vmosaic_fifo_tb___024root(Vmosaic_fifo_tb__Syms* symsp, const char* namep);
    ~Vmosaic_fifo_tb___024root();
    VL_UNCOPYABLE(Vmosaic_fifo_tb___024root);

    // INTERNAL METHODS
    void __Vconfigure(bool first);
};


#endif  // guard
