`timescale 1ns / 1ps
`default_nettype none

// Four logical 1024-word command banks in one true dual-clock block RAM.
//
// Port A belongs to the 100 MHz AXI control clock and supports direct readback
// plus byte-enabled writes.  Port B belongs to the programmable Intan SPI
// clock and is deliberately synchronous with one cycle of latency.  The AXI
// register bank blocks writes while a START is pending or acquisition is
// active; mixed-clock same-address writes are therefore excluded by contract.
module intan_aux_command_ram (
    input  wire        bus_clk,
    input  wire [ 1:0] bus_write_strobe,
    input  wire [11:0] bus_address,
    input  wire [15:0] bus_write_data,
    output reg  [15:0] bus_read_data,

    input  wire        read_clk,
    input  wire        read_reset,
    input  wire [11:0] read_address,
    output reg  [15:0] read_data
);
    (* ram_style = "block" *) reg [15:0] memory [0:4095];

    always @(posedge bus_clk) begin
        if (bus_write_strobe[0])
            memory[bus_address][7:0] <= bus_write_data[7:0];
        if (bus_write_strobe[1])
            memory[bus_address][15:8] <= bus_write_data[15:8];
        bus_read_data <= memory[bus_address];
    end

    always @(posedge read_clk) begin
        if (read_reset) begin
            read_data <= 16'h0000;
        end else begin
            read_data <= memory[read_address];
        end
    end
endmodule

`default_nettype wire
