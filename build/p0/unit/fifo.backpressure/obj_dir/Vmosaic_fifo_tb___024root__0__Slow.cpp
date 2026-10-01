// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Design implementation internals
// See Vmosaic_fifo_tb.h for the primary calling header

#include "Vmosaic_fifo_tb__pch.h"

VL_ATTR_COLD void Vmosaic_fifo_tb___024root___eval_static(Vmosaic_fifo_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_fifo_tb___024root___eval_static\n"); );
    Vmosaic_fifo_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
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
    vlSelfRef.__Vtrigprevexpr___TOP__clk__1 = vlSelfRef.clk;
}

VL_ATTR_COLD void Vmosaic_fifo_tb___024root___eval_initial(Vmosaic_fifo_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_fifo_tb___024root___eval_initial\n"); );
    Vmosaic_fifo_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
}

#ifdef VL_DEBUG
VL_ATTR_COLD void Vmosaic_fifo_tb___024root___dump_triggers__stl(const VlUnpacked<QData/*63:0*/, 1> &triggers, const std::string &tag);
#endif  // VL_DEBUG
VL_ATTR_COLD bool Vmosaic_fifo_tb___024root___trigger_anySet__stl(const VlUnpacked<QData/*63:0*/, 1> &in);

VL_ATTR_COLD bool Vmosaic_fifo_tb___024root___eval_stl(Vmosaic_fifo_tb___024root* vlSelf, CData/*0:0*/ firstIteration) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_fifo_tb___024root___eval_stl\n"); );
    Vmosaic_fifo_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Locals
    CData/*0:0*/ __VstlExecute;
    // Body
    vlSelfRef.__VstlTriggered[0U] = ((0xfffffffffffffffeULL 
                                      & vlSelfRef.__VstlTriggered[0U]) 
                                     | (IData)((IData)(firstIteration)));
#ifdef VL_DEBUG
    if (VL_UNLIKELY(vlSymsp->_vm_contextp__->debug())) {
        Vmosaic_fifo_tb___024root___dump_triggers__stl(vlSelfRef.__VstlTriggered, "stl"s);
    }
