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

#include "oshmem/mca/scoll/scoll.h"
#include "oshmem/proc/proc.h"
#include "oshmem/proc/team.h"
#include "oshmem/op/op.h"

#if OSHMEM_PROFILING
#include "oshmem/include/pshmem.h"
#include "oshmem/shmem/c/profile-defines.h"
#endif

/* Helper to get the operation object name - same as in shmem_reduce.c */
/* Due to C preprocessor limitations with ##, we use explicit per-type mappings */
/* For types that have direct operation objects, use them directly */
#define OSHMEM_OP_OBJ_NAME(_op, type_name) \
    OSHMEM_OP_OBJ_NAME_##type_name(_op)

/* Direct mappings for types that have operation objects */
#define OSHMEM_OP_OBJ_NAME__short(_op) oshmem_op_##_op##_short
#define OSHMEM_OP_OBJ_NAME__int(_op) oshmem_op_##_op##_int
#define OSHMEM_OP_OBJ_NAME__long(_op) oshmem_op_##_op##_long
#define OSHMEM_OP_OBJ_NAME__longlong(_op) oshmem_op_##_op##_longlong
#define OSHMEM_OP_OBJ_NAME__int8(_op) oshmem_op_##_op##_int16
#define OSHMEM_OP_OBJ_NAME__int16(_op) oshmem_op_##_op##_int16
#define OSHMEM_OP_OBJ_NAME__int32(_op) oshmem_op_##_op##_int32
#define OSHMEM_OP_OBJ_NAME__int64(_op) oshmem_op_##_op##_int64
#define OSHMEM_OP_OBJ_NAME__float(_op) oshmem_op_##_op##_float
#define OSHMEM_OP_OBJ_NAME__double(_op) oshmem_op_##_op##_double
#define OSHMEM_OP_OBJ_NAME__longdouble(_op) oshmem_op_##_op##_longdouble
#define OSHMEM_OP_OBJ_NAME__complexf(_op) oshmem_op_##_op##_complexf
#define OSHMEM_OP_OBJ_NAME__complexd(_op) oshmem_op_##_op##_complexd

/* Mappings for unsigned/special types to available operation objects */
#define OSHMEM_OP_OBJ_NAME__uchar(_op) oshmem_op_##_op##_short
#define OSHMEM_OP_OBJ_NAME__ushort(_op) oshmem_op_##_op##_short
#define OSHMEM_OP_OBJ_NAME__uint(_op) oshmem_op_##_op##_int
#define OSHMEM_OP_OBJ_NAME__ulong(_op) oshmem_op_##_op##_long
#define OSHMEM_OP_OBJ_NAME__ulonglong(_op) oshmem_op_##_op##_longlong
#define OSHMEM_OP_OBJ_NAME__uint8(_op) oshmem_op_##_op##_int16
#define OSHMEM_OP_OBJ_NAME__uint16(_op) oshmem_op_##_op##_int16
#define OSHMEM_OP_OBJ_NAME__uint32(_op) oshmem_op_##_op##_int32
#define OSHMEM_OP_OBJ_NAME__uint64(_op) oshmem_op_##_op##_int64
#define OSHMEM_OP_OBJ_NAME__char(_op) oshmem_op_##_op##_short
#define OSHMEM_OP_OBJ_NAME__schar(_op) oshmem_op_##_op##_short
#define OSHMEM_OP_OBJ_NAME__size(_op) oshmem_op_##_op##_long
#define OSHMEM_OP_OBJ_NAME__ptrdiff(_op) oshmem_op_##_op##_long

/*
 * Team-based inclusive scan operation macro
 * inclusive = true: result[i] = source[0] op source[1] op ... op source[i]
 */
