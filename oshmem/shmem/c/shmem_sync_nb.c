/*
 * Copyright (c) 2024      NVIDIA Corporation. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
#include "oshmem_config.h"

#include "oshmem/constants.h"
#include "oshmem/include/shmem.h"
#include "oshmem/include/shmemx.h"

#include "oshmem/runtime/runtime.h"
#include "oshmem/mca/scoll/scoll.h"
#include "oshmem/mca/scoll/base/base.h"
#include "oshmem/proc/proc.h"
#include "oshmem/proc/team.h"

#if OSHMEM_PROFILING
#include "oshmem/include/pshmem.h"
#pragma weak shmem_sync_nb      = pshmem_sync_nb
#pragma weak shmem_team_sync_nb = pshmem_team_sync_nb
#include "oshmem/shmem/c/profile-defines.h"
#endif

int shmem_sync_nb(shmem_team_t team, shmem_req_h *request)
{
    oshmem_group_t *group;

    RUNTIME_CHECK_INIT();

    if (NULL == request) {
        return OSHMEM_ERR_BAD_PARAM;
    }
    *request = SHMEM_REQ_INVALID;

    if (!oshmem_team_is_valid(team)) {
        return OSHMEM_ERR_BAD_PARAM;
    }

    group = oshmem_team_get_group(team);
    if (NULL == group->g_scoll.scoll_sync_nb) {
        return OSHMEM_ERR_NOT_IMPLEMENTED;
    }

    return group->g_scoll.scoll_sync_nb(group, team->sync,
                                        SCOLL_DEFAULT_ALG, request);
}

int shmem_team_sync_nb(shmem_team_t team, shmem_req_h *request)
{
    return shmem_sync_nb(team, request);
}
