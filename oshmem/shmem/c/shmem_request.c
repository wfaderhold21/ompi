/*
 * Copyright (c) 2024      NVIDIA Corporation. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
#include "oshmem_config.h"

#include <stdlib.h>

#include "oshmem/constants.h"
#include "oshmem/include/shmem.h"
#include "oshmem/include/shmemx.h"

#include "oshmem/runtime/runtime.h"

#if OSHMEM_PROFILING
#include "oshmem/include/pshmem.h"
#pragma weak shmem_req_test = pshmem_req_test
#pragma weak shmem_req_wait = pshmem_req_wait
#include "oshmem/shmem/c/profile-defines.h"
#endif

int shmem_req_test(shmem_req_h *request)
{
    int ret;

    RUNTIME_CHECK_INIT();

    if (NULL == request || NULL == *request) {
        return OSHMEM_ERROR;
    }

    ret = (*request)->test((*request)->ctx);
    if (ret <= 0) {
        if (NULL != (*request)->release) {
            (*request)->release((*request)->ctx);
        }
        free(*request);
        *request = SHMEM_REQ_INVALID;
    }
    return ret;
}

int shmem_req_wait(shmem_req_h *request)
{
    int ret;

    RUNTIME_CHECK_INIT();

    if (NULL == request || NULL == *request) {
        return OSHMEM_ERROR;
    }

    ret = (*request)->wait((*request)->ctx);
    if (ret <= 0) {
        if (NULL != (*request)->release) {
            (*request)->release((*request)->ctx);
        }
        free(*request);
        *request = SHMEM_REQ_INVALID;
    }
    return ret;
}
