/*
 * Copyright (c) 2013-2016 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include <stdio.h>

#include "oshmem_config.h"

#include "oshmem/constants.h"
#include "oshmem/mca/scoll/scoll.h"
#include "oshmem/mca/scoll/base/base.h"
#include "scoll_basic.h"

pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t progress_cond = PTHREAD_COND_INITIALIZER;
static opal_list_t pending_requests;

void * progress_thread(void *args);

/* Custom list item type for pending requests */
typedef struct {
    opal_list_item_t super;
    scoll_basic_nb_ctx_t *ctx;
} pending_request_item_t;
OBJ_CLASS_DECLARATION(pending_request_item_t);

static void pending_request_item_construct(pending_request_item_t *item)
{
    item->ctx = NULL;
}
OBJ_CLASS_INSTANCE(pending_request_item_t, opal_list_item_t, pending_request_item_construct, NULL);

static void scoll_basic_nb_ctx_construct(scoll_basic_nb_ctx_t *ctx)
{
    ctx->status = SHMEM_NB_COLL_BLOCKED;
    ctx->nb_coll = NULL;
}

static void scoll_basic_nb_ctx_destruct(scoll_basic_nb_ctx_t *ctx)
{
    if (ctx->nb_coll) {
        OBJ_RELEASE(ctx->nb_coll);
    }
}

OBJ_CLASS_INSTANCE(scoll_basic_nb_ctx_t, opal_object_t, scoll_basic_nb_ctx_construct, scoll_basic_nb_ctx_destruct);

/*
 * Initial query function that is invoked during initialization, allowing
 * this module to indicate what level of thread support it provides.
 */
int mca_scoll_basic_init(bool enable_progress_threads, bool enable_threads)
{
    OBJ_CONSTRUCT(&pending_requests, opal_list_t);
 
    if (1 || enable_progress_threads) {
        pthread_t thread;
        int ret = pthread_create(&thread, NULL, progress_thread, NULL);
        if (ret != 0) {
            SCOLL_ERROR("Failed to create progress thread");
            return OSHMEM_ERROR;
        }
        /* Detach the thread since we don't need to join it */
        pthread_detach(thread);
    }
 
    return OSHMEM_SUCCESS;
}

/*
 * Invoked when there's a new communicator that has been created.
 * Look at the communicator and decide which set of functions and
 * priority we want to return.
 */
static int mca_scoll_basic_enable(mca_scoll_base_module_t *module,
                                  struct oshmem_group_t *comm)
{
    /*nothing to do here*/
    return OSHMEM_SUCCESS;
}

int scoll_basic_nb_req_test(void *ctx)
{
    scoll_basic_nb_ctx_t *nb_ctx = (scoll_basic_nb_ctx_t *)ctx;
    nb_coll_t *nb;

    if (nb_ctx == NULL) {
        return -1;  /* invalid request */
    }

    if (nb_ctx->status == SHMEM_NB_COLL_COMPLETE) {
        return 0;  /* it's complete */
    }
    if (nb_ctx->status == SHMEM_NB_COLL_ERROR) {
        return -1;  /* error */
    }

    opal_progress();
    return 1;
}

int scoll_basic_nb_req_wait(void *ctx)
{
    scoll_basic_nb_ctx_t *nb_ctx = (scoll_basic_nb_ctx_t *)ctx;
    
    if (nb_ctx == NULL) {
        return -1;  /* invalid request */
    }

    if (nb_ctx->status == SHMEM_NB_COLL_COMPLETE) {
        return 0;  /* it's complete */
    }
    if (nb_ctx->status == SHMEM_NB_COLL_ERROR) {
        return -1;  /* error */
    }
    
    while (nb_ctx->status != SHMEM_NB_COLL_COMPLETE && nb_ctx->status != SHMEM_NB_COLL_ERROR) {
        sched_yield();
        //opal_progress();
    }
    return nb_ctx->status;
}

