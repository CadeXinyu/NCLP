`timescale 1ns/1ps
`default_nettype none

// The compute mode selects one trigger source. A source must return low after
// reset or a mode change before its next rising edge can produce a pulse.
// This prevents a held level from becoming a new trigger when routes change.
// Stimulation itself owns all armed/busy/safe-off decisions.
module stim_trigger_mux (
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 clk CLK" *)
    (* X_INTERFACE_PARAMETER = "FREQ_HZ 100000000, ASSOCIATED_RESET resetn" *)
    input wire clk,
    (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 resetn RST" *)
    (* X_INTERFACE_PARAMETER = "POLARITY ACTIVE_LOW" *)
    input wire resetn,
    input wire sfp_mode_selected,
    input wire local_stim_trigger,
    input wire sfp_stim_trigger,
    output reg stim_trigger
);
    reg sfp_mode_selected_prev;
    reg selected_trigger_armed;
    wire selected_trigger = sfp_mode_selected ? sfp_stim_trigger :
                                                local_stim_trigger;
    always @(posedge clk) begin
        if (!resetn) begin
            sfp_mode_selected_prev <= 1'b0;
            selected_trigger_armed <= 1'b0;
            stim_trigger <= 1'b0;
        end else begin
            sfp_mode_selected_prev <= sfp_mode_selected;
            stim_trigger <= 1'b0;
            if (sfp_mode_selected != sfp_mode_selected_prev)
                selected_trigger_armed <= !selected_trigger;
            else if (!selected_trigger)
                selected_trigger_armed <= 1'b1;
            else if (selected_trigger_armed) begin
                stim_trigger <= 1'b1;
                selected_trigger_armed <= 1'b0;
            end
        end
    end
endmodule

`default_nettype wire
