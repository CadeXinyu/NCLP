# Validate the complete live NCLP block design after create_bd_NCLP.tcl.
# Endpoint connectivity is checked instead of generated net names, which change
# when Vivado regenerates hierarchy.
proc nclp_connected_net_segments {kind endpoint} {
    if {$kind eq "interface"} {
        set net_command get_bd_intf_nets
        set pin_command get_bd_intf_pins
    } else {
        set net_command get_bd_nets
        set pin_command get_bd_pins
    }
    set queue [$net_command -quiet -boundary_type both -of_objects $endpoint]
    set seen [dict create]
    while {[llength $queue] > 0} {
        set net [lindex $queue 0]
        set queue [lrange $queue 1 end]
        if {[dict exists $seen $net]} {continue}
        dict set seen $net 1
        set pins [$pin_command -quiet -of_objects [$net_command $net]]
        foreach pin $pins {
            foreach neighbor [$net_command -quiet -boundary_type both -of_objects [$pin_command $pin]] {
                if {![dict exists $seen $neighbor]} {lappend queue $neighbor}
            }
        }
    }
    return [lsort [dict keys $seen]]
}

proc nclp_assert_shared_net {kind endpoints} {
    set first_net ""
    foreach endpoint $endpoints {
        if {$kind eq "interface"} {
            set pin [get_bd_intf_pins -quiet $endpoint]
            set net [nclp_connected_net_segments interface $pin]
        } else {
            set pin [get_bd_pins -quiet $endpoint]
            if {[llength $pin] == 0} {set pin [get_bd_ports -quiet $endpoint]}
            set net [nclp_connected_net_segments signal $pin]
        }
        if {[llength $pin] != 1 || [llength $net] == 0} {
            error "NCLP integration: missing/unconnected $kind endpoint $endpoint"
        }
        if {$first_net eq ""} {set first_net $net}
        if {$net ne $first_net} {
            error "NCLP integration: $endpoints do not share one connected $kind net across hierarchy"
        }
    }
}

proc nclp_assert_bus_width {endpoint expected_left} {
    set pin [get_bd_pins -quiet $endpoint]
    if {[llength $pin] != 1 || [get_property LEFT $pin] != $expected_left ||
        [get_property RIGHT $pin] != 0} {
        error "NCLP integration: $endpoint must be [expr {$expected_left + 1}] bits"
    }
}

