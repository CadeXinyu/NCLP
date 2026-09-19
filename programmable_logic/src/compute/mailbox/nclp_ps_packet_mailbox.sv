`timescale 1ns/1ps
`default_nettype none

// Fixed-size PS control mailbox core for the SFP command path.
//
// RX accepts a header-stripped 64-byte target-2 command and publishes it to
// software only after all eight full-width AXI-stream beats have arrived.
// TX snapshots a 40-byte reply from AXI-Lite staging registers and retains the
// queued packet until the transport reports that the local Aurora TX interface
// accepted its final PS-owned beat.  An interrupted attempt is drained and
// replayed from beat zero.
module nclp_ps_packet_mailbox #(
    parameter integer C_S00_AXI_DATA_WIDTH = 32,
    parameter integer C_S00_AXI_ADDR_WIDTH = 12
) (
    input  wire transport_up,
    input  wire tx_packet_accepted_pulse,
    input  wire tx_packet_interrupted_pulse,
    output wire mailbox_irq,

    input  wire s00_axi_aclk,
    input  wire s00_axi_aresetn,

    input  wire [C_S00_AXI_ADDR_WIDTH-1:0] s00_axi_awaddr,
    input  wire [2:0] s00_axi_awprot,
    input  wire s00_axi_awvalid,
    output wire s00_axi_awready,
    input  wire [C_S00_AXI_DATA_WIDTH-1:0] s00_axi_wdata,
    input  wire [(C_S00_AXI_DATA_WIDTH/8)-1:0] s00_axi_wstrb,
    input  wire s00_axi_wvalid,
    output wire s00_axi_wready,
    output wire [1:0] s00_axi_bresp,
    output wire s00_axi_bvalid,
    input  wire s00_axi_bready,
    input  wire [C_S00_AXI_ADDR_WIDTH-1:0] s00_axi_araddr,
    input  wire [2:0] s00_axi_arprot,
    input  wire s00_axi_arvalid,
    output wire s00_axi_arready,
    output wire [C_S00_AXI_DATA_WIDTH-1:0] s00_axi_rdata,
    output wire [1:0] s00_axi_rresp,
    output wire s00_axi_rvalid,
    input  wire s00_axi_rready,

    input  wire [63:0] s_axis_rx_tdata,
    input  wire [7:0] s_axis_rx_tkeep,
    input  wire s_axis_rx_tvalid,
    output wire s_axis_rx_tready,
    input  wire s_axis_rx_tlast,

    output wire [63:0] m_axis_tx_tdata,
    output wire [7:0] m_axis_tx_tkeep,
    output wire m_axis_tx_tvalid,
    input  wire m_axis_tx_tready,
    output wire m_axis_tx_tlast
);

    localparam [31:0] BLOCK_ID_VALUE = 32'h4e50_4d42; // "NPMB"
    localparam [31:0] ABI_VERSION_VALUE = 32'h0002_0000;
    localparam [31:0] CAPABILITIES_VALUE = 32'h0000_003f;
    localparam [31:0] INFO_VALUE = 32'h0028_0040; // TX=40 bytes, RX=64 bytes
    localparam integer MAILBOX_SLOT_COUNT = 2;
    localparam integer RX_PACKET_BEATS = 8;
    localparam integer TX_PACKET_BEATS = 5;
    localparam integer TX_STAGING_WORDS = 10;
    localparam [1:0] FULL_SLOT_COUNT = MAILBOX_SLOT_COUNT;
    localparam [2:0] RX_LAST_BEAT_INDEX = RX_PACKET_BEATS - 1;
    localparam [2:0] TX_LAST_BEAT_INDEX = TX_PACKET_BEATS - 1;

    localparam [C_S00_AXI_ADDR_WIDTH-1:0] ADDR_BLOCK_ID = 12'h000;
    localparam [C_S00_AXI_ADDR_WIDTH-1:0] ADDR_ABI_VERSION = 12'h004;
    localparam [C_S00_AXI_ADDR_WIDTH-1:0] ADDR_CAPABILITIES = 12'h008;
    localparam [C_S00_AXI_ADDR_WIDTH-1:0] ADDR_INFO = 12'h00c;
    localparam [C_S00_AXI_ADDR_WIDTH-1:0] ADDR_STATUS = 12'h010;
    localparam [C_S00_AXI_ADDR_WIDTH-1:0] ADDR_IRQ_ENABLE = 12'h014;
    localparam [C_S00_AXI_ADDR_WIDTH-1:0] ADDR_COMMAND = 12'h018;
    localparam [C_S00_AXI_ADDR_WIDTH-1:0] ADDR_ERROR_STATUS = 12'h01c;
    localparam [C_S00_AXI_ADDR_WIDTH-1:0] ADDR_RX_PACKET_COUNT = 12'h020;
    localparam [C_S00_AXI_ADDR_WIDTH-1:0] ADDR_RX_OVERFLOW_COUNT = 12'h024;
    localparam [C_S00_AXI_ADDR_WIDTH-1:0] ADDR_RX_MALFORMED_COUNT = 12'h028;
    localparam [C_S00_AXI_ADDR_WIDTH-1:0] ADDR_TX_AURORA_ACCEPTED_COUNT = 12'h02c;
    localparam [C_S00_AXI_ADDR_WIDTH-1:0] ADDR_TX_REJECT_COUNT = 12'h030;
    localparam [C_S00_AXI_ADDR_WIDTH-1:0] ADDR_TX_REPLAY_COUNT = 12'h034;
    localparam [C_S00_AXI_ADDR_WIDTH-1:0] ADDR_RX_DATA_BASE = 12'h040;
    localparam [C_S00_AXI_ADDR_WIDTH-1:0] ADDR_RX_DATA_LAST = 12'h07c;
    localparam [C_S00_AXI_ADDR_WIDTH-1:0] ADDR_TX_DATA_BASE = 12'h080;
    localparam [C_S00_AXI_ADDR_WIDTH-1:0] ADDR_TX_DATA_LAST = 12'h0a4;

    localparam [4:0] ERROR_RX_MALFORMED = 5'b00001;
    localparam [4:0] ERROR_RX_OVERFLOW = 5'b00010;
    localparam [4:0] ERROR_RX_POP_EMPTY = 5'b00100;
    localparam [4:0] ERROR_TX_COMMIT_FULL = 5'b01000;
    localparam [4:0] ERROR_TX_FEEDBACK = 5'b10000;

    wire reg_wr_en;
    wire [C_S00_AXI_ADDR_WIDTH-1:0] reg_wr_addr;
    wire [31:0] reg_wr_data;
    wire [3:0] reg_wr_strb;
    wire [C_S00_AXI_ADDR_WIDTH-1:0] reg_rd_addr;
    reg [31:0] reg_rd_data;

    reg [2:0] irq_enable;
    reg [4:0] error_status;
    reg tx_aurora_accepted_sticky;

    reg [31:0] rx_packet_count;
    reg [31:0] rx_overflow_count;
    reg [31:0] rx_malformed_count;
    reg [31:0] tx_aurora_accepted_count;
    reg [31:0] tx_reject_count;
    reg [31:0] tx_replay_count;

    // Flatten each two-slot packet RAM so slot selection stays explicit.
    (* ram_style = "registers" *)
    reg [63:0] rx_packet_slots
        [0:(MAILBOX_SLOT_COUNT*RX_PACKET_BEATS)-1];
    reg [63:0] rx_packet_prefix [0:RX_PACKET_BEATS-2];
    reg rx_read_slot;
    reg rx_write_slot;
    reg [1:0] rx_count;
    reg [2:0] rx_beat_index;
    reg rx_drop_packet;

    reg [31:0] tx_staging [0:TX_STAGING_WORDS-1];
    (* ram_style = "registers" *)
    reg [63:0] tx_packet_slots
        [0:(MAILBOX_SLOT_COUNT*TX_PACKET_BEATS)-1];
    reg tx_read_slot;
    reg tx_write_slot;
    reg [1:0] tx_count;
    reg [2:0] tx_beat_index;
    reg tx_wait_aurora_acceptance;
    reg tx_attempt_interrupted;
    reg tx_replay_pending;
    reg tx_replay_payload_started;
    reg transport_up_prev;

    wire command_write = reg_wr_en && reg_wr_addr == ADDR_COMMAND &&
                         reg_wr_strb[0];
    wire command_rx_pop = command_write && reg_wr_data[0];
    wire command_tx_commit = command_write && reg_wr_data[1];
    wire command_clear_tx_aurora_accepted = command_write && reg_wr_data[2];
    wire command_clear_diagnostics = command_write && reg_wr_data[3];

    wire rx_transfer = s_axis_rx_tvalid && s_axis_rx_tready;
    wire rx_encoding_error = s_axis_rx_tkeep != 8'hff;
    wire rx_length_error =
        (rx_beat_index != RX_LAST_BEAT_INDEX && s_axis_rx_tlast) ||
        (rx_beat_index == RX_LAST_BEAT_INDEX && !s_axis_rx_tlast);
    // S_AXIS_RX is fed only from the RX router's committed-packet output.
    // Once that router exposes a packet, a later physical link transition must
    // not interrupt this internal transfer.
    wire rx_malformed_event = rx_transfer && !rx_drop_packet &&
                              (rx_encoding_error || rx_length_error);
    wire rx_commit_event = rx_transfer && !rx_drop_packet &&
                           !rx_encoding_error &&
                           rx_beat_index == RX_LAST_BEAT_INDEX &&
                           s_axis_rx_tlast;
    wire rx_pop_accepted = command_rx_pop && rx_count != 0;
    wire rx_enqueue_accepted = rx_commit_event &&
                               (rx_count != FULL_SLOT_COUNT ||
                                rx_pop_accepted);
    wire rx_overflow_event = rx_commit_event && !rx_enqueue_accepted;
    wire rx_pop_empty_event = command_rx_pop && rx_count == 0;

    wire tx_final_transfer = m_axis_tx_tvalid && m_axis_tx_tready &&
                             tx_beat_index == TX_LAST_BEAT_INDEX;
    wire tx_link_lost_while_waiting =
        tx_wait_aurora_acceptance && transport_up_prev && !transport_up;
    wire tx_replay_request =
        (tx_packet_interrupted_pulse ||
         tx_link_lost_while_waiting) && tx_count != 0;
    wire tx_attempt_was_interrupted = tx_attempt_interrupted ||
                                tx_replay_request;
    // A link can fall after the complete reply entered the TX CDC but before
    // its Aurora acceptance crosses back.  While the transport remains down,
    // that late acceptance still belongs to the front packet and safely
    // cancels the rewind.  Once the transport returns, its retry header may
    // already be in flight before the mailbox sees a payload handshake, so an
    // older acceptance must no longer retire the packet.
    wire tx_acceptance_matches_front_packet =
        tx_packet_accepted_pulse && tx_count != 0 &&
        (tx_wait_aurora_acceptance ||
         (tx_final_transfer && !tx_attempt_was_interrupted) ||
         (tx_replay_pending && !transport_up && !tx_replay_payload_started));
    wire tx_commit_accepted = command_tx_commit &&
        (tx_count != FULL_SLOT_COUNT ||
         tx_acceptance_matches_front_packet);
    wire tx_commit_full_event = command_tx_commit && !tx_commit_accepted;
    wire tx_feedback_error =
        (tx_packet_accepted_pulse && !tx_acceptance_matches_front_packet) ||
        (tx_packet_interrupted_pulse && tx_count == 0);

    wire tx_packet_output_valid = tx_count != 0 && !tx_wait_aurora_acceptance;

    wire error_pending = |error_status;
    wire [31:0] status_value = {
        19'd0,
        error_pending,
        tx_count,
        rx_count,
        tx_replay_pending,
        tx_aurora_accepted_sticky,
        transport_up,
        tx_packet_output_valid,
        tx_count != 0,
        tx_count != FULL_SLOT_COUNT,
        rx_count == FULL_SLOT_COUNT,
        rx_count != 0
    };

    assign s_axis_rx_tready = s00_axi_aresetn;
    assign m_axis_tx_tdata = tx_packet_slots[
        (tx_read_slot ? TX_PACKET_BEATS : 0) + tx_beat_index];
    assign m_axis_tx_tkeep = 8'hff;
    assign m_axis_tx_tvalid = s00_axi_aresetn && tx_packet_output_valid;
    assign m_axis_tx_tlast = tx_beat_index == TX_LAST_BEAT_INDEX;

    assign mailbox_irq = s00_axi_aresetn &&
        ((irq_enable[0] && rx_count != 0) ||
         (irq_enable[1] && tx_aurora_accepted_sticky) ||
         (irq_enable[2] && error_pending));

    integer rx_word_index;
    integer tx_word_index;
    integer byte_index;
    reg [4:0] error_events;
    reg [4:0] error_clear_mask;

    always @* begin
        error_events = 5'd0;
        if (rx_malformed_event)
            error_events = error_events | ERROR_RX_MALFORMED;
        if (rx_overflow_event)
            error_events = error_events | ERROR_RX_OVERFLOW;
        if (rx_pop_empty_event)
            error_events = error_events | ERROR_RX_POP_EMPTY;
        if (tx_commit_full_event)
            error_events = error_events | ERROR_TX_COMMIT_FULL;
        if (tx_feedback_error)
            error_events = error_events | ERROR_TX_FEEDBACK;

        error_clear_mask = 5'd0;
        if (reg_wr_en && reg_wr_addr == ADDR_ERROR_STATUS && reg_wr_strb[0])
            error_clear_mask = reg_wr_data[4:0];
    end

    always @(posedge s00_axi_aclk) begin
        if (!s00_axi_aresetn) begin
            irq_enable <= 3'd0;
            error_status <= 5'd0;
            tx_aurora_accepted_sticky <= 1'b0;
            rx_packet_count <= 32'd0;
            rx_overflow_count <= 32'd0;
            rx_malformed_count <= 32'd0;
            tx_aurora_accepted_count <= 32'd0;
            tx_reject_count <= 32'd0;
            tx_replay_count <= 32'd0;

            rx_read_slot <= 1'b0;
            rx_write_slot <= 1'b0;
            rx_count <= 2'd0;
            rx_beat_index <= 3'd0;
            rx_drop_packet <= 1'b0;

            tx_read_slot <= 1'b0;
            tx_write_slot <= 1'b0;
            tx_count <= 2'd0;
            tx_beat_index <= 3'd0;
            tx_wait_aurora_acceptance <= 1'b0;
            tx_attempt_interrupted <= 1'b0;
            tx_replay_pending <= 1'b0;
            tx_replay_payload_started <= 1'b0;
            transport_up_prev <= 1'b0;

            for (tx_word_index = 0; tx_word_index < TX_STAGING_WORDS;
                 tx_word_index = tx_word_index + 1)
                tx_staging[tx_word_index] <= 32'd0;
        end else begin
            transport_up_prev <= transport_up;

            if (reg_wr_en && reg_wr_addr == ADDR_IRQ_ENABLE &&
                reg_wr_strb[0])
                irq_enable <= reg_wr_data[2:0];

            if (reg_wr_en && reg_wr_addr >= ADDR_TX_DATA_BASE &&
                reg_wr_addr <= ADDR_TX_DATA_LAST &&
                reg_wr_addr[1:0] == 2'b00) begin
                for (byte_index = 0; byte_index < 4;
                     byte_index = byte_index + 1) begin
                    if (reg_wr_strb[byte_index])
                        tx_staging[(reg_wr_addr - ADDR_TX_DATA_BASE) >> 2]
                            [byte_index*8 +: 8] <=
                            reg_wr_data[byte_index*8 +: 8];
                end
            end

            error_status <= (error_status & ~error_clear_mask) | error_events;
            if (command_clear_tx_aurora_accepted)
                tx_aurora_accepted_sticky <= 1'b0;
            if (tx_acceptance_matches_front_packet)
                tx_aurora_accepted_sticky <= 1'b1;

            if (command_clear_diagnostics) begin
                rx_packet_count <= rx_enqueue_accepted ? 32'd1 : 32'd0;
                rx_overflow_count <= rx_overflow_event ? 32'd1 : 32'd0;
                rx_malformed_count <= rx_malformed_event ? 32'd1 : 32'd0;
                tx_aurora_accepted_count <=
                    tx_acceptance_matches_front_packet ? 32'd1 : 32'd0;
                tx_reject_count <= tx_commit_full_event ? 32'd1 : 32'd0;
                tx_replay_count <= tx_replay_request ? 32'd1 : 32'd0;
            end else begin
                if (rx_enqueue_accepted)
                    rx_packet_count <= rx_packet_count + 1'b1;
                if (rx_overflow_event)
                    rx_overflow_count <= rx_overflow_count + 1'b1;
                if (rx_malformed_event)
                    rx_malformed_count <= rx_malformed_count + 1'b1;
                if (tx_acceptance_matches_front_packet)
                    tx_aurora_accepted_count <=
                        tx_aurora_accepted_count + 1'b1;
                if (tx_commit_full_event)
                    tx_reject_count <= tx_reject_count + 1'b1;
                if (tx_replay_request)
                    tx_replay_count <= tx_replay_count + 1'b1;
            end

            // The upstream RX router exposes only complete committed packets,
            // so parsing continues independently of physical-link status.
            if (rx_transfer) begin
                if (rx_drop_packet) begin
                    if (s_axis_rx_tlast) begin
                        rx_drop_packet <= 1'b0;
                        rx_beat_index <= 3'd0;
                    end
                end else if (rx_encoding_error || rx_length_error) begin
                    rx_beat_index <= 3'd0;
                    rx_drop_packet <= !s_axis_rx_tlast;
                end else if (rx_beat_index == RX_LAST_BEAT_INDEX) begin
                    rx_beat_index <= 3'd0;
                end else begin
                    rx_packet_prefix[rx_beat_index] <= s_axis_rx_tdata;
                    rx_beat_index <= rx_beat_index + 1'b1;
                end
            end

            if (rx_enqueue_accepted) begin
                for (rx_word_index = 0;
                     rx_word_index < RX_PACKET_BEATS-1;
                     rx_word_index = rx_word_index + 1)
                    rx_packet_slots[(rx_write_slot ? RX_PACKET_BEATS : 0) +
                                     rx_word_index] <=
                        rx_packet_prefix[rx_word_index];
                rx_packet_slots[(rx_write_slot ? RX_PACKET_BEATS : 0) +
                                RX_PACKET_BEATS-1] <=
                    s_axis_rx_tdata;
                rx_write_slot <= ~rx_write_slot;
            end
            if (rx_pop_accepted)
                rx_read_slot <= ~rx_read_slot;
            case ({rx_enqueue_accepted, rx_pop_accepted})
                2'b10: rx_count <= rx_count + 1'b1;
                2'b01: rx_count <= rx_count - 1'b1;
                default: rx_count <= rx_count;
            endcase

            if (tx_commit_accepted) begin
                for (tx_word_index = 0; tx_word_index < TX_PACKET_BEATS;
                     tx_word_index = tx_word_index + 1)
                    tx_packet_slots[(tx_write_slot ? TX_PACKET_BEATS : 0) +
                                     tx_word_index] <= {
                        tx_staging[tx_word_index*2 + 1],
                        tx_staging[tx_word_index*2]
                    };
                tx_write_slot <= ~tx_write_slot;
            end
            if (tx_acceptance_matches_front_packet)
                tx_read_slot <= ~tx_read_slot;
            case ({tx_commit_accepted, tx_acceptance_matches_front_packet})
                2'b10: tx_count <= tx_count + 1'b1;
                2'b01: tx_count <= tx_count - 1'b1;
                default: tx_count <= tx_count;
            endcase

            if (tx_acceptance_matches_front_packet) begin
                tx_beat_index <= 3'd0;
                tx_wait_aurora_acceptance <= 1'b0;
                tx_attempt_interrupted <= 1'b0;
                tx_replay_pending <= 1'b0;
                tx_replay_payload_started <= 1'b0;
            end else begin
                if (tx_replay_request) begin
                    tx_replay_pending <= 1'b1;
                    tx_replay_payload_started <= 1'b0;
                    if (tx_wait_aurora_acceptance) begin
                        tx_wait_aurora_acceptance <= 1'b0;
                        tx_beat_index <= 3'd0;
                        tx_attempt_interrupted <= 1'b0;
                    end else begin
                        tx_attempt_interrupted <= 1'b1;
                    end
                end

                if (m_axis_tx_tvalid && m_axis_tx_tready) begin
                    if (tx_replay_pending && !tx_replay_request)
                        tx_replay_payload_started <= 1'b1;
                    if (tx_beat_index == TX_LAST_BEAT_INDEX) begin
                        tx_beat_index <= 3'd0;
                        if (tx_attempt_was_interrupted) begin
                            tx_wait_aurora_acceptance <= 1'b0;
                            tx_attempt_interrupted <= 1'b0;
                            tx_replay_pending <= 1'b1;
                        end else begin
                            tx_wait_aurora_acceptance <= 1'b1;
                        end
                    end else begin
                        tx_beat_index <= tx_beat_index + 1'b1;
                    end
                end
            end
        end
    end

    always @* begin
        reg_rd_data = 32'd0;
        case (reg_rd_addr)
            ADDR_BLOCK_ID: reg_rd_data = BLOCK_ID_VALUE;
            ADDR_ABI_VERSION: reg_rd_data = ABI_VERSION_VALUE;
            ADDR_CAPABILITIES: reg_rd_data = CAPABILITIES_VALUE;
            ADDR_INFO: reg_rd_data = INFO_VALUE;
            ADDR_STATUS: reg_rd_data = status_value;
            ADDR_IRQ_ENABLE: reg_rd_data = {29'd0, irq_enable};
            ADDR_ERROR_STATUS: reg_rd_data = {27'd0, error_status};
            ADDR_RX_PACKET_COUNT: reg_rd_data = rx_packet_count;
            ADDR_RX_OVERFLOW_COUNT: reg_rd_data = rx_overflow_count;
            ADDR_RX_MALFORMED_COUNT: reg_rd_data = rx_malformed_count;
            ADDR_TX_AURORA_ACCEPTED_COUNT:
                reg_rd_data = tx_aurora_accepted_count;
            ADDR_TX_REJECT_COUNT: reg_rd_data = tx_reject_count;
            ADDR_TX_REPLAY_COUNT: reg_rd_data = tx_replay_count;
            default: begin
                if (reg_rd_addr >= ADDR_RX_DATA_BASE &&
                    reg_rd_addr <= ADDR_RX_DATA_LAST &&
                    reg_rd_addr[1:0] == 2'b00 && rx_count != 0) begin
                    if (reg_rd_addr[2])
                        reg_rd_data = rx_packet_slots[
                            (rx_read_slot ? RX_PACKET_BEATS : 0) +
                            ((reg_rd_addr - ADDR_RX_DATA_BASE) >> 3)][63:32];
                    else
                        reg_rd_data = rx_packet_slots[
                            (rx_read_slot ? RX_PACKET_BEATS : 0) +
                            ((reg_rd_addr - ADDR_RX_DATA_BASE) >> 3)][31:0];
                end else if (reg_rd_addr >= ADDR_TX_DATA_BASE &&
                             reg_rd_addr <= ADDR_TX_DATA_LAST &&
                             reg_rd_addr[1:0] == 2'b00) begin
                    reg_rd_data = tx_staging[
                        (reg_rd_addr - ADDR_TX_DATA_BASE) >> 2];
                end
            end
        endcase
    end

    axil_register_slave #(
        .DATA_WIDTH(C_S00_AXI_DATA_WIDTH),
        .ADDR_WIDTH(C_S00_AXI_ADDR_WIDTH)
    ) registers (
        .clk(s00_axi_aclk),
        .resetn(s00_axi_aresetn),
        .s_axil_awaddr(s00_axi_awaddr),
        .s_axil_awprot(s00_axi_awprot),
        .s_axil_awvalid(s00_axi_awvalid),
        .s_axil_awready(s00_axi_awready),
        .s_axil_wdata(s00_axi_wdata),
        .s_axil_wstrb(s00_axi_wstrb),
        .s_axil_wvalid(s00_axi_wvalid),
        .s_axil_wready(s00_axi_wready),
        .s_axil_bresp(s00_axi_bresp),
        .s_axil_bvalid(s00_axi_bvalid),
        .s_axil_bready(s00_axi_bready),
        .s_axil_araddr(s00_axi_araddr),
        .s_axil_arprot(s00_axi_arprot),
        .s_axil_arvalid(s00_axi_arvalid),
        .s_axil_arready(s00_axi_arready),
        .s_axil_rdata(s00_axi_rdata),
        .s_axil_rresp(s00_axi_rresp),
        .s_axil_rvalid(s00_axi_rvalid),
        .s_axil_rready(s00_axi_rready),
        .reg_wr_en(reg_wr_en),
        .reg_wr_addr(reg_wr_addr),
        .reg_wr_data(reg_wr_data),
        .reg_wr_strb(reg_wr_strb),
        .reg_rd_addr(reg_rd_addr),
        .reg_rd_data(reg_rd_data)
    );

`ifndef SYNTHESIS
    initial begin
        if (C_S00_AXI_DATA_WIDTH != 32 || C_S00_AXI_ADDR_WIDTH != 12)
            $fatal(1, "nclp_ps_packet_mailbox requires 32-bit AXI-Lite and a 4KB aperture");
    end

    always @(posedge s00_axi_aclk) begin
        if (s00_axi_aresetn) begin
            if (rx_count > 2)
                $error("RX mailbox count exceeded two slots");
            if (tx_count > 2)
                $error("TX mailbox count exceeded two slots");
            if (m_axis_tx_tvalid && !m_axis_tx_tlast &&
                m_axis_tx_tkeep != 8'hff)
                $error("non-final mailbox TX beat has partial TKEEP");
        end
    end
`endif
endmodule

`default_nettype wire
