#include "result_store_control.h"

#include "nclp_shared.h"

#include "xil_cache.h"

#include <stddef.h>
#include <string.h>

static nclp_result_store_t g_result_staging __attribute__((aligned(64)));
static nclp_progress_t g_progress_staging __attribute__((aligned(64)));
static uint32_t g_next_result_id;

static void result_barrier(void)
{
#if defined(__aarch64__)
    __asm__ volatile("dmb sy" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

static volatile nclp_result_store_t *shared_result_store(void)
{
    return (volatile nclp_result_store_t *)(uintptr_t)NCLP_RESULT_BASE_ADDRESS;
}

static nclp_result_header_t *staging_header(uint32_t type)
{
    if (type == NCLP_RESULT_TYPE_SCAN) {
        return &g_result_staging.scan.header;
    }
    if (type == NCLP_RESULT_TYPE_INIT) {
        return &g_result_staging.init.header;
    }
    if (type == NCLP_RESULT_TYPE_IMPEDANCE) {
        return &g_result_staging.impedance.header;
    }
    return NULL;
}

static void *staging_slot(uint32_t type, uint32_t *bytes)
{
    if (bytes == NULL) {
        return NULL;
    }
    if (type == NCLP_RESULT_TYPE_SCAN) {
        *bytes = sizeof(g_result_staging.scan);
        return &g_result_staging.scan;
    }
    if (type == NCLP_RESULT_TYPE_INIT) {
        *bytes = sizeof(g_result_staging.init);
        return &g_result_staging.init;
    }
    if (type == NCLP_RESULT_TYPE_IMPEDANCE) {
        *bytes = sizeof(g_result_staging.impedance);
        return &g_result_staging.impedance;
    }
    *bytes = 0U;
    return NULL;
}

static volatile void *shared_slot(uint32_t type)
{
    volatile nclp_result_store_t *store = shared_result_store();

    if (type == NCLP_RESULT_TYPE_SCAN) {
        return &store->scan;
    }
    if (type == NCLP_RESULT_TYPE_INIT) {
        return &store->init;
    }
    if (type == NCLP_RESULT_TYPE_IMPEDANCE) {
        return &store->impedance;
    }
    return NULL;
}

static uint32_t result_capacity(uint32_t type)
{
    if (type == NCLP_RESULT_TYPE_SCAN) {
        return NCLP_RESULT_MAX_SCAN_RECORDS;
    }
    if (type == NCLP_RESULT_TYPE_INIT) {
        return NCLP_RESULT_MAX_INIT_RECORDS;
    }
    if (type == NCLP_RESULT_TYPE_IMPEDANCE) {
        return NCLP_RESULT_MAX_IMPEDANCE_RECORDS;
    }
    return 0U;
}

static void publish_progress(void)
{
    volatile nclp_progress_t *dst = &nclp_shared_state()->progress;
    const uint32_t *src_words = (const uint32_t *)&g_progress_staging;
    volatile uint32_t *dst_words = (volatile uint32_t *)dst;
    uint32_t sequence = dst->publish_seq;

    if ((sequence & 1U) != 0U) {
        sequence++;
    }
    sequence++;
    dst->publish_seq = sequence;
    result_barrier();
    for (uint32_t i = 1U; i < sizeof(nclp_progress_t) / 4U; ++i) {
        dst_words[i] = src_words[i];
    }
    result_barrier();
    dst->publish_seq = sequence + 1U;
    Xil_DCacheFlushRange((INTPTR)dst, sizeof(*dst));
    result_barrier();
    g_progress_staging.publish_seq = sequence + 1U;
}

void nclp_result_store_control_init(void)
{
    volatile uint32_t *words =
        (volatile uint32_t *)(uintptr_t)NCLP_RESULT_BASE_ADDRESS;

    memset(&g_result_staging, 0, sizeof(g_result_staging));
    memset(&g_progress_staging, 0, sizeof(g_progress_staging));
    for (uint32_t i = 0U; i < NCLP_RESULT_REGION_BYTES / 4U; ++i) {
        words[i] = 0U;
    }
    result_barrier();
    Xil_DCacheFlushRange((INTPTR)NCLP_RESULT_BASE_ADDRESS,
                         NCLP_RESULT_REGION_BYTES);
    g_next_result_id = 0U;
}

uint32_t nclp_result_begin(uint32_t type, uint32_t total_records,
                           uint32_t command_sequence)
{
    nclp_result_header_t *header = staging_header(type);
    void *slot;
    uint32_t bytes;
    uint32_t capacity = result_capacity(type);

    slot = staging_slot(type, &bytes);
    if (header == NULL || slot == NULL || total_records > capacity) {
        return 0U;
    }
    memset(slot, 0, bytes);
    g_next_result_id++;
    if (g_next_result_id == 0U) {
        g_next_result_id = 1U;
    }
    header->magic = NCLP_RESULT_MAGIC;
    header->version = NCLP_RESULT_VERSION;
    header->result_id = g_next_result_id;
    header->result_type = type;
    header->state = NCLP_RESULT_STATE_RUNNING;
    header->total_records = total_records;
    header->record_bytes = sizeof(nclp_result_record_t);
    header->command_sequence = command_sequence;
    return header->result_id;
}

int nclp_result_write_record(uint32_t type, uint32_t index,
                             const nclp_result_record_t *record)
{
    nclp_result_header_t *header = staging_header(type);
    uint32_t capacity = result_capacity(type);
    void *destination;

    if (header == NULL || record == NULL || header->result_id == 0U ||
        index >= capacity || index >= header->total_records) {
        return -1;
    }
    if (type == NCLP_RESULT_TYPE_SCAN) {
        destination = &g_result_staging.scan.records[index];
    } else if (type == NCLP_RESULT_TYPE_INIT) {
        destination = &g_result_staging.init.records[index];
    } else {
        destination = &g_result_staging.impedance.records[index];
    }
    memcpy(destination, record, sizeof(*record));
    if (header->available_records <= index) {
        header->available_records = index + 1U;
    }
    return 0;
}

uint32_t nclp_result_staged_count(uint32_t type)
{
    nclp_result_header_t *header = staging_header(type);

    return header == NULL ? 0U : header->available_records;
}

int nclp_result_commit(uint32_t type, uint32_t state,
                       uint32_t operation_status, uint32_t fail_count,
                       uint32_t layout_id)
{
    nclp_result_header_t *header = staging_header(type);
    volatile void *destination = shared_slot(type);
    void *source;
    volatile uint32_t *dst_words;
    const uint32_t *src_words;
    uint32_t bytes;
    uint32_t sequence;

    source = staging_slot(type, &bytes);
    if (header == NULL || source == NULL || destination == NULL ||
        header->result_id == 0U ||
        (state != NCLP_RESULT_STATE_COMPLETE &&
         state != NCLP_RESULT_STATE_FAILED &&
         state != NCLP_RESULT_STATE_CANCELLED)) {
        return -1;
    }
    header->state = state;
    header->operation_status = operation_status;
    header->fail_count = fail_count;
    header->layout_id = layout_id;

    dst_words = (volatile uint32_t *)destination;
    src_words = (const uint32_t *)source;
    sequence = dst_words[offsetof(nclp_result_header_t, publish_seq) / 4U];
    if ((sequence & 1U) != 0U) {
        sequence++;
    }
    sequence++;
    dst_words[offsetof(nclp_result_header_t, publish_seq) / 4U] = sequence;
    result_barrier();
    for (uint32_t i = 0U; i < bytes / 4U; ++i) {
        if (i != (offsetof(nclp_result_header_t, publish_seq) / 4U)) {
            dst_words[i] = src_words[i];
        }
    }
    result_barrier();
    Xil_DCacheFlushRange((INTPTR)destination, bytes);
    result_barrier();
    dst_words[offsetof(nclp_result_header_t, publish_seq) / 4U] = sequence + 1U;
    Xil_DCacheFlushRange((INTPTR)&dst_words[
                             offsetof(nclp_result_header_t, publish_seq) / 4U],
                         sizeof(uint32_t));
    result_barrier();
    header->publish_seq = sequence + 1U;
    return 0;
}

void nclp_progress_begin(uint32_t operation, uint32_t state,
                         uint32_t result_id, uint32_t total_units,
                         uint32_t fail_count)
{
    memset(&g_progress_staging, 0, sizeof(g_progress_staging));
    g_progress_staging.operation = operation;
    g_progress_staging.state = state;
    g_progress_staging.result_id = result_id;
    g_progress_staging.total_units = total_units;
    g_progress_staging.current_stream = 0xFFFFFFFFU;
    g_progress_staging.current_channel = 0xFFFFFFFFU;
    g_progress_staging.current_cap_range = 0xFFFFFFFFU;
    g_progress_staging.fail_count = fail_count;
    publish_progress();
}

void nclp_progress_update(uint32_t completed_units,
                          uint32_t current_stream,
                          uint32_t current_channel,
                          uint32_t current_cap_range,
                          uint32_t status,
                          uint32_t fail_count)
{
    g_progress_staging.completed_units = completed_units;
    g_progress_staging.current_stream = current_stream;
    g_progress_staging.current_channel = current_channel;
    g_progress_staging.current_cap_range = current_cap_range;
    g_progress_staging.status = status;
    g_progress_staging.fail_count = fail_count;
    publish_progress();
}

void nclp_progress_advance(uint32_t current_stream,
                           uint32_t current_channel,
                           uint32_t current_cap_range,
                           uint32_t status,
                           uint32_t fail_count)
{
    uint32_t completed = g_progress_staging.completed_units;

    if (completed < g_progress_staging.total_units) {
        completed++;
    }
    nclp_progress_update(completed, current_stream, current_channel,
                         current_cap_range, status, fail_count);
}

void nclp_progress_set_state(uint32_t state, uint32_t status,
                             uint32_t fail_count)
{
    g_progress_staging.state = state;
    g_progress_staging.status = status;
    g_progress_staging.fail_count = fail_count;
    publish_progress();
}

void nclp_progress_finish(uint32_t state, uint32_t status,
                          uint32_t fail_count)
{
    g_progress_staging.state = state;
    g_progress_staging.status = status;
    g_progress_staging.current_stream = 0xFFFFFFFFU;
    g_progress_staging.current_channel = 0xFFFFFFFFU;
    g_progress_staging.current_cap_range = 0xFFFFFFFFU;
    g_progress_staging.fail_count = fail_count;
    publish_progress();
}
