`timescale 1ns / 1ps
`default_nettype none

// Collect packed DDR-writer entries and submit bounded AXI4 write bursts.
//
// The scheduler owns one local burst at a time.  A burst is closed before it
// can cross a 4 KiB address boundary or the current logical block boundary,
// and is also closed by the sixteenth beat or by the session EOS entry.  The
// corresponding commit is reported only after an OKAY B response.
module ddrw_burst_scheduler #(
    parameter integer AXI_ADDR_WIDTH = 49,
    parameter integer AXI_ID_WIDTH   = 1
)(
    input  wire                         clk,
    input  wire                         rst,
    input  wire                         clear,
    input  wire                         cancel,
    input  wire                         enable,

    input  wire [AXI_ADDR_WIDTH-1:0]    current_write_addr,
    input  wire [31:0]                  block_bytes_remaining,

    input  wire [63:0]                  entry_data,
    input  wire [3:0]                   entry_byte_count,
    input  wire                         entry_eos,
    input  wire                         entry_valid,
    output wire                         entry_ready,

    output reg                          commit_valid,
    output reg  [31:0]                  commit_bytes,
    output reg                          commit_block_end,
    output reg                          commit_eos,

    output reg                          fault_valid,
    output reg  [1:0]                   fault_bresp,
    output reg                          cancel_done,
    output wire                         busy,

    output wire [AXI_ID_WIDTH-1:0]      m_axi_awid,
    output wire [AXI_ADDR_WIDTH-1:0]    m_axi_awaddr,
    output wire [7:0]                   m_axi_awlen,
    output wire [2:0]                   m_axi_awsize,
    output wire [1:0]                   m_axi_awburst,
    output wire                         m_axi_awlock,
    output wire [3:0]                   m_axi_awcache,
    output wire [2:0]                   m_axi_awprot,
    output wire                         m_axi_awvalid,
    input  wire                         m_axi_awready,

    output wire [63:0]                  m_axi_wdata,
    output wire [7:0]                   m_axi_wstrb,
    output wire                         m_axi_wlast,
    output wire                         m_axi_wvalid,
    input  wire                         m_axi_wready,

    input  wire [AXI_ID_WIDTH-1:0]      m_axi_bid,
    input  wire [1:0]                   m_axi_bresp,
    input  wire                         m_axi_bvalid,
    output wire                         m_axi_bready
);

    localparam [2:0] STATE_COLLECT = 3'd0;
    localparam [2:0] STATE_ISSUE   = 3'd1;
    localparam [2:0] STATE_ACTIVE  = 3'd2;
    localparam [2:0] STATE_CLOSED  = 3'd3;
    localparam [2:0] STATE_FAULT   = 3'd4;

    reg [2:0] state;

    reg [63:0] burst_data [0:15];
    reg [7:0]  burst_strb [0:15];
    reg [4:0]  burst_beat_count;
    reg [4:0]  send_beat_index;
    reg [31:0] burst_byte_count;
    reg [AXI_ADDR_WIDTH-1:0] burst_addr;
    reg [31:0] burst_block_bytes_remaining;
    reg        burst_block_end;
    reg        burst_eos;
    reg        cancel_pending;
    reg        cancel_seen;

    wire entry_is_marker = entry_eos && (entry_byte_count == 4'd0);
    wire entry_is_full_data = (entry_byte_count == 4'd8);
    wire entry_is_partial_eos = entry_eos &&
                                ((entry_byte_count == 4'd2) ||
                                 (entry_byte_count == 4'd4) ||
                                 (entry_byte_count == 4'd6));
    wire entry_format_valid = entry_is_marker || entry_is_partial_eos ||
                              (entry_is_full_data && !entry_eos) ||
                              (entry_is_full_data && entry_eos);
    wire entry_has_data = (entry_byte_count != 4'd0);

    wire [AXI_ADDR_WIDTH-1:0] candidate_burst_addr =
        (burst_beat_count == 5'd0) ? current_write_addr : burst_addr;
    wire [31:0] candidate_block_remaining_bytes =
        (burst_beat_count == 5'd0) ? block_bytes_remaining :
                                     burst_block_bytes_remaining;
    wire [31:0] candidate_total_bytes =
        burst_byte_count + {28'd0, entry_byte_count};
    wire [12:0] candidate_page_end_offset_bytes =
        {1'b0, candidate_burst_addr[11:0]} +
        ({8'd0, burst_beat_count} << 3) +
        {9'd0, entry_byte_count};

    wire entry_fits_block = entry_is_marker ||
                            (candidate_total_bytes <= candidate_block_remaining_bytes);
    wire entry_fits_4k = entry_is_marker ||
                         (candidate_page_end_offset_bytes <= 13'd4096);
    wire entry_fits_buffer = entry_is_marker || (burst_beat_count < 5'd16);

    assign entry_ready = enable && !cancel && (state == STATE_COLLECT) &&
                         entry_format_valid && entry_fits_block &&
                         entry_fits_4k && entry_fits_buffer;

    wire entry_handshake = entry_valid && entry_ready;
    wire closes_at_beat_limit = entry_has_data &&
                                (burst_beat_count == 5'd15);
    wire closes_at_4k = entry_has_data &&
                        (({1'b0, candidate_burst_addr[11:0]} +
                          ({8'd0, burst_beat_count + 5'd1} << 3)) >= 13'd4096);
    wire closes_at_block_end = entry_has_data &&
                               (candidate_total_bytes == candidate_block_remaining_bytes);
    wire closes_burst = entry_eos || closes_at_beat_limit ||
                        closes_at_4k || closes_at_block_end;

    function automatic [7:0] strobe_for_byte_count(input [3:0] byte_count);
        begin
            case (byte_count)
                4'd2: strobe_for_byte_count = 8'h03;
                4'd4: strobe_for_byte_count = 8'h0F;
                4'd6: strobe_for_byte_count = 8'h3F;
                4'd8: strobe_for_byte_count = 8'hFF;
                default: strobe_for_byte_count = 8'h00;
            endcase
        end
    endfunction

    wire master_cmd_ready;
    wire master_data_ready;
    wire master_done_valid;
    wire [1:0] master_done_bresp;
    wire master_busy;

    wire master_cmd_valid = (state == STATE_ISSUE) && !cancel;
    wire master_data_valid = (state == STATE_ACTIVE) &&
                             (send_beat_index < burst_beat_count);
    wire master_command_handshake = master_cmd_valid && master_cmd_ready;
    wire master_data_handshake = master_data_valid && master_data_ready;

    assign busy = master_busy || (state == STATE_ISSUE) ||
                  (state == STATE_ACTIVE) ||
                  ((state == STATE_COLLECT) && (burst_beat_count != 5'd0));

    ddrw_axi_burst_master #(
        .AXI_ADDR_WIDTH(AXI_ADDR_WIDTH),
        .AXI_ID_WIDTH(AXI_ID_WIDTH)
    ) u_axi_burst_master (
        .clk(clk),
        .rst(rst),
        .cmd_addr(burst_addr),
        .cmd_beats(burst_beat_count),
        .cmd_valid(master_cmd_valid),
        .cmd_ready(master_cmd_ready),
        .s_data(burst_data[send_beat_index[3:0]]),
        .s_strb(burst_strb[send_beat_index[3:0]]),
        .s_valid(master_data_valid),
        .s_ready(master_data_ready),
        .done_valid(master_done_valid),
        .done_bresp(master_done_bresp),
        .busy(master_busy),
        .m_axi_awid(m_axi_awid),
        .m_axi_awaddr(m_axi_awaddr),
        .m_axi_awlen(m_axi_awlen),
        .m_axi_awsize(m_axi_awsize),
        .m_axi_awburst(m_axi_awburst),
        .m_axi_awlock(m_axi_awlock),
        .m_axi_awcache(m_axi_awcache),
        .m_axi_awprot(m_axi_awprot),
        .m_axi_awvalid(m_axi_awvalid),
        .m_axi_awready(m_axi_awready),
        .m_axi_wdata(m_axi_wdata),
        .m_axi_wstrb(m_axi_wstrb),
        .m_axi_wlast(m_axi_wlast),
        .m_axi_wvalid(m_axi_wvalid),
        .m_axi_wready(m_axi_wready),
        .m_axi_bid(m_axi_bid),
        .m_axi_bresp(m_axi_bresp),
        .m_axi_bvalid(m_axi_bvalid),
        .m_axi_bready(m_axi_bready)
    );

    always @(posedge clk) begin
        if (rst) begin
            state                       <= STATE_COLLECT;
            burst_beat_count            <= 5'd0;
            send_beat_index             <= 5'd0;
            burst_byte_count            <= 32'd0;
            burst_addr                  <= {AXI_ADDR_WIDTH{1'b0}};
            burst_block_bytes_remaining <= 32'd0;
            burst_block_end             <= 1'b0;
            burst_eos                   <= 1'b0;
            commit_valid                <= 1'b0;
            commit_bytes                <= 32'd0;
            commit_block_end            <= 1'b0;
            commit_eos                  <= 1'b0;
            fault_valid                 <= 1'b0;
            fault_bresp                 <= 2'b00;
            cancel_done                 <= 1'b0;
            cancel_pending              <= 1'b0;
            cancel_seen                 <= 1'b0;
        end else begin
            commit_valid <= 1'b0;
            fault_valid  <= 1'b0;
            cancel_done  <= 1'b0;

            if (!cancel) begin
                cancel_seen <= 1'b0;
            end

            if (clear && !busy) begin
                state                       <= STATE_COLLECT;
                burst_beat_count            <= 5'd0;
                send_beat_index             <= 5'd0;
                burst_byte_count            <= 32'd0;
                burst_addr                  <= {AXI_ADDR_WIDTH{1'b0}};
                burst_block_bytes_remaining <= 32'd0;
                burst_block_end             <= 1'b0;
                burst_eos                   <= 1'b0;
                commit_bytes                <= 32'd0;
                commit_block_end            <= 1'b0;
                commit_eos                  <= 1'b0;
                fault_bresp                 <= 2'b00;
                cancel_pending              <= 1'b0;
                cancel_seen                 <= cancel;
            end else begin
                case (state)
                    STATE_COLLECT: begin
                        if (cancel && !cancel_seen) begin
                            burst_beat_count <= 5'd0;
                            send_beat_index  <= 5'd0;
                            burst_byte_count <= 32'd0;
                            burst_block_end  <= 1'b0;
                            burst_eos        <= 1'b0;
                            cancel_seen      <= 1'b1;
                            cancel_done      <= 1'b1;
                            state            <= STATE_CLOSED;
                        end else if (entry_handshake) begin
                            if (entry_is_marker) begin
                                if (burst_beat_count == 5'd0) begin
                                    commit_valid     <= 1'b1;
                                    commit_bytes     <= 32'd0;
                                    commit_block_end <= 1'b0;
                                    commit_eos       <= 1'b1;
                                    state            <= STATE_CLOSED;
                                end else begin
                                    burst_eos <= 1'b1;
                                    state     <= STATE_ISSUE;
                                end
                            end else begin
                                burst_data[burst_beat_count[3:0]] <= entry_data;
                                burst_strb[burst_beat_count[3:0]] <=
                                    strobe_for_byte_count(entry_byte_count);
                                burst_beat_count <= burst_beat_count + 5'd1;
                                burst_byte_count <= candidate_total_bytes;

                                if (burst_beat_count == 5'd0) begin
                                    burst_addr <= current_write_addr;
                                    burst_block_bytes_remaining <=
                                        block_bytes_remaining;
                                end

                                if (closes_burst) begin
                                    burst_block_end <= closes_at_block_end;
                                    burst_eos       <= entry_eos;
                                    state           <= STATE_ISSUE;
                                end
                            end
                        end
                    end

                    STATE_ISSUE: begin
                        if (cancel && !cancel_seen) begin
                            burst_beat_count <= 5'd0;
                            send_beat_index  <= 5'd0;
                            burst_byte_count <= 32'd0;
                            burst_block_end  <= 1'b0;
                            burst_eos        <= 1'b0;
                            cancel_seen      <= 1'b1;
                            cancel_done      <= 1'b1;
                            state            <= STATE_CLOSED;
                        end else if (master_command_handshake) begin
                            send_beat_index <= 5'd0;
                            state           <= STATE_ACTIVE;
                        end
                    end

                    STATE_ACTIVE: begin
                        if (cancel && !cancel_seen) begin
                            cancel_seen    <= 1'b1;
                            cancel_pending <= 1'b1;
                        end

                        if (master_data_handshake &&
                            (send_beat_index + 5'd1 < burst_beat_count)) begin
                            send_beat_index <= send_beat_index + 5'd1;
                        end

                        if (master_done_valid) begin
                            if (master_done_bresp == 2'b00) begin
                                commit_valid     <= 1'b1;
                                commit_bytes     <= burst_byte_count;
                                commit_block_end <= burst_block_end;
                                commit_eos       <= burst_eos;
                                state <= (burst_eos || cancel_pending ||
                                          (cancel && !cancel_seen)) ?
                                         STATE_CLOSED : STATE_COLLECT;
                            end else begin
                                fault_valid <= 1'b1;
                                fault_bresp <= master_done_bresp;
                                state       <= STATE_FAULT;
                            end

                            if (cancel_pending || (cancel && !cancel_seen)) begin
                                cancel_done <= 1'b1;
                            end

                            burst_beat_count <= 5'd0;
                            send_beat_index  <= 5'd0;
                            burst_byte_count <= 32'd0;
                            burst_block_end  <= 1'b0;
                            burst_eos        <= 1'b0;
                            cancel_pending   <= 1'b0;
                        end
                    end

                    STATE_CLOSED: begin
                        // Hold input closed until the session controller clears
                        // this scheduler for the next session.
                        if (cancel && !cancel_seen) begin
                            cancel_seen <= 1'b1;
                            cancel_done <= 1'b1;
                        end
                    end

                    STATE_FAULT: begin
                        // The failed burst is intentionally not committed.
                        // Recovery requires an explicit clear while idle.
                        if (cancel && !cancel_seen) begin
                            cancel_seen <= 1'b1;
                            cancel_done <= 1'b1;
                        end
                    end

                    default: state <= STATE_FAULT;
                endcase
            end
        end
    end

`ifndef SYNTHESIS
    always @(posedge clk) begin
        if (!rst) begin
            if (clear && busy) begin
                $error("ddrw_burst_scheduler: clear is only legal while idle");
            end
            if (entry_valid && (state == STATE_COLLECT) && enable &&
                !entry_format_valid) begin
                $error("ddrw_burst_scheduler: invalid entry byte count/EOS combination");
            end
            if (entry_handshake && entry_has_data &&
                (candidate_burst_addr[2:0] != 3'b000)) begin
                $error("ddrw_burst_scheduler: AXI write address must be 8-byte aligned");
            end
            if (master_command_handshake &&
                ((burst_beat_count == 5'd0) || (burst_beat_count > 5'd16))) begin
                $error("ddrw_burst_scheduler: invalid scheduled burst length");
            end
        end
    end
`endif

endmodule

`default_nettype wire
