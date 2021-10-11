/**
  Copyright (c) 2021 Mellanox Technologies. All rights reserved.
  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
 */
#include "scoll_ucc.h"
#include "scoll_ucc_dtypes.h"
#include "scoll_ucc_common.h"

#include <ucc/api/ucc.h>

static inline ucc_status_t mca_scoll_ucc_alltoall_init(const void *sbuf, void *rbuf,
                                                       int count, size_t element_size,
                                                       mca_scoll_ucc_module_t * ucc_module,
                                                       ucc_coll_req_h * req)
{
    ucc_datatype_t dt;
    if (element_size == 8) {
        dt = UCC_DT_INT64;
    } else if (element_size == 4) {
        dt = UCC_DT_INT32;
    }
    
    ucc_coll_args_t coll = {
        .mask = 0,
        .coll_type = UCC_COLL_TYPE_ALLTOALL,
        .src.info = {
            .buffer = (void *)sbuf,
            .count = count,
            .datatype = dt,
            .mem_type = UCC_MEMORY_TYPE_UNKNOWN
        },
        .dst.info = {
            .buffer = rbuf,
            .count = count,
            .datatype = dt,
            .mem_type = UCC_MEMORY_TYPE_UNKNOWN
        },
    };


    if (mca_scoll_ucc_component.libucc_state < SCOLL_UCC_INITIALIZED) {
        if (OSHMEM_ERROR == mca_scoll_ucc_init_ctx(ucc_module->group)) {
            return OSHMEM_ERROR;
        }
    }
    if (ucc_module->ucc_team == NULL) {
        if (OSHMEM_ERROR == mca_scoll_ucc_team_create(ucc_module, ucc_module->group)) {
            return OSHMEM_ERROR;
        }
    }

    SCOLL_UCC_REQ_INIT(req, coll, ucc_module);
    return UCC_OK;
fallback:
    return UCC_ERR_NOT_SUPPORTED;
}


int mca_scoll_ucc_alltoall(struct oshmem_group_t *group,
                           void *target,
                           const void *source,
                           ptrdiff_t dst, ptrdiff_t sst,
                           size_t nelems,
                           size_t element_size,
                           long *pSync,
                           int alg)
{
    mca_scoll_ucc_module_t *ucc_module;
    size_t count;
    ucc_coll_req_h req;

    UCC_VERBOSE(3, "running ucc alltoall");
    ucc_module = (mca_scoll_ucc_module_t *) group->g_scoll.scoll_alltoall_module;
    count = nelems * element_size;

    /* Do nothing on zero-length request */
    if (OPAL_UNLIKELY(!nelems)) {
        return OSHMEM_SUCCESS;
    }

    SCOLL_UCC_CHECK(mca_scoll_ucc_alltoall_init(source, target, count, element_size, ucc_module, &req));
    SCOLL_UCC_CHECK(ucc_collective_post(req));
    SCOLL_UCC_CHECK(scoll_ucc_req_wait(req));
    return OSHMEM_SUCCESS;
fallback:
    UCC_VERBOSE(3, "running fallback alltoall");
    return ucc_module->previous_alltoall(group, target, source, dst, sst, nelems, 
                                         element_size, pSync, alg);
}

int req_test(shmem_req_h req)
{
    ucc_coll_req_h request = (ucc_coll_req_h) req->ctx;
    ucc_status_t status;

    status = ucc_collective_test(request);
    if (UCC_OK != status) {
        if (0 > status) {
            UCC_ERROR("ucc_collective_test failed: %s", ucc_status_string(status));
            return -1;
        }
        ucc_context_progress(mca_scoll_ucc_component.ucc_context);
        opal_progress();
        return 0;
    }
    ucc_collective_finalize(request);
    return 1;
}


int mca_scoll_ucc_alltoall_nb(struct oshmem_group_t *group,
                           void *target,
                           const void *source,
                           ptrdiff_t dst, ptrdiff_t sst,
                           size_t nelems,
                           size_t element_size,
                           long *pSync,
                           int alg,
                           uint32_t tag,
                           shmem_req_h * request)
{
    mca_scoll_ucc_module_t *ucc_module;
    size_t count;
    ucc_coll_req_h req;

    UCC_VERBOSE(3, "running ucc alltoall_nb");
    ucc_module = (mca_scoll_ucc_module_t *) group->g_scoll.scoll_alltoall_nb_module;
    count = nelems * element_size;

    /* Do nothing on zero-length request */
    if (OPAL_UNLIKELY(!nelems)) {
        return OSHMEM_SUCCESS;
    }

    SCOLL_UCC_CHECK(mca_scoll_ucc_alltoall_init(source, target, count, element_size, ucc_module, &req));
    SCOLL_UCC_CHECK(ucc_collective_post(req));
    (*request)->test = req_test;
    (*request)->ctx = (void *) req; 
    return OSHMEM_SUCCESS;
fallback:
    UCC_VERBOSE(3, "running fallback alltoall_nb");
    return ucc_module->previous_alltoall_nb(group, target, source, dst, sst, nelems, 
                                         element_size, pSync, alg, tag, request);
}

