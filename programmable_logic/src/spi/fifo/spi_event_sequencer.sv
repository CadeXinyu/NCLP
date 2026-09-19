`timescale 1ns / 1ps
`default_nettype none

// Convert non-backpressurable SPI sample events and session-end pulses into
// ordered FIFO enqueue events. A coincident final sample goes before EOS;
// a pending earlier EOS goes before a new sample. Rejected samples are reported
// immediately and are never retried. Only EOS is held until event_ready.
//
// This is the synchronous enqueue side of an acquisition FIFO, not a complete
// AXI-Stream master: a source sample may change while the FIFO is full. The
// owning subsystem accounts for sample_dropped and owns the FIFO/CDC policy.
module spi_event_sequencer #(
    parameter integer DATA_WIDTH = 16
)(
    input  wire                  clk,
    input  wire                  reset,
    input  wire [DATA_WIDTH-1:0]  sample_data,
    input  wire                  sample_valid,
    input  wire                  sample_last,
    input  wire                  session_end,
    output wire [DATA_WIDTH-1:0]  event_data,
    output wire [DATA_WIDTH/8-1:0] event_keep,
    output wire                  event_valid,
    output wire                  event_last,
    input  wire                  event_ready,
    output wire                  sample_dropped,
    output reg                   eos_pending = 1'b0
);
    wire event_is_sample = sample_valid && !eos_pending;
    wire direct_eos = session_end && !sample_valid && !eos_pending;
    assign event_data = event_is_sample ? sample_data : {DATA_WIDTH{1'b0}};
    assign event_keep = event_is_sample ? {(DATA_WIDTH/8){1'b1}} : {(DATA_WIDTH/8){1'b0}};
    assign event_valid = event_is_sample || eos_pending || direct_eos;
    assign event_last = event_is_sample ? sample_last : 1'b1;
    assign sample_dropped = sample_valid && !(event_is_sample && event_ready);

    // The SPI clock can stop during runtime clock recovery. Its reset tree
    // must clear a queued EOS even when no further source clock edge arrives.
    always @(posedge clk or posedge reset) begin
        if (reset)
            eos_pending <= 1'b0;
        else if (eos_pending) begin
            // Accept the old EOS, retaining a new simultaneous session end.
            if (event_ready)
                eos_pending <= session_end;
        end else if (session_end && (sample_valid || !event_ready)) begin
            eos_pending <= 1'b1;
        end
    end

    initial begin
        if (DATA_WIDTH < 8 || DATA_WIDTH % 8 != 0)
            $error("spi_event_sequencer: DATA_WIDTH must be a positive multiple of eight");
    end
endmodule

`default_nettype wire
