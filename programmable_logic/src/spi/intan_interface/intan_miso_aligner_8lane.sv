`timescale 1ns / 1ps
`default_nettype none

// Compose eight independent lane aligners in the public logical-stream order.
module intan_miso_aligner_8lane (
    input wire [31:0] primary_phases,
    input wire [31:0] secondary_phases,
    input wire [75:0] samples_a1,
    input wire [75:0] samples_a2,
    input wire [75:0] samples_b1,
    input wire [75:0] samples_b2,
    input wire [75:0] samples_c1,
    input wire [75:0] samples_c2,
    input wire [75:0] samples_d1,
    input wire [75:0] samples_d2,
    output wire [255:0] aligned_words
);
    // Fixed public stream order:
    //   0 A1 primary, 1 A1 secondary, 2 A2 primary, 3 A2 secondary, ...
    intan_miso_lane_aligner u_a1 (.primary_phase(primary_phases[ 3: 0]), .secondary_phase(secondary_phases[ 3: 0]), .samples_4x(samples_a1), .primary_word(aligned_words[ 15:  0]), .secondary_word(aligned_words[ 31: 16]));
    intan_miso_lane_aligner u_a2 (.primary_phase(primary_phases[ 7: 4]), .secondary_phase(secondary_phases[ 7: 4]), .samples_4x(samples_a2), .primary_word(aligned_words[ 47: 32]), .secondary_word(aligned_words[ 63: 48]));
    intan_miso_lane_aligner u_b1 (.primary_phase(primary_phases[11: 8]), .secondary_phase(secondary_phases[11: 8]), .samples_4x(samples_b1), .primary_word(aligned_words[ 79: 64]), .secondary_word(aligned_words[ 95: 80]));
    intan_miso_lane_aligner u_b2 (.primary_phase(primary_phases[15:12]), .secondary_phase(secondary_phases[15:12]), .samples_4x(samples_b2), .primary_word(aligned_words[111: 96]), .secondary_word(aligned_words[127:112]));
    intan_miso_lane_aligner u_c1 (.primary_phase(primary_phases[19:16]), .secondary_phase(secondary_phases[19:16]), .samples_4x(samples_c1), .primary_word(aligned_words[143:128]), .secondary_word(aligned_words[159:144]));
    intan_miso_lane_aligner u_c2 (.primary_phase(primary_phases[23:20]), .secondary_phase(secondary_phases[23:20]), .samples_4x(samples_c2), .primary_word(aligned_words[175:160]), .secondary_word(aligned_words[191:176]));
    intan_miso_lane_aligner u_d1 (.primary_phase(primary_phases[27:24]), .secondary_phase(secondary_phases[27:24]), .samples_4x(samples_d1), .primary_word(aligned_words[207:192]), .secondary_word(aligned_words[223:208]));
    intan_miso_lane_aligner u_d2 (.primary_phase(primary_phases[31:28]), .secondary_phase(secondary_phases[31:28]), .samples_4x(samples_d2), .primary_word(aligned_words[239:224]), .secondary_word(aligned_words[255:240]));
endmodule

`default_nettype wire
