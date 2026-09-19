`timescale 1ns/1ps
`default_nettype none

// Select one consumer for the compact Intan compute stream.  PS changes the
// selection only while the complete compute path is idle, so this block needs
// no packet state of its own.
module intan_compute_path_selector (
    input  wire        resetn,
    input  wire        sfp_mode_selected,

    input  wire [15:0] s_axis_tdata,
    input  wire [1:0]  s_axis_tkeep,
    input  wire        s_axis_tvalid,
    output wire        s_axis_tready,
    input  wire        s_axis_tlast,

    output wire [15:0] m_axis_local_tdata,
    output wire [1:0]  m_axis_local_tkeep,
    output wire        m_axis_local_tvalid,
    input  wire        m_axis_local_tready,
    output wire        m_axis_local_tlast,

    output wire [15:0] m_axis_sfp_tdata,
    output wire [1:0]  m_axis_sfp_tkeep,
    output wire        m_axis_sfp_tvalid,
    input  wire        m_axis_sfp_tready,
    output wire        m_axis_sfp_tlast
);
    assign m_axis_local_tdata  = s_axis_tdata;
    assign m_axis_local_tkeep  = s_axis_tkeep;
    assign m_axis_local_tvalid = resetn && s_axis_tvalid &&
                                 !sfp_mode_selected;
    assign m_axis_local_tlast  = s_axis_tlast;

    assign m_axis_sfp_tdata  = s_axis_tdata;
    assign m_axis_sfp_tkeep  = s_axis_tkeep;
    assign m_axis_sfp_tvalid = resetn && s_axis_tvalid &&
                               sfp_mode_selected;
    assign m_axis_sfp_tlast  = s_axis_tlast;

    assign s_axis_tready = resetn &&
        (sfp_mode_selected ? m_axis_sfp_tready : m_axis_local_tready);

`ifndef SYNTHESIS
    always @* begin
        assert (!(m_axis_local_tvalid && m_axis_sfp_tvalid))
            else $error("Intan compute beat offered to both paths");
    end
`endif
endmodule

`default_nettype wire
