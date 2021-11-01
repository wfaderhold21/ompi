/*
 * Copyright (c) 2021      Mellanox Technologies. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef MCA_SCOLL_UCC_H
#define MCA_SCOLL_UCC_H

#include "oshmem_config.h"

#include "shmem.h"
#include "oshmem/mca/mca.h"
#include "oshmem/mca/scoll/scoll.h"
#include "oshmem/proc/proc.h"

#include "scoll_ucc_debug.h"

#include <ucc/api/ucc.h>

BEGIN_C_DECLS

#define SCOLL_UCC_CTS (UCC_COLL_TYPE_BARRIER | UCC_COLL_TYPE_BCAST | \
                       UCC_COLL_TYPE_ALLREDUCE | UCC_COLL_TYPE_ALLGATHER | \
                       UCC_COLL_TYPE_ALLTOALL)

#define SCOLL_UCC_CTS_STR "barrier,broadcast,reduce,collect,alltoall"

int mca_scoll_ucc_progress(void);

#define SCOLL_UCC_UNINITIALIZED     0
#define SCOLL_UCC_REG_PROGRESS      1
#define SCOLL_UCC_INITIALIZED       2

int mca_scoll_ucc_init_ctx(oshmem_group_t *osh_group);

/**
 * Globally exported structure
 */
struct mca_scoll_ucc_component_t {
    mca_scoll_base_component_1_0_0_t super;
    int ucc_priority;
    int ucc_verbose;
    int ucc_enable;
    int ucc_np;
    char * cls;
    char * cts;
    int nr_modules;
    int libucc_state;
    ucc_lib_h ucc_lib;
    ucc_lib_attr_t ucc_lib_attr;
    ucc_coll_type_t cts_requested;
    ucc_context_h ucc_context;
};
typedef struct mca_scoll_ucc_component_t mca_scoll_ucc_component_t;

OMPI_MODULE_DECLSPEC extern mca_scoll_ucc_component_t mca_scoll_ucc_component;

/**
 * UCC enabled team
 */
struct mca_scoll_ucc_module_t {
    mca_scoll_base_module_t super;

    oshmem_group_t             *group;
    ucc_team_h                  ucc_team;
    long *                      pSync;
    
    /* Saved handlers - for fallback */
    mca_scoll_base_module_reduce_fn_t previous_reduce;
    mca_scoll_base_module_t *previous_reduce_module;
    mca_scoll_base_module_broadcast_fn_t previous_broadcast;
    mca_scoll_base_module_t *previous_broadcast_module;
    mca_scoll_base_module_barrier_fn_t previous_barrier;
    mca_scoll_base_module_t *previous_barrier_module;
    mca_scoll_base_module_collect_fn_t previous_collect;
    mca_scoll_base_module_t *previous_collect_module;
    mca_scoll_base_module_alltoall_fn_t previous_alltoall;
    mca_scoll_base_module_t *previous_alltoall_module;
    mca_scoll_base_module_alltoall_nb_fn_t previous_alltoall_nb;
    mca_scoll_base_module_t *previous_alltoall_nb_module;
};
typedef struct mca_scoll_ucc_module_t mca_scoll_ucc_module_t;

OBJ_CLASS_DECLARATION(mca_scoll_ucc_module_t);

/* API functions */
int mca_scoll_ucc_init_query(bool enable_progress_threads, bool enable_mpi_threads);

mca_scoll_base_module_t* mca_scoll_ucc_comm_query(oshmem_group_t *osh_group, int *priority);

int mca_scoll_ucc_barrier(struct oshmem_group_t *group, long *pSync, int alg);

int mca_scoll_ucc_broadcast(struct oshmem_group_t *group,
                            int PE_root,
                            void *target,
                            const void *source,
                            size_t nlong,
                            long *pSync,
                            bool nlong_type,
                            int alg);

int mca_scoll_ucc_collect(struct oshmem_group_t *group,
                          void *target,
                          const void *source,
                          size_t nlong,
                          long *pSync,
                          bool nlong_type,
                          int alg);

int mca_scoll_ucc_reduce(struct oshmem_group_t *group,
                         struct oshmem_op_t *op,
                         void *target,
                         const void *source,
                         size_t nlong,
                         long *pSync,
                         void *pWrk,
                         int alg);

int mca_scoll_ucc_alltoall(struct oshmem_group_t *group,
                           void *target,
                           const void *source,
                           ptrdiff_t dst, ptrdiff_t sst,
                           size_t nelems,
                           size_t element_size,
                           long *pSync,
                           int alg);

int mca_scoll_ucc_alltoall_nb(struct oshmem_group_t *group,
                           void *target,
                           const void *source,
                           ptrdiff_t dst, ptrdiff_t sst,
                           size_t nelems,
                           size_t element_size,
                           long *pSync,
                           int alg,
                           uint32_t tag,
                           shmem_req_h * req);


END_C_DECLS

#endif
