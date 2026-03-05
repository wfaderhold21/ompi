/*
 * Copyright (c) 2014      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2015 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2018      Amazon.com, Inc. or its affiliates.  All Rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "oshmem_config.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_SYS_RESOURCE_H
#include <sys/resource.h>
#endif

#include "opal/util/printf.h"

#include "oshmem/constants.h"
#include "oshmem/util/oshmem_util.h"

void oshmem_output_verbose(int level, int output_id, const char* prefix,
    const char* file, int line, const char* function, const char* format, ...)
{
    va_list args;
    char *buff, *str;
    int ret = 0;

    if (level <= opal_output_get_verbosity(output_id)) {
        UNREFERENCED_PARAMETER(ret);

        va_start(args, format);

        ret = opal_vasprintf(&str, format, args);
        assert(-1 != ret);

        ret = opal_asprintf(&buff, "%s %s", prefix, str);
        assert(-1 != ret);

        opal_output(output_id, buff, file, line, function);

        va_end(args);

        free(buff);
        free(str);
    }
}


void oshmem_output(int output_id, const char* prefix, const char* file,
    int line, const char* function, const char* format, ...)
{
    va_list args;
    char *buff, *str;
    int ret = 0;

    UNREFERENCED_PARAMETER(ret);

    va_start(args, format);

    ret = opal_vasprintf(&str, format, args);
    assert(-1 != ret);

    ret = opal_asprintf(&buff, "%s %s", prefix, str);
    assert(-1 != ret);

    opal_output(output_id, buff, file, line, function);

    va_end(args);

    free(buff);
    free(str);
}

void oshmem_memory_snapshot(const char *phase, int rank, int nprocs)
{
    long vmrss_kb = -1;
    long vmsize_kb = -1;
    long vmpeak_kb = -1;
    FILE *fp = NULL;
    const char *path;
    char line[256];
#ifdef HAVE_SYS_RESOURCE_H
    struct rusage ru;
#endif

    path = getenv("OSHMEM_SPML_UCX_MEMORY_PROFILE_FILE");

#ifdef __linux__
    fp = fopen("/proc/self/status", "r");
    if (fp != NULL) {
        while (fgets(line, sizeof(line), fp) != NULL) {
            if (strncmp(line, "VmRSS:", 6) == 0) {
                sscanf(line + 6, "%ld", &vmrss_kb);
            } else if (strncmp(line, "VmSize:", 7) == 0) {
                sscanf(line + 7, "%ld", &vmsize_kb);
            } else if (strncmp(line, "VmPeak:", 7) == 0) {
                sscanf(line + 7, "%ld", &vmpeak_kb);
            }
        }
        fclose(fp);
        fp = NULL;
    }
#endif

#ifdef HAVE_SYS_RESOURCE_H
    if (vmrss_kb < 0 && getrusage(RUSAGE_SELF, &ru) == 0) {
        vmrss_kb = (long) ru.ru_maxrss;
#if !defined(__linux__)
        /* On many systems ru_maxrss is in bytes */
        if (ru.ru_maxrss > 10000000) {
            vmrss_kb /= 1024;
        }
#endif
    }
#endif

    if (path != NULL && path[0] != '\0') {
        fp = fopen(path, "a");
        if (fp != NULL) {
            if (ftell(fp) == 0) {
                fprintf(fp, "phase,rank,nprocs,vmrss_kb,vmsize_kb,vmpeak_kb\n");
            }
            fprintf(fp, "%s,%d,%d,%ld,%ld,%ld\n",
                    phase, rank, nprocs, vmrss_kb, vmsize_kb, vmpeak_kb);
            fclose(fp);
        }
    } else {
        /* default: print one line to stdout (no header to avoid spam from every rank) */
        fprintf(stdout, "oshmem_memory_profile: %s,%d,%d,%ld,%ld,%ld\n",
                phase, rank, nprocs, vmrss_kb, vmsize_kb, vmpeak_kb);
    }
}
