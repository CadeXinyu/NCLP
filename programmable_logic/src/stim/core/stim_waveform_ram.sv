`timescale 1ns / 1ps
`default_nettype none

// Simple dual-port waveform memory.
//
// The PS-facing port supports byte writes/readback in the AXI clock domain.
// The playback port is a synchronous, read-only port in the independently
// generated DAC-engine clock domain.  Do not reset the memory array: doing so
// prevents Vivado from inferring block RAM.
module stim_waveform_ram #(
    parameter integer RAM_DEPTH = 1024,
    parameter integer RAM_ADDR_WIDTH = (RAM_DEPTH > 1) ? $clog2(RAM_DEPTH) : 1
)(
    input  wire                         bus_clk,
    input  wire [RAM_ADDR_WIDTH-1:0]    bus_addr,
    input  wire                         bus_write_enable,
    input  wire [3:0]                   bus_write_strobes,
    input  wire [31:0]                  bus_write_data,
    input  wire                         bus_read_enable,
    output reg  [31:0]                  bus_read_data,

    input  wire                         playback_clk,
    input  wire [RAM_ADDR_WIDTH-1:0]    playback_addr,
    output reg  [31:0]                  playback_data
);
    localparam integer REQUIRED_RAM_ADDR_WIDTH =
        (RAM_DEPTH > 1) ? $clog2(RAM_DEPTH) : 1;

    (* ram_style = "block" *) reg [31:0] waveform_memory [0:RAM_DEPTH-1];
    integer byte_index;

    initial begin
        if (RAM_DEPTH < 1) begin
            $error("stim_waveform_ram: RAM_DEPTH must be positive");
            $finish;
        end
        if (RAM_ADDR_WIDTH != REQUIRED_RAM_ADDR_WIDTH) begin
            $error("stim_waveform_ram: RAM_ADDR_WIDTH must match RAM_DEPTH");
            $finish;
        end
    end

    always @(posedge bus_clk) begin
        if (bus_read_enable)
            bus_read_data <= waveform_memory[bus_addr];

        if (bus_write_enable) begin
            for (byte_index = 0; byte_index < 4; byte_index = byte_index + 1) begin
                if (bus_write_strobes[byte_index])
                    waveform_memory[bus_addr][byte_index*8 +: 8] <=
                        bus_write_data[byte_index*8 +: 8];
            end
        end
    end

    always @(posedge playback_clk)
        playback_data <= waveform_memory[playback_addr];

endmodule

`default_nettype wire
