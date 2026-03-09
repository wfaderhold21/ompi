/*
 * Copyright (c) 2024      NVIDIA Corporation.
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
#include <string.h>

#include "oshmem/constants.h"
#include "oshmem/op/op.h"
#include "oshmem/mca/spml/spml.h"
#include "oshmem/mca/scoll/scoll.h"
#include "oshmem/mca/scoll/base/base.h"
#include "scoll_basic.h"

/*
 * Linear scan algorithm using symmetric pWrk for passing partial results.
 * PE 0 starts with its own value; PE i receives partial in pWrk from PE i-1.
 *
 * For inclusive scan: result[i] = source[0] op source[1] op ... op source[i]
 * For exclusive scan: result[i] = source[0] op source[1] op ... op source[i-1]
 */
static int _algorithm_linear(struct oshmem_group_t *group,
                             struct oshmem_op_t *op,
                             void *target,
                             const void *source,
                             size_t nlong,
                             long *pSync,
                             void *pWrk,
                             bool inclusive)
{
    int rc = OSHMEM_SUCCESS;
    int my_id = oshmem_proc_group_find_id(group, group->my_pe);
    int size = group->proc_count;
    int prev_pe, next_pe;
    void *partial = NULL;

    SCOLL_VERBOSE(12, "[#%d] Scan algorithm: Linear (inclusive=%d)",
                  group->my_pe, inclusive);

    if (NULL == pWrk) {
        return OSHMEM_ERR_BAD_PARAM;
    }

    /* Exclusive scan needs a temp for partial op my_source before sending */
    if (!inclusive && size > 1) {
        partial = malloc(nlong);
        if (NULL == partial) {
            return OSHMEM_ERR_OUT_OF_RESOURCE;
        }
    }

    if (my_id == 0) {
        if (inclusive) {
            memcpy(target, source, nlong);
        } else {
            memset(target, 0, nlong);
        }
        if (size > 1) {
            next_pe = oshmem_proc_pe_vpid(group, 1);
            /* Put our source into next PE's pWrk (symmetric) */
            rc = MCA_SPML_CALL(put(oshmem_ctx_default, pWrk, nlong,
                                   (void*)source, next_pe));
            MCA_SPML_CALL(fence(oshmem_ctx_default));
            long sig = 1;
            rc = MCA_SPML_CALL(put(oshmem_ctx_default, pSync, sizeof(long),
                                   &sig, next_pe));
        }
    } else {
        long wait_val = 1;
        prev_pe = oshmem_proc_pe_vpid(group, my_id - 1);
        rc = MCA_SPML_CALL(wait((void*)pSync, SHMEM_CMP_EQ, &wait_val, SHMEM_LONG));
        if (rc != OSHMEM_SUCCESS) {
            if (partial) free(partial);
            if (pSync) pSync[0] = _SHMEM_SYNC_VALUE;
            return rc;
        }
        /* Partial result from previous PE is in pWrk */
        if (inclusive) {
            memcpy(target, pWrk, nlong);
            op->o_func.c_fn((void*)source, target, nlong / op->dt_size);
        } else {
            memcpy(target, pWrk, nlong);
            memcpy(partial, pWrk, nlong);
            op->o_func.c_fn((void*)source, partial, nlong / op->dt_size);
        }
        if (my_id < size - 1) {
            next_pe = oshmem_proc_pe_vpid(group, my_id + 1);
            rc = MCA_SPML_CALL(put(oshmem_ctx_default, pWrk, nlong,
                                   inclusive ? target : partial, next_pe));
            MCA_SPML_CALL(fence(oshmem_ctx_default));
            long sig = 1;
            rc = MCA_SPML_CALL(put(oshmem_ctx_default, pSync, sizeof(long),
                                   &sig, next_pe));
        }
        if (partial) {
            free(partial);
        }
    }

    if (pSync) {
        pSync[0] = _SHMEM_SYNC_VALUE;
    }
    return rc;
}

int mca_scoll_basic_scan(struct oshmem_group_t *group,
                         struct oshmem_op_t *op,
                         void *target,
                         const void *source,
                         size_t nlong,
                         long *pSync,
                         void *pWrk,
                         bool inclusive,
                         int alg)
{
    int rc = OSHMEM_SUCCESS;

    /* Arguments validation */
    if (!group) {
        SCOLL_ERROR("Active set (group) of PE is not defined");
        rc = OSHMEM_ERR_BAD_PARAM;
    }

    /* Check if this PE is part of the group */
    if ((rc == OSHMEM_SUCCESS) && oshmem_proc_group_is_member(group)) {
        /* Do nothing on zero-length request */
        if (OPAL_UNLIKELY(!nlong)) {
            return OSHMEM_SUCCESS;
        }

        /* For team-based scan (pSync == NULL), allocate internal sync */
        long internal_sync = _SHMEM_SYNC_VALUE;
        long *sync_ptr = pSync ? pSync : &internal_sync;

        rc = _algorithm_linear(group, op, target, source, nlong,
                               sync_ptr, pWrk, inclusive);
    }

    return rc;
}
