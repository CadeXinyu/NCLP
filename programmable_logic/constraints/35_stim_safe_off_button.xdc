# RPi header physical pin 31 (GPIO6). The board-level safety button idles high
# and is active-low when pressed. The pull-up also leaves an open input inactive.
set_property -dict {PACKAGE_PIN AG13 IOSTANDARD LVCMOS33} [get_ports {button_0}]
set_property PULLTYPE PULLUP [get_ports {button_0}]

# The input is synchronized and release-debounced in stim_safe_off_button.
set_false_path -from [get_ports {button_0}]
