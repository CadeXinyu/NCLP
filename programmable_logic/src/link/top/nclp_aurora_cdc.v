`timescale 1ns/1ps
`default_nettype none
// Physical transport boundary only. Packet routing and trigger policy belong
// to the compute fabric. One overflow invalidates the link; no partial-frame recovery.
module nclp_aurora_cdc (
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 sys_clk CLK", X_INTERFACE_PARAMETER = "ASSOCIATED_BUSIF S_AXIS_TX:M_AXIS_RX, ASSOCIATED_RESET sys_resetn, FREQ_HZ 100000000" *)
    input wire sys_clk,
    input wire sys_resetn,
    input wire phy_enable,
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 aurora_clk CLK", X_INTERFACE_PARAMETER = "ASSOCIATED_BUSIF M_AURORA_TX:S_AURORA_RX, FREQ_HZ 161132813" *)
    input wire aurora_clk,
    input wire channel_up,
    input wire core_reset,
    input wire hard_err,
    input wire soft_err,
    input wire sfp_mod_abs,
    input wire sfp_tx_fault,
    input wire sfp_rx_los,
    output wire sfp_tx_disable,
    output wire core_reset_pb,
    output wire pma_init,
    output wire link_up,
    output wire link_fault,
    output wire ps_tx_packet_accepted_pulse,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_TX TDATA" *)
    input wire [63:0] s_axis_tx_tdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_TX TKEEP" *)
    input wire [7:0] s_axis_tx_tkeep,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_TX TLAST" *)
    input wire s_axis_tx_tlast,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_TX TUSER" *)
    input wire s_axis_tx_ps_owned,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_TX TVALID" *)
    input wire s_axis_tx_tvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_TX TREADY" *)
    output wire s_axis_tx_tready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_RX TDATA" *)
    output wire [63:0] m_axis_rx_tdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_RX TKEEP" *)
    output wire [7:0] m_axis_rx_tkeep,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_RX TLAST" *)
    output wire m_axis_rx_tlast,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_RX TVALID" *)
    output wire m_axis_rx_tvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_RX TREADY" *)
    input wire m_axis_rx_tready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AURORA_TX TDATA" *)
    output wire [63:0] aurora_tx_tdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AURORA_TX TKEEP" *)
    output wire [7:0] aurora_tx_tkeep,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AURORA_TX TLAST" *)
    output wire aurora_tx_tlast,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AURORA_TX TVALID" *)
    output wire aurora_tx_tvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AURORA_TX TREADY" *)
    input wire aurora_tx_tready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AURORA_RX TDATA" *)
    input wire [63:0] aurora_rx_tdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AURORA_RX TKEEP" *)
    input wire [7:0] aurora_rx_tkeep,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AURORA_RX TLAST" *)
    input wire aurora_rx_tlast,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AURORA_RX TVALID" *)
    input wire aurora_rx_tvalid
);
    // Carrier sidebands are asynchronous. Never gate transmitter enable with
    // LOS/channel_up: doing that would deadlock two initially disconnected ends.
    (* ASYNC_REG = "TRUE" *) reg [2:0] sfp_status_meta = 3'b111;
    (* ASYNC_REG = "TRUE" *) reg [2:0] sfp_status_sync = 3'b111;
    always @(posedge sys_clk or negedge sys_resetn) begin
        if (!sys_resetn) begin
            sfp_status_meta <= 3'b111;
            sfp_status_sync <= 3'b111;
        end
        else begin
            sfp_status_meta <= {sfp_mod_abs, sfp_tx_fault, sfp_rx_los};
            sfp_status_sync <= sfp_status_meta;
        end
    end
    wire core_reset_condition = !phy_enable || sfp_status_sync[2] ||
                                sfp_status_sync[1];
    reg [10:0] core_startup_counter = 0;
    // Both reset requests leave a flip-flop, never a combinational gate.
    // Aurora may stop its user clock while reset is asserted.
    reg core_reset_request = 1'b1;
    reg aurora_domain_reset_request = 1'b1;
    always @(posedge sys_clk or negedge sys_resetn) begin
        if (!sys_resetn) begin
            core_startup_counter <= 0;
            core_reset_request <= 1'b1;
            aurora_domain_reset_request <= 1'b1;
        end else begin
            aurora_domain_reset_request <= !phy_enable;
            core_reset_request <= !core_startup_counter[10] ||
                                  core_reset_condition;
            if (core_reset_condition)
                core_startup_counter <= 0;
            else if (!core_startup_counter[10])
                core_startup_counter <= core_startup_counter + 1'b1;
        end
    end
    assign core_reset_pb = core_reset_request;
    assign pma_init = core_reset_pb;
    assign sfp_tx_disable = !sys_resetn || core_reset_condition;

    // Assert even with a stopped Aurora clock; release only on Aurora edges.
    // XPM supplies the scoped asynchronous-reset timing exception.
    wire user_reset;
    xpm_cdc_async_rst #(
        .DEST_SYNC_FF(3), .INIT_SYNC_FF(1), .RST_ACTIVE_HIGH(1)
    ) user_reset_sync (
        .src_arst(aurora_domain_reset_request), .dest_clk(aurora_clk),
        .dest_arst(user_reset)
    );
    (* ASYNC_REG = "TRUE" *) reg [1:0] aurora_enable_pipe = 0;
    always @(posedge aurora_clk or posedge user_reset) begin
        if (user_reset) aurora_enable_pipe <= 0;
        else aurora_enable_pipe <= {aurora_enable_pipe[0], 1'b1};
    end
    wire rx_full, rx_empty, rx_wr_busy, rx_rd_busy;
    wire rx_reset;
    reg aurora_fault_sticky = 0;
    always @(posedge aurora_clk or posedge user_reset) begin
        if (user_reset) aurora_fault_sticky <= 0;
        else if (!aurora_enable_pipe[1]) aurora_fault_sticky <= 0;
        else if (hard_err || soft_err ||
                 (aurora_rx_tvalid && channel_up && !rx_reset &&
                  !rx_wr_busy && rx_full)) aurora_fault_sticky <= 1;
    end
    // Aurora already crosses its SYSTEM_RESET register into init_clk. Give
    // our independent status crossing its own user-clock source register so
    // one asynchronous source cannot feed two system-domain synchronizers.
    // Assert conservatively even when the user clock has stopped. Only status
    // takes this extra cycle; FIFO and user-data gating still use core_reset.
    reg core_reset_sample_aurora = 1'b1;
    always @(posedge aurora_clk or posedge user_reset) begin
        if (user_reset) core_reset_sample_aurora <= 1'b1;
        else core_reset_sample_aurora <= core_reset;
    end
    (* ASYNC_REG = "TRUE" *) reg [2:0] aurora_status_meta = 0;
    (* ASYNC_REG = "TRUE" *) reg [2:0] aurora_status_sync = 0;
    always @(posedge sys_clk or negedge sys_resetn) begin
        if (!sys_resetn) begin
            aurora_status_meta <= 0;
            aurora_status_sync <= 0;
        end
        else begin
            aurora_status_meta <= {
                aurora_fault_sticky, core_reset_sample_aurora, channel_up
            };
            aurora_status_sync <= aurora_status_meta;
        end
    end
    assign link_fault = aurora_status_sync[2];
    wire link_healthy_sys = !core_reset_pb && !sfp_status_sync[0] &&
        aurora_status_sync[0] && !aurora_status_sync[1] &&
        !aurora_status_sync[2];
    wire link_healthy_aurora = aurora_enable_pipe[1] && channel_up &&
        !core_reset && !aurora_fault_sticky;
    // Hold each FIFO reset for >=32 write clocks after link loss. Both FIFOs
    // internally synchronize the reset into their read domain.
    reg [5:0] tx_fifo_reset_release_counter = 0;
    reg [5:0] rx_fifo_reset_release_counter = 0;
    always @(posedge sys_clk or negedge sys_resetn) begin
        if (!sys_resetn) tx_fifo_reset_release_counter <= 0;
        else if (!link_healthy_sys) tx_fifo_reset_release_counter <= 0;
        else if (!tx_fifo_reset_release_counter[5])
            tx_fifo_reset_release_counter <=
                tx_fifo_reset_release_counter + 1'b1;
    end
    always @(posedge aurora_clk or posedge user_reset) begin
        if (user_reset) rx_fifo_reset_release_counter <= 0;
        else if (!link_healthy_aurora) rx_fifo_reset_release_counter <= 0;
        else if (!rx_fifo_reset_release_counter[5])
            rx_fifo_reset_release_counter <=
                rx_fifo_reset_release_counter + 1'b1;
    end
    wire tx_reset = !tx_fifo_reset_release_counter[5];
    assign rx_reset = !rx_fifo_reset_release_counter[5];
    wire tx_full, tx_empty, tx_wr_busy, tx_rd_busy;
    wire [73:0] tx_fifo_output_word;
    wire [72:0] rx_fifo_output_word;
    wire aurora_tx_beat_ps_owned;
    assign link_up = link_healthy_sys && !tx_reset && !tx_wr_busy && !rx_rd_busy;
    assign s_axis_tx_tready = link_up && !tx_full;
    assign {aurora_tx_beat_ps_owned, aurora_tx_tlast,
            aurora_tx_tkeep, aurora_tx_tdata} = tx_fifo_output_word;
    assign aurora_tx_tvalid = link_healthy_aurora && !tx_empty && !tx_rd_busy;
    assign {m_axis_rx_tlast, m_axis_rx_tkeep, m_axis_rx_tdata} = rx_fifo_output_word;
    assign m_axis_rx_tvalid = link_up && !rx_empty;

    // Cross only the event created by the final PS-owned beat being accepted
    // by the Aurora core. Ownership is opaque transport metadata carried in
    // the TX FIFO; no application header is inspected here. The event toggle
    // deliberately survives link and PHY resets so independently resetting
    // one side of the crossing cannot create a false completion pulse.
    wire ps_tx_packet_accepted_user_pulse = aurora_tx_tvalid &&
        aurora_tx_tready && aurora_tx_tlast && aurora_tx_beat_ps_owned;
    wire sys_reset_aurora;
    xpm_cdc_async_rst #(
        .DEST_SYNC_FF(3), .INIT_SYNC_FF(1), .RST_ACTIVE_HIGH(1)
    ) acceptance_system_reset_sync (
        .src_arst(!sys_resetn), .dest_clk(aurora_clk),
        .dest_arst(sys_reset_aurora)
    );
    reg ps_tx_acceptance_toggle_user = 1'b0;
    always @(posedge aurora_clk or posedge sys_reset_aurora) begin
        if (sys_reset_aurora)
            ps_tx_acceptance_toggle_user <= 1'b0;
        else if (ps_tx_packet_accepted_user_pulse)
            ps_tx_acceptance_toggle_user <= ~ps_tx_acceptance_toggle_user;
    end

    wire ps_tx_acceptance_toggle_sys;
    xpm_cdc_single #(
        .DEST_SYNC_FF(3), .INIT_SYNC_FF(1), .SRC_INPUT_REG(0),
        .SIM_ASSERT_CHK(0)
    ) ps_tx_acceptance_toggle_cdc (
        .src_clk(aurora_clk), .src_in(ps_tx_acceptance_toggle_user),
        .dest_clk(sys_clk), .dest_out(ps_tx_acceptance_toggle_sys)
    );
    reg [2:0] acceptance_sync_arm_shift = 3'd0;
    reg acceptance_toggle_seen = 1'b0;
    reg acceptance_pulse_sys = 1'b0;
    always @(posedge sys_clk or negedge sys_resetn) begin
        if (!sys_resetn) begin
            acceptance_sync_arm_shift <= 3'd0;
            acceptance_toggle_seen <= 1'b0;
            acceptance_pulse_sys <= 1'b0;
        end else begin
            acceptance_pulse_sys <= 1'b0;
            if (!acceptance_sync_arm_shift[2]) begin
                acceptance_sync_arm_shift <= {acceptance_sync_arm_shift[1:0], 1'b1};
                acceptance_toggle_seen <= ps_tx_acceptance_toggle_sys;
            end else begin
                acceptance_pulse_sys <=
                    ps_tx_acceptance_toggle_sys ^ acceptance_toggle_seen;
                acceptance_toggle_seen <= ps_tx_acceptance_toggle_sys;
            end
        end
    end
    assign ps_tx_packet_accepted_pulse = acceptance_pulse_sys;

    xpm_fifo_async #(
        .FIFO_MEMORY_TYPE("block"), .FIFO_WRITE_DEPTH(2048),
        .WRITE_DATA_WIDTH(74), .READ_DATA_WIDTH(74), .CDC_SYNC_STAGES(3),
        .READ_MODE("fwft"), .FIFO_READ_LATENCY(0), .DOUT_RESET_VALUE("0"),
        .ECC_MODE("no_ecc"), .FULL_RESET_VALUE(0), .RELATED_CLOCKS(0),
        .USE_ADV_FEATURES("0000"), .WAKEUP_TIME(0),
        .WR_DATA_COUNT_WIDTH(12), .RD_DATA_COUNT_WIDTH(12),
        .PROG_EMPTY_THRESH(10), .PROG_FULL_THRESH(2038)
    ) tx_fifo (
        .rst(tx_reset), .wr_clk(sys_clk), .rd_clk(aurora_clk),
        .din({s_axis_tx_ps_owned,s_axis_tx_tlast,
              s_axis_tx_tkeep,s_axis_tx_tdata}),
        .wr_en(s_axis_tx_tvalid && s_axis_tx_tready),
        .rd_en(aurora_tx_tvalid && aurora_tx_tready), .dout(tx_fifo_output_word),
        .full(tx_full), .empty(tx_empty), .wr_rst_busy(tx_wr_busy),
        .rd_rst_busy(tx_rd_busy), .sleep(1'b0),
        .injectdbiterr(1'b0), .injectsbiterr(1'b0),
        .almost_empty(), .almost_full(), .data_valid(), .dbiterr(),
        .overflow(), .prog_empty(), .prog_full(), .rd_data_count(),
        .sbiterr(), .underflow(), .wr_ack(), .wr_data_count()
    );
    xpm_fifo_async #(
        .FIFO_MEMORY_TYPE("block"), .FIFO_WRITE_DEPTH(2048),
        .WRITE_DATA_WIDTH(73), .READ_DATA_WIDTH(73), .CDC_SYNC_STAGES(3),
        .READ_MODE("fwft"), .FIFO_READ_LATENCY(0), .DOUT_RESET_VALUE("0"),
        .ECC_MODE("no_ecc"), .FULL_RESET_VALUE(0), .RELATED_CLOCKS(0),
        .USE_ADV_FEATURES("0000"), .WAKEUP_TIME(0),
        .WR_DATA_COUNT_WIDTH(12), .RD_DATA_COUNT_WIDTH(12),
        .PROG_EMPTY_THRESH(10), .PROG_FULL_THRESH(2038)
    ) rx_fifo (
        .rst(rx_reset), .wr_clk(aurora_clk), .rd_clk(sys_clk),
        .din({aurora_rx_tlast,aurora_rx_tkeep,aurora_rx_tdata}),
        // Aurora has no receive backpressure. The application router validates
        // the first header after reset and discards any truncated tail through
        // TLAST, so a complete first packet must not be sacrificed here.
        .wr_en(aurora_rx_tvalid && link_healthy_aurora &&
               !rx_reset && !rx_wr_busy && !rx_full),
        .rd_en(m_axis_rx_tvalid && m_axis_rx_tready), .dout(rx_fifo_output_word),
        .full(rx_full), .empty(rx_empty), .wr_rst_busy(rx_wr_busy),
        .rd_rst_busy(rx_rd_busy), .sleep(1'b0),
        .injectdbiterr(1'b0), .injectsbiterr(1'b0),
        .almost_empty(), .almost_full(), .data_valid(), .dbiterr(),
        .overflow(), .prog_empty(), .prog_full(), .rd_data_count(),
        .sbiterr(), .underflow(), .wr_ack(), .wr_data_count()
    );
endmodule

`default_nettype wire
