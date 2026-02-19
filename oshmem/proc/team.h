/*
 * Copyright (c) 2021-2026 NVIDIA Corporation.
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
#ifndef OSHMEM_PROC_TEAM_H
#define OSHMEM_PROC_TEAM_H

#include "oshmem_config.h"
#include "oshmem/constants.h"
#include "oshmem/include/shmem.h"
#include "oshmem/proc/proc.h"

BEGIN_C_DECLS

/* Forward declarations */
struct oshmem_team_t;

/* Note: SHMEM_TEAM_INVALID and SHMEM_TEAM_NUM_CONTEXTS are defined in shmem.h */

/**
 * OpenSHMEM team configuration structure
 *
 * This structure holds the configuration options for a team.
 * Currently only num_contexts is defined in the OpenSHMEM specification.
 */
typedef struct oshmem_team_config {
    int num_contexts;   /**< Maximum number of contexts that can be created on this team */
} oshmem_team_config_t;

/**
 * OpenSHMEM team structure
 *
 * This structure wraps oshmem_group_t and adds only the team-specific
 * functionality not already provided by groups:
 * - Team configuration (num_contexts)
 * - Parent team reference for split operations
 * - Context tracking
 */
struct oshmem_team_t {
    /* Underlying group - provides PE membership, collectives, refcounting */
    oshmem_group_t          *group;

    /* Team hierarchy (for split operations) */
    struct oshmem_team_t    *parent;        /**< Parent team (NULL for SHMEM_TEAM_WORLD) */

    /* Team configuration - the only thing groups don't have */
    oshmem_team_config_t    config;         /**< Team configuration (num_contexts) */
    long                    config_mask;    /**< Mask for valid config fields */

    /* Context tracking */
    int                     num_contexts;   /**< Number of contexts created on this team */

    /* Flags */
    uint32_t                flags;

    /*
     * Internal synchronization arrays for team-based collectives
     * These are allocated from symmetric memory and used internally
     * to avoid requiring user-provided pSync/pWrk buffers
     */
    long                    *sync;          /**< pSync array for collectives */
    void                    *work;          /**< pWrk array for reductions */
    size_t                  sync_size;      /**< Size of sync array in longs */
    size_t                  work_size;      /**< Size of work array in bytes */
};
typedef struct oshmem_team_t oshmem_team_t;

/* Team flags */
#define OSHMEM_TEAM_FLAG_PREDEFINED  (1 << 0)  /**< Team is predefined (WORLD, SHARED) */

/* Predefined teams */
OSHMEM_DECLSPEC extern oshmem_team_t *oshmem_team_world;
OSHMEM_DECLSPEC extern oshmem_team_t *oshmem_team_shared;

/**
 * Initialize the team subsystem
 *
 * @retval OSHMEM_SUCCESS  Successfully initialized
 * @retval OSHMEM_ERROR    Initialization failed
 */
OSHMEM_DECLSPEC int oshmem_team_init(void);

/**
 * Finalize the team subsystem
 *
 * @retval OSHMEM_SUCCESS  Successfully finalized
 * @retval OSHMEM_ERROR    Finalization failed
 */
OSHMEM_DECLSPEC int oshmem_team_finalize(void);

/**
 * Create a new team
 *
 * @param[in]  parent       Parent team
 * @param[in]  start        Starting PE in parent team
 * @param[in]  stride       Stride between PEs
 * @param[in]  size         Number of PEs in new team
 * @param[in]  config       Team configuration (may be NULL)
 * @param[in]  config_mask  Mask for valid config fields
 * @param[out] new_team     Newly created team
 *
 * @retval OSHMEM_SUCCESS   Team created successfully
 * @retval OSHMEM_ERROR     Team creation failed
 */
OSHMEM_DECLSPEC int oshmem_team_create(oshmem_team_t *parent,
                                       int start,
                                       int stride,
                                       int size,
                                       const oshmem_team_config_t *config,
                                       long config_mask,
                                       oshmem_team_t **new_team);

/**
 * Destroy a team
 *
 * @param[in] team  Team to destroy
 */
OSHMEM_DECLSPEC void oshmem_team_destroy(oshmem_team_t *team);

