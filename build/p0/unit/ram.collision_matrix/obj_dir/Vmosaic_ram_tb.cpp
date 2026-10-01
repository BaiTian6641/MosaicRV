// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Model implementation (design independent parts)

#include "Vmosaic_ram_tb__pch.h"

//============================================================
// Constructors

Vmosaic_ram_tb::Vmosaic_ram_tb(VerilatedContext* _vcontextp__, const char* _vcname__)
    : VerilatedModel{*_vcontextp__}
    , vlSymsp{new Vmosaic_ram_tb__Syms(contextp(), _vcname__, this)}
    , m_evalLoop{*this, /*convergeLimit:*/ 10000}
    , clk{vlSymsp->TOP.clk}
    , rst{vlSymsp->TOP.rst}
    , waddr{vlSymsp->TOP.waddr}
    , wmask{vlSymsp->TOP.wmask}
    , we{vlSymsp->TOP.we}
    , raddr{vlSymsp->TOP.raddr}
    , rvalid_a{vlSymsp->TOP.rvalid_a}
    , rvalid_b{vlSymsp->TOP.rvalid_b}
    , wdata{vlSymsp->TOP.wdata}
    , rdata_a{vlSymsp->TOP.rdata_a}
    , rdata_b{vlSymsp->TOP.rdata_b}
    , rootp{&(vlSymsp->TOP)}
{
    // Register model with the context
    contextp()->addModel(this);
}

Vmosaic_ram_tb::Vmosaic_ram_tb(const char* _vcname__)
    : Vmosaic_ram_tb(Verilated::threadContextp(), _vcname__)
{
}

//============================================================
// Destructor

Vmosaic_ram_tb::~Vmosaic_ram_tb() {
    delete vlSymsp;
}

//============================================================
// Evaluation function

#ifdef VL_DEBUG
void Vmosaic_ram_tb___024root___eval_debug_assertions(Vmosaic_ram_tb___024root* vlSelf);
#endif  // VL_DEBUG
VL_ATTR_COLD void Vmosaic_ram_tb___024root___eval_static(Vmosaic_ram_tb___024root* vlSelf);
VL_ATTR_COLD void Vmosaic_ram_tb___024root___eval_initial(Vmosaic_ram_tb___024root* vlSelf);
VL_ATTR_COLD bool Vmosaic_ram_tb___024root___eval_stl(Vmosaic_ram_tb___024root* vlSelf, CData/*0:0*/ firstIteration);
void Vmosaic_ram_tb___024root___eval_sample(Vmosaic_ram_tb___024root* vlSelf);
bool Vmosaic_ram_tb___024root___eval_ico(Vmosaic_ram_tb___024root* vlSelf, CData/*0:0*/ firstIteration);
bool Vmosaic_ram_tb___024root___eval_act(Vmosaic_ram_tb___024root* vlSelf);
bool Vmosaic_ram_tb___024root___eval_inact(Vmosaic_ram_tb___024root* vlSelf);
bool Vmosaic_ram_tb___024root___eval_nba(Vmosaic_ram_tb___024root* vlSelf);
bool Vmosaic_ram_tb___024root___eval_obs(Vmosaic_ram_tb___024root* vlSelf);
bool Vmosaic_ram_tb___024root___eval_react(Vmosaic_ram_tb___024root* vlSelf);
void Vmosaic_ram_tb___024root___eval_postponed(Vmosaic_ram_tb___024root* vlSelf);
VL_ATTR_COLD void Vmosaic_ram_tb___024root___eval_final(Vmosaic_ram_tb___024root* vlSelf);
VL_ATTR_COLD void Vmosaic_ram_tb___024root___eval_dump_triggers__stl(Vmosaic_ram_tb___024root* vlSelf);
VL_ATTR_COLD void Vmosaic_ram_tb___024root___eval_dump_triggers__ico(Vmosaic_ram_tb___024root* vlSelf);
VL_ATTR_COLD void Vmosaic_ram_tb___024root___eval_dump_triggers__act(Vmosaic_ram_tb___024root* vlSelf);
VL_ATTR_COLD void Vmosaic_ram_tb___024root___eval_dump_triggers__nba(Vmosaic_ram_tb___024root* vlSelf);
VL_ATTR_COLD void Vmosaic_ram_tb___024root___eval_dump_triggers__obs(Vmosaic_ram_tb___024root* vlSelf);
VL_ATTR_COLD void Vmosaic_ram_tb___024root___eval_dump_triggers__react(Vmosaic_ram_tb___024root* vlSelf);

void Vmosaic_ram_tb::eval_step() {
    VL_DEBUG_IF(VL_DBG_MSGF("+++++TOP Evaluate Vmosaic_ram_tb::eval_step\n"); );
    m_evalLoop.eval();
}

