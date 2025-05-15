/*
 * Copyright (c) 2013-2016 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef MCA_SCOLL_BASIC_H
#define MCA_SCOLL_BASIC_H

#include "oshmem_config.h"

#include "oshmem/mca/mca.h"
#include "oshmem/mca/scoll/scoll.h"
#include "oshmem/util/oshmem_util.h"

BEGIN_C_DECLS

/* These functions (BARRIER_FUNC, BCAST_FUNC)  may be called from any basic algorithm.
 * In case of shmem, the implementation of broadcast doesn't require
 * each process to know message size ( just root should know).
 * It differs from other implementations, so it may cause problems if
 * BCAST_FUNC is a callback to another implementation (e.g, fca, hcoll).
 * So we replace a callback (group->g_scoll.scoll_[func])
 * with a corresponding basic function. */

#define BARRIER_FUNC mca_scoll_basic_barrier
#define BCAST_FUNC mca_scoll_basic_broadcast

/* Globally exported variables */

OSHMEM_DECLSPEC extern mca_scoll_base_component_1_0_0_t
mca_scoll_basic_component;

extern int mca_scoll_basic_priority_param;
OSHMEM_DECLSPEC extern int mca_scoll_basic_param_barrier_algorithm;
extern int mca_scoll_basic_param_broadcast_algorithm;
extern int mca_scoll_basic_param_collect_algorithm;
extern int mca_scoll_basic_param_reduce_algorithm;

/* API functions */

int mca_scoll_basic_init(bool enable_progress_threads, bool enable_threads);
mca_scoll_base_module_t*
mca_scoll_basic_query(struct oshmem_group_t *group, int *priority);

#define SCOLL_BASIC_NUM_OUTSTANDING 16

struct mca_scoll_basic_module_t {
    mca_scoll_base_module_t super;

    long *pSync;
    size_t nr_colls;
    size_t nr_current_colls;
    char pSync_bm[SCOLL_BASIC_NUM_OUTSTANDING];
};
typedef struct mca_scoll_basic_module_t mca_scoll_basic_module_t;
OBJ_CLASS_DECLARATION(mca_scoll_basic_module_t);

typedef int (*mca_scoll_basic_start_fn_t)(struct oshmem_group_t *group,
                                  void *target,
                                  const void *source,
                                  ptrdiff_t dst, ptrdiff_t sst,
                                  size_t nelems,
                                  size_t element_size,
                                  long *pSync,
                                  void **coll);

typedef int (*mca_scoll_basic_progress_fn_t)(struct oshmem_group_t *group,
                                  void *target,
                                  const void *source,
                                  ptrdiff_t dst, ptrdiff_t sst,
                                  size_t nelems,
                                  size_t element_size,
                                  long *pSync,
                                  void **coll);

int scoll_basic_nb_req_test(void *ctx);
int scoll_basic_nb_req_wait(void *ctx);

extern pthread_mutex_t queue_lock;

typedef enum {
    SHMEM_NB_COLL_ERROR = -1,
    SHMEM_NB_COLL_COMPLETE,
    SHMEM_NB_COLL_RUNNING,
    SHMEM_NB_COLL_BLOCKED,
} scoll_basic_nb_coll_status;
typedef struct nb_coll nb_coll_t;
typedef struct {
    opal_object_t super;
    scoll_basic_nb_coll_status status;  /* Current status of the non-blocking operation */
    nb_coll_t *nb_coll;                 /* Pointer to the non-blocking collective operation */
} scoll_basic_nb_ctx_t;
OBJ_CLASS_DECLARATION(scoll_basic_nb_ctx_t);

typedef struct nb_coll {
    size_t                        coll_id;
    mca_scoll_basic_module_t     *module;
    scoll_basic_nb_coll_status    status;
    mca_scoll_basic_start_fn_t    start;
    mca_scoll_basic_progress_fn_t progress;
    void                         *quiet_handle;  /* Handle for quiet operation */
    void                        **handles;       /* Array of handles for non-blocking operations */
    struct args {
        struct oshmem_group_t *group;
        void *target;
        const void *source;
        size_t nlong;
        union {
            struct bcast {
                int PE_root;
                bool nlong_type;
            } bcast;
            struct alltoall {
                ptrdiff_t dst;
                ptrdiff_t sst;
                size_t element_size;
            } alltoall;
        };
    } args;
} nb_coll_t;
//OBJ_CLASS_DECLARATION(nb_coll_t);

void enqueue_nb_coll(scoll_basic_nb_ctx_t *ctx);
void dequeue_nb_coll(void);

enum {
    SHMEM_SYNC_INIT = _SHMEM_SYNC_VALUE,
    SHMEM_SYNC_WAIT = -2,
    SHMEM_SYNC_RUN = -3,
    SHMEM_SYNC_READY = -4,
};

int mca_scoll_basic_barrier(struct oshmem_group_t *group, long *pSync, int alg);
int mca_scoll_basic_broadcast(struct oshmem_group_t *group,
                              int PE_root,
                              void *target,
                              const void *source,
                              size_t nlong,
                              long *pSync,
                              bool nlong_type,
                              int alg);
int mca_scoll_basic_collect(struct oshmem_group_t *group,
                            void *target,
                            const void *source,
                            size_t nlong,
                            long *pSync,
                            bool nlong_type,
                            int alg);
int mca_scoll_basic_reduce(struct oshmem_group_t *group,
                           struct oshmem_op_t *op,
                           void *target,
                           const void *source,
                           size_t nlong,
                           long *pSync,
                           void *pWrk,
                           int alg);
int mca_scoll_basic_alltoall(struct oshmem_group_t *group,
                             void *target,
                             const void *source,
                             ptrdiff_t dst, ptrdiff_t sst,
                             size_t nelems,
                             size_t element_size,
                             long *pSync,
                             int alg);

int mca_scoll_basic_alltoall_nb(struct oshmem_group_t *group,
                             void *target,
                             const void *source,
                             ptrdiff_t dst, ptrdiff_t sst,
                             size_t nelems,
                             size_t element_size,
                             long *pSync,
                             int alg,
                             shmem_req_h * request);
int mca_scoll_basic_broadcast_nb(struct oshmem_group_t *group,
                             int PE_root,
                             void *target,
                             const void *source,
                             size_t nelems,
                             long *pSync,
                             bool nlong_type,
                             int alg,
                             shmem_req_h * request);



static inline unsigned int scoll_log2(unsigned long val)
{
    unsigned int count = 0;

    while (val > 0) {
        val = val >> 1;
        count++;
    }

    return count > 0 ? count - 1 : 0;
}

END_C_DECLS

#endif /* MCA_SCOLL_BASIC_H */
