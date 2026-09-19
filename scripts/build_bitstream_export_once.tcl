# Build the recreated NCLP project through bitstream generation and export an
# XSA containing that exact bitstream.
#
# Run from any directory with Vivado 2025.1:
#   vivado -mode batch -source scripts/build_bitstream_export_once.tcl

# Keep every Vivado phase within the workstation's memory-safe process limit.
# Only one run is launched at a time; each child is capped at four threads.
set_param general.maxThreads 4

set root [file normalize [file join [file dirname [info script]] ..]]
if {[info exists ::env(NCLP_VIVADO_PROJECT_DIR)] &&
    $::env(NCLP_VIVADO_PROJECT_DIR) ne ""} {
    set proj_dir [file normalize $::env(NCLP_VIVADO_PROJECT_DIR)]
} else {
    set proj_dir [file join $root vivado_project]
}
set project_file [file join $proj_dir NCLP.xpr]
set report_dir [file join $proj_dir reports]
set bit_file [file join $proj_dir NCLP.runs impl_1 NCLP_wrapper.bit]
set xsa_file [file join $proj_dir NCLP_wrapper.xsa]
set thread_limit_hook [file join $root scripts lib vivado_max_threads_4.tcl]

if {![file exists $project_file]} {
    error "Missing recreated Vivado project: $project_file"
}
if {![file exists $thread_limit_hook]} {
    error "Missing Vivado thread-limit hook: $thread_limit_hook"
}

open_project $project_file
set build_started_at [clock seconds]

if {[version -short] ne "2025.1"} {
    error "This project is qualified for Vivado 2025.1, found [version -short]"
}
if {[get_property PART [current_project]] ne "xck26-sfvc784-2LV-c"} {
    error "Unexpected project part: [get_property PART [current_project]]"
}
if {[get_property TOP [get_filesets sources_1]] ne "NCLP_wrapper"} {
    error "Unexpected synthesis top: [get_property TOP [get_filesets sources_1]]"
}

# Refuse to remove prior deliverables or launch expensive runs from a project
# that predates the clock-manager and internal-module cleanup. Recreate it with
# scripts/create_project.tcl so the source set, file types, and generated
# module-reference runs are coherent.
if {[get_property SOURCE_MGMT_MODE [current_project]] ne "All"} {
    error "Project source management is stale; rerun scripts/create_project.tcl"
}
set production_fileset [get_filesets sources_1]
foreach required_source {
    clock_drp_pkg.sv
    clock_drp_sequencer.sv
    clock_safe_startup.sv
    mmcme4_clock.sv
    plle4_clock.sv
    axil_register_slave.sv
    spi_frame_master.sv
    system_health.sv
    fan_pwm_select.sv
    nclp_compute_fabric_bd.v
    compute_fabric_control.sv
    intan_compute_path_selector.sv
    intan_frame_depacketizer.sv
    intan_sfp_packetizer.sv
    nclp_sfp_tx_arbiter.sv
    nclp_sfp_rx_router.sv
    nclp_ps_packet_mailbox.sv
    nclp_ps_packet_mailbox_bd.v
    stim_trigger_mux.sv
    spi_event_sequencer.sv
    intan_miso_lane_aligner.sv
    intan_miso_aligner_8lane.sv
    ripple_detector.sv
    ripple_detector_bd.v
    nclp_ripple_hls.v
    stim_dac_cdc.sv
    ddrw_session_config.sv
    nclp_aurora_cdc.v
} {
    set source_object [get_files -quiet -of_objects $production_fileset \
        -filter "NAME =~ */$required_source"]
    if {[llength $source_object] != 1 ||
        ![file exists [get_property NAME [lindex $source_object 0]]]} {
        error "Current production source is missing or ambiguous; recreate project: $required_source"
    }
}
foreach module_reference_source {system_health.sv fan_pwm_select.sv stim_trigger_mux.sv} {
    set source_object [get_files -quiet -of_objects $production_fileset \
        -filter "NAME =~ */$module_reference_source"]
    if {[get_property FILE_TYPE $source_object] ne "Verilog"} {
        error "IPI module-reference source must be classified as Verilog: $module_reference_source"
    }
}
foreach obsolete_source {
    stim_trigger_scheduler.sv
    stim_trigger_test.sv
    stim_trigger_test_bd.v
    clock_generator.v
    pll_clock_generator.v
    pll_filter_lookup.v
    pll_filter_balanced_lookup.v
    pll_timer_values.v
    pll_lock_lookup.v
    nclp_axil_register_slave.sv
    nclp_spi_frame_master.sv
    nclp_capture_health.v
    capture_health.sv
    nclp_compute_source_router.sv
    nclp_compute_stream_switch.sv
    compute_mode_control.sv
    intan_compute_demux.sv
    nclp_compute_selector_bd.v
    nclp_intan_stream_adapter.sv
    nclp_link_event_fifo.sv
    nclp_intan_packed_stream_adapter.sv
    compute_stream_control.sv
    intan_packed_stream_router.sv
    nclp_compute_stream_router_bd.v
    nclp_link_controller.sv
    nclp_link_controller_bd.v
    nclp_link_rx.sv
    nclp_link_tx.sv
    nclp_ps_stream_bridge.sv
    nclp_sfp_trigger_source.sv
    nclp_typed_stream_switch.sv
    nclp_typed_stream_arbiter.sv
    nclp_event_fifo.sv
    intan_miso_aligner.sv
    nclp_fan_pwm_select.v
} {
    if {[llength [get_files -quiet -of_objects $production_fileset \
            -filter "NAME =~ */$obsolete_source"]] != 0} {
        error "Compatibility-era source remains in project; recreate project: $obsolete_source"
    }
}
puts "PASS: project source set uses the current clock, compute fabric and pure SFP transport"
open_bd_design [get_files */NCLP.bd]
source [file join $root programmable_logic block_design validate_bd_NCLP.tcl]
validate_nclp_bd

