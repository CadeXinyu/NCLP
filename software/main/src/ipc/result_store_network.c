#include "result_store_network.h"

#include "xil_cache.h"

#include <stddef.h>
#include <string.h>

static void result_barrier(void)
{
#if defined(__aarch64__)
    __asm__ volatile("dmb sy" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

static volatile const nclp_result_header_t *result_header(uint32_t type)
{
    volatile const nclp_result_store_t *store =
        (volatile const nclp_result_store_t *)(uintptr_t)
            NCLP_RESULT_BASE_ADDRESS;

    if (type == NCLP_RESULT_TYPE_SCAN) {
        return &store->scan.header;
    }
    if (type == NCLP_RESULT_TYPE_INIT) {
        return &store->init.header;
    }
    if (type == NCLP_RESULT_TYPE_IMPEDANCE) {
        return &store->impedance.header;
    }
    return NULL;
}

static volatile const uint32_t *result_record_words(uint32_t type,
                                                     uint32_t index)
{
    volatile const nclp_result_store_t *store =
        (volatile const nclp_result_store_t *)(uintptr_t)
            NCLP_RESULT_BASE_ADDRESS;

    if (type == NCLP_RESULT_TYPE_SCAN &&
        index < NCLP_RESULT_MAX_SCAN_RECORDS) {
        return (volatile const uint32_t *)&store->scan.records[index];
    }
    if (type == NCLP_RESULT_TYPE_INIT &&
        index < NCLP_RESULT_MAX_INIT_RECORDS) {
        return (volatile const uint32_t *)&store->init.records[index];
    }
    if (type == NCLP_RESULT_TYPE_IMPEDANCE &&
        index < NCLP_RESULT_MAX_IMPEDANCE_RECORDS) {
        return (volatile const uint32_t *)&store->impedance.records[index];
    }
    return NULL;
}

static void copy_header(nclp_result_header_t *destination,
                        volatile const nclp_result_header_t *source)
{
    uint32_t *dst = (uint32_t *)destination;
    volatile const uint32_t *src = (volatile const uint32_t *)source;

    for (uint32_t i = 0U; i < sizeof(*destination) / 4U; ++i) {
        dst[i] = src[i];
    }
}

int nclp_result_read_progress(nclp_progress_t *progress)
{
    volatile const nclp_progress_t *source = &nclp_shared_state()->progress;
    uint32_t *dst;

    if (progress == NULL) {
        return NCLP_RESULT_READ_INVALID;
    }
    dst = (uint32_t *)progress;
    for (uint32_t attempt = 0U; attempt < 3U; ++attempt) {
        uint32_t before;
        uint32_t after;

        Xil_DCacheInvalidateRange((INTPTR)source, sizeof(*source));
        result_barrier();
        before = source->publish_seq;
        if ((before & 1U) != 0U) {
            continue;
        }
        for (uint32_t i = 0U; i < sizeof(*progress) / 4U; ++i) {
            dst[i] = ((volatile const uint32_t *)source)[i];
        }
        result_barrier();
        after = source->publish_seq;
        if (before == after && (after & 1U) == 0U) {
            return NCLP_RESULT_READ_OK;
        }
    }
    return NCLP_RESULT_READ_BUSY;
}

int nclp_result_read(uint32_t type, uint32_t result_id,
                     uint32_t record_index,
                     nclp_result_header_t *header,
                     nclp_result_record_t *record)
{
    volatile const nclp_result_header_t *source = result_header(type);

    if (source == NULL || header == NULL ||
        (record_index != NCLP_RESULT_INDEX_METADATA && record == NULL)) {
        return NCLP_RESULT_READ_INVALID;
    }

    for (uint32_t attempt = 0U; attempt < 3U; ++attempt) {
        volatile const uint32_t *record_words;
        uint32_t before;
        uint32_t after;

        Xil_DCacheInvalidateRange((INTPTR)source, sizeof(*source));
        result_barrier();
        before = source->publish_seq;
        if ((before & 1U) != 0U) {
            continue;
        }
        copy_header(header, source);
        result_barrier();
        after = source->publish_seq;
        if (before != after || (after & 1U) != 0U) {
            continue;
        }
        if (header->magic != NCLP_RESULT_MAGIC ||
            header->version != NCLP_RESULT_VERSION ||
            header->result_type != type || header->result_id == 0U ||
            header->state == NCLP_RESULT_STATE_EMPTY) {
            return NCLP_RESULT_READ_NONE;
        }
        if (result_id != 0U && result_id != header->result_id) {
            return NCLP_RESULT_READ_STALE;
        }
        if (record_index != NCLP_RESULT_INDEX_METADATA) {
            if (record_index >= header->available_records ||
                record_index >= header->total_records) {
                return NCLP_RESULT_READ_RANGE;
            }
            record_words = result_record_words(type, record_index);
            if (record_words == NULL) {
                return NCLP_RESULT_READ_RANGE;
            }
            Xil_DCacheInvalidateRange((INTPTR)record_words,
                                      sizeof(record->words));
            result_barrier();
            for (uint32_t i = 0U; i < 4U; ++i) {
                record->words[i] = record_words[i];
            }
        }
        result_barrier();
        after = source->publish_seq;
        if (before == after &&
            header->result_id == source->result_id) {
            return NCLP_RESULT_READ_OK;
        }
    }
    return NCLP_RESULT_READ_BUSY;
}
