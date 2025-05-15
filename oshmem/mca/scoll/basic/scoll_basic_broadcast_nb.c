/*
 * Copyright (c) 2013-2016 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "oshmem_config.h"
#include <stdio.h>
#include <stdlib.h>

#include "oshmem/constants.h"
#include "oshmem/mca/spml/spml.h"
#include "oshmem/mca/scoll/scoll.h"
#include "oshmem/mca/scoll/base/base.h"
#include "scoll_basic.h"

int basic_broadcast_nb_progress(struct oshmem_group_t *group,
                             void *target,
                             const void *source,
                             ptrdiff_t dst, ptrdiff_t sst,
                             size_t nelems,
                             size_t element_size,
                             long *pSync,
                             void *coll)
{
    int rc = OSHMEM_SUCCESS;
    scoll_basic_nb_ctx_t *nb_ctx = (scoll_basic_nb_ctx_t *)coll;
    nb_coll_t *nb = nb_ctx->nb_coll;

    /* Progress the SPML component */
    opal_progress();

    /* Use barrier to ensure all PEs have received the data */
    rc = BARRIER_FUNC(group, pSync, SCOLL_DEFAULT_ALG);
    if (rc != OSHMEM_SUCCESS) {
        nb_ctx->status = SHMEM_NB_COLL_ERROR;
        return rc;
    }

    /* If we get here, the operation is complete */
    nb_ctx->status = SHMEM_NB_COLL_COMPLETE;
    return OSHMEM_SUCCESS;
}

int basic_broadcast_nb_start(struct oshmem_group_t *group,
                             void *target,
                             const void *source,
                             ptrdiff_t dst, ptrdiff_t sst,
                             size_t nelems,
                             size_t element_size,
                             long *pSync,
                             void *coll)
{
    int rc;
    int pe_cur;
    scoll_basic_nb_ctx_t *nb_ctx = (scoll_basic_nb_ctx_t *)coll;
    nb_coll_t *nb = nb_ctx->nb_coll;
    void **handles;
    int PE_root = nb->args.bcast.PE_root;

    SCOLL_VERBOSE(14, "[#%d] send data to all PEs in the group", group->my_pe);

    /* Allocate array to store handles */
    handles = calloc(group->proc_count, sizeof(void *));
    if (handles == NULL) {
        nb_ctx->status = SHMEM_NB_COLL_ERROR;
        return OSHMEM_ERR_OUT_OF_RESOURCE;
    }
    nb->handles = handles;  /* Store handles array in coll structure */

    /* Root sends data to all other PEs */
    if (PE_root == group->my_pe) {
        for (int i = 0; i < group->proc_count; i++) {
            pe_cur = oshmem_proc_pe_vpid(group, i);
            if (pe_cur != PE_root) {
                rc = MCA_SPML_CALL(put_nb(oshmem_ctx_default, target, nelems, source, pe_cur, &handles[i]));
                if (OSHMEM_SUCCESS != rc) {
                    free(handles);
                    nb_ctx->status = SHMEM_NB_COLL_ERROR;
                    return rc;
                }
            }
        }
        /* Issue quiet to ensure all puts are ordered */
        rc = MCA_SPML_CALL(quiet(oshmem_ctx_default));
        if (OSHMEM_SUCCESS != rc) {
            free(handles);
            nb_ctx->status = SHMEM_NB_COLL_ERROR;
            return rc;
        }
    }

    return OSHMEM_SUCCESS;
}

int mca_scoll_basic_broadcast_nb(struct oshmem_group_t *group,
                             int PE_root,
                             void *target,
                             const void *source,
                             size_t nelems,
                             long *pSync,
                             bool nlong_type,
                             int alg,
                             shmem_req_h * request)
{
    mca_scoll_basic_module_t *module;
    nb_coll_t *coll;
    scoll_basic_nb_ctx_t *ctx;
    module = (mca_scoll_basic_module_t *) group->g_scoll.scoll_broadcast_nb_module;
    int rc = OSHMEM_SUCCESS;

    if (OPAL_UNLIKELY(!nelems)) {
        *request = SHMEM_REQ_INVALID;
        return rc;
    }

    /* Create the non-blocking collective operation */
    coll = calloc(1, sizeof(nb_coll_t));
    if (NULL == coll) {
        return OSHMEM_ERR_OUT_OF_RESOURCE;
    }
    coll->start = basic_broadcast_nb_start;
    coll->progress = basic_broadcast_nb_progress;
    coll->coll_id = module->nr_colls++;
    coll->status = SHMEM_NB_COLL_BLOCKED;
    coll->module = module;

    coll->args.group = group;
    coll->args.target = target;
    coll->args.source = source;
    coll->args.nlong = nelems;
    coll->args.bcast.PE_root = PE_root;
    coll->args.bcast.nlong_type = nlong_type;

    /* Create the context object */
    ctx = OBJ_NEW(scoll_basic_nb_ctx_t);
    if (NULL == ctx) {
        free(coll);
        return OSHMEM_ERR_OUT_OF_RESOURCE;
    }
    ctx->nb_coll = coll;
    ctx->status = SHMEM_NB_COLL_BLOCKED;

    /* Create the request object */
    *request = malloc(sizeof(struct shmem_req));
    if (NULL == *request) {
        OBJ_RELEASE(ctx);
        return OSHMEM_ERR_OUT_OF_RESOURCE;
    }
    (*request)->test = scoll_basic_nb_req_test;
    (*request)->wait = scoll_basic_nb_req_wait;
    (*request)->ctx = ctx;

    /* Add the request to the pending requests list */
    enqueue_nb_coll(ctx);

    return OSHMEM_SUCCESS;
} 