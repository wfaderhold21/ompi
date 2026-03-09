/*
 * Copyright (c) 2024      NVIDIA Corporation.
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
#include "oshmem_config.h"

#include "oshmem/constants.h"
#include "oshmem/include/shmem.h"

#include "oshmem/runtime/runtime.h"

#include "oshmem/mca/spml/spml.h"

/*
 * Interleaved block put routines copy strided blocks from local PE
 * to strided blocks on the destination PE.
 *
 * Parameters:
 *   target  - data object on remote PE
 *   source  - data object on local PE
 *   tst     - target stride (number of blocks between consecutive target blocks)
 *   sst     - source stride (number of blocks between consecutive source blocks)
 *   bsize   - block size in bytes (for shmem_ibputXX) or elements
 *   nblocks - number of blocks to transfer
 *   pe      - remote PE number
 */

#define DO_SHMEM_TYPE_IBPUT(ctx, type_size, target, source, tst, sst, bsize, nblocks, pe) do { \
        int rc = OSHMEM_SUCCESS;                                    \
        size_t i;                                                   \
        size_t block_bytes = bsize * type_size;                     \
        char *t = (char *)target;                                   \
        const char *s = (const char *)source;                       \
                                                                    \
        RUNTIME_CHECK_INIT();                                       \
        RUNTIME_CHECK_PE(pe);                                       \
        RUNTIME_CHECK_ADDR(target);                                 \
                                                                    \
        for (i = 0; i < nblocks; i++) {                             \
            rc = MCA_SPML_CALL(put(                                 \
                ctx,                                                \
                (void*)(t + i * tst * block_bytes),                 \
                block_bytes,                                        \
                (void*)(s + i * sst * block_bytes),                 \
                pe));                                               \
            if (rc != OSHMEM_SUCCESS) break;                        \
        }                                                           \
        RUNTIME_CHECK_RC(rc);                                       \
    } while (0)

#if OSHMEM_PROFILING
#define SHMEM_CTX_TYPE_IBPUT(name, type_size)                       \
    void pshmem_ctx_ibput##name(shmem_ctx_t ctx, void* target,       \
                                const void* source, ptrdiff_t tst,  \
                                ptrdiff_t sst, size_t bsize,         \
                                size_t nblocks, int pe)             \
    {                                                               \
        DO_SHMEM_TYPE_IBPUT(ctx, type_size, target, source,         \
                            tst, sst, bsize, nblocks, pe);          \
        return;                                                     \
    }
#define SHMEM_TYPE_IBPUT(name, type_size)                            \
    void pshmem_ibput##name(void* target, const void* source,        \
                            ptrdiff_t tst, ptrdiff_t sst,            \
                            size_t bsize, size_t nblocks, int pe)    \
    {                                                               \
        DO_SHMEM_TYPE_IBPUT(oshmem_ctx_default, type_size, target,  \
                            source, tst, sst, bsize, nblocks, pe);   \
        return;                                                     \
    }
#else
#define SHMEM_CTX_TYPE_IBPUT(name, type_size)                        \
    void shmem_ctx_ibput##name(shmem_ctx_t ctx, void* target,       \
                               const void* source, ptrdiff_t tst,   \
                               ptrdiff_t sst, size_t bsize,         \
                               size_t nblocks, int pe)              \
    {                                                               \
        DO_SHMEM_TYPE_IBPUT(ctx, type_size, target, source,         \
                            tst, sst, bsize, nblocks, pe);          \
        return;                                                     \
    }
#define SHMEM_TYPE_IBPUT(name, type_size)                            \
    void shmem_ibput##name(void* target, const void* source,         \
                           ptrdiff_t tst, ptrdiff_t sst,             \
                           size_t bsize, size_t nblocks, int pe)     \
    {                                                               \
        DO_SHMEM_TYPE_IBPUT(oshmem_ctx_default, type_size, target,  \
                            source, tst, sst, bsize, nblocks, pe);   \
        return;                                                     \
    }
#endif

#if OSHMEM_PROFILING
#include "oshmem/include/pshmem.h"
#pragma weak shmem_ctx_ibput8   = pshmem_ctx_ibput8
#pragma weak shmem_ctx_ibput16  = pshmem_ctx_ibput16
#pragma weak shmem_ctx_ibput32  = pshmem_ctx_ibput32
#pragma weak shmem_ctx_ibput64  = pshmem_ctx_ibput64
#pragma weak shmem_ctx_ibput128 = pshmem_ctx_ibput128
#pragma weak shmem_ibput8       = pshmem_ibput8
#pragma weak shmem_ibput16      = pshmem_ibput16
#pragma weak shmem_ibput32      = pshmem_ibput32
#pragma weak shmem_ibput64      = pshmem_ibput64
#pragma weak shmem_ibput128     = pshmem_ibput128
#include "oshmem/shmem/c/profile-defines.h"
#endif

/* Context variants */
SHMEM_CTX_TYPE_IBPUT(8, 1)
SHMEM_CTX_TYPE_IBPUT(16, 2)
SHMEM_CTX_TYPE_IBPUT(32, 4)
SHMEM_CTX_TYPE_IBPUT(64, 8)
SHMEM_CTX_TYPE_IBPUT(128, 16)

/* Default context variants */
SHMEM_TYPE_IBPUT(8, 1)
SHMEM_TYPE_IBPUT(16, 2)
SHMEM_TYPE_IBPUT(32, 4)
SHMEM_TYPE_IBPUT(64, 8)
SHMEM_TYPE_IBPUT(128, 16)
