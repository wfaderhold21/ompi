/*
 * Copyright (c) 2026      NVIDIA Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 * SPDX-License-Identifier: BSD-3-Clause-Open-MPI
 */

/**
 * spml/gpi — SPML transport over the libgpi shm engine (OSHMEM_GPI.md S1).
 *
 * Topology (locked design, OSHMEM_GPI.md §6-A1): one gpi channel per
 * ordered pair, shipped as ONE channel per PE that PE owns and services:
 *
 *   chan[pe] (mine) — created by ME (gpi_shm_chan_create, no rendezvous
 *                     dir); pe produces into it; MY emu engine runs it.
 *   out[pe]         — pe's chan record, imported from the registration blob
 *                     at rmkey_unpack; I produce into it; pe's engine runs.
 *
 * Flat bind-id space on every channel (n_binds = 2*GPI_SPML_MAX_SEGS):
 *
 *   partition A, ids 1..MAX_SEGS        — the PRODUCER's symmetric seg j
 *                     (slot j-1; the chan owner records this slot when it
 *                     maps the producer's published bind at rmkey_unpack)
 *   partition B, ids 1+MAX_SEGS..2*MAX  — the OWNER's own seg j
 *                     (slot MAX_SEGS+j; the owner records it itself once
 *                     its segment is adopted in the first register())
 *   id 1+2*MAX_SEGS                     — the single completion counter
 *
 * Both peers stamp descriptors with the same positional formulas, so a
 * producer needs no knowledge of its own slots — only that every id the
 * engine will resolve exists before the engine starts (the owner gates
 * gpi_emu_start until all binds of the inbound channel are recorded;
 * producer descriptors queue on the ring meanwhile).
 *
 * Completion: every descriptor is GPI_FLAG_WITH_COUNTER|COUNTER_COUNTED on
 * counter id GPI_SPML_COUNTER_ID. The channel is FIFO, so a counted
 * GPI_CTRL_CHANNEL_FLUSH closes a quiet epoch: the target is the local
 * monotone count of counted descriptors posted to that PE; quiet polls
 * (counters[0] & GPI_COUNTER_SUCCESS_MASK) >= target; the ERROR bit
 * (GPI_COUNTER_FLAG_ERROR) is sticky until the next counted completion and
 * is checked on the way out.
 *
 * Wire blob (sshmem_mkey_t.u.data, shipped by memheap's own OOB — the
 * libgpi rendezvous directory is never touched): hdr + this segment's
 * published gpi_bind_handle_t + shm object name + n_procs channel records.
 */

#ifndef MCA_SPML_GPI_H
#define MCA_SPML_GPI_H

#include "oshmem_config.h"

#include "oshmem/request/request.h"
#include "oshmem/mca/spml/spml.h"
#include "oshmem/mca/spml/base/base.h"
#include "oshmem/proc/proc.h"
#include "oshmem/runtime/runtime.h"

#include "gpi_ring.h"
#include "gpi_shm.h"
#include "gpi_emu.h"

BEGIN_C_DECLS

/* ------------------------------------------------------------------ *
 * Wire protocol constants
 * ------------------------------------------------------------------ */
#define GPI_SPML_WIRE_MAGIC      0x47505357u  /* "GPSW" */
#define GPI_SPML_WIRE_VERSION    1u
#define GPI_SPML_MAX_SEGS        16u          /* segments per PE we support */
#define GPI_SPML_MAX_PROCS       256u         /* wire chans[] capacity      */
#define GPI_SPML_CHAN_LOG_DEPTH  12u          /* 4096 descriptors           */

#define GPI_SPML_N_BINDS         (2u * GPI_SPML_MAX_SEGS)
#define GPI_SPML_COUNTER_ID      (1u + GPI_SPML_N_BINDS)
#define GPI_SPML_BOUNCE_ID       (2u + GPI_SPML_N_BINDS)  /* FETCH_OP sink */

/* Producer-side (remote in the channel sense) seg j lives in partition A;
 * owner-side (local in the channel sense) seg j in partition B. */
#define GPI_SPML_BIND_PROD(j)    ((uint16_t)(1u + (j)))
#define GPI_SPML_BIND_OWN(j)     ((uint16_t)(1u + GPI_SPML_MAX_SEGS + (j)))

/* Every descriptor is counted on the channel's single counter. */
#define GPI_SPML_DESC_FLAGS      (uint8_t)(GPI_FLAG_WITH_COUNTER | \
                                           GPI_FLAG_COUNTER_COUNTED)

/* ------------------------------------------------------------------ *
 * Wire records (all fixed-size, size/magic-gated)
 * ------------------------------------------------------------------ */