/**
 * Get the PE number of the calling PE within the team
 *
 * @param[in] team  The team
 *
 * @return PE number within team, or -1 if not a member
 */
static inline int oshmem_team_my_pe(oshmem_team_t *team)
{
    return (team != NULL && team->group != NULL) ? 
           oshmem_proc_group_find_id(team->group, oshmem_my_proc_id()) : -1;
}

/**
 * Get the number of PEs in the team
 *
 * @param[in] team  The team
 *
 * @return Number of PEs in team, or -1 on error
 */
static inline int oshmem_team_n_pes(oshmem_team_t *team)
{
    return (team != NULL && team->group != NULL) ? team->group->proc_count : -1;
}

/**
 * Translate a PE number from one team to another
 *
 * @param[in] src_team  Source team
 * @param[in] src_pe    PE number in source team
 * @param[in] dest_team Destination team
 *
 * @return PE number in destination team, or -1 if not found
 */
OSHMEM_DECLSPEC int oshmem_team_translate_pe(oshmem_team_t *src_team,
                                             int src_pe,
                                             oshmem_team_t *dest_team);

/**
 * Get the team configuration
 *
 * @param[in]  team         The team
 * @param[in]  config_mask  Mask indicating which config fields to retrieve
 * @param[out] config       Configuration structure to fill
 *
 * @retval OSHMEM_SUCCESS   Success
 * @retval OSHMEM_ERROR     Invalid team or config
 */
OSHMEM_DECLSPEC int oshmem_team_get_config(oshmem_team_t *team,
                                           long config_mask,
                                           shmem_team_config_t *config);

/**
 * Split a team into 2D grid teams
 *
 * @param[in]  parent_team   Parent team to split
 * @param[in]  xrange        Size of X dimension
 * @param[in]  xaxis_config  Configuration for X-axis team (may be NULL)
 * @param[in]  xaxis_mask    Config mask for X-axis team
 * @param[out] xaxis_team    Resulting X-axis team
 * @param[in]  yaxis_config  Configuration for Y-axis team (may be NULL)
 * @param[in]  yaxis_mask    Config mask for Y-axis team
 * @param[out] yaxis_team    Resulting Y-axis team
 *
 * @retval OSHMEM_SUCCESS   Success
 * @retval OSHMEM_ERROR     Split failed
 */
OSHMEM_DECLSPEC int oshmem_team_split_2d(oshmem_team_t *parent_team,
                                         int xrange,
                                         const shmem_team_config_t *xaxis_config,
                                         long xaxis_mask,
                                         oshmem_team_t **xaxis_team,
                                         const shmem_team_config_t *yaxis_config,
                                         long yaxis_mask,
                                         oshmem_team_t **yaxis_team);

/**
 * Synchronize all PEs in a team
 *
 * @param[in] team  The team to synchronize
 *
 * @retval OSHMEM_SUCCESS   Success
 * @retval OSHMEM_ERROR     Sync failed
 */
OSHMEM_DECLSPEC int oshmem_team_sync(oshmem_team_t *team);

/**
 * Check if a team handle is valid
 *
 * @param[in] team  Team to check
 *
 * @return true if valid, false otherwise
 */
static inline bool oshmem_team_is_valid(oshmem_team_t *team)
{
    return (team != NULL) && (team->group != NULL);
}

/**
 * Get the group associated with a team
 *
 * @param[in] team  The team
 *
 * @return Pointer to the underlying group, or NULL on error
 */
static inline oshmem_group_t *oshmem_team_get_group(oshmem_team_t *team)
{
    return (team != NULL) ? team->group : NULL;
}

/**
 * Get the world PE number for a team-local PE
 *
 * @param[in] team    The team
 * @param[in] team_pe PE number within the team
 *
 * @return World PE number, or -1 on error
 */
static inline int oshmem_team_pe_to_world(oshmem_team_t *team, int team_pe)
{
    if (team == NULL || team->group == NULL ||
        team_pe < 0 || team_pe >= team->group->proc_count) {
        return -1;
    }
    return oshmem_proc_pe_vpid(team->group, team_pe);
}

END_C_DECLS

#endif /* OSHMEM_PROC_TEAM_H */
