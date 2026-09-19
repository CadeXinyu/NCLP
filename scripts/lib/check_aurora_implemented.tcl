# Read-only post-route audit of the real SFP hierarchy. Source this file after
# open_run impl_1, then call check_aurora_implemented <report-directory>.
# This checker adds no timing exceptions and does not waive any findings.
namespace eval nclp_aurora_audit {
    proc require_count {label objects expected} {
        if {[llength $objects] != $expected} {
            error "$label: expected $expected objects, found [llength $objects]: $objects"
        }
    }
    proc require_period {label clocks expected tolerance} {
        require_count $label $clocks 1
        set period [get_property PERIOD $clocks]
        if {![string is double -strict $period] ||
            abs($period - $expected) > $tolerance} {
            error "$label: expected $expected ns, found $period ns"
        }
        puts "PASS: $label = $clocks ($period ns)"
    }
}

proc check_aurora_implemented {report_dir} {
    file mkdir $report_dir
    set scope NCLP_i/compute/sfp_link/cdc/inst
    set status_meta [get_cells -quiet ${scope}/aurora_status_meta_reg*]
    set status_sync [get_cells -quiet ${scope}/aurora_status_sync_reg*]
    nclp_aurora_audit::require_count "SFP status first-stage registers" $status_meta 3
    nclp_aurora_audit::require_count "SFP status second-stage registers" $status_sync 3
    foreach stage [concat $status_meta $status_sync] {
        if {![get_property ASYNC_REG $stage]} {
            error "SFP status synchronizer lost ASYNC_REG: $stage"
        }
    }
    set meta_pins [get_pins -of_objects $status_meta -filter {REF_PIN_NAME == D}]
    set ignored_status [get_timing_paths -quiet -user_ignored -delay_type max \
        -to $meta_pins -max_paths 3 -nworst 1]
    nclp_aurora_audit::require_count "SFP status first-stage asynchronous paths" \
        $ignored_status 3
    foreach path $ignored_status {
        if {![regexp -nocase {false.*path} [get_property EXCEPTION $path]]} {
            error "SFP status first stage is missing its targeted false path: $path"
        }
    }
    set local_status [get_timing_paths -quiet -delay_type max \
        -from $status_meta -to $status_sync -max_paths 3 -nworst 1]
    nclp_aurora_audit::require_count "SFP status interstage paths" $local_status 3
    foreach path $local_status {
        if {[get_property EXCEPTION $path] ne ""} {
            error "SFP status second stage must remain normally timed: $path"
        }
    }
    puts "PASS: three SFP status crossings cut only at first-stage D pins"

    set gt [get_cells -quiet -hierarchical -filter \
        {NAME =~ NCLP_i/compute/sfp_link/* && REF_NAME == GTHE4_CHANNEL}]
    nclp_aurora_audit::require_count "SFP GTHE4 channel" $gt 1
    if {[get_property LOC $gt] ne "GTHE4_CHANNEL_X0Y6"} {
        error "SFP GT channel is placed on the wrong site: [get_property LOC $gt]"
    }
    foreach {port pin} {
        sfp_rx_p T2 sfp_rx_n T1 sfp_tx_p R4 sfp_tx_n R3
        sfp_refclk_p Y6 sfp_refclk_n Y5
    } {
        set p [get_ports -quiet $port]
        nclp_aurora_audit::require_count "SFP port $port" $p 1
        if {[get_property PACKAGE_PIN $p] ne $pin} {
            error "SFP port $port must use package pin $pin"
        }
    }
    set ref_clock [get_clocks -quiet -of_objects [get_ports sfp_refclk_p]]
    set user_clock [get_clocks -quiet -of_objects [get_pins ${scope}/aurora_clk]]
    set app_clock [get_clocks -quiet -of_objects [get_pins ${scope}/sys_clk]]
    nclp_aurora_audit::require_period "SFP reference clock" $ref_clock 6.4 0.001
    nclp_aurora_audit::require_period "SFP user clock" $user_clock \
        [expr {1000.0 / 161.1328125}] 0.002
    nclp_aurora_audit::require_period "SFP application clock" $app_clock 10.0 0.001
    if {![get_property IS_GENERATED $user_clock]} {
        error "SFP user clock must be derived from the native GT clock, not a standalone fabric clock"
    }
    # Follow generated-clock ancestry, which may include a divided BUFG_GT
    # clock. The input oscillator must remain the ultimate clock source.
    set ancestor $user_clock
    for {set depth 0} {$depth < 16 && $ancestor ne $ref_clock} {incr depth} {
        set master [get_property MASTER_CLOCK $ancestor]
        if {$master eq ""} {break}
        set ancestor [get_clocks -quiet $master]
        nclp_aurora_audit::require_count "SFP clock ancestor" $ancestor 1
    }
    if {$ancestor ne $ref_clock} {
        error "SFP user-clock ancestry does not reach the constrained SFP refclk: $ancestor"
    }
    puts "PASS: SFP native GT placement and reference/user/application clocks"

    # Audit every implemented Gray pointer synchronizer in each physical FIFO.
    # A normal timing path can still be returned for an ignored crossing in
    # Vivado; explicitly inspect its effective exception and numeric bound.
    # No blanket clock group or false path may override these XPM max delays.
    set gray_report [file join $report_dir aurora_gray_timing.rpt]
    file delete -force $gray_report
    set gray_count 0
    foreach fifo {tx_fifo rx_fifo} {
        # Linking OOC checkpoints prefixes REF_NAME with NCLP_cdc_0_; the
        # original XPM module identity remains in ORIG_REF_NAME.
        set gray_cells [get_cells -quiet -hierarchical -filter \
            "NAME =~ $scope/$fifo/* && (ORIG_REF_NAME == xpm_cdc_gray || REF_NAME =~ xpm_cdc_gray*)"]
        if {[llength $gray_cells] < 2} {
            error "Physical $fifo is missing its bidirectional XPM Gray synchronizers"
        }
        foreach gray $gray_cells {
            set src [get_cells -quiet ${gray}/src_gray_ff_reg*]
            set dst {}
            foreach candidate [get_cells -quiet ${gray}/dest_graysync_ff_reg*] {
                if {[regexp {/dest_graysync_ff_reg\[0\](\[|$)} \
                        [get_property NAME $candidate]]} {
                    lappend dst $candidate
                }
            }
            if {[llength $src] == 0 || [llength $dst] == 0} {
                error "Cannot resolve XPM Gray source/first-stage endpoints in $gray"
            }
            set source_clock [get_clocks -quiet -of_objects \
                [get_pins -of_objects $src -filter {REF_PIN_NAME == C}]]
            nclp_aurora_audit::require_count "$gray source clock" $source_clock 1
            set expected_bound [get_property PERIOD $source_clock]
            set paths [get_timing_paths -quiet -delay_type max -from $src -to $dst \
                -max_paths [llength $dst] -nworst 1]
            nclp_aurora_audit::require_count "$gray constrained pointer paths" \
                $paths [llength $dst]
            foreach path $paths {
                set exception [get_property EXCEPTION $path]
                set bound [get_property REQUIREMENT $path]
                if {![regexp -nocase {max.*delay.*-datapath_only} $exception] ||
                    ![string is double -strict $bound] || $bound <= 0 ||
                    $bound > $expected_bound + 0.002} {
                    error "XPM Gray bound missing/overridden: $path; exception=$exception requirement=$bound expected<=$expected_bound"
                }
            }
            report_timing -of_objects $paths -append -file $gray_report
            incr gray_count
        }
    }
    # Keep full collision/coverage and bus-skew evidence for review. These are
    # diagnostic reports, not a blanket failure on unrelated vendor exceptions.
    report_exceptions -ignored -file [file join $report_dir exceptions_ignored.rpt]
    report_exceptions -coverage -file [file join $report_dir exceptions_coverage.rpt]
    report_bus_skew -file [file join $report_dir bus_skew.rpt]
    puts "PASS: $gray_count physical FIFO Gray synchronizers retain effective source-period max-delay bounds"
}
