// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Design implementation internals
// See Vmosaic_fifo_tb.h for the primary calling header

#include "Vmosaic_fifo_tb__pch.h"

void Vmosaic_fifo_tb___024root___eval_sample(Vmosaic_fifo_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_fifo_tb___024root___eval_sample\n"); );
    Vmosaic_fifo_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
}

void Vmosaic_fifo_tb___024root___eval_triggers_vec__ico(Vmosaic_fifo_tb___024root* vlSelf);
#ifdef VL_DEBUG
VL_ATTR_COLD void Vmosaic_fifo_tb___024root___dump_triggers__ico(const VlUnpacked<QData/*63:0*/, 2> &triggers, const std::string &tag);
#endif  // VL_DEBUG
bool Vmosaic_fifo_tb___024root___trigger_anySet__ico(const VlUnpacked<QData/*63:0*/, 2> &in);

bool Vmosaic_fifo_tb___024root___eval_ico(Vmosaic_fifo_tb___024root* vlSelf, CData/*0:0*/ firstIteration) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_fifo_tb___024root___eval_ico\n"); );
    Vmosaic_fifo_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Locals
    CData/*0:0*/ __VicoExecute;
    // Body
    vlSelfRef.__VicoTriggered[1U] = ((0xfffffffffffffffeULL 
                                      & vlSelfRef.__VicoTriggered[1U]) 
                                     | (IData)((IData)(firstIteration)));
    Vmosaic_fifo_tb___024root___eval_triggers_vec__ico(vlSelf);
#ifdef VL_DEBUG
    if (VL_UNLIKELY(vlSymsp->_vm_contextp__->debug())) {
        Vmosaic_fifo_tb___024root___dump_triggers__ico(vlSelfRef.__VicoTriggered, "ico"s);
    }
#endif
    __VicoExecute = Vmosaic_fifo_tb___024root___trigger_anySet__ico(vlSelfRef.__VicoTriggered);
    if (__VicoExecute) {
        {
            // Inlined CFunc: _eval_body__ico
            if ((4ULL & vlSelfRef.__VicoTriggered[0U])) {
                {
                    // Inlined CFunc: _ico_sequent__TOP__0
                    vlSelfRef.mosaic_fifo_tb__DOT__u_d1__DOT__push 
                        = ((IData)(vlSelfRef.d1_in_ready) 
                           & (IData)(vlSelfRef.d1_in_valid));
                }
            }
            if ((0x0000000000000010ULL & vlSelfRef.__VicoTriggered[0U])) {
                {
                    // Inlined CFunc: _ico_sequent__TOP__1
                    vlSelfRef.mosaic_fifo_tb__DOT__u_d1__DOT__pop 
                        = ((IData)(vlSelfRef.d1_out_ready) 
                           & (IData)(vlSelfRef.d1_count));
                }
            }
            if ((0x0000000000000020ULL & vlSelfRef.__VicoTriggered[0U])) {
                {
                    // Inlined CFunc: _ico_sequent__TOP__2
                    vlSelfRef.mosaic_fifo_tb__DOT__u_d2__DOT__push 
                        = ((IData)(vlSelfRef.d2_in_valid) 
                           & (IData)(vlSelfRef.d2_in_ready));
                }
            }
            if ((0x0000000000000080ULL & vlSelfRef.__VicoTriggered[0U])) {
                {
                    // Inlined CFunc: _ico_sequent__TOP__3
                    vlSelfRef.mosaic_fifo_tb__DOT__u_d2__DOT__pop 
                        = ((IData)(vlSelfRef.d2_out_ready) 
                           & (IData)(vlSelfRef.d2_out_valid));
                }
            }
            if ((0x0000000000000100ULL & vlSelfRef.__VicoTriggered[0U])) {
                {
                    // Inlined CFunc: _ico_sequent__TOP__4
                    vlSelfRef.mosaic_fifo_tb__DOT__u_d3__DOT__push 
                        = ((IData)(vlSelfRef.d3_in_valid) 
                           & (IData)(vlSelfRef.d3_in_ready));
                }
            }
            if ((0x0000000000000400ULL & vlSelfRef.__VicoTriggered[0U])) {
                {
                    // Inlined CFunc: _ico_sequent__TOP__5
                    vlSelfRef.mosaic_fifo_tb__DOT__u_d3__DOT__pop 
                        = ((IData)(vlSelfRef.d3_out_ready) 
                           & (IData)(vlSelfRef.d3_out_valid));
                }
            }
            if ((0x0000000000000800ULL & vlSelfRef.__VicoTriggered[0U])) {
                {
                    // Inlined CFunc: _ico_sequent__TOP__6
                    vlSelfRef.mosaic_fifo_tb__DOT__u_d8__DOT__push 
                        = ((IData)(vlSelfRef.d8_in_valid) 
                           & (IData)(vlSelfRef.d8_in_ready));
                }
            }
            if ((0x0000000000002000ULL & vlSelfRef.__VicoTriggered[0U])) {
                {
                    // Inlined CFunc: _ico_sequent__TOP__7
                    vlSelfRef.mosaic_fifo_tb__DOT__u_d8__DOT__pop 
                        = ((IData)(vlSelfRef.d8_out_ready) 
                           & (IData)(vlSelfRef.d8_out_valid));
                }
            }
            if ((0x0000000000010000ULL & vlSelfRef.__VicoTriggered[0U])) {
                {
                    // Inlined CFunc: _ico_sequent__TOP__8
                    vlSelfRef.sk_in_ready = (1U & (
                                                   (~ (IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_sk__DOT__valid_r)) 
                                                   | (IData)(vlSelfRef.sk_out_ready)));
                }
            }
            if ((0x0000000000004000ULL & vlSelfRef.__VicoTriggered[0U])) {
                {
                    // Inlined CFunc: _ico_sequent__TOP__9
                    vlSelfRef.sk_out_valid = ((IData)(vlSelfRef.sk_in_valid) 
                                              | (IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_sk__DOT__valid_r));
                }
            }
            if ((0x0000000000008000ULL & vlSelfRef.__VicoTriggered[0U])) {
                {
                    // Inlined CFunc: _ico_sequent__TOP__10
                    vlSelfRef.sk_out_payload = ((IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_sk__DOT__valid_r)
                                                 ? vlSelfRef.mosaic_fifo_tb__DOT__u_sk__DOT__payload_r
                                                 : vlSelfRef.sk_in_payload);
                }
            }
        }
    }
    return (__VicoExecute);
}

