`timescale 1ns / 1ps
`default_nettype none

package clock_drp_pkg;

    typedef struct packed {
        logic        edge_select;
        logic        no_count;
        logic [5:0]  high_time;
        logic [5:0]  low_time;
    } clock_count_t;

    typedef struct packed {
        logic [6:0]  address;
        // A one keeps the corresponding bit read from the DRP register.
        // The command writes (old_value & preserve_mask) | data.
        logic [15:0] preserve_mask;
        logic [15:0] data;
    } drp_command_t;

    typedef enum logic [2:0] {
        CLOCK_ERROR_NONE          = 3'd0,
        CLOCK_ERROR_INVALID       = 3'd1,
        CLOCK_ERROR_READ_TIMEOUT  = 3'd2,
        CLOCK_ERROR_WRITE_TIMEOUT = 3'd3,
        CLOCK_ERROR_LOCK_TIMEOUT  = 3'd4,
        CLOCK_ERROR_LOCK_LOST     = 3'd5,
        CLOCK_ERROR_RESET_TIMEOUT = 3'd6
    } clock_error_t;

    // Vivado 2025.1 Clocking Wizard integer-divider encoding.  The encoded
    // six-bit zero represents 64.  Divide-by-one must set NO_COUNT; omitting
    // that bit programs a divide-by-two counter instead.
    function automatic clock_count_t clock_count(
        input logic [7:0] divide
    );
        clock_count_t result;
        logic [7:0] high_wide;
        logic [7:0] low_wide;
        begin
            result = '0;
            if (divide == 8'd1) begin
                result.high_time = 6'd1;
                result.low_time  = 6'd1;
                result.no_count  = 1'b1;
            end else begin
                high_wide = divide >> 1;
                low_wide  = divide - high_wide;
                result.high_time = high_wide[5:0];
                result.low_time  = low_wide[5:0];
                result.edge_select = divide[0];
            end
            return result;
        end
    endfunction

    function automatic logic clock_divider_valid(input logic [7:0] divide);
        return divide >= 8'd1 && divide <= 8'd128;
    endfunction

    // LOCK table shared by MMCME4_ADV and PLLE4_ADV. Values are the
    // UltraScale+ table emitted by Vivado 2025.1 Clocking Wizard.
    function automatic logic [39:0] lock_settings(input logic [7:0] multiply);
        begin
            case (multiply - 8'd1)
                8'd0:  lock_settings = 40'b00110_00110_1111101000_1111101001_0000000001;
                8'd1:  lock_settings = 40'b00110_00110_1111101000_1111101001_0000000001;
                8'd2:  lock_settings = 40'b01000_01000_1111101000_1111101001_0000000001;
                8'd3:  lock_settings = 40'b01011_01011_1111101000_1111101001_0000000001;
                8'd4:  lock_settings = 40'b01110_01110_1111101000_1111101001_0000000001;
                8'd5:  lock_settings = 40'b10001_10001_1111101000_1111101001_0000000001;
                8'd6:  lock_settings = 40'b10011_10011_1111101000_1111101001_0000000001;
                8'd7:  lock_settings = 40'b10110_10110_1111101000_1111101001_0000000001;
                8'd8:  lock_settings = 40'b11001_11001_1111101000_1111101001_0000000001;
                8'd9:  lock_settings = 40'b11100_11100_1111101000_1111101001_0000000001;
                8'd10: lock_settings = 40'b11111_11111_1110000100_1111101001_0000000001;
                8'd11: lock_settings = 40'b11111_11111_1100111001_1111101001_0000000001;
                8'd12: lock_settings = 40'b11111_11111_1011101110_1111101001_0000000001;
                8'd13: lock_settings = 40'b11111_11111_1010111100_1111101001_0000000001;
                8'd14: lock_settings = 40'b11111_11111_1010001010_1111101001_0000000001;
                8'd15: lock_settings = 40'b11111_11111_1001110001_1111101001_0000000001;
                8'd16: lock_settings = 40'b11111_11111_1000111111_1111101001_0000000001;
                8'd17: lock_settings = 40'b11111_11111_1000100110_1111101001_0000000001;
                8'd18: lock_settings = 40'b11111_11111_1000001101_1111101001_0000000001;
                8'd19: lock_settings = 40'b11111_11111_0111110100_1111101001_0000000001;
                8'd20: lock_settings = 40'b11111_11111_0111011011_1111101001_0000000001;
                8'd21: lock_settings = 40'b11111_11111_0111000010_1111101001_0000000001;
                8'd22: lock_settings = 40'b11111_11111_0110101001_1111101001_0000000001;
                8'd23: lock_settings = 40'b11111_11111_0110010000_1111101001_0000000001;
                8'd24: lock_settings = 40'b11111_11111_0110010000_1111101001_0000000001;
                8'd25: lock_settings = 40'b11111_11111_0101110111_1111101001_0000000001;
                8'd26: lock_settings = 40'b11111_11111_0101011110_1111101001_0000000001;
                8'd27: lock_settings = 40'b11111_11111_0101011110_1111101001_0000000001;
                8'd28: lock_settings = 40'b11111_11111_0101000101_1111101001_0000000001;
                8'd29: lock_settings = 40'b11111_11111_0101000101_1111101001_0000000001;
                8'd30: lock_settings = 40'b11111_11111_0100101100_1111101001_0000000001;
                8'd31: lock_settings = 40'b11111_11111_0100101100_1111101001_0000000001;
                8'd32: lock_settings = 40'b11111_11111_0100101100_1111101001_0000000001;
                8'd33: lock_settings = 40'b11111_11111_0100010011_1111101001_0000000001;
                8'd34: lock_settings = 40'b11111_11111_0100010011_1111101001_0000000001;
                8'd35: lock_settings = 40'b11111_11111_0100010011_1111101001_0000000001;
                8'd36: lock_settings = 40'b11111_11111_0011111010_1111101001_0000000001;
                8'd37: lock_settings = 40'b11111_11111_0011111010_1111101001_0000000001;
                8'd38: lock_settings = 40'b11111_11111_0011111010_1111101001_0000000001;
                8'd39: lock_settings = 40'b11111_11111_0011111010_1111101001_0000000001;
                8'd40: lock_settings = 40'b11111_11111_0011111010_1111101001_0000000001;
                8'd41: lock_settings = 40'b11111_11111_0011111010_1111101001_0000000001;
                8'd42: lock_settings = 40'b11111_11111_0011111010_1111101001_0000000001;
                8'd43: lock_settings = 40'b11111_11111_0011111010_1111101001_0000000001;
                8'd44: lock_settings = 40'b11111_11111_0011111010_1111101001_0000000001;
                8'd45: lock_settings = 40'b11111_11111_0011111010_1111101001_0000000001;
                8'd46: lock_settings = 40'b11111_11111_0011111010_1111101001_0000000001;
                8'd47: lock_settings = 40'b11111_11111_0011111010_1111101001_0000000001;
                8'd48: lock_settings = 40'b11111_11111_0011111010_1111101001_0000000001;
                8'd49: lock_settings = 40'b11111_11111_0011111010_1111101001_0000000001;
                8'd50: lock_settings = 40'b11111_11111_0011111010_1111101001_0000000001;
                8'd51: lock_settings = 40'b11111_11111_0011111010_1111101001_0000000001;
                8'd52: lock_settings = 40'b11111_11111_0011111010_1111101001_0000000001;
                8'd53: lock_settings = 40'b11111_11111_0011111010_1111101001_0000000001;
                8'd54: lock_settings = 40'b11111_11111_0011111010_1111101001_0000000001;
                8'd55: lock_settings = 40'b11111_11111_0011111010_1111101001_0000000001;
                8'd56: lock_settings = 40'b11111_11111_0011111010_1111101001_0000000001;
                8'd57: lock_settings = 40'b11111_11111_0011111010_1111101001_0000000001;
                8'd58: lock_settings = 40'b11111_11111_0011111010_1111101001_0000000001;
                8'd59: lock_settings = 40'b11111_11111_0011111010_1111101001_0000000001;
                8'd60: lock_settings = 40'b11111_11111_0011111010_1111101001_0000000001;
                8'd61: lock_settings = 40'b11111_11111_0011111010_1111101001_0000000001;
                8'd62: lock_settings = 40'b11111_11111_0011111010_1111101001_0000000001;
                8'd63: lock_settings = 40'b11111_11111_0011111010_1111101001_0000000001;
                default: lock_settings = 40'b00110_00110_1111101000_1111101001_0000000001;
            endcase
        end
    endfunction

    function automatic logic [9:0] mmcm_filter_settings(input logic [7:0] multiply);
        begin
            case (multiply - 8'd1)
                8'd0, 8'd1: mmcm_filter_settings = 10'b0111_1111_11;
                8'd2:  mmcm_filter_settings = 10'b1110_1111_11;
                8'd3:  mmcm_filter_settings = 10'b1111_1111_11;
                8'd4:  mmcm_filter_settings = 10'b1111_1011_11;
                8'd5:  mmcm_filter_settings = 10'b1111_1101_11;
                8'd6:  mmcm_filter_settings = 10'b1111_0011_11;
                8'd7:  mmcm_filter_settings = 10'b1110_0101_11;
                8'd8, 8'd9: mmcm_filter_settings = 10'b1111_1001_11;
                8'd10: mmcm_filter_settings = 10'b1110_1110_11;
                8'd11: mmcm_filter_settings = 10'b1111_1110_11;
                8'd12, 8'd13, 8'd14: mmcm_filter_settings = 10'b1111_0001_11;
                8'd15, 8'd16: mmcm_filter_settings = 10'b1110_0110_11;
                8'd17: mmcm_filter_settings = 10'b1111_0110_11;
                8'd18, 8'd19: mmcm_filter_settings = 10'b1110_1010_11;
                8'd20, 8'd21, 8'd22, 8'd23, 8'd24:
                    mmcm_filter_settings = 10'b1111_1010_11;
                8'd25, 8'd26, 8'd27: mmcm_filter_settings = 10'b1101_1100_11;
                8'd28, 8'd29, 8'd30: mmcm_filter_settings = 10'b1110_1100_11;
                8'd31, 8'd32, 8'd33, 8'd34, 8'd35, 8'd36:
                    mmcm_filter_settings = 10'b1111_1100_11;
                8'd37, 8'd38, 8'd39, 8'd40:
                    mmcm_filter_settings = 10'b1110_0010_11;
                8'd41, 8'd42, 8'd43, 8'd44, 8'd45, 8'd46, 8'd47,
                8'd48, 8'd49, 8'd50, 8'd51, 8'd52, 8'd53, 8'd54,
                8'd55, 8'd56, 8'd57, 8'd58, 8'd59, 8'd60, 8'd61:
                    mmcm_filter_settings = 10'b1111_0010_11;
                8'd62, 8'd63: mmcm_filter_settings = 10'b1100_0100_11;
                default: mmcm_filter_settings = 10'b0111_1111_11;
            endcase
        end
    endfunction

    function automatic logic [9:0] pll_filter_settings(input logic [7:0] multiply);
        begin
            case (multiply - 8'd1)
                8'd0, 8'd1: pll_filter_settings = 10'b0011_0111_11;
                8'd2:  pll_filter_settings = 10'b0011_0011_11;
                8'd3:  pll_filter_settings = 10'b0011_1001_11;
                8'd4:  pll_filter_settings = 10'b0011_0001_11;
                8'd5:  pll_filter_settings = 10'b0100_1110_11;
                8'd6:  pll_filter_settings = 10'b0011_0110_11;
                8'd7:  pll_filter_settings = 10'b0011_1010_11;
                8'd8, 8'd9: pll_filter_settings = 10'b0111_1001_11;
                8'd10: pll_filter_settings = 10'b0101_0110_11;
                8'd11: pll_filter_settings = 10'b1100_0101_11;
                8'd12: pll_filter_settings = 10'b0101_1010_11;
                8'd13: pll_filter_settings = 10'b0110_0110_11;
                8'd14: pll_filter_settings = 10'b0110_1010_11;
                8'd15: pll_filter_settings = 10'b0111_0110_11;
                8'd16: pll_filter_settings = 10'b1111_0101_11;
                8'd17: pll_filter_settings = 10'b1100_0110_11;
                8'd18: pll_filter_settings = 10'b1110_0001_11;
                8'd19: pll_filter_settings = 10'b1101_0110_11;
                8'd20: pll_filter_settings = 10'b1111_0001_11;
                default: pll_filter_settings = 10'b0011_0111_11;
            endcase
        end
    endfunction

endpackage

`default_nettype wire
