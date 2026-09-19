`timescale 1ns / 1ps
`default_nettype none

// Select independent absolute quarter-SCLK cable-delay phases for the two
// RHD2164 MISO views from one 4x-sampled physical transaction.
module intan_miso_lane_aligner (
    input  wire [ 3:0] primary_phase,
    input  wire [ 3:0] secondary_phase,
    input  wire [75:0] samples_4x,
    output reg  [15:0] primary_word,
    output reg  [15:0] secondary_word
);
    integer bit_index;
    integer primary_index;
    integer secondary_index;
    always @* begin
        primary_word   = 16'h0000;
        secondary_word = 16'h0000;
        for (bit_index = 0; bit_index < 16; bit_index = bit_index + 1) begin
            primary_index = primary_phase + 4 * bit_index;
            secondary_index = secondary_phase + 4 * bit_index;
            primary_word[15-bit_index]   = samples_4x[primary_index];
            secondary_word[15-bit_index] = samples_4x[secondary_index];
        end
    end
endmodule

`default_nettype wire
