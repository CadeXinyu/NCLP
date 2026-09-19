#ifndef NCLP_RESULTS_H
#define NCLP_RESULTS_H

#include <stddef.h>
#include <stdint.h>
#include "../../../common/nclp_wire.h"

#define NCLP_RESULT_BASE_ADDRESS          0x70001000U
#define NCLP_RESULT_REGION_BYTES          0x00003000U
#define NCLP_SHARED_RESERVED_BYTES        0x00004000U

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t publish_seq;
    uint32_t result_id;
    uint32_t result_type;
    uint32_t state;
    uint32_t available_records;
    uint32_t total_records;
    uint32_t record_bytes;
    uint32_t operation_status;
    uint32_t fail_count;
    uint32_t command_sequence;
    uint32_t layout_id;
    uint32_t reserved[3];
} nclp_result_header_t;

typedef struct {
    uint32_t info;
    /* Selected cable-delay tap reported to clients. Single-MISO devices use
     * their primary/MISO-A tap. RHD2164 reports the later of its independently
     * selected MISO-A and MISO-B taps so one scalar covers both data views. */
    uint32_t phase;
    uint32_t score;
    uint32_t logical_mask;
} nclp_scan_result_record_t;

static inline uint32_t nclp_scan_result_phase(uint32_t phase_a,
                                               uint32_t phase_b,
                                               uint32_t has_secondary)
{
    return has_secondary != 0U && phase_b > phase_a ? phase_b : phase_a;
}

typedef struct {
    uint32_t info;
    uint32_t checks_errors;
    uint32_t analog_upper_hz;
    uint32_t analog_lower_millihz;
} nclp_init_result_record_t;

typedef struct {
    uint32_t info;
    uint32_t magnitude_milliohms_low;
    uint32_t magnitude_milliohms_high;
    int32_t phase_microdegrees;
} nclp_impedance_result_record_t;

typedef struct __attribute__((aligned(64))) {
    nclp_result_header_t header;
    nclp_scan_result_record_t records[NCLP_RESULT_MAX_SCAN_RECORDS];
} nclp_scan_result_slot_t;

typedef struct __attribute__((aligned(64))) {
    nclp_result_header_t header;
    nclp_init_result_record_t records[NCLP_RESULT_MAX_INIT_RECORDS];
} nclp_init_result_slot_t;

typedef struct __attribute__((aligned(64))) {
    nclp_result_header_t header;
    nclp_impedance_result_record_t records[NCLP_RESULT_MAX_IMPEDANCE_RECORDS];
} nclp_impedance_result_slot_t;

typedef struct __attribute__((aligned(64))) {
    nclp_scan_result_slot_t scan;
    nclp_init_result_slot_t init;
    nclp_impedance_result_slot_t impedance;
} nclp_result_store_t;

typedef union {
    nclp_scan_result_record_t scan;
    nclp_init_result_record_t init;
    nclp_impedance_result_record_t impedance;
    uint32_t words[4];
} nclp_result_record_t;

_Static_assert(sizeof(nclp_result_header_t) == 64U,
               "result header must be one cache line");
_Static_assert(sizeof(nclp_scan_result_record_t) == 16U,
               "scan result record must fit one TCP reply");
_Static_assert(sizeof(nclp_init_result_record_t) == 16U,
               "init result record must fit one TCP reply");
_Static_assert(sizeof(nclp_impedance_result_record_t) == 16U,
               "impedance result record must fit one TCP reply");
_Static_assert(sizeof(nclp_result_store_t) == 0x21C0U,
               "shared result store layout changed");
_Static_assert(sizeof(nclp_result_store_t) <= NCLP_RESULT_REGION_BYTES,
               "shared result store exceeds reserved region");

#endif
