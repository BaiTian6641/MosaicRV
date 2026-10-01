// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Design implementation internals
// See Vmosaic_ram_tb.h for the primary calling header

#include "Vmosaic_ram_tb__pch.h"

VL_ATTR_COLD void Vmosaic_ram_tb___024root___eval_static(Vmosaic_ram_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_ram_tb___024root___eval_static\n"); );
    Vmosaic_ram_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    vlSelfRef.__Vtrigprevexpr___TOP__clk__0 = vlSelfRef.clk;
    vlSelfRef.__Vtrigprevexpr___TOP__rst__0 = vlSelfRef.rst;
    vlSelfRef.__Vtrigprevexpr___TOP__waddr__0 = vlSelfRef.waddr;
    vlSelfRef.__Vtrigprevexpr___TOP__wdata__0 = vlSelfRef.wdata;
    vlSelfRef.__Vtrigprevexpr___TOP__wmask__0 = vlSelfRef.wmask;
    vlSelfRef.__Vtrigprevexpr___TOP__we__0 = vlSelfRef.we;
    vlSelfRef.__Vtrigprevexpr___TOP__raddr__0 = vlSelfRef.raddr;
    vlSelfRef.__Vtrigprevexpr___TOP__clk__1 = vlSelfRef.clk;
}

VL_ATTR_COLD void Vmosaic_ram_tb___024root___eval_initial(Vmosaic_ram_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_ram_tb___024root___eval_initial\n"); );
    Vmosaic_ram_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
}

#ifdef VL_DEBUG
VL_ATTR_COLD void Vmosaic_ram_tb___024root___dump_triggers__stl(const VlUnpacked<QData/*63:0*/, 1> &triggers, const std::string &tag);
#endif  // VL_DEBUG

VL_ATTR_COLD bool Vmosaic_ram_tb___024root___eval_stl(Vmosaic_ram_tb___024root* vlSelf, CData/*0:0*/ firstIteration) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_ram_tb___024root___eval_stl\n"); );
    Vmosaic_ram_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    vlSelfRef.__VstlTriggered[0U] = ((0xfffffffffffffffeULL 
                                      & vlSelfRef.__VstlTriggered[0U]) 
                                     | (IData)((IData)(firstIteration)));
#ifdef VL_DEBUG
    if (VL_UNLIKELY(vlSymsp->_vm_contextp__->debug())) {
        Vmosaic_ram_tb___024root___dump_triggers__stl(vlSelfRef.__VstlTriggered, "stl"s);
    }
#endif
    return (0U);
}

VL_ATTR_COLD void Vmosaic_ram_tb___024root___eval_dump_triggers__stl(Vmosaic_ram_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_ram_tb___024root___eval_dump_triggers__stl\n"); );
    Vmosaic_ram_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
#ifdef VL_DEBUG
    Vmosaic_ram_tb___024root___dump_triggers__stl(vlSelfRef.__VstlTriggered, "stl"s);
#endif
}

#ifdef VL_DEBUG
VL_ATTR_COLD void Vmosaic_ram_tb___024root___dump_triggers__ico(const VlUnpacked<QData/*63:0*/, 2> &triggers, const std::string &tag);
#endif  // VL_DEBUG

VL_ATTR_COLD void Vmosaic_ram_tb___024root___eval_dump_triggers__ico(Vmosaic_ram_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_ram_tb___024root___eval_dump_triggers__ico\n"); );
    Vmosaic_ram_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
#ifdef VL_DEBUG
    Vmosaic_ram_tb___024root___dump_triggers__ico(vlSelfRef.__VicoTriggered, "ico"s);
#endif
}

#ifdef VL_DEBUG
VL_ATTR_COLD void Vmosaic_ram_tb___024root___dump_triggers__act(const VlUnpacked<QData/*63:0*/, 1> &triggers, const std::string &tag);
#endif  // VL_DEBUG

VL_ATTR_COLD void Vmosaic_ram_tb___024root___eval_dump_triggers__act(Vmosaic_ram_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_ram_tb___024root___eval_dump_triggers__act\n"); );
    Vmosaic_ram_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
#ifdef VL_DEBUG
    Vmosaic_ram_tb___024root___dump_triggers__act(vlSelfRef.__VactTriggered, "act"s);
#endif
}

VL_ATTR_COLD void Vmosaic_ram_tb___024root___eval_dump_triggers__nba(Vmosaic_ram_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_ram_tb___024root___eval_dump_triggers__nba\n"); );
    Vmosaic_ram_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
#ifdef VL_DEBUG
    Vmosaic_ram_tb___024root___dump_triggers__act(vlSelfRef.__VnbaTriggered, "nba"s);
#endif
}

VL_ATTR_COLD void Vmosaic_ram_tb___024root___eval_dump_triggers__obs(Vmosaic_ram_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_ram_tb___024root___eval_dump_triggers__obs\n"); );
    Vmosaic_ram_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
}

VL_ATTR_COLD void Vmosaic_ram_tb___024root___eval_dump_triggers__react(Vmosaic_ram_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_ram_tb___024root___eval_dump_triggers__react\n"); );
    Vmosaic_ram_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
}

VL_ATTR_COLD void Vmosaic_ram_tb___024root___eval_final(Vmosaic_ram_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_ram_tb___024root___eval_final\n"); );
    Vmosaic_ram_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
}

VL_ATTR_COLD bool Vmosaic_ram_tb___024root___trigger_anySet__stl(const VlUnpacked<QData/*63:0*/, 1> &in);