void Vmosaic_ram_tb::evalBegin() {
#ifdef VL_DEBUG
    // Debug assertions
    Vmosaic_ram_tb___024root___eval_debug_assertions(&(vlSymsp->TOP));
#endif  // VL_DEBUG
    vlSymsp->__Vm_deleter.deleteAll();
}

void Vmosaic_ram_tb::evalEnd() {
    // Evaluate cleanup
    Verilated::endOfEval(vlSymsp->__Vm_evalMsgQp);
}

void Vmosaic_ram_tb::evalStatic() {
    Vmosaic_ram_tb___024root___eval_static(&(vlSymsp->TOP));
}

void Vmosaic_ram_tb::evalInitial() {
    Vmosaic_ram_tb___024root___eval_initial(&(vlSymsp->TOP));
}

bool Vmosaic_ram_tb::evalStl(bool firstIteration) {
    return Vmosaic_ram_tb___024root___eval_stl(&(vlSymsp->TOP), firstIteration);
}

void Vmosaic_ram_tb::evalSample() {
    Vmosaic_ram_tb___024root___eval_sample(&(vlSymsp->TOP));
}

bool Vmosaic_ram_tb::evalIco(bool firstIteration) {
    return Vmosaic_ram_tb___024root___eval_ico(&(vlSymsp->TOP), firstIteration);
}

bool Vmosaic_ram_tb::evalAct() {
    return Vmosaic_ram_tb___024root___eval_act(&(vlSymsp->TOP));
}

bool Vmosaic_ram_tb::evalInact() {
    return Vmosaic_ram_tb___024root___eval_inact(&(vlSymsp->TOP));
}

bool Vmosaic_ram_tb::evalNba() {
    return Vmosaic_ram_tb___024root___eval_nba(&(vlSymsp->TOP));
}

bool Vmosaic_ram_tb::evalObs() {
    return Vmosaic_ram_tb___024root___eval_obs(&(vlSymsp->TOP));
}

bool Vmosaic_ram_tb::evalReact() {
    return Vmosaic_ram_tb___024root___eval_react(&(vlSymsp->TOP));
}

void Vmosaic_ram_tb::evalPostponed() {
    Vmosaic_ram_tb___024root___eval_postponed(&(vlSymsp->TOP));
}

void Vmosaic_ram_tb::evalFinal() {
    Vmosaic_ram_tb___024root___eval_final(&(vlSymsp->TOP));
}

VL_ATTR_COLD void Vmosaic_ram_tb::dumpTriggersStl() {
    Vmosaic_ram_tb___024root___eval_dump_triggers__stl(&(vlSymsp->TOP));
}

VL_ATTR_COLD void Vmosaic_ram_tb::dumpTriggersIco() {
    Vmosaic_ram_tb___024root___eval_dump_triggers__ico(&(vlSymsp->TOP));
}

VL_ATTR_COLD void Vmosaic_ram_tb::dumpTriggersAct() {
    Vmosaic_ram_tb___024root___eval_dump_triggers__act(&(vlSymsp->TOP));
}

VL_ATTR_COLD void Vmosaic_ram_tb::dumpTriggersNba() {
    Vmosaic_ram_tb___024root___eval_dump_triggers__nba(&(vlSymsp->TOP));
}

VL_ATTR_COLD void Vmosaic_ram_tb::dumpTriggersObs() {
    Vmosaic_ram_tb___024root___eval_dump_triggers__obs(&(vlSymsp->TOP));
}

VL_ATTR_COLD void Vmosaic_ram_tb::dumpTriggersReact() {
    Vmosaic_ram_tb___024root___eval_dump_triggers__react(&(vlSymsp->TOP));
}

//============================================================
// Events and timing
bool Vmosaic_ram_tb::eventsPending() { return false; }

uint64_t Vmosaic_ram_tb::nextTimeSlot() {
    VL_FATAL_MT(__FILE__, __LINE__, "", "No delays in the design");
    return 0;
}

//============================================================
// Utilities

const char* Vmosaic_ram_tb::name() const {
    return vlSymsp->name();
}

//============================================================
// Invoke final blocks

VL_ATTR_COLD void Vmosaic_ram_tb::final() {
    contextp()->executingFinal(true);
    evalFinal();
    contextp()->executingFinal(false);
}

//============================================================
// Implementations of abstract methods from VerilatedModel

const char* Vmosaic_ram_tb::hierName() const { return vlSymsp->name(); }
const char* Vmosaic_ram_tb::modelName() const { return "Vmosaic_ram_tb"; }
unsigned Vmosaic_ram_tb::threads() const { return 1; }
void Vmosaic_ram_tb::prepareClone() const { contextp()->prepareClone(); }
void Vmosaic_ram_tb::atClone() const {
    contextp()->threadPoolpOnClone();
}
