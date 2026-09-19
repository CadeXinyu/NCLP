#ifndef NCLP_INTAN_SYNC_CONTROL_H
#define NCLP_INTAN_SYNC_CONTROL_H
#include <stdint.h>

#define NCLP_INTAN_SYNC_OK 0
#define NCLP_INTAN_SYNC_ERROR_HARDWARE (-1)
#define NCLP_INTAN_SYNC_ERROR_ARGUMENT (-2)
#define NCLP_INTAN_SYNC_ERROR_BUSY (-3)

/* Timing is measured in Intan frames. Non-periodic modes take zero timing
 * arguments. Changes require stopped acquisition and apply on its next START. */
void nclp_intan_sync_read(uint32_t words[4]);
int nclp_intan_sync_set(uint32_t mode, uint32_t period_frames,
                        uint32_t high_frames, uint32_t words[4]);
#endif