#ifdef VL_DEBUG
VL_ATTR_COLD void Vmosaic_ram_tb___024root___dump_triggers__stl(const VlUnpacked<QData/*63:0*/, 1> &triggers, const std::string &tag) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_ram_tb___024root___dump_triggers__stl\n"); );
    // Body
    if ((1U & (~ (IData)(Vmosaic_ram_tb___024root___trigger_anySet__stl(triggers))))) {
        VL_DBG_MSGS("         No '" + tag + "' region triggers active\n");
    }
    if ((1U & (IData)(triggers[0U]))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 0 is active: Internal 'stl' trigger - first iteration\n");
    }
}
#endif  // VL_DEBUG

VL_ATTR_COLD bool Vmosaic_ram_tb___024root___trigger_anySet__stl(const VlUnpacked<QData/*63:0*/, 1> &in) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_ram_tb___024root___trigger_anySet__stl\n"); );
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

bool Vmosaic_ram_tb___024root___trigger_anySet__ico(const VlUnpacked<QData/*63:0*/, 2> &in);

#ifdef VL_DEBUG
VL_ATTR_COLD void Vmosaic_ram_tb___024root___dump_triggers__ico(const VlUnpacked<QData/*63:0*/, 2> &triggers, const std::string &tag) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_ram_tb___024root___dump_triggers__ico\n"); );
    // Body
    if ((1U & (~ (IData)(Vmosaic_ram_tb___024root___trigger_anySet__ico(triggers))))) {
        VL_DBG_MSGS("         No '" + tag + "' region triggers active\n");
    }
    if ((1U & (IData)(triggers[0U]))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 0 is active: @( clk)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 1U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 1 is active: @( rst)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 2U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 2 is active: @( waddr)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 3U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 3 is active: @( wdata)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 4U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 4 is active: @( wmask)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 5U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 5 is active: @( we)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 6U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 6 is active: @( raddr)\n");
    }
    if ((1U & (IData)(triggers[1U]))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 64 is active: Internal 'ico' trigger - first iteration\n");
    }
}
#endif  // VL_DEBUG

bool Vmosaic_ram_tb___024root___trigger_anySet__act(const VlUnpacked<QData/*63:0*/, 1> &in);

#ifdef VL_DEBUG
VL_ATTR_COLD void Vmosaic_ram_tb___024root___dump_triggers__act(const VlUnpacked<QData/*63:0*/, 1> &triggers, const std::string &tag) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_ram_tb___024root___dump_triggers__act\n"); );
    // Body
    if ((1U & (~ (IData)(Vmosaic_ram_tb___024root___trigger_anySet__act(triggers))))) {
        VL_DBG_MSGS("         No '" + tag + "' region triggers active\n");
    }
    if ((1U & (IData)(triggers[0U]))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 0 is active: @(posedge clk)\n");
    }
}
#endif  // VL_DEBUG

VL_ATTR_COLD void Vmosaic_ram_tb___024root___ctor_var_reset(Vmosaic_ram_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_ram_tb___024root___ctor_var_reset\n"); );
    Vmosaic_ram_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    const uint64_t __VscopeHash = VL_MURMUR64_HASH(vlSelf->vlNamep);
    vlSelf->clk = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 16707436170211756652ull);
    vlSelf->rst = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 18209466448985614591ull);
    vlSelf->waddr = VL_SCOPED_RAND_RESET_I(6, __VscopeHash, 15530376126285439671ull);
    vlSelf->wdata = VL_SCOPED_RAND_RESET_Q(64, __VscopeHash, 12890271867161903902ull);
    vlSelf->wmask = VL_SCOPED_RAND_RESET_I(8, __VscopeHash, 7533683342978896906ull);
    vlSelf->we = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 10105644630884274164ull);
    vlSelf->raddr = VL_SCOPED_RAND_RESET_I(6, __VscopeHash, 12822808926477430924ull);
    vlSelf->rdata_a = VL_SCOPED_RAND_RESET_Q(64, __VscopeHash, 2234507492688033197ull);
    vlSelf->rdata_b = VL_SCOPED_RAND_RESET_Q(64, __VscopeHash, 3918670664674893561ull);
    vlSelf->rvalid_a = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 5904547454715271604ull);
    vlSelf->rvalid_b = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 16909356717269440672ull);
    for (int __Vi0 = 0; __Vi0 < 64; ++__Vi0) {
        vlSelf->mosaic_ram_tb__DOT__u_bank_b__DOT__mem[__Vi0] = VL_SCOPED_RAND_RESET_Q(64, __VscopeHash, 9173009534824930506ull);
    }
    for (int __Vi0 = 0; __Vi0 < 64; ++__Vi0) {
        vlSelf->mosaic_ram_tb__DOT__u_bank_a__DOT__mem[__Vi0] = VL_SCOPED_RAND_RESET_Q(64, __VscopeHash, 2499081095259584846ull);
    }
    for (int __Vi0 = 0; __Vi0 < 1; ++__Vi0) {
        vlSelf->__VstlTriggered[__Vi0] = 0;
    }
    for (int __Vi0 = 0; __Vi0 < 2; ++__Vi0) {
        vlSelf->__VicoTriggered[__Vi0] = 0;
    }
    vlSelf->__Vtrigprevexpr___TOP__clk__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__rst__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__waddr__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__wdata__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__wmask__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__we__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__raddr__0 = 0;
    vlSelf->__VicoDidInit = 0;
    for (int __Vi0 = 0; __Vi0 < 1; ++__Vi0) {
        vlSelf->__VactTriggered[__Vi0] = 0;
    }
    vlSelf->__Vtrigprevexpr___TOP__clk__1 = 0;
    for (int __Vi0 = 0; __Vi0 < 1; ++__Vi0) {
        vlSelf->__VnbaTriggered[__Vi0] = 0;
    }
}
