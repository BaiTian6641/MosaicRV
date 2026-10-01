// GENERATED FILE - do not edit.
// Produced by tools/gen_manifest.py --profile p0 from config/contracts/.
// Widths are evaluated from the profile geometry; tools/check_contracts.py
// proves every counter modulus exceeds twice the maximum compare distance.

package mosaic_id_pkg;

  // ---- identity field widths ----
  localparam int unsigned MOSAIC_ID_W_ELEMENT_INDEX = 7;
  localparam int unsigned MOSAIC_ID_W_EPOCH        = 7;
  localparam int unsigned MOSAIC_ID_W_HART         = 1;
  localparam int unsigned MOSAIC_ID_W_NEW_OWNER_GEN = 7;
  localparam int unsigned MOSAIC_ID_W_OLD_OWNER_GEN = 7;
  localparam int unsigned MOSAIC_ID_W_OUTSTANDING  = 10;
  localparam int unsigned MOSAIC_ID_W_OWNER_GEN    = 7;
  localparam int unsigned MOSAIC_ID_W_PC           = 64;
  localparam int unsigned MOSAIC_ID_W_PRF_GEN      = 8;
  localparam int unsigned MOSAIC_ID_W_PRF_TAG      = 7;
  localparam int unsigned MOSAIC_ID_W_PRODUCER_OWNER_GEN = 7;
  localparam int unsigned MOSAIC_ID_W_REQ_ID       = 2;
  localparam int unsigned MOSAIC_ID_W_RETIRE_SEQ   = 8;
  localparam int unsigned MOSAIC_ID_W_ROB_GEN      = 7;
  localparam int unsigned MOSAIC_ID_W_ROB_INDEX    = 6;
  localparam int unsigned MOSAIC_ID_W_ROUTE_ID     = 2;
  localparam int unsigned MOSAIC_ID_W_SEQ          = 11;
  localparam int unsigned MOSAIC_ID_W_TX_GEN       = 6;
  localparam int unsigned MOSAIC_ID_W_TX_ID        = 5;
  localparam int unsigned MOSAIC_ID_W_UOP_BITMAP_INDEX = 3;
  localparam int unsigned MOSAIC_ID_W_UOP_INDEX    = 3;

  // ---- counter moduli (expressions from config/contracts/counters.json) ----
  // rob_generation: modulus 128, at most rob_entries live
  localparam int unsigned MOSAIC_CNT_W_ROB_GENERATION   = 128;
  // uop_sequence: modulus 1025, at most rob_entries*max_uops_per_macro live
  localparam int unsigned MOSAIC_CNT_W_UOP_SEQUENCE     = 1025;
  // prf_generation: modulus 192, at most int_prf_entries live
  localparam int unsigned MOSAIC_CNT_W_PRF_GENERATION   = 192;
  // memory_transaction_generation: modulus 45, at most fetch_outstanding+lq_entries+sq_entries+mshrs live
  localparam int unsigned MOSAIC_CNT_W_MEMORY_TRANSACTION_GENERATION = 45;
  // retire_sequence: modulus 129, at most rob_entries live
  localparam int unsigned MOSAIC_CNT_W_RETIRE_SEQUENCE  = 129;

  // ---- composite identities ----
  typedef struct packed {
    logic [MOSAIC_ID_W_HART-1:0]     hart;
    logic [MOSAIC_ID_W_ROB_INDEX-1:0] rob_index;
    logic [MOSAIC_ID_W_ROB_GEN-1:0]   rob_gen;
    logic [MOSAIC_ID_W_UOP_INDEX-1:0] uop_index;
  } macro_id_t;

  typedef struct packed {
    logic [MOSAIC_ID_W_HART-1:0]   hart;
    logic [MOSAIC_ID_W_PRF_TAG-1:0] prf_tag;
    logic [MOSAIC_ID_W_PRF_GEN-1:0] prf_gen;
  } prf_id_t;

  // A macro identity matches only when the generation agrees. Comparing the
  // wrapping ROB index alone would let a squashed macro's late completion act
  // on the macro that has since taken the same entry.
  function automatic logic macro_id_eq(input macro_id_t a, input macro_id_t b);
    return (a.hart == b.hart) && (a.rob_gen == b.rob_gen) &&
           (a.rob_index == b.rob_index) && (a.uop_index == b.uop_index);
  endfunction

  function automatic logic macro_id_live(input macro_id_t a, input macro_id_t b);
    return (a.hart == b.hart) && (a.rob_gen == b.rob_gen) && (a.rob_index == b.rob_index);
  endfunction

  // Age comparison. The sequence modulus strictly exceeds twice the largest
  // distance two live uops can be separated by, so the sign of the modular
  // difference is unambiguous and no extra tag bit is needed.
  function automatic logic seq_older(input logic [MOSAIC_CNT_W_UOP_SEQUENCE-1:0] a,
                                        input logic [MOSAIC_CNT_W_UOP_SEQUENCE-1:0] b);
    logic [MOSAIC_CNT_W_UOP_SEQUENCE-1:0] diff;
    begin
      diff = a - b;
      seq_older = (diff != '0) && (diff[MOSAIC_CNT_W_UOP_SEQUENCE-1]);
    end
  endfunction

endpackage : mosaic_id_pkg
