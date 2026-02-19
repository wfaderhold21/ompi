/*
 * Copyright (c) 2021-2026 NVIDIA Corporation.
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "oshmem_config.h"
#include "oshmem/proc/team.h"
#include "oshmem/runtime/runtime.h"
#include "oshmem/mca/scoll/scoll.h"
#include "oshmem/mca/scoll/base/base.h"
#include "oshmem/mca/memheap/memheap.h"
#include "oshmem/mca/memheap/base/base.h"
#include "oshmem/include/shmem.h"

#include "ompi/communicator/communicator.h"

#include <stdlib.h>
#include <string.h>

/*
 * Size of internal sync/work arrays for team-based collectives
 * We use the maximum size needed across all collective operations
 */
#define OSHMEM_TEAM_SYNC_SIZE   _SHMEM_REDUCE_SYNC_SIZE
#define OSHMEM_TEAM_WORK_SIZE   _SHMEM_REDUCE_MIN_WRKDATA_SIZE

/* Predefined teams - use shmem_team_t to match public API */
shmem_team_t oshmem_team_world = NULL;
shmem_team_t oshmem_team_shared = NULL;

/* Static team for TEAM_WORLD */
static oshmem_team_t _oshmem_team_world;

/* Static team for TEAM_SHARED */
static oshmem_team_t _oshmem_team_shared;

/**
 * Allocate team sync and work buffers from symmetric memory
 */
static int oshmem_team_alloc_sync_buffers(oshmem_team_t *team)
{
    size_t sync_bytes;
    size_t work_bytes;
    int i;

    team->sync_size = OSHMEM_TEAM_SYNC_SIZE;
    team->work_size = OSHMEM_TEAM_WORK_SIZE;

    sync_bytes = team->sync_size * sizeof(long);
    work_bytes = team->work_size;

    /* Allocate sync array from symmetric memory */
    team->sync = (long *)shmem_malloc(sync_bytes);
    if (NULL == team->sync) {
        return OSHMEM_ERR_OUT_OF_RESOURCE;
    }

    /* Initialize sync array to _SHMEM_SYNC_VALUE */
    for (i = 0; i < (int)team->sync_size; i++) {
        team->sync[i] = _SHMEM_SYNC_VALUE;
    }

    /* Allocate work array from symmetric memory */
    team->work = shmem_malloc(work_bytes);
    if (NULL == team->work) {
        shmem_free(team->sync);
        team->sync = NULL;
        return OSHMEM_ERR_OUT_OF_RESOURCE;
    }

    memset(team->work, 0, work_bytes);

    return OSHMEM_SUCCESS;
}

/**
 * Free team sync and work buffers
 */
static void oshmem_team_free_sync_buffers(oshmem_team_t *team)
{
    if (team->sync != NULL) {
        shmem_free(team->sync);
        team->sync = NULL;
    }
    if (team->work != NULL) {
        shmem_free(team->work);
        team->work = NULL;
    }
    team->sync_size = 0;
    team->work_size = 0;
}

/**
 * Allocate and initialize a new team structure
 */
static oshmem_team_t *oshmem_team_alloc(void)
{
    oshmem_team_t *team;

    team = (oshmem_team_t *)calloc(1, sizeof(oshmem_team_t));
    if (NULL == team) {
        return NULL;
    }

    team->group = NULL;
    team->parent = NULL;
    team->config.num_contexts = 0;
    team->config_mask = 0;
    team->num_contexts = 0;
    team->flags = 0;
    team->sync = NULL;
    team->work = NULL;
    team->sync_size = 0;
    team->work_size = 0;

    return team;
}

/**
 * Free a group created via oshmem_group_create_from_list
 */
static void oshmem_group_free(oshmem_group_t *group)
{
    if (NULL == group) {
        return;
    }

    mca_scoll_base_group_unselect(group);

    if (group->proc_vpids != NULL) {
        free(group->proc_vpids);
    }

    OBJ_RELEASE(group);
}

/**
 * Free a team structure
 */
static void oshmem_team_free(oshmem_team_t *team)
{
    if (NULL == team) {
        return;
    }

    /* Don't free predefined teams' static storage */
    if (team->flags & OSHMEM_TEAM_FLAG_PREDEFINED) {
        return;
    }

    /* Free sync/work buffers */
    oshmem_team_free_sync_buffers(team);

    /* Free the underlying group (created via oshmem_group_create_from_list) */
    if (team->group != NULL) {
        oshmem_group_free(team->group);
        team->group = NULL;
    }

    free(team);
}

