#ifndef NCLP_RESULT_STORE_NETWORK_H
#define NCLP_RESULT_STORE_NETWORK_H

#include "nclp_shared.h"
#include "../protocol/nclp_results.h"

#include <stdint.h>

#define NCLP_RESULT_READ_OK        0
#define NCLP_RESULT_READ_NONE      1
#define NCLP_RESULT_READ_STALE     2
#define NCLP_RESULT_READ_RANGE     3
#define NCLP_RESULT_READ_BUSY      4
#define NCLP_RESULT_READ_INVALID   5

int nclp_result_read_progress(nclp_progress_t *progress);
int nclp_result_read(uint32_t type, uint32_t result_id,
                     uint32_t record_index,
                     nclp_result_header_t *header,
                     nclp_result_record_t *record);

#endif