# Remove previous generated deliverables so a failed rebuild cannot be mistaken
# for a current bitstream/XSA/report set.
file delete -force $bit_file
file delete -force $xsa_file
file delete -force $report_dir
file mkdir $report_dir

proc require_completed_run {run_name expected_step} {
    set run [get_runs $run_name]
    set progress [get_property PROGRESS $run]
    set status [get_property STATUS $run]
    if {$progress ne "100%" || ![string match "$expected_step Complete*" $status]} {
        error "$run_name did not complete successfully: progress=$progress status=$status"
    }
    if {[get_property NEEDS_REFRESH $run]} {
        error "$run_name completed but is marked NEEDS_REFRESH"
    }
    puts "PASS: $run_name completed: $status"
}

proc require_one {label objects} {
    set count [llength $objects]
    if {$count != 1} {
        error "$label must resolve exactly one object, found $count"
    }
    puts "PASS: $label resolves to [lindex $objects 0]"
}

proc require_same_net_segments {label lhs rhs} {
    set lhs_segments [lsort -unique [get_property NAME \
        [get_nets -quiet -segments -of_objects $lhs]]]
    set rhs_segments [lsort -unique [get_property NAME \
        [get_nets -quiet -segments -of_objects $rhs]]]

    if {[llength $lhs_segments] == 0 || $lhs_segments ne $rhs_segments} {
        error "$label topology mismatch: lhs=$lhs_segments rhs=$rhs_segments"
    }
    puts "PASS: $label uses routed net segments $lhs_segments"
}

proc require_ports_absent_from_timing_check {label ports report_text} {
    set report_lines [split $report_text "\n"]
    foreach port $ports {
        set port_name [get_property NAME $port]
        foreach report_line $report_lines {
            if {[string trim $report_line] eq $port_name} {
                error "$label is unconstrained: $port_name"
            }
        }
    }
    puts "PASS: $label all have explicit I/O timing coverage"
}

proc require_nonnegative_run_stat {run_name property_name} {
    set value [get_property $property_name [get_runs $run_name]]
    if {$value eq "" || ![string is double -strict $value]} {
        error "$run_name has no numeric $property_name result: $value"
    }
    if {$value < 0.0} {
        error "$run_name failed $property_name: $value"
    }
    puts "PASS: $run_name $property_name=$value"
}