#endif
    __VstlExecute = Vmosaic_fifo_tb___024root___trigger_anySet__stl(vlSelfRef.__VstlTriggered);
    if (__VstlExecute) {
        {
            // Inlined CFunc: _eval_body__stl
            if ((1ULL & vlSelfRef.__VstlTriggered[0U])) {
                {
                    // Inlined CFunc: _stl_sequent__TOP__0
                    vlSelfRef.d1_out_valid = vlSelfRef.d1_count;
                    vlSelfRef.mosaic_fifo_tb__DOT__u_d1__DOT__pop 
                        = ((IData)(vlSelfRef.d1_out_ready) 
                           & (IData)(vlSelfRef.d1_count));
                    vlSelfRef.d2_out_payload = vlSelfRef.mosaic_fifo_tb__DOT__u_d2__DOT__mem
                        [vlSelfRef.mosaic_fifo_tb__DOT__u_d2__DOT__rd_ptr];
                    vlSelfRef.d8_out_payload = vlSelfRef.mosaic_fifo_tb__DOT__u_d8__DOT__mem
                        [vlSelfRef.mosaic_fifo_tb__DOT__u_d8__DOT__rd_ptr];
                    vlSelfRef.sk_in_ready = (1U & (
                                                   (~ (IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_sk__DOT__valid_r)) 
                                                   | (IData)(vlSelfRef.sk_out_ready)));
                    vlSelfRef.sk_out_valid = ((IData)(vlSelfRef.sk_in_valid) 
                                              | (IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_sk__DOT__valid_r));
                    vlSelfRef.d1_out_payload = ((0U 
                                                 >= (IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_d1__DOT__rd_ptr))
                                                 ? vlSelfRef.mosaic_fifo_tb__DOT__u_d1__DOT__mem
                                                [vlSelfRef.mosaic_fifo_tb__DOT__u_d1__DOT__rd_ptr]
                                                 : vlSelfRef.mosaic_fifo_tb__DOT__u_d1__DOT____Vxrand___0);
                    vlSelfRef.d3_out_payload = ((2U 
                                                 >= (IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_d3__DOT__rd_ptr))
                                                 ? vlSelfRef.mosaic_fifo_tb__DOT__u_d3__DOT__mem
                                                [vlSelfRef.mosaic_fifo_tb__DOT__u_d3__DOT__rd_ptr]
                                                 : vlSelfRef.mosaic_fifo_tb__DOT__u_d3__DOT____Vxrand___0);
                    vlSelfRef.sk_out_payload = ((IData)(vlSelfRef.mosaic_fifo_tb__DOT__u_sk__DOT__valid_r)
                                                 ? vlSelfRef.mosaic_fifo_tb__DOT__u_sk__DOT__payload_r
                                                 : vlSelfRef.sk_in_payload);
                    vlSelfRef.d1_in_ready = (1U & (~ (IData)(vlSelfRef.d1_count)));
                    vlSelfRef.d2_in_ready = (2U != (IData)(vlSelfRef.d2_count));
                    vlSelfRef.d2_out_valid = (0U != (IData)(vlSelfRef.d2_count));
                    vlSelfRef.d3_in_ready = (3U != (IData)(vlSelfRef.d3_count));
                    vlSelfRef.d3_out_valid = (0U != (IData)(vlSelfRef.d3_count));
                    vlSelfRef.d8_in_ready = (8U != (IData)(vlSelfRef.d8_count));
                    vlSelfRef.d8_out_valid = (0U != (IData)(vlSelfRef.d8_count));
                    vlSelfRef.mosaic_fifo_tb__DOT__u_d1__DOT__push 
                        = ((IData)(vlSelfRef.d1_in_ready) 
                           & (IData)(vlSelfRef.d1_in_valid));
                    vlSelfRef.mosaic_fifo_tb__DOT__u_d2__DOT__push 
                        = ((IData)(vlSelfRef.d2_in_valid) 
                           & (IData)(vlSelfRef.d2_in_ready));
                    vlSelfRef.mosaic_fifo_tb__DOT__u_d2__DOT__pop 
                        = ((IData)(vlSelfRef.d2_out_ready) 
                           & (IData)(vlSelfRef.d2_out_valid));
                    vlSelfRef.mosaic_fifo_tb__DOT__u_d3__DOT__push 
                        = ((IData)(vlSelfRef.d3_in_valid) 
                           & (IData)(vlSelfRef.d3_in_ready));
                    vlSelfRef.mosaic_fifo_tb__DOT__u_d3__DOT__pop 
                        = ((IData)(vlSelfRef.d3_out_ready) 
                           & (IData)(vlSelfRef.d3_out_valid));
                    vlSelfRef.mosaic_fifo_tb__DOT__u_d8__DOT__push 
                        = ((IData)(vlSelfRef.d8_in_valid) 
                           & (IData)(vlSelfRef.d8_in_ready));
                    vlSelfRef.mosaic_fifo_tb__DOT__u_d8__DOT__pop 
                        = ((IData)(vlSelfRef.d8_out_ready) 
                           & (IData)(vlSelfRef.d8_out_valid));
                }
            }
        }
    }
    return (__VstlExecute);
}

VL_ATTR_COLD void Vmosaic_fifo_tb___024root___eval_dump_triggers__stl(Vmosaic_fifo_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_fifo_tb___024root___eval_dump_triggers__stl\n"); );
    Vmosaic_fifo_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
#ifdef VL_DEBUG
    Vmosaic_fifo_tb___024root___dump_triggers__stl(vlSelfRef.__VstlTriggered, "stl"s);
#endif
}

#ifdef VL_DEBUG
VL_ATTR_COLD void Vmosaic_fifo_tb___024root___dump_triggers__ico(const VlUnpacked<QData/*63:0*/, 2> &triggers, const std::string &tag);
#endif  // VL_DEBUG

VL_ATTR_COLD void Vmosaic_fifo_tb___024root___eval_dump_triggers__ico(Vmosaic_fifo_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_fifo_tb___024root___eval_dump_triggers__ico\n"); );
    Vmosaic_fifo_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
#ifdef VL_DEBUG
    Vmosaic_fifo_tb___024root___dump_triggers__ico(vlSelfRef.__VicoTriggered, "ico"s);
#endif
}

#ifdef VL_DEBUG
VL_ATTR_COLD void Vmosaic_fifo_tb___024root___dump_triggers__act(const VlUnpacked<QData/*63:0*/, 1> &triggers, const std::string &tag);
#endif  // VL_DEBUG

