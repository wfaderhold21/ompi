/*
 * Copyright (c) 2013-2016 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

#include "oshmem_config.h"

#include "oshmem/constants.h"
#include "oshmem/mca/scoll/scoll.h"
#include "oshmem/mca/scoll/base/base.h"
#include "oshmem/runtime/runtime.h"
#include "scoll_basic.h"

pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t progress_cond = PTHREAD_COND_INITIALIZER;
static opal_list_t pending_requests;
static pthread_t progress_thread_id;
static bool progress_thread_started;
static bool progress_thread_stop;
static bool pending_requests_constructed;

static void *progress_thread(void *args);
static void progress_nb_ctx(scoll_basic_nb_ctx_t *ctx);

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
        free(ctx->nb_coll->handles);
        free(ctx->nb_coll);
        ctx->nb_coll = NULL;
    }
}

OBJ_CLASS_INSTANCE(scoll_basic_nb_ctx_t, opal_object_t, scoll_basic_nb_ctx_construct, scoll_basic_nb_ctx_destruct);

/*
 * Initial query function that is invoked during initialization, allowing
 * this module to indicate what level of thread support it provides.
 */
int mca_scoll_basic_init(bool enable_progress_threads, bool enable_threads)
{
    (void)enable_progress_threads;
    (void)enable_threads;

    OBJ_CONSTRUCT(&pending_requests, opal_list_t);
    pending_requests_constructed = true;
    progress_thread_stop = false;

    /*
     * A background collective invokes SPML/UCX concurrently with the
     * application.  That is only legal when the user requested
     * SHMEM_THREAD_MULTIPLE.  At lower thread levels enqueue_nb_coll()
     * executes the operation synchronously, preserving correctness while
     * still returning a valid, already-completed request.
     */
    if (oshmem_mpi_thread_provided == SHMEM_THREAD_MULTIPLE) {
        int ret = pthread_create(&progress_thread_id, NULL, progress_thread, NULL);
        if (ret != 0) {
            SCOLL_ERROR("Failed to create progress thread");
            OBJ_DESTRUCT(&pending_requests);
            pending_requests_constructed = false;
            return OSHMEM_ERROR;
        }
        progress_thread_started = true;
    }
    return OSHMEM_SUCCESS;
}

int mca_scoll_basic_finalize(void)
{
    pending_request_item_t *item;

    if (!pending_requests_constructed) {
        return OSHMEM_SUCCESS;
    }

    if (progress_thread_started) {
        pthread_mutex_lock(&queue_lock);
        progress_thread_stop = true;
        pthread_cond_broadcast(&progress_cond);
        pthread_mutex_unlock(&queue_lock);

        pthread_join(progress_thread_id, NULL);
        progress_thread_started = false;
    }

    /*
     * The worker drains queued operations before exiting.  Keep this cleanup
     * defensive for initialization failures or an incomplete caller.
     */
    while (NULL !=
           (item = (pending_request_item_t *)opal_list_remove_first(&pending_requests))) {
        if (NULL != item->ctx) {
            item->ctx->status = SHMEM_NB_COLL_ERROR;
            OBJ_RELEASE(item->ctx);
        }
        OBJ_RELEASE(item);
    }

    OBJ_DESTRUCT(&pending_requests);
    pending_requests_constructed = false;
    progress_thread_stop = false;
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
    (void)module;
    (void)comm;
    /*nothing to do here*/
    return OSHMEM_SUCCESS;
}

int scoll_basic_nb_req_test(void *ctx)
{
    scoll_basic_nb_ctx_t *nb_ctx = (scoll_basic_nb_ctx_t *)ctx;
    int status;

    if (nb_ctx == NULL) {
        return -1;  /* invalid request */
    }

    status = nb_ctx->status;
    if (status == SHMEM_NB_COLL_COMPLETE) {
        return 0;  /* it's complete */
    }
    if (status == SHMEM_NB_COLL_ERROR) {
        return -1;  /* error */
    }

    opal_progress();
    return 1;
}

int scoll_basic_nb_req_wait(void *ctx)
{
    scoll_basic_nb_ctx_t *nb_ctx = (scoll_basic_nb_ctx_t *)ctx;
    int status;
    
    if (nb_ctx == NULL) {
        return -1;  /* invalid request */
    }

    while (SHMEM_NB_COLL_COMPLETE != (status = nb_ctx->status) &&
           SHMEM_NB_COLL_ERROR != status) {
        sched_yield();
    }
    return (status == SHMEM_NB_COLL_COMPLETE) ? 0 : -1;
}

