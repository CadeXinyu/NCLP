# Recreate the clean NCLP Vivado project from programmable_logic resources.
# Run from NCLP root:
#   vivado -mode batch -source scripts/create_project.tcl

# Keep project creation and any runs launched from this session within the
# workstation's safe memory envelope.  Command-specific -jobs values should
# use the same limit.
set_param general.maxThreads 4

set root [file normalize [file join [file dirname [info script]] ..]]
set pl_root [file join $root programmable_logic]
if {[info exists ::env(NCLP_VIVADO_PROJECT_DIR)] &&
    $::env(NCLP_VIVADO_PROJECT_DIR) ne ""} {
    set proj_dir [file normalize $::env(NCLP_VIVADO_PROJECT_DIR)]
} else {
    set proj_dir [file join $root vivado_project]
}
create_project NCLP $proj_dir -part xck26-sfvc784-2LV-c -force
set_property board_part xilinx.com:kr260_som:part0:1.1 [current_project]
set_property target_language Verilog [current_project]
set_property simulator_language Mixed [current_project]
set_property default_lib xil_defaultlib [current_project]
set_property source_mgmt_mode All [current_project]
set_property XPM_LIBRARIES {XPM_CDC XPM_MEMORY XPM_FIFO} [current_project]

set thread_limit_hook [file join $root scripts lib vivado_max_threads_4.tcl]
if {![file exists $thread_limit_hook]} {
    error "Missing Vivado thread-limit hook: $thread_limit_hook"
}
add_files -fileset utils_1 -norecurse $thread_limit_hook
set thread_limit_hook_files [get_files -quiet -of_objects \
    [get_filesets utils_1] -filter "NAME =~ */vivado_max_threads_4.tcl"]
if {[llength $thread_limit_hook_files] != 1} {
    error "Vivado thread-limit hook was not added exactly once: $thread_limit_hook"
}

proc collect_files {dir patterns} {
    set result [list]
    if {![file exists $dir]} { return $result }
    foreach item [glob -nocomplain -directory $dir *] {
        if {[file isdirectory $item]} {
            set result [concat $result [collect_files $item $patterns]]
        } else {
            foreach pat $patterns {
                if {[string match $pat [file tail $item]]} {
                    lappend result $item
                    break
                }
            }
        }
    }
    return $result
}

set intan_register_bank [file join $pl_root src spi control intan_axil_register_bank.sv]
if {![file exists $intan_register_bank]} {
    error "Missing flat Intan ABI v3 register bank: $intan_register_bank"
}

set src_files [collect_files [file join $pl_root src] [list *.v *.sv *.vh *.vhd *.vhdl]]
if {[llength $src_files] > 0} {
    add_files -fileset sources_1 $src_files
}
source [file join $root scripts lib add_ripple_hls_sources.tcl]
if {[llength [get_files -quiet $intan_register_bank]] != 1} {
    error "Flat Intan ABI v3 register bank was not added exactly once: $intan_register_bank"
}

# Vivado 2025.1 IPI does not resolve a direct module-reference cell from a file
# classified as SystemVerilog. These small direct-reference leaves are valid
# Verilog-2001, so classify only their source files as Verilog while retaining
# their unprefixed module and repository file names.
foreach module_reference_source [list \
    [file join $pl_root src system health system_health.sv] \
    [file join $pl_root src system fan fan_pwm_select.sv] \
    [file join $pl_root src compute top stim_trigger_mux.sv] \
] {
    set module_reference_file [get_files -quiet $module_reference_source]
    if {[llength $module_reference_file] != 1} {
        error "Required module-reference source is missing or ambiguous: $module_reference_source"
    }
    set_property FILE_TYPE Verilog $module_reference_file
}

set ip_files [collect_files [file join $pl_root ip] [list *.xci]]
if {[llength $ip_files] > 0} {
    # Import checked-in XCI files so all generated products live inside the
    # recreated project instead of following stale paths from the project that
    # originally exported the IP.
    foreach ip_file $ip_files {
        import_ip -files $ip_file
    }
    foreach ip [get_ips -quiet] {
        if {[get_property IS_LOCKED $ip]} {
            upgrade_ip $ip
        }
        generate_target all $ip
    }
}

set xdc_files [lsort [collect_files [file join $pl_root constraints] [list *.xdc]]]
if {[llength $xdc_files] > 0} {
    puts "Adding constraint files:"
    foreach xdc_file $xdc_files {
        puts "  $xdc_file"
    }
    add_files -fileset constrs_1 $xdc_files
}