proc validate_nclp_bd {} {
    set expected_boxes {capture compute platform stimulation}
    set actual_boxes {}
    foreach cell [get_bd_cells] {
        if {[get_property TYPE $cell] ne "hier"} {
            error "NCLP hierarchy: root leaf $cell must be inside a functional box"
        }
        lappend actual_boxes [file tail $cell]
    }
    if {[lsort $actual_boxes] ne $expected_boxes} {
        error "NCLP hierarchy: expected $expected_boxes, found $actual_boxes"
    }

    foreach {subsystem expected_interfaces} {
        capture {M_AXIS_COMPUTE M_AXI_DDR S_AXI_CTRL}
        compute {S_AXIS_INTAN_RAW_FRAME S_AXI_CTRL}
        stimulation {S_AXI_CTRL}
        platform {M_AXI_CAPTURE M_AXI_COMPUTE M_AXI_STIMULATION S_AXI_RECORDING_DDR}
    } {
        set interfaces {}
        foreach pin [get_bd_intf_pins -of_objects [get_bd_cells $subsystem]] {
            lappend interfaces [file tail $pin]
        }
        if {[lsort $interfaces] ne [lsort $expected_interfaces]} {
            error "NCLP boundary: $subsystem interface names differ: $interfaces"
        }
        foreach pin_name {sys_clk sys_resetn} {
            if {[llength [get_bd_pins -quiet $subsystem/$pin_name]] != 1} {
                error "NCLP boundary: missing $subsystem/$pin_name"
            }
        }
    }

    set compute_children {}
    foreach child [get_bd_cells -quiet -hierarchical] {
        if {[string trimleft [file dirname $child] /] eq "compute"} {
            lappend compute_children [file tail $child]
        }
    }
    set expected_compute_children {compute_fabric control_interconnect sfp_link sfp_mailbox ripple_detector trigger_mux}
    if {[lsort $compute_children] ne [lsort $expected_compute_children]} {
        error "NCLP compute boundary: expected $expected_compute_children, found $compute_children"
    }
    set sfp_children {}
    foreach child [get_bd_cells -quiet -hierarchical] {
        if {[string trimleft [file dirname $child] /] eq "compute/sfp_link"} {
            lappend sfp_children [file tail $child]
        }
    }
    set expected_sfp_children {cdc core zero1 zero3 zero10 zero16}
    if {[lsort $sfp_children] ne [lsort $expected_sfp_children]} {
        error "NCLP SFP transport: expected only Aurora, CDC and constant ties; found $sfp_children"
    }

    foreach {cell expected} {
        compute/compute_fabric nclp_compute_fabric_bd
        compute/sfp_mailbox nclp_ps_packet_mailbox_bd
        compute/trigger_mux stim_trigger_mux
        compute/ripple_detector ripple_detector_bd
        compute/sfp_link/cdc nclp_aurora_cdc
        stimulation/stim_controller stim_controller_bd
        stimulation/ttl_output_router ttl_output_router_bd
    } {
        set object [get_bd_cells -quiet $cell]
        if {[llength $object] != 1 || [get_property VLNV $object] ne "xilinx.com:module_ref:${expected}:1.0"} {
            error "NCLP integration: $cell must reference $expected"
        }
    }
    if {[get_property VLNV [get_bd_cells compute/sfp_link/core]] ne "xilinx.com:ip:aurora_64b66b:13.0"} {
        error "NCLP SFP transport: real Aurora 13.0 core missing"
    }
    set mailbox [get_bd_cells compute/sfp_mailbox]
    set mailbox_interfaces {}
    foreach pin [get_bd_intf_pins -of_objects $mailbox] {
        lappend mailbox_interfaces [file tail $pin]
    }
    if {[lsort $mailbox_interfaces] ne [lsort {S00_AXI S_AXIS_RX M_AXIS_TX}]} {
        error "NCLP PS mailbox: unexpected interface set: $mailbox_interfaces"
    }

    set fabric_interfaces {}
    foreach pin [get_bd_intf_pins -of_objects [get_bd_cells compute/compute_fabric]] {
        lappend fabric_interfaces [file tail $pin]
    }
    set expected_fabric_interfaces {
        S00_AXI S_AXIS_INTAN_RAW_FRAME M_AXIS_ALGO_SAMPLE S_AXIS_PS_TX
        M_AXIS_PS_RX S_AXIS_SFP_RX M_AXIS_SFP_TX
    }
    if {[lsort $fabric_interfaces] ne [lsort $expected_fabric_interfaces]} {
        error "NCLP compute fabric: interface boundary differs: $fabric_interfaces"
    }
    set sfp_interfaces {}
    foreach pin [get_bd_intf_pins -of_objects [get_bd_cells compute/sfp_link]] {
        lappend sfp_interfaces [file tail $pin]
    }
    if {[lsort $sfp_interfaces] ne [lsort {M_AXIS_RX S_AXIS_TX}]} {
        error "NCLP SFP transport: hierarchy exposes application/control interfaces: $sfp_interfaces"
    }
    set sfp_pins {}
    foreach pin [get_bd_pins -of_objects [get_bd_cells compute/sfp_link]] {
        set pin_name [file tail $pin]
        if {![string match "M_AXIS_RX_*" $pin_name] &&
            ![string match "S_AXIS_TX_*" $pin_name]} {
            lappend sfp_pins $pin_name
        }
    }
    set expected_sfp_pins {
        sys_clk sys_resetn phy_enable link_up link_fault
        ps_tx_packet_accepted_pulse
        sfp_rx_p sfp_rx_n sfp_tx_p sfp_tx_n sfp_refclk_p sfp_refclk_n
        sfp_mod_abs sfp_tx_fault sfp_rx_los sfp_tx_disable
    }
    if {[lsort $sfp_pins] ne [lsort $expected_sfp_pins]} {
        error "NCLP SFP transport: hierarchy pin boundary differs: $sfp_pins"
    }

    foreach endpoints {
        {capture/intan_spi_module/M_AXIS_COMPUTE compute/compute_fabric/S_AXIS_INTAN_RAW_FRAME}
        {compute/compute_fabric/M_AXIS_ALGO_SAMPLE compute/ripple_detector/S_AXIS_ALGO_SAMPLE}
        {compute/sfp_mailbox/M_AXIS_TX compute/compute_fabric/S_AXIS_PS_TX}
        {compute/compute_fabric/M_AXIS_PS_RX compute/sfp_mailbox/S_AXIS_RX}
        {compute/compute_fabric/M_AXIS_SFP_TX compute/sfp_link/S_AXIS_TX
         compute/sfp_link/cdc/S_AXIS_TX}
        {compute/sfp_link/M_AXIS_RX compute/compute_fabric/S_AXIS_SFP_RX}
        {compute/sfp_link/cdc/M_AURORA_TX compute/sfp_link/core/USER_DATA_S_AXIS_TX}
        {compute/sfp_link/core/USER_DATA_M_AXIS_RX compute/sfp_link/cdc/S_AURORA_RX}
        {capture/intan_spi_module/M_AXIS_RECORDING capture/intan_ddr_writer/S_AXIS}
        {platform/zynq_ultra_ps/M_AXI_HPM0_LPD platform/smartconnect_M_LP/S00_AXI}
        {platform/smartconnect_M_LP/M00_AXI capture/control_interconnect/S00_AXI}
        {platform/smartconnect_M_LP/M01_AXI compute/control_interconnect/S00_AXI}
        {capture/control_interconnect/M00_AXI capture/intan_spi_module/S00_AXI}
        {capture/control_interconnect/M01_AXI capture/intan_ddr_writer/S00_AXI}
        {compute/control_interconnect/M00_AXI compute/ripple_detector/S00_AXI}
        {compute/control_interconnect/M01_AXI compute/compute_fabric/S00_AXI}
        {compute/control_interconnect/M02_AXI compute/sfp_mailbox/S00_AXI}
        {platform/smartconnect_M_LP/M02_AXI platform/pl_led_control/S00_AXI}
        {platform/smartconnect_M_LP/M03_AXI stimulation/control_interconnect/S00_AXI}
        {stimulation/control_interconnect/M00_AXI stimulation/stim_controller/S00_AXI}
        {stimulation/control_interconnect/M01_AXI stimulation/ttl_output_router/S00_AXI}
        {capture/intan_ddr_writer/M_AXI platform/smartconnect_S_HP/S00_AXI}
        {platform/smartconnect_S_HP/M00_AXI platform/zynq_ultra_ps/S_AXI_HP0_FPD}
    } {nclp_assert_shared_net interface $endpoints}

    # Recording remains a direct, two-endpoint stream wholly inside capture.
    set recording_segments [nclp_connected_net_segments interface \
        [get_bd_intf_pins capture/intan_spi_module/M_AXIS_RECORDING]]
    set recording_endpoints {}
    foreach net $recording_segments {
        foreach pin [get_bd_intf_pins -of_objects [get_bd_intf_nets $net]] {
            lappend recording_endpoints [string trimleft $pin /]
        }
    }
    if {[llength $recording_segments] != 1 ||
        [lsort -unique $recording_endpoints] ne
        [lsort {capture/intan_spi_module/M_AXIS_RECORDING capture/intan_ddr_writer/S_AXIS}]} {
        error "NCLP capture: recording stream gained an extra endpoint: $recording_endpoints"
    }

    foreach endpoints {
        {capture/intan_spi_module/compute_stream_active compute/compute_fabric/compute_stream_active}
        {capture/intan_spi_module/acquisition_30ksps compute/ripple_detector/acquisition_30ksps}
        {compute/compute_fabric/sfp_mode_selected compute/trigger_mux/sfp_mode_selected}
        {compute/compute_fabric/local_compute_stream_active compute/ripple_detector/local_compute_stream_active}
        {compute/ripple_detector/stim_trigger compute/trigger_mux/local_stim_trigger}
        {compute/compute_fabric/sfp_stim_trigger compute/trigger_mux/sfp_stim_trigger}
        {compute/trigger_mux/stim_trigger stimulation/stim_controller/stim_trigger}
        {compute/compute_fabric/phy_enable compute/sfp_link/phy_enable}
        {compute/sfp_link/link_up compute/compute_fabric/link_up}
        {compute/sfp_link/link_up compute/sfp_mailbox/transport_up}
        {compute/sfp_link/link_fault compute/compute_fabric/link_fault}
        {compute/sfp_link/ps_tx_packet_accepted_pulse compute/sfp_mailbox/tx_packet_accepted_pulse}
        {compute/compute_fabric/ps_tx_packet_interrupted_pulse compute/sfp_mailbox/tx_packet_interrupted_pulse}
        {compute/compute_fabric/compute_irq platform/ps_irq_concat/In1}
        {compute/sfp_mailbox/mailbox_irq platform/ps_irq_concat/In2}
        {stimulation/stim_controller/stimulus_level stimulation/ttl_output_router/stimulus_level}
        {stimulation/stim_controller/trigger_monitor_level stimulation/ttl_output_router/trigger_monitor_level}
        {stimulation/stim_controller/configuration_locked stimulation/ttl_output_router/route_write_locked}
        {capture/intan_spi_module/intan_sync_out stimulation/ttl_output_router/intan_sync}
        {capture/intan_ddr_writer/transfer_irq platform/system_health/intan_ddr_transfer_irq}
        {capture/intan_ddr_writer/transfer_fault platform/system_health/intan_ddr_transfer_fault}
        {capture/intan_spi_module/intan_error_irq platform/system_health/intan_error_irq}
        {platform/system_health/ps_irq platform/ps_irq_concat/In0}
        {platform/ps_irq_concat/dout platform/zynq_ultra_ps/pl_ps_irq0}
        {stimulation/ttl_output_router/ttl_out ttl_out}
    } {nclp_assert_shared_net signal $endpoints}

    foreach endpoints {
        {platform/clk_wiz/clk_out1 compute/compute_fabric/s00_axi_aclk
         compute/trigger_mux/clk compute/ripple_detector/s00_axi_aclk
         compute/sfp_link/cdc/sys_clk compute/sfp_mailbox/s00_axi_aclk
         compute/control_interconnect/aclk platform/smartconnect_S_HP/aclk}
        {platform/rst_ps/peripheral_aresetn compute/compute_fabric/s00_axi_aresetn
         compute/trigger_mux/resetn compute/ripple_detector/s00_axi_aresetn
         compute/sfp_link/cdc/sys_resetn compute/sfp_mailbox/s00_axi_aresetn
         compute/control_interconnect/aresetn platform/smartconnect_S_HP/aresetn}
    } {
        set kind signal
        nclp_assert_shared_net $kind $endpoints
    }

    nclp_assert_bus_width compute/compute_fabric/s_axis_intan_raw_frame_tdata 15
    nclp_assert_bus_width compute/compute_fabric/s_axis_intan_raw_frame_tkeep 1
    foreach endpoint {
        compute/compute_fabric/m_axis_sfp_tx_ps_owned
        compute/sfp_link/S_AXIS_TX_tuser
        compute/sfp_link/cdc/s_axis_tx_ps_owned
    } {
        set scalar_pin [get_bd_pins -quiet $endpoint]
        if {[llength $scalar_pin] != 1 ||
            [get_property LEFT $scalar_pin] ne "" ||
            [get_property RIGHT $scalar_pin] ne ""} {
            error "NCLP SFP ownership: $endpoint must be a scalar AXIS TUSER pin"
        }
    }
    foreach endpoint {
        compute/compute_fabric/M_AXIS_SFP_TX
        compute/sfp_link/cdc/S_AXIS_TX
    } {
        set interface_pin [get_bd_intf_pins -quiet $endpoint]
        if {[llength $interface_pin] != 1 ||
            [get_property CONFIG.TUSER_WIDTH $interface_pin] != 1} {
            error "NCLP SFP ownership: $endpoint must carry one TUSER bit"
        }
    }
    foreach prefix {
        m_axis_algo_sample s_axis_ps_tx m_axis_ps_rx s_axis_sfp_rx m_axis_sfp_tx
    } {
        nclp_assert_bus_width compute/compute_fabric/${prefix}_tdata 63
        nclp_assert_bus_width compute/compute_fabric/${prefix}_tkeep 7
    }
    foreach prefix {s_axis_rx m_axis_tx} {
        nclp_assert_bus_width compute/sfp_mailbox/${prefix}_tdata 63
        nclp_assert_bus_width compute/sfp_mailbox/${prefix}_tkeep 7
    }

    foreach obsolete_cell {
        compute_stream_router link_controller link_rx_ready sfp_dma
    } {
        foreach cell [get_bd_cells -quiet -hierarchical] {
            if {[file tail $cell] eq $obsolete_cell} {
                error "NCLP cleanup: obsolete cell remains: $cell"
            }
        }
    }
    foreach obsolete_reference {
        nclp_compute_stream_router_bd nclp_link_controller_bd
    } {
        if {[llength [get_bd_cells -quiet -hierarchical -filter \
                "VLNV == xilinx.com:module_ref:${obsolete_reference}:1.0"]] != 0} {
            error "NCLP cleanup: obsolete module reference remains: $obsolete_reference"
        }
    }
    foreach forbidden_interface {S00_AXI S_AXIS_COMPUTE S_AXIS_PS_TX M_AXIS_PS_RX} {
        if {[llength [get_bd_intf_pins -quiet compute/sfp_link/$forbidden_interface]] != 0} {
            error "NCLP SFP transport: application interface remains: $forbidden_interface"
        }
    }
    foreach forbidden_pin {
        sfp_stim_trigger compute_irq sfp_mode_selected compute_stream_active
        latest_compute_timestamp invalid_compute_event_count
    } {
        if {[llength [get_bd_pins -quiet compute/sfp_link/$forbidden_pin]] != 0} {
            error "NCLP SFP transport: application pin remains: $forbidden_pin"
        }
    }
    foreach obsolete_pin {
        compute_tx_idle compute_abort_safe compute_transport_available
        compute_diagnostic_clear compute_stream_fault_clear compute_session_abort
        ps_tx_packet_delivered
        sfp_compute_active sfp_stream_fault sfp_timestamp_valid
        sfp_latest_timestamp sfp_invalid_event_count source_stream_mask
        admission_armed admission_busy admission_safe_off admission_accepted
        admission_rejected admission_generation link_disarm
    } {
        if {[llength [get_bd_pins -quiet compute/compute_fabric/$obsolete_pin]] != 0} {
            error "NCLP compute fabric: obsolete cross-module pin remains: $obsolete_pin"
        }
    }
    foreach obsolete_interface {
        S_AXIS_COMPUTE M_AXIS_SFP_COMPUTE M_AXIS_SFP_PACKED
        S_STIM_STATUS M_STIM_CONTROL S_AXIS_RECORDING M_AXIS_DDR
    } {
        if {[llength [get_bd_intf_pins -quiet compute/compute_fabric/$obsolete_interface]] != 0} {
            error "NCLP compute fabric: obsolete interface remains: $obsolete_interface"
        }
    }
    foreach obsolete {intan_running intan_sync_out ttl_route ttl_route_conflict ttl_safe_off_active ttl_stimulus_active ttl_trigger_monitor stimulus_active safe_off_active} {
        if {[llength [get_bd_pins -quiet stimulation/stim_controller/$obsolete]] != 0} {
            error "NCLP stimulation: controller retains obsolete TTL routing pin $obsolete"
        }
    }
    foreach obsolete {safe_off_active configuration_locked stimulus_active trigger_monitor} {
        if {[llength [get_bd_pins -quiet stimulation/ttl_output_router/$obsolete]] != 0} {
            error "NCLP stimulation: TTL router retains obsolete policy pin $obsolete"
        }
    }

    # Only the TTL router drives the two physical outputs.
    set ttl_drivers {}
    foreach net [nclp_connected_net_segments signal [get_bd_ports ttl_out]] {
        foreach pin [get_bd_pins -quiet -of_objects [get_bd_nets $net]] {
            set owner [get_bd_cells -quiet [file dirname $pin]]
            if {[get_property DIR $pin] eq "O" && [get_property TYPE $owner] ne "hier"} {
                lappend ttl_drivers [string trimleft $pin /]
            }
        }
    }
    if {[lsort -unique $ttl_drivers] ne "stimulation/ttl_output_router/ttl_out"} {
        error "NCLP TTL routing: ttl_out has unexpected driver(s): $ttl_drivers"
    }
    if {[get_property LEFT [get_bd_ports ttl_out]] != 1 ||
        [get_property RIGHT [get_bd_ports ttl_out]] != 0} {
        error "NCLP TTL routing: physical ttl_out must remain two bits"
    }

    foreach subsystem {capture compute stimulation} {
        set control_interfaces {}
        foreach pin [get_bd_intf_pins -of_objects [get_bd_cells $subsystem]] {
            if {[get_property VLNV $pin] eq "xilinx.com:interface:aximm_rtl:1.0" &&
                [get_property MODE $pin] eq "Slave"} {
                lappend control_interfaces [file tail $pin]
            }
        }
        if {$control_interfaces ne "S_AXI_CTRL"} {
            error "NCLP boundary: $subsystem must expose exactly one AXI control slave"
        }
    }
    foreach {cell outputs} {
        capture/control_interconnect 2
        compute/control_interconnect 3
        stimulation/control_interconnect 2
    } {
        if {[get_property CONFIG.NUM_SI [get_bd_cells $cell]] != 1 ||
            [get_property CONFIG.NUM_MI [get_bd_cells $cell]] != $outputs} {
            error "NCLP control decode: $cell must be a 1x${outputs} SmartConnect"
        }
        nclp_assert_shared_net signal [list platform/clk_wiz/clk_out1 $cell/aclk]
        nclp_assert_shared_net signal [list platform/rst_ps/peripheral_aresetn $cell/aresetn]
    }
    if {[get_property CONFIG.NUM_SI [get_bd_cells platform/smartconnect_S_HP]] != 1 ||
        [get_property CONFIG.NUM_MI [get_bd_cells platform/smartconnect_S_HP]] != 1} {
        error "NCLP DDR path: shared HP SmartConnect must have only the capture writer input"
    }

    set ps [get_bd_cells platform/zynq_ultra_ps]
    set irq [get_bd_pins platform/zynq_ultra_ps/pl_ps_irq0]
    set concat [get_bd_cells platform/ps_irq_concat]
    if {[get_property CONFIG.PSU__NUM_F2P0__INTR__INPUTS $ps] != 3 ||
        [get_property LEFT $irq] != 2 || [get_property RIGHT $irq] != 0 ||
        [get_property VLNV $concat] ne "xilinx.com:ip:xlconcat:2.1" ||
        [get_property CONFIG.NUM_PORTS $concat] != 3} {
        error "NCLP interrupts: expected system-health, compute and SFP-mailbox bits"
    }
    foreach index {0 1 2} {
        if {[get_property CONFIG.IN${index}_WIDTH $concat] != 1} {
            error "NCLP interrupts: ps_irq_concat/In$index must be one bit"
        }
    }

    foreach {segment offset size} {
        compute_fabric 0x80021000 0x1000
        sfp_mailbox 0x80030000 0x1000
        intan_spi_module 0x80000000 0x10000
        intan_ddr_writer 0x80010000 0x1000
        pl_led_control 0x80011000 0x1000
        stim_controller 0x80012000 0x2000
        ttl_output_router 0x80014000 0x1000
        ripple_detector 0x80020000 0x1000
    } {
        set mapping [get_bd_addr_segs -quiet platform/zynq_ultra_ps/Data/SEG_${segment}_reg0]
        if {[llength $mapping] != 1 || [get_property OFFSET $mapping] != $offset ||
            [get_property RANGE $mapping] != $size} {
            error "NCLP address map: $segment must remain $offset / $size bytes"
        }
    }
    foreach mapping [get_bd_addr_segs -quiet -of_objects [get_bd_addr_spaces platform/zynq_ultra_ps/Data]] {
        if {[get_property OFFSET $mapping] == 0x80022000} {
            error "NCLP address map: obsolete 0x80022000 compute aperture remains"
        }
    }

    puts "NCLP_BD_CONNECTIVITY_PASS: pure SFP transport; one compute fabric; fixed PS packet mailbox; one DDR master; independent three-bit PS IRQ"
}
