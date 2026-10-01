// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Primary model header
//
// This header should be included by all source files instantiating the design.
// The class here is then constructed to instantiate the design.
// See the Verilator manual for examples.

#ifndef VERILATED_VMOSAIC_FIFO_TB_H_
#define VERILATED_VMOSAIC_FIFO_TB_H_  // guard

#include "verilated.h"

class Vmosaic_fifo_tb__Syms;
class Vmosaic_fifo_tb___024root;

// This class is the main interface to the Verilated model
class alignas(VL_CACHE_LINE_BYTES) Vmosaic_fifo_tb VL_NOT_FINAL : public VerilatedModel {
    friend class Vmosaic_fifo_tb__Syms;
  private:
    // Symbol table holding complete model state (owned by this class)
    Vmosaic_fifo_tb__Syms* const vlSymsp;
    // Evaluation loop
    VerilatedEvalLoop m_evalLoop;

  public:

    // CONSTEXPR CAPABILITIES
    // Verilated with --trace?
    static constexpr bool traceCapable = false;

    // PORTS
    // The application code writes and reads these signals to
    // propagate new values into/out from the Verilated model.
    VL_IN8(&clk,0,0);
    VL_IN8(&rst,0,0);
    VL_IN8(&d1_in_valid,0,0);
    VL_OUT8(&d1_in_ready,0,0);
    VL_OUT8(&d1_out_valid,0,0);
    VL_IN8(&d1_out_ready,0,0);
    VL_OUT8(&d1_count,0,0);
    VL_IN8(&d2_in_valid,0,0);
    VL_OUT8(&d2_in_ready,0,0);
    VL_OUT8(&d2_out_valid,0,0);
    VL_IN8(&d2_out_ready,0,0);
    VL_OUT8(&d2_count,1,0);
    VL_IN8(&d3_in_valid,0,0);
    VL_OUT8(&d3_in_ready,0,0);
    VL_OUT8(&d3_out_valid,0,0);
    VL_IN8(&d3_out_ready,0,0);
    VL_OUT8(&d3_count,1,0);
    VL_IN8(&d8_in_valid,0,0);
    VL_OUT8(&d8_in_ready,0,0);
    VL_OUT8(&d8_out_valid,0,0);
    VL_IN8(&d8_out_ready,0,0);
    VL_OUT8(&d8_count,3,0);
    VL_IN8(&sk_in_valid,0,0);
    VL_OUT8(&sk_in_ready,0,0);
    VL_OUT8(&sk_out_valid,0,0);
    VL_IN8(&sk_out_ready,0,0);
    VL_IN(&d1_in_payload,31,0);
    VL_OUT(&d1_out_payload,31,0);
    VL_IN(&d2_in_payload,31,0);
    VL_OUT(&d2_out_payload,31,0);
    VL_IN(&d3_in_payload,31,0);
    VL_OUT(&d3_out_payload,31,0);
    VL_IN(&d8_in_payload,31,0);
    VL_OUT(&d8_out_payload,31,0);
    VL_IN(&sk_in_payload,31,0);
    VL_OUT(&sk_out_payload,31,0);

    // CELLS
    // Public to allow access to /* verilator public */ items.
    // Otherwise the application code can consider these internals.

    // Root instance pointer to allow access to model internals,
    // including inlined /* verilator public_flat_* */ items.
    Vmosaic_fifo_tb___024root* const rootp;

    // CONSTRUCTORS
    /// Construct the model; called by application code
    /// If contextp is null, then the model will use the default global context
    /// If name is "", then makes a wrapper with a
    /// single model invisible with respect to DPI scope names.
    explicit Vmosaic_fifo_tb(VerilatedContext* contextp, const char* name = "TOP");
    explicit Vmosaic_fifo_tb(const char* name = "TOP");
    /// Destroy the model; called (often implicitly) by application code
    virtual ~Vmosaic_fifo_tb();
  private:
    VL_UNCOPYABLE(Vmosaic_fifo_tb);  ///< Copying not allowed

  public:
    // API METHODS
    /// Evaluate the model.  Application must call when inputs change.
    void eval() { eval_step(); }
    /// Evaluate when calling multiple units/models per time step.
    void eval_step();
    /// Evaluate at end of a timestep for tracing, when using eval_step().
    /// Application must call after all eval() and before time changes.
    void eval_end_step() {}
    /// Simulation complete, run final blocks.  Application must call on completion.
    void final();
    /// Are there scheduled events to handle?
    bool eventsPending();
    /// Returns time at next time slot. Aborts if !eventsPending()
    uint64_t nextTimeSlot();
    /// Trace signals in the model; called by application code
    void trace(VerilatedTraceBaseC* tfp, int levels, int options = 0) { contextp()->trace(tfp, levels, options); }
    /// Retrieve name of this model instance (as passed to constructor).
    const char* name() const;

    // Abstract methods from VerilatedModel
    const char* hierName() const override final;
    const char* modelName() const override final;
    unsigned threads() const override final;
    /// Prepare for cloning the model at the process level (e.g. fork in Linux)
    /// Release necessary resources. Called before cloning.
    void prepareClone() const;
    /// Re-init after cloning the model at the process level (e.g. fork in Linux)
    /// Re-allocate necessary resources. Called after cloning.
    void atClone() const;
  private:

    // Internal functions - the model's evaluation entry points
    void evalBegin() override final;
    void evalEnd() override final;
    void evalStatic() override final;
    void evalInitial() override final;
    bool evalStl(bool firstIteration) override final;
    void evalSample() override final;
    bool evalIco(bool firstIteration) override final;
    bool evalAct() override final;
    bool evalInact() override final;
    bool evalNba() override final;
    bool evalObs() override final;
    bool evalReact() override final;
    void evalPostponed() override final;
    void evalFinal() override final;
    void dumpTriggersStl() override final;
    void dumpTriggersIco() override final;
    void dumpTriggersAct() override final;
    void dumpTriggersNba() override final;
    void dumpTriggersObs() override final;
    void dumpTriggersReact() override final;

    // Internal functions - trace registration
    void traceBaseModel(VerilatedTraceBaseC* tfp, int levels, int options);
};

#endif  // guard
