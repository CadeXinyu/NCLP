# KR260/NCLP clock constraints
# Target part: xck26-sfvc784-2LV-c
#
# Clock architecture:
#   * clk_25mhz on C3 drives platform/clk_wiz.
#   * platform/clk_wiz/clk_out1 is the 100 MHz fabric/control/AXI clock.
#   * platform/clk_wiz/clk_out2 is the 140 MHz Intan/DAC clock-generator reference.

# Intan/control 25 MHz PL reference clock: HPA_CLK0_P som240_1_a6.
# clk_wiz IP XDC creates the clk_25mhz clock on this port; keep only the pin
# constraint here to avoid duplicate-clock warnings.
set_property -dict {LOC C3 IOSTANDARD LVCMOS18} [get_ports {clk_25mhz}]

# The 100 MHz AXI/control s00_axi_aclk and the runtime-reconfigured SPI clock
# have no fixed phase relationship in the design contract.  Their crossings use
# explicit CDC logic and asynchronous FIFOs.  Resolve each auto-derived clock
# through a stable module or clock-buffer pin so this constraint does not depend
# on the generated clock name or on the BD-generated inst/impl wrapper hierarchy.
set_clock_groups -quiet -asynchronous \
    -group [get_clocks -quiet -of_objects \
        [get_pins -quiet {NCLP_i/capture/intan_spi_module/s00_axi_aclk}]] \
    -group [get_clocks -quiet -of_objects \
        [get_pins -quiet -hierarchical -filter \
            {NAME =~ */spi_clkgen/u_output_buffer/u_output_bufg/O}]] \
    -group [get_clocks -quiet -of_objects \
        [get_pins -quiet -hierarchical -filter \
            {NAME =~ */stim_controller/inst/impl/g_real_clock.dac_clkgen/u_output_buffer/u_output_bufg/O}]]