#define SHMEM_TYPE_TEAM_ISCAN_OP(_op, type_name, type)                          \
int shmem##type_name##_##_op##_iscan(shmem_team_t team, type *dest,             \
                                     const type *source, size_t nreduce)        \
{                                                                               \
    int rc = OSHMEM_SUCCESS;                                                    \
    oshmem_op_t* op = OSHMEM_OP_OBJ_NAME(_op, type_name);                       \
    size_t size;                                                                \
                                                                                \
    RUNTIME_CHECK_INIT();                                                       \
                                                                                \
    if (!oshmem_team_is_valid(team)) {                                          \
        return OSHMEM_ERR_BAD_PARAM;                                            \
    }                                                                           \
                                                                                \
    size = nreduce * op->dt_size;                                               \
                                                                                \
    /* Call collective scan operation via team's group (inclusive=true) */      \
    rc = team->group->g_scoll.scoll_scan(team->group, op,                       \
            (void*)dest, (const void*)source, size,                             \
            team->sync, team->work, true, SCOLL_DEFAULT_ALG);                   \
                                                                                \
    RUNTIME_CHECK_RC(rc);                                                       \
                                                                                \
    return rc;                                                                  \
}

/*
 * Team-based exclusive scan operation macro
 * exclusive = true: result[i] = source[0] op source[1] op ... op source[i-1]
 *             result[0] = identity element
 */
#define SHMEM_TYPE_TEAM_ESCAN_OP(_op, type_name, type)                          \
int shmem##type_name##_##_op##_escan(shmem_team_t team, type *dest,             \
                                     const type *source, size_t nreduce)        \
{                                                                               \
    int rc = OSHMEM_SUCCESS;                                                    \
    oshmem_op_t* op = OSHMEM_OP_OBJ_NAME(_op, type_name);                       \
    size_t size;                                                                \
                                                                                \
    RUNTIME_CHECK_INIT();                                                       \
                                                                                \
    if (!oshmem_team_is_valid(team)) {                                          \
        return OSHMEM_ERR_BAD_PARAM;                                            \
    }                                                                           \
                                                                                \
    size = nreduce * op->dt_size;                                               \
                                                                                \
    /* Call collective scan operation via team's group (inclusive=false) */     \
    rc = team->group->g_scoll.scoll_scan(team->group, op,                       \
            (void*)dest, (const void*)source, size,                             \
            team->sync, team->work, false, SCOLL_DEFAULT_ALG);                  \
                                                                                \
    RUNTIME_CHECK_RC(rc);                                                       \
                                                                                \
    return rc;                                                                  \
}

/* Inclusive SUM scan */
SHMEM_TYPE_TEAM_ISCAN_OP(sum, _char, char)
SHMEM_TYPE_TEAM_ISCAN_OP(sum, _short, short)
SHMEM_TYPE_TEAM_ISCAN_OP(sum, _int, int)
SHMEM_TYPE_TEAM_ISCAN_OP(sum, _long, long)
SHMEM_TYPE_TEAM_ISCAN_OP(sum, _float, float)
SHMEM_TYPE_TEAM_ISCAN_OP(sum, _double, double)
SHMEM_TYPE_TEAM_ISCAN_OP(sum, _longlong, long long)
SHMEM_TYPE_TEAM_ISCAN_OP(sum, _schar, signed char)
SHMEM_TYPE_TEAM_ISCAN_OP(sum, _uchar, unsigned char)
SHMEM_TYPE_TEAM_ISCAN_OP(sum, _ushort, unsigned short)
SHMEM_TYPE_TEAM_ISCAN_OP(sum, _uint, unsigned int)
SHMEM_TYPE_TEAM_ISCAN_OP(sum, _ulong, unsigned long)
SHMEM_TYPE_TEAM_ISCAN_OP(sum, _ulonglong, unsigned long long)
SHMEM_TYPE_TEAM_ISCAN_OP(sum, _longdouble, long double)
SHMEM_TYPE_TEAM_ISCAN_OP(sum, _int8, int8_t)
SHMEM_TYPE_TEAM_ISCAN_OP(sum, _int16, int16_t)
SHMEM_TYPE_TEAM_ISCAN_OP(sum, _int32, int32_t)
SHMEM_TYPE_TEAM_ISCAN_OP(sum, _int64, int64_t)
SHMEM_TYPE_TEAM_ISCAN_OP(sum, _uint8, uint8_t)
SHMEM_TYPE_TEAM_ISCAN_OP(sum, _uint16, uint16_t)
SHMEM_TYPE_TEAM_ISCAN_OP(sum, _uint32, uint32_t)
SHMEM_TYPE_TEAM_ISCAN_OP(sum, _uint64, uint64_t)
SHMEM_TYPE_TEAM_ISCAN_OP(sum, _size, size_t)
SHMEM_TYPE_TEAM_ISCAN_OP(sum, _ptrdiff, ptrdiff_t)

