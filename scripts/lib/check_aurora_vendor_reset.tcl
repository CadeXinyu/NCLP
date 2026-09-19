# Classify exactly three reviewed AMD GT Wizard reset-OR methodology warnings.
# This is a project acceptance decision, not an AMD-published waiver. It neither
# changes severity nor creates waivers, modifies the netlist, or cuts timing.
# Basis: Vivado 2025.1 gtwizard_ultrascale_v1_7_20_gtwiz_reset/reset_sync RTL,
# PG182 Reset Controller Helper Block Ports, and docs/sfp-hardware.md.
namespace eval nclp_vendor_reset {
    proc one {label objects} {
        if {[llength $objects] != 1} {
            error "$label: expected one object, found [llength $objects]: $objects"
        }
        return [lindex $objects 0]
    }
    proc cell {name} {
        return [one $name [get_cells -quiet $name]]
    }
    proc pin {name ref_pin} {
        return [one "$name/$ref_pin" [get_pins -quiet -of_objects [cell $name] \
            -filter "REF_PIN_NAME == $ref_pin"]]
    }
    proc driver {input_pin} {
        return [one "Driver of $input_pin" [get_pins -quiet -leaf \
            -of_objects [get_nets -quiet -of_objects $input_pin] \
            -filter {DIRECTION == OUT}]]
    }
    proc constant {input_pin expected_ref} {
        set source [driver $input_pin]
        set source_cell [one "Constant source of $input_pin" \
            [get_cells -quiet -of_objects $source]]
        if {[get_property REF_NAME $source_cell] ne $expected_ref} {
            error "$input_pin must be driven by $expected_ref, found $source"
        }
    }
    proc same_driver {input_pin expected} {
        set actual [driver $input_pin]
        if {$actual ne $expected} {
            error "Unexpected connection at $input_pin: $actual; expected $expected"
        }
    }
    proc clock {name expected} {
        set actual [one "Clock of $name" [get_clocks -quiet -of_objects [pin $name C]]]
        if {$actual ne $expected} {error "$name uses $actual instead of $expected"}
    }
    proc pure_or {name width expected_drivers evidence} {
        set obj [cell $name]
        set expected_init [expr {$width == 4 ? "16'hFFFE" : "4'hE"}]
        if {[get_property REF_NAME $obj] ne "LUT$width" ||
            ![string equal -nocase [get_property INIT $obj] $expected_init]} {
            error "$name no longer implements the reviewed positive reset OR"
        }
        set actual {}
        foreach input_pin [get_pins -quiet -of_objects $obj -filter {DIRECTION == IN}] {
            lappend actual [driver $input_pin]
        }
        if {[lsort $actual] ne [lsort $expected_drivers]} {
            error "$name reset sources changed: $actual; expected $expected_drivers"
        }
        puts $evidence "OR $name [get_property REF_NAME $obj] INIT=[get_property INIT $obj]"
        foreach source $actual {puts $evidence "  INPUT $source"}
    }
    proc reset_chain {scope lut clock_name evidence} {
        set expected_loads {}
        set previous {}
        set index 0
        foreach name {rst_in_meta_reg rst_in_sync1_reg rst_in_sync2_reg rst_in_sync3_reg rst_in_out_reg} {
            set reg $scope/$name
            set obj [cell $reg]
            if {[get_property REF_NAME $obj] ne "FDPE" ||
                [get_property INIT $obj] ne "1'b0"} {
                error "$reg no longer matches the vendor asynchronous-set reset stage"
            }
            if {$index < 4 && ![get_property ASYNC_REG $obj]} {
                error "$reg lost ASYNC_REG"
            }
            clock $reg $clock_name
            constant [pin $reg CE] VCC
            same_driver [pin $reg PRE] [pin $lut O]
            lappend expected_loads [pin $reg PRE]
            if {$index == 0} {
                constant [pin $reg D] GND
            } else {
                same_driver [pin $reg D] [pin $previous Q]
            }
            puts $evidence "  STAGE $reg FDPE INIT=0 ASYNC_REG=[get_property ASYNC_REG $obj] CLOCK=$clock_name"
            set previous $reg
            incr index
        }
        set loads [get_pins -quiet -leaf -of_objects \
            [get_nets -quiet -of_objects [pin $lut O]] -filter {DIRECTION == IN}]
        if {[lsort $loads] ne [lsort $expected_loads]} {
            error "$lut drives additional or different asynchronous endpoints: $loads"
        }
    }
}

