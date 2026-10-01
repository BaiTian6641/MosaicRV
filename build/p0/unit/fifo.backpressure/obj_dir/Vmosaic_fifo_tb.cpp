// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Model implementation (design independent parts)

#include "Vmosaic_fifo_tb__pch.h"

//============================================================
// Constructors

Vmosaic_fifo_tb::Vmosaic_fifo_tb(VerilatedContext* _vcontextp__, const char* _vcname__)
    : VerilatedModel{*_vcontextp__}
    , vlSymsp{new Vmosaic_fifo_tb__Syms(contextp(), _vcname__, this)}
    , m_evalLoop{*this, /*convergeLimit:*/ 10000}
    , clk{vlSymsp->TOP.clk}
    , rst{vlSymsp->TOP.rst}
    , d1_in_valid{vlSymsp->TOP.d1_in_valid}
    , d1_in_ready{vlSymsp->TOP.d1_in_ready}
    , d1_out_valid{vlSymsp->TOP.d1_out_valid}
    , d1_out_ready{vlSymsp->TOP.d1_out_ready}
    , d1_count{vlSymsp->TOP.d1_count}
    , d2_in_valid{vlSymsp->TOP.d2_in_valid}
    , d2_in_ready{vlSymsp->TOP.d2_in_ready}
    , d2_out_valid{vlSymsp->TOP.d2_out_valid}
    , d2_out_ready{vlSymsp->TOP.d2_out_ready}
    , d2_count{vlSymsp->TOP.d2_count}
    , d3_in_valid{vlSymsp->TOP.d3_in_valid}
    , d3_in_ready{vlSymsp->TOP.d3_in_ready}
    , d3_out_valid{vlSymsp->TOP.d3_out_valid}
    , d3_out_ready{vlSymsp->TOP.d3_out_ready}
    , d3_count{vlSymsp->TOP.d3_count}
    , d8_in_valid{vlSymsp->TOP.d8_in_valid}
    , d8_in_ready{vlSymsp->TOP.d8_in_ready}
    , d8_out_valid{vlSymsp->TOP.d8_out_valid}
    , d8_out_ready{vlSymsp->TOP.d8_out_ready}
    , d8_count{vlSymsp->TOP.d8_count}
    , sk_in_valid{vlSymsp->TOP.sk_in_valid}
    , sk_in_ready{vlSymsp->TOP.sk_in_ready}
    , sk_out_valid{vlSymsp->TOP.sk_out_valid}
    , sk_out_ready{vlSymsp->TOP.sk_out_ready}
    , d1_in_payload{vlSymsp->TOP.d1_in_payload}
    , d1_out_payload{vlSymsp->TOP.d1_out_payload}
    , d2_in_payload{vlSymsp->TOP.d2_in_payload}
    , d2_out_payload{vlSymsp->TOP.d2_out_payload}
    , d3_in_payload{vlSymsp->TOP.d3_in_payload}
    , d3_out_payload{vlSymsp->TOP.d3_out_payload}
    , d8_in_payload{vlSymsp->TOP.d8_in_payload}
    , d8_out_payload{vlSymsp->TOP.d8_out_payload}
    , sk_in_payload{vlSymsp->TOP.sk_in_payload}
    , sk_out_payload{vlSymsp->TOP.sk_out_payload}
    , rootp{&(vlSymsp->TOP)}
{
    // Register model with the context
    contextp()->addModel(this);
}

Vmosaic_fifo_tb::Vmosaic_fifo_tb(const char* _vcname__)
    : Vmosaic_fifo_tb(Verilated::threadContextp(), _vcname__)
{
}

//============================================================
// Destructor

Vmosaic_fifo_tb::~Vmosaic_fifo_tb() {
    delete vlSymsp;
}

//============================================================
// Evaluation function