mca_scoll_base_module_t *
mca_scoll_basic_query(struct oshmem_group_t *group, int *priority)
{
    mca_scoll_basic_module_t *module;
    long *pSync;

    *priority = mca_scoll_basic_priority_param;

    module = OBJ_NEW(mca_scoll_basic_module_t);
    if (module) {
        module->super.scoll_barrier = mca_scoll_basic_barrier;
        module->super.scoll_broadcast = mca_scoll_basic_broadcast;
        module->super.scoll_collect = mca_scoll_basic_collect;
        module->super.scoll_reduce = mca_scoll_basic_reduce;
        module->super.scoll_alltoall = mca_scoll_basic_alltoall;
        module->super.scoll_alltoall_nb = mca_scoll_basic_alltoall_nb;
        module->super.scoll_broadcast_nb = mca_scoll_basic_broadcast_nb;
        module->super.scoll_module_enable = mca_scoll_basic_enable;

        /*MCA_MEMHEAP_CALL(private_alloc(2 * SCOLL_BASIC_NUM_OUTSTANDING * sizeof(long), (void **)&pSync));
        memset(pSync, 0, 2 * SCOLL_BASIC_NUM_OUTSTANDING * sizeof(long));*/
        module->pSync = pSync;
        module->nr_colls = 0;

        return &(module->super);
    }


    return NULL;
}

void enqueue_nb_coll(scoll_basic_nb_ctx_t *ctx)
{
    pending_request_item_t *list_item = OBJ_NEW(pending_request_item_t);
    list_item->ctx = ctx;
 
    pthread_mutex_lock(&queue_lock);
    opal_list_append(&pending_requests, &list_item->super);
    pthread_cond_signal(&progress_cond);
    pthread_mutex_unlock(&queue_lock);
}

void dequeue_nb_coll(void)
{
    pending_request_item_t *item = (pending_request_item_t *)opal_list_remove_first(&pending_requests);
    if (item) {
        OBJ_RELEASE(item);
    }
}

void * progress_thread(void *args)
{
    const int concurrent = SCOLL_BASIC_NUM_OUTSTANDING / 2;
    int ret;
    pending_request_item_t *item;
    scoll_basic_nb_ctx_t *ctx;
    nb_coll_t *nb;

    while (1) {
        pthread_mutex_lock(&queue_lock);
        while (opal_list_is_empty(&pending_requests)) {
            pthread_cond_wait(&progress_cond, &queue_lock);
        }

        item = (pending_request_item_t *)opal_list_get_first(&pending_requests);
        ctx = item->ctx;
        if (ctx == NULL) {
            SCOLL_VERBOSE(14, "queue is malformed");
            abort();
        }
 
        nb = ctx->nb_coll;
        if (nb == NULL) {
            SCOLL_VERBOSE(14, "queue is malformed");
            abort();
        }
 
        if (ctx->status == SHMEM_NB_COLL_BLOCKED) {
            ctx->status = SHMEM_NB_COLL_RUNNING;
            ret = nb->start(nb->args.group,
                          nb->args.target,
                          nb->args.source,
                          nb->args.alltoall.dst,
                          nb->args.alltoall.sst,
                          nb->args.nlong,
                          nb->args.alltoall.element_size,
                          &nb->module->pSync[nb->coll_id % SCOLL_BASIC_NUM_OUTSTANDING],
                          ctx);
            if (ret < 0) {
                ctx->status = SHMEM_NB_COLL_ERROR;
            }
        } else {
            if (ctx->status == SHMEM_NB_COLL_COMPLETE ||
                ctx->status == SHMEM_NB_COLL_ERROR) {
                dequeue_nb_coll();
            } else {
                nb->progress(nb->args.group,
                           nb->args.target,
                           nb->args.source,
                           nb->args.alltoall.dst,
                           nb->args.alltoall.sst,
                           nb->args.nlong,
                           nb->args.alltoall.element_size,
                           &nb->module->pSync[nb->coll_id % SCOLL_BASIC_NUM_OUTSTANDING],
                           ctx);
            }
        }
        pthread_mutex_unlock(&queue_lock);
    }

    pthread_exit(NULL);
}
