`ifndef DDRW_REGISTERS_VH
`define DDRW_REGISTERS_VH

// DDR writer ABI v2. Register addresses are byte offsets in the complete
// 4-KiB AXI-Lite aperture. Reserved and unaligned accesses have no effect and
// read as zero.

`define DDRW_BLOCK_ID                            32'h4444_5257 // "DDRW"
`define DDRW_ABI_VERSION                         32'h0002_0000
// [23:16] maximum AXI burst beats, [15:8] AXI beat bytes,
// [7:0] input AXIS beat bytes.
`define DDRW_INFO_VALUE                          32'h0010_0802

// Identity header.
`define DDRW_REG_BLOCK_ID                                12'h000
`define DDRW_REG_ABI_VERSION                             12'h004
`define DDRW_REG_CAPABILITIES                            12'h008
`define DDRW_REG_INFO                                    12'h00C

// Session lifecycle: command pulses and live status.
`define DDRW_REG_COMMAND                                 12'h010
`define DDRW_REG_STATUS                                  12'h014
`define DDRW_REG_SESSION_STATE                           12'h018
`define DDRW_REG_SESSION_ID                              12'h01C

// Next-session shadow geometry; START latches a coherent snapshot.
`define DDRW_REG_RING_BASE_ADDR_LO                       12'h020
`define DDRW_REG_RING_BASE_ADDR_HI                       12'h024
`define DDRW_REG_RING_SIZE_BYTES                         12'h028
`define DDRW_REG_BLOCK_SIZE_BYTES                        12'h02C
`define DDRW_REG_RING_CAPACITY_BLOCKS                    12'h030
`define DDRW_REG_IRQ_COMPLETION_INTERVAL_BLOCKS          12'h034

// Live write position and the last committed block.
`define DDRW_REG_CURRENT_WRITE_ADDR_LO                   12'h040
`define DDRW_REG_CURRENT_WRITE_ADDR_HI                   12'h044
`define DDRW_REG_CURRENT_WRITE_OFFSET_BYTES              12'h048
`define DDRW_REG_LAST_PRODUCED_BLOCK_ADDR_LO             12'h04C
`define DDRW_REG_LAST_PRODUCED_BLOCK_ADDR_HI             12'h050
`define DDRW_REG_LAST_PRODUCED_BLOCK_SIZE_BYTES          12'h054

// Producer/consumer accounting. CONSUMED_BLOCK_COUNT is RW and absolute.
`define DDRW_REG_CONSUMED_BLOCK_COUNT                    12'h060
`define DDRW_REG_PRODUCED_BLOCK_COUNT                    12'h064
`define DDRW_REG_OUTSTANDING_BLOCK_COUNT                 12'h068
`define DDRW_REG_FINAL_BLOCK_SIZE_BYTES                  12'h06C

// FIFO diagnostics.
`define DDRW_REG_FIFO_LEVEL_ENTRIES                      12'h070
`define DDRW_REG_FIFO_HIGH_WATER_ENTRIES                 12'h074

// Sticky errors. ERROR_STATUS is write-one-to-clear.
`define DDRW_REG_ERROR_STATUS                            12'h080
`define DDRW_REG_LAST_AXI_BRESP                          12'h084
`define DDRW_REG_INPUT_PROTOCOL_ERROR_COUNT              12'h088

// Transfer diagnostics.
`define DDRW_REG_INPUT_BACKPRESSURE_CYCLE_COUNT          12'h0A0
`define DDRW_REG_DDR_COMMITTED_BYTE_COUNT_LO             12'h0A4
`define DDRW_REG_DDR_COMMITTED_BYTE_COUNT_HI             12'h0A8

// Coherent counters captured by COMMAND.SNAPSHOT.
`define DDRW_REG_SNAPSHOT_SEQUENCE                       12'h0C0
`define DDRW_REG_SNAPSHOT_PRODUCED_BLOCK_COUNT           12'h0C4
`define DDRW_REG_SNAPSHOT_CONSUMED_BLOCK_COUNT           12'h0C8
`define DDRW_REG_SNAPSHOT_OUTSTANDING_BLOCK_COUNT        12'h0CC
`define DDRW_REG_SNAPSHOT_FINAL_BLOCK_SIZE_BYTES         12'h0D0

`define DDRW_CAP_BURST16                         32'h0000_0001
`define DDRW_CAP_INTERNAL_FIFO                   32'h0000_0002
`define DDRW_CAP_FIXED_SLOT                      32'h0000_0004
`define DDRW_CAP_EOS                             32'h0000_0008
`define DDRW_CAPABILITIES_VALUE                  32'h0000_000F

`define DDRW_COMMAND_START                       32'h0000_0001
`define DDRW_COMMAND_ABORT                       32'h0000_0002
`define DDRW_COMMAND_SOFT_RESET                  32'h0000_0004
`define DDRW_COMMAND_IRQ_ACK                     32'h0000_0008
`define DDRW_COMMAND_SNAPSHOT                    32'h0000_0010

`define DDRW_STATUS_RUNNING                      32'h0000_0001
`define DDRW_STATUS_UNCONSUMED_BLOCK_AVAILABLE   32'h0000_0002
`define DDRW_STATUS_DATA_PATH_BUSY               32'h0000_0004
`define DDRW_STATUS_RING_FULL                    32'h0000_0008
`define DDRW_STATUS_IRQ_PENDING                  32'h0000_0010
`define DDRW_STATUS_EOS_SEEN                     32'h0000_0020
`define DDRW_STATUS_EOS_COMMITTED                32'h0000_0040
`define DDRW_STATUS_ABORT_DONE                   32'h0000_0080
`define DDRW_STATUS_FIFO_NONEMPTY                32'h0000_0100
`define DDRW_STATUS_FAULT                        32'h0000_0200
`define DDRW_STATUS_START_REJECTED               32'h0000_0400
`define DDRW_STATUS_CONFIG_ERROR                 32'h0000_0800
`define DDRW_STATUS_AXI_ERROR                    32'h0000_1000
`define DDRW_STATUS_DATA_LOSS                    32'h0000_2000

`define DDRW_ERROR_DATA_LOSS                     32'h0000_0001
`define DDRW_ERROR_CONFIG_INVALID                32'h0000_0002
`define DDRW_ERROR_AXI_WRITE_RESPONSE            32'h0000_0004
`define DDRW_ERROR_SESSION_ABORTED               32'h0000_0008
`define DDRW_ERROR_INPUT_PROTOCOL                32'h0000_0010
`define DDRW_ERROR_ADDRESS_INVALID               32'h0000_0020
`define DDRW_ERROR_RESET_DURING_SESSION          32'h0000_0040
`define DDRW_ERROR_START_REJECTED                32'h0000_0080
`define DDRW_ERROR_IMPLEMENTED_MASK              32'h0000_00FF

`define DDRW_STATE_IDLE                          4'd0
`define DDRW_STATE_CAPTURING                     4'd1
`define DDRW_STATE_FULL_WAIT                     4'd2
`define DDRW_STATE_DRAINING                      4'd3
`define DDRW_STATE_DONE                          4'd4
`define DDRW_STATE_ABORT_DRAIN                   4'd5
`define DDRW_STATE_ABORT_DONE                    4'd6
`define DDRW_STATE_FAULT_DRAIN                   4'd7
`define DDRW_STATE_FAULT                         4'd8

`endif
