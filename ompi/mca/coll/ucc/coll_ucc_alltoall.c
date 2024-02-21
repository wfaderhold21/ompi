
/**
 * Copyright (c) 2021 Mellanox Technologies. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 */

#include "coll_ucc_common.h"

static int mapped = 0;
static long * pSync;

static inline ucc_status_t mca_coll_ucc_alltoall_init(const void *sbuf, int scount, struct ompi_datatype_t *sdtype,
                                                      void* rbuf, int rcount, struct ompi_datatype_t *rdtype,
                                                      mca_coll_ucc_module_t *ucc_module,
                                                      ucc_coll_req_h *req,
                                                      mca_coll_ucc_req_t *coll_req)
{
    ucc_datatype_t         ucc_sdt, ucc_rdt;
    int comm_size = ompi_comm_size(ucc_module->comm);
    ucc_mem_map_t *map;

    if (!ompi_datatype_is_contiguous_memory_layout(sdtype, scount * comm_size) ||
        !ompi_datatype_is_contiguous_memory_layout(rdtype, rcount * comm_size)) {
        goto fallback;
    }
    ucc_sdt = ompi_dtype_to_ucc_dtype(sdtype);
    ucc_rdt = ompi_dtype_to_ucc_dtype(rdtype);

    ucc_coll_args_t coll = {
        .mask = 0,
        .coll_type = UCC_COLL_TYPE_ALLTOALL,
        .src.info = {
            .buffer   = (void*)sbuf,
            .count    = scount * comm_size,
            .datatype = ucc_sdt,
            .mem_type = UCC_MEMORY_TYPE_UNKNOWN
        },
        .dst.info = {
            .buffer   = (void*)rbuf,
            .count    = rcount * comm_size,
            .datatype = ucc_rdt,
            .mem_type = UCC_MEMORY_TYPE_UNKNOWN
        },
    };

    if (COLL_UCC_DT_UNSUPPORTED == ucc_sdt ||
        COLL_UCC_DT_UNSUPPORTED == ucc_rdt) {
        UCC_VERBOSE(5, "ompi_datatype is not supported: dtype = %s",
                    (COLL_UCC_DT_UNSUPPORTED == ucc_sdt) ?
                    sdtype->super.name : rdtype->super.name);
        goto fallback;
    }

    if (!mapped) {
        map = (ucc_mem_map_t *)calloc(2, sizeof(ucc_mem_map_t));
        pSync = (long *)calloc(1,sizeof(long));

        map[0].address = rbuf;
        map[0].len = rcount * comm_size;
        map[0].resource = NULL;

        map[1].address = pSync;
        map[1].len = sizeof(long);
        map[1].resource = NULL;

        coll.mask = UCC_COLL_ARGS_FIELD_FLAGS | UCC_COLL_ARGS_FIELD_MEM_MAP;
        coll.flags = UCC_COLL_ARGS_FLAG_MEM_MAPPED_BUFFERS;
        coll.mem_map.n_segments = 2;
        coll.mem_map.segments = map;
        mapped = 1;
    }

    if (mapped) {
        coll.mask |= UCC_COLL_ARGS_FIELD_FLAGS | UCC_COLL_ARGS_FIELD_GLOBAL_WORK_BUFFER;
        coll.flags |= UCC_COLL_ARGS_FLAG_MEM_MAPPED_BUFFERS;
        coll.global_work_buffer = pSync;
    }

    if (MPI_IN_PLACE == sbuf) {
        coll.mask  = UCC_COLL_ARGS_FIELD_FLAGS;
        coll.flags = UCC_COLL_ARGS_FLAG_IN_PLACE;
    }
    COLL_UCC_REQ_INIT(coll_req, req, coll, ucc_module);
    return UCC_OK;
fallback:
    return UCC_ERR_NOT_SUPPORTED;
}

int mca_coll_ucc_alltoall(const void *sbuf, int scount, struct ompi_datatype_t *sdtype,
                          void* rbuf, int rcount, struct ompi_datatype_t *rdtype,
                          struct ompi_communicator_t *comm,
                          mca_coll_base_module_t *module)
{
    mca_coll_ucc_module_t *ucc_module = (mca_coll_ucc_module_t*)module;
    ucc_coll_req_h         req;

    UCC_VERBOSE(3, "running ucc alltoall");
    COLL_UCC_CHECK(mca_coll_ucc_alltoall_init(sbuf, scount, sdtype,
                                              rbuf, rcount, rdtype,
                                              ucc_module, &req, NULL));
    COLL_UCC_POST_AND_CHECK(req);
    COLL_UCC_CHECK(coll_ucc_req_wait(req));
    return OMPI_SUCCESS;
fallback:
    UCC_VERBOSE(3, "running fallback alltoall");
    return ucc_module->previous_alltoall(sbuf, scount, sdtype, rbuf, rcount, rdtype,
                                          comm, ucc_module->previous_alltoall_module);
}

int mca_coll_ucc_ialltoall(const void *sbuf, int scount, struct ompi_datatype_t *sdtype,
                           void* rbuf, int rcount, struct ompi_datatype_t *rdtype,
                           struct ompi_communicator_t *comm,
                           ompi_request_t** request,
                           mca_coll_base_module_t *module)
{
    mca_coll_ucc_module_t *ucc_module = (mca_coll_ucc_module_t*)module;
    ucc_coll_req_h         req;
    mca_coll_ucc_req_t    *coll_req = NULL;

    UCC_VERBOSE(3, "running ucc ialltoall");
    COLL_UCC_GET_REQ(coll_req);
    COLL_UCC_CHECK(mca_coll_ucc_alltoall_init(sbuf, scount, sdtype,
                                              rbuf, rcount, rdtype,
                                              ucc_module, &req, coll_req));
    COLL_UCC_POST_AND_CHECK(req);
    *request = &coll_req->super;
    return OMPI_SUCCESS;
fallback:
    UCC_VERBOSE(3, "running fallback ialltoall");
    if (coll_req) {
        mca_coll_ucc_req_free((ompi_request_t **)&coll_req);
    }
    return ucc_module->previous_ialltoall(sbuf, scount, sdtype, rbuf, rcount, rdtype,
                                          comm, request, ucc_module->previous_ialltoall_module);
}
