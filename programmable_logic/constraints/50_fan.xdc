# K26/KR260 fan gate control.
# TTC0 waveform channel 2 is exported through EMIO to HDA20, which maps to
# FPGA package pin A12. The fan polarity and duty cycle are configured in PS
# software; the K26 SOM provides the board-level pull-up.

set_property -dict {PACKAGE_PIN A12 IOSTANDARD LVCMOS33 SLEW SLOW DRIVE 4} [get_ports fan_pwm]

# The external fan gate has no synchronous capture requirement. Only the
# package output endpoint is excluded from timing.
set_false_path -to [get_ports fan_pwm]
