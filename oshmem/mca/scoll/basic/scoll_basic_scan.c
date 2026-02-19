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
 * Linear scan algorithm:
 * PE 0 starts with its own value
 * PE i receives partial result from PE i-1, combines with its value, sends to PE i+1
 * 
 * For inclusive scan: result[i] = source[0] op source[1] op ... op source[i]
 * For exclusive scan: result[i] = source[0] op source[1] op ... op source[i-1]
 *                     (result[0] = identity element)
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
    void *recv_buf = NULL;
    void *partial = NULL;
    int prev_pe, next_pe;

    SCOLL_VERBOSE(12, "[#%d] Scan algorithm: Linear (inclusive=%d)", 
                  group->my_pe, inclusive);

    /* Allocate temporary buffer for receiving partial results */
    recv_buf = malloc(nlong);
    if (NULL == recv_buf) {
        return OSHMEM_ERR_OUT_OF_RESOURCE;
    }

    /* For exclusive scan, we need another buffer to hold the partial result */
    if (!inclusive) {
        partial = malloc(nlong);
        if (NULL == partial) {
            free(recv_buf);
            return OSHMEM_ERR_OUT_OF_RESOURCE;
        }
    }

    if (my_id == 0) {
        /* PE 0: Initialize with own value for inclusive, identity for exclusive */
        if (inclusive) {
            memcpy(target, source, nlong);
        } else {
            /* For exclusive scan, PE 0 gets identity element (zeros for most ops) */
            memset(target, 0, nlong);
        }

        /* Send our source value to next PE if there is one */
        if (size > 1) {
            next_pe = oshmem_proc_pe_vpid(group, 1);
            /* For exclusive, send our source; for inclusive, send the result */
            if (inclusive) {
                rc = MCA_SPML_CALL(put(oshmem_ctx_default, recv_buf, nlong, 
                                       (void*)source, next_pe));
            } else {
                rc = MCA_SPML_CALL(put(oshmem_ctx_default, recv_buf, nlong,
                                       (void*)source, next_pe));
            }
            MCA_SPML_CALL(fence(oshmem_ctx_default));
            
            /* Signal next PE */
            long sig = 1;
            rc = MCA_SPML_CALL(put(oshmem_ctx_default, pSync, sizeof(long),
                                   &sig, next_pe));
        }
    } else {
        /* Wait for signal from previous PE */
        long wait_val = 1;
        prev_pe = oshmem_proc_pe_vpid(group, my_id - 1);
        
        SCOLL_VERBOSE(14, "[#%d] Waiting for partial result from PE %d",
                      group->my_pe, prev_pe);
        
        rc = MCA_SPML_CALL(wait((void*)pSync, SHMEM_CMP_EQ, &wait_val, SHMEM_LONG));
        if (rc != OSHMEM_SUCCESS) {
            goto cleanup;
        }

        /* recv_buf now contains the partial result from previous PE */
        if (inclusive) {
            /* Inclusive: result = partial_result op my_source */
            memcpy(target, recv_buf, nlong);
            op->o_func.c_fn((void*)source, target, nlong / op->dt_size);
        } else {
            /* Exclusive: my result is just the partial (before my contribution) */
            memcpy(target, recv_buf, nlong);
            /* Compute new partial for next PE: partial op my_source */
            memcpy(partial, recv_buf, nlong);
            op->o_func.c_fn((void*)source, partial, nlong / op->dt_size);
        }

        /* Forward to next PE if not last */
        if (my_id < size - 1) {
            next_pe = oshmem_proc_pe_vpid(group, my_id + 1);
            
            if (inclusive) {
                /* Send our result as the partial */
                rc = MCA_SPML_CALL(put(oshmem_ctx_default, recv_buf, nlong,
                                       target, next_pe));
            } else {
                /* Send the updated partial */
                rc = MCA_SPML_CALL(put(oshmem_ctx_default, recv_buf, nlong,
                                       partial, next_pe));
            }
            MCA_SPML_CALL(fence(oshmem_ctx_default));
            
            /* Signal next PE */
            long sig = 1;
            rc = MCA_SPML_CALL(put(oshmem_ctx_default, pSync, sizeof(long),
                                   &sig, next_pe));
        }
    }

cleanup:
    if (recv_buf) {
        free(recv_buf);
    }
    if (partial) {
        free(partial);
    }

    /* Restore pSync */
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