proc require_fresh_four_thread_log {run_name log_file build_started_at} {
    if {![file exists $log_file] || [file mtime $log_file] < $build_started_at} {
        error "$run_name does not have a fresh run log: $log_file"
    }
    set handle [open $log_file r]
    set contents [read $handle]
    close $handle

    set saw_thread_report 0
    foreach line [split $contents "\n"] {
        set count ""
        if {[regexp -nocase {maximum of ([0-9]+) (?:processes|CPUs)} \
                $line -> count] ||
            [regexp -nocase {with ([0-9]+) threads} \
                $line -> count]} {
            set saw_thread_report 1
            if {$count > 4} {
                error "$run_name exceeded four threads: $line"
            }
        }
    }
    if {!$saw_thread_report} {
        error "$run_name log did not report its thread/process limit"
    }
    puts "PASS: $run_name log reports no phase above four threads"
}

proc require_no_critical_cdc {report_file} {
    if {![file exists $report_file]} {
        error "Missing CDC report: $report_file"
    }
    set handle [open $report_file r]
    set contents [read $handle]
    close $handle

    set violations {}
    foreach line [split $contents "\n"] {
        if {[regexp {(^|[[:space:]])Critical([[:space:]]|$)} $line]} {
            lappend violations [string trim $line]
        }
    }
    if {[llength $violations] != 0} {
        error "Critical CDC findings remain:\n[join $violations \n]"
    }
    puts "PASS: complete design has no Critical CDC findings"
}

# A newly recreated project has not created its BD OOC runs yet. Materialize
# them before enumerating/resetting runs and attaching child thread-limit hooks;
# otherwise launch_runs creates them later and they miss those explicit hooks.
create_ip_run [get_files */NCLP.bd]
# The generated fixed-point HLS RTL is synthesized inside the ripple module's
# OOC run under the same thread hook. Optional generated XCI also stays there.
# Inline BD utility IP also has no independent synthesis run.
foreach required_ooc_run {
    NCLP_compute_fabric_0_synth_1
    NCLP_ripple_detector_0_synth_1
    NCLP_trigger_mux_0_synth_1
    NCLP_sfp_mailbox_0_synth_1 NCLP_core_0_synth_1 NCLP_cdc_0_synth_1
} {
    if {[llength [get_runs -quiet $required_ooc_run]] != 1} {
        error "BD OOC synthesis run was not created: $required_ooc_run"
    }
}
set synthesis_runs [concat [get_runs synth_1] [get_runs -quiet *_synth_1]]
puts "PASS: [llength $synthesis_runs] synthesis runs will receive the four-thread hook"

# Always rebuild from synthesis so the four-thread limit applies to every
# phase, even when an older GUI run exists in the recreated project.  Reset
# unconditionally so an interrupted process cannot leave a stale "running"
# status at 0% that prevents relaunch.
reset_run impl_1
foreach synthesis_run $synthesis_runs {
    reset_run $synthesis_run
}

# A recreated GUI project may remember an automatically imported incremental
# checkpoint from an older build.  Do not let a release build reuse that
# netlist: every source file must be synthesized again under this run's
# four-thread cap.
set_property AUTO_INCREMENTAL_CHECKPOINT 0 [get_runs synth_1]
set_property INCREMENTAL_CHECKPOINT {} [get_runs synth_1]
if {[get_property AUTO_INCREMENTAL_CHECKPOINT [get_runs synth_1]] ||
    [get_property INCREMENTAL_CHECKPOINT [get_runs synth_1]] ne ""} {
    error "Top synthesis still has an incremental checkpoint configured"
}
puts "PASS: top synthesis incremental-checkpoint reuse is disabled"

# A PRE hook is required because launch_runs starts child Vivado processes;
# the parent session's general.maxThreads value is not inherited reliably.
# Set these properties after reset_run, which clears generated run state and
# can otherwise discard step hooks. Apply the hook to every OOC dependency.
# Implementation receives it before init_design because that step already
# launches threaded DRC work; setting it only before opt_design is too late.
foreach synthesis_run $synthesis_runs {
    set_property STEPS.SYNTH_DESIGN.TCL.PRE $thread_limit_hook $synthesis_run
}
set_property STEPS.INIT_DESIGN.TCL.PRE $thread_limit_hook [get_runs impl_1]

