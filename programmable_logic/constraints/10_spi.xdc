# Intan SPI interface constraints for KR260/K26
# Board/pin source: reference/kr260.xdc and reference/pi_header.txt
# Top-level source: NCLP_wrapper generated from the current block design.

# Intan SPI interface on KR260 Raspberry Pi header, LVCMOS33.
set_property -dict {PACKAGE_PIN AG14 IOSTANDARD LVCMOS33 SLEW FAST DRIVE 8} [get_ports sclk]
set_property -dict {PACKAGE_PIN W14  IOSTANDARD LVCMOS33 SLEW FAST DRIVE 8} [get_ports cs]
set_property -dict {PACKAGE_PIN W13  IOSTANDARD LVCMOS33 SLEW FAST DRIVE 8} [get_ports mosi_a]
set_property -dict {PACKAGE_PIN Y14  IOSTANDARD LVCMOS33 SLEW FAST DRIVE 8} [get_ports mosi_c]
set_property -dict {PACKAGE_PIN AB14 IOSTANDARD LVCMOS33 SLEW FAST DRIVE 8} [get_ports mosi_b]
set_property -dict {PACKAGE_PIN AB9  IOSTANDARD LVCMOS33 SLEW FAST DRIVE 8} [get_ports mosi_d]

set_property -dict {PACKAGE_PIN Y9   IOSTANDARD LVCMOS33} [get_ports miso_a1]
set_property -dict {PACKAGE_PIN AE13 IOSTANDARD LVCMOS33} [get_ports miso_a2]
set_property -dict {PACKAGE_PIN AC13 IOSTANDARD LVCMOS33} [get_ports miso_b1]
set_property -dict {PACKAGE_PIN AA8  IOSTANDARD LVCMOS33} [get_ports miso_b2]
set_property -dict {PACKAGE_PIN AF13 IOSTANDARD LVCMOS33} [get_ports miso_c1]
set_property -dict {PACKAGE_PIN AC14 IOSTANDARD LVCMOS33} [get_ports miso_c2]
set_property -dict {PACKAGE_PIN AH13 IOSTANDARD LVCMOS33} [get_ports miso_d1]
set_property -dict {PACKAGE_PIN AH14 IOSTANDARD LVCMOS33} [get_ports miso_d2]

# Pack the SPI output launch registers into the I/O boundary when legal. This
# reduces fabric-to-pad routing delay and output skew for the source-synchronous
# Intan SPI bus.
set_property IOB TRUE [get_ports sclk]
set_property IOB TRUE [get_ports cs]
set_property IOB TRUE [get_ports mosi_a]
set_property IOB TRUE [get_ports mosi_b]
set_property IOB TRUE [get_ports mosi_c]
set_property IOB TRUE [get_ports mosi_d]

# Keep unplugged MISO inputs from floating during bring-up. Remove these if the
# external Intan adapter board already has stronger biasing.
set_property PULLTYPE PULLDOWN [get_ports miso_a1]
set_property PULLTYPE PULLDOWN [get_ports miso_a2]
set_property PULLTYPE PULLDOWN [get_ports miso_b1]
set_property PULLTYPE PULLDOWN [get_ports miso_b2]
set_property PULLTYPE PULLDOWN [get_ports miso_c1]
set_property PULLTYPE PULLDOWN [get_ports miso_c2]
set_property PULLTYPE PULLDOWN [get_ports miso_d1]
set_property PULLTYPE PULLDOWN [get_ports miso_d2]

# The FPGA pins drive external LVCMOS-to-LVDS interface devices. SLEW FAST is
# an electrical output-buffer choice; the constraints below provide the
# package-I/O timing contract used for implementation.
#
# Reference all SPI package paths to the surviving output-BUFG clock object. The
# default/faster profile is 84 MHz; runtime reconfiguration only slows it to
# 70/56/42/28/14 MHz. SCLK is registered data from this clock (a 21 MHz
# waveform at the fastest profile), not a clock-tree endpoint, so do not create
# a generated clock on the SCLK output port.
set spi_clock_pin [get_pins -quiet -hierarchical -filter \
    {NAME =~ */spi_clkgen/u_output_buffer/u_output_bufg/O}]
set spi_sampling_clock [get_clocks -quiet -of_objects $spi_clock_pin]

# Provisional external-interface allowance. Until measured transmitter,
# receiver, PCB, cable, and Intan setup/hold limits are available, use the
# Glance-style package bounds agreed for bring-up: every outbound SPI signal
# has +2 ns max / -2 ns min relative to spi_clk.
set spi_driven_outputs [get_ports {sclk cs mosi_a mosi_b mosi_c mosi_d}]
set_output_delay -clock $spi_sampling_clock -max 2.000 $spi_driven_outputs
set_output_delay -clock $spi_sampling_clock -min -2.000 $spi_driven_outputs

# MISO is captured on all four quarter-SCLK phases and the valid phase is
# selected per lane at runtime. Constrain the package-to-sampler paths with the
# agreed provisional 0.5 ns earliest / 3.0 ns latest arrival window. The much
# larger external round-trip latency remains handled by lane phase calibration;
# these values must be replaced after the complete interface is measured.
set spi_sampled_inputs [get_ports {
    miso_a1 miso_a2 miso_b1 miso_b2
    miso_c1 miso_c2 miso_d1 miso_d2
}]
set_input_delay -clock $spi_sampling_clock -max 3.000 $spi_sampled_inputs
set_input_delay -clock $spi_sampling_clock -min 0.500 $spi_sampled_inputs

# Do not false-path this interface. The build audit deliberately fails if any
# critical SPI port loses its I/O-delay coverage or an invalid generated clock
# is introduced.
