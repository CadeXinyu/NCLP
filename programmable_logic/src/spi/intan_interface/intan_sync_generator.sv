`timescale 1ns / 1ps
`default_nettype none

// Sample-synchronous Intan synchronization output generator.
//
// Configuration is captured only at session_start and remains immutable until
// the next session.  session_start is asserted on the same spi_clk edge that
// launches scan slot 0.  frame_advance is asserted on each later edge that
// wraps AUX3 to the next scan slot 0.  Consequently, periodic-mode edges are
// aligned to logical Intan sample boundaries and never depend on s00_axi_aclk.
module intan_sync_generator (
    input  wire        spi_clk,
    input  wire        reset,
    input  wire        session_start,
    input  wire        frame_advance,
    input  wire        session_stop,
    input  wire [ 1:0] config_mode,
    input  wire [15:0] config_period_frames,
    input  wire [15:0] config_high_frames,
    output reg         sync_out_spi
);
    localparam [1:0] SYNC_MODE_OFF         = 2'd0;
    localparam [1:0] SYNC_MODE_PERIODIC    = 2'd1;
    localparam [1:0] SYNC_MODE_RECORD_GATE = 2'd2;

    reg [ 1:0] active_mode;
    reg [15:0] active_period_frames;
    reg [15:0] active_high_frames;
    reg [15:0] phase_index;

    wire periodic_config_valid =
        (config_period_frames != 16'd0) &&
        (config_high_frames != 16'd0) &&
        (config_high_frames <= config_period_frames);

    wire [15:0] next_phase_index =
        (phase_index == (active_period_frames - 16'd1)) ?
            16'd0 : phase_index + 16'd1;

    // The runtime SPI clock may be gated during startup/reconfiguration.
    // reset comes from the domain's async-assert/sync-release XPM tree, so
    // clear the external synchronization level even while no SPI edge exists.
    always @(posedge spi_clk or posedge reset) begin
        if (reset) begin
            active_mode <= SYNC_MODE_OFF;
            active_period_frames <= 16'd1;
            active_high_frames <= 16'd0;
            phase_index <= 16'd0;
            sync_out_spi <= 1'b0;
        end else if (session_stop) begin
            // STOP has fail-safe priority over a coincident START.  The Intan
            // engine generates these events from mutually exclusive states,
            // but this ordering keeps the standalone block safe as well.
            active_mode <= SYNC_MODE_OFF;
            active_period_frames <= 16'd1;
            active_high_frames <= 16'd0;
            phase_index <= 16'd0;
            sync_out_spi <= 1'b0;
        end else if (session_start) begin
            phase_index <= 16'd0;

            case (config_mode)
                SYNC_MODE_PERIODIC: begin
                    if (periodic_config_valid) begin
                        active_mode <= SYNC_MODE_PERIODIC;
                        active_period_frames <= config_period_frames;
                        active_high_frames <= config_high_frames;
                        sync_out_spi <= (config_high_frames != 16'd0);
                    end else begin
                        // Invalid periodic timing fails safe to OFF for the
                        // complete session instead of silently clamping it.
                        active_mode <= SYNC_MODE_OFF;
                        active_period_frames <= 16'd1;
                        active_high_frames <= 16'd0;
                        sync_out_spi <= 1'b0;
                    end
                end

                SYNC_MODE_RECORD_GATE: begin
                    active_mode <= SYNC_MODE_RECORD_GATE;
                    active_period_frames <= 16'd1;
                    active_high_frames <= 16'd1;
                    sync_out_spi <= 1'b1;
                end

                default: begin
                    active_mode <= SYNC_MODE_OFF;
                    active_period_frames <= 16'd1;
                    active_high_frames <= 16'd0;
                    sync_out_spi <= 1'b0;
                end
            endcase
        end else if (frame_advance && active_mode == SYNC_MODE_PERIODIC) begin
            phase_index <= next_phase_index;
            sync_out_spi <= (next_phase_index < active_high_frames);
        end
    end

endmodule

`default_nettype wire