# Only one run is launched at a time; that child may use at most four threads.
# This prevents four OOC runs from each allocating four worker threads.
launch_runs synth_1 -jobs 1
wait_on_run synth_1
require_completed_run synth_1 synth_design

# Verify the generated child script did not quietly restore/import an
# incremental seed checkpoint.
set top_synth_tcl [file join $proj_dir NCLP.runs synth_1 NCLP_wrapper.tcl]
if {![file exists $top_synth_tcl]} {
    error "Missing generated top-synthesis Tcl: $top_synth_tcl"
}
set top_synth_handle [open $top_synth_tcl r]
set top_synth_contents [read $top_synth_handle]
close $top_synth_handle
if {[regexp -nocase {read_checkpoint[^\n]*incremental} $top_synth_contents]} {
    error "Top synthesis imported an incremental checkpoint"
}
puts "PASS: top synthesis ran without an incremental checkpoint"

launch_runs impl_1 -to_step route_design -jobs 1
wait_on_run impl_1
require_completed_run impl_1 route_design

foreach synthesis_run $synthesis_runs {
    set run_name [get_property NAME $synthesis_run]
    require_fresh_four_thread_log $run_name \
        [file join $proj_dir NCLP.runs $run_name runme.log] \
        $build_started_at
}
require_fresh_four_thread_log impl_1 \
    [file join $proj_dir NCLP.runs impl_1 runme.log] \
    $build_started_at

# Vivado 2025.1 does not expose pulse-width slack as an impl-run STATS.WPWS
# property.  Pulse-width timing remains covered by timing_summary.rpt below;
# do not make an otherwise successful bitstream build fail on a nonexistent
# run property.

open_run impl_1

# Save the complete diagnosis first, including when a gate below rejects the
# routed design. Timing/CDC failures must not leave an empty reports directory.
report_timing_summary -delay_type min_max -report_unconstrained \
    -check_timing_verbose -max_paths 20 \
    -file [file join $report_dir timing_summary.rpt]
report_route_status -file [file join $report_dir route_status.rpt]
report_drc -file [file join $report_dir drc.rpt]
report_methodology -file [file join $report_dir methodology.rpt]
set cdc_report_file [file join $report_dir cdc.rpt]
report_cdc -details -file $cdc_report_file
report_clock_interaction -delay_type min_max \
    -file [file join $report_dir clock_interaction.rpt]
report_clock_utilization -file [file join $report_dir clock_utilization.rpt]
report_io -file [file join $report_dir io.rpt]
check_timing -verbose -file [file join $report_dir check_timing.rpt]
report_bus_skew -file [file join $report_dir bus_skew.rpt]
report_exceptions -ignored -file [file join $report_dir exceptions_ignored.rpt]
report_exceptions -coverage -file [file join $report_dir exceptions_coverage.rpt]

require_nonnegative_run_stat impl_1 STATS.WNS
require_nonnegative_run_stat impl_1 STATS.WHS

# This release flow audits the routed checkpoint. A later physical-optimization
# step would change that netlist before write_bitstream and invalidate the audit.
# Fresh projects default to disabled; never silently change a selected strategy.
if {[get_property STEPS.POST_ROUTE_PHYS_OPT_DESIGN.IS_ENABLED [get_runs impl_1]]} {
    error "Post-route physical optimization is enabled; this route-checkpoint audit requires it disabled"
}

# These are stable RTL/module-reference or clock-buffer pins. Checking them
# after linking makes hierarchy drift fail loudly while keeping the XDC itself
# clean.
set axi_clock_pin [get_pins -quiet {NCLP_i/capture/intan_spi_module/s00_axi_aclk}]
set spi_clock_pin [get_pins -quiet -hierarchical -filter \
    {NAME =~ */spi_clkgen/u_output_buffer/u_output_bufg/O}]
set dac_clock_pin [get_pins -quiet -hierarchical -filter \
    {NAME =~ */stim_controller/inst/impl/g_real_clock.dac_clkgen/u_output_buffer/u_output_bufg/O}]
