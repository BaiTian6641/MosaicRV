// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Design implementation internals
// See Vmosaic_ram_tb.h for the primary calling header

#include "Vmosaic_ram_tb__pch.h"

void Vmosaic_ram_tb___024root___eval_sample(Vmosaic_ram_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_ram_tb___024root___eval_sample\n"); );
    Vmosaic_ram_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
}

#ifdef VL_DEBUG
VL_ATTR_COLD void Vmosaic_ram_tb___024root___dump_triggers__ico(const VlUnpacked<QData/*63:0*/, 2> &triggers, const std::string &tag);
#endif  // VL_DEBUG

bool Vmosaic_ram_tb___024root___eval_ico(Vmosaic_ram_tb___024root* vlSelf, CData/*0:0*/ firstIteration) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_ram_tb___024root___eval_ico\n"); );
    Vmosaic_ram_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    vlSelfRef.__VicoTriggered[1U] = ((0xfffffffffffffffeULL 
                                      & vlSelfRef.__VicoTriggered[1U]) 
                                     | (IData)((IData)(firstIteration)));
    {
        // Inlined CFunc: _eval_triggers_vec__ico
        vlSelfRef.__VicoTriggered[0U] = (QData)((IData)(
                                                        (((((IData)(vlSelfRef.raddr) 
                                                            != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__raddr__0)) 
                                                           << 6U) 
                                                          | ((((IData)(vlSelfRef.we) 
                                                               != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__we__0)) 
                                                              << 5U) 
                                                             | (((IData)(vlSelfRef.wmask) 
                                                                 != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__wmask__0)) 
                                                                << 4U))) 
                                                         | ((((vlSelfRef.wdata 
                                                               != vlSelfRef.__Vtrigprevexpr___TOP__wdata__0) 
                                                              << 3U) 
                                                             | (((IData)(vlSelfRef.waddr) 
                                                                 != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__waddr__0)) 
                                                                << 2U)) 
                                                            | ((((IData)(vlSelfRef.rst) 
                                                                 != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__rst__0)) 
                                                                << 1U) 
                                                               | ((IData)(vlSelfRef.clk) 
                                                                  != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__clk__0)))))));
        vlSelfRef.__Vtrigprevexpr___TOP__clk__0 = vlSelfRef.clk;
        vlSelfRef.__Vtrigprevexpr___TOP__rst__0 = vlSelfRef.rst;
        vlSelfRef.__Vtrigprevexpr___TOP__waddr__0 = vlSelfRef.waddr;
        vlSelfRef.__Vtrigprevexpr___TOP__wdata__0 = vlSelfRef.wdata;
        vlSelfRef.__Vtrigprevexpr___TOP__wmask__0 = vlSelfRef.wmask;
        vlSelfRef.__Vtrigprevexpr___TOP__we__0 = vlSelfRef.we;
        vlSelfRef.__Vtrigprevexpr___TOP__raddr__0 = vlSelfRef.raddr;
        if (VL_UNLIKELY(((1U & (~ (IData)(vlSelfRef.__VicoDidInit)))))) {
            vlSelfRef.__VicoDidInit = 1U;
            vlSelfRef.__VicoTriggered[0U] = (1ULL | vlSelfRef.__VicoTriggered[0U]);
            vlSelfRef.__VicoTriggered[0U] = (2ULL | vlSelfRef.__VicoTriggered[0U]);
            vlSelfRef.__VicoTriggered[0U] = (4ULL | vlSelfRef.__VicoTriggered[0U]);
            vlSelfRef.__VicoTriggered[0U] = (8ULL | vlSelfRef.__VicoTriggered[0U]);
            vlSelfRef.__VicoTriggered[0U] = (0x0000000000000010ULL 
                                             | vlSelfRef.__VicoTriggered[0U]);
            vlSelfRef.__VicoTriggered[0U] = (0x0000000000000020ULL 
                                             | vlSelfRef.__VicoTriggered[0U]);
            vlSelfRef.__VicoTriggered[0U] = (0x0000000000000040ULL 
                                             | vlSelfRef.__VicoTriggered[0U]);
        }
    }
#ifdef VL_DEBUG
    if (VL_UNLIKELY(vlSymsp->_vm_contextp__->debug())) {
        Vmosaic_ram_tb___024root___dump_triggers__ico(vlSelfRef.__VicoTriggered, "ico"s);
    }
