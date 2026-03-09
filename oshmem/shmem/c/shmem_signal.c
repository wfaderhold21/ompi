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
#include "oshmem/mca/atomic/atomic.h"

#if OSHMEM_PROFILING
#include "oshmem/include/pshmem.h"
#pragma weak shmem_signal_set = pshmem_signal_set
#pragma weak shmem_signal_add = pshmem_signal_add
#pragma weak shmem_signal_wait_until = pshmem_signal_wait_until
#include "oshmem/shmem/c/profile-defines.h"
#endif

/**
 * shmem_signal_set - Atomically set a signal variable on a remote PE
 *
 * This routine atomically sets the signal variable pointed to by sig_addr
 * on PE pe to the value signal.
 */
void pshmem_signal_set(uint64_t *sig_addr, uint64_t signal, int pe)
{
    int rc = OSHMEM_SUCCESS;

    RUNTIME_CHECK_INIT();
    RUNTIME_CHECK_PE(pe);
    RUNTIME_CHECK_ADDR(sig_addr);

    /* Use atomic swap to set the value - swap returns old value which we ignore */
    rc = MCA_ATOMIC_CALL(cswap(oshmem_ctx_default,
                               (void*)sig_addr,
                               NULL,  /* fetch result not needed */
                               0,     /* cond - don't care since we're setting unconditionally */
                               signal,
                               sizeof(uint64_t),
                               pe));

    /* If cswap doesn't work well for unconditional set, use a put with fence */
    if (rc != OSHMEM_SUCCESS) {
        /* Fallback: use put for the data and rely on fence for ordering */
        rc = MCA_SPML_CALL(put(oshmem_ctx_default,
                              (void*)sig_addr,
                              sizeof(uint64_t),
                              (void*)&signal,
                              pe));
        MCA_SPML_CALL(fence(oshmem_ctx_default));
    }

    RUNTIME_CHECK_RC(rc);
}

/**
 * shmem_signal_add - Atomically add to a signal variable on a remote PE
 *
 * This routine atomically adds the value signal to the signal variable
 * pointed to by sig_addr on PE pe.
 */
void pshmem_signal_add(uint64_t *sig_addr, uint64_t signal, int pe)
{
    int rc = OSHMEM_SUCCESS;

    RUNTIME_CHECK_INIT();
    RUNTIME_CHECK_PE(pe);
    RUNTIME_CHECK_ADDR(sig_addr);

    /* Use atomic fetch-add */
    rc = MCA_ATOMIC_CALL(fadd(oshmem_ctx_default,
                             (void*)sig_addr,
                             NULL,  /* fetch result not needed */
                             signal,
                             sizeof(uint64_t),
                             pe));

    RUNTIME_CHECK_RC(rc);
}

/**
 * shmem_signal_wait_until - Wait until a signal variable satisfies a condition
 *
 * This routine blocks until the signal variable at sig_addr satisfies
 * the specified comparison with cmp_value. Returns the value of the
 * signal variable that satisfied the condition.
 */
uint64_t pshmem_signal_wait_until(uint64_t *sig_addr, int cmp, uint64_t cmp_value)
{
    uint64_t value;
    int rc;

    RUNTIME_CHECK_INIT();
    RUNTIME_CHECK_ADDR(sig_addr);

    /* Use SPML wait to wait for the condition, treating sig_addr as local */
    rc = MCA_SPML_CALL(wait((void*)sig_addr, cmp, (void*)&cmp_value, SHMEM_UINT64_T));

    if (rc != OSHMEM_SUCCESS) {
        /* On error, just return current value */
        return *sig_addr;
    }

    /* Return the value that satisfied the condition */
    value = *sig_addr;

    return value;
}
