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
#include "oshmem/mca/scoll/scoll.h"
#include "oshmem/mca/scoll/base/base.h"
#include "oshmem/include/shmemx.h"
#include "scoll_basic.h"

/*
 * Context for a basic nonblocking sync operation.
 * The progress thread runs a blocking barrier on behalf of the initiator.
 */
typedef struct {
    scoll_basic_nb_ctx_t base;   /* must be first: enqueue_nb_coll casts to this */
    struct oshmem_group_t *group;
    long                  *pSync;
    int                    alg;
} scoll_basic_sync_nb_ctx_t;

static int scoll_basic_sync_nb_test(void *ctx)
{
    scoll_basic_nb_ctx_t *nb_ctx = (scoll_basic_nb_ctx_t *)ctx;

    if (NULL == nb_ctx) {
        return -1;
    }
    if (nb_ctx->status == SHMEM_NB_COLL_COMPLETE) {
        return 0;
    }
    if (nb_ctx->status == SHMEM_NB_COLL_ERROR) {
        return -1;
    }
    opal_progress();
    return 1;
}

static int scoll_basic_sync_nb_wait(void *ctx)
{
    scoll_basic_nb_ctx_t *nb_ctx = (scoll_basic_nb_ctx_t *)ctx;

    if (NULL == nb_ctx) {
        return -1;
    }
    while (nb_ctx->status != SHMEM_NB_COLL_COMPLETE &&
           nb_ctx->status != SHMEM_NB_COLL_ERROR) {
        sched_yield();
    }
    return (nb_ctx->status == SHMEM_NB_COLL_COMPLETE) ? 0 : -1;
}

/*
 * nb_coll start function: runs the blocking barrier on the progress thread.
 * Parameters beyond group/pSync are unused for sync but the signature must
 * match mca_scoll_basic_start_fn_t.
 */
static int scoll_basic_sync_nb_start(struct oshmem_group_t *group,
                                     void *target,
                                     const void *source,
                                     ptrdiff_t dst, ptrdiff_t sst,
                                     size_t nelems,
                                     size_t element_size,
                                     long *pSync,
                                     void *coll)
{
    scoll_basic_nb_ctx_t *nb_ctx = (scoll_basic_nb_ctx_t *)coll;
    int rc;

    rc = BARRIER_FUNC(group, pSync, SCOLL_DEFAULT_ALG);
    if (OSHMEM_SUCCESS != rc) {
        nb_ctx->status = SHMEM_NB_COLL_ERROR;
        return rc;
    }
    nb_ctx->status = SHMEM_NB_COLL_COMPLETE;
    return OSHMEM_SUCCESS;
}

static int scoll_basic_sync_nb_progress(struct oshmem_group_t *group,
                                        void *target,
                                        const void *source,
                                        ptrdiff_t dst, ptrdiff_t sst,
                                        size_t nelems,
                                        size_t element_size,
                                        long *pSync,
                                        void *coll)
{
    /* barrier runs to completion in start; nothing left to progress */
    return OSHMEM_SUCCESS;
}

int mca_scoll_basic_sync_nb(struct oshmem_group_t *group, long *pSync, int alg,
                            shmem_req_h *request)
{
    mca_scoll_basic_module_t *module;
    nb_coll_t                *coll;
    scoll_basic_nb_ctx_t     *ctx;

    module = (mca_scoll_basic_module_t *) group->g_scoll.scoll_sync_nb_module;

    coll = calloc(1, sizeof(nb_coll_t));
    if (NULL == coll) {
        return OSHMEM_ERR_OUT_OF_RESOURCE;
    }
    coll->start    = scoll_basic_sync_nb_start;
    coll->progress = scoll_basic_sync_nb_progress;
    coll->coll_id  = module->nr_colls++;
    coll->status   = SHMEM_NB_COLL_BLOCKED;
    coll->module   = module;
    coll->pSync    = pSync;

    coll->args.group  = group;
    coll->args.target = NULL;
    coll->args.source = NULL;
    coll->args.nlong  = 0;

    ctx = OBJ_NEW(scoll_basic_nb_ctx_t);
    if (NULL == ctx) {
        free(coll);
        return OSHMEM_ERR_OUT_OF_RESOURCE;
    }
    ctx->nb_coll = coll;
    ctx->status  = SHMEM_NB_COLL_BLOCKED;

    *request = malloc(sizeof(struct shmem_req));
    if (NULL == *request) {
        OBJ_RELEASE(ctx);
        return OSHMEM_ERR_OUT_OF_RESOURCE;
    }
    (*request)->test = scoll_basic_sync_nb_test;
    (*request)->wait = scoll_basic_sync_nb_wait;
    (*request)->release = scoll_basic_nb_req_release;
    (*request)->ctx  = ctx;

    enqueue_nb_coll(ctx);
    return OSHMEM_SUCCESS;
}