/* One registered symmetric segment as published by its owner. */
typedef struct {
    gpi_bind_handle_t published;              /* 32B libgpi wire envelope  */
    char              obj[GPI_SHM_MAX_NAME];  /* POSIX shm object name     */
} gpi_spml_seg_record_t;

/* One channel as published by its creator (the channel owner). */
typedef struct {
    gpi_shm_chan_handle_t handle;             /* 64B libgpi chan record    */
    uint32_t              creator_pe;
    uint32_t              reserved;
} gpi_spml_chan_record_t;

/* The blob shipped in sshmem_mkey_t.u.data by ONE register() call: this
 * segment's record + every channel this PE owns. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    int32_t  my_pe;
    int32_t  n_procs;
    uint32_t segno;      /* the segment this blob describes          */
    uint32_t n_segs;     /* the sender's total segment count         */
    uint64_t seg_len;
    gpi_spml_seg_record_t  seg;
    gpi_spml_chan_record_t chans[GPI_SPML_MAX_PROCS];
} gpi_spml_wire_t;

/* ------------------------------------------------------------------ *
 * Module
 * ------------------------------------------------------------------ */
struct mca_spml_gpi_t {
    mca_spml_base_module_t super;
    int priority;
};
typedef struct mca_spml_gpi_t mca_spml_gpi_t;

extern mca_spml_gpi_t mca_spml_gpi;

/**
 * Component bring-up: open the "shm" gpi device and create ctx + domain.
 * Called from the component's spmlm_init; returns OSHMEM_SUCCESS or error.
 */
int spml_gpi_init(void);

/**
 * Component teardown.
 */
int spml_gpi_fini(void);

#define SPML_GPI_ERROR(...) \
    oshmem_output(oshmem_spml_base_framework.framework_output, \
        "Error %s:%d - %s()", __FILE__, __LINE__, __func__, __VA_ARGS__)

#define SPML_GPI_VERBOSE(level, ...) \
    oshmem_output_verbose(level, oshmem_spml_base_framework.framework_output, \
                          "%s:%d - %s()", __FILE__, __LINE__, __func__, __VA_ARGS__)

/* ------------------------------------------------------------------ *
 * SPML module functions (signatures mirror oshmem/mca/spml/spml.h)
 * ------------------------------------------------------------------ */
int mca_spml_gpi_add_procs(struct oshmem_group_t *group, size_t nprocs);
int mca_spml_gpi_del_procs(struct oshmem_group_t *group, size_t nprocs);

int mca_spml_gpi_enable(bool enable);
sshmem_mkey_t *mca_spml_gpi_register(void *addr, size_t size, uint64_t shmid,
                                     int *count);
int mca_spml_gpi_deregister(sshmem_mkey_t *mkeys);
int mca_spml_gpi_oob_get_mkeys(shmem_ctx_t ctx, int pe, uint32_t seg,
                               sshmem_mkey_t *mkeys);

int mca_spml_gpi_ctx_create(long options, shmem_ctx_t *ctx);
void mca_spml_gpi_ctx_destroy(shmem_ctx_t ctx);

int mca_spml_gpi_put(shmem_ctx_t ctx, void *dst_addr, size_t size,
                     void *src_addr, int dst_pe);
int mca_spml_gpi_put_nb(shmem_ctx_t ctx, void *dst_addr, size_t size,
                        void *src_addr, int dst_pe, void **handle);
int mca_spml_gpi_put_signal(shmem_ctx_t ctx, void *dst_addr, size_t size,
                            void *src_addr, uint64_t *sig_addr,
                            uint64_t signal, int sig_op, int dst_pe);
int mca_spml_gpi_put_signal_nb(shmem_ctx_t ctx, void *dst_addr, size_t size,
                               void *src_addr, uint64_t *sig_addr,
                               uint64_t signal, int sig_op, int dst_pe);
int mca_spml_gpi_get(shmem_ctx_t ctx, void *dst_addr, size_t size,
                     void *src_addr, int src_pe);
int mca_spml_gpi_get_nb(shmem_ctx_t ctx, void *dst_addr, size_t size,
                        void *src_addr, int src_pe, void **handle);

int mca_spml_gpi_recv(void *buf, size_t count, int src_pe);
int mca_spml_gpi_send(void *buf, size_t count, int dst_pe,
                      mca_spml_base_put_mode_t mode);

int mca_spml_gpi_wait(void *addr, int cmp, void *value, int datatype);
int mca_spml_gpi_wait_nb(void *value);
void mca_spml_gpi_wait_until_all(void *ivars, int cmp, void *cmp_value,
                                 size_t nelems, const int *status,
                                 int datatype);
size_t mca_spml_gpi_wait_until_any(void *ivars, int cmp, void *cmp_value,
                                   size_t nelems, const int *status,
                                   int datatype);
