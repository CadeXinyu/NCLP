// SPDX-License-Identifier: MIT
`timescale 1ns/1ps
`default_nettype none

// Frame-oriented, transmit-only SPI master with one active and one queued
// transaction.
//
// A frame is accepted on i_Frame_Valid && o_Frame_Ready. The active payload is
// i_MOSI_Frame[i_Frame_Bits-1:0]. o_Done pulses on the clock edge that raises
// CS at the end of a frame. Timing parameters count complete i_Clk periods.
// CS_SETUP_CLKS and CS_HOLD_CLKS support a true zero-cycle dwell. Because CS is
// raised and lowered by registered edges, CS_HIGH_CLKS values zero and one both
// select the minimum physical high interval of one complete i_Clk period.
module spi_frame_master #(
    parameter integer MAX_FRAME_BITS     = 32,
    parameter integer FRAME_BITS_W       = (MAX_FRAME_BITS > 1) ? $clog2(MAX_FRAME_BITS+1) : 1,
    parameter integer CLKS_PER_HALF_BIT  = 2,
    parameter integer SPI_MODE           = 0,
    parameter integer CS_SETUP_CLKS      = 1,
    parameter integer CS_HOLD_CLKS       = 1,
    parameter integer CS_HIGH_CLKS       = 4,
    parameter         MSB_FIRST          = 1'b1,
    parameter         MOSI_IDLE          = 1'b0
) (
    input  wire                         i_Rst_L,
    input  wire                         i_Clk,

    input  wire                         i_Frame_Valid,
    input  wire [MAX_FRAME_BITS-1:0]    i_MOSI_Frame,
    input  wire [FRAME_BITS_W-1:0]      i_Frame_Bits,

    output wire                         o_Frame_Ready,
    output reg                          o_Busy,
    output reg                          o_Done,

    // Keep the source-synchronous DAC pins in the I/O bank when the hierarchy
    // is implemented.  All three launch from this same engine-clock edge,
    // bounding their relative package-pin skew even though board-level output
    // delays are intentionally not asserted in the simulation-only flow.
    (* IOB = "TRUE" *) output reg       o_SPI_CS_n,
    (* IOB = "TRUE" *) output reg       o_SPI_Clk,
    (* IOB = "TRUE" *) output reg       o_SPI_MOSI
);

    localparam integer HALF_EDGE_MAX = MAX_FRAME_BITS * 2;
    localparam integer HALF_EDGE_W   = (HALF_EDGE_MAX > 1) ? $clog2(HALF_EDGE_MAX+1) : 1;
    localparam integer HALF_COUNT_W  = (CLKS_PER_HALF_BIT > 1) ? $clog2(CLKS_PER_HALF_BIT) : 1;
    localparam integer REQUIRED_FRAME_BITS_W =
        (MAX_FRAME_BITS > 1) ? $clog2(MAX_FRAME_BITS+1) : 1;
    localparam integer CS_HIGH_LAST_COUNT =
        (CS_HIGH_CLKS > 0) ? (CS_HIGH_CLKS-1) : 0;

    localparam [FRAME_BITS_W-1:0] FRAME_BITS_ZERO = {FRAME_BITS_W{1'b0}};
    localparam [FRAME_BITS_W-1:0] FRAME_BITS_ONE  = {{(FRAME_BITS_W-1){1'b0}}, 1'b1};
    localparam [FRAME_BITS_W-1:0] MAX_FRAME_BITS_VALUE = MAX_FRAME_BITS;

    localparam [2:0] ST_IDLE     = 3'd0;
    localparam [2:0] ST_CS_SETUP = 3'd1;
    localparam [2:0] ST_SHIFT    = 3'd2;
    localparam [2:0] ST_CS_HOLD  = 3'd3;
    localparam [2:0] ST_CS_HIGH  = 3'd4;

    wire w_CPOL = (SPI_MODE == 2) || (SPI_MODE == 3);
    wire w_CPHA = (SPI_MODE == 1) || (SPI_MODE == 3);

    reg [2:0]                     r_State;
    reg [MAX_FRAME_BITS-1:0]      r_MOSI_Frame;
    reg [FRAME_BITS_W-1:0]        r_Frame_Bits;
    reg [HALF_EDGE_W-1:0]         r_Half_Edge_Target;
    reg [HALF_EDGE_W-1:0]         r_Half_Edge_Count;
    reg [HALF_COUNT_W-1:0]        r_Half_Clk_Count;
    reg [FRAME_BITS_W-1:0]        r_TX_Index;
    reg [31:0]                    r_Wait_Count;

    reg                           r_Pending_Valid;
    reg [MAX_FRAME_BITS-1:0]      r_Pending_MOSI_Frame;
    reg [FRAME_BITS_W-1:0]        r_Pending_Frame_Bits;

    wire w_Frame_Length_Valid = (i_Frame_Bits != FRAME_BITS_ZERO) &&
                                (i_Frame_Bits <= MAX_FRAME_BITS_VALUE);
    wire w_CS_High_Completes_Now = (r_State == ST_CS_HIGH) &&
                                   ((CS_HIGH_CLKS <= 1) ||
                                    (r_Wait_Count == CS_HIGH_LAST_COUNT));

    assign o_Frame_Ready = (!i_Frame_Valid || w_Frame_Length_Valid) &&
                           ((r_State == ST_IDLE) || !r_Pending_Valid || w_CS_High_Completes_Now);

    function [FRAME_BITS_W-1:0] f_select_bit_index;
        input [FRAME_BITS_W-1:0] frame_bits;
        input [FRAME_BITS_W-1:0] bit_index;
        begin
            if (MSB_FIRST)
                f_select_bit_index = frame_bits - FRAME_BITS_ONE - bit_index;
            else
                f_select_bit_index = bit_index;
        end
    endfunction

    task t_load_active_frame;
        input [MAX_FRAME_BITS-1:0] frame;
        input [FRAME_BITS_W-1:0]   frame_bits;
        begin
            r_MOSI_Frame       <= frame;
            r_Frame_Bits       <= frame_bits;
            r_Half_Edge_Target <= {frame_bits, 1'b0};
            r_Half_Edge_Count  <= {HALF_EDGE_W{1'b0}};
            r_Half_Clk_Count   <= {HALF_COUNT_W{1'b0}};
            r_Wait_Count       <= 32'd0;
            o_SPI_Clk          <= w_CPOL;
            if (w_CPHA) begin
                o_SPI_MOSI <= MOSI_IDLE;
                r_TX_Index <= FRAME_BITS_ZERO;
            end else begin
                o_SPI_MOSI <= frame[f_select_bit_index(frame_bits, FRAME_BITS_ZERO)];
                r_TX_Index <= FRAME_BITS_ONE;
            end
        end
    endtask

    task t_store_pending_frame;
        input [MAX_FRAME_BITS-1:0] frame;
        input [FRAME_BITS_W-1:0]   frame_bits;
        begin
            r_Pending_Valid      <= 1'b1;
            r_Pending_MOSI_Frame <= frame;
            r_Pending_Frame_Bits <= frame_bits;
        end
    endtask

`ifndef SYNTHESIS
    initial begin
        if (MAX_FRAME_BITS < 1) $fatal(1, "spi_frame_master: MAX_FRAME_BITS must be >= 1");
        if (FRAME_BITS_W != REQUIRED_FRAME_BITS_W)
            $fatal(1, "spi_frame_master: FRAME_BITS_W must match MAX_FRAME_BITS");
        if (CLKS_PER_HALF_BIT < 1) $fatal(1, "spi_frame_master: CLKS_PER_HALF_BIT must be >= 1");
        if ((SPI_MODE < 0) || (SPI_MODE > 3)) $fatal(1, "spi_frame_master: SPI_MODE must be in 0..3");
        if (CS_SETUP_CLKS < 0) $fatal(1, "spi_frame_master: CS_SETUP_CLKS must be nonnegative");
        if (CS_HOLD_CLKS < 0) $fatal(1, "spi_frame_master: CS_HOLD_CLKS must be nonnegative");
        if (CS_HIGH_CLKS < 0) $fatal(1, "spi_frame_master: CS_HIGH_CLKS must be nonnegative");
    end
`endif

    always @(posedge i_Clk or negedge i_Rst_L) begin
        if (!i_Rst_L) begin
            r_State              <= ST_IDLE;
            r_MOSI_Frame         <= {MAX_FRAME_BITS{1'b0}};
            r_Frame_Bits         <= FRAME_BITS_ZERO;
            r_Half_Edge_Target   <= {HALF_EDGE_W{1'b0}};
            r_Half_Edge_Count    <= {HALF_EDGE_W{1'b0}};
            r_Half_Clk_Count     <= {HALF_COUNT_W{1'b0}};
            r_TX_Index           <= FRAME_BITS_ZERO;
            r_Wait_Count         <= 32'd0;
            r_Pending_Valid      <= 1'b0;
            r_Pending_MOSI_Frame <= {MAX_FRAME_BITS{1'b0}};
            r_Pending_Frame_Bits <= FRAME_BITS_ZERO;
            o_Busy               <= 1'b0;
            o_Done               <= 1'b0;
            o_SPI_CS_n           <= 1'b1;
            o_SPI_Clk            <= w_CPOL;
            o_SPI_MOSI           <= MOSI_IDLE;
        end else begin
            o_Done <= 1'b0;

            case (r_State)
                ST_IDLE: begin
                    o_Busy       <= 1'b0;
                    o_SPI_CS_n   <= 1'b1;
                    o_SPI_Clk    <= w_CPOL;
                    o_SPI_MOSI   <= MOSI_IDLE;
                    r_Wait_Count <= 32'd0;

                    if (i_Frame_Valid && o_Frame_Ready) begin
                        o_Busy     <= 1'b1;
                        o_SPI_CS_n <= 1'b0;
                        t_load_active_frame(i_MOSI_Frame, i_Frame_Bits);
                        if (CS_SETUP_CLKS == 0)
                            r_State <= ST_SHIFT;
                        else
                            r_State <= ST_CS_SETUP;
                    end
                end

                ST_CS_SETUP: begin
                    o_Busy     <= 1'b1;
                    o_SPI_CS_n <= 1'b0;
                    o_SPI_Clk  <= w_CPOL;

                    if (i_Frame_Valid && o_Frame_Ready)
                        t_store_pending_frame(i_MOSI_Frame, i_Frame_Bits);

                    if (r_Wait_Count == (CS_SETUP_CLKS-1)) begin
                        r_Wait_Count <= 32'd0;
                        r_State      <= ST_SHIFT;
                    end else begin
                        r_Wait_Count <= r_Wait_Count + 1'b1;
                    end
                end

                ST_SHIFT: begin
                    o_Busy     <= 1'b1;
                    o_SPI_CS_n <= 1'b0;

                    if (i_Frame_Valid && o_Frame_Ready)
                        t_store_pending_frame(i_MOSI_Frame, i_Frame_Bits);

                    if (r_Half_Clk_Count == (CLKS_PER_HALF_BIT-1)) begin
                        r_Half_Clk_Count <= {HALF_COUNT_W{1'b0}};
                        o_SPI_Clk        <= ~o_SPI_Clk;

                        if (!r_Half_Edge_Count[0]) begin
                            if (w_CPHA) begin
                                if (r_TX_Index < r_Frame_Bits) begin
                                    o_SPI_MOSI <= r_MOSI_Frame[f_select_bit_index(r_Frame_Bits, r_TX_Index)];
                                    r_TX_Index <= r_TX_Index + 1'b1;
                                end else begin
                                    o_SPI_MOSI <= MOSI_IDLE;
                                end
                            end
                        end else begin
                            if (!w_CPHA) begin
                                if (r_TX_Index < r_Frame_Bits) begin
                                    o_SPI_MOSI <= r_MOSI_Frame[f_select_bit_index(r_Frame_Bits, r_TX_Index)];
                                    r_TX_Index <= r_TX_Index + 1'b1;
                                end else begin
                                    o_SPI_MOSI <= MOSI_IDLE;
                                end
                            end
                        end

                        if (r_Half_Edge_Count == (r_Half_Edge_Target - 1'b1)) begin
                            r_Half_Edge_Count <= {HALF_EDGE_W{1'b0}};
                            r_Wait_Count      <= 32'd0;
                            if (CS_HOLD_CLKS == 0) begin
                                o_SPI_CS_n <= 1'b1;
                                o_Done     <= 1'b1;
                                o_SPI_MOSI <= MOSI_IDLE;
                                r_State    <= ST_CS_HIGH;
                            end else begin
                                r_State <= ST_CS_HOLD;
                            end
                        end else begin
                            r_Half_Edge_Count <= r_Half_Edge_Count + 1'b1;
                        end
                    end else begin
                        r_Half_Clk_Count <= r_Half_Clk_Count + 1'b1;
                    end
                end

                ST_CS_HOLD: begin
                    o_Busy     <= 1'b1;
                    o_SPI_CS_n <= 1'b0;
                    o_SPI_Clk  <= w_CPOL;

                    if (i_Frame_Valid && o_Frame_Ready)
                        t_store_pending_frame(i_MOSI_Frame, i_Frame_Bits);

                    if (r_Wait_Count == (CS_HOLD_CLKS-1)) begin
                        o_SPI_CS_n   <= 1'b1;
                        o_Done       <= 1'b1;
                        o_SPI_Clk    <= w_CPOL;
                        o_SPI_MOSI   <= MOSI_IDLE;
                        r_Wait_Count <= 32'd0;
                        r_State      <= ST_CS_HIGH;
                    end else begin
                        r_Wait_Count <= r_Wait_Count + 1'b1;
                    end
                end

                ST_CS_HIGH: begin
                    o_Busy     <= 1'b1;
                    o_SPI_CS_n <= 1'b1;
                    o_SPI_Clk  <= w_CPOL;
                    o_SPI_MOSI <= MOSI_IDLE;

                    if ((CS_HIGH_CLKS <= 1) ||
                        (r_Wait_Count == CS_HIGH_LAST_COUNT)) begin
                        if (r_Pending_Valid) begin
                            o_SPI_CS_n <= 1'b0;
                            t_load_active_frame(r_Pending_MOSI_Frame, r_Pending_Frame_Bits);
                            if (i_Frame_Valid && o_Frame_Ready)
                                t_store_pending_frame(i_MOSI_Frame, i_Frame_Bits);
                            else
                                r_Pending_Valid <= 1'b0;
                            if (CS_SETUP_CLKS == 0)
                                r_State <= ST_SHIFT;
                            else
                                r_State <= ST_CS_SETUP;
                        end else if (i_Frame_Valid && o_Frame_Ready) begin
                            o_SPI_CS_n <= 1'b0;
                            t_load_active_frame(i_MOSI_Frame, i_Frame_Bits);
                            if (CS_SETUP_CLKS == 0)
                                r_State <= ST_SHIFT;
                            else
                                r_State <= ST_CS_SETUP;
                        end else begin
                            o_Busy       <= 1'b0;
                            r_Wait_Count <= 32'd0;
                            r_State      <= ST_IDLE;
                        end
                    end else begin
                        if (i_Frame_Valid && o_Frame_Ready)
                            t_store_pending_frame(i_MOSI_Frame, i_Frame_Bits);
                        r_Wait_Count <= r_Wait_Count + 1'b1;
                    end
                end

                default: begin
                    r_State         <= ST_IDLE;
                    r_Pending_Valid <= 1'b0;
                    o_Busy          <= 1'b0;
                    o_Done          <= 1'b0;
                    o_SPI_CS_n      <= 1'b1;
                    o_SPI_Clk       <= w_CPOL;
                    o_SPI_MOSI      <= MOSI_IDLE;
                end
            endcase
        end
    end

endmodule

`default_nettype wire