proc check_aurora_vendor_reset {report_dir} {
    file mkdir $report_dir
    # Keep these warnings visible in the report and Vivado's violation objects.
    report_methodology -quiet -checks {LUTAR-1} \
        -file [file join $report_dir lutar_methodology.rpt]
    set violations [get_methodology_violations -quiet -filter {CHECK == LUTAR-1}]
    set evidence [open [file join $report_dir aurora_vendor_reset_review.rpt] w]
    try {
        puts $evidence "Project-reviewed AMD reset OR warnings; no waiver or severity change."
        puts $evidence "Tool: [version -short]; vendor implementation: gtwizard_ultrascale_v1_7_20."
        puts $evidence "Basis: docs/sfp-hardware.md; PG182 Reset Controller Helper Block Ports."
        if {[version -short] ne "2025.1"} {error "Vendor reset review is qualified only for Vivado 2025.1"}
        set core NCLP_i/compute/sfp_link/core/inst
        set wrapper $core/NCLP_core_0_core_i/NCLP_core_0_wrapper_i
        set controller $wrapper/NCLP_core_0_multi_gt_i/NCLP_core_0_gt_i/inst/gen_gtwizard_gthe4_top.NCLP_core_0_gt_gtwizard_gthe4_inst/gen_gtwizard_gthe4.gen_reset_controller_internal.gen_single_instance.gtwiz_reset_inst
        set init_clock [nclp_vendor_reset::one "Aurora init clock" \
            [get_clocks -quiet -of_objects [get_pins NCLP_i/compute/sfp_link/cdc/inst/sys_clk]]]
        if {abs([get_property PERIOD $init_clock] - 10.0) > 0.001} {
            error "Vendor reset review expects the 100 MHz init clock"
        }
        # These six sources are already registered in the vendor free-running
        # init clock domain. No user combinational reset drives these OR gates.
        set pma_reg $core/support_reset_logic_i/dly_gt_rst_r_reg\[18\]
        set dp_reg $controller/gtwiz_reset_rx_datapath_int_reg
        set pll_reg $controller/gtwiz_reset_rx_pll_and_datapath_int_reg
        set rxfsm_or $wrapper/cbcc_gtx0_i/rxfsm_reset_i_inferred_i_1
        set rxfsm_regs [list $pma_reg $wrapper/hard_err_rst_int_reg \
            $wrapper/cbcc_gtx0_i/LINK_RESET_reg\[0\] $wrapper/cdr_reset_fsm_lnkreset_reg]
        foreach reg [concat $rxfsm_regs [list $dp_reg $pll_reg]] {
            if {[get_property REF_NAME [nclp_vendor_reset::cell $reg]] ne "FDRE"} {
                error "Vendor reset source must remain a synchronous FDRE: $reg"
            }
            nclp_vendor_reset::clock $reg $init_clock
            puts $evidence "REGISTERED_SOURCE $reg CLOCK=$init_clock"
        }
        set rxfsm_sources {}
        foreach reg $rxfsm_regs {lappend rxfsm_sources [nclp_vendor_reset::pin $reg Q]}
        nclp_vendor_reset::pure_or $rxfsm_or 4 $rxfsm_sources $evidence

        set accepted {}
        foreach {suffix lut_name width source_names} [list \
            rx_any rst_in_meta_i_1 4 [list $dp_reg $rxfsm_or $pma_reg $pll_reg] \
            rx_datapath rst_in_meta_i_1__1 2 [list $rxfsm_or $dp_reg] \
            rx_pll_and_datapath rst_in_meta_i_1__0 2 [list $pll_reg $pma_reg]] {
            set scope $controller/reset_synchronizer_gtwiz_reset_${suffix}_inst
            set lut $scope/$lut_name
            set sources {}
            foreach source $source_names {
                set output [expr {$source eq $rxfsm_or ? "O" : "Q"}]
                lappend sources [nclp_vendor_reset::pin $source $output]
            }
            nclp_vendor_reset::pure_or $lut $width $sources $evidence
            nclp_vendor_reset::reset_chain $scope $lut $init_clock $evidence
            lappend accepted $lut
        }
        set observed {}
        foreach violation $violations {
            set lut [nclp_vendor_reset::one "LUT in $violation" \
                [get_cells -quiet -of_objects $violation -filter {REF_NAME =~ LUT*}]]
            if {[lsearch -exact $accepted $lut] < 0} {
                error "Unreviewed LUTAR-1 remains fatal: $violation ($lut)"
            }
            puts $evidence "ACCEPTED_WARNING $violation $lut"
            lappend observed $lut
        }
        if {[lsort $observed] ne [lsort $accepted]} {
            error "Expected exactly the three reviewed vendor reset warnings; found $observed"
        }
        puts $evidence "PASS: exactly 3 reviewed vendor LUTAR-1 warnings; all other LUTAR-1 findings prohibited."
        puts "PASS: exactly 3 structurally checked vendor reset-OR warnings accepted; no other LUTAR-1 findings"
    } finally {
        close $evidence
    }
}