#endif
    return (0U);
}

#ifdef VL_DEBUG
VL_ATTR_COLD void Vmosaic_ram_tb___024root___dump_triggers__act(const VlUnpacked<QData/*63:0*/, 1> &triggers, const std::string &tag);
#endif  // VL_DEBUG
void Vmosaic_ram_tb___024root___trigger_orInto__act_vec_vec(VlUnpacked<QData/*63:0*/, 1> &out, const VlUnpacked<QData/*63:0*/, 1> &in);

bool Vmosaic_ram_tb___024root___eval_act(Vmosaic_ram_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_ram_tb___024root___eval_act\n"); );
    Vmosaic_ram_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    {
        // Inlined CFunc: _eval_triggers_vec__act
        vlSelfRef.__VactTriggered[0U] = (QData)((IData)(
                                                        ((IData)(vlSelfRef.clk) 
                                                         & (~ (IData)(vlSelfRef.__Vtrigprevexpr___TOP__clk__1)))));
        vlSelfRef.__Vtrigprevexpr___TOP__clk__1 = vlSelfRef.clk;
    }
#ifdef VL_DEBUG
    if (VL_UNLIKELY(vlSymsp->_vm_contextp__->debug())) {
        Vmosaic_ram_tb___024root___dump_triggers__act(vlSelfRef.__VactTriggered, "act"s);
    }
#endif
    Vmosaic_ram_tb___024root___trigger_orInto__act_vec_vec(vlSelfRef.__VnbaTriggered, vlSelfRef.__VactTriggered);
    return (0U);
}

bool Vmosaic_ram_tb___024root___eval_inact(Vmosaic_ram_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_ram_tb___024root___eval_inact\n"); );
    Vmosaic_ram_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    return (0U);
}

bool Vmosaic_ram_tb___024root___trigger_anySet__act(const VlUnpacked<QData/*63:0*/, 1> &in);
void Vmosaic_ram_tb___024root___nba_sequent__TOP__0(Vmosaic_ram_tb___024root* vlSelf);
void Vmosaic_ram_tb___024root___trigger_clear__act(VlUnpacked<QData/*63:0*/, 1> &out);

bool Vmosaic_ram_tb___024root___eval_nba(Vmosaic_ram_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_ram_tb___024root___eval_nba\n"); );
    Vmosaic_ram_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Locals
    CData/*0:0*/ __VnbaExecute;
    // Body
    __VnbaExecute = Vmosaic_ram_tb___024root___trigger_anySet__act(vlSelfRef.__VnbaTriggered);
    if (__VnbaExecute) {
        {
            // Inlined CFunc: _eval_body__nba
            if ((1ULL & vlSelfRef.__VnbaTriggered[0U])) {
                Vmosaic_ram_tb___024root___nba_sequent__TOP__0(vlSelf);
            }
        }
        Vmosaic_ram_tb___024root___trigger_clear__act(vlSelfRef.__VnbaTriggered);
    }
    return (__VnbaExecute);
}

bool Vmosaic_ram_tb___024root___eval_obs(Vmosaic_ram_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_ram_tb___024root___eval_obs\n"); );
    Vmosaic_ram_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    return (0U);
}

bool Vmosaic_ram_tb___024root___eval_react(Vmosaic_ram_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_ram_tb___024root___eval_react\n"); );
    Vmosaic_ram_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    return (0U);
}

void Vmosaic_ram_tb___024root___eval_postponed(Vmosaic_ram_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_ram_tb___024root___eval_postponed\n"); );
    Vmosaic_ram_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
}

bool Vmosaic_ram_tb___024root___trigger_anySet__ico(const VlUnpacked<QData/*63:0*/, 2> &in) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_ram_tb___024root___trigger_anySet__ico\n"); );
    // Locals
    IData/*31:0*/ n;
    // Body
    n = 0U;
    do {
        if (in[n]) {
            return (1U);
        }
        n = ((IData)(1U) + n);
    } while ((2U > n));
    return (0U);
}

