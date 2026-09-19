`timescale 1ns/1ps
`default_nettype none
`include "ddrw_registers.vh"

// Owns the next-session geometry register bank and its combinational validation.
// Writes never modify an active session. The session controller snapshots these
// outputs on START using their pre-edge values; this leaf has no START/ARM state.
module ddrw_session_config #(
    parameter integer REG_ADDR_WIDTH = 12,
    parameter integer AXI_ADDR_WIDTH = 49
)(
    input wire clk,
    input wire rst,
    input wire reg_wr_en,
    input wire [REG_ADDR_WIDTH-1:0] reg_wr_addr,
    input wire [31:0] reg_wr_data,
    input wire [3:0] reg_wr_strb,
    output wire [63:0] ring_base_address,
    output reg [31:0] ring_size_bytes,
    output reg [31:0] block_size_bytes,
    output reg [31:0] ring_capacity_blocks,
    output reg [31:0] irq_completion_interval_blocks,
    output wire address_valid,
    output wire config_valid
);
    function automatic [31:0] apply_wstrb;
        input [31:0] old_value;
        input [31:0] new_value;
        input [3:0]  write_strobes;
        integer byte_index;
        begin
            apply_wstrb = old_value;
            for (byte_index = 0; byte_index < 4; byte_index = byte_index + 1) begin
                if (write_strobes[byte_index]) begin
                    apply_wstrb[byte_index*8 +: 8] =
                        new_value[byte_index*8 +: 8];
                end
            end
        end
    endfunction

    reg [31:0] ring_base_lo;
    reg [31:0] ring_base_hi;

    assign ring_base_address = {ring_base_hi, ring_base_lo};
    // Explicitly widen one operand: Verilog otherwise permits a 32-bit
    // multiplication result before it is assigned into this 64-bit wire.
    wire [63:0] shadow_ring_geometry_product =
        {32'd0, block_size_bytes} * ring_capacity_blocks;
    wire [31:0] shadow_ring_last_byte_offset =
        (ring_size_bytes == 0) ? 32'd0 : ring_size_bytes - 32'd1;
    wire [64:0] shadow_ring_last_address =
        {1'b0, ring_base_address} + {33'd0, shadow_ring_last_byte_offset};
    wire shadow_address_fits =
        (ring_base_address[63:AXI_ADDR_WIDTH] == 0) &&
        (shadow_ring_last_address[64:AXI_ADDR_WIDTH] == 0);
    assign address_valid = shadow_address_fits &&
                                (ring_base_address[2:0] == 3'b000);
    assign config_valid =
        (ring_size_bytes != 0) &&
        (block_size_bytes >= 32'd8) &&
        (ring_capacity_blocks != 0) &&
        (ring_size_bytes >= block_size_bytes) &&
        address_valid &&
        ((ring_size_bytes[2:0]) == 3'b000) &&
        ((block_size_bytes[2:0]) == 3'b000) &&
        (shadow_ring_geometry_product == {32'd0, ring_size_bytes});

    always @(posedge clk) begin
        if (rst) begin
            ring_base_lo         <= 32'd0;
            ring_base_hi         <= 32'd0;
            ring_size_bytes    <= 32'd0;
            block_size_bytes     <= 32'd0;
            ring_capacity_blocks <= 32'd0;
            irq_completion_interval_blocks   <= 32'd1;
        end else if (reg_wr_en) begin
            case (reg_wr_addr)
                `DDRW_REG_RING_BASE_ADDR_LO:
                    ring_base_lo <= apply_wstrb(
                        ring_base_lo, reg_wr_data, reg_wr_strb);
                `DDRW_REG_RING_BASE_ADDR_HI:
                    ring_base_hi <= apply_wstrb(
                        ring_base_hi, reg_wr_data, reg_wr_strb);
                `DDRW_REG_RING_SIZE_BYTES:
                    ring_size_bytes <= apply_wstrb(
                        ring_size_bytes, reg_wr_data, reg_wr_strb);
                `DDRW_REG_BLOCK_SIZE_BYTES:
                    block_size_bytes <= apply_wstrb(
                        block_size_bytes, reg_wr_data, reg_wr_strb);
                `DDRW_REG_RING_CAPACITY_BLOCKS:
                    ring_capacity_blocks <= apply_wstrb(
                        ring_capacity_blocks, reg_wr_data,
                        reg_wr_strb);
                `DDRW_REG_IRQ_COMPLETION_INTERVAL_BLOCKS:
                    irq_completion_interval_blocks <= apply_wstrb(
                        irq_completion_interval_blocks, reg_wr_data,
                        reg_wr_strb);
                default: begin end
            endcase
        end
    end

endmodule
`default_nettype wire