/* Exclusive SUM scan */
SHMEM_TYPE_TEAM_ESCAN_OP(sum, _char, char)
SHMEM_TYPE_TEAM_ESCAN_OP(sum, _short, short)
SHMEM_TYPE_TEAM_ESCAN_OP(sum, _int, int)
SHMEM_TYPE_TEAM_ESCAN_OP(sum, _long, long)
SHMEM_TYPE_TEAM_ESCAN_OP(sum, _float, float)
SHMEM_TYPE_TEAM_ESCAN_OP(sum, _double, double)
SHMEM_TYPE_TEAM_ESCAN_OP(sum, _longlong, long long)
SHMEM_TYPE_TEAM_ESCAN_OP(sum, _schar, signed char)
SHMEM_TYPE_TEAM_ESCAN_OP(sum, _uchar, unsigned char)
SHMEM_TYPE_TEAM_ESCAN_OP(sum, _ushort, unsigned short)
SHMEM_TYPE_TEAM_ESCAN_OP(sum, _uint, unsigned int)
SHMEM_TYPE_TEAM_ESCAN_OP(sum, _ulong, unsigned long)
SHMEM_TYPE_TEAM_ESCAN_OP(sum, _ulonglong, unsigned long long)
SHMEM_TYPE_TEAM_ESCAN_OP(sum, _longdouble, long double)
SHMEM_TYPE_TEAM_ESCAN_OP(sum, _int8, int8_t)
SHMEM_TYPE_TEAM_ESCAN_OP(sum, _int16, int16_t)
SHMEM_TYPE_TEAM_ESCAN_OP(sum, _int32, int32_t)
SHMEM_TYPE_TEAM_ESCAN_OP(sum, _int64, int64_t)
SHMEM_TYPE_TEAM_ESCAN_OP(sum, _uint8, uint8_t)
SHMEM_TYPE_TEAM_ESCAN_OP(sum, _uint16, uint16_t)
SHMEM_TYPE_TEAM_ESCAN_OP(sum, _uint32, uint32_t)
SHMEM_TYPE_TEAM_ESCAN_OP(sum, _uint64, uint64_t)
SHMEM_TYPE_TEAM_ESCAN_OP(sum, _size, size_t)
SHMEM_TYPE_TEAM_ESCAN_OP(sum, _ptrdiff, ptrdiff_t)