require_one "AXI clock anchor pin" $axi_clock_pin
require_one "SPI clock anchor pin" $spi_clock_pin
require_one "DAC clock anchor pin" $dac_clock_pin
require_one "AXI clock object" [get_clocks -quiet -of_objects $axi_clock_pin]
require_one "SPI clock object" [get_clocks -quiet -of_objects $spi_clock_pin]
require_one "DAC clock object" [get_clocks -quiet -of_objects $dac_clock_pin]

# The source requests MMCME4 AUTO compensation.  In the complete block design
# CLKIN1 is driven by the upstream clock-wizard BUFG, so Vivado must resolve
# AUTO to BUF_IN.  The direct-input OOC topology intentionally resolves to
# ZHOLD instead.  Make this legal, topology-dependent transformation explicit
# so a future clock-tree edit cannot silently change the implemented phase
# contract.
set intan_runtime_mmcm [get_cells -quiet -hierarchical -filter \
    {NAME =~ */spi_clkgen/u_mmcm && REF_NAME == MMCME4_ADV}]
require_one "Intan runtime MMCME4" $intan_runtime_mmcm
set intan_compensation [get_property COMPENSATION $intan_runtime_mmcm]
if {$intan_compensation ne "BUF_IN"} {
    error "Integrated Intan MMCME4 AUTO compensation did not resolve to BUF_IN: $intan_compensation"
}

# Resolve the deliberate RTL instances instead of counting every BUFG-class
# sink on CLKOUT0. Vivado inserts a second, constant-enabled BUFGCE for the
# small raw-clock qualification pipeline inside clock_safe_startup; the named
# u_output_bufg is the separately gated buffer that drives the SPI domain.
set intan_clock_scope [file dirname [get_property NAME $intan_runtime_mmcm]]
set intan_clkin_segments [get_nets -quiet -segments -of_objects \
    [get_pins $intan_runtime_mmcm/CLKIN1]]
set intan_upstream_bufg [get_cells -quiet -of_objects \
    [get_pins -quiet -of_objects $intan_clkin_segments \
        -filter {DIRECTION == OUT}] \
    -filter {REF_NAME =~ BUFG*}]
require_one "BUFG driver of Intan CLKIN1" $intan_upstream_bufg
if {[get_property REF_NAME $intan_upstream_bufg] ne "BUFGCE"} {
    error "Expected BUFGCE before Intan CLKIN1, got [get_property REF_NAME $intan_upstream_bufg]"
}

set intan_feedback_bufg [get_cells -quiet \
    ${intan_clock_scope}/u_feedback_bufg]
require_one "Intan feedback BUFG" $intan_feedback_bufg

set intan_output_bufg [get_cells -quiet \
    ${intan_clock_scope}/u_output_buffer/u_output_bufg]
require_one "Intan CLKOUT0 BUFG" $intan_output_bufg

foreach {label cell} [list feedback $intan_feedback_bufg output $intan_output_bufg] {
    if {[get_property REF_NAME $cell] ne "BUFGCE"} {
        error "Expected Intan $label buffer to implement as BUFGCE, got [get_property REF_NAME $cell]"
    }
}

require_same_net_segments "Intan CLKFBOUT to feedback BUFG" \
    [get_pins $intan_runtime_mmcm/CLKFBOUT] \
    [get_pins $intan_feedback_bufg/I]
require_same_net_segments "Intan feedback BUFG to CLKFBIN" \
    [get_pins $intan_feedback_bufg/O] \
    [get_pins $intan_runtime_mmcm/CLKFBIN]
require_same_net_segments "Intan CLKOUT0 to gated output BUFG" \
    [get_pins $intan_runtime_mmcm/CLKOUT0] \
    [get_pins $intan_output_bufg/I]

set intan_feedback_root [get_property -quiet CLOCK_ROOT \
    [get_nets -quiet -of_objects [get_pins $intan_feedback_bufg/O]]]
set intan_output_root [get_property -quiet CLOCK_ROOT \
    [get_nets -quiet -of_objects [get_pins $intan_output_bufg/O]]]
