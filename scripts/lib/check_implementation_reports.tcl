# Release gates over reports emitted before any post-route assertions fail.
# Report formats and route-status flags are verified against Vivado 2025.1.
namespace eval nclp_impl_audit {
    proc read_report {path} {
        if {![file exists $path] || [file size $path] == 0} {
            error "Missing implementation report: $path"
        }
        set handle [open $path r]
        set text [read $handle]
        close $handle
        return $text
    }

    proc check_internal_coverage {text} {
        set seen [dict create no_clock 0 unconstrained_internal_endpoints 0]
        foreach line [split $text "\n"] {
            if {![regexp {^[ \t]*[0-9]+[.] checking ([a-z_]+) [(]([0-9]+)[)]} \
                    $line -> check count] || ![dict exists $seen $check]} {continue}
            dict set seen $check 1
            if {$count != 0} {
                error "Internal timing coverage failed: $check=$count; see check_timing.rpt"
            }
        }
        dict for {check found} $seen {
            if {!$found} {error "Missing check_timing result: $check"}
        }
        puts "PASS: no unclocked registers or unconstrained internal endpoints"
    }

    proc check_bus_skew {text} {
        # The full report emits the worst case for every constraint. Inspect
        # both the summary numbers and VIOLATED markers (including -0.000).
        if {[regexp -nocase {Slack[ \t]*[(]VIOLATED[)]} $text]} {
            error "Bus-skew violation remains; see bus_skew.rpt"
        }
        # Long endpoint lists wrap the summary row over many lines. The
        # per-constraint section has a stable Id / Slack pair for both short
        # and wrapped summaries, so require exactly one result for each Id.
        set rows 0
        set pending ""
        set seen_ids {}
        foreach line [split $text "\n"] {
            if {[regexp {^[ \t]*Id:[ \t]+([0-9]+)[ \t]*$} $line -> id]} {
                if {$pending ne "" || $id in $seen_ids} {
                    error "Missing or duplicated bus-skew result: $line"
                }
                set pending $id
                lappend seen_ids $id
            }
            if {[regexp {^[ \t]*Slack[ \t]+[(](MET|VIOLATED)[)][ \t]*:[ \t]*([-+]?[0-9]+[.]?[0-9]*)ns} \
                    $line -> status slack]} {
                if {$pending eq ""} {error "Bus-skew result without a constraint Id: $line"}
                incr rows
                if {$status ne "MET" || $slack < 0.0} {
                    error "Bus-skew violation remains: $line"
                }
                set pending ""
            }
        }
        if {$pending ne ""} {error "Missing bus-skew result for constraint $pending"}
        if {$rows == 0} {
            error "No bus-skew constraint results found; physical FIFO Gray bounds must be present"
        }
        puts "PASS: all $rows reported bus-skew constraints meet timing"
    }

    proc check_timing_summary {text} {
        set header_seen 0
        foreach line [split $text "\n"] {
            if {[string first "WNS(ns)" $line] >= 0 &&
                [string first "WPWS(ns)" $line] >= 0} {
                set header_seen 1
                continue
            }
            if {!$header_seen} {continue}
            set fields [regexp -all -inline {[-+]?[0-9]+(?:\.[0-9]+)?} $line]
            if {[llength $fields] != 12} {continue}
            foreach index {0 4 8} {
                if {[lindex $fields $index] <= 0.0} {
                    error "Timing margin must be positive: $line"
                }
            }
            foreach index {1 2 5 6 9 10} {
                if {[lindex $fields $index] != 0.0} {
                    error "Timing failures remain: $line"
                }
            }
            puts "PASS: setup, hold and pulse-width timing with zero failing endpoints: $line"
            return
        }
        error "Could not verify full timing-summary table"
    }
}

proc check_implementation_report_gates {report_dir} {
    # Native boolean flags avoid interpreting route-summary prose and include
    # incomplete routes, unplaced logic and route errors.
    foreach {flag expected} {PLACED_FULLY 1 ROUTED_FULLY 1 ERRORS_IN_ROUTES 0} {
        set value [report_route_status -boolean_check $flag]
        if {![string is boolean -strict $value] || $value != $expected} {
            error "Implementation route gate failed: $flag=$value; see route_status.rpt"
        }
    }
    puts "PASS: complete placement and routing with no routing errors"
    nclp_impl_audit::check_internal_coverage \
        [nclp_impl_audit::read_report [file join $report_dir check_timing.rpt]]
    nclp_impl_audit::check_bus_skew \
        [nclp_impl_audit::read_report [file join $report_dir bus_skew.rpt]]
    nclp_impl_audit::check_timing_summary \
        [nclp_impl_audit::read_report [file join $report_dir timing_summary.rpt]]
}
