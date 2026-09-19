`timescale 1ns/1ps
`default_nettype none

// Packet-level SFP transmitter.  It adds one routing header, then forwards an
// entire payload packet without interleaving.  Intan and PS payload bytes are
// never modified.  When both sources wait, grants alternate at TLAST.
module nclp_sfp_tx_arbiter (
    input  wire        clk,
    input  wire        resetn,
    input  wire        transport_up,
    input  wire [63:0] s_axis_intan_tdata,
    input  wire [7:0]  s_axis_intan_tkeep,
    input  wire        s_axis_intan_tvalid,
    output wire        s_axis_intan_tready,
    input  wire        s_axis_intan_tlast,

    input  wire [63:0] s_axis_ps_tdata,
    input  wire [7:0]  s_axis_ps_tkeep,
    input  wire        s_axis_ps_tvalid,
    output wire        s_axis_ps_tready,
    input  wire        s_axis_ps_tlast,

    output wire [63:0] m_axis_tdata,
    output wire [7:0]  m_axis_tkeep,
    output wire        m_axis_tvalid,
    input  wire        m_axis_tready,
    output wire        m_axis_tlast,
    output wire        m_axis_ps_owned,

    output wire        idle,
    output wire        intan_packet_active,
    output wire        ps_packet_active,
    output reg         intan_packet_enqueued_pulse,
    output reg         ps_packet_attempt_enqueued_pulse,
    output reg         interrupted_packet_pulse,
    output reg         ps_packet_interrupted_pulse
);
    localparam [1:0] IDLE    = 2'd0;
    localparam [1:0] HEADER  = 2'd1;
    localparam [1:0] PAYLOAD = 2'd2;
    localparam [1:0] DROP    = 2'd3;

    localparam [6:0] TARGET_INTAN_STREAM = 7'h01;
    localparam [6:0] TARGET_PS = 7'h02;
    localparam [7:0] PROTOCOL_VERSION = 8'h01;

    reg [1:0] state;
    reg selected_source_is_ps;
    reg previous_grant_was_ps;
    reg [7:0] routing_header_low_byte;

    wire choose_ps_source = s_axis_ps_tvalid &&
        (!s_axis_intan_tvalid || !previous_grant_was_ps);
    wire selected_valid = selected_source_is_ps ? s_axis_ps_tvalid :
                                                  s_axis_intan_tvalid;
    wire [63:0] selected_data = selected_source_is_ps ? s_axis_ps_tdata :
                                                       s_axis_intan_tdata;
    wire [7:0] selected_keep = selected_source_is_ps ? s_axis_ps_tkeep :
                                                      s_axis_intan_tkeep;
    wire selected_last = selected_source_is_ps ? s_axis_ps_tlast :
                                                 s_axis_intan_tlast;
    wire payload_transfer = state == PAYLOAD && transport_up &&
                            selected_valid && m_axis_tready;
    wire drop_transfer = state == DROP && selected_valid;

    assign m_axis_tdata = state == HEADER ?
        {48'd0, PROTOCOL_VERSION, routing_header_low_byte} : selected_data;
    assign m_axis_tkeep = state == HEADER ? 8'hff : selected_keep;
    assign m_axis_tvalid = resetn && transport_up &&
        ((state == HEADER) || (state == PAYLOAD && selected_valid));
    assign m_axis_tlast = state == PAYLOAD && selected_last;
    // This opaque per-beat sideband follows the packet through the transport
    // FIFO.  The transport uses it only to confirm a PS-owned TLAST at the
    // Aurora user interface; it never decodes the routing header.
    assign m_axis_ps_owned = selected_source_is_ps;

    assign s_axis_intan_tready = resetn && !selected_source_is_ps &&
        ((state == PAYLOAD && transport_up && m_axis_tready) ||
         (state == DROP));
    assign s_axis_ps_tready = resetn && selected_source_is_ps &&
        ((state == PAYLOAD && transport_up && m_axis_tready) ||
         (state == DROP));

    assign idle = state == IDLE;
    assign intan_packet_active = state != IDLE && !selected_source_is_ps;
    assign ps_packet_active = state != IDLE && selected_source_is_ps;

    always @(posedge clk) begin
        if (!resetn) begin
            state <= IDLE;
            selected_source_is_ps <= 1'b0;
            previous_grant_was_ps <= 1'b1;
            routing_header_low_byte <= 8'd0;
            intan_packet_enqueued_pulse <= 1'b0;
            ps_packet_attempt_enqueued_pulse <= 1'b0;
            interrupted_packet_pulse <= 1'b0;
            ps_packet_interrupted_pulse <= 1'b0;
        end else begin
            intan_packet_enqueued_pulse <= 1'b0;
            ps_packet_attempt_enqueued_pulse <= 1'b0;
            interrupted_packet_pulse <= 1'b0;
            ps_packet_interrupted_pulse <= 1'b0;

            case (state)
                IDLE: begin
                    // A lost link must not strand buffered acquisition frames.
                    // Drain only Intan packets while transport is down so its
                    // upstream packetizer can consume EOS and become idle.  A
                    // PS packet that has not started remains held for retry.
                    if (!transport_up && s_axis_intan_tvalid) begin
                        selected_source_is_ps <= 1'b0;
                        state <= DROP;
                    end else if (transport_up &&
                                 (s_axis_intan_tvalid || s_axis_ps_tvalid)) begin
                        selected_source_is_ps <= choose_ps_source;
                        routing_header_low_byte <= choose_ps_source ?
                            {1'b1, TARGET_PS} :
                            {1'b1, TARGET_INTAN_STREAM};
                        state <= HEADER;
                    end
                end

                HEADER: begin
                    // No payload beat has been consumed yet, so a disappearing
                    // link can preserve an untouched PS packet.  Intan instead
                    // drains through TLAST so queued frames and EOS cannot block
                    // a later switch back to local compute.
                    if (!transport_up)
                        state <= selected_source_is_ps ? IDLE : DROP;
                    else if (m_axis_tready)
                        state <= PAYLOAD;
                end

                PAYLOAD: begin
                    if (!transport_up) begin
                        // Aurora reset invalidates the physical packet.  Drain
                        // the remaining source packet so its tail cannot become
                        // a new packet after reconnection.
                        state <= DROP;
                        interrupted_packet_pulse <= 1'b1;
                        ps_packet_interrupted_pulse <= selected_source_is_ps;
                    end else if (payload_transfer && selected_last) begin
                        state <= IDLE;
                        previous_grant_was_ps <= selected_source_is_ps;
                        if (selected_source_is_ps)
                            ps_packet_attempt_enqueued_pulse <= 1'b1;
                        else
                            intan_packet_enqueued_pulse <= 1'b1;
                    end
                end

                DROP: begin
                    if (drop_transfer && selected_last) begin
                        state <= IDLE;
                        previous_grant_was_ps <= selected_source_is_ps;
                    end
                end

                default: state <= IDLE;
            endcase
        end
    end

`ifndef SYNTHESIS
    always @(posedge clk) begin
        if (resetn && payload_transfer && !selected_last)
            assert (selected_keep == 8'hff)
                else $error("non-final SFP payload beat has partial TKEEP");
        if (resetn && payload_transfer && selected_last)
            assert (selected_keep != 8'h00)
                else $error("final SFP payload beat has empty TKEEP");
    end
`endif
endmodule

`default_nettype wire
