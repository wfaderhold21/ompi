/*
 * Copyright (c) 2026      NVIDIA Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 * SPDX-License-Identifier: BSD-3-Clause-Open-MPI
 */

#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

#include "oshmem_config.h"
#include "shmem.h"
#include "oshmem/runtime/params.h"
#include "oshmem/mca/spml/spml.h"
#include "oshmem/mca/spml/base/base.h"
#include "spml_gpi_component.h"
#include "oshmem/mca/spml/gpi/spml_gpi.h"

static int mca_spml_gpi_component_register(void);
static int mca_spml_gpi_component_open(void);
static int mca_spml_gpi_component_close(void);
static mca_spml_base_module_t *
mca_spml_gpi_component_init(int *priority,
                            bool enable_progress_threads,
                            bool enable_mpi_threads);
static int mca_spml_gpi_component_fini(void);

mca_spml_base_component_2_0_0_t mca_spml_gpi_component = {

    /* First, the mca_base_component_t struct containing meta
       information about the component itself */

    .spmlm_version = {
        MCA_SPML_BASE_VERSION_2_0_0,

        .mca_component_name            = "gpi",
        .mca_component_major_version   = OSHMEM_MAJOR_VERSION,
        .mca_component_minor_version   = OSHMEM_MINOR_VERSION,
        .mca_component_release_version = OSHMEM_RELEASE_VERSION,
        .mca_open_component            = mca_spml_gpi_component_open,
        .mca_close_component           = mca_spml_gpi_component_close,
        .mca_query_component           = NULL,
        .mca_register_component_params = mca_spml_gpi_component_register
    },
    .spmlm_data = {
        /* The component is checkpoint ready */
        .param_field                   = MCA_BASE_METADATA_PARAM_CHECKPOINT
    },

    .spmlm_init                        = mca_spml_gpi_component_init,
    .spmlm_finalize                    = mca_spml_gpi_component_fini
};
MCA_BASE_COMPONENT_INIT(oshmem, spml, gpi)