/* Forward declaration */
static oshmem_group_t *oshmem_group_create_from_list(int *world_pes, int count);

/**
 * Create SHMEM_TEAM_SHARED - team of PEs on the same node
 */
static int oshmem_team_create_shared(void)
{
    int *shared_pes = NULL;
    int shared_count = 0;
    int npes, i;

    npes = oshmem_num_procs();

    /* Count PEs on local node */
    for (i = 0; i < npes; i++) {
        if (oshmem_proc_on_local_node(i)) {
            shared_count++;
        }
    }

    if (shared_count == 0) {
        /* At minimum, we should be on our own node */
        return OSHMEM_ERROR;
    }

    /* Build list of shared PEs */
    shared_pes = (int *)malloc(shared_count * sizeof(int));
    if (NULL == shared_pes) {
        return OSHMEM_ERR_OUT_OF_RESOURCE;
    }

    shared_count = 0;
    for (i = 0; i < npes; i++) {
        if (oshmem_proc_on_local_node(i)) {
            shared_pes[shared_count++] = i;
        }
    }

    /* Initialize static team structure */
    memset(&_oshmem_team_shared, 0, sizeof(_oshmem_team_shared));

    /* Create group from the PE list - handles both strided and non-strided cases */
    _oshmem_team_shared.group = oshmem_group_create_from_list(shared_pes, shared_count);

    free(shared_pes);

    if (NULL == _oshmem_team_shared.group) {
        return OSHMEM_ERROR;
    }

    _oshmem_team_shared.parent = NULL;
    _oshmem_team_shared.config.num_contexts = 0;
    _oshmem_team_shared.config_mask = 0;
    _oshmem_team_shared.num_contexts = 0;
    _oshmem_team_shared.flags = OSHMEM_TEAM_FLAG_PREDEFINED;

    /* Allocate sync buffers for TEAM_SHARED */
    if (OSHMEM_SUCCESS != oshmem_team_alloc_sync_buffers(&_oshmem_team_shared)) {
        oshmem_group_free(_oshmem_team_shared.group);
        _oshmem_team_shared.group = NULL;
        return OSHMEM_ERR_OUT_OF_RESOURCE;
    }

    oshmem_team_shared = &_oshmem_team_shared;

    return OSHMEM_SUCCESS;
}

int oshmem_team_init(void)
{
    int rc;

    /* Initialize SHMEM_TEAM_WORLD using the existing oshmem_group_all */
    memset(&_oshmem_team_world, 0, sizeof(_oshmem_team_world));
    _oshmem_team_world.group = oshmem_group_all;
    _oshmem_team_world.parent = NULL;
    _oshmem_team_world.config.num_contexts = 0;
    _oshmem_team_world.config_mask = 0;
    _oshmem_team_world.num_contexts = 0;
    _oshmem_team_world.flags = OSHMEM_TEAM_FLAG_PREDEFINED;

    /* Allocate sync buffers for TEAM_WORLD */
    rc = oshmem_team_alloc_sync_buffers(&_oshmem_team_world);
    if (OSHMEM_SUCCESS != rc) {
        return rc;
    }

    oshmem_team_world = &_oshmem_team_world;

    /* Initialize SHMEM_TEAM_SHARED */
    rc = oshmem_team_create_shared();
    if (OSHMEM_SUCCESS != rc) {
        oshmem_team_free_sync_buffers(&_oshmem_team_world);
        oshmem_team_world = NULL;
        return rc;
    }

    return OSHMEM_SUCCESS;
}

int oshmem_team_finalize(void)
{
    /* Free sync buffers for predefined teams */
    if (oshmem_team_shared != NULL) {
        oshmem_team_free_sync_buffers(oshmem_team_shared);
    }
    if (oshmem_team_world != NULL) {
        oshmem_team_free_sync_buffers(oshmem_team_world);
    }

    /* Clean up TEAM_SHARED's group (TEAM_WORLD uses oshmem_group_all) */
    if (oshmem_team_shared != NULL && 
        oshmem_team_shared->group != NULL &&
        oshmem_team_shared->group != oshmem_group_all) {
        /* Note: group destruction is handled by proc_group_finalize */
    }

    oshmem_team_world = NULL;
    oshmem_team_shared = NULL;

    return OSHMEM_SUCCESS;
}