#ifdef VL_DEBUG
void Vmosaic_fifo_tb___024root___eval_debug_assertions(Vmosaic_fifo_tb___024root* vlSelf);
#endif  // VL_DEBUG
VL_ATTR_COLD void Vmosaic_fifo_tb___024root___eval_static(Vmosaic_fifo_tb___024root* vlSelf);
VL_ATTR_COLD void Vmosaic_fifo_tb___024root___eval_initial(Vmosaic_fifo_tb___024root* vlSelf);
VL_ATTR_COLD bool Vmosaic_fifo_tb___024root___eval_stl(Vmosaic_fifo_tb___024root* vlSelf, CData/*0:0*/ firstIteration);
void Vmosaic_fifo_tb___024root___eval_sample(Vmosaic_fifo_tb___024root* vlSelf);
bool Vmosaic_fifo_tb___024root___eval_ico(Vmosaic_fifo_tb___024root* vlSelf, CData/*0:0*/ firstIteration);
bool Vmosaic_fifo_tb___024root___eval_act(Vmosaic_fifo_tb___024root* vlSelf);
bool Vmosaic_fifo_tb___024root___eval_inact(Vmosaic_fifo_tb___024root* vlSelf);
bool Vmosaic_fifo_tb___024root___eval_nba(Vmosaic_fifo_tb___024root* vlSelf);
bool Vmosaic_fifo_tb___024root___eval_obs(Vmosaic_fifo_tb___024root* vlSelf);
bool Vmosaic_fifo_tb___024root___eval_react(Vmosaic_fifo_tb___024root* vlSelf);
void Vmosaic_fifo_tb___024root___eval_postponed(Vmosaic_fifo_tb___024root* vlSelf);
VL_ATTR_COLD void Vmosaic_fifo_tb___024root___eval_final(Vmosaic_fifo_tb___024root* vlSelf);
VL_ATTR_COLD void Vmosaic_fifo_tb___024root___eval_dump_triggers__stl(Vmosaic_fifo_tb___024root* vlSelf);
VL_ATTR_COLD void Vmosaic_fifo_tb___024root___eval_dump_triggers__ico(Vmosaic_fifo_tb___024root* vlSelf);
VL_ATTR_COLD void Vmosaic_fifo_tb___024root___eval_dump_triggers__act(Vmosaic_fifo_tb___024root* vlSelf);
VL_ATTR_COLD void Vmosaic_fifo_tb___024root___eval_dump_triggers__nba(Vmosaic_fifo_tb___024root* vlSelf);
VL_ATTR_COLD void Vmosaic_fifo_tb___024root___eval_dump_triggers__obs(Vmosaic_fifo_tb___024root* vlSelf);
VL_ATTR_COLD void Vmosaic_fifo_tb___024root___eval_dump_triggers__react(Vmosaic_fifo_tb___024root* vlSelf);

void Vmosaic_fifo_tb::eval_step() {
    VL_DEBUG_IF(VL_DBG_MSGF("+++++TOP Evaluate Vmosaic_fifo_tb::eval_step\n"); );
    m_evalLoop.eval();
}

void Vmosaic_fifo_tb::evalBegin() {
#ifdef VL_DEBUG
    // Debug assertions
    Vmosaic_fifo_tb___024root___eval_debug_assertions(&(vlSymsp->TOP));
#endif  // VL_DEBUG
    vlSymsp->__Vm_deleter.deleteAll();
}

void Vmosaic_fifo_tb::evalEnd() {
    // Evaluate cleanup
    Verilated::endOfEval(vlSymsp->__Vm_evalMsgQp);
}

void Vmosaic_fifo_tb::evalStatic() {
    Vmosaic_fifo_tb___024root___eval_static(&(vlSymsp->TOP));
}

void Vmosaic_fifo_tb::evalInitial() {
    Vmosaic_fifo_tb___024root___eval_initial(&(vlSymsp->TOP));
}

bool Vmosaic_fifo_tb::evalStl(bool firstIteration) {
    return Vmosaic_fifo_tb___024root___eval_stl(&(vlSymsp->TOP), firstIteration);
}

