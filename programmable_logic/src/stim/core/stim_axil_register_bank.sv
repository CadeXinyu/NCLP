`timescale 1ns / 1ps
`default_nettype none

// AXI4-Lite register bank plus a directly addressable synchronous waveform-RAM
// aperture.  The normal NCLP register transport samples read data on the AR
// handshake and therefore cannot service a block-RAM read; this slave holds the
// read transaction until the RAM's registered data is available.
module stim_axil_register_bank #(
    parameter integer AXI_DATA_WIDTH = 32,
    parameter integer AXI_ADDR_WIDTH = 13,
    parameter integer RAM_DEPTH = 1024,
    parameter integer RAM_ADDR_WIDTH = (RAM_DEPTH > 1) ? $clog2(RAM_DEPTH) : 1
)(
    input  wire                              clk,
    input  wire                              resetn,

    input  wire [AXI_ADDR_WIDTH-1:0]         s_axi_awaddr,
    input  wire [2:0]                        s_axi_awprot,
    input  wire                              s_axi_awvalid,
    output wire                              s_axi_awready,
    input  wire [AXI_DATA_WIDTH-1:0]         s_axi_wdata,
    input  wire [(AXI_DATA_WIDTH/8)-1:0]     s_axi_wstrb,
    input  wire                              s_axi_wvalid,
    output wire                              s_axi_wready,
    output reg  [1:0]                        s_axi_bresp,
    output reg                               s_axi_bvalid,
    input  wire                              s_axi_bready,
    input  wire [AXI_ADDR_WIDTH-1:0]         s_axi_araddr,
    input  wire [2:0]                        s_axi_arprot,
    input  wire                              s_axi_arvalid,
    output wire                              s_axi_arready,
    output reg  [AXI_DATA_WIDTH-1:0]         s_axi_rdata,
    output reg  [1:0]                        s_axi_rresp,
    output reg                               s_axi_rvalid,
    input  wire                              s_axi_rready,

    output reg                               arm_pulse,
    output reg                               disarm_pulse,
    output reg                               software_trigger_pulse,
    output reg                               stop_pulse,
    output reg                               clear_diagnostics_pulse,
    output reg                               prime_dac_pulse,
    output reg                               dac_clock_program_pulse,
    output reg                               dac_prime_invalidate_pulse,

    output reg  [1:0]                        cfg_action_mode,
    output reg                               cfg_external_trigger_enable,
    output reg  [31:0]                       cfg_ttl_pulse_width_axi_cycles,
    output reg  [13:0]                       cfg_intan_stim_marker_mask,
    output reg                               cfg_dac_a_enable,
    output reg                               cfg_dac_b_enable,
    output reg                               cfg_dac_continuous,
    output reg  [31:0]                       cfg_dac_update_period_clocks,
    output reg  [RAM_ADDR_WIDTH-1:0]         cfg_dac_start_index,
    output reg  [RAM_ADDR_WIDTH-1:0]         cfg_dac_loop_index,
    output reg  [RAM_ADDR_WIDTH-1:0]         cfg_dac_end_index,
    output reg  [31:0]                       cfg_dac_finite_update_count,
    output reg  [7:0]                        cfg_dac_clock_o,
    output reg  [3:0]                        cfg_dac_clock_d,
    output reg  [6:0]                        cfg_dac_clock_m,

    input  wire                              configuration_locked,
    input  wire                              configuration_valid,
    input  wire                              dac_clock_config_valid,
    input  wire                              armed,
    input  wire                              controller_busy,
    input  wire                              ttl_active,
    input  wire                              dac_transaction_active,
    input  wire                              dac_busy,
    input  wire                              dac_clock_locked,
    input  wire                              dac_clock_ready,
    input  wire                              dac_clock_program_busy,
    input  wire                              dac_clock_error,
    input  wire [2:0]                        dac_clock_error_code,
    input  wire [RAM_ADDR_WIDTH-1:0]         dac_current_waveform_index,
    input  wire [31:0]                       dac_completed_update_count,
    input  wire [31:0]                       accepted_trigger_count,
    input  wire [31:0]                       unserved_trigger_count,
    input  wire                              dac_primed,
    input  wire                              dac_prime_busy,
    input  wire                              dac_prime_done_sticky,
    input  wire                              dac_prime_fault_sticky,
    input  wire                              safe_off_active,
    input  wire                              safe_off_event,
    // AXI-domain, one-cycle incident pulses for ERROR_STATUS[6:1]. The
    // register bank supplies reserved bit 0 and local blocked-write bits [8:7].
    input  wire [6:1]                        error_event_vector,
    output wire                              stim_fault_irq,

    output reg  [RAM_ADDR_WIDTH-1:0]         ram_bus_addr,
    output reg                               ram_bus_write_enable,
    output reg  [3:0]                        ram_bus_write_strobes,
    output reg  [31:0]                       ram_bus_write_data,
    output reg                               ram_bus_read_enable,
    input  wire [31:0]                       ram_bus_read_data
);

    localparam [31:0] BLOCK_ID_VALUE = 32'h5354_494D; // "STIM"
    localparam [31:0] ABI_VERSION_VALUE = 32'h0003_0000; // v3
    localparam [31:0] CAPABILITIES_VALUE = 32'h0000_07EF;
    localparam [31:0] ERROR_ALL_MASK = 32'h0000_01FE;
    localparam integer RAM_BASE_INT = 16'h1000;
    localparam integer RAM_BYTES = RAM_DEPTH * 4;
    localparam integer AXI_APERTURE_BYTES = (1 << AXI_ADDR_WIDTH);
    localparam integer REQUIRED_RAM_ADDR_WIDTH =
        (RAM_DEPTH > 1) ? $clog2(RAM_DEPTH) : 1;

    localparam [AXI_ADDR_WIDTH-1:0] ADDR_BLOCK_ID            = 13'h0000;
    localparam [AXI_ADDR_WIDTH-1:0] ADDR_ABI_VERSION         = 13'h0004;
    localparam [AXI_ADDR_WIDTH-1:0] ADDR_CAPABILITIES        = 13'h0008;
    localparam [AXI_ADDR_WIDTH-1:0] ADDR_INFO                = 13'h000C;
    localparam [AXI_ADDR_WIDTH-1:0] ADDR_COMMAND             = 13'h0010;
    localparam [AXI_ADDR_WIDTH-1:0] ADDR_STATUS              = 13'h0014;
    localparam [AXI_ADDR_WIDTH-1:0] ADDR_ACCEPTED_TRIGGER_COUNT = 13'h0018;
    localparam [AXI_ADDR_WIDTH-1:0] ADDR_UNSERVED_TRIGGER_COUNT = 13'h001C;
    localparam [AXI_ADDR_WIDTH-1:0] ADDR_ACTION_CONFIG       = 13'h0020;
    localparam [AXI_ADDR_WIDTH-1:0] ADDR_TTL_PULSE_WIDTH_AXI_CYCLES = 13'h0024;
    localparam [AXI_ADDR_WIDTH-1:0] ADDR_INTAN_STIM_MARKER_MASK = 13'h0028;
    localparam [AXI_ADDR_WIDTH-1:0] ADDR_DAC_PLAYBACK_CONFIG = 13'h0040;
    localparam [AXI_ADDR_WIDTH-1:0] ADDR_DAC_UPDATE_PERIOD_CLOCKS = 13'h0044;
    localparam [AXI_ADDR_WIDTH-1:0] ADDR_DAC_START_INDEX     = 13'h0048;
    localparam [AXI_ADDR_WIDTH-1:0] ADDR_DAC_LOOP_INDEX      = 13'h004C;
    localparam [AXI_ADDR_WIDTH-1:0] ADDR_DAC_END_INDEX       = 13'h0050;
    localparam [AXI_ADDR_WIDTH-1:0] ADDR_DAC_FINITE_UPDATE_COUNT = 13'h0054;
    localparam [AXI_ADDR_WIDTH-1:0] ADDR_DAC_PRIME_STATUS = 13'h0060;
    localparam [AXI_ADDR_WIDTH-1:0] ADDR_DAC_CURRENT_WAVEFORM_INDEX = 13'h0064;
    localparam [AXI_ADDR_WIDTH-1:0] ADDR_DAC_COMPLETED_UPDATE_COUNT = 13'h0068;
    localparam [AXI_ADDR_WIDTH-1:0] ADDR_SAFETY_STATUS       = 13'h0070;
    localparam [AXI_ADDR_WIDTH-1:0] ADDR_SAFE_OFF_COUNT      = 13'h0074;
    localparam [AXI_ADDR_WIDTH-1:0] ADDR_ERROR_STATUS        = 13'h0080;
    localparam [AXI_ADDR_WIDTH-1:0] ADDR_FAULT_IRQ_ENABLE = 13'h0084;
    localparam [AXI_ADDR_WIDTH-1:0] ADDR_ERROR_INCIDENT_CYCLE_COUNT = 13'h0088;
    localparam [AXI_ADDR_WIDTH-1:0] ADDR_LAST_ERROR_VECTOR = 13'h008C;
    localparam [AXI_ADDR_WIDTH-1:0] ADDR_DAC_CLOCK_CONFIG = 13'h00A0;
    localparam [AXI_ADDR_WIDTH-1:0] ADDR_DAC_CLOCK_COMMAND = 13'h00A4;
    localparam [AXI_ADDR_WIDTH-1:0] ADDR_DAC_CLOCK_STATUS = 13'h00A8;
    localparam [AXI_ADDR_WIDTH-1:0] ADDR_RAM_BASE            = 13'h1000;

    localparam [1:0] READ_IDLE = 2'd0;
    localparam [1:0] READ_RAM_WAIT = 2'd1;
    localparam [1:0] READ_RAM_CAPTURE = 2'd2;

    reg aw_pending;
    reg [AXI_ADDR_WIDTH-1:0] awaddr_pending;
    reg w_pending;
    reg [31:0] wdata_pending;
    reg [3:0] wstrb_pending;
    reg [1:0] read_state;
    reg [31:0] error_status;
    reg [31:0] fault_irq_enable;
    reg [31:0] error_incident_cycle_count;
    reg [31:0] last_error_vector;
    reg safe_off_seen;
    reg [31:0] safe_off_count;
    reg status_snapshot_valid;
    reg [31:0] status_snapshot_dac_completed_update_count;

    wire aw_fire = s_axi_awvalid && s_axi_awready;
    wire w_fire = s_axi_wvalid && s_axi_wready;
    wire ar_fire = s_axi_arvalid && s_axi_arready;
    wire have_aw = aw_pending || aw_fire;
    wire have_w = w_pending || w_fire;
    wire write_commit = have_aw && have_w && !s_axi_bvalid;
    wire [AXI_ADDR_WIDTH-1:0] write_address =
        aw_pending ? awaddr_pending : s_axi_awaddr;
    wire [31:0] write_data = w_pending ? wdata_pending : s_axi_wdata;
    wire [3:0] write_strobes = w_pending ? wstrb_pending : s_axi_wstrb;

    // The owned-block ABI is word addressed.  An unaligned access inside the
    // byte range is therefore reserved, not an alias of the preceding RAM word.
    wire write_is_ram = (write_address[1:0] == 2'b00) &&
                        (write_address >= ADDR_RAM_BASE) &&
                        (write_address < (RAM_BASE_INT + RAM_BYTES));
    wire read_is_ram = (s_axi_araddr[1:0] == 2'b00) &&
                       (s_axi_araddr >= ADDR_RAM_BASE) &&
                       (s_axi_araddr < (RAM_BASE_INT + RAM_BYTES));

    wire write_targets_configuration =
        (write_address == ADDR_ACTION_CONFIG) ||
        (write_address == ADDR_TTL_PULSE_WIDTH_AXI_CYCLES) ||
        (write_address == ADDR_INTAN_STIM_MARKER_MASK) ||
        (write_address == ADDR_DAC_PLAYBACK_CONFIG) ||
        (write_address == ADDR_DAC_UPDATE_PERIOD_CLOCKS) ||
        (write_address == ADDR_DAC_START_INDEX) ||
        (write_address == ADDR_DAC_LOOP_INDEX) ||
        (write_address == ADDR_DAC_END_INDEX) ||
        (write_address == ADDR_DAC_FINITE_UPDATE_COUNT) ||
        (write_address == ADDR_DAC_CLOCK_CONFIG);
    // One policy drives both write enforcement and its diagnostic incident.
    wire configuration_write_allowed = !configuration_locked &&
        ((write_address != ADDR_DAC_CLOCK_CONFIG) || dac_clock_ready);
    wire config_write_blocked_event = write_commit && write_targets_configuration &&
        (|write_strobes) && !configuration_write_allowed;
    wire ram_write_blocked_event = write_commit && write_is_ram &&
        (|write_strobes) && configuration_locked;
    wire dac_clock_program_request = write_commit && !write_is_ram &&
        (write_address == ADDR_DAC_CLOCK_COMMAND) &&
        write_strobes[0] && write_data[0];
    wire dac_clock_program_allowed = !configuration_locked && dac_clock_ready;
    wire dac_clock_program_invalid_event = dac_clock_program_request &&
        !dac_clock_config_valid;
    wire dac_clock_program_blocked_event = dac_clock_program_request &&
        dac_clock_config_valid && !dac_clock_program_allowed;

    wire [31:0] status_word = {
        15'd0,
        stim_fault_irq,
        |error_status,
        configuration_locked,
        1'b0, // bit 13 reserved
        dac_prime_busy,
        dac_primed,
        2'b00, // bits 10:9 reserved
        dac_clock_program_busy,
        dac_clock_ready,
        dac_clock_locked,
        configuration_valid,
        dac_busy,
        dac_transaction_active,
        ttl_active,
        controller_busy,
        armed
    };

    assign stim_fault_irq = |(error_status & fault_irq_enable);

    function automatic [31:0] apply_wstrb;
        input [31:0] old_value;
        input [31:0] new_value;
        input [3:0] strobes;
        integer byte_index;
        begin
            apply_wstrb = old_value;
            for (byte_index = 0; byte_index < 4; byte_index = byte_index + 1)
                if (strobes[byte_index])
                    apply_wstrb[byte_index*8 +: 8] = new_value[byte_index*8 +: 8];
        end
    endfunction

    wire error_status_w1c_commit = write_commit && !write_is_ram &&
                                    (write_address == ADDR_ERROR_STATUS);
    wire fault_irq_enable_write_commit = write_commit && !write_is_ram &&
                                      (write_address == ADDR_FAULT_IRQ_ENABLE);
    wire clear_all_diagnostics_commit = write_commit && !write_is_ram &&
        (write_address == ADDR_COMMAND) && write_strobes[0] && write_data[4];
    wire [31:0] error_status_w1c_mask =
        apply_wstrb(32'd0, write_data, write_strobes) & ERROR_ALL_MASK;
    wire [31:0] incident_vector =
        {23'd0, ram_write_blocked_event,
         (config_write_blocked_event || dac_clock_program_blocked_event),
         error_event_vector, 1'b0} |
        (dac_clock_program_invalid_event ? 32'h0000_0002 : 32'd0);
    wire [31:0] error_status_after_clear = clear_all_diagnostics_commit ? 32'd0 :
        (error_status_w1c_commit ?
         (error_status & ~error_status_w1c_mask) : error_status);

    function automatic [31:0] register_read_data;
        input [AXI_ADDR_WIDTH-1:0] address;
        begin
            case (address)
                ADDR_BLOCK_ID:          register_read_data = BLOCK_ID_VALUE;
                ADDR_ABI_VERSION:       register_read_data = ABI_VERSION_VALUE;
                ADDR_CAPABILITIES:      register_read_data = CAPABILITIES_VALUE;
                // channels[31:24], bytes/word[23:16], RAM depth[15:0]
                ADDR_INFO:              register_read_data = {8'd2, 8'd4,
                                                               RAM_DEPTH[15:0]};
                ADDR_COMMAND:           register_read_data = 32'd0;
                ADDR_STATUS:            register_read_data = status_word;
                ADDR_ACTION_CONFIG:           register_read_data = {27'd0, cfg_external_trigger_enable, 2'd0, cfg_action_mode};
                ADDR_INTAN_STIM_MARKER_MASK:    register_read_data = {18'd0, cfg_intan_stim_marker_mask};
                ADDR_TTL_PULSE_WIDTH_AXI_CYCLES:   register_read_data = cfg_ttl_pulse_width_axi_cycles;
                ADDR_DAC_PLAYBACK_CONFIG:           register_read_data = {29'd0,
                                                               cfg_dac_continuous,
                                                               cfg_dac_b_enable,
                                                               cfg_dac_a_enable};
                ADDR_DAC_UPDATE_PERIOD_CLOCKS:    register_read_data = cfg_dac_update_period_clocks;
                ADDR_DAC_START_INDEX:   register_read_data = {{(32-RAM_ADDR_WIDTH){1'b0}}, cfg_dac_start_index};
                ADDR_DAC_LOOP_INDEX:    register_read_data = {{(32-RAM_ADDR_WIDTH){1'b0}}, cfg_dac_loop_index};
                ADDR_DAC_END_INDEX:     register_read_data = {{(32-RAM_ADDR_WIDTH){1'b0}}, cfg_dac_end_index};
                ADDR_DAC_FINITE_UPDATE_COUNT: register_read_data = cfg_dac_finite_update_count;
                ADDR_DAC_PRIME_STATUS:   register_read_data = {
                                                               28'd0,
                                                               dac_prime_fault_sticky,
                                                               dac_prime_done_sticky,
                                                               dac_prime_busy,
                                                               dac_primed};
                // Reading DAC_CURRENT_WAVEFORM_INDEX snapshots both fields. A
                // subsequent DAC_COMPLETED_UPDATE_COUNT read returns its paired value, even if the
                // live playback bundle advances between the two AXI accesses.
                ADDR_DAC_CURRENT_WAVEFORM_INDEX:     register_read_data = {{(32-RAM_ADDR_WIDTH){1'b0}}, dac_current_waveform_index};
                ADDR_DAC_COMPLETED_UPDATE_COUNT: register_read_data = status_snapshot_valid ?
                                                             status_snapshot_dac_completed_update_count :
                                                             dac_completed_update_count;
                ADDR_SAFETY_STATUS:      register_read_data = {30'd0,
                                                               safe_off_seen,
                                                               safe_off_active};
                ADDR_SAFE_OFF_COUNT:     register_read_data = safe_off_count;
                ADDR_ACCEPTED_TRIGGER_COUNT: register_read_data = accepted_trigger_count;
                ADDR_UNSERVED_TRIGGER_COUNT:   register_read_data = unserved_trigger_count;
                ADDR_ERROR_STATUS:      register_read_data = error_status;
                ADDR_FAULT_IRQ_ENABLE:      register_read_data = fault_irq_enable;
                ADDR_ERROR_INCIDENT_CYCLE_COUNT:       register_read_data = error_incident_cycle_count;
                ADDR_LAST_ERROR_VECTOR:        register_read_data = last_error_vector;
                ADDR_DAC_CLOCK_CONFIG:         register_read_data = {13'd0, cfg_dac_clock_m, cfg_dac_clock_d, cfg_dac_clock_o};
                ADDR_DAC_CLOCK_COMMAND:     register_read_data = 32'd0;
                ADDR_DAC_CLOCK_STATUS:      register_read_data = {
                    25'd0, dac_clock_error_code, dac_clock_error,
                    dac_clock_program_busy, dac_clock_ready, dac_clock_locked};
                default:                register_read_data = 32'd0;
            endcase
        end
    endfunction

    assign s_axi_awready = !aw_pending && !s_axi_bvalid;
    assign s_axi_wready = !w_pending && !s_axi_bvalid;
    // The waveform RAM uses one shared bus address register.  Do not accept a
    // read on the same edge that commits a write: otherwise the read-address
    // assignment would replace the address needed by the RAM's delayed write
    // enable on the following clock edge.
    assign s_axi_arready = (read_state == READ_IDLE) && !s_axi_rvalid &&
                           !write_commit;

    initial begin
        if (AXI_DATA_WIDTH != 32) begin
            $error("stim_axil_register_bank: AXI_DATA_WIDTH must be 32");
            $finish;
        end
        if (RAM_DEPTH < 1) begin
            $error("stim_axil_register_bank: RAM_DEPTH must be positive");
            $finish;
        end
        if (RAM_ADDR_WIDTH != REQUIRED_RAM_ADDR_WIDTH) begin
            $error("stim_axil_register_bank: RAM_ADDR_WIDTH must match RAM_DEPTH");
            $finish;
        end
        if ((RAM_BASE_INT + RAM_BYTES) > AXI_APERTURE_BYTES) begin
            $error("stim_axil_register_bank: 8KB aperture is too small for waveform RAM");
            $finish;
        end
    end

    always @(posedge clk) begin
        arm_pulse <= 1'b0;
        disarm_pulse <= 1'b0;
        software_trigger_pulse <= 1'b0;
        stop_pulse <= 1'b0;
        clear_diagnostics_pulse <= 1'b0;
        prime_dac_pulse <= 1'b0;
        dac_clock_program_pulse <= 1'b0;
        dac_prime_invalidate_pulse <= 1'b0;
        ram_bus_write_enable <= 1'b0;
        ram_bus_read_enable <= 1'b0;

        if (!resetn) begin
            aw_pending <= 1'b0;
            awaddr_pending <= {AXI_ADDR_WIDTH{1'b0}};
            w_pending <= 1'b0;
            wdata_pending <= 32'd0;
            wstrb_pending <= 4'd0;
            s_axi_bresp <= 2'b00;
            s_axi_bvalid <= 1'b0;
            s_axi_rdata <= 32'd0;
            s_axi_rresp <= 2'b00;
            s_axi_rvalid <= 1'b0;
            read_state <= READ_IDLE;

            cfg_action_mode <= 2'd0;
            cfg_external_trigger_enable <= 1'b0;
            cfg_ttl_pulse_width_axi_cycles <= 32'd0;
            cfg_intan_stim_marker_mask <= 14'd0;
            cfg_dac_a_enable <= 1'b1;
            cfg_dac_b_enable <= 1'b1;
            cfg_dac_continuous <= 1'b0;
            cfg_dac_update_period_clocks <= 32'd16000;
            cfg_dac_start_index <= {RAM_ADDR_WIDTH{1'b0}};
            cfg_dac_loop_index <= {RAM_ADDR_WIDTH{1'b0}};
            cfg_dac_end_index <= {RAM_ADDR_WIDTH{1'b0}};
            cfg_dac_finite_update_count <= 32'd1;
            cfg_dac_clock_o <= 8'd35;
            cfg_dac_clock_d <= 4'd2;
            cfg_dac_clock_m <= 7'd20;

            ram_bus_addr <= {RAM_ADDR_WIDTH{1'b0}};
            ram_bus_write_strobes <= 4'd0;
            ram_bus_write_data <= 32'd0;
            error_status <= 32'd0;
            fault_irq_enable <= 32'h0000_003E;
            error_incident_cycle_count <= 32'd0;
            last_error_vector <= 32'd0;
            safe_off_seen <= 1'b0;
            safe_off_count <= 32'd0;
            status_snapshot_valid <= 1'b0;
            status_snapshot_dac_completed_update_count <= 32'd0;
        end else begin
            if (s_axi_bvalid && s_axi_bready)
                s_axi_bvalid <= 1'b0;
            if (s_axi_rvalid && s_axi_rready)
                s_axi_rvalid <= 1'b0;

            if (write_commit) begin
                aw_pending <= 1'b0;
                w_pending <= 1'b0;
                s_axi_bresp <= 2'b00;
                s_axi_bvalid <= 1'b1;

                if (write_is_ram) begin
                    if (!configuration_locked && (|write_strobes)) begin
                        ram_bus_addr <= (write_address - ADDR_RAM_BASE) >> 2;
                        ram_bus_write_data <= write_data;
                        ram_bus_write_strobes <= write_strobes;
                        ram_bus_write_enable <= 1'b1;
                        dac_prime_invalidate_pulse <= 1'b1;
                    end
                end else begin
                    case (write_address)
                        ADDR_COMMAND: begin
                            if (write_strobes[0]) begin
                                if (write_data[0]) arm_pulse <= 1'b1;
                                if (write_data[1]) disarm_pulse <= 1'b1;
                                if (write_data[2]) software_trigger_pulse <= 1'b1;
                                if (write_data[3]) stop_pulse <= 1'b1;
                                if (write_data[4]) begin
                                    clear_diagnostics_pulse <= 1'b1;
                                    status_snapshot_valid <= 1'b0;
                                end
                                if (write_data[5]) prime_dac_pulse <= 1'b1;
                            end
                        end
                        ADDR_DAC_CLOCK_COMMAND: begin
                            if (dac_clock_program_request &&
                                dac_clock_config_valid && dac_clock_program_allowed) begin
                                dac_clock_program_pulse <= 1'b1;
                                dac_prime_invalidate_pulse <= 1'b1;
                            end
                        end
                        default: begin
                            if (write_targets_configuration &&
                                configuration_write_allowed && (|write_strobes)) begin
                                case (write_address)
                                    ADDR_ACTION_CONFIG: begin
                                        automatic reg [31:0] merged;
                                        merged = apply_wstrb({27'd0, cfg_external_trigger_enable, 2'd0,
                                                             cfg_action_mode}, write_data, write_strobes);
                                        cfg_action_mode <= merged[1:0];
                                        cfg_external_trigger_enable <= merged[4];
                                        // PRIME_DAC belongs to the complete action
                                        // configuration.  Changing mode or trigger
                                        // policy requires an explicit re-prime and,
                                        // importantly, cannot leave a dormant DAC
                                        // prime attached to a later TTL-only session.
                                        dac_prime_invalidate_pulse <= 1'b1;
                                    end
                                    ADDR_INTAN_STIM_MARKER_MASK: begin
                                        automatic reg [31:0] merged;
                                        merged = apply_wstrb({18'd0, cfg_intan_stim_marker_mask}, write_data, write_strobes);
                                        cfg_intan_stim_marker_mask <= merged[13:0];
                                    end
                                    ADDR_TTL_PULSE_WIDTH_AXI_CYCLES:
                                        cfg_ttl_pulse_width_axi_cycles <= apply_wstrb(cfg_ttl_pulse_width_axi_cycles, write_data, write_strobes);
                                    ADDR_DAC_PLAYBACK_CONFIG: begin
                                        automatic reg [31:0] merged;
                                        merged = apply_wstrb({29'd0,
                                                             cfg_dac_continuous,
                                                             cfg_dac_b_enable,
                                                             cfg_dac_a_enable}, write_data, write_strobes);
                                        cfg_dac_a_enable <= merged[0];
                                        cfg_dac_b_enable <= merged[1];
                                        cfg_dac_continuous <= merged[2];
                                        dac_prime_invalidate_pulse <= 1'b1;
                                    end
                                    ADDR_DAC_UPDATE_PERIOD_CLOCKS: begin
                                        cfg_dac_update_period_clocks <= apply_wstrb(cfg_dac_update_period_clocks, write_data, write_strobes);
                                        dac_prime_invalidate_pulse <= 1'b1;
                                    end
                                    ADDR_DAC_START_INDEX: begin
                                        automatic reg [31:0] merged;
                                        merged = apply_wstrb({{(32-RAM_ADDR_WIDTH){1'b0}}, cfg_dac_start_index},
                                                             write_data, write_strobes);
                                        cfg_dac_start_index <= merged[RAM_ADDR_WIDTH-1:0];
                                        dac_prime_invalidate_pulse <= 1'b1;
                                    end
                                    ADDR_DAC_LOOP_INDEX: begin
                                        automatic reg [31:0] merged;
                                        merged = apply_wstrb({{(32-RAM_ADDR_WIDTH){1'b0}}, cfg_dac_loop_index},
                                                             write_data, write_strobes);
                                        cfg_dac_loop_index <= merged[RAM_ADDR_WIDTH-1:0];
                                        dac_prime_invalidate_pulse <= 1'b1;
                                    end
                                    ADDR_DAC_END_INDEX: begin
                                        automatic reg [31:0] merged;
                                        merged = apply_wstrb({{(32-RAM_ADDR_WIDTH){1'b0}}, cfg_dac_end_index},
                                                             write_data, write_strobes);
                                        cfg_dac_end_index <= merged[RAM_ADDR_WIDTH-1:0];
                                        dac_prime_invalidate_pulse <= 1'b1;
                                    end
                                    ADDR_DAC_FINITE_UPDATE_COUNT: begin
                                        cfg_dac_finite_update_count <= apply_wstrb(cfg_dac_finite_update_count, write_data, write_strobes);
                                        dac_prime_invalidate_pulse <= 1'b1;
                                    end
                                    ADDR_DAC_CLOCK_CONFIG: begin
                                        automatic reg [31:0] merged;
                                        merged = apply_wstrb({13'd0, cfg_dac_clock_m, cfg_dac_clock_d, cfg_dac_clock_o},
                                                             write_data, write_strobes);
                                        cfg_dac_clock_o <= merged[7:0];
                                        cfg_dac_clock_d <= merged[11:8];
                                        cfg_dac_clock_m <= merged[18:12];
                                        dac_prime_invalidate_pulse <= 1'b1;
                                    end
                                    default: begin end
                                endcase
                            end
                        end
                    endcase
                end
            end else begin
                if (aw_fire) begin
                    aw_pending <= 1'b1;
                    awaddr_pending <= s_axi_awaddr;
                end
                if (w_fire) begin
                    w_pending <= 1'b1;
                    wdata_pending <= s_axi_wdata;
                    wstrb_pending <= s_axi_wstrb;
                end
            end

            case (read_state)
                READ_IDLE: begin
                    if (ar_fire) begin
                        if (read_is_ram) begin
                            ram_bus_addr <= (s_axi_araddr - ADDR_RAM_BASE) >> 2;
                            ram_bus_read_enable <= 1'b1;
                            read_state <= READ_RAM_WAIT;
                        end else begin
                            s_axi_rdata <= register_read_data(s_axi_araddr);
                            s_axi_rresp <= 2'b00;
                            s_axi_rvalid <= 1'b1;
                            if (s_axi_araddr == ADDR_DAC_CURRENT_WAVEFORM_INDEX) begin
                                status_snapshot_dac_completed_update_count <= dac_completed_update_count;
                                status_snapshot_valid <= 1'b1;
                            end else if ((s_axi_araddr == ADDR_DAC_COMPLETED_UPDATE_COUNT) &&
                                         status_snapshot_valid) begin
                                status_snapshot_valid <= 1'b0;
                            end
                        end
                    end
                end
                READ_RAM_WAIT:
                    read_state <= READ_RAM_CAPTURE;
                READ_RAM_CAPTURE: begin
                    s_axi_rdata <= ram_bus_read_data;
                    s_axi_rresp <= 2'b00;
                    s_axi_rvalid <= 1'b1;
                    read_state <= READ_IDLE;
                end
                default:
                    read_state <= READ_IDLE;
            endcase

            // Error registers are deliberately independent of the stimulation
            // configuration lock so software can always mask or acknowledge an
            // interrupt. CLEAR_DIAGNOSTICS clears the incident history and existing
            // controller diagnostics; a simultaneous new event is set-dominant.
            if (fault_irq_enable_write_commit)
                fault_irq_enable <= apply_wstrb(fault_irq_enable, write_data,
                                             write_strobes) & ERROR_ALL_MASK;

            if (clear_all_diagnostics_commit) begin
                error_status <= 32'd0;
                error_incident_cycle_count <= 32'd0;
                last_error_vector <= 32'd0;
            end else if (error_status_w1c_commit) begin
                error_status <= error_status & ~error_status_w1c_mask;
            end

            if (incident_vector != 32'd0) begin
                error_status <= error_status_after_clear | incident_vector;
                last_error_vector <= incident_vector;
                if (clear_all_diagnostics_commit)
                    error_incident_cycle_count <= 32'd1;
                else if (error_incident_cycle_count != 32'hFFFF_FFFF)
                    error_incident_cycle_count <= error_incident_cycle_count + 32'd1;
            end

            // The hardware safety button is an operator action, not a fault.
            // Keep independent, saturating history and make a simultaneous
            // button event dominant over CLEAR_DIAGNOSTICS.
            if (clear_all_diagnostics_commit) begin
                safe_off_seen <= 1'b0;
                safe_off_count <= 32'd0;
            end
            if (safe_off_event) begin
                safe_off_seen <= 1'b1;
                if (clear_all_diagnostics_commit)
                    safe_off_count <= 32'd1;
                else if (safe_off_count != 32'hFFFF_FFFF)
                    safe_off_count <= safe_off_count + 32'd1;
            end
        end
    end

    // AXI protection attributes are accepted but do not affect this local register aperture.

endmodule

`default_nettype wire
