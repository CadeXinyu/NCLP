#ifndef NCLP_RESULT_STORE_CONTROL_H
#define NCLP_RESULT_STORE_CONTROL_H

#include "../protocol/nclp_results.h"

#include <stdint.h>

void nclp_result_store_control_init(void);
uint32_t nclp_result_begin(uint32_t type, uint32_t total_records,
                           uint32_t command_sequence);
int nclp_result_write_record(uint32_t type, uint32_t index,
                             const nclp_result_record_t *record);
uint32_t nclp_result_staged_count(uint32_t type);
int nclp_result_commit(uint32_t type, uint32_t state,
                       uint32_t operation_status, uint32_t fail_count,
                       uint32_t layout_id);

void nclp_progress_begin(uint32_t operation, uint32_t state,
                         uint32_t result_id, uint32_t total_units,
                         uint32_t fail_count);
void nclp_progress_update(uint32_t completed_units,
                          uint32_t current_stream,
                          uint32_t current_channel,
                          uint32_t current_cap_range,
                          uint32_t status,
                          uint32_t fail_count);
void nclp_progress_advance(uint32_t current_stream,
                           uint32_t current_channel,
                           uint32_t current_cap_range,
                           uint32_t status,
                           uint32_t fail_count);
void nclp_progress_set_state(uint32_t state, uint32_t status,
                             uint32_t fail_count);
void nclp_progress_finish(uint32_t state, uint32_t status,
                          uint32_t fail_count);

#endif