void Vmosaic_fifo_tb::evalSample() {
    Vmosaic_fifo_tb___024root___eval_sample(&(vlSymsp->TOP));
}

bool Vmosaic_fifo_tb::evalIco(bool firstIteration) {
    return Vmosaic_fifo_tb___024root___eval_ico(&(vlSymsp->TOP), firstIteration);
}

bool Vmosaic_fifo_tb::evalAct() {
    return Vmosaic_fifo_tb___024root___eval_act(&(vlSymsp->TOP));
}

bool Vmosaic_fifo_tb::evalInact() {
    return Vmosaic_fifo_tb___024root___eval_inact(&(vlSymsp->TOP));
}

bool Vmosaic_fifo_tb::evalNba() {
    return Vmosaic_fifo_tb___024root___eval_nba(&(vlSymsp->TOP));
}

bool Vmosaic_fifo_tb::evalObs() {
    return Vmosaic_fifo_tb___024root___eval_obs(&(vlSymsp->TOP));
}

bool Vmosaic_fifo_tb::evalReact() {
    return Vmosaic_fifo_tb___024root___eval_react(&(vlSymsp->TOP));
}

void Vmosaic_fifo_tb::evalPostponed() {
    Vmosaic_fifo_tb___024root___eval_postponed(&(vlSymsp->TOP));
}

void Vmosaic_fifo_tb::evalFinal() {
    Vmosaic_fifo_tb___024root___eval_final(&(vlSymsp->TOP));
}

VL_ATTR_COLD void Vmosaic_fifo_tb::dumpTriggersStl() {
    Vmosaic_fifo_tb___024root___eval_dump_triggers__stl(&(vlSymsp->TOP));
}

VL_ATTR_COLD void Vmosaic_fifo_tb::dumpTriggersIco() {
    Vmosaic_fifo_tb___024root___eval_dump_triggers__ico(&(vlSymsp->TOP));
}

VL_ATTR_COLD void Vmosaic_fifo_tb::dumpTriggersAct() {
    Vmosaic_fifo_tb___024root___eval_dump_triggers__act(&(vlSymsp->TOP));
}

VL_ATTR_COLD void Vmosaic_fifo_tb::dumpTriggersNba() {
    Vmosaic_fifo_tb___024root___eval_dump_triggers__nba(&(vlSymsp->TOP));
}

VL_ATTR_COLD void Vmosaic_fifo_tb::dumpTriggersObs() {
    Vmosaic_fifo_tb___024root___eval_dump_triggers__obs(&(vlSymsp->TOP));
}

VL_ATTR_COLD void Vmosaic_fifo_tb::dumpTriggersReact() {
    Vmosaic_fifo_tb___024root___eval_dump_triggers__react(&(vlSymsp->TOP));
}

//============================================================
// Events and timing
bool Vmosaic_fifo_tb::eventsPending() { return false; }

uint64_t Vmosaic_fifo_tb::nextTimeSlot() {
    VL_FATAL_MT(__FILE__, __LINE__, "", "No delays in the design");
    return 0;
}

//============================================================
// Utilities

const char* Vmosaic_fifo_tb::name() const {
    return vlSymsp->name();
}

//============================================================
// Invoke final blocks

VL_ATTR_COLD void Vmosaic_fifo_tb::final() {
    contextp()->executingFinal(true);
    evalFinal();
    contextp()->executingFinal(false);
}

//============================================================
// Implementations of abstract methods from VerilatedModel

const char* Vmosaic_fifo_tb::hierName() const { return vlSymsp->name(); }
const char* Vmosaic_fifo_tb::modelName() const { return "Vmosaic_fifo_tb"; }
unsigned Vmosaic_fifo_tb::threads() const { return 1; }
void Vmosaic_fifo_tb::prepareClone() const { contextp()->prepareClone(); }
void Vmosaic_fifo_tb::atClone() const {
    contextp()->threadPoolpOnClone();
}
