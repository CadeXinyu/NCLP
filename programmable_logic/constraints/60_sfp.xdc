# KR260 SFP+ candidate carrier mapping, device side validated in Vivado 2025.1.
# See docs/sfp-hardware.md for evidence and the remaining XTP743 revision check.
# AMD XTP743 currently redirects to an account page; exact carrier trace review
# is still required before a hardware build. Do not describe this as board tested.
# Load this file only in designs that instantiate the physical SFP hierarchy.
# SOM240_2: B1/B2 RX DP2, B5/B6 TX DP2; C3/C4 REFCLK0.
# For xck26-sfvc784-2LV-c in Vivado 2025.1 these use
# GTHE4_CHANNEL_X0Y6 and GTHE4_COMMON_X0Y1 (not X1Y12/X1Y3).
set_property PACKAGE_PIN T2 [get_ports sfp_rx_p]
set_property PACKAGE_PIN T1 [get_ports sfp_rx_n]
set_property PACKAGE_PIN R4 [get_ports sfp_tx_p]
set_property PACKAGE_PIN R3 [get_ports sfp_tx_n]
set_property PACKAGE_PIN Y6 [get_ports sfp_refclk_p]
set_property PACKAGE_PIN Y5 [get_ports sfp_refclk_n]
create_clock -name sfp_refclk -period 6.400 [get_ports sfp_refclk_p]
# Dedicated GT pins intentionally have no LVCMOS/LVDS IOSTANDARD.
set_property -dict {PACKAGE_PIN Y10 IOSTANDARD LVCMOS33 DRIVE 8 SLEW SLOW} [get_ports sfp_tx_disable]
set_property -dict {PACKAGE_PIN W10 IOSTANDARD LVCMOS33} [get_ports sfp_mod_abs]
set_property -dict {PACKAGE_PIN A10 IOSTANDARD LVCMOS33} [get_ports sfp_tx_fault]
set_property -dict {PACKAGE_PIN J12 IOSTANDARD LVCMOS33} [get_ports sfp_rx_los]
# Asynchronous module status ends at the two-stage sideband synchronizer;
# no timing relationship to the local PL clock exists at these pins.
set_false_path -from [get_ports {sfp_mod_abs sfp_tx_fault sfp_rx_los}]
set_false_path -to [get_ports sfp_tx_disable]
# XPM asynchronous FIFO constraints protect Gray pointer CDC. Do not apply
# blanket asynchronous clock groups over those generated max-delay bounds.
# The three independent status levels from Aurora are sampled by the first
# stage of the explicit two-stage synchronizer. Cut only these D pins; stage
# two remains timed, and the unrelated XPM Gray-pointer bounds remain active.
set nclp_sfp_status_meta [get_cells -quiet -hierarchical -filter \
    {NAME =~ */compute/sfp_link/cdc/inst/aurora_status_meta_reg*}]
set_false_path -quiet -to [get_pins -quiet -of_objects $nclp_sfp_status_meta \
    -filter {REF_PIN_NAME == D}]
