`timescale 1ns / 1ps
`default_nettype none

`include "ddrw_registers.vh"

// Intan AXI-Stream to DDR circular writer, ABI v2.
//
// Public stream contract:
//   payload : TKEEP=2'b11, TLAST=0
//   EOS     : TKEEP=2'b00, TLAST=1
//
// Payload is packed into 64-bit words and buffered in one private UltraRAM.
// The burst scheduler writes up to sixteen beats per AXI transaction and never
// crosses a 4-KiB, logical-block, or ring boundary.  A session is complete only
// after the ordered EOS and every preceding AXI write response have committed.
module axis_ddr_circular_writer #(
    parameter integer C_S00_AXI_DATA_WIDTH = 32,
    parameter integer C_S00_AXI_ADDR_WIDTH = 12,
    parameter integer AXIS_DATA_WIDTH       = 16,
    parameter integer AXIS_KEEP_WIDTH       = 2,
    parameter integer AXI_DATA_WIDTH        = 64,
    parameter integer AXI_STRB_WIDTH        = 8,
    parameter integer AXI_ADDR_WIDTH        = 49,
    parameter integer AXI_ID_WIDTH          = 1,
    parameter integer AXI_MAX_BURST_BEATS     = 16,
    parameter integer FIFO_DEPTH_ENTRIES            = 4096
)(
    input  wire                              s00_axi_aclk,
    input  wire                              s00_axi_aresetn,

    input  wire [C_S00_AXI_ADDR_WIDTH-1:0]   s00_axi_awaddr,
    input  wire [2:0]                        s00_axi_awprot,
    input  wire                              s00_axi_awvalid,
    output wire                              s00_axi_awready,
    input  wire [C_S00_AXI_DATA_WIDTH-1:0]   s00_axi_wdata,
    input  wire [(C_S00_AXI_DATA_WIDTH/8)-1:0] s00_axi_wstrb,
    input  wire                              s00_axi_wvalid,
    output wire                              s00_axi_wready,
    output wire [1:0]                        s00_axi_bresp,
    output wire                              s00_axi_bvalid,
    input  wire                              s00_axi_bready,
    input  wire [C_S00_AXI_ADDR_WIDTH-1:0]   s00_axi_araddr,
    input  wire [2:0]                        s00_axi_arprot,
    input  wire                              s00_axi_arvalid,
    output wire                              s00_axi_arready,
    output wire [C_S00_AXI_DATA_WIDTH-1:0]   s00_axi_rdata,
    output wire [1:0]                        s00_axi_rresp,
    output wire                              s00_axi_rvalid,
    input  wire                              s00_axi_rready,

    input  wire [AXIS_DATA_WIDTH-1:0]        s_axis_tdata,
    input  wire [AXIS_KEEP_WIDTH-1:0]        s_axis_tkeep,
    input  wire                              s_axis_tvalid,
    output wire                              s_axis_tready,
    input  wire                              s_axis_tlast,

    output wire                              transfer_irq,
    // Dedicated fault level for local PL health/error indication. Unlike
    // transfer_irq, this never asserts for normal block-threshold or EOS events.
    output wire                              transfer_fault,

    output wire [AXI_ID_WIDTH-1:0]           m_axi_awid,
    output wire [AXI_ADDR_WIDTH-1:0]         m_axi_awaddr,
    output wire [7:0]                        m_axi_awlen,
    output wire [2:0]                        m_axi_awsize,
    output wire [1:0]                        m_axi_awburst,
    output wire                              m_axi_awlock,
    output wire [3:0]                        m_axi_awcache,
    output wire [2:0]                        m_axi_awprot,
    output wire                              m_axi_awvalid,
    input  wire                              m_axi_awready,
    output wire [AXI_DATA_WIDTH-1:0]         m_axi_wdata,
    output wire [AXI_STRB_WIDTH-1:0]         m_axi_wstrb,
    output wire                              m_axi_wlast,
    output wire                              m_axi_wvalid,
    input  wire                              m_axi_wready,
    input  wire [AXI_ID_WIDTH-1:0]           m_axi_bid,
    input  wire [1:0]                        m_axi_bresp,
    input  wire                              m_axi_bvalid,
    output wire                              m_axi_bready
);

    localparam integer FIFO_OCCUPANCY_WIDTH =
        (FIFO_DEPTH_ENTRIES <= 1) ? 1 : $clog2(FIFO_DEPTH_ENTRIES + 1);

    initial begin
        if (C_S00_AXI_DATA_WIDTH != 32 || C_S00_AXI_ADDR_WIDTH < 12) begin
            $error("axis_ddr_circular_writer: AXI-Lite must be 32-bit with >=12 address bits");
        end
        if (AXIS_DATA_WIDTH != 16 || AXIS_KEEP_WIDTH != 2) begin
            $error("axis_ddr_circular_writer: public AXIS must be 16-bit with 2 keep bits");
        end
        if (AXI_DATA_WIDTH != 64 || AXI_STRB_WIDTH != 8) begin
            $error("axis_ddr_circular_writer: DDR AXI must be 64-bit with 8 strobes");
        end
        if (AXI_MAX_BURST_BEATS != 16) begin
            $error("axis_ddr_circular_writer: v1 scheduler requires 16-beat maximum bursts");
        end
        if (AXI_ADDR_WIDTH > 63 || AXI_ADDR_WIDTH < 32) begin
            $error("axis_ddr_circular_writer: unsupported AXI address width");
        end
    end

    wire clk = s00_axi_aclk;
    wire rst = !s00_axi_aresetn;

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

    // ---------------------------------------------------------------------
    // Protocol-clean AXI-Lite transport.
    // ---------------------------------------------------------------------
    wire                                      register_write_enable;
    wire [C_S00_AXI_ADDR_WIDTH-1:0]           register_write_address;
    wire [31:0]                               register_write_data;
    wire [3:0]                                register_write_strobes;
    wire [C_S00_AXI_ADDR_WIDTH-1:0]           register_read_address;
    reg  [31:0]                               register_read_data;

    axil_register_slave #(
        .DATA_WIDTH(32),
        .ADDR_WIDTH(C_S00_AXI_ADDR_WIDTH)
    ) u_axil_slave (
        .clk(clk),
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
        .reg_wr_en(register_write_enable),
        .reg_wr_addr(register_write_address),
        .reg_wr_data(register_write_data),
        .reg_wr_strb(register_write_strobes),
        .reg_rd_addr(register_read_address),
        .reg_rd_data(register_read_data)
    );

    // ---------------------------------------------------------------------
    // Shadow configuration.  Writes during a session intentionally affect
    // only the following START; active geometry is immutable.
    // ---------------------------------------------------------------------
    wire [63:0] shadow_ring_base_address;
    wire [31:0] shadow_ring_size_bytes;
    wire [31:0] shadow_block_size_bytes;
    wire [31:0] shadow_ring_capacity_blocks;
    wire [31:0] shadow_irq_completion_interval_blocks;
    wire shadow_address_valid, shadow_config_valid;

    ddrw_session_config #(
        .REG_ADDR_WIDTH(C_S00_AXI_ADDR_WIDTH), .AXI_ADDR_WIDTH(AXI_ADDR_WIDTH)
    ) u_session_config (
        .clk(clk), .rst(rst),
        .reg_wr_en(register_write_enable), .reg_wr_addr(register_write_address),
        .reg_wr_data(register_write_data), .reg_wr_strb(register_write_strobes),
        .ring_base_address(shadow_ring_base_address),
        .ring_size_bytes(shadow_ring_size_bytes), .block_size_bytes(shadow_block_size_bytes),
        .ring_capacity_blocks(shadow_ring_capacity_blocks),
        .irq_completion_interval_blocks(shadow_irq_completion_interval_blocks),
        .address_valid(shadow_address_valid), .config_valid(shadow_config_valid)
    );

    wire command_write = register_write_enable &&
                         (register_write_address == `DDRW_REG_COMMAND);
    wire [31:0] command_pulse_value = apply_wstrb(
        32'd0, register_write_data, register_write_strobes);

    // ---------------------------------------------------------------------
    // Session/ring state.
    // ---------------------------------------------------------------------
    reg [3:0]  session_state;
    reg [31:0] session_id;

    reg [AXI_ADDR_WIDTH-1:0] active_ring_base_address;
    reg [31:0] active_block_size_bytes;
    reg [31:0] active_ring_capacity_blocks;
    reg [31:0] active_irq_completion_interval_blocks;
    reg [AXI_ADDR_WIDTH-1:0] current_slot_address;
    reg [31:0] current_slot_index;
    reg [31:0] bytes_in_current_block;

    reg [31:0] produced_block_count;
    reg [31:0] consumed_block_count;
    wire [31:0] outstanding_block_count =
        produced_block_count - consumed_block_count;

    reg [AXI_ADDR_WIDTH-1:0] last_produced_block_address;
    reg [31:0] last_produced_block_size_bytes;
    reg [31:0] final_block_size_bytes;
    reg [63:0] ddr_committed_byte_count;

    reg [31:0] input_protocol_error_count;
    reg [31:0] input_backpressure_cycle_count;
    reg [31:0] error_flags;
    reg [1:0]  last_bresp;
    reg         eos_seen;
    reg         eos_committed;
    reg         irq_pending;
    reg [31:0]  irq_interval_block_count;

    reg         drain_eos_seen;
    reg         scheduler_quiesced;
    reg         invalid_input_had_tlast;

    reg [31:0] snapshot_sequence;
    reg [31:0] snapshot_produced_block_count;
    reg [31:0] snapshot_consumed_block_count;
    reg [31:0] snapshot_outstanding_block_count;
    reg [31:0] snapshot_final_block_size_bytes;

    wire session_is_running =
        (session_state == `DDRW_STATE_CAPTURING) ||
        (session_state == `DDRW_STATE_FULL_WAIT) ||
        (session_state == `DDRW_STATE_DRAINING);
    wire session_is_abort_draining =
        (session_state == `DDRW_STATE_ABORT_DRAIN);
    wire session_is_fault_draining =
        (session_state == `DDRW_STATE_FAULT_DRAIN);
    wire session_accepts_normal_input =
        (session_state == `DDRW_STATE_CAPTURING) ||
        (session_state == `DDRW_STATE_FULL_WAIT);
    wire session_accepts_discard_input =
        session_is_abort_draining || session_is_fault_draining;

    wire session_can_start =
        (session_state == `DDRW_STATE_IDLE) ||
        (session_state == `DDRW_STATE_DONE) ||
        (session_state == `DDRW_STATE_ABORT_DONE);

    wire soft_reset_request = command_write &&
        ((command_pulse_value & `DDRW_COMMAND_SOFT_RESET) != 0);
    wire abort_request = command_write &&
        ((command_pulse_value & `DDRW_COMMAND_ABORT) != 0);
    wire start_request = command_write &&
        ((command_pulse_value & `DDRW_COMMAND_START) != 0) &&
        !soft_reset_request && !abort_request;

    // Leaf busy/level signals are declared before start acceptance below.
    wire [FIFO_OCCUPANCY_WIDTH-1:0] fifo_occupancy_entries;
    wire [FIFO_OCCUPANCY_WIDTH-1:0] fifo_high_water_entries;
    wire fifo_partial_pending;
    wire scheduler_busy;

    wire internal_path_empty = (fifo_occupancy_entries == 0) &&
                               !fifo_partial_pending && !scheduler_busy;
    wire start_accept = start_request && session_can_start && internal_path_empty &&
                        shadow_config_valid;
    wire start_reject = start_request && !start_accept;
    wire soft_reset_while_idle = soft_reset_request &&
                                 !session_is_running &&
                                 !session_is_abort_draining &&
                                 !session_is_fault_draining &&
                                 !scheduler_busy;

    wire abort_control_request = session_is_running &&
                                 (abort_request || soft_reset_request);
    wire snapshot_request = command_write &&
        ((command_pulse_value & `DDRW_COMMAND_SNAPSHOT) != 0);
    wire irq_ack_request = command_write &&
        ((command_pulse_value & `DDRW_COMMAND_IRQ_ACK) != 0);

    // ---------------------------------------------------------------------
    // Input packer and private elasticity FIFO.
    // ---------------------------------------------------------------------
    wire [63:0] packed_entry_data;
    wire [3:0]  packed_entry_byte_count;
    wire        packed_entry_eos;
    wire        packed_entry_valid;
    wire        packed_entry_ready;
    wire        packer_protocol_error;
    wire        packer_discard_eos;

    wire legal_payload_transfer =
        (s_axis_tkeep == 2'b11) && !s_axis_tlast;
    wire legal_eos_transfer =
        (s_axis_tkeep == 2'b00) && s_axis_tlast;
    wire input_handshake = s_axis_tvalid && s_axis_tready;
    wire invalid_input_handshake = input_handshake &&
        session_accepts_normal_input &&
        !(legal_payload_transfer || legal_eos_transfer);
    wire input_eos_handshake = input_handshake && legal_eos_transfer;

    // Scheduler faults are declared below; both events clear queued data on
    // the edge that transitions the session into controlled fault drain.
    wire scheduler_fault_valid;
    // The packer reports malformed accepted beats one cycle later.  Using the
    // registered pulse avoids a combinational ready->fault->clear->ready loop.
    // Discard-mode protocol diagnostics must not restart an existing drain.
    wire input_protocol_fault = packer_protocol_error &&
                                session_accepts_normal_input;
    wire fault_entry_request = input_protocol_fault || scheduler_fault_valid;
    wire packer_clear = start_accept || soft_reset_while_idle ||
                        abort_control_request || fault_entry_request;

    ddrw_axis_packer_fifo #(
        .DEPTH_ENTRIES(FIFO_DEPTH_ENTRIES),
        .OCCUPANCY_WIDTH(FIFO_OCCUPANCY_WIDTH)
    ) u_input_packer_fifo (
        .clk(clk),
        .rst(rst),
        .clear(packer_clear),
        .accept_enable(session_accepts_normal_input ||
                       session_accepts_discard_input),
        .discard_mode(session_accepts_discard_input),
        .s_axis_tdata(s_axis_tdata),
        .s_axis_tkeep(s_axis_tkeep),
        .s_axis_tvalid(s_axis_tvalid),
        .s_axis_tready(s_axis_tready),
        .s_axis_tlast(s_axis_tlast),
        .m_entry_data(packed_entry_data),
        .m_entry_byte_count(packed_entry_byte_count),
        .m_entry_eos(packed_entry_eos),
        .m_entry_valid(packed_entry_valid),
        .m_entry_ready(packed_entry_ready),
        .occupancy_entries(fifo_occupancy_entries),
        .high_water_entries(fifo_high_water_entries),
        .partial_pending(fifo_partial_pending),
        .protocol_error_pulse(packer_protocol_error),
        .discard_eos_pulse(packer_discard_eos)
    );

    // ---------------------------------------------------------------------
    // Burst scheduling and AXI write transport.
    // ---------------------------------------------------------------------
    wire [AXI_ADDR_WIDTH-1:0] current_write_address =
        current_slot_address + bytes_in_current_block;
    wire [31:0] block_bytes_remaining =
        active_block_size_bytes - bytes_in_current_block;
    wire ring_full = (active_ring_capacity_blocks != 0) &&
                     (outstanding_block_count >= active_ring_capacity_blocks);
    wire slot_available = (bytes_in_current_block != 0) || !ring_full;
    wire head_is_zero_byte_eos = packed_entry_valid && packed_entry_eos &&
                                 (packed_entry_byte_count == 0);
    wire        scheduler_commit_valid;
    wire [31:0] scheduler_commit_bytes;
    wire        scheduler_commit_block_end;
    wire        scheduler_commit_eos;
    wire [1:0]  scheduler_fault_bresp;
    wire        scheduler_cancel_done;

    // A successful BRESP produces commit_valid after the scheduler has already
    // returned to COLLECT.  Hold entry acceptance for that one cycle so the
    // controller can first advance current_write_address/block remaining.
    wire scheduler_enable = session_is_running && !scheduler_commit_valid &&
                            (slot_available || head_is_zero_byte_eos);
    wire scheduler_cancel = abort_control_request || fault_entry_request ||
                            session_is_abort_draining ||
                            session_is_fault_draining;
    wire scheduler_clear = start_accept || soft_reset_while_idle;

    ddrw_burst_scheduler #(
        .AXI_ADDR_WIDTH(AXI_ADDR_WIDTH),
        .AXI_ID_WIDTH(AXI_ID_WIDTH)
    ) u_burst_scheduler (
        .clk(clk),
        .rst(rst),
        .clear(scheduler_clear),
        .cancel(scheduler_cancel),
        .enable(scheduler_enable),
        .current_write_addr(current_write_address),
        .block_bytes_remaining(block_bytes_remaining),
        .entry_data(packed_entry_data),
        .entry_byte_count(packed_entry_byte_count),
        .entry_eos(packed_entry_eos),
        .entry_valid(packed_entry_valid),
        .entry_ready(packed_entry_ready),
        .commit_valid(scheduler_commit_valid),
        .commit_bytes(scheduler_commit_bytes),
        .commit_block_end(scheduler_commit_block_end),
        .commit_eos(scheduler_commit_eos),
        .fault_valid(scheduler_fault_valid),
        .fault_bresp(scheduler_fault_bresp),
        .cancel_done(scheduler_cancel_done),
        .busy(scheduler_busy),
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

    wire data_path_busy = fifo_partial_pending || (fifo_occupancy_entries != 0) ||
                           scheduler_busy || scheduler_commit_valid ||
                           (session_state == `DDRW_STATE_DRAINING) ||
                           session_is_abort_draining ||
                           session_is_fault_draining;

    wire [31:0] commit_accumulated_block_bytes =
        bytes_in_current_block + scheduler_commit_bytes;
    wire normal_commit_context = session_is_running;
    wire commit_produces_block = normal_commit_context &&
        scheduler_commit_valid &&
        (scheduler_commit_block_end ||
         (scheduler_commit_eos &&
          (commit_accumulated_block_bytes != 0)));

    wire [63:0] current_write_address_64 =
        {{(64-AXI_ADDR_WIDTH){1'b0}}, current_write_address};
    wire [63:0] last_produced_block_address_64 =
        {{(64-AXI_ADDR_WIDTH){1'b0}}, last_produced_block_address};
    wire [31:0] current_write_offset_bytes =
        current_write_address - active_ring_base_address;

    wire consumed_block_count_update_write = register_write_enable &&
        (register_write_address == `DDRW_REG_CONSUMED_BLOCK_COUNT);
    wire [31:0] consumed_block_count_update_value = apply_wstrb(
        consumed_block_count, register_write_data, register_write_strobes);
    wire [31:0] consumed_block_count_advance =
        consumed_block_count_update_value - consumed_block_count;
    wire consumed_block_count_update_valid = consumed_block_count_update_write &&
        (|register_write_strobes) &&
        (consumed_block_count_advance <= outstanding_block_count);

    wire error_status_write = register_write_enable &&
        (register_write_address == `DDRW_REG_ERROR_STATUS);
    wire [31:0] error_clear_mask = apply_wstrb(
        32'd0, register_write_data, register_write_strobes) &
        `DDRW_ERROR_IMPLEMENTED_MASK;
    wire [31:0] event_error_base = error_status_write ?
        (error_flags & ~error_clear_mask) : error_flags;

    // Fold every event sampled on this edge into one set mask.  Keeping the
    // W1C base separate makes newly raised events win over a simultaneous
    // software clear, and avoids nonblocking-assignment priority silently
    // dropping a START rejection when a datapath fault arrives on the same
    // edge.  Abort is included even when the higher-priority state transition
    // upgrades the session directly to FAULT_DRAIN.
    wire [31:0] start_reject_error_events = start_reject ?
        (`DDRW_ERROR_START_REJECTED |
         (!shadow_config_valid ? `DDRW_ERROR_CONFIG_INVALID : 32'd0) |
         (!shadow_address_valid ? `DDRW_ERROR_ADDRESS_INVALID : 32'd0)) :
        32'd0;
    wire [31:0] scheduler_fault_error_events = scheduler_fault_valid ?
        `DDRW_ERROR_AXI_WRITE_RESPONSE : 32'd0;
    wire [31:0] input_protocol_error_events = input_protocol_fault ?
        (`DDRW_ERROR_INPUT_PROTOCOL | `DDRW_ERROR_DATA_LOSS) : 32'd0;
    wire [31:0] abort_error_events = abort_control_request ?
        (`DDRW_ERROR_SESSION_ABORTED |
         (soft_reset_request ? `DDRW_ERROR_RESET_DURING_SESSION : 32'd0)) :
        32'd0;
    wire [31:0] sampled_error_events =
        start_reject_error_events | scheduler_fault_error_events |
        input_protocol_error_events | abort_error_events;
    wire sampled_error_event_valid = start_reject || scheduler_fault_valid ||
        input_protocol_fault || abort_control_request;

    // ---------------------------------------------------------------------
    // Session controller and accounting.
    // ---------------------------------------------------------------------
    always @(posedge clk) begin
        if (rst) begin
            session_state              <= `DDRW_STATE_IDLE;
            session_id                 <= 32'd0;
            active_ring_base_address        <= {AXI_ADDR_WIDTH{1'b0}};
            active_block_size_bytes         <= 32'd0;
            active_ring_capacity_blocks     <= 32'd0;
            active_irq_completion_interval_blocks       <= 32'd1;
            current_slot_address       <= {AXI_ADDR_WIDTH{1'b0}};
            current_slot_index         <= 32'd0;
            bytes_in_current_block     <= 32'd0;
            produced_block_count      <= 32'd0;
            consumed_block_count       <= 32'd0;
            last_produced_block_address     <= {AXI_ADDR_WIDTH{1'b0}};
            last_produced_block_size_bytes      <= 32'd0;
            final_block_size_bytes       <= 32'd0;
            ddr_committed_byte_count            <= 64'd0;
            input_protocol_error_count            <= 32'd0;
            input_backpressure_cycle_count               <= 32'd0;
            error_flags                <= 32'd0;
            last_bresp                 <= 2'b00;
            eos_seen                   <= 1'b0;
            eos_committed              <= 1'b0;
            irq_pending                <= 1'b0;
            irq_interval_block_count         <= 32'd0;
            drain_eos_seen             <= 1'b0;
            scheduler_quiesced         <= 1'b0;
            invalid_input_had_tlast       <= 1'b0;
            snapshot_sequence          <= 32'd0;
            snapshot_produced_block_count          <= 32'd0;
            snapshot_consumed_block_count          <= 32'd0;
            snapshot_outstanding_block_count       <= 32'd0;
            snapshot_final_block_size_bytes      <= 32'd0;
        end else if (soft_reset_while_idle) begin
            // Preserve the monotonically increasing session ID across a
            // software reset so stale UDP/network state cannot alias a new run.
            session_state              <= `DDRW_STATE_IDLE;
            active_ring_base_address        <= {AXI_ADDR_WIDTH{1'b0}};
            active_block_size_bytes         <= 32'd0;
            active_ring_capacity_blocks     <= 32'd0;
            active_irq_completion_interval_blocks       <= 32'd1;
            current_slot_address       <= {AXI_ADDR_WIDTH{1'b0}};
            current_slot_index         <= 32'd0;
            bytes_in_current_block     <= 32'd0;
            produced_block_count      <= 32'd0;
            consumed_block_count       <= 32'd0;
            last_produced_block_address     <= {AXI_ADDR_WIDTH{1'b0}};
            last_produced_block_size_bytes      <= 32'd0;
            final_block_size_bytes       <= 32'd0;
            ddr_committed_byte_count            <= 64'd0;
            input_protocol_error_count            <= 32'd0;
            input_backpressure_cycle_count               <= 32'd0;
            error_flags                <= 32'd0;
            last_bresp                 <= 2'b00;
            eos_seen                   <= 1'b0;
            eos_committed              <= 1'b0;
            irq_pending                <= 1'b0;
            irq_interval_block_count         <= 32'd0;
            drain_eos_seen             <= 1'b0;
            scheduler_quiesced         <= 1'b0;
            invalid_input_had_tlast       <= 1'b0;
            snapshot_sequence          <= 32'd0;
            snapshot_produced_block_count          <= 32'd0;
            snapshot_consumed_block_count          <= 32'd0;
            snapshot_outstanding_block_count       <= 32'd0;
            snapshot_final_block_size_bytes      <= 32'd0;
        end else if (start_accept) begin
            session_state              <= `DDRW_STATE_CAPTURING;
            session_id                 <= session_id + 32'd1;
            active_ring_base_address        <= shadow_ring_base_address[AXI_ADDR_WIDTH-1:0];
            active_block_size_bytes         <= shadow_block_size_bytes;
            active_ring_capacity_blocks     <= shadow_ring_capacity_blocks;
            active_irq_completion_interval_blocks       <= shadow_irq_completion_interval_blocks;
            current_slot_address       <= shadow_ring_base_address[AXI_ADDR_WIDTH-1:0];
            current_slot_index         <= 32'd0;
            bytes_in_current_block     <= 32'd0;
            produced_block_count      <= 32'd0;
            consumed_block_count       <= 32'd0;
            last_produced_block_address     <= {AXI_ADDR_WIDTH{1'b0}};
            last_produced_block_size_bytes      <= 32'd0;
            final_block_size_bytes       <= 32'd0;
            ddr_committed_byte_count            <= 64'd0;
            input_protocol_error_count            <= 32'd0;
            input_backpressure_cycle_count               <= 32'd0;
            error_flags                <= 32'd0;
            last_bresp                 <= 2'b00;
            eos_seen                   <= 1'b0;
            eos_committed              <= 1'b0;
            irq_pending                <= 1'b0;
            irq_interval_block_count         <= 32'd0;
            drain_eos_seen             <= 1'b0;
            scheduler_quiesced         <= 1'b0;
            invalid_input_had_tlast       <= 1'b0;
            // A new session must never expose a coherent snapshot from the
            // preceding SESSION_ID. Software may START directly from DONE.
            snapshot_sequence          <= 32'd0;
            snapshot_produced_block_count          <= 32'd0;
            snapshot_consumed_block_count          <= 32'd0;
            snapshot_outstanding_block_count       <= 32'd0;
            snapshot_final_block_size_bytes      <= 32'd0;
        end else begin
            // Error bits use W1C semantics, with all same-cycle hardware events
            // ORed into one update so no source depends on statement ordering.
            if (error_status_write || sampled_error_event_valid)
                error_flags <= event_error_base | sampled_error_events;
            if (scheduler_fault_valid)
                last_bresp <= scheduler_fault_bresp;
            else if (error_status_write &&
                     ((error_clear_mask & `DDRW_ERROR_AXI_WRITE_RESPONSE) != 0))
                last_bresp <= 2'b00;
            if (input_protocol_fault)
                input_protocol_error_count <= input_protocol_error_count + 32'd1;
            if (irq_ack_request) begin
                irq_pending <= 1'b0;
            end

            if (start_reject) begin
                // A rejected START is a terminal control-plane event.  Raise
                // the same sticky interrupt used for produced blocks/EOS so
                // software does not have to poll to discover a bad session.
                irq_pending <= 1'b1;
            end

            if (session_is_running && s_axis_tvalid && !s_axis_tready) begin
                input_backpressure_cycle_count <= input_backpressure_cycle_count + 32'd1;
            end

            if (invalid_input_handshake) begin
                invalid_input_had_tlast <= s_axis_tlast;
            end

            if (consumed_block_count_update_valid)
                consumed_block_count <= consumed_block_count_update_value;

            // SOFT_RESET invalidates the diagnostic snapshot immediately in
            // every state. An active reset still follows the ordered abort-drain
            // state transition below, but stale snapshot data is never retained.
            if (soft_reset_request) begin
                snapshot_sequence     <= 32'd0;
                snapshot_produced_block_count     <= 32'd0;
                snapshot_consumed_block_count     <= 32'd0;
                snapshot_outstanding_block_count  <= 32'd0;
                snapshot_final_block_size_bytes <= 32'd0;
            end else if (snapshot_request) begin
                snapshot_sequence     <= snapshot_sequence + 32'd1;
                snapshot_produced_block_count     <= produced_block_count;
                snapshot_consumed_block_count     <= consumed_block_count;
                snapshot_outstanding_block_count  <= outstanding_block_count;
                snapshot_final_block_size_bytes <= final_block_size_bytes;
            end

            if (scheduler_commit_valid) begin
                // This is physical DDR accounting.  During abort/fault drain
                // the successful in-flight transaction is not published as a
                // software-visible ring block, but these bytes did commit.
                ddr_committed_byte_count <= ddr_committed_byte_count + scheduler_commit_bytes;
            end

            if (scheduler_cancel_done) begin
                scheduler_quiesced <= 1'b1;
            end
            if (packer_discard_eos) begin
                drain_eos_seen <= 1'b1;
            end

            // Highest-priority transitions are faults and explicit aborts.
            if (fault_entry_request) begin
                session_state      <= `DDRW_STATE_FAULT_DRAIN;
                scheduler_quiesced <= scheduler_cancel_done || !scheduler_busy;
                drain_eos_seen     <= eos_seen ||
                    (input_protocol_fault && invalid_input_had_tlast);
                // Fault notification is sticky until IRQ_ACK; error bits are W1C.
                // This is the only DDR-writer interrupt wired to the PS, so a
                // failed AXI response or malformed input must not be silent.
                irq_pending        <= 1'b1;
                invalid_input_had_tlast <= 1'b0;
            end else if (abort_control_request) begin
                session_state      <= `DDRW_STATE_ABORT_DRAIN;
                scheduler_quiesced <= scheduler_cancel_done || !scheduler_busy;
                drain_eos_seen     <= eos_seen;
            end else begin
                if ((session_state == `DDRW_STATE_CAPTURING) &&
                    !slot_available) begin
                    session_state <= `DDRW_STATE_FULL_WAIT;
                end else if ((session_state == `DDRW_STATE_FULL_WAIT) &&
                             slot_available) begin
                    session_state <= eos_seen ? `DDRW_STATE_DRAINING :
                                                `DDRW_STATE_CAPTURING;
                end

                // In abort/fault discard mode the packer reports EOS through
                // packer_discard_eos; it must not re-enter normal DRAINING.
                if (input_eos_handshake && session_accepts_normal_input) begin
                    eos_seen     <= 1'b1;
                    session_state<= `DDRW_STATE_DRAINING;
                end

                if (scheduler_commit_valid && normal_commit_context) begin
                    if (commit_produces_block) begin
                        produced_block_count  <= produced_block_count + 32'd1;
                        last_produced_block_address <= current_slot_address;
                        last_produced_block_size_bytes  <= commit_accumulated_block_bytes;
                        bytes_in_current_block <= 32'd0;

                        if ((current_slot_index + 32'd1) >=
                            active_ring_capacity_blocks) begin
                            current_slot_index   <= 32'd0;
                            current_slot_address <= active_ring_base_address;
                        end else begin
                            current_slot_index <= current_slot_index + 32'd1;
                            current_slot_address <= current_slot_address +
                                                    active_block_size_bytes;
                        end

                        if (active_irq_completion_interval_blocks != 0) begin
                            if ((irq_interval_block_count + 32'd1) >=
                                active_irq_completion_interval_blocks) begin
                                irq_pending        <= 1'b1;
                                irq_interval_block_count <= 32'd0;
                            end else begin
                                irq_interval_block_count <= irq_interval_block_count + 32'd1;
                            end
                        end
                    end else begin
                        bytes_in_current_block <= commit_accumulated_block_bytes;
                    end

                    if (scheduler_commit_eos) begin
                        eos_committed <= 1'b1;
                        session_state <= `DDRW_STATE_DONE;
                        irq_pending   <= 1'b1;
                        irq_interval_block_count <= 32'd0;
                        if (commit_accumulated_block_bytes != 0) begin
                            final_block_size_bytes <=
                                commit_accumulated_block_bytes;
                        end else begin
                            // A marker-only EOS after an exactly full final
                            // block uses the last published length.  Empty
                            // sessions naturally retain the reset value zero.
                            final_block_size_bytes <= last_produced_block_size_bytes;
                        end
                    end
                end
            end

            if (!fault_entry_request && !abort_control_request) begin
                if (session_is_abort_draining && scheduler_quiesced &&
                    drain_eos_seen) begin
                    session_state <= `DDRW_STATE_ABORT_DONE;
                end else if (session_is_fault_draining && scheduler_quiesced &&
                             drain_eos_seen) begin
                    session_state <= `DDRW_STATE_FAULT;
                end
            end
        end
    end

    // ---------------------------------------------------------------------
    // Read-only status composition and register read mux.
    // ---------------------------------------------------------------------
    wire status_fault = (session_state == `DDRW_STATE_FAULT) ||
                        (session_state == `DDRW_STATE_FAULT_DRAIN);
    wire [31:0] status_word =
        (session_is_running ? `DDRW_STATUS_RUNNING : 32'd0) |
        ((outstanding_block_count != 0) ?
            `DDRW_STATUS_UNCONSUMED_BLOCK_AVAILABLE : 32'd0) |
        (data_path_busy ? `DDRW_STATUS_DATA_PATH_BUSY : 32'd0) |
        (ring_full ? `DDRW_STATUS_RING_FULL : 32'd0) |
        (irq_pending ? `DDRW_STATUS_IRQ_PENDING : 32'd0) |
        (eos_seen ? `DDRW_STATUS_EOS_SEEN : 32'd0) |
        (eos_committed ? `DDRW_STATUS_EOS_COMMITTED : 32'd0) |
        ((session_state == `DDRW_STATE_ABORT_DONE) ?
            `DDRW_STATUS_ABORT_DONE : 32'd0) |
        ((fifo_occupancy_entries != 0) ? `DDRW_STATUS_FIFO_NONEMPTY : 32'd0) |
        (status_fault ? `DDRW_STATUS_FAULT : 32'd0) |
        ((error_flags & `DDRW_ERROR_START_REJECTED) != 0 ?
            `DDRW_STATUS_START_REJECTED : 32'd0) |
        ((error_flags & `DDRW_ERROR_CONFIG_INVALID) != 0 ?
            `DDRW_STATUS_CONFIG_ERROR : 32'd0) |
        ((error_flags & `DDRW_ERROR_AXI_WRITE_RESPONSE) != 0 ?
            `DDRW_STATUS_AXI_ERROR : 32'd0) |
        ((error_flags & `DDRW_ERROR_DATA_LOSS) != 0 ?
            `DDRW_STATUS_DATA_LOSS : 32'd0);

    always @* begin
        case (register_read_address)
            `DDRW_REG_BLOCK_ID:
                register_read_data = `DDRW_BLOCK_ID;
            `DDRW_REG_ABI_VERSION:
                register_read_data = `DDRW_ABI_VERSION;
            `DDRW_REG_CAPABILITIES:
                register_read_data = `DDRW_CAPABILITIES_VALUE;
            `DDRW_REG_INFO:
                register_read_data = `DDRW_INFO_VALUE;
            `DDRW_REG_SESSION_STATE:
                register_read_data = {28'd0, session_state};
            `DDRW_REG_SESSION_ID:
                register_read_data = session_id;
            `DDRW_REG_STATUS:
                register_read_data = status_word;
            `DDRW_REG_ERROR_STATUS:
                register_read_data = error_flags;
            `DDRW_REG_LAST_AXI_BRESP:
                register_read_data = {30'd0, last_bresp};
            `DDRW_REG_RING_BASE_ADDR_LO:
                register_read_data = shadow_ring_base_address[31:0];
            `DDRW_REG_RING_BASE_ADDR_HI:
                register_read_data = shadow_ring_base_address[63:32];
            `DDRW_REG_RING_SIZE_BYTES:
                register_read_data = shadow_ring_size_bytes;
            `DDRW_REG_BLOCK_SIZE_BYTES:
                register_read_data = shadow_block_size_bytes;
            `DDRW_REG_RING_CAPACITY_BLOCKS:
                register_read_data = shadow_ring_capacity_blocks;
            `DDRW_REG_IRQ_COMPLETION_INTERVAL_BLOCKS:
                register_read_data = shadow_irq_completion_interval_blocks;
            `DDRW_REG_CURRENT_WRITE_ADDR_LO:
                register_read_data = current_write_address_64[31:0];
            `DDRW_REG_CURRENT_WRITE_ADDR_HI:
                register_read_data = current_write_address_64[63:32];
            `DDRW_REG_CURRENT_WRITE_OFFSET_BYTES:
                register_read_data = current_write_offset_bytes;
            `DDRW_REG_FIFO_LEVEL_ENTRIES:
                register_read_data = {{(32-FIFO_OCCUPANCY_WIDTH){1'b0}}, fifo_occupancy_entries};
            `DDRW_REG_LAST_PRODUCED_BLOCK_ADDR_LO:
                register_read_data = last_produced_block_address_64[31:0];
            `DDRW_REG_LAST_PRODUCED_BLOCK_ADDR_HI:
                register_read_data = last_produced_block_address_64[63:32];
            `DDRW_REG_LAST_PRODUCED_BLOCK_SIZE_BYTES:
                register_read_data = last_produced_block_size_bytes;
            `DDRW_REG_FINAL_BLOCK_SIZE_BYTES:
                register_read_data = final_block_size_bytes;
            `DDRW_REG_PRODUCED_BLOCK_COUNT:
                register_read_data = produced_block_count;
            `DDRW_REG_CONSUMED_BLOCK_COUNT:
                register_read_data = consumed_block_count;
            `DDRW_REG_OUTSTANDING_BLOCK_COUNT:
                register_read_data = outstanding_block_count;
            `DDRW_REG_INPUT_PROTOCOL_ERROR_COUNT:
                register_read_data = input_protocol_error_count;
            `DDRW_REG_FIFO_HIGH_WATER_ENTRIES:
                register_read_data = {{(32-FIFO_OCCUPANCY_WIDTH){1'b0}}, fifo_high_water_entries};
            `DDRW_REG_INPUT_BACKPRESSURE_CYCLE_COUNT:
                register_read_data = input_backpressure_cycle_count;
            `DDRW_REG_SNAPSHOT_SEQUENCE:
                register_read_data = snapshot_sequence;
            `DDRW_REG_SNAPSHOT_PRODUCED_BLOCK_COUNT:
                register_read_data = snapshot_produced_block_count;
            `DDRW_REG_SNAPSHOT_CONSUMED_BLOCK_COUNT:
                register_read_data = snapshot_consumed_block_count;
            `DDRW_REG_SNAPSHOT_OUTSTANDING_BLOCK_COUNT:
                register_read_data = snapshot_outstanding_block_count;
            `DDRW_REG_SNAPSHOT_FINAL_BLOCK_SIZE_BYTES:
                register_read_data = snapshot_final_block_size_bytes;
            `DDRW_REG_DDR_COMMITTED_BYTE_COUNT_LO:
                register_read_data = ddr_committed_byte_count[31:0];
            `DDRW_REG_DDR_COMMITTED_BYTE_COUNT_HI:
                register_read_data = ddr_committed_byte_count[63:32];
            default:
                register_read_data = 32'd0;
        endcase
    end

    assign transfer_irq = irq_pending;
    assign transfer_fault = status_fault ||
        ((error_flags & (`DDRW_ERROR_DATA_LOSS | `DDRW_ERROR_CONFIG_INVALID |
                         `DDRW_ERROR_AXI_WRITE_RESPONSE | `DDRW_ERROR_INPUT_PROTOCOL |
                         `DDRW_ERROR_ADDRESS_INVALID |
                         `DDRW_ERROR_START_REJECTED)) != 0);

`ifndef SYNTHESIS
    always @(posedge clk) begin
        if (!rst) begin
            if (commit_produces_block &&
                (commit_accumulated_block_bytes > active_block_size_bytes)) begin
                $error("axis_ddr_circular_writer: committed block exceeds configured slot");
            end
            if (scheduler_commit_valid && scheduler_commit_eos &&
                !eos_seen && normal_commit_context) begin
                $error("axis_ddr_circular_writer: EOS committed before input EOS was observed");
            end
            if ((active_ring_capacity_blocks != 0) &&
                (outstanding_block_count > active_ring_capacity_blocks)) begin
                $error("axis_ddr_circular_writer: outstanding ring count exceeds capacity");
            end
        end
    end
`endif

endmodule

`default_nettype wire
