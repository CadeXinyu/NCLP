`timescale 1ns / 1ps
`default_nettype none

// Qualify a dynamically programmed primitive output before it reaches the
// global clock network. Permission and LOCKED first pass through recognized
// two-flop synchronizers in the raw-clock domain. The startup shift register
// then requires STARTUP_EDGES additional stable raw-clock edges before it can
// enable the global clock (eight edges with the default parameter).
module clock_safe_startup #(
    parameter integer STARTUP_EDGES = 8
) (
    input  wire logic raw_clock,
    input  wire logic reset,
    input  wire logic primitive_locked,
    input  wire logic permission,
    output wire logic clock_out
);
    (* ASYNC_REG = "TRUE" *) logic [1:0] permission_sync = 2'b00;
    (* ASYNC_REG = "TRUE" *) logic [1:0] locked_sync = 2'b00;
    logic [STARTUP_EDGES-1:0] startup_pipe = '0;

    // Only the recognized synchronizer stages need asynchronous assertion.
    // Their cleared outputs hold BUFGCE disabled even if raw_clock is stopped.
    always_ff @(posedge raw_clock or posedge reset) begin
        if (reset) begin
            permission_sync <= 2'b00;
            locked_sync     <= 2'b00;
        end else begin
            permission_sync <= {permission_sync[0], permission};
            locked_sync     <= {locked_sync[0], primitive_locked};
        end
    end

    // Clearing from the synchronized qualification state gives this pipeline
    // synchronous reset removal and avoids an asynchronous-reset CDC on every
    // startup bit. The first raw edge after reset necessarily observes the
    // synchronizer outputs low and clears any pre-reset pipeline contents.
    always_ff @(posedge raw_clock) begin
        if (!permission_sync[1] || !locked_sync[1])
            startup_pipe <= '0;
        else
            startup_pipe <= {startup_pipe[STARTUP_EDGES-2:0], 1'b1};
    end

    BUFGCE #(
        .CE_TYPE("SYNC"),
        .SIM_DEVICE("ULTRASCALE_PLUS")
    ) u_output_bufg (
        .I(raw_clock),
        .CE(permission_sync[1] && locked_sync[1] &&
            startup_pipe[STARTUP_EDGES-1]),
        .O(clock_out)
    );

    initial begin
        if (STARTUP_EDGES < 2)
            $error("STARTUP_EDGES must be at least two");
    end
endmodule

`default_nettype wire
