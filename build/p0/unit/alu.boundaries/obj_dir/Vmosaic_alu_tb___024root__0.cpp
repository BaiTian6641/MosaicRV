// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Design implementation internals
// See Vmosaic_alu_tb.h for the primary calling header

#include "Vmosaic_alu_tb__pch.h"

void Vmosaic_alu_tb___024root___eval_sample(Vmosaic_alu_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_alu_tb___024root___eval_sample\n"); );
    Vmosaic_alu_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
}

#ifdef VL_DEBUG
VL_ATTR_COLD void Vmosaic_alu_tb___024root___dump_triggers__ico(const VlUnpacked<QData/*63:0*/, 2> &triggers, const std::string &tag);
#endif  // VL_DEBUG
bool Vmosaic_alu_tb___024root___trigger_anySet__ico(const VlUnpacked<QData/*63:0*/, 2> &in);
void Vmosaic_alu_tb___024root___eval_body__ico(Vmosaic_alu_tb___024root* vlSelf);

bool Vmosaic_alu_tb___024root___eval_ico(Vmosaic_alu_tb___024root* vlSelf, CData/*0:0*/ firstIteration) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_alu_tb___024root___eval_ico\n"); );
    Vmosaic_alu_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Locals
    CData/*0:0*/ __VicoExecute;
    // Body
    vlSelfRef.__VicoTriggered[1U] = ((0xfffffffffffffffeULL 
                                      & vlSelfRef.__VicoTriggered[1U]) 
                                     | (IData)((IData)(firstIteration)));
    {
        // Inlined CFunc: _eval_triggers_vec__ico
        vlSelfRef.__VicoTriggered[0U] = (QData)((IData)(
                                                        ((((IData)(vlSelfRef.op) 
                                                           != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__op__0)) 
                                                          << 2U) 
                                                         | (((vlSelfRef.b 
                                                              != vlSelfRef.__Vtrigprevexpr___TOP__b__0) 
                                                             << 1U) 
                                                            | (vlSelfRef.a 
                                                               != vlSelfRef.__Vtrigprevexpr___TOP__a__0)))));
        vlSelfRef.__Vtrigprevexpr___TOP__a__0 = vlSelfRef.a;
        vlSelfRef.__Vtrigprevexpr___TOP__b__0 = vlSelfRef.b;
        vlSelfRef.__Vtrigprevexpr___TOP__op__0 = vlSelfRef.op;
        if (VL_UNLIKELY(((1U & (~ (IData)(vlSelfRef.__VicoDidInit)))))) {
            vlSelfRef.__VicoDidInit = 1U;
            vlSelfRef.__VicoTriggered[0U] = (1ULL | vlSelfRef.__VicoTriggered[0U]);
            vlSelfRef.__VicoTriggered[0U] = (2ULL | vlSelfRef.__VicoTriggered[0U]);
            vlSelfRef.__VicoTriggered[0U] = (4ULL | vlSelfRef.__VicoTriggered[0U]);
        }
    }
#ifdef VL_DEBUG
    if (VL_UNLIKELY(vlSymsp->_vm_contextp__->debug())) {
        Vmosaic_alu_tb___024root___dump_triggers__ico(vlSelfRef.__VicoTriggered, "ico"s);
    }
#endif
    __VicoExecute = Vmosaic_alu_tb___024root___trigger_anySet__ico(vlSelfRef.__VicoTriggered);
    if (__VicoExecute) {
        Vmosaic_alu_tb___024root___eval_body__ico(vlSelf);
    }
    return (__VicoExecute);
}

bool Vmosaic_alu_tb___024root___eval_act(Vmosaic_alu_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_alu_tb___024root___eval_act\n"); );
    Vmosaic_alu_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    return (0U);
}

bool Vmosaic_alu_tb___024root___eval_inact(Vmosaic_alu_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_alu_tb___024root___eval_inact\n"); );
    Vmosaic_alu_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    return (0U);
}

bool Vmosaic_alu_tb___024root___eval_nba(Vmosaic_alu_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_alu_tb___024root___eval_nba\n"); );
    Vmosaic_alu_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    return (0U);
}

bool Vmosaic_alu_tb___024root___eval_obs(Vmosaic_alu_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_alu_tb___024root___eval_obs\n"); );
    Vmosaic_alu_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    return (0U);
}

bool Vmosaic_alu_tb___024root___eval_react(Vmosaic_alu_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_alu_tb___024root___eval_react\n"); );
    Vmosaic_alu_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    return (0U);
}

void Vmosaic_alu_tb___024root___eval_postponed(Vmosaic_alu_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_alu_tb___024root___eval_postponed\n"); );
    Vmosaic_alu_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
}

bool Vmosaic_alu_tb___024root___trigger_anySet__ico(const VlUnpacked<QData/*63:0*/, 2> &in) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_alu_tb___024root___trigger_anySet__ico\n"); );
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