#ifdef VL_DEBUG
VL_ATTR_COLD void Vmosaic_fifo_tb___024root___dump_triggers__act(const VlUnpacked<QData/*63:0*/, 1> &triggers, const std::string &tag);
#endif  // VL_DEBUG
void Vmosaic_fifo_tb___024root___trigger_orInto__act_vec_vec(VlUnpacked<QData/*63:0*/, 1> &out, const VlUnpacked<QData/*63:0*/, 1> &in);

bool Vmosaic_fifo_tb___024root___eval_act(Vmosaic_fifo_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_fifo_tb___024root___eval_act\n"); );
    Vmosaic_fifo_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
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
        Vmosaic_fifo_tb___024root___dump_triggers__act(vlSelfRef.__VactTriggered, "act"s);
    }
#endif
    Vmosaic_fifo_tb___024root___trigger_orInto__act_vec_vec(vlSelfRef.__VnbaTriggered, vlSelfRef.__VactTriggered);
    return (0U);
}

bool Vmosaic_fifo_tb___024root___eval_inact(Vmosaic_fifo_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_fifo_tb___024root___eval_inact\n"); );
    Vmosaic_fifo_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    return (0U);
}

bool Vmosaic_fifo_tb___024root___trigger_anySet__act(const VlUnpacked<QData/*63:0*/, 1> &in);
void Vmosaic_fifo_tb___024root___nba_sequent__TOP__0(Vmosaic_fifo_tb___024root* vlSelf);
void Vmosaic_fifo_tb___024root___trigger_clear__act(VlUnpacked<QData/*63:0*/, 1> &out);

bool Vmosaic_fifo_tb___024root___eval_nba(Vmosaic_fifo_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_fifo_tb___024root___eval_nba\n"); );
    Vmosaic_fifo_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Locals
    CData/*0:0*/ __VnbaExecute;
    // Body
    __VnbaExecute = Vmosaic_fifo_tb___024root___trigger_anySet__act(vlSelfRef.__VnbaTriggered);
    if (__VnbaExecute) {
        {
            // Inlined CFunc: _eval_body__nba
            if ((1ULL & vlSelfRef.__VnbaTriggered[0U])) {
                Vmosaic_fifo_tb___024root___nba_sequent__TOP__0(vlSelf);
            }
        }
        Vmosaic_fifo_tb___024root___trigger_clear__act(vlSelfRef.__VnbaTriggered);
    }
    return (__VnbaExecute);
}

bool Vmosaic_fifo_tb___024root___eval_obs(Vmosaic_fifo_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_fifo_tb___024root___eval_obs\n"); );
    Vmosaic_fifo_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    return (0U);
}

bool Vmosaic_fifo_tb___024root___eval_react(Vmosaic_fifo_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_fifo_tb___024root___eval_react\n"); );
    Vmosaic_fifo_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    return (0U);
}

void Vmosaic_fifo_tb___024root___eval_postponed(Vmosaic_fifo_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_fifo_tb___024root___eval_postponed\n"); );
    Vmosaic_fifo_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
}