update_compile_order -fileset sources_1
foreach required_source {
    compute_fabric_control.sv
    intan_compute_path_selector.sv
    intan_frame_depacketizer.sv
    intan_sfp_packetizer.sv
    nclp_sfp_tx_arbiter.sv
    nclp_sfp_rx_router.sv
    nclp_compute_fabric_bd.v
    nclp_ps_packet_mailbox.sv
    nclp_ps_packet_mailbox_bd.v
    stim_trigger_mux.sv
    nclp_aurora_cdc.v
    ripple_detector.sv
    ripple_detector_bd.v
    nclp_ripple_hls.v
} {
    set source_object [get_files -quiet -of_objects [get_filesets sources_1] \
        -filter "NAME =~ */$required_source"]
    if {[llength $source_object] != 1} {
        error "Required clean compute-fabric source is missing or ambiguous: $required_source"
    }
}
foreach obsolete_source {
    stim_trigger_test.sv
    stim_trigger_test_bd.v
    stim_trigger_scheduler.sv
    compute_stream_control.sv
    intan_packed_stream_router.sv
    nclp_compute_stream_router_bd.v
    nclp_link_controller.sv
    nclp_link_controller_bd.v
    nclp_link_tx.sv
    nclp_link_rx.sv
    nclp_ps_stream_bridge.sv
    nclp_sfp_trigger_source.sv
    nclp_typed_stream_switch.sv
    nclp_typed_stream_arbiter.sv
    nclp_event_fifo.sv
} {
    if {[llength [get_files -quiet -of_objects [get_filesets sources_1] \
            -filter "NAME =~ */$obsolete_source"]] != 0} {
        error "Obsolete compute/LINK source remains in the production project: $obsolete_source"
    }
}
foreach required_module [list \
    axis_ddr_circular_writer_bd \
    NCLP_Intan_SPI_Module_bd \
    pl_led_control_bd \
    stim_controller_bd \
    ttl_output_router_bd \
    ripple_detector_bd \
    nclp_compute_fabric_bd \
    nclp_ps_packet_mailbox_bd \
    stim_trigger_mux \
    nclp_aurora_cdc \
    system_health \
    fan_pwm_select \
] {
    if {![can_resolve_reference $required_module]} {
        error "Required block-design module cannot be resolved: $required_module"
    }
}

source [file join $pl_root block_design create_bd_NCLP.tcl]
set bd_file [file join $proj_dir NCLP.srcs sources_1 bd NCLP NCLP.bd]
if {![file exists $bd_file]} {
    error "NCLP block design was not created completely"
}
foreach required_bd_cell [list \
    capture/intan_spi_module \
    capture/control_interconnect \
    compute/control_interconnect \
    capture/intan_ddr_writer \
    platform/pl_led_control \
    stimulation/stim_controller \
    stimulation/ttl_output_router \
    stimulation/control_interconnect \
    compute/ripple_detector \
    compute/compute_fabric \
    compute/trigger_mux \
    compute/sfp_mailbox \
    compute/sfp_link \
    compute/sfp_link/core \
    compute/sfp_link/cdc \
    platform/system_health \
    platform/fan_pwm_select \
    platform/ps_irq_concat \
] {
    if {[llength [get_bd_cells -quiet $required_bd_cell]] != 1} {
        error "NCLP block design is missing required cell: $required_bd_cell"
    }
}
validate_bd_design
source [file join $pl_root block_design validate_bd_NCLP.tcl]
validate_nclp_bd
save_bd_design
if {[info exists ::env(NCLP_BD_VALIDATE_ONLY)] && $::env(NCLP_BD_VALIDATE_ONLY) eq "1"} {
    puts "NCLP_BD_VALIDATE_PASS: $bd_file (no synthesis/implementation/export)"
    return
}
generate_target all [get_files $bd_file]
make_wrapper -files [get_files $bd_file] -top
add_files -norecurse [file join $proj_dir NCLP.gen sources_1 bd NCLP hdl NCLP_wrapper.v]
set_property top NCLP_wrapper [current_fileset]
update_compile_order -fileset sources_1

set_property platform.name NCLP [current_project]
set_property platform.board_id NCLP [current_project]
set_property platform.vendor user.org [current_project]
set_property platform.version 1.0 [current_project]
write_hw_platform -fixed -force [file join $proj_dir NCLP_wrapper.xsa]
puts "NCLP project created: $proj_dir/NCLP.xpr"
puts "NCLP hardware platform exported: $proj_dir/NCLP_wrapper.xsa"