mca_spml_gpi_t mca_spml_gpi = {
    .super = {
        .spml_add_procs              = mca_spml_gpi_add_procs,
        .spml_del_procs              = mca_spml_gpi_del_procs,
        .spml_enable                 = mca_spml_gpi_enable,
        .spml_register               = mca_spml_gpi_register,
        .spml_deregister             = mca_spml_gpi_deregister,
        .spml_oob_get_mkeys          = mca_spml_gpi_oob_get_mkeys,
        .spml_ctx_create             = mca_spml_gpi_ctx_create,
        .spml_ctx_destroy            = mca_spml_gpi_ctx_destroy,
        .spml_put                    = mca_spml_gpi_put,
        .spml_put_nb                 = mca_spml_gpi_put_nb,
        .spml_put_signal             = mca_spml_gpi_put_signal,
        .spml_put_signal_nb          = mca_spml_gpi_put_signal_nb,
        .spml_get                    = mca_spml_gpi_get,
        .spml_get_nb                 = mca_spml_gpi_get_nb,
        .spml_recv                   = mca_spml_gpi_recv,
        .spml_send                   = mca_spml_gpi_send,
        .spml_wait                   = mca_spml_gpi_wait,
        .spml_wait_nb                = mca_spml_gpi_wait_nb,
        .spml_wait_until_all         = mca_spml_gpi_wait_until_all,
        .spml_wait_until_any         = mca_spml_gpi_wait_until_any,
        .spml_wait_until_some        = mca_spml_gpi_wait_until_some,
        .spml_wait_until_all_vector  = mca_spml_gpi_wait_until_all_vector,
        .spml_wait_until_any_vector  = mca_spml_gpi_wait_until_any_vector,
        .spml_wait_until_some_vector = mca_spml_gpi_wait_until_some_vector,
        .spml_test                   = mca_spml_gpi_test,
        .spml_test_all               = mca_spml_gpi_test_all,
        .spml_test_any               = mca_spml_gpi_test_any,
        .spml_test_some              = mca_spml_gpi_test_some,
        .spml_test_all_vector        = mca_spml_gpi_test_all_vector,
        .spml_test_any_vector        = mca_spml_gpi_test_any_vector,
        .spml_test_some_vector       = mca_spml_gpi_test_some_vector,
        .spml_team_sync              = mca_spml_gpi_team_sync,
        .spml_team_my_pe             = mca_spml_gpi_team_my_pe,
        .spml_team_n_pes             = mca_spml_gpi_team_n_pes,
        .spml_team_get_config        = mca_spml_gpi_team_get_config,
        .spml_team_translate_pe      = mca_spml_gpi_team_translate_pe,
        .spml_team_split_strided     = mca_spml_gpi_team_split_strided,
        .spml_team_split_2d          = mca_spml_gpi_team_split_2d,
        .spml_team_destroy           = mca_spml_gpi_team_destroy,
        .spml_team_get               = mca_spml_gpi_team_get,
        .spml_team_create_ctx        = mca_spml_gpi_team_create_ctx,
        .spml_team_alltoall          = mca_spml_gpi_team_alltoall,
        .spml_team_alltoalls         = mca_spml_gpi_team_alltoalls,
        .spml_team_broadcast         = mca_spml_gpi_team_broadcast,
        .spml_team_collect           = mca_spml_gpi_team_collect,
        .spml_team_fcollect          = mca_spml_gpi_team_fcollect,
        .spml_team_reduce            = mca_spml_gpi_team_reduce,
        .spml_fence                  = mca_spml_gpi_fence,
        .spml_quiet                  = mca_spml_gpi_quiet,
        .spml_rmkey_unpack           = mca_spml_gpi_rmkey_unpack,
        .spml_rmkey_free             = mca_spml_gpi_rmkey_free,
        .spml_rmkey_ptr              = mca_spml_gpi_rmkey_ptr,
        .spml_memuse_hook            = mca_spml_gpi_memuse_hook,
        .spml_put_all_nb             = mca_spml_gpi_put_all_nb,
        .self                        = (struct mca_spml_base_module_t *)&mca_spml_gpi
    },
    .priority = 5
};

static int mca_spml_gpi_component_register(void)
{
    mca_spml_gpi.priority = 5;
    (void) mca_base_component_var_register(&mca_spml_gpi_component.spmlm_version,
                                           "priority",
                                           "Priority of the gpi spml component",
                                           MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                           OPAL_INFO_LVL_9,
                                           MCA_BASE_VAR_SCOPE_READONLY,
                                           &mca_spml_gpi.priority);
    return OSHMEM_SUCCESS;
}

static int mca_spml_gpi_component_open(void)
{
    return OSHMEM_SUCCESS;
}

static int mca_spml_gpi_component_close(void)
{
    return OSHMEM_SUCCESS;
}

static mca_spml_base_module_t *
mca_spml_gpi_component_init(int *priority,
                            bool enable_progress_threads,
                            bool enable_mpi_threads)
{
    SPML_GPI_VERBOSE(10, "in spml gpi component init, priority=%d,"
                     "enable_progress_threads=%s, enable_mpi_threads=%s",
                     mca_spml_gpi.priority,
                     enable_progress_threads ? "true" : "false",
                     enable_mpi_threads ? "true" : "false");

    /* ucx convention: another selected component with a better (lower)
     * priority vetoes us; never veto on the caller's unset sentinel. */
    if ((*priority >= 0) && (*priority) < mca_spml_gpi.priority) {
        return NULL;
    }
    *priority = mca_spml_gpi.priority;

    if (OSHMEM_SUCCESS != spml_gpi_init()) {
        SPML_GPI_ERROR("gpi spml init failed");
        return NULL;
    }

    return &mca_spml_gpi.super;
}

static int mca_spml_gpi_component_fini(void)
{
    return spml_gpi_fini();
}