VL_ATTR_COLD void Vmosaic_fifo_tb___024root___eval_dump_triggers__act(Vmosaic_fifo_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_fifo_tb___024root___eval_dump_triggers__act\n"); );
    Vmosaic_fifo_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
#ifdef VL_DEBUG
    Vmosaic_fifo_tb___024root___dump_triggers__act(vlSelfRef.__VactTriggered, "act"s);
#endif
}

VL_ATTR_COLD void Vmosaic_fifo_tb___024root___eval_dump_triggers__nba(Vmosaic_fifo_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_fifo_tb___024root___eval_dump_triggers__nba\n"); );
    Vmosaic_fifo_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
#ifdef VL_DEBUG
    Vmosaic_fifo_tb___024root___dump_triggers__act(vlSelfRef.__VnbaTriggered, "nba"s);
#endif
}

VL_ATTR_COLD void Vmosaic_fifo_tb___024root___eval_dump_triggers__obs(Vmosaic_fifo_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_fifo_tb___024root___eval_dump_triggers__obs\n"); );
    Vmosaic_fifo_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
}

VL_ATTR_COLD void Vmosaic_fifo_tb___024root___eval_dump_triggers__react(Vmosaic_fifo_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_fifo_tb___024root___eval_dump_triggers__react\n"); );
    Vmosaic_fifo_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
}

VL_ATTR_COLD void Vmosaic_fifo_tb___024root___eval_final(Vmosaic_fifo_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_fifo_tb___024root___eval_final\n"); );
    Vmosaic_fifo_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
}

#ifdef VL_DEBUG
VL_ATTR_COLD void Vmosaic_fifo_tb___024root___dump_triggers__stl(const VlUnpacked<QData/*63:0*/, 1> &triggers, const std::string &tag) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_fifo_tb___024root___dump_triggers__stl\n"); );
    // Body
    if ((1U & (~ (IData)(Vmosaic_fifo_tb___024root___trigger_anySet__stl(triggers))))) {
        VL_DBG_MSGS("         No '" + tag + "' region triggers active\n");
    }
    if ((1U & (IData)(triggers[0U]))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 0 is active: Internal 'stl' trigger - first iteration\n");
    }
}
#endif  // VL_DEBUG

VL_ATTR_COLD bool Vmosaic_fifo_tb___024root___trigger_anySet__stl(const VlUnpacked<QData/*63:0*/, 1> &in) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_fifo_tb___024root___trigger_anySet__stl\n"); );
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

bool Vmosaic_fifo_tb___024root___trigger_anySet__ico(const VlUnpacked<QData/*63:0*/, 2> &in);

#ifdef VL_DEBUG
VL_ATTR_COLD void Vmosaic_fifo_tb___024root___dump_triggers__ico(const VlUnpacked<QData/*63:0*/, 2> &triggers, const std::string &tag) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_fifo_tb___024root___dump_triggers__ico\n"); );
    // Body
    if ((1U & (~ (IData)(Vmosaic_fifo_tb___024root___trigger_anySet__ico(triggers))))) {
        VL_DBG_MSGS("         No '" + tag + "' region triggers active\n");
    }
    if ((1U & (IData)(triggers[0U]))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 0 is active: @( clk)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 1U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 1 is active: @( rst)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 2U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 2 is active: @( d1_in_valid)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 3U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 3 is active: @( d1_in_payload)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 4U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 4 is active: @( d1_out_ready)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 5U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 5 is active: @( d2_in_valid)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 6U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 6 is active: @( d2_in_payload)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 7U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 7 is active: @( d2_out_ready)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 8U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 8 is active: @( d3_in_valid)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 9U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 9 is active: @( d3_in_payload)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 0x0000000aU)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 10 is active: @( d3_out_ready)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 0x0000000bU)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 11 is active: @( d8_in_valid)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 0x0000000cU)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 12 is active: @( d8_in_payload)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 0x0000000dU)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 13 is active: @( d8_out_ready)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 0x0000000eU)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 14 is active: @( sk_in_valid)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 0x0000000fU)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 15 is active: @( sk_in_payload)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 0x00000010U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 16 is active: @( sk_out_ready)\n");
    }
    if ((1U & (IData)(triggers[1U]))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 64 is active: Internal 'ico' trigger - first iteration\n");
    }
}
#endif  // VL_DEBUG

bool Vmosaic_fifo_tb___024root___trigger_anySet__act(const VlUnpacked<QData/*63:0*/, 1> &in);

