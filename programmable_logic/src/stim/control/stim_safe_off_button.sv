`timescale 1ns / 1ps
`default_nettype none

// Synchronize the board's active-low safety button and turn each complete
// press into one safe-off interval. Press assertion is intentionally not
// debounced: the synchronized low level asserts safe_off_active immediately.
// Re-arming requires a continuously high synchronized input for the configured
// number of clock cycles, so contact bounce cannot create additional events.
module stim_safe_off_button #(
    parameter integer RELEASE_DEBOUNCE_CYCLES = 1_000_000
) (
    input  wire clk,
    input  wire resetn,
    input  wire button_n_async,

    output wire safe_off_active,
    output wire safe_off_event
);
    localparam integer RELEASE_COUNT_WIDTH =
        (RELEASE_DEBOUNCE_CYCLES <= 1) ? 1 :
        $clog2(RELEASE_DEBOUNCE_CYCLES);

    (* ASYNC_REG = "TRUE" *) logic button_n_sync_meta;
    (* ASYNC_REG = "TRUE" *) logic button_n_sync_level;

    logic [RELEASE_COUNT_WIDTH-1:0] release_stable_cycles;
    logic safe_off_latched;
    logic press_event_armed;

    wire button_pressed = !button_n_sync_level;

    // OR-ing in the normalized synchronized press avoids adding a third clock
    // of latency. safe_off_latched then holds the request through bounce.
    assign safe_off_active = button_pressed | safe_off_latched;

    // press_event_armed is cleared on the clock after the synchronized pressed level,
    // making this exactly one clock wide without using the asynchronous input.
    assign safe_off_event = button_pressed & press_event_armed;

    always_ff @(posedge clk) begin
        if (!resetn) begin
            button_n_sync_meta  <= 1'b1;
            button_n_sync_level <= 1'b1;
        end else begin
            button_n_sync_meta  <= button_n_async;
            button_n_sync_level <= button_n_sync_meta;
        end
    end

    always_ff @(posedge clk) begin
        if (!resetn) begin
            release_stable_cycles    <= '0;
            safe_off_latched <= 1'b0;
            press_event_armed      <= 1'b1;
        end else if (!safe_off_latched) begin
            release_stable_cycles <= '0;
            if (button_pressed) begin
                safe_off_latched <= 1'b1;
                press_event_armed      <= 1'b0;
            end else begin
                safe_off_latched <= 1'b0;
                press_event_armed      <= 1'b1;
            end
        end else if (button_pressed) begin
            // Any new low/pressed sample restarts release qualification.
            release_stable_cycles <= '0;
            press_event_armed   <= 1'b0;
        end else if (RELEASE_DEBOUNCE_CYCLES <= 1) begin
            release_stable_cycles    <= '0;
            safe_off_latched <= 1'b0;
            press_event_armed      <= 1'b1;
        end else if (release_stable_cycles == RELEASE_DEBOUNCE_CYCLES - 1) begin
            release_stable_cycles    <= '0;
            safe_off_latched <= 1'b0;
            press_event_armed      <= 1'b1;
        end else begin
            release_stable_cycles <= release_stable_cycles + 1'b1;
        end
    end
endmodule

`default_nettype wire
