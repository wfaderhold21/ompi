/*
 *  Copyright (c) 2014      Mellanox Technologies, Inc.
 *                          All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef OSHMEM_UTIL_H
#define OSHMEM_UTIL_H

#include "oshmem_config.h"

#include "opal/util/output.h"
#include "opal/mca/base/base.h"
#include "opal/mca/base/mca_base_framework.h"

/*
 * Environment variables
 */
#define OSHMEM_ENV_SYMMETRIC_SIZE      "SMA_SYMMETRIC_SIZE"
#define OSHMEM_ENV_DEBUG               "SMA_DEBUG"
#define OSHMEM_ENV_INFO                "SMA_INFO"
#define OSHMEM_ENV_VERSION             "SMA_VERSION"


void oshmem_output_verbose(int level, int output_id, const char* prefix,
    const char* file, int line, const char* function, const char* format, ...);

/*
 * Temporary wrapper which ignores output verbosity level
 * to ensure error messages are seeing by user
 */
void oshmem_output(int output_id, const char* prefix, const char* file,
    int line, const char* function, const char* format, ...);


/* Force opening output for framework
 * We would like to display error messages in any case (debug/release mode,
 * set/unset verbose level)
 * Setting verbose level is not a way because it enables non error messages
 */
static inline void oshmem_framework_open_output(struct mca_base_framework_t *framework)
{
    if (-1 == framework->framework_output) {
        framework->framework_output = opal_output_open(NULL);
    }
}

/**
 * Record process memory snapshot for SPML/UCX profiling.
 * Reads VmRSS, VmSize, VmPeak from /proc/self/status on Linux; otherwise
 * uses getrusage() for maxrss.
 * If OSHMEM_SPML_UCX_MEMORY_PROFILE_FILE is set, appends one CSV line to that file.
 * Otherwise prints one line to stdout.
 *
 * @param phase   Label for this snapshot (e.g. "spml_ucx_init")
 * @param rank    PE rank (use -1 if not yet known)
 * @param nprocs  Number of PEs (use -1 if not yet known)
 */
void oshmem_memory_snapshot(const char *phase, int rank, int nprocs);

#endif /* OSHMEM_UTIL_H */