bool Vmosaic_ram_tb___024root___trigger_anySet__act(const VlUnpacked<QData/*63:0*/, 1> &in) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_ram_tb___024root___trigger_anySet__act\n"); );
    // Locals
    IData/*31:0*/ n;
    // Body
    n = 0U;
    do {
        if (in[n]) {
            return (1U);
        }
        n = ((IData)(1U) + n);
    } while ((1U > n));
    return (0U);
}

void Vmosaic_ram_tb___024root___nba_sequent__TOP__0(Vmosaic_ram_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_ram_tb___024root___nba_sequent__TOP__0\n"); );
    Vmosaic_ram_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Locals
    CData/*7:0*/ __VdlyVal__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v0;
    __VdlyVal__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v0 = 0;
    CData/*5:0*/ __VdlyDim0__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v0;
    __VdlyDim0__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v0 = 0;
    CData/*0:0*/ __VdlySet__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v0;
    __VdlySet__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v0 = 0;
    CData/*7:0*/ __VdlyVal__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v1;
    __VdlyVal__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v1 = 0;
    CData/*5:0*/ __VdlyDim0__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v1;
    __VdlyDim0__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v1 = 0;
    CData/*0:0*/ __VdlySet__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v1;
    __VdlySet__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v1 = 0;
    CData/*7:0*/ __VdlyVal__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v2;
    __VdlyVal__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v2 = 0;
    CData/*5:0*/ __VdlyDim0__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v2;
    __VdlyDim0__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v2 = 0;
    CData/*0:0*/ __VdlySet__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v2;
    __VdlySet__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v2 = 0;
    CData/*7:0*/ __VdlyVal__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v3;
    __VdlyVal__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v3 = 0;
    CData/*5:0*/ __VdlyDim0__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v3;
    __VdlyDim0__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v3 = 0;
    CData/*0:0*/ __VdlySet__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v3;
    __VdlySet__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v3 = 0;
    CData/*7:0*/ __VdlyVal__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v4;
    __VdlyVal__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v4 = 0;
    CData/*5:0*/ __VdlyDim0__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v4;
    __VdlyDim0__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v4 = 0;
    CData/*0:0*/ __VdlySet__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v4;
    __VdlySet__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v4 = 0;
    CData/*7:0*/ __VdlyVal__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v5;
    __VdlyVal__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v5 = 0;
    CData/*5:0*/ __VdlyDim0__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v5;
    __VdlyDim0__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v5 = 0;
    CData/*0:0*/ __VdlySet__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v5;
    __VdlySet__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v5 = 0;
    CData/*7:0*/ __VdlyVal__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v6;
    __VdlyVal__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v6 = 0;
    CData/*5:0*/ __VdlyDim0__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v6;
    __VdlyDim0__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v6 = 0;
    CData/*0:0*/ __VdlySet__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v6;
    __VdlySet__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v6 = 0;
    CData/*7:0*/ __VdlyVal__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v7;
    __VdlyVal__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v7 = 0;
    CData/*5:0*/ __VdlyDim0__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v7;
    __VdlyDim0__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v7 = 0;
    CData/*0:0*/ __VdlySet__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v7;
    __VdlySet__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v7 = 0;
    CData/*7:0*/ __VdlyVal__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v0;
    __VdlyVal__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v0 = 0;
    CData/*5:0*/ __VdlyDim0__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v0;
    __VdlyDim0__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v0 = 0;
    CData/*0:0*/ __VdlySet__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v0;
    __VdlySet__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v0 = 0;
    CData/*7:0*/ __VdlyVal__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v1;
    __VdlyVal__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v1 = 0;
    CData/*5:0*/ __VdlyDim0__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v1;
    __VdlyDim0__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v1 = 0;
    CData/*0:0*/ __VdlySet__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v1;
    __VdlySet__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v1 = 0;
    CData/*7:0*/ __VdlyVal__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v2;
    __VdlyVal__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v2 = 0;
    CData/*5:0*/ __VdlyDim0__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v2;
    __VdlyDim0__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v2 = 0;
    CData/*0:0*/ __VdlySet__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v2;
    __VdlySet__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v2 = 0;
    CData/*7:0*/ __VdlyVal__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v3;
    __VdlyVal__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v3 = 0;
    CData/*5:0*/ __VdlyDim0__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v3;
    __VdlyDim0__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v3 = 0;
    CData/*0:0*/ __VdlySet__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v3;
    __VdlySet__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v3 = 0;
    CData/*7:0*/ __VdlyVal__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v4;
    __VdlyVal__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v4 = 0;
    CData/*5:0*/ __VdlyDim0__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v4;
    __VdlyDim0__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v4 = 0;
    CData/*0:0*/ __VdlySet__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v4;
    __VdlySet__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v4 = 0;
    CData/*7:0*/ __VdlyVal__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v5;
    __VdlyVal__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v5 = 0;
    CData/*5:0*/ __VdlyDim0__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v5;
    __VdlyDim0__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v5 = 0;
    CData/*0:0*/ __VdlySet__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v5;
    __VdlySet__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v5 = 0;
    CData/*7:0*/ __VdlyVal__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v6;
    __VdlyVal__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v6 = 0;
    CData/*5:0*/ __VdlyDim0__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v6;
    __VdlyDim0__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v6 = 0;
    CData/*0:0*/ __VdlySet__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v6;
    __VdlySet__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v6 = 0;
    CData/*7:0*/ __VdlyVal__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v7;
    __VdlyVal__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v7 = 0;
    CData/*5:0*/ __VdlyDim0__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v7;
    __VdlyDim0__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v7 = 0;
    CData/*0:0*/ __VdlySet__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v7;
    __VdlySet__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v7 = 0;
    // Body
    __VdlySet__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v0 = 0U;
    __VdlySet__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v1 = 0U;
    __VdlySet__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v2 = 0U;
    __VdlySet__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v3 = 0U;
    __VdlySet__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v4 = 0U;
    __VdlySet__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v5 = 0U;
    __VdlySet__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v6 = 0U;
    __VdlySet__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v7 = 0U;
    __VdlySet__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v0 = 0U;
    __VdlySet__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v1 = 0U;
    __VdlySet__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v2 = 0U;
    __VdlySet__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v3 = 0U;
    __VdlySet__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v4 = 0U;
    __VdlySet__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v5 = 0U;
    __VdlySet__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v6 = 0U;
    __VdlySet__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v7 = 0U;
    if (((~ (IData)(vlSelfRef.rst)) & (IData)(vlSelfRef.we))) {
        if ((1U & (IData)(vlSelfRef.wmask))) {
            __VdlyVal__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v0 
                = (0x000000ffU & (IData)(vlSelfRef.wdata));
            __VdlyDim0__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v0 
                = (0x2aU ^ (IData)(vlSelfRef.waddr));
            __VdlySet__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v0 = 1U;
            __VdlyVal__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v0 
                = (0x000000ffU & (IData)(vlSelfRef.wdata));
            __VdlyDim0__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v0 
                = vlSelfRef.waddr;
            __VdlySet__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v0 = 1U;
        }
        if ((2U & (IData)(vlSelfRef.wmask))) {
            __VdlyVal__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v1 
                = (0x000000ffU & (IData)((vlSelfRef.wdata 
                                          >> 8U)));
            __VdlyDim0__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v1 
                = (0x2aU ^ (IData)(vlSelfRef.waddr));
            __VdlySet__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v1 = 1U;
            __VdlyVal__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v1 
                = (0x000000ffU & (IData)((vlSelfRef.wdata 
                                          >> 8U)));
            __VdlyDim0__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v1 
                = vlSelfRef.waddr;
            __VdlySet__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v1 = 1U;
        }
        if ((4U & (IData)(vlSelfRef.wmask))) {
            __VdlyVal__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v2 
                = (0x000000ffU & (IData)((vlSelfRef.wdata 
                                          >> 0x10U)));
            __VdlyDim0__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v2 
                = (0x2aU ^ (IData)(vlSelfRef.waddr));
            __VdlySet__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v2 = 1U;
            __VdlyVal__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v2 
                = (0x000000ffU & (IData)((vlSelfRef.wdata 
                                          >> 0x10U)));
            __VdlyDim0__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v2 
                = vlSelfRef.waddr;
            __VdlySet__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v2 = 1U;
        }
        if ((8U & (IData)(vlSelfRef.wmask))) {
            __VdlyVal__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v3 
                = (0x000000ffU & (IData)((vlSelfRef.wdata 
                                          >> 0x18U)));
            __VdlyDim0__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v3 
                = (0x2aU ^ (IData)(vlSelfRef.waddr));
            __VdlySet__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v3 = 1U;
            __VdlyVal__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v3 
                = (0x000000ffU & (IData)((vlSelfRef.wdata 
                                          >> 0x18U)));
            __VdlyDim0__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v3 
                = vlSelfRef.waddr;
            __VdlySet__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v3 = 1U;
        }
        if ((0x00000010U & (IData)(vlSelfRef.wmask))) {
            __VdlyVal__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v4 
                = (0x000000ffU & (IData)((vlSelfRef.wdata 
                                          >> 0x20U)));
            __VdlyDim0__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v4 
                = (0x2aU ^ (IData)(vlSelfRef.waddr));
            __VdlySet__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v4 = 1U;
            __VdlyVal__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v4 
                = (0x000000ffU & (IData)((vlSelfRef.wdata 
                                          >> 0x20U)));
            __VdlyDim0__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v4 
                = vlSelfRef.waddr;
            __VdlySet__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v4 = 1U;
        }
        if ((0x00000020U & (IData)(vlSelfRef.wmask))) {
            __VdlyVal__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v5 
                = (0x000000ffU & (IData)((vlSelfRef.wdata 
                                          >> 0x28U)));
            __VdlyDim0__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v5 
                = (0x2aU ^ (IData)(vlSelfRef.waddr));
            __VdlySet__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v5 = 1U;
            __VdlyVal__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v5 
                = (0x000000ffU & (IData)((vlSelfRef.wdata 
                                          >> 0x28U)));
            __VdlyDim0__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v5 
                = vlSelfRef.waddr;
            __VdlySet__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v5 = 1U;
        }
        if ((0x00000040U & (IData)(vlSelfRef.wmask))) {
            __VdlyVal__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v6 
                = (0x000000ffU & (IData)((vlSelfRef.wdata 
                                          >> 0x30U)));
            __VdlyDim0__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v6 
                = (0x2aU ^ (IData)(vlSelfRef.waddr));
            __VdlySet__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v6 = 1U;
            __VdlyVal__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v6 
                = (0x000000ffU & (IData)((vlSelfRef.wdata 
                                          >> 0x30U)));
            __VdlyDim0__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v6 
                = vlSelfRef.waddr;
            __VdlySet__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v6 = 1U;
        }
        if ((0x00000080U & (IData)(vlSelfRef.wmask))) {
            __VdlyVal__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v7 
                = (0x000000ffU & (IData)((vlSelfRef.wdata 
                                          >> 0x38U)));
            __VdlyDim0__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v7 
                = (0x2aU ^ (IData)(vlSelfRef.waddr));
            __VdlySet__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v7 = 1U;
            __VdlyVal__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v7 
                = (0x000000ffU & (IData)((vlSelfRef.wdata 
                                          >> 0x38U)));
            __VdlyDim0__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v7 
                = vlSelfRef.waddr;
            __VdlySet__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v7 = 1U;
        }
    }
    vlSelfRef.rvalid_b = (1U & (~ (IData)(vlSelfRef.rst)));
    vlSelfRef.rvalid_a = (1U & (~ (IData)(vlSelfRef.rst)));
    if ((1U & (~ (IData)(vlSelfRef.rst)))) {
        vlSelfRef.rdata_b = vlSelfRef.mosaic_ram_tb__DOT__u_bank_b__DOT__mem
            [(0x2aU ^ (IData)(vlSelfRef.raddr))];
        vlSelfRef.rdata_a = vlSelfRef.mosaic_ram_tb__DOT__u_bank_a__DOT__mem
            [vlSelfRef.raddr];
    }
    if (__VdlySet__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v0) {
        vlSelfRef.mosaic_ram_tb__DOT__u_bank_b__DOT__mem[__VdlyDim0__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v0] 
            = ((0xffffffffffffff00ULL & vlSelfRef.mosaic_ram_tb__DOT__u_bank_b__DOT__mem
                [__VdlyDim0__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v0]) 
               | (IData)((IData)(__VdlyVal__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v0)));
    }
    if (__VdlySet__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v1) {
        vlSelfRef.mosaic_ram_tb__DOT__u_bank_b__DOT__mem[__VdlyDim0__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v1] 
            = ((0xffffffffffff00ffULL & vlSelfRef.mosaic_ram_tb__DOT__u_bank_b__DOT__mem
                [__VdlyDim0__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v1]) 
               | ((QData)((IData)(__VdlyVal__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v1)) 
                  << 8U));
    }
    if (__VdlySet__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v2) {
        vlSelfRef.mosaic_ram_tb__DOT__u_bank_b__DOT__mem[__VdlyDim0__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v2] 
            = ((0xffffffffff00ffffULL & vlSelfRef.mosaic_ram_tb__DOT__u_bank_b__DOT__mem
                [__VdlyDim0__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v2]) 
               | ((QData)((IData)(__VdlyVal__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v2)) 
                  << 0x00000010U));
    }
    if (__VdlySet__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v3) {
        vlSelfRef.mosaic_ram_tb__DOT__u_bank_b__DOT__mem[__VdlyDim0__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v3] 
            = ((0xffffffff00ffffffULL & vlSelfRef.mosaic_ram_tb__DOT__u_bank_b__DOT__mem
                [__VdlyDim0__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v3]) 
               | ((QData)((IData)(__VdlyVal__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v3)) 
                  << 0x00000018U));
    }
    if (__VdlySet__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v4) {
        vlSelfRef.mosaic_ram_tb__DOT__u_bank_b__DOT__mem[__VdlyDim0__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v4] 
            = ((0xffffff00ffffffffULL & vlSelfRef.mosaic_ram_tb__DOT__u_bank_b__DOT__mem
                [__VdlyDim0__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v4]) 
               | ((QData)((IData)(__VdlyVal__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v4)) 
                  << 0x00000020U));
    }
    if (__VdlySet__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v5) {
        vlSelfRef.mosaic_ram_tb__DOT__u_bank_b__DOT__mem[__VdlyDim0__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v5] 
            = ((0xffff00ffffffffffULL & vlSelfRef.mosaic_ram_tb__DOT__u_bank_b__DOT__mem
                [__VdlyDim0__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v5]) 
               | ((QData)((IData)(__VdlyVal__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v5)) 
                  << 0x00000028U));
    }
    if (__VdlySet__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v6) {
        vlSelfRef.mosaic_ram_tb__DOT__u_bank_b__DOT__mem[__VdlyDim0__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v6] 
            = ((0xff00ffffffffffffULL & vlSelfRef.mosaic_ram_tb__DOT__u_bank_b__DOT__mem
                [__VdlyDim0__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v6]) 
               | ((QData)((IData)(__VdlyVal__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v6)) 
                  << 0x00000030U));
    }
    if (__VdlySet__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v7) {
        vlSelfRef.mosaic_ram_tb__DOT__u_bank_b__DOT__mem[__VdlyDim0__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v7] 
            = ((0x00ffffffffffffffULL & vlSelfRef.mosaic_ram_tb__DOT__u_bank_b__DOT__mem
                [__VdlyDim0__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v7]) 
               | ((QData)((IData)(__VdlyVal__mosaic_ram_tb__DOT__u_bank_b__DOT__mem__v7)) 
                  << 0x00000038U));
    }
    if (__VdlySet__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v0) {
        vlSelfRef.mosaic_ram_tb__DOT__u_bank_a__DOT__mem[__VdlyDim0__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v0] 
            = ((0xffffffffffffff00ULL & vlSelfRef.mosaic_ram_tb__DOT__u_bank_a__DOT__mem
                [__VdlyDim0__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v0]) 
               | (IData)((IData)(__VdlyVal__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v0)));
    }
    if (__VdlySet__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v1) {
        vlSelfRef.mosaic_ram_tb__DOT__u_bank_a__DOT__mem[__VdlyDim0__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v1] 
            = ((0xffffffffffff00ffULL & vlSelfRef.mosaic_ram_tb__DOT__u_bank_a__DOT__mem
                [__VdlyDim0__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v1]) 
               | ((QData)((IData)(__VdlyVal__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v1)) 
                  << 8U));
    }
    if (__VdlySet__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v2) {
        vlSelfRef.mosaic_ram_tb__DOT__u_bank_a__DOT__mem[__VdlyDim0__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v2] 
            = ((0xffffffffff00ffffULL & vlSelfRef.mosaic_ram_tb__DOT__u_bank_a__DOT__mem
                [__VdlyDim0__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v2]) 
               | ((QData)((IData)(__VdlyVal__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v2)) 
                  << 0x00000010U));
    }
    if (__VdlySet__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v3) {
        vlSelfRef.mosaic_ram_tb__DOT__u_bank_a__DOT__mem[__VdlyDim0__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v3] 
            = ((0xffffffff00ffffffULL & vlSelfRef.mosaic_ram_tb__DOT__u_bank_a__DOT__mem
                [__VdlyDim0__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v3]) 
               | ((QData)((IData)(__VdlyVal__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v3)) 
                  << 0x00000018U));
    }
    if (__VdlySet__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v4) {
        vlSelfRef.mosaic_ram_tb__DOT__u_bank_a__DOT__mem[__VdlyDim0__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v4] 
            = ((0xffffff00ffffffffULL & vlSelfRef.mosaic_ram_tb__DOT__u_bank_a__DOT__mem
                [__VdlyDim0__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v4]) 
               | ((QData)((IData)(__VdlyVal__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v4)) 
                  << 0x00000020U));
    }
    if (__VdlySet__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v5) {
        vlSelfRef.mosaic_ram_tb__DOT__u_bank_a__DOT__mem[__VdlyDim0__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v5] 
            = ((0xffff00ffffffffffULL & vlSelfRef.mosaic_ram_tb__DOT__u_bank_a__DOT__mem
                [__VdlyDim0__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v5]) 
               | ((QData)((IData)(__VdlyVal__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v5)) 
                  << 0x00000028U));
    }
    if (__VdlySet__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v6) {
        vlSelfRef.mosaic_ram_tb__DOT__u_bank_a__DOT__mem[__VdlyDim0__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v6] 
            = ((0xff00ffffffffffffULL & vlSelfRef.mosaic_ram_tb__DOT__u_bank_a__DOT__mem
                [__VdlyDim0__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v6]) 
               | ((QData)((IData)(__VdlyVal__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v6)) 
                  << 0x00000030U));
    }
    if (__VdlySet__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v7) {
        vlSelfRef.mosaic_ram_tb__DOT__u_bank_a__DOT__mem[__VdlyDim0__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v7] 
            = ((0x00ffffffffffffffULL & vlSelfRef.mosaic_ram_tb__DOT__u_bank_a__DOT__mem
                [__VdlyDim0__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v7]) 
               | ((QData)((IData)(__VdlyVal__mosaic_ram_tb__DOT__u_bank_a__DOT__mem__v7)) 
                  << 0x00000038U));
    }
}

