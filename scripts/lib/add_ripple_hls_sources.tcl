# Load the complete generated HLS implementation into a project-mode fileset.
# Callers provide pl_root; generated RTL is replaceable build output.
set ripple_hls_rtl_dir [file join $pl_root hls ripple_detector build solution1 syn verilog]
set ripple_hls_rtl_files [lsort [glob -nocomplain -directory $ripple_hls_rtl_dir *.v]]
if {![file isfile [file join $ripple_hls_rtl_dir nclp_ripple_hls.v]] ||
    [llength $ripple_hls_rtl_files] == 0} {
    error "Missing ripple detector HLS RTL in $ripple_hls_rtl_dir. From the NCLP root run: python3 scripts/build_ripple_hls.py"
}
add_files -fileset sources_1 -norecurse $ripple_hls_rtl_files
set ripple_hls_data_files [lsort [glob -nocomplain -directory $ripple_hls_rtl_dir *.dat *.mem *.coe]]
if {[llength $ripple_hls_data_files] > 0} {
    add_files -fileset sources_1 -norecurse $ripple_hls_data_files
}
# Source any optional HLS-generated IP scripts. The fixed-point detector is
# normally self-contained Verilog, so an integer-only RTL tree has none.
foreach ripple_hls_ip_script [lsort [glob -nocomplain -directory $ripple_hls_rtl_dir *_ip.tcl]] {
    source $ripple_hls_ip_script
}