size_t mca_spml_gpi_wait_until_some(void *ivars, int cmp, void *cmp_value,
                                    size_t nelems, size_t *indices,
                                    const int *status, int datatype);
void mca_spml_gpi_wait_until_all_vector(void *ivars, int cmp,
                                        void *cmp_values, size_t nelems,
                                        const int *status, int datatype);
size_t mca_spml_gpi_wait_until_any_vector(void *ivars, int cmp,
                                          void *cmp_values, size_t nelems,
                                          const int *status, int datatype);
size_t mca_spml_gpi_wait_until_some_vector(void *ivars, int cmp,
                                           void *cmp_values, size_t nelems,
                                           size_t *indices, const int *status,
                                           int datatype);
int mca_spml_gpi_test(void *addr, int cmp, void *value, int datatype,
                      int *out_value);
int mca_spml_gpi_test_all(void *ivars, int cmp, void *cmp_value,
                          size_t nelems, const int *status, int datatype);
size_t mca_spml_gpi_test_any(void *ivars, int cmp, void *cmp_value,
                             size_t nelems, const int *status, int datatype);
size_t mca_spml_gpi_test_some(void *ivars, int cmp, void *cmp_value,
                              size_t nelems, size_t *indices,
                              const int *status, int datatype);
int mca_spml_gpi_test_all_vector(void *ivars, int cmp, void *cmp_values,
                                 size_t nelems, const int *status,
                                 int datatype);
size_t mca_spml_gpi_test_any_vector(void *ivars, int cmp, void *cmp_values,
                                    size_t nelems, const int *status,
                                    int datatype);
size_t mca_spml_gpi_test_some_vector(void *ivars, int cmp, void *cmp_values,
                                     size_t nelems, size_t *indices,
                                     const int *status, int datatype);

int mca_spml_gpi_team_sync(shmem_team_t team);
int mca_spml_gpi_team_my_pe(shmem_team_t team);
int mca_spml_gpi_team_n_pes(shmem_team_t team);
int mca_spml_gpi_team_get_config(shmem_team_t team, long config_mask,
                                 shmem_team_config_t *config);
int mca_spml_gpi_team_translate_pe(shmem_team_t src_team, int src_pe,
                                   shmem_team_t dest_team);
int mca_spml_gpi_team_split_strided(shmem_team_t parent_team, int start,
                                    int stride, int size,
                                    const shmem_team_config_t *config,
                                    long config_mask,
                                    shmem_team_t *new_team);
int mca_spml_gpi_team_split_2d(shmem_team_t parent_team, int xrange,
                               const shmem_team_config_t *xaxis_config,
                               long xaxis_mask, shmem_team_t *xaxis_team,
                               const shmem_team_config_t *yaxis_config,
                               long yaxis_mask, shmem_team_t *yaxis_team);
int mca_spml_gpi_team_destroy(shmem_team_t team);
int mca_spml_gpi_team_get(shmem_ctx_t ctx, shmem_team_t *team);
int mca_spml_gpi_team_create_ctx(shmem_team_t team, long options,
                                 shmem_ctx_t *ctx);
int mca_spml_gpi_team_alltoall(shmem_team_t team, void *dest,
                               const void *source, size_t nelems,
                               int datatype);
int mca_spml_gpi_team_alltoalls(shmem_team_t team, void *dest,
                                const void *source, ptrdiff_t dst,
                                ptrdiff_t sst, size_t nelems, int datatype);
int mca_spml_gpi_team_broadcast(shmem_team_t team, void *dest,
                                const void *source, size_t nelems,
                                int PE_root, int datatype);
int mca_spml_gpi_team_collect(shmem_team_t team, void *dest,
                              const void *source, size_t nelems,
                              int datatype);
int mca_spml_gpi_team_fcollect(shmem_team_t team, void *dest,
                               const void *source, size_t nelems,
                               int datatype);
int mca_spml_gpi_team_reduce(shmem_team_t team, void *dest,
                             const void *source, size_t nreduce,
                             int operation, int datatype);

int mca_spml_gpi_fence(shmem_ctx_t ctx);
int mca_spml_gpi_quiet(shmem_ctx_t ctx);

void mca_spml_gpi_rmkey_unpack(shmem_ctx_t ctx, sshmem_mkey_t *mkey,
                               uint32_t segno, int remote_pe, int tr_id);
void mca_spml_gpi_rmkey_free(sshmem_mkey_t *mkey, int pe);
void *mca_spml_gpi_rmkey_ptr(const void *dst_addr, sshmem_mkey_t *mkey,
                             int pe);

void mca_spml_gpi_memuse_hook(void *addr, size_t size);
int mca_spml_gpi_put_all_nb(void *dest, const void *source, size_t size,
                            long *counter);

END_C_DECLS

#endif /* MCA_SPML_GPI_H */