/* Inclusive PROD scan */
SHMEM_TYPE_TEAM_ISCAN_OP(prod, _char, char)
SHMEM_TYPE_TEAM_ISCAN_OP(prod, _short, short)
SHMEM_TYPE_TEAM_ISCAN_OP(prod, _int, int)
SHMEM_TYPE_TEAM_ISCAN_OP(prod, _long, long)
SHMEM_TYPE_TEAM_ISCAN_OP(prod, _float, float)
SHMEM_TYPE_TEAM_ISCAN_OP(prod, _double, double)
SHMEM_TYPE_TEAM_ISCAN_OP(prod, _longlong, long long)
SHMEM_TYPE_TEAM_ISCAN_OP(prod, _schar, signed char)
SHMEM_TYPE_TEAM_ISCAN_OP(prod, _uchar, unsigned char)
SHMEM_TYPE_TEAM_ISCAN_OP(prod, _ushort, unsigned short)
SHMEM_TYPE_TEAM_ISCAN_OP(prod, _uint, unsigned int)
SHMEM_TYPE_TEAM_ISCAN_OP(prod, _ulong, unsigned long)
SHMEM_TYPE_TEAM_ISCAN_OP(prod, _ulonglong, unsigned long long)
SHMEM_TYPE_TEAM_ISCAN_OP(prod, _longdouble, long double)
SHMEM_TYPE_TEAM_ISCAN_OP(prod, _int8, int8_t)
SHMEM_TYPE_TEAM_ISCAN_OP(prod, _int16, int16_t)
SHMEM_TYPE_TEAM_ISCAN_OP(prod, _int32, int32_t)
SHMEM_TYPE_TEAM_ISCAN_OP(prod, _int64, int64_t)
SHMEM_TYPE_TEAM_ISCAN_OP(prod, _uint8, uint8_t)
SHMEM_TYPE_TEAM_ISCAN_OP(prod, _uint16, uint16_t)
SHMEM_TYPE_TEAM_ISCAN_OP(prod, _uint32, uint32_t)
SHMEM_TYPE_TEAM_ISCAN_OP(prod, _uint64, uint64_t)
SHMEM_TYPE_TEAM_ISCAN_OP(prod, _size, size_t)
SHMEM_TYPE_TEAM_ISCAN_OP(prod, _ptrdiff, ptrdiff_t)

/* Exclusive PROD scan */
SHMEM_TYPE_TEAM_ESCAN_OP(prod, _char, char)
SHMEM_TYPE_TEAM_ESCAN_OP(prod, _short, short)
SHMEM_TYPE_TEAM_ESCAN_OP(prod, _int, int)
SHMEM_TYPE_TEAM_ESCAN_OP(prod, _long, long)
SHMEM_TYPE_TEAM_ESCAN_OP(prod, _float, float)
SHMEM_TYPE_TEAM_ESCAN_OP(prod, _double, double)
SHMEM_TYPE_TEAM_ESCAN_OP(prod, _longlong, long long)
SHMEM_TYPE_TEAM_ESCAN_OP(prod, _schar, signed char)
SHMEM_TYPE_TEAM_ESCAN_OP(prod, _uchar, unsigned char)
SHMEM_TYPE_TEAM_ESCAN_OP(prod, _ushort, unsigned short)
SHMEM_TYPE_TEAM_ESCAN_OP(prod, _uint, unsigned int)
SHMEM_TYPE_TEAM_ESCAN_OP(prod, _ulong, unsigned long)
SHMEM_TYPE_TEAM_ESCAN_OP(prod, _ulonglong, unsigned long long)
SHMEM_TYPE_TEAM_ESCAN_OP(prod, _longdouble, long double)
SHMEM_TYPE_TEAM_ESCAN_OP(prod, _int8, int8_t)
SHMEM_TYPE_TEAM_ESCAN_OP(prod, _int16, int16_t)
SHMEM_TYPE_TEAM_ESCAN_OP(prod, _int32, int32_t)
SHMEM_TYPE_TEAM_ESCAN_OP(prod, _int64, int64_t)
SHMEM_TYPE_TEAM_ESCAN_OP(prod, _uint8, uint8_t)
SHMEM_TYPE_TEAM_ESCAN_OP(prod, _uint16, uint16_t)
SHMEM_TYPE_TEAM_ESCAN_OP(prod, _uint32, uint32_t)
SHMEM_TYPE_TEAM_ESCAN_OP(prod, _uint64, uint64_t)
SHMEM_TYPE_TEAM_ESCAN_OP(prod, _size, size_t)
SHMEM_TYPE_TEAM_ESCAN_OP(prod, _ptrdiff, ptrdiff_t)