void Vmosaic_ram_tb___024root___trigger_orInto__act_vec_vec(VlUnpacked<QData/*63:0*/, 1> &out, const VlUnpacked<QData/*63:0*/, 1> &in) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_ram_tb___024root___trigger_orInto__act_vec_vec\n"); );
    // Locals
    IData/*31:0*/ n;
    // Body
    n = 0U;
    do {
        out[n] = (out[n] | in[n]);
        n = ((IData)(1U) + n);
    } while ((0U >= n));
}

void Vmosaic_ram_tb___024root___trigger_clear__act(VlUnpacked<QData/*63:0*/, 1> &out) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_ram_tb___024root___trigger_clear__act\n"); );
    // Locals
    IData/*31:0*/ n;
    // Body
    n = 0U;
    do {
        out[n] = 0ULL;
        n = ((IData)(1U) + n);
    } while ((1U > n));
}

#ifdef VL_DEBUG
void Vmosaic_ram_tb___024root___eval_debug_assertions(Vmosaic_ram_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_ram_tb___024root___eval_debug_assertions\n"); );
    Vmosaic_ram_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    if (VL_UNLIKELY(((vlSelfRef.clk & 0xfeU)))) {
        Verilated::overWidthError("clk");
    }
    if (VL_UNLIKELY(((vlSelfRef.rst & 0xfeU)))) {
        Verilated::overWidthError("rst");
    }
    if (VL_UNLIKELY(((vlSelfRef.waddr & 0xc0U)))) {
        Verilated::overWidthError("waddr");
    }
    if (VL_UNLIKELY(((vlSelfRef.we & 0xfeU)))) {
        Verilated::overWidthError("we");
    }
    if (VL_UNLIKELY(((vlSelfRef.raddr & 0xc0U)))) {
        Verilated::overWidthError("raddr");
    }
}
#endif  // VL_DEBUG