/**
 * Create a group from an explicit list of world PEs
 * This is needed because oshmem_proc_group_create only supports strided patterns
 */
static oshmem_group_t *oshmem_group_create_from_list(int *world_pes, int count)
{
    oshmem_group_t *group;
    int my_pe;
    int i;

    if (count <= 0 || world_pes == NULL) {
        return NULL;
    }

    group = OBJ_NEW(oshmem_group_t);
    if (NULL == group) {
        return NULL;
    }

    group->proc_vpids = (opal_vpid_t *)malloc(count * sizeof(opal_vpid_t));
    if (NULL == group->proc_vpids) {
        OBJ_RELEASE(group);
        return NULL;
    }

    my_pe = oshmem_my_proc_id();
    group->my_pe = my_pe;
    group->is_member = 0;
    group->proc_count = count;

    for (i = 0; i < count; i++) {
        group->proc_vpids[i] = world_pes[i];
        if (world_pes[i] == my_pe) {
            group->is_member = 1;
        }
    }

    group->ompi_comm = NULL;
    group->id = -1;  /* Not registered in global array */

    memset(&group->g_scoll, 0, sizeof(mca_scoll_base_group_scoll_t));

    if (OSHMEM_SUCCESS != mca_scoll_base_select(group)) {
        free(group->proc_vpids);
        OBJ_RELEASE(group);
        return NULL;
    }

    return group;
}

int oshmem_team_create(oshmem_team_t *parent,
                       int start,
                       int stride,
                       int size,
                       const oshmem_team_config_t *config,
                       long config_mask,
                       oshmem_team_t **new_team)
{
    oshmem_team_t *team = NULL;
    oshmem_group_t *group = NULL;
    int *world_pes = NULL;
    int parent_pe;
    int i;

    *new_team = NULL;

    /* Validate parent team */
    if (!oshmem_team_is_valid(parent)) {
        return OSHMEM_ERR_BAD_PARAM;
    }

    /* Validate parameters */
    if (size < 0 || start < 0 || start >= parent->group->proc_count) {
        return OSHMEM_ERR_BAD_PARAM;
    }

    if (size == 0) {
        /* Empty team - return invalid team */
        *new_team = NULL;
        return OSHMEM_SUCCESS;
    }

    /* Build explicit list of world PEs for the new team
     * This handles arbitrary parent teams (including 2D splits) correctly */
    world_pes = (int *)malloc(size * sizeof(int));
    if (NULL == world_pes) {
        return OSHMEM_ERR_OUT_OF_RESOURCE;
    }

    for (i = 0; i < size; i++) {
        if (stride == 0) {
            parent_pe = start;
        } else {
            parent_pe = start + (i * stride);
        }

        if (parent_pe >= parent->group->proc_count) {
            /* PE out of range */
            free(world_pes);
            return OSHMEM_ERR_BAD_PARAM;
        }

        world_pes[i] = oshmem_proc_pe_vpid(parent->group, parent_pe);
    }

    /* Create the underlying group from the PE list */
    group = oshmem_group_create_from_list(world_pes, size);
    free(world_pes);

    if (NULL == group) {
        return OSHMEM_ERR_OUT_OF_RESOURCE;
    }

    /* Check if this PE is a member of the new team */
    if (!oshmem_proc_group_is_member(group)) {
        /* This PE is not in the new team */
        oshmem_group_free(group);
        *new_team = NULL;
        return OSHMEM_SUCCESS;
    }

    /* Allocate team structure */
    team = oshmem_team_alloc();
    if (NULL == team) {
        oshmem_group_free(group);
        return OSHMEM_ERR_OUT_OF_RESOURCE;
    }

    team->group = group;
    team->parent = parent;

    /* Apply configuration */
    if (config != NULL && (config_mask & SHMEM_TEAM_NUM_CONTEXTS)) {
        team->config.num_contexts = config->num_contexts;
        team->config_mask = config_mask;
    }

    /* Allocate sync buffers for team-based collectives */
    if (OSHMEM_SUCCESS != oshmem_team_alloc_sync_buffers(team)) {
        oshmem_group_free(group);
        free(team);
        return OSHMEM_ERR_OUT_OF_RESOURCE;
    }

    *new_team = team;
    return OSHMEM_SUCCESS;
}

