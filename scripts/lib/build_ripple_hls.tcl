# Run from any directory:
# vitis-run --mode hls --tcl /absolute/path/scripts/lib/build_ripple_hls.tcl
set repo_root [file normalize [file join [file dirname [info script]] .. ..]]
set source_dir [file join $repo_root programmable_logic hls ripple_detector]
set build_dir [file join $source_dir build]
cd $source_dir
open_project -reset $build_dir
open_solution -reset solution1 -flow_target vivado
set_top nclp_ripple_hls
add_files [file join $source_dir ripple_detector.cpp] -cflags "-std=c++11"
set_part {xck26-sfvc784-2LV-c}
create_clock -period 10 -name default
config_compile -pipeline_loops 0
config_rtl -reset state -reset_level low
puts "Building integer fixed-point ripple detector: Q1.17 FIR, Q2.16 IIR"
# Preserve the repository's maximum four-thread policy for tool subprocesses.
set_param general.maxThreads 4
csynth_design
exit
