/*
 * Copyright (c) 2024      NVIDIA Corporation. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef OSHMEM_SCOLL_BASE_REQUEST_H
#define OSHMEM_SCOLL_BASE_REQUEST_H

#include "oshmem/request/request.h"

/*
 * Base request type for nonblocking SCOLL operations.
 * Provider-specific request structs embed this as their first member.
 * The progress_fn is called by shmem_req_test/shmem_req_wait to drive
 * completion; it should call oshmem_request_complete() when done and
 * return 1 on completion, 0 if still in progress, negative on error.
 */
typedef struct scoll_coll_request_t {
    oshmem_request_t super;
    int (*progress_fn)(struct scoll_coll_request_t *req);
} scoll_coll_request_t;

static inline void scoll_coll_request_init(scoll_coll_request_t *req)
{
    OSHMEM_REQUEST_INIT(&req->super, false);
    req->super.req_type         = OSHMEM_REQUEST_COLL;
    req->super.req_state        = OSHMEM_REQUEST_ACTIVE;
    req->super.req_f_to_c_index = SHMEM_UNDEFINED;
    req->super.req_status.SHMEM_ERROR = OSHMEM_SUCCESS;
    req->super.req_complete_cb  = NULL;
    req->super.req_cancel       = NULL;
    req->progress_fn            = NULL;
}

#endif /* OSHMEM_SCOLL_BASE_REQUEST_H */