void oshmem_team_destroy(oshmem_team_t *team)
{
    if (NULL == team) {
        return;
    }

    /* Cannot destroy predefined teams */
    if (team->flags & OSHMEM_TEAM_FLAG_PREDEFINED) {
        return;
    }

    oshmem_team_free(team);
}

int oshmem_team_translate_pe(oshmem_team_t *src_team,
                             int src_pe,
                             oshmem_team_t *dest_team)
{
    int world_pe;
    int dest_pe;

    /* Validate teams */
    if (!oshmem_team_is_valid(src_team) || !oshmem_team_is_valid(dest_team)) {
        return -1;
    }

    /* Validate source PE */
    if (src_pe < 0 || src_pe >= src_team->group->proc_count) {
        return -1;
    }

    /* Get the world PE number */
    world_pe = oshmem_proc_pe_vpid(src_team->group, src_pe);

    /* Find this PE in the destination team */
    dest_pe = oshmem_proc_group_find_id(dest_team->group, world_pe);

    return dest_pe;
}

int oshmem_team_get_config(oshmem_team_t *team,
                           long config_mask,
                           shmem_team_config_t *config)
{
    if (!oshmem_team_is_valid(team) || config == NULL) {
        return OSHMEM_ERR_BAD_PARAM;
    }

    if (config_mask & SHMEM_TEAM_NUM_CONTEXTS) {
        config->num_contexts = team->config.num_contexts;
    }

    return OSHMEM_SUCCESS;
}

int oshmem_team_split_2d(oshmem_team_t *parent_team,
                         int xrange,
                         const shmem_team_config_t *xaxis_config,
                         long xaxis_mask,
                         oshmem_team_t **xaxis_team,
                         const shmem_team_config_t *yaxis_config,
                         long yaxis_mask,
                         oshmem_team_t **yaxis_team)
{
    int my_pe, npes;
    int row, col;
    int yrange;
    int rc;

    *xaxis_team = NULL;
    *yaxis_team = NULL;

    if (!oshmem_team_is_valid(parent_team)) {
        return OSHMEM_ERR_BAD_PARAM;
    }

    if (xrange < 1) {
        return OSHMEM_ERR_BAD_PARAM;
    }

    npes = parent_team->group->proc_count;
    my_pe = oshmem_team_my_pe(parent_team);

    /* Calculate Y range (number of rows) */
    yrange = (npes + xrange - 1) / xrange;

    /* Calculate my position in the 2D grid */
    row = my_pe / xrange;
    col = my_pe % xrange;

    /*
     * X-axis team: PEs in the same row
     * Row 'row' contains PEs: row*xrange, row*xrange+1, ..., min((row+1)*xrange-1, npes-1)
     */
    {
        int x_start = row * xrange;
        int x_size = xrange;

        /* Adjust size for the last row which may be partial */
        if (x_start + x_size > npes) {
            x_size = npes - x_start;
        }

        /* X-axis team: start at x_start, stride 1, size x_size */
        rc = oshmem_team_create(parent_team, x_start, 1, x_size,
                                (const oshmem_team_config_t *)xaxis_config,
                                xaxis_mask, xaxis_team);
        if (OSHMEM_SUCCESS != rc) {
            return rc;
        }
    }

    /*
     * Y-axis team: PEs in the same column
     * Column 'col' contains PEs: col, col+xrange, col+2*xrange, ...
     */
    {
        int y_start = col;
        int y_size = yrange;

        /* Adjust size if column extends beyond npes */
        while (y_start + (y_size - 1) * xrange >= npes) {
            y_size--;
        }

        /* Y-axis team: start at y_start (col), stride xrange, size y_size */
        rc = oshmem_team_create(parent_team, y_start, xrange, y_size,
                                (const oshmem_team_config_t *)yaxis_config,
                                yaxis_mask, yaxis_team);
        if (OSHMEM_SUCCESS != rc) {
            oshmem_team_destroy(*xaxis_team);
            *xaxis_team = NULL;
            return rc;
        }
    }

    return OSHMEM_SUCCESS;
}

int oshmem_team_sync(oshmem_team_t *team)
{
    if (!oshmem_team_is_valid(team)) {
        return OSHMEM_ERR_BAD_PARAM;
    }

    /* Use the group's barrier collective */
    return team->group->g_scoll.scoll_barrier(team->group,
                                               NULL,  /* pSync - NULL for team-based */
                                               SCOLL_DEFAULT_ALG);
}