void Vmosaic_fifo_tb___024root___eval_triggers_vec__ico(Vmosaic_fifo_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_fifo_tb___024root___eval_triggers_vec__ico\n"); );
    Vmosaic_fifo_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    vlSelfRef.__VicoTriggered[0U] = (QData)((IData)(
                                                    ((((IData)(vlSelfRef.sk_out_ready) 
                                                       != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__sk_out_ready__0)) 
                                                      << 0x00000010U) 
                                                     | (((((((vlSelfRef.sk_in_payload 
                                                              != vlSelfRef.__Vtrigprevexpr___TOP__sk_in_payload__0) 
                                                             << 3U) 
                                                            | (((IData)(vlSelfRef.sk_in_valid) 
                                                                != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__sk_in_valid__0)) 
                                                               << 2U)) 
                                                           | ((((IData)(vlSelfRef.d8_out_ready) 
                                                                != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__d8_out_ready__0)) 
                                                               << 1U) 
                                                              | (vlSelfRef.d8_in_payload 
                                                                 != vlSelfRef.__Vtrigprevexpr___TOP__d8_in_payload__0))) 
                                                          << 0x0000000cU) 
                                                         | ((((((IData)(vlSelfRef.d8_in_valid) 
                                                                != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__d8_in_valid__0)) 
                                                               << 3U) 
                                                              | (((IData)(vlSelfRef.d3_out_ready) 
                                                                  != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__d3_out_ready__0)) 
                                                                 << 2U)) 
                                                             | (((vlSelfRef.d3_in_payload 
                                                                  != vlSelfRef.__Vtrigprevexpr___TOP__d3_in_payload__0) 
                                                                 << 1U) 
                                                                | ((IData)(vlSelfRef.d3_in_valid) 
                                                                   != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__d3_in_valid__0)))) 
                                                            << 8U)) 
                                                        | (((((((IData)(vlSelfRef.d2_out_ready) 
                                                                != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__d2_out_ready__0)) 
                                                               << 3U) 
                                                              | ((vlSelfRef.d2_in_payload 
                                                                  != vlSelfRef.__Vtrigprevexpr___TOP__d2_in_payload__0) 
                                                                 << 2U)) 
                                                             | ((((IData)(vlSelfRef.d2_in_valid) 
                                                                  != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__d2_in_valid__0)) 
                                                                 << 1U) 
                                                                | ((IData)(vlSelfRef.d1_out_ready) 
                                                                   != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__d1_out_ready__0)))) 
                                                            << 4U) 
                                                           | ((((vlSelfRef.d1_in_payload 
                                                                 != vlSelfRef.__Vtrigprevexpr___TOP__d1_in_payload__0) 
                                                                << 3U) 
                                                               | (((IData)(vlSelfRef.d1_in_valid) 
                                                                   != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__d1_in_valid__0)) 
                                                                  << 2U)) 
                                                              | ((((IData)(vlSelfRef.rst) 
                                                                   != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__rst__0)) 
                                                                  << 1U) 
                                                                 | ((IData)(vlSelfRef.clk) 
                                                                    != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__clk__0)))))))));
    vlSelfRef.__Vtrigprevexpr___TOP__clk__0 = vlSelfRef.clk;
    vlSelfRef.__Vtrigprevexpr___TOP__rst__0 = vlSelfRef.rst;
    vlSelfRef.__Vtrigprevexpr___TOP__d1_in_valid__0 
        = vlSelfRef.d1_in_valid;
    vlSelfRef.__Vtrigprevexpr___TOP__d1_in_payload__0 
        = vlSelfRef.d1_in_payload;
    vlSelfRef.__Vtrigprevexpr___TOP__d1_out_ready__0 
        = vlSelfRef.d1_out_ready;
    vlSelfRef.__Vtrigprevexpr___TOP__d2_in_valid__0 
        = vlSelfRef.d2_in_valid;
    vlSelfRef.__Vtrigprevexpr___TOP__d2_in_payload__0 
        = vlSelfRef.d2_in_payload;
    vlSelfRef.__Vtrigprevexpr___TOP__d2_out_ready__0 
        = vlSelfRef.d2_out_ready;
    vlSelfRef.__Vtrigprevexpr___TOP__d3_in_valid__0 
        = vlSelfRef.d3_in_valid;
    vlSelfRef.__Vtrigprevexpr___TOP__d3_in_payload__0 
        = vlSelfRef.d3_in_payload;
    vlSelfRef.__Vtrigprevexpr___TOP__d3_out_ready__0 
        = vlSelfRef.d3_out_ready;
    vlSelfRef.__Vtrigprevexpr___TOP__d8_in_valid__0 
        = vlSelfRef.d8_in_valid;
    vlSelfRef.__Vtrigprevexpr___TOP__d8_in_payload__0 
        = vlSelfRef.d8_in_payload;
    vlSelfRef.__Vtrigprevexpr___TOP__d8_out_ready__0 
        = vlSelfRef.d8_out_ready;
    vlSelfRef.__Vtrigprevexpr___TOP__sk_in_valid__0 
        = vlSelfRef.sk_in_valid;
    vlSelfRef.__Vtrigprevexpr___TOP__sk_in_payload__0 
        = vlSelfRef.sk_in_payload;
    vlSelfRef.__Vtrigprevexpr___TOP__sk_out_ready__0 
        = vlSelfRef.sk_out_ready;
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
        vlSelfRef.__VicoTriggered[0U] = (0x0000000000000080ULL 
                                         | vlSelfRef.__VicoTriggered[0U]);
        vlSelfRef.__VicoTriggered[0U] = (0x0000000000000100ULL 
                                         | vlSelfRef.__VicoTriggered[0U]);
        vlSelfRef.__VicoTriggered[0U] = (0x0000000000000200ULL 
                                         | vlSelfRef.__VicoTriggered[0U]);
        vlSelfRef.__VicoTriggered[0U] = (0x0000000000000400ULL 
                                         | vlSelfRef.__VicoTriggered[0U]);
        vlSelfRef.__VicoTriggered[0U] = (0x0000000000000800ULL 
                                         | vlSelfRef.__VicoTriggered[0U]);
        vlSelfRef.__VicoTriggered[0U] = (0x0000000000001000ULL 
                                         | vlSelfRef.__VicoTriggered[0U]);
        vlSelfRef.__VicoTriggered[0U] = (0x0000000000002000ULL 
                                         | vlSelfRef.__VicoTriggered[0U]);
        vlSelfRef.__VicoTriggered[0U] = (0x0000000000004000ULL 
                                         | vlSelfRef.__VicoTriggered[0U]);
        vlSelfRef.__VicoTriggered[0U] = (0x0000000000008000ULL 
                                         | vlSelfRef.__VicoTriggered[0U]);
        vlSelfRef.__VicoTriggered[0U] = (0x0000000000010000ULL 
                                         | vlSelfRef.__VicoTriggered[0U]);
    }
}

bool Vmosaic_fifo_tb___024root___trigger_anySet__ico(const VlUnpacked<QData/*63:0*/, 2> &in) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_fifo_tb___024root___trigger_anySet__ico\n"); );
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

bool Vmosaic_fifo_tb___024root___trigger_anySet__act(const VlUnpacked<QData/*63:0*/, 1> &in) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_fifo_tb___024root___trigger_anySet__act\n"); );
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

