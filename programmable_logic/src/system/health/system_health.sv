`timescale 1ns / 1ps
`default_nettype none

// Capture/stimulation status aggregation for PS interrupt bit 0 and the
// autonomous board error LED.  A normal Intan-to-DDR transfer completion raises
// intan_ddr_transfer_irq but is not an error. The Intan and stimulation sources are
// error IRQ levels, so each drives both the PS interrupt and board error output.
module system_health (
    input  wire intan_ddr_transfer_irq,
    input  wire intan_ddr_transfer_fault,
    input  wire intan_error_irq,
    input  wire stim_fault_irq,
    output wire ps_irq,
    output wire hardware_error
);

    assign ps_irq = intan_ddr_transfer_irq | intan_error_irq | stim_fault_irq;
    assign hardware_error = intan_ddr_transfer_fault | intan_error_irq |
                            stim_fault_irq;

endmodule

`default_nettype wire