#ifdef VL_DEBUG
VL_ATTR_COLD void Vmosaic_fifo_tb___024root___dump_triggers__act(const VlUnpacked<QData/*63:0*/, 1> &triggers, const std::string &tag) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_fifo_tb___024root___dump_triggers__act\n"); );
    // Body
    if ((1U & (~ (IData)(Vmosaic_fifo_tb___024root___trigger_anySet__act(triggers))))) {
        VL_DBG_MSGS("         No '" + tag + "' region triggers active\n");
    }
    if ((1U & (IData)(triggers[0U]))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 0 is active: @(posedge clk)\n");
    }
}
#endif  // VL_DEBUG

VL_ATTR_COLD void Vmosaic_fifo_tb___024root___ctor_var_reset(Vmosaic_fifo_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_fifo_tb___024root___ctor_var_reset\n"); );
    Vmosaic_fifo_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    const uint64_t __VscopeHash = VL_MURMUR64_HASH(vlSelf->vlNamep);
    vlSelf->clk = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 16707436170211756652ull);
    vlSelf->rst = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 18209466448985614591ull);
    vlSelf->d1_in_valid = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 12528657269590810593ull);
    vlSelf->d1_in_ready = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 16415316382158614305ull);
    vlSelf->d1_in_payload = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 11183122312741615376ull);
    vlSelf->d1_out_valid = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 4156530243108043181ull);
    vlSelf->d1_out_ready = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 761314945112091901ull);
    vlSelf->d1_out_payload = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 12415214424171545669ull);
    vlSelf->d1_count = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 2298387613605915038ull);
    vlSelf->d2_in_valid = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 543417870645796880ull);
    vlSelf->d2_in_ready = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 5225367777713757295ull);
    vlSelf->d2_in_payload = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 9658600090002738171ull);
    vlSelf->d2_out_valid = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 12606556747662391450ull);
    vlSelf->d2_out_ready = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 555408260223555111ull);
    vlSelf->d2_out_payload = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 2857097781210530285ull);
    vlSelf->d2_count = VL_SCOPED_RAND_RESET_I(2, __VscopeHash, 11874626515872234053ull);
    vlSelf->d3_in_valid = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 1748976562918653466ull);
    vlSelf->d3_in_ready = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 16150345388276971839ull);
    vlSelf->d3_in_payload = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 1113611239879317233ull);
    vlSelf->d3_out_valid = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 13957287317192320384ull);
    vlSelf->d3_out_ready = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 17986855220846074436ull);
    vlSelf->d3_out_payload = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 7384435569717949326ull);
    vlSelf->d3_count = VL_SCOPED_RAND_RESET_I(2, __VscopeHash, 6623799684617817503ull);
    vlSelf->d8_in_valid = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 15663748264189016368ull);
    vlSelf->d8_in_ready = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 11538998500697601880ull);
    vlSelf->d8_in_payload = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 5568488942236178277ull);
    vlSelf->d8_out_valid = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 1024518136123973393ull);
    vlSelf->d8_out_ready = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 8253071221299694665ull);
    vlSelf->d8_out_payload = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 12001985343653246329ull);
    vlSelf->d8_count = VL_SCOPED_RAND_RESET_I(4, __VscopeHash, 1558184647231393255ull);
    vlSelf->sk_in_valid = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 4746906769484220635ull);
    vlSelf->sk_in_ready = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 8410495931078859952ull);
    vlSelf->sk_in_payload = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 855175887428210920ull);
    vlSelf->sk_out_valid = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 13419163040102867224ull);
    vlSelf->sk_out_ready = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 10627857869189530885ull);
    vlSelf->sk_out_payload = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 4951538089339348630ull);
    vlSelf->mosaic_fifo_tb__DOT__u_sk__DOT__valid_r = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 11288213385235166411ull);
    vlSelf->mosaic_fifo_tb__DOT__u_sk__DOT__payload_r = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 9508425713593581540ull);
    for (int __Vi0 = 0; __Vi0 < 8; ++__Vi0) {
        vlSelf->mosaic_fifo_tb__DOT__u_d8__DOT__mem[__Vi0] = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 12193056819076420716ull);
    }
    vlSelf->mosaic_fifo_tb__DOT__u_d8__DOT__rd_ptr = VL_SCOPED_RAND_RESET_I(3, __VscopeHash, 9817298263667292639ull);
    vlSelf->mosaic_fifo_tb__DOT__u_d8__DOT__wr_ptr = VL_SCOPED_RAND_RESET_I(3, __VscopeHash, 1060753266561093860ull);
    vlSelf->mosaic_fifo_tb__DOT__u_d8__DOT__push = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 17663186388670862264ull);
    vlSelf->mosaic_fifo_tb__DOT__u_d8__DOT__pop = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 17284894338933185587ull);
    vlSelf->mosaic_fifo_tb__DOT__u_d3__DOT____Vxrand___0 = VL_SCOPED_RAND_RESET_ASSIGN_I(32, __VscopeHash, 6408819332412280799ull);
    for (int __Vi0 = 0; __Vi0 < 3; ++__Vi0) {
        vlSelf->mosaic_fifo_tb__DOT__u_d3__DOT__mem[__Vi0] = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 9791783663418300744ull);
    }
    vlSelf->mosaic_fifo_tb__DOT__u_d3__DOT__rd_ptr = VL_SCOPED_RAND_RESET_I(2, __VscopeHash, 13999738392495249954ull);
    vlSelf->mosaic_fifo_tb__DOT__u_d3__DOT__wr_ptr = VL_SCOPED_RAND_RESET_I(2, __VscopeHash, 4015660372498556779ull);
    vlSelf->mosaic_fifo_tb__DOT__u_d3__DOT__push = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 16837544802274739001ull);
    vlSelf->mosaic_fifo_tb__DOT__u_d3__DOT__pop = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 12619911720935371972ull);
    for (int __Vi0 = 0; __Vi0 < 2; ++__Vi0) {
        vlSelf->mosaic_fifo_tb__DOT__u_d2__DOT__mem[__Vi0] = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 15414033044582204152ull);
    }
    vlSelf->mosaic_fifo_tb__DOT__u_d2__DOT__rd_ptr = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 5042136445645117803ull);
    vlSelf->mosaic_fifo_tb__DOT__u_d2__DOT__wr_ptr = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 4318458100160264139ull);
    vlSelf->mosaic_fifo_tb__DOT__u_d2__DOT__push = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 5296891458575952883ull);
    vlSelf->mosaic_fifo_tb__DOT__u_d2__DOT__pop = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 7911707305944554514ull);
    vlSelf->mosaic_fifo_tb__DOT__u_d1__DOT____Vxrand___0 = VL_SCOPED_RAND_RESET_ASSIGN_I(32, __VscopeHash, 1501980108736433769ull);
    for (int __Vi0 = 0; __Vi0 < 1; ++__Vi0) {
        vlSelf->mosaic_fifo_tb__DOT__u_d1__DOT__mem[__Vi0] = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 8330199153802715900ull);
    }
    vlSelf->mosaic_fifo_tb__DOT__u_d1__DOT__rd_ptr = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 18309276669006258018ull);
    vlSelf->mosaic_fifo_tb__DOT__u_d1__DOT__wr_ptr = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 11994353698766200763ull);
    vlSelf->mosaic_fifo_tb__DOT__u_d1__DOT__push = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 13031662291720439408ull);
    vlSelf->mosaic_fifo_tb__DOT__u_d1__DOT__pop = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 1782747641067727891ull);
    for (int __Vi0 = 0; __Vi0 < 1; ++__Vi0) {
        vlSelf->__VstlTriggered[__Vi0] = 0;
    }
    for (int __Vi0 = 0; __Vi0 < 2; ++__Vi0) {
        vlSelf->__VicoTriggered[__Vi0] = 0;
    }
    vlSelf->__Vtrigprevexpr___TOP__clk__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__rst__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__d1_in_valid__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__d1_in_payload__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__d1_out_ready__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__d2_in_valid__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__d2_in_payload__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__d2_out_ready__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__d3_in_valid__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__d3_in_payload__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__d3_out_ready__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__d8_in_valid__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__d8_in_payload__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__d8_out_ready__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__sk_in_valid__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__sk_in_payload__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__sk_out_ready__0 = 0;
    vlSelf->__VicoDidInit = 0;
    for (int __Vi0 = 0; __Vi0 < 1; ++__Vi0) {
        vlSelf->__VactTriggered[__Vi0] = 0;
    }
    vlSelf->__Vtrigprevexpr___TOP__clk__1 = 0;
    for (int __Vi0 = 0; __Vi0 < 1; ++__Vi0) {
        vlSelf->__VnbaTriggered[__Vi0] = 0;
    }
}