void Vmosaic_fifo_tb___024root___nba_sequent__TOP__0(Vmosaic_fifo_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_fifo_tb___024root___nba_sequent__TOP__0\n"); );
    Vmosaic_fifo_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Locals
    CData/*0:0*/ __Vdly__mosaic_fifo_tb__DOT__u_sk__DOT__valid_r;
    __Vdly__mosaic_fifo_tb__DOT__u_sk__DOT__valid_r = 0;
    CData/*2:0*/ __Vdly__mosaic_fifo_tb__DOT__u_d8__DOT__rd_ptr;
    __Vdly__mosaic_fifo_tb__DOT__u_d8__DOT__rd_ptr = 0;
    CData/*3:0*/ __Vdly__d8_count;
    __Vdly__d8_count = 0;
    CData/*2:0*/ __Vdly__mosaic_fifo_tb__DOT__u_d8__DOT__wr_ptr;
    __Vdly__mosaic_fifo_tb__DOT__u_d8__DOT__wr_ptr = 0;
    CData/*1:0*/ __Vdly__mosaic_fifo_tb__DOT__u_d3__DOT__rd_ptr;
    __Vdly__mosaic_fifo_tb__DOT__u_d3__DOT__rd_ptr = 0;
    CData/*1:0*/ __Vdly__d3_count;
    __Vdly__d3_count = 0;
    CData/*1:0*/ __Vdly__mosaic_fifo_tb__DOT__u_d3__DOT__wr_ptr;
    __Vdly__mosaic_fifo_tb__DOT__u_d3__DOT__wr_ptr = 0;
    CData/*0:0*/ __Vdly__mosaic_fifo_tb__DOT__u_d2__DOT__rd_ptr;
    __Vdly__mosaic_fifo_tb__DOT__u_d2__DOT__rd_ptr = 0;
    CData/*1:0*/ __Vdly__d2_count;
    __Vdly__d2_count = 0;
    CData/*0:0*/ __Vdly__mosaic_fifo_tb__DOT__u_d2__DOT__wr_ptr;
    __Vdly__mosaic_fifo_tb__DOT__u_d2__DOT__wr_ptr = 0;
    CData/*0:0*/ __Vdly__mosaic_fifo_tb__DOT__u_d1__DOT__rd_ptr;
    __Vdly__mosaic_fifo_tb__DOT__u_d1__DOT__rd_ptr = 0;
    CData/*0:0*/ __Vdly__d1_count;
    __Vdly__d1_count = 0;
    CData/*0:0*/ __Vdly__mosaic_fifo_tb__DOT__u_d1__DOT__wr_ptr;
    __Vdly__mosaic_fifo_tb__DOT__u_d1__DOT__wr_ptr = 0;
    IData/*31:0*/ __VdlyVal__mosaic_fifo_tb__DOT__u_d8__DOT__mem__v0;
    __VdlyVal__mosaic_fifo_tb__DOT__u_d8__DOT__mem__v0 = 0;
    CData/*2:0*/ __VdlyDim0__mosaic_fifo_tb__DOT__u_d8__DOT__mem__v0;
    __VdlyDim0__mosaic_fifo_tb__DOT__u_d8__DOT__mem__v0 = 0;
    CData/*0:0*/ __VdlySet__mosaic_fifo_tb__DOT__u_d8__DOT__mem__v0;
    __VdlySet__mosaic_fifo_tb__DOT__u_d8__DOT__mem__v0 = 0;
    IData/*31:0*/ __VdlyVal__mosaic_fifo_tb__DOT__u_d3__DOT__mem__v0;
    __VdlyVal__mosaic_fifo_tb__DOT__u_d3__DOT__mem__v0 = 0;
    CData/*1:0*/ __VdlyDim0__mosaic_fifo_tb__DOT__u_d3__DOT__mem__v0;
    __VdlyDim0__mosaic_fifo_tb__DOT__u_d3__DOT__mem__v0 = 0;
    CData/*0:0*/ __VdlySet__mosaic_fifo_tb__DOT__u_d3__DOT__mem__v0;
    __VdlySet__mosaic_fifo_tb__DOT__u_d3__DOT__mem__v0 = 0;
    IData/*31:0*/ __VdlyVal__mosaic_fifo_tb__DOT__u_d2__DOT__mem__v0;
    __VdlyVal__mosaic_fifo_tb__DOT__u_d2__DOT__mem__v0 = 0;
    CData/*0:0*/ __VdlyDim0__mosaic_fifo_tb__DOT__u_d2__DOT__mem__v0;
    __VdlyDim0__mosaic_fifo_tb__DOT__u_d2__DOT__mem__v0 = 0;
    CData/*0:0*/ __VdlySet__mosaic_fifo_tb__DOT__u_d2__DOT__mem__v0;
    __VdlySet__mosaic_fifo_tb__DOT__u_d2__DOT__mem__v0 = 0;
    IData/*31:0*/ __VdlyVal__mosaic_fifo_tb__DOT__u_d1__DOT__mem__v0;
    __VdlyVal__mosaic_fifo_tb__DOT__u_d1__DOT__mem__v0 = 0;
    CData/*0:0*/ __VdlyDim0__mosaic_fifo_tb__DOT__u_d1__DOT__mem__v0;
    __VdlyDim0__mosaic_fifo_tb__DOT__u_d1__DOT__mem__v0 = 0;
    CData/*0:0*/ __VdlySet__mosaic_fifo_tb__DOT__u_d1__DOT__mem__v0;
    __VdlySet__mosaic_fifo_tb__DOT__u_d1__DOT__mem__v0 = 0;
    // Body
    __Vdly__mosaic_fifo_tb__DOT__u_d8__DOT__rd_ptr 
        = vlSelfRef.mosaic_fifo_tb__DOT__u_d8__DOT__rd_ptr;
    __Vdly__mosaic_fifo_tb__DOT__u_d2__DOT__rd_ptr 
        = vlSelfRef.mosaic_fifo_tb__DOT__u_d2__DOT__rd_ptr;
    __Vdly__mosaic_fifo_tb__DOT__u_d3__DOT__rd_ptr 
        = vlSelfRef.mosaic_fifo_tb__DOT__u_d3__DOT__rd_ptr;
    __Vdly__mosaic_fifo_tb__DOT__u_d1__DOT__rd_ptr 
        = vlSelfRef.mosaic_fifo_tb__DOT__u_d1__DOT__rd_ptr;
    __Vdly__mosaic_fifo_tb__DOT__u_d8__DOT__wr_ptr 
        = vlSelfRef.mosaic_fifo_tb__DOT__u_d8__DOT__wr_ptr;
    __Vdly__mosaic_fifo_tb__DOT__u_d2__DOT__wr_ptr 
        = vlSelfRef.mosaic_fifo_tb__DOT__u_d2__DOT__wr_ptr;
    __Vdly__mosaic_fifo_tb__DOT__u_d3__DOT__wr_ptr 
        = vlSelfRef.mosaic_fifo_tb__DOT__u_d3__DOT__wr_ptr;
    __Vdly__mosaic_fifo_tb__DOT__u_d1__DOT__wr_ptr 
        = vlSelfRef.mosaic_fifo_tb__DOT__u_d1__DOT__wr_ptr;
    __VdlySet__mosaic_fifo_tb__DOT__u_d8__DOT__mem__v0 = 0U;
    __VdlySet__mosaic_fifo_tb__DOT__u_d2__DOT__mem__v0 = 0U;
    __VdlySet__mosaic_fifo_tb__DOT__u_d3__DOT__mem__v0 = 0U;
    __VdlySet__mosaic_fifo_tb__DOT__u_d1__DOT__mem__v0 = 0U;
    __Vdly__mosaic_fifo_tb__DOT__u_sk__DOT__valid_r 
        = vlSelfRef.mosaic_fifo_tb__DOT__u_sk__DOT__valid_r;
    __Vdly__d8_count = vlSelfRef.d8_count;
    __Vdly__d3_count = vlSelfRef.d3_count;
    __Vdly__d2_count = vlSelfRef.d2_count;
    __Vdly__d1_count = vlSelfRef.d1_count;
    if (vlSelfRef.rst) {
        __Vdly__mosaic_fifo_tb__DOT__u_d8__DOT__rd_ptr = 0U;
        __Vdly__mosaic_fifo_tb__DOT__u_d2__DOT__rd_ptr = 0U;
        __Vdly__mosaic_fifo_tb__DOT__u_d3__DOT__rd_ptr = 0U;
        __Vdly__mosaic_fifo_tb__DOT__u_d1__DOT__rd_ptr = 0U;
        __Vdly__mosaic_fifo_tb__DOT__u_d8__DOT__wr_ptr = 0U;
        __Vdly__mosaic_fifo_tb__DOT__u_d2__DOT__wr_ptr = 0U;
        __Vdly__mosaic_fifo_tb__DOT__u_d3__DOT__wr_ptr = 0U;
        __Vdly__mosaic_fifo_tb__DOT__u_d1__DOT__wr_ptr = 0U;
        __Vdly__d8_count = 0U;
        __Vdly__d3_count = 0U;
        __Vdly__d2_count = 0U;
        __Vdly__d1_count = 0U;
        __Vdly__mosaic_fifo_tb__DOT__u_sk__DOT__valid_r = 0U;
    } else {
        if (vlSelfRef.mosaic_fifo_tb__DOT__u_d8__DOT__pop) {
            __Vdly__mosaic_fifo_tb__DOT__u_d8__DOT__rd_ptr 
                = ((7U == (IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_d8__DOT__rd_ptr))
                    ? 0U : (7U & ((IData)(1U) + (IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_d8__DOT__rd_ptr))));
        }
        if (vlSelfRef.mosaic_fifo_tb__DOT__u_d2__DOT__pop) {
            __Vdly__mosaic_fifo_tb__DOT__u_d2__DOT__rd_ptr 
                = (1U & ((~ (IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_d2__DOT__rd_ptr)) 
                         & ((IData)(1U) + (IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_d2__DOT__rd_ptr))));
        }
        if (vlSelfRef.mosaic_fifo_tb__DOT__u_d3__DOT__pop) {
            __Vdly__mosaic_fifo_tb__DOT__u_d3__DOT__rd_ptr 
                = ((2U == (IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_d3__DOT__rd_ptr))
                    ? 0U : (3U & ((IData)(1U) + (IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_d3__DOT__rd_ptr))));
        }
        if (vlSelfRef.mosaic_fifo_tb__DOT__u_d1__DOT__pop) {
            __Vdly__mosaic_fifo_tb__DOT__u_d1__DOT__rd_ptr 
                = ((IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_d1__DOT__rd_ptr) 
                   & ((IData)(1U) + (IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_d1__DOT__rd_ptr)));
        }
        if (vlSelfRef.mosaic_fifo_tb__DOT__u_d8__DOT__push) {
            __VdlyVal__mosaic_fifo_tb__DOT__u_d8__DOT__mem__v0 
                = vlSelfRef.d8_in_payload;
            __VdlyDim0__mosaic_fifo_tb__DOT__u_d8__DOT__mem__v0 
                = vlSelfRef.mosaic_fifo_tb__DOT__u_d8__DOT__wr_ptr;
            __VdlySet__mosaic_fifo_tb__DOT__u_d8__DOT__mem__v0 = 1U;
            __Vdly__mosaic_fifo_tb__DOT__u_d8__DOT__wr_ptr 
                = ((7U == (IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_d8__DOT__wr_ptr))
                    ? 0U : (7U & ((IData)(1U) + (IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_d8__DOT__wr_ptr))));
        }
        if (vlSelfRef.mosaic_fifo_tb__DOT__u_d2__DOT__push) {
            __VdlyVal__mosaic_fifo_tb__DOT__u_d2__DOT__mem__v0 
                = vlSelfRef.d2_in_payload;
            __VdlyDim0__mosaic_fifo_tb__DOT__u_d2__DOT__mem__v0 
                = vlSelfRef.mosaic_fifo_tb__DOT__u_d2__DOT__wr_ptr;
            __VdlySet__mosaic_fifo_tb__DOT__u_d2__DOT__mem__v0 = 1U;
            __Vdly__mosaic_fifo_tb__DOT__u_d2__DOT__wr_ptr 
                = (1U & ((~ (IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_d2__DOT__wr_ptr)) 
                         & ((IData)(1U) + (IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_d2__DOT__wr_ptr))));
        }
        if (vlSelfRef.mosaic_fifo_tb__DOT__u_d3__DOT__push) {
            if ((2U >= (IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_d3__DOT__wr_ptr))) {
                __VdlyVal__mosaic_fifo_tb__DOT__u_d3__DOT__mem__v0 
                    = vlSelfRef.d3_in_payload;
                __VdlyDim0__mosaic_fifo_tb__DOT__u_d3__DOT__mem__v0 
                    = vlSelfRef.mosaic_fifo_tb__DOT__u_d3__DOT__wr_ptr;
                __VdlySet__mosaic_fifo_tb__DOT__u_d3__DOT__mem__v0 = 1U;
            }
            __Vdly__mosaic_fifo_tb__DOT__u_d3__DOT__wr_ptr 
                = ((2U == (IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_d3__DOT__wr_ptr))
                    ? 0U : (3U & ((IData)(1U) + (IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_d3__DOT__wr_ptr))));
        }
        if (vlSelfRef.mosaic_fifo_tb__DOT__u_d1__DOT__push) {
            if ((0U >= (IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_d1__DOT__wr_ptr))) {
                __VdlyVal__mosaic_fifo_tb__DOT__u_d1__DOT__mem__v0 
                    = vlSelfRef.d1_in_payload;
                __VdlyDim0__mosaic_fifo_tb__DOT__u_d1__DOT__mem__v0 
                    = vlSelfRef.mosaic_fifo_tb__DOT__u_d1__DOT__wr_ptr;
                __VdlySet__mosaic_fifo_tb__DOT__u_d1__DOT__mem__v0 = 1U;
            }
            __Vdly__mosaic_fifo_tb__DOT__u_d1__DOT__wr_ptr 
                = ((IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_d1__DOT__wr_ptr) 
                   & ((IData)(1U) + (IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_d1__DOT__wr_ptr)));
        }
        if (((IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_d8__DOT__push) 
             & (~ (IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_d8__DOT__pop)))) {
            __Vdly__d8_count = (0x0000000fU & ((IData)(1U) 
                                               + (IData)(vlSelfRef.d8_count)));
        } else if (((~ (IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_d8__DOT__push)) 
                    & (IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_d8__DOT__pop))) {
            __Vdly__d8_count = (0x0000000fU & ((IData)(vlSelfRef.d8_count) 
                                               - (IData)(1U)));
        }
        if (((IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_d3__DOT__push) 
             & (~ (IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_d3__DOT__pop)))) {
            __Vdly__d3_count = (3U & ((IData)(1U) + (IData)(vlSelfRef.d3_count)));
        } else if (((~ (IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_d3__DOT__push)) 
                    & (IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_d3__DOT__pop))) {
            __Vdly__d3_count = (3U & ((IData)(vlSelfRef.d3_count) 
                                      - (IData)(1U)));
        }
        if (((IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_d2__DOT__push) 
             & (~ (IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_d2__DOT__pop)))) {
            __Vdly__d2_count = (3U & ((IData)(1U) + (IData)(vlSelfRef.d2_count)));
        } else if (((~ (IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_d2__DOT__push)) 
                    & (IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_d2__DOT__pop))) {
            __Vdly__d2_count = (3U & ((IData)(vlSelfRef.d2_count) 
                                      - (IData)(1U)));
        }
        if (((IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_d1__DOT__push) 
             & (~ (IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_d1__DOT__pop)))) {
            __Vdly__d1_count = (1U & ((IData)(1U) + (IData)(vlSelfRef.d1_count)));
        } else if (((~ (IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_d1__DOT__push)) 
                    & (IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_d1__DOT__pop))) {
            __Vdly__d1_count = (1U & ((IData)(vlSelfRef.d1_count) 
                                      - (IData)(1U)));
        }
        if (vlSelfRef.sk_out_ready) {
            if (((IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_sk__DOT__valid_r) 
                 & (IData)(vlSelfRef.sk_in_valid))) {
                __Vdly__mosaic_fifo_tb__DOT__u_sk__DOT__valid_r = 1U;
                vlSelfRef.mosaic_fifo_tb__DOT__u_sk__DOT__payload_r 
                    = vlSelfRef.sk_in_payload;
            } else {
                __Vdly__mosaic_fifo_tb__DOT__u_sk__DOT__valid_r = 0U;
            }
        } else {
            __Vdly__mosaic_fifo_tb__DOT__u_sk__DOT__valid_r 
                = ((IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_sk__DOT__valid_r) 
                   | (IData)(vlSelfRef.sk_in_valid));
            if (((~ (IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_sk__DOT__valid_r)) 
                 & (IData)(vlSelfRef.sk_in_valid))) {
                vlSelfRef.mosaic_fifo_tb__DOT__u_sk__DOT__payload_r 
                    = vlSelfRef.sk_in_payload;
            }
        }
    }
    vlSelfRef.mosaic_fifo_tb__DOT__u_d8__DOT__rd_ptr 
        = __Vdly__mosaic_fifo_tb__DOT__u_d8__DOT__rd_ptr;
    vlSelfRef.mosaic_fifo_tb__DOT__u_d2__DOT__rd_ptr 
        = __Vdly__mosaic_fifo_tb__DOT__u_d2__DOT__rd_ptr;
    vlSelfRef.mosaic_fifo_tb__DOT__u_d3__DOT__rd_ptr 
        = __Vdly__mosaic_fifo_tb__DOT__u_d3__DOT__rd_ptr;
    vlSelfRef.mosaic_fifo_tb__DOT__u_d1__DOT__rd_ptr 
        = __Vdly__mosaic_fifo_tb__DOT__u_d1__DOT__rd_ptr;
    vlSelfRef.mosaic_fifo_tb__DOT__u_d8__DOT__wr_ptr 
        = __Vdly__mosaic_fifo_tb__DOT__u_d8__DOT__wr_ptr;
    if (__VdlySet__mosaic_fifo_tb__DOT__u_d8__DOT__mem__v0) {
        vlSelfRef.mosaic_fifo_tb__DOT__u_d8__DOT__mem[__VdlyDim0__mosaic_fifo_tb__DOT__u_d8__DOT__mem__v0] 
            = __VdlyVal__mosaic_fifo_tb__DOT__u_d8__DOT__mem__v0;
    }
    vlSelfRef.mosaic_fifo_tb__DOT__u_d2__DOT__wr_ptr 
        = __Vdly__mosaic_fifo_tb__DOT__u_d2__DOT__wr_ptr;
    if (__VdlySet__mosaic_fifo_tb__DOT__u_d2__DOT__mem__v0) {
        vlSelfRef.mosaic_fifo_tb__DOT__u_d2__DOT__mem[__VdlyDim0__mosaic_fifo_tb__DOT__u_d2__DOT__mem__v0] 
            = __VdlyVal__mosaic_fifo_tb__DOT__u_d2__DOT__mem__v0;
    }
    vlSelfRef.mosaic_fifo_tb__DOT__u_d3__DOT__wr_ptr 
        = __Vdly__mosaic_fifo_tb__DOT__u_d3__DOT__wr_ptr;
    if (__VdlySet__mosaic_fifo_tb__DOT__u_d3__DOT__mem__v0) {
        vlSelfRef.mosaic_fifo_tb__DOT__u_d3__DOT__mem[__VdlyDim0__mosaic_fifo_tb__DOT__u_d3__DOT__mem__v0] 
            = __VdlyVal__mosaic_fifo_tb__DOT__u_d3__DOT__mem__v0;
    }
    vlSelfRef.mosaic_fifo_tb__DOT__u_d1__DOT__wr_ptr 
        = __Vdly__mosaic_fifo_tb__DOT__u_d1__DOT__wr_ptr;
    if (__VdlySet__mosaic_fifo_tb__DOT__u_d1__DOT__mem__v0) {
        vlSelfRef.mosaic_fifo_tb__DOT__u_d1__DOT__mem[__VdlyDim0__mosaic_fifo_tb__DOT__u_d1__DOT__mem__v0] 
            = __VdlyVal__mosaic_fifo_tb__DOT__u_d1__DOT__mem__v0;
    }
    vlSelfRef.d8_count = __Vdly__d8_count;
    vlSelfRef.d3_count = __Vdly__d3_count;
    vlSelfRef.d2_count = __Vdly__d2_count;
    vlSelfRef.d1_count = __Vdly__d1_count;
    vlSelfRef.mosaic_fifo_tb__DOT__u_sk__DOT__valid_r 
        = __Vdly__mosaic_fifo_tb__DOT__u_sk__DOT__valid_r;
    vlSelfRef.d8_out_payload = vlSelfRef.mosaic_fifo_tb__DOT__u_d8__DOT__mem
        [vlSelfRef.mosaic_fifo_tb__DOT__u_d8__DOT__rd_ptr];
    vlSelfRef.d2_out_payload = vlSelfRef.mosaic_fifo_tb__DOT__u_d2__DOT__mem
        [vlSelfRef.mosaic_fifo_tb__DOT__u_d2__DOT__rd_ptr];
    vlSelfRef.d3_out_payload = ((2U >= (IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_d3__DOT__rd_ptr))
                                 ? vlSelfRef.mosaic_fifo_tb__DOT__u_d3__DOT__mem
                                [vlSelfRef.mosaic_fifo_tb__DOT__u_d3__DOT__rd_ptr]
                                 : vlSelfRef.mosaic_fifo_tb__DOT__u_d3__DOT____Vxrand___0);
    vlSelfRef.d1_out_payload = ((0U >= (IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_d1__DOT__rd_ptr))
                                 ? vlSelfRef.mosaic_fifo_tb__DOT__u_d1__DOT__mem
                                [vlSelfRef.mosaic_fifo_tb__DOT__u_d1__DOT__rd_ptr]
                                 : vlSelfRef.mosaic_fifo_tb__DOT__u_d1__DOT____Vxrand___0);
    vlSelfRef.d8_in_ready = (8U != (IData)(vlSelfRef.d8_count));
    vlSelfRef.d8_out_valid = (0U != (IData)(vlSelfRef.d8_count));
    vlSelfRef.d3_in_ready = (3U != (IData)(vlSelfRef.d3_count));
    vlSelfRef.d3_out_valid = (0U != (IData)(vlSelfRef.d3_count));
    vlSelfRef.d2_in_ready = (2U != (IData)(vlSelfRef.d2_count));
    vlSelfRef.d2_out_valid = (0U != (IData)(vlSelfRef.d2_count));
    vlSelfRef.d1_out_valid = vlSelfRef.d1_count;
    vlSelfRef.mosaic_fifo_tb__DOT__u_d1__DOT__pop = 
        ((IData)(vlSelfRef.d1_out_ready) & (IData)(vlSelfRef.d1_count));
    vlSelfRef.d1_in_ready = (1U & (~ (IData)(vlSelfRef.d1_count)));
    vlSelfRef.sk_in_ready = (1U & ((~ (IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_sk__DOT__valid_r)) 
                                   | (IData)(vlSelfRef.sk_out_ready)));
    vlSelfRef.sk_out_valid = ((IData)(vlSelfRef.sk_in_valid) 
                              | (IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_sk__DOT__valid_r));
    vlSelfRef.sk_out_payload = ((IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_sk__DOT__valid_r)
                                 ? vlSelfRef.mosaic_fifo_tb__DOT__u_sk__DOT__payload_r
                                 : vlSelfRef.sk_in_payload);
    vlSelfRef.mosaic_fifo_tb__DOT__u_d8__DOT__push 
        = ((IData)(vlSelfRef.d8_in_valid) & (IData)(vlSelfRef.d8_in_ready));
    vlSelfRef.mosaic_fifo_tb__DOT__u_d8__DOT__pop = 
        ((IData)(vlSelfRef.d8_out_ready) & (IData)(vlSelfRef.d8_out_valid));
    vlSelfRef.mosaic_fifo_tb__DOT__u_d3__DOT__push 
        = ((IData)(vlSelfRef.d3_in_valid) & (IData)(vlSelfRef.d3_in_ready));
    vlSelfRef.mosaic_fifo_tb__DOT__u_d3__DOT__pop = 
        ((IData)(vlSelfRef.d3_out_ready) & (IData)(vlSelfRef.d3_out_valid));
    vlSelfRef.mosaic_fifo_tb__DOT__u_d2__DOT__push 
        = ((IData)(vlSelfRef.d2_in_valid) & (IData)(vlSelfRef.d2_in_ready));
    vlSelfRef.mosaic_fifo_tb__DOT__u_d2__DOT__pop = 
        ((IData)(vlSelfRef.d2_out_ready) & (IData)(vlSelfRef.d2_out_valid));
    vlSelfRef.mosaic_fifo_tb__DOT__u_d1__DOT__push 
        = ((IData)(vlSelfRef.d1_in_ready) & (IData)(vlSelfRef.d1_in_valid));
}