void scoll_basic_nb_req_release(void *ctx)
{
    if (NULL != ctx) {
        scoll_basic_nb_ctx_t *nb_ctx = (scoll_basic_nb_ctx_t *)ctx;
        OBJ_RELEASE(nb_ctx);
    }
}

mca_scoll_base_module_t *
mca_scoll_basic_query(struct oshmem_group_t *group, int *priority)
{
    mca_scoll_basic_module_t *module;

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
        module->super.scoll_sync_nb = mca_scoll_basic_sync_nb;
        module->super.scoll_scan = mca_scoll_basic_scan;
        module->super.scoll_module_enable = mca_scoll_basic_enable;

        module->pSync = NULL;
        module->nr_colls = 0;

        return &(module->super);
    }


    return NULL;
}

void enqueue_nb_coll(scoll_basic_nb_ctx_t *ctx)
{
    pending_request_item_t *list_item;

    if (!progress_thread_started) {
        progress_nb_ctx(ctx);
        return;
    }

    list_item = OBJ_NEW(pending_request_item_t);
    if (NULL == list_item) {
        ctx->status = SHMEM_NB_COLL_ERROR;
        return;
    }

    /* The queue owns a reference until the worker removes the item. */
    OBJ_RETAIN(ctx);
    list_item->ctx = ctx;

    pthread_mutex_lock(&queue_lock);
    opal_list_append(&pending_requests, &list_item->super);
    pthread_cond_signal(&progress_cond);
    pthread_mutex_unlock(&queue_lock);
}

static void progress_nb_ctx(scoll_basic_nb_ctx_t *ctx)
{
    nb_coll_t *nb;
    long *pSync;
    int ret;

    if (NULL == ctx || NULL == (nb = ctx->nb_coll)) {
        if (NULL != ctx) {
            ctx->status = SHMEM_NB_COLL_ERROR;
        }
        return;
    }

    /*
     * Team sync owns a symmetric buffer whose address is already valid for
     * every PE in that team.  Other nonblocking BASIC collectives continue to
     * use the module's private slot array.
     */
    if (NULL != nb->pSync) {
        pSync = nb->pSync;
    } else if (NULL != nb->module && NULL != nb->module->pSync) {
        pSync = &nb->module->pSync[nb->coll_id % SCOLL_BASIC_NUM_OUTSTANDING];
    } else {
        ctx->status = SHMEM_NB_COLL_ERROR;
        return;
    }

    ctx->status = SHMEM_NB_COLL_RUNNING;
    ret = nb->start(nb->args.group,
                    nb->args.target,
                    nb->args.source,
                    nb->args.alltoall.dst,
                    nb->args.alltoall.sst,
                    nb->args.nlong,
                    nb->args.alltoall.element_size,
                    pSync,
                    ctx);
    if (ret < 0) {
        ctx->status = SHMEM_NB_COLL_ERROR;
        return;
    }

    while (ctx->status == SHMEM_NB_COLL_RUNNING) {
        ret = nb->progress(nb->args.group,
                           nb->args.target,
                           nb->args.source,
                           nb->args.alltoall.dst,
                           nb->args.alltoall.sst,
                           nb->args.nlong,
                           nb->args.alltoall.element_size,
                           pSync,
                           ctx);
        if (ret < 0) {
            ctx->status = SHMEM_NB_COLL_ERROR;
            return;
        }
    }
}

static void *progress_thread(void *args)
{
    pending_request_item_t *item;
    scoll_basic_nb_ctx_t *ctx;

    (void)args;

    while (true) {
        pthread_mutex_lock(&queue_lock);
        while (opal_list_is_empty(&pending_requests) && !progress_thread_stop) {
            pthread_cond_wait(&progress_cond, &queue_lock);
        }

        if (progress_thread_stop && opal_list_is_empty(&pending_requests)) {
            pthread_mutex_unlock(&queue_lock);
            break;
        }

        item = (pending_request_item_t *)opal_list_remove_first(&pending_requests);
        ctx = item->ctx;
        pthread_mutex_unlock(&queue_lock);

        if (NULL == ctx) {
            SCOLL_ERROR("Pending request has no context");
        } else {
            progress_nb_ctx(ctx);
            OBJ_RELEASE(ctx);
        }
        OBJ_RELEASE(item);
    }

    return NULL;
}