if {$intan_feedback_root eq "" || $intan_output_root eq "" ||
    $intan_feedback_root ne $intan_output_root} {
    error "Intan feedback/output clock roots do not match: feedback=$intan_feedback_root output=$intan_output_root"
}
puts "PASS: integrated Intan MMCME4 AUTO resolved to $intan_compensation; upstream BUFGCE and feedback/output root $intan_feedback_root verified"

set backbone_nets [get_nets -quiet -hierarchical -filter \
    {CLOCK_DEDICATED_ROUTE == BACKBONE}]
if {[llength $backbone_nets] != 0} {
    error "Unexpected CLOCK_DEDICATED_ROUTE=BACKBONE nets: $backbone_nets"
}
puts "PASS: no CLOCK_DEDICATED_ROUTE=BACKBONE override is active"

set axi_clock [get_clocks -quiet -of_objects $axi_clock_pin]
set spi_clock [get_clocks -quiet -of_objects $spi_clock_pin]
set dac_clock [get_clocks -quiet -of_objects $dac_clock_pin]
set axi_period [get_property PERIOD $axi_clock]
set spi_period [get_property PERIOD $spi_clock]
set dac_period [get_property PERIOD $dac_clock]
if {$axi_period < 9.99 || $axi_period > 10.01} {
    error "Unexpected AXI-clock period: $axi_period ns"
}
if {$spi_period < 11.8 || $spi_period > 12.0} {
    error "Unexpected default SPI-clock period: $spi_period ns"
}
if {$dac_period < 24.99 || $dac_period > 25.01} {
    error "Unexpected default DAC-engine clock period: $dac_period ns"
}
puts "PASS: linked AXI/SPI/DAC clock periods are $axi_period ns / $spi_period ns / $dac_period ns"

# The AXI clock and both runtime-programmable peripheral clocks have no fixed
# phase relation.
# Vivado 2025.1 still returns ignored paths from get_timing_paths, so an empty
# collection is not a valid clock-group audit.  Require a real CDC path in each
# direction and prove that both the path exception and the clock-interaction
# classification are the asynchronous group from 00_clock.xdc.
proc require_asynchronous_clock_pair {label from_clock to_clock} {
    set representative_path [lindex [get_timing_paths -quiet -user_ignored \
        -from $from_clock -to $to_clock -max_paths 1] 0]
    if {$representative_path eq ""} {
        error "$label has no representative CDC path to audit"
    }

    set path_exception [get_property EXCEPTION $representative_path]
    if {$path_exception ne "Asynchronous Clock Groups"} {
        error "$label is not cut by an asynchronous clock group: path=$representative_path exception=$path_exception"
    }

    set interaction [report_clock_interaction -quiet \
        -from $from_clock -to $to_clock -return_string]
    if {![regexp {Ignored[[:space:]]+Asynchronous Groups} $interaction]} {
        error "$label clock interaction is not classified as an asynchronous group"
    }
    puts "PASS: $label representative path is ignored by Asynchronous Clock Groups"
}

require_asynchronous_clock_pair "AXI->SPI" $axi_clock $spi_clock
require_asynchronous_clock_pair "SPI->AXI" $spi_clock $axi_clock
require_asynchronous_clock_pair "AXI->DAC" $axi_clock $dac_clock
require_asynchronous_clock_pair "DAC->AXI" $dac_clock $axi_clock

# A successful WNS/WHS alone does not prove that package I/O has an external
# timing model. Fail the release build if any critical MISO, SCLK, MOSI, or CS
# port appears in Vivado's no-delay checks.
set critical_spi_inputs [get_ports -quiet {
    miso_a1 miso_a2 miso_b1 miso_b2
    miso_c1 miso_c2 miso_d1 miso_d2
}]
set critical_spi_outputs [get_ports -quiet {
    sclk cs mosi_a mosi_b mosi_c mosi_d
}]
if {[llength $critical_spi_inputs] != 8} {
    error "Expected 8 critical SPI input ports, found [llength $critical_spi_inputs]"
}
if {[llength $critical_spi_outputs] != 6} {
    error "Expected 6 critical SPI output ports, found [llength $critical_spi_outputs]"
}
set no_input_delay_report [check_timing -verbose \
    -override_defaults no_input_delay -return_string]