void Vmosaic_alu_tb___024root___eval_body__ico(Vmosaic_alu_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_alu_tb___024root___eval_body__ico\n"); );
    Vmosaic_alu_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    if ((3ULL & vlSelfRef.__VicoTriggered[0U])) {
        {
            // Inlined CFunc: _ico_comb__TOP__0
            vlSelfRef.__VdfgRegularize_hebeb780c_0_0 
                = ((IData)(vlSelfRef.a) + (IData)(vlSelfRef.b));
            vlSelfRef.__VdfgRegularize_hebeb780c_0_1 
                = ((IData)(vlSelfRef.a) - (IData)(vlSelfRef.b));
            vlSelfRef.__VdfgRegularize_hebeb780c_0_2 
                = ((IData)(vlSelfRef.a) << (0x0000001fU 
                                            & (IData)(vlSelfRef.b)));
            vlSelfRef.__VdfgRegularize_hebeb780c_0_3 
                = ((IData)(vlSelfRef.a) >> (0x0000001fU 
                                            & (IData)(vlSelfRef.b)));
            vlSelfRef.__VdfgRegularize_hebeb780c_0_4 
                = VL_SHIFTRS_III(32,32,5, (IData)(vlSelfRef.a), 
                                 (0x0000001fU & (IData)(vlSelfRef.b)));
        }
    }
    if ((7ULL & vlSelfRef.__VicoTriggered[0U])) {
        {
            // Inlined CFunc: _ico_comb__TOP__1
            vlSelfRef.result = ((8U & (IData)(vlSelfRef.op))
                                 ? ((4U & (IData)(vlSelfRef.op))
                                     ? ((2U & (IData)(vlSelfRef.op))
                                         ? ((1U & (IData)(vlSelfRef.op))
                                             ? vlSelfRef.b
                                             : (((QData)((IData)(
                                                                 (- (IData)(
                                                                            (vlSelfRef.__VdfgRegularize_hebeb780c_0_4 
                                                                             >> 0x0000001fU))))) 
                                                 << 0x00000020U) 
                                                | (QData)((IData)(vlSelfRef.__VdfgRegularize_hebeb780c_0_4))))
                                         : ((1U & (IData)(vlSelfRef.op))
                                             ? (((QData)((IData)(
                                                                 (- (IData)(
                                                                            (vlSelfRef.__VdfgRegularize_hebeb780c_0_3 
                                                                             >> 0x0000001fU))))) 
                                                 << 0x00000020U) 
                                                | (QData)((IData)(vlSelfRef.__VdfgRegularize_hebeb780c_0_3)))
                                             : (((QData)((IData)(
                                                                 (- (IData)(
                                                                            (vlSelfRef.__VdfgRegularize_hebeb780c_0_2 
                                                                             >> 0x0000001fU))))) 
                                                 << 0x00000020U) 
                                                | (QData)((IData)(vlSelfRef.__VdfgRegularize_hebeb780c_0_2)))))
                                     : ((2U & (IData)(vlSelfRef.op))
                                         ? ((1U & (IData)(vlSelfRef.op))
                                             ? (((QData)((IData)(
                                                                 (- (IData)(
                                                                            (vlSelfRef.__VdfgRegularize_hebeb780c_0_1 
                                                                             >> 0x0000001fU))))) 
                                                 << 0x00000020U) 
                                                | (QData)((IData)(vlSelfRef.__VdfgRegularize_hebeb780c_0_1)))
                                             : (((QData)((IData)(
                                                                 (- (IData)(
                                                                            (vlSelfRef.__VdfgRegularize_hebeb780c_0_0 
                                                                             >> 0x0000001fU))))) 
                                                 << 0x00000020U) 
                                                | (QData)((IData)(vlSelfRef.__VdfgRegularize_hebeb780c_0_0))))
                                         : ((1U & (IData)(vlSelfRef.op))
                                             ? (vlSelfRef.a 
                                                & vlSelfRef.b)
                                             : (vlSelfRef.a 
                                                | vlSelfRef.b))))
                                 : ((4U & (IData)(vlSelfRef.op))
                                     ? ((2U & (IData)(vlSelfRef.op))
                                         ? ((1U & (IData)(vlSelfRef.op))
                                             ? VL_SHIFTRS_QQI(64,64,6, vlSelfRef.a, 
                                                              (0x0000003fU 
                                                               & (IData)(vlSelfRef.b)))
                                             : (vlSelfRef.a 
                                                >> 
                                                (0x0000003fU 
                                                 & (IData)(vlSelfRef.b))))
                                         : ((1U & (IData)(vlSelfRef.op))
                                             ? (vlSelfRef.a 
                                                ^ vlSelfRef.b)
                                             : (1ULL 
                                                & (- (QData)((IData)(
                                                                     (vlSelfRef.a 
                                                                      < vlSelfRef.b)))))))
                                     : ((2U & (IData)(vlSelfRef.op))
                                         ? ((1U & (IData)(vlSelfRef.op))
                                             ? (1ULL 
                                                & (- (QData)((IData)(
                                                                     VL_LTS_IQQ(64, vlSelfRef.a, vlSelfRef.b)))))
                                             : (vlSelfRef.a 
                                                << 
                                                (0x0000003fU 
                                                 & (IData)(vlSelfRef.b))))
                                         : ((1U & (IData)(vlSelfRef.op))
                                             ? (vlSelfRef.a 
                                                - vlSelfRef.b)
                                             : (vlSelfRef.a 
                                                + vlSelfRef.b)))));
            vlSelfRef.zero = (0ULL == vlSelfRef.result);
        }
    }
}

#ifdef VL_DEBUG
void Vmosaic_alu_tb___024root___eval_debug_assertions(Vmosaic_alu_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_alu_tb___024root___eval_debug_assertions\n"); );
    Vmosaic_alu_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    if (VL_UNLIKELY(((vlSelfRef.op & 0xf0U)))) {
        Verilated::overWidthError("op");
    }
}
#endif  // VL_DEBUG
