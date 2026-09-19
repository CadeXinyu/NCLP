# Physical stimulation/synchronization I/O for the revised J1 breakout.
#
# J1 mapping:
#   32 TTL IN 1   33 TTL IN 0
#   36 TTL OUT 1  35 TTL OUT 0
#   38 DAC SDIN   37 DAC SCLK
#   40 DAC SYNC   39 GND
#
# Logical Intan TTL[1:0] are these two physical inputs. TTL[15:2] are
# generated internally by the stimulation/algorithm block.

set_property -dict {PACKAGE_PIN AB13 IOSTANDARD LVCMOS33} [get_ports {ttl_in_external[0]}]
set_property -dict {PACKAGE_PIN AA13 IOSTANDARD LVCMOS33} [get_ports {ttl_in_external[1]}]

# Bias open external TTL inputs low during bring-up.
set_property PULLTYPE PULLDOWN [get_ports {ttl_in_external[0]}]
set_property PULLTYPE PULLDOWN [get_ports {ttl_in_external[1]}]

set_property -dict {PACKAGE_PIN Y13  IOSTANDARD LVCMOS33 SLEW FAST DRIVE 8} [get_ports {ttl_out[0]}]
set_property -dict {PACKAGE_PIN AB15 IOSTANDARD LVCMOS33 SLEW FAST DRIVE 8} [get_ports {ttl_out[1]}]

# MCP4922 uses SPI mode 0. LDAC is tied low on the board, so each rising
# DAC_SYNC_n edge transfers that channel's input register directly to output.
set_property -dict {PACKAGE_PIN AB10 IOSTANDARD LVCMOS33 SLEW FAST DRIVE 8} [get_ports {dac_sclk}]
set_property -dict {PACKAGE_PIN W12  IOSTANDARD LVCMOS33 SLEW FAST DRIVE 8} [get_ports {dac_sdin}]
set_property -dict {PACKAGE_PIN W11  IOSTANDARD LVCMOS33 SLEW FAST DRIVE 8} [get_ports {dac_sync_n}]

# These package pins have no board-defined synchronous timing contract.
# Keep exceptions at the package boundary; internal CDC and routing remain
# normally timed.
set_false_path -from [get_ports {ttl_in_external[*]}]
set_false_path -to [get_ports {ttl_out[*]}]
set_false_path -to [get_ports {dac_sclk dac_sdin dac_sync_n}]
