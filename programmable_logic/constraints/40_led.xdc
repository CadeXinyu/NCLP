# Active-high LED/light constraints for KR260/K26
# Board/pin source: reference/kr260.xdc and revised reference/Pins.png
# Top-level source: NCLP_wrapper generated from the current block design.
# All lights are active-high, so pull down keeps them off during configuration.

set_property -dict {PACKAGE_PIN AE15 IOSTANDARD LVCMOS33 SLEW SLOW DRIVE 4} [get_ports led_a]
set_property -dict {PACKAGE_PIN AE14 IOSTANDARD LVCMOS33 SLEW SLOW DRIVE 4} [get_ports led_b]
set_property -dict {PACKAGE_PIN AA12 IOSTANDARD LVCMOS33 SLEW SLOW DRIVE 4} [get_ports led_c]
set_property -dict {PACKAGE_PIN Y12  IOSTANDARD LVCMOS33 SLEW SLOW DRIVE 4} [get_ports led_d]
set_property -dict {PACKAGE_PIN AD14 IOSTANDARD LVCMOS33 SLEW SLOW DRIVE 4} [get_ports led_error]
set_property -dict {PACKAGE_PIN AD15 IOSTANDARD LVCMOS33 SLEW SLOW DRIVE 4} [get_ports running]

set_property PULLTYPE PULLDOWN [get_ports led_a]
set_property PULLTYPE PULLDOWN [get_ports led_b]
set_property PULLTYPE PULLDOWN [get_ports led_c]
set_property PULLTYPE PULLDOWN [get_ports led_d]
set_property PULLTYPE PULLDOWN [get_ports led_error]
set_property PULLTYPE PULLDOWN [get_ports running]

# Indicator outputs have no external synchronous receiver. Keep the exception
# at the package boundary; the internal LED/error logic remains timed.
set_false_path -to [get_ports {
    led_a led_b led_c led_d led_error running
}]
