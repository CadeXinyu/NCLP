`timescale 1ns / 1ps
`default_nettype none

module plle4_clock #(
    // O: output divider, D: input divider, M: feedback multiplier.
    parameter logic [7:0] DEFAULT_O = 8'd35,
    parameter logic [3:0] DEFAULT_D = 4'd2,
    parameter logic [6:0] DEFAULT_M = 7'd20,
    // config_clk settling cycles after destination reset release, before READY.
    parameter integer RESET_RELEASE_CYCLES = 16
) (
    input  wire logic                              config_clk,
    input  wire logic                              source_clk,
    input  wire logic                              reset,
    input  wire logic                              request,
    input  wire logic                              config_valid,
    input  wire logic [7:0]                        config_o,
    input  wire logic [3:0]                        config_d,
    input  wire logic [6:0]                        config_m,
    output logic                                   ready,
    output logic                                   busy,
    output logic                                   done_pulse,
    output logic                                   error,
    output clock_drp_pkg::clock_error_t error_code,
    output logic                                   locked,
    output logic                                   domain_reset_request,
    input  wire logic                              domain_reset_ack,
    output logic                                   clock_out
);

    import clock_drp_pkg::*;

    logic [7:0] active_o;
    logic [3:0] active_d;
    logic [6:0] active_m;
    logic [3:0] command_index;
    drp_command_t command;

    clock_count_t output_count;
    clock_count_t input_count;
    clock_count_t feedback_count;
    logic [39:0] lock_words;
    logic [9:0] filter_words;

    logic [6:0] drp_address;
    logic [15:0] drp_write_data;
    logic [15:0] drp_read_data;
    logic drp_enable;
    logic drp_write_enable;
    logic drp_ready;
    logic pll_reset;
    logic pll_locked;
    logic output_enable;

    (* keep = "true" *) logic source_clk_kept;
    logic pll_feedback;
    logic pll_clock_raw;

    always_comb begin
        output_count   = clock_count(active_o);
        input_count    = clock_count({4'd0, active_d});
        feedback_count = clock_count({1'b0, active_m});
        lock_words   = clock_drp_pkg::lock_settings({1'b0, active_m});
        filter_words = pll_filter_settings({1'b0, active_m});

        command = '0;
        unique case (command_index)
            4'd0: command = '{address: 7'h08, preserve_mask: 16'h1000,
                              data: {4'b0000, output_count.high_time,
                                     output_count.low_time}};
            4'd1: command = '{address: 7'h09, preserve_mask: 16'h8000,
                              data: {8'b00000000, output_count.edge_select,
                                     output_count.no_count, 6'b000000}};
            4'd2: command = '{address: 7'h16, preserve_mask: 16'hC000,
                              data: {2'b00, input_count.edge_select,
                                     input_count.no_count,
                                     input_count.high_time,
                                     input_count.low_time}};
            4'd3: command = '{address: 7'h14, preserve_mask: 16'h1000,
                              data: {4'b0000, feedback_count.high_time,
                                     feedback_count.low_time}};
            4'd4: command = '{address: 7'h15, preserve_mask: 16'h8000,
                              data: {8'b00000000, feedback_count.edge_select,
                                     feedback_count.no_count, 6'b000000}};
            4'd5: command = '{address: 7'h18, preserve_mask: 16'hFC00,
                              data: {6'b000000, lock_words[29:20]}};
            4'd6: command = '{address: 7'h19, preserve_mask: 16'h8000,
                              data: {1'b0, lock_words[34:30],
                                     lock_words[9:0]}};
            4'd7: command = '{address: 7'h1A, preserve_mask: 16'h8000,
                              data: {1'b0, lock_words[39:35],
                                     lock_words[19:10]}};
            4'd8: command = '{address: 7'h4E, preserve_mask: 16'h0000,
                              data: {filter_words[9], 2'b00,
                                     filter_words[8:7], 2'b00,
                                     filter_words[6], 8'b00000000}};
            4'd9: command = '{address: 7'h4F, preserve_mask: 16'h666F,
                               data: {filter_words[5], 2'b00,
                                      filter_words[4:3], 2'b00,
                                      filter_words[2:1], 2'b00,
                                      filter_words[0], 4'b0000}};
            default: command = '0;
        endcase
    end

    logic primitive_config_valid;
    always_comb begin
        primitive_config_valid = config_valid &&
            clock_divider_valid(config_o) &&
            config_d >= 4'd1 &&
            config_m >= 7'd2 && config_m <= 7'd21;
    end

    clock_drp_sequencer #(
        .DEFAULT_O(DEFAULT_O),
        .DEFAULT_D(DEFAULT_D),
        .DEFAULT_M(DEFAULT_M),
        .LAST_COMMAND_INDEX(9),
        .RESET_RELEASE_CYCLES(RESET_RELEASE_CYCLES)
    ) u_drp_sequencer (
        .config_clk(config_clk),
        .reset(reset),
        .request(request),
        .request_valid(primitive_config_valid),
        .requested_o(config_o),
        .requested_d(config_d),
        .requested_m(config_m),
        .active_o(active_o),
        .active_d(active_d),
        .active_m(active_m),
        .command_index(command_index),
        .command(command),
        .primitive_locked(pll_locked),
        .drp_read_data(drp_read_data),
        .drp_ready(drp_ready),
        .drp_address(drp_address),
        .drp_write_data(drp_write_data),
        .drp_enable(drp_enable),
        .drp_write_enable(drp_write_enable),
        .primitive_reset(pll_reset),
        .output_enable(output_enable),
        .domain_reset_request(domain_reset_request),
        .domain_reset_ack(domain_reset_ack),
        .ready(ready),
        .busy(busy),
        .done_pulse(done_pulse),
        .error(error),
        .error_code(error_code),
        .locked(locked)
    );

    assign source_clk_kept = source_clk;

    PLLE4_ADV #(
        .CLKFBOUT_MULT(DEFAULT_M),
        .CLKFBOUT_PHASE(0.000),
        .CLKIN_PERIOD(7.142857),
        .CLKOUT0_DIVIDE(DEFAULT_O),
        .CLKOUT0_DUTY_CYCLE(0.500),
        .CLKOUT0_PHASE(0.000),
        .COMPENSATION("INTERNAL"),
        .DIVCLK_DIVIDE(DEFAULT_D),
        .REF_JITTER(0.010),
        .STARTUP_WAIT("FALSE")
    ) u_pll (
        .CLKFBOUT(pll_feedback),
        .CLKOUT0(pll_clock_raw),
        .CLKOUT0B(),
        .CLKOUT1(),
        .CLKOUT1B(),
        .CLKOUTPHY(),
        .DO(drp_read_data),
        .DRDY(drp_ready),
        .LOCKED(pll_locked),
        .CLKFBIN(pll_feedback),
        .CLKIN(source_clk_kept),
        .CLKOUTPHYEN(1'b0),
        .DADDR(drp_address),
        .DCLK(config_clk),
        .DEN(drp_enable),
        .DI(drp_write_data),
        .DWE(drp_write_enable),
        .PWRDWN(1'b0),
        .RST(pll_reset)
    );

    clock_safe_startup u_output_buffer (
        .raw_clock(pll_clock_raw),
        .reset(pll_reset),
        .primitive_locked(pll_locked),
        .permission(output_enable),
        .clock_out(clock_out)
    );

endmodule

`default_nettype wire