set no_output_delay_report [check_timing -verbose \
    -override_defaults no_output_delay -return_string]
set partial_input_delay_report [check_timing -verbose \
    -override_defaults partial_input_delay -return_string]
set partial_output_delay_report [check_timing -verbose \
    -override_defaults partial_output_delay -return_string]
require_ports_absent_from_timing_check "critical SPI inputs" \
    $critical_spi_inputs $no_input_delay_report
require_ports_absent_from_timing_check "critical SPI outputs" \
    $critical_spi_outputs $no_output_delay_report
require_ports_absent_from_timing_check "critical SPI inputs with partial delays" \
    $critical_spi_inputs $partial_input_delay_report
require_ports_absent_from_timing_check "critical SPI outputs with partial delays" \
    $critical_spi_outputs $partial_output_delay_report

# A data waveform on an output port must not be modeled as a generated clock
# unless Vivado can propagate real clock edges to it. Reject both the timing
# engine's disconnected-generated-clock diagnostic and methodology TIMING-36.
set sclk_port_clocks [get_clocks -quiet -of_objects [get_ports sclk]]
if {[llength $sclk_port_clocks] != 0} {
    error "SCLK is registered data and must not own a clock object: $sclk_port_clocks"
}
set invalid_generated_clock_report [check_timing -verbose \
    -override_defaults generated_clocks -return_string]
if {[regexp -nocase \
        {There (is|are) [1-9][0-9]* generated clock.*not connected} \
        $invalid_generated_clock_report]} {
    error "Invalid generated clock detected:\n$invalid_generated_clock_report"
}
report_methodology -quiet -checks {TIMING-36} -return_string
set timing_36_violations [get_methodology_violations -quiet \
    -filter {CHECK == TIMING-36}]
if {[llength $timing_36_violations] != 0} {
    error "TIMING-36 invalid generated-clock violations: $timing_36_violations"
}
puts "PASS: no invalid generated clocks or TIMING-36 violations"

# Reject combinational asynchronous resets, except the three reviewed AMD GT
# reset ORs whose complete source and five-stage release topology is checked.
# These vendor warnings remain visible; no waiver or severity change is made.
source [file join $root scripts lib check_aurora_vendor_reset.tcl]
check_aurora_vendor_reset $report_dir

# A bitstream is releasable only when the complete implemented design is free
# of Critical CDC findings; hierarchy-scoped signoff can hide replicated CDC
# fanout elsewhere in the design.
require_no_critical_cdc $cdc_report_file
source [file join $root scripts lib check_aurora_implemented.tcl]
check_aurora_implemented $report_dir

source [file join $root scripts lib check_implementation_reports.tcl]
check_implementation_report_gates $report_dir
report_drc -ruledecks bitstream_checks -file [file join $report_dir bitstream_drc.rpt]
set blocking_drc [get_drc_violations -quiet -filter {SEVERITY == "Error" || SEVERITY == "Critical Warning"}]
if {[llength $blocking_drc] != 0} {error "Bitstream DRC findings remain: $blocking_drc"}

close_design
launch_runs impl_1 -to_step write_bitstream -jobs 1
wait_on_run impl_1
require_completed_run impl_1 write_bitstream
if {![file exists $bit_file] || [file size $bit_file] == 0 ||
    [file mtime $bit_file] < $build_started_at} {
    error "Bitstream was not generated: $bit_file"
}
puts "PASS: bitstream generated: $bit_file"
open_run impl_1

# Export only after write_bitstream so software receives the same PL image that
# was audited above, rather than a structural XSA without a bitstream.
write_hw_platform -fixed -include_bit -force $xsa_file
if {![file exists $xsa_file] || [file size $xsa_file] == 0 ||
    [file mtime $xsa_file] < $build_started_at} {
    error "Hardware platform was not exported: $xsa_file"
}

puts "PASS: four-thread-capped bitstream build and audit reports completed"
puts "PASS: hardware platform with bitstream exported: $xsa_file"
close_project
