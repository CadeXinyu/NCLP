`timescale 1ns / 1ps
`default_nettype none

// Route TTC0 waveform channel 2 to the board fan gate.  Keeping this tiny
// board-level selection in RTL avoids the xlslice IP, which is deprecated
// after Vivado 2025.1.
module fan_pwm_select (
    input  wire [2:0] ttc_wave,
    output wire       fan_pwm
);
    assign fan_pwm = ttc_wave[2];
endmodule

`default_nettype wire