void Vmosaic_fifo_tb___024root___trigger_orInto__act_vec_vec(VlUnpacked<QData/*63:0*/, 1> &out, const VlUnpacked<QData/*63:0*/, 1> &in) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_fifo_tb___024root___trigger_orInto__act_vec_vec\n"); );
    // Locals
    IData/*31:0*/ n;
    // Body
    n = 0U;
    do {
        out[n] = (out[n] | in[n]);
        n = ((IData)(1U) + n);
    } while ((0U >= n));
}

void Vmosaic_fifo_tb___024root___trigger_clear__act(VlUnpacked<QData/*63:0*/, 1> &out) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_fifo_tb___024root___trigger_clear__act\n"); );
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
void Vmosaic_fifo_tb___024root___eval_debug_assertions(Vmosaic_fifo_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_fifo_tb___024root___eval_debug_assertions\n"); );
    Vmosaic_fifo_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    if (VL_UNLIKELY(((vlSelfRef.clk & 0xfeU)))) {
        Verilated::overWidthError("clk");
    }
    if (VL_UNLIKELY(((vlSelfRef.rst & 0xfeU)))) {
        Verilated::overWidthError("rst");
    }
    if (VL_UNLIKELY(((vlSelfRef.d1_in_valid & 0xfeU)))) {
        Verilated::overWidthError("d1_in_valid");
    }
    if (VL_UNLIKELY(((vlSelfRef.d1_out_ready & 0xfeU)))) {
        Verilated::overWidthError("d1_out_ready");
    }
    if (VL_UNLIKELY(((vlSelfRef.d2_in_valid & 0xfeU)))) {
        Verilated::overWidthError("d2_in_valid");
    }
    if (VL_UNLIKELY(((vlSelfRef.d2_out_ready & 0xfeU)))) {
        Verilated::overWidthError("d2_out_ready");
    }
    if (VL_UNLIKELY(((vlSelfRef.d3_in_valid & 0xfeU)))) {
        Verilated::overWidthError("d3_in_valid");
    }
    if (VL_UNLIKELY(((vlSelfRef.d3_out_ready & 0xfeU)))) {
        Verilated::overWidthError("d3_out_ready");
    }
    if (VL_UNLIKELY(((vlSelfRef.d8_in_valid & 0xfeU)))) {
        Verilated::overWidthError("d8_in_valid");
    }
    if (VL_UNLIKELY(((vlSelfRef.d8_out_ready & 0xfeU)))) {
        Verilated::overWidthError("d8_out_ready");
    }
    if (VL_UNLIKELY(((vlSelfRef.sk_in_valid & 0xfeU)))) {
        Verilated::overWidthError("sk_in_valid");
    }
    if (VL_UNLIKELY(((vlSelfRef.sk_out_ready & 0xfeU)))) {
        Verilated::overWidthError("sk_out_ready");
    }
}
#endif  // VL_DEBUG
