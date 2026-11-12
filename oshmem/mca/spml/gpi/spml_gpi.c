/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2026      NVIDIA Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 * SPDX-License-Identifier: BSD-3-Clause-Open-MPI
 */

/*
 * spml/gpi module — S1 tranche (OSHMEM_GPI.md §4–§9).
 *
 * Topology (one libgpi shm channel per ordered PE pair):
 *   in[pe]  — created by ME (directory-free gpi_shm_chan_create); pe
 *             produces INTO it; MY emu engine executes it. Traffic into me.
 *   out[pe] — pe's in[me], imported from pe's wire blob at rmkey_unpack;
 *             I produce into it; PE's engine executes it. Traffic out.
 *
 * Flat bind-id space on every channel (§9.1):
 *   partition A, ids 1..M          : the PRODUCER's seg j — the channel
 *             OWNER maps the producer's published bind at rmkey_unpack
 *             and stamps slot j itself.
 *   partition B, ids 1+M..2M       : the OWNER's own seg j — stamped by
 *             the owner in register() from its adopted mapping (identity
 *             va; memheap never calls rmkey_unpack for self, so this is
 *             also the self-traffic bootstrap).
 *   id 1+2M  (GPI_SPML_COUNTER_ID) : the single counted counter.
 *   id 2+2M  (GPI_SPML_BOUNCE_ID)  : the single signal word, used as the
 *             FETCH_OP bounce-slot sink (fetched value discarded).
 *
 * A producer stamps descriptors positionally — its own data from
 * partition A, the owner's from partition B — and needs no knowledge of
 * either side's mappings. Segment indices are wire-identical because every
 * rank registers the same memheap segments in the same order.
 *
 * Completion: every posted descriptor (data, signal, flush) is
 * WITH_COUNTER|COUNTER_COUNTED on the channel counter, so a per-channel
 * monotone "counted_posted" count is the quiet target. quiet() posts a
 * counted CHANNEL_FLUSH on every dirty outbound channel, then polls the
 * completion counter: out[q].counters[0] for remote peers (their engine
 * advances their object) and in[me].counters[0] for self. The counter
 * ERROR bit is checked on every poll pass — libgpi's counted increment
 * rewrites the word from the 56-bit success field, so an error flag set
 * mid-stream is erased by the NEXT completion; an error descriptor also
 * short-counts, which the ERROR-bit check converts from a hang into a
 * reported failure. S1 limitation, documented in STATUS.
 *
 * Progress: the opal_progress hook is the SOLE gpi_emu_wait caller (the
 * engine contract is single-waiter). Engines start once both bind
 * partitions of their inbound channel are fully stamped; descriptors
 * queue meanwhile (gpi_reserve still applies ring backpressure).
 * Consequence, same caveat as ucx-without-progress: a REMOTE put's
 * completion requires the OWNER's process to call opal_progress.
 */

#include "oshmem_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#include "opal/runtime/opal_progress.h"
#include "ompi/mca/pml/pml.h"
#include "ompi/datatype/ompi_datatype.h"
#include "oshmem/include/shmem.h"
#include "oshmem/mca/atomic/atomic.h"
#include "oshmem/mca/memheap/base/base.h"
#include "oshmem/mca/spml/base/base.h"
#include "oshmem/mca/spml/spml.h"
#include "oshmem/runtime/params.h"
#include "oshmem/runtime/runtime.h"

#include "spml_gpi.h"
#include "spml_gpi_component.h"

#include "gpi_ring.h"
#include "gpi_shm.h"
#include "gpi_emu.h"


/* ------------------------------------------------------------------ *
 * per-PE transport state
 * ------------------------------------------------------------------ */
typedef struct {
    gpi_shm_chan_t        in;          /* I own; pe produces; I execute  */
    gpi_shm_chan_handle_t in_handle;   /* shipped in my wire blob        */
    int                   in_created;
    gpi_producer_t        self_prod;   /* only for pe == my_pe           */
    int                   self_prod_open;
    gpi_emu_t            *emu;
    int                   emu_started;
    volatile int          chan_error;  /* latched by seam on_event       */
    gpi_shm_chan_t        out;         /* pe's in[me]; I produce         */
    int                   out_imported;
    gpi_producer_t        prod;
    int                   prod_open;
    uint32_t              a_mask;      /* A slots stamped (pe's segs)    */
    uint64_t              counted_posted; /* counted descs posted       */
    int                   dirty;       /* posted since last quiet        */
} gpi_pe_t;

static gpi_pe_t      *g_pe;                        /* [n_procs] */
static gpi_device_t  *g_device;
static gpi_ctx_t     *g_ctx;
static gpi_domain_t  *g_domain;
static unsigned       g_nprocs, g_my_pe;
static uint32_t       g_b_mask;                    /* my adopted segs */
static char           g_adopt_obj[GPI_SPML_MAX_SEGS][GPI_SHM_MAX_NAME];
static unsigned       g_adopt_count;
static int            g_hook_registered;

/* S1 has no real contexts: hand out one stable pointer as the default
 * ctx (ucx precedent: oshmem_ctx_default = &mca_spml_ucx_ctx_default). */
static uint64_t gpi_ctx_default_storage[4];

/* ------------------------------------------------------------------ *
 * helpers
 * ------------------------------------------------------------------ */

static inline uint32_t gpi_full_mask(void)
{
    return (1u << memheap_map->n_segments) - 1u;
}

static inline int gpi_seg_of(const void *addr, uint64_t *off_out)
{
    const char *a = (const char *) addr;
    uint32_t s;

    for (s = 0; s < (uint32_t) memheap_map->n_segments; s++) {
        map_segment_t *seg = &memheap_map->mem_segs[s];
        if (a >= (const char *) seg->super.va_base
            && a < (const char *) seg->super.va_end) {
            *off_out = (uint64_t) (a - (const char *) seg->super.va_base);
            return (int) s;
        }
    }
    return -1;
}

/* seam shims: the gpi_emu_seam contract is void*-shaped */
static void *gpi_seam_resolve(void *ctx, uint16_t id, uint64_t offset)
{
    /* seam.ctx is the gpi_pe_t owning this engine (set in the hook) */
    return gpi_shm_chan_resolve(&((gpi_pe_t *) ctx)->in, id, offset);
}

static void gpi_seam_event(void *ctx, const gpi_emu_event_t *ev)
{
    gpi_pe_t *p = (gpi_pe_t *) ctx;
    (void) ev;
    p->chan_error = 1;
}

/* ------------------------------------------------------------------ *
 * progress hook — the SOLE gpi_emu_wait caller (single-waiter contract)
 * ------------------------------------------------------------------ */
static int gpi_spml_progress(void)
{
    int count = 0;
    uint32_t full;
    unsigned q;

    if (NULL == g_pe || NULL == memheap_map) {
        return 0;
    }
    full = gpi_full_mask();

    for (q = 0; q < g_nprocs; q++) {
        gpi_pe_t *p = &g_pe[q];

        if (!p->in_created) {
            continue;
        }
        if (p->a_mask != full || g_b_mask != full) {
            continue; /* binds incomplete: descs queue, engine waits */
        }
        if (!p->emu_started) {
            gpi_emu_seam_t seam = {
                .struct_size      = sizeof(gpi_emu_seam_t),
                .ctx              = p,
                .resolve          = gpi_seam_resolve,
                .on_exec          = NULL,
                .on_flush_pe      = NULL,
                .on_channel_flush = NULL,
                .on_event         = gpi_seam_event,
            };
            /* seam is copied by gpi_emu_start; ctx (g_pe entry) is static
             * for the process lifetime */
            p->emu = gpi_emu_start(p->in.ring, &seam);
            if (NULL == p->emu) {
                SPML_GPI_ERROR("gpi_emu_start on in[%u] failed", q);
                continue;
            }
            p->emu_started = 1;
        }
        if (gpi_ring_pi(p->in.ring) != gpi_ring_ci(p->in.ring)) {
            int rc = gpi_emu_wait(p->emu, UINT64_MAX);
            if (rc < 0) {
                continue;          /* engine stopped */
            }
            if (rc > 0) {          /* drain-all must not short */
                SPML_GPI_ERROR("in[%u] burst shortfall rc=%d", q, rc);
                return count;
            }
            count++;
        }
    }
    return count;
}

/* ------------------------------------------------------------------ *
 * component bring-up / teardown
 * ------------------------------------------------------------------ */

int spml_gpi_init(void)
{
    g_device = gpi_shm_device_open();
    if (NULL == g_device) {
        SPML_GPI_ERROR("gpi_shm_device_open failed");
        return OSHMEM_ERR_NOT_AVAILABLE;
    }
    if (0 != gpi_ctx_create(g_device, NULL, &g_ctx)) {
        SPML_GPI_ERROR("gpi_ctx_create failed");
        return OSHMEM_ERR_NOT_AVAILABLE;
    }
    if (0 != gpi_domain_create(g_ctx, NULL, &g_domain)) {
        SPML_GPI_ERROR("gpi_domain_create failed");
        return OSHMEM_ERR_NOT_AVAILABLE;
    }
    oshmem_ctx_default = (shmem_ctx_t) (void *) gpi_ctx_default_storage;
    return OSHMEM_SUCCESS;
}

int spml_gpi_fini(void)
{
    /* children before parents; both destroys return GPI_EBUSY rather than
     * corrupting state if something is still alive (best-effort: S1 has no
     * error path out of MCA finalize) */
    if (NULL != g_domain) {
        int rc = gpi_domain_destroy(g_domain);
        if (0 != rc) {
            SPML_GPI_ERROR("domain_destroy rc=%d (%s)", rc,
                           gpi_strerror(rc));
        }
        g_domain = NULL;
    }
    if (NULL != g_ctx) {
        gpi_ctx_destroy(g_ctx);
        g_ctx = NULL;
    }
    /* shm objects outlive mappings (libgpi rule: cleanup is explicit):
     * unlink our adopted region names; channel objects were unlinked in
     * del_procs */
    while (g_adopt_count > 0) {
        g_adopt_count--;
        shm_unlink(g_adopt_obj[g_adopt_count]);
    }
    /* no gpi_device_close exists in this libgpi build (the device is a
     * process-lifetime singleton) — nothing to release */
    return OSHMEM_SUCCESS;
}

/* ------------------------------------------------------------------ *
 * procs
 * ------------------------------------------------------------------ */

int mca_spml_gpi_add_procs(struct oshmem_group_t *group, size_t nprocs)
{
    char label[32];
    unsigned p;

    (void) group; /* S1: WORLD only; pe indices are global ids */

    if (nprocs > GPI_SPML_MAX_PROCS) {
        SPML_GPI_ERROR("%zu procs exceeds wire capacity %u",
                       (size_t) nprocs, GPI_SPML_MAX_PROCS);
        return OSHMEM_ERROR;
    }
    if (NULL != g_pe) {
        return OSHMEM_SUCCESS; /* group re-selection (S1: same WORLD set) */
    }
    g_nprocs = (unsigned) nprocs;
    g_my_pe  = (unsigned) shmem_my_pe();

    g_pe = (gpi_pe_t *) calloc(nprocs, sizeof(*g_pe));
    if (NULL == g_pe) {
        return OSHMEM_ERROR;
    }

    for (p = 0; p < g_nprocs; p++) {
        snprintf(label, sizeof(label), "g%u-%d-%u", g_my_pe, (int) getpid(), p);
        if (0 > gpi_shm_chan_create(label, GPI_SPML_CHAN_LOG_DEPTH,
                                    1 /* n_counters */, 1 /* n_signals:
                                        * FETCH_OP bounce sink */,
                                    GPI_SPML_N_BINDS,
                                    &g_pe[p].in_handle, &g_pe[p].in)) {
            SPML_GPI_ERROR("gpi_shm_chan_create in[%u] (%s) failed", p, label);
            return OSHMEM_ERROR;
        }
        g_pe[p].in_created = 1;
    }

    /* self traffic posts into in[me] directly */
    if (0 != gpi_producer_open(g_pe[g_my_pe].in.ring, GPI_PROD_HOST_SHARED,
                               &g_pe[g_my_pe].self_prod)) {
        SPML_GPI_ERROR("self gpi_producer_open failed");
        return OSHMEM_ERROR;
    }
    g_pe[g_my_pe].self_prod_open = 1;

    if (!g_hook_registered) {
        opal_progress_register(gpi_spml_progress);
        g_hook_registered = 1;
    }
    return OSHMEM_SUCCESS;
}

int mca_spml_gpi_del_procs(struct oshmem_group_t *group, size_t nprocs)
{
    unsigned q;

    (void) group; (void) nprocs;

    if (NULL == g_pe) {
        return OSHMEM_SUCCESS;
    }
    for (q = 0; q < g_nprocs; q++) {
        gpi_pe_t *p = &g_pe[q];
        if (p->emu_started) {
            gpi_emu_stop(p->emu);   /* wakes the waiter, joins the thread */
            p->emu_started = 0;
        }
        if (p->out_imported) {
            gpi_shm_chan_close(&p->out); /* consumer view: munmap only */
            p->out_imported = 0;
        }
        if (p->in_created) {
            gpi_shm_chan_close(&p->in);
            shm_unlink(p->in.obj);  /* owner cleanup: close, then unlink */
            p->in_created = 0;
        }
    }
    free(g_pe);
    g_pe = NULL;
    return OSHMEM_SUCCESS;
}

int mca_spml_gpi_enable(bool enable)
{
    (void) enable;
    return OSHMEM_SUCCESS;
}

/* ------------------------------------------------------------------ *
 * registration
 *
 * memheap calls register(va_base, va_len, shmid) once per segment BEFORE
 * the mkey exchange. The identity adopt (remap MAP_SHARED at the same va)
 * lets the engine resolve both sides through bind slots; both bind
 * partitions of in[me] are stamped here because memheap never
 * self-unpacks (memheap_base_mkey.c skips i == my_pe).
 * ------------------------------------------------------------------ */

sshmem_mkey_t *mca_spml_gpi_register(void *addr, size_t size, uint64_t shmid,
                                     int *count)
{
    gpi_spml_wire_t *w;
    gpi_bind_handle_t published;
    sshmem_mkey_t *mkeys;
    uint64_t off;
    unsigned q, r;
    int seg;

    *count = 0;

    if ((uint32_t) memheap_map->n_segments > GPI_SPML_MAX_SEGS) {
        SPML_GPI_ERROR("%u memheap segments exceed bind capacity %u",
                       memheap_map->n_segments, GPI_SPML_MAX_SEGS);
        return NULL;
    }
    seg = gpi_seg_of(addr, &off);
    if (seg < 0 || addr != memheap_map->mem_segs[seg].super.va_base) {
        SPML_GPI_ERROR("register: %p is not a segment base", addr);
        return NULL;
    }
    if (g_adopt_count >= GPI_SPML_MAX_SEGS) {
        SPML_GPI_ERROR("register: adopt overflow");
        return NULL;
    }

    snprintf(g_adopt_obj[g_adopt_count], GPI_SHM_MAX_NAME,
             "/gpi-h%up%us%d", g_my_pe, (int) getpid(), seg);

    if (0 != gpi_shm_bind_adopt(g_domain, addr, size,
                                g_adopt_obj[g_adopt_count], &published)) {
        SPML_GPI_ERROR("gpi_shm_bind_adopt seg %d (%p, %zu) failed",
                       seg, addr, size);
        return NULL;
    }
    g_adopt_count++;

    w = (gpi_spml_wire_t *) calloc(1, sizeof(*w));
    if (NULL == w) {
        return NULL;
    }
    w->magic   = GPI_SPML_WIRE_MAGIC;
    w->version = GPI_SPML_WIRE_VERSION;
    w->my_pe   = (int32_t) g_my_pe;
    w->n_procs = (int32_t) g_nprocs;
    w->segno   = (uint32_t) seg;
    w->n_segs  = memheap_map->n_segments;
    w->seg_len = size;
    w->seg.published = published;
    memcpy(w->seg.obj, g_adopt_obj[g_adopt_count - 1], GPI_SHM_MAX_NAME);
    for (r = 0; r < g_nprocs; r++) {
        w->chans[r].handle     = g_pe[r].in_handle;
        w->chans[r].creator_pe = (uint32_t) g_my_pe;
    }

    /* partition B on every inbound channel: our seg, identity va.
     * On in[me] also partition A (producer == owner): self bootstrap. */
    for (q = 0; q < g_nprocs; q++) {
        gpi_pe_t *p = &g_pe[q];
        p->in.binds[GPI_SPML_MAX_SEGS + seg].base_addr =
            (uint64_t) (uintptr_t) addr;
        p->in.binds[GPI_SPML_MAX_SEGS + seg].length = size;
        if (q == g_my_pe) {
            p->in.binds[seg].base_addr = (uint64_t) (uintptr_t) addr;
            p->in.binds[seg].length    = size;
            p->a_mask |= (1u << seg);
        }
    }
    g_b_mask |= (1u << seg);

    mkeys = (sshmem_mkey_t *) calloc(1, sizeof(*mkeys));
    if (NULL == mkeys) {
        free(w);
        return NULL;
    }
    /* nonzero va_base → memheap packs u.data (the blob) on the wire */
    mkeys[0].va_base      = addr;
    mkeys[0].len          = (uint16_t) sizeof(*w);
    mkeys[0].u.data       = w;
    mkeys[0].spml_context = NULL;
    *count = 1;
    (void) shmid;
    return mkeys;
}

int mca_spml_gpi_deregister(sshmem_mkey_t *mkeys)
{
    /* local mkeys only (ucx-verbatim ownership): we allocated both */
    if (NULL != mkeys) {
        if (mkeys->len > 0) {
            free(mkeys->u.data);
        }
        free(mkeys);
    }
    return OSHMEM_SUCCESS;
}

/* ------------------------------------------------------------------ *
 * rmkey_unpack: stamp partition A of in[pe]; import out[pe] once
 * ------------------------------------------------------------------ */

void mca_spml_gpi_rmkey_unpack(shmem_ctx_t ctx, sshmem_mkey_t *mkey,
                               uint32_t segno, int remote_pe, int tr_id)
{
    gpi_spml_wire_t *w;
    gpi_pe_t *q;
    gpi_bind_handle_t local;
    void *base = NULL;
    size_t len = 0;

    (void) ctx; (void) tr_id;

    if (remote_pe < 0 || (unsigned) remote_pe >= g_nprocs) {
        SPML_GPI_ERROR("unpack: bad pe %d", remote_pe);
        return;
    }
    if (segno >= GPI_SPML_MAX_SEGS) {
        SPML_GPI_ERROR("unpack: seg %u out of range", segno);
        return;
    }
    if (NULL == mkey->u.data ||
        mkey->len < (uint16_t) sizeof(gpi_spml_wire_t)) {
        SPML_GPI_ERROR("unpack from pe %d: short blob (%u B)", remote_pe,
                       (unsigned) mkey->len);
        return;
    }
    w = (gpi_spml_wire_t *) mkey->u.data;
    if (GPI_SPML_WIRE_MAGIC != w->magic ||
        GPI_SPML_WIRE_VERSION != w->version ||
        w->my_pe != remote_pe ||
        (uint32_t) w->n_procs != g_nprocs ||
        w->segno != segno ||
        0 == w->n_segs || w->n_segs > GPI_SPML_MAX_SEGS ||
        w->n_segs != (uint32_t) memheap_map->n_segments) {
        SPML_GPI_ERROR("unpack pe %d seg %u: wire gate failed "
                       "(my_pe %d nprocs %u nsegs %u/%u)",
                       remote_pe, segno, w->my_pe, (unsigned) w->n_procs,
                       (unsigned) w->n_segs, memheap_map->n_segments);
        return;
    }
    q = &g_pe[remote_pe];

    /* partition A stamp: map pe's seg `segno` locally; bind_map records
     * nothing — the caller owns the chan.binds slot write */
    if (0 != gpi_shm_bind_map(&w->seg.published, w->seg.obj, &local,
                              &base, &len)) {
        SPML_GPI_ERROR("gpi_shm_bind_map seg %u from pe %d failed",
                       segno, remote_pe);
        return;
    }
    q->in.binds[segno].base_addr = (uint64_t) (uintptr_t) base;
    q->in.binds[segno].length    = len;
    q->a_mask |= (1u << segno);

    /* one-shot: import pe's in[me] (the channel WE produce into) */
    if (!q->out_imported) {
        const gpi_spml_chan_record_t *cr = &w->chans[g_my_pe];
        if ((uint32_t) remote_pe != cr->creator_pe) {
            SPML_GPI_ERROR("chan record from pe %d claims creator %u",
                           remote_pe, cr->creator_pe);
            return;
        }
        if (0 > gpi_shm_chan_import_record(&cr->handle, &q->out)) {
            SPML_GPI_ERROR("chan import from pe %d failed", remote_pe);
            return;
        }
        q->out_imported = 1;
        if (0 != gpi_producer_open(q->out.ring, GPI_PROD_HOST_SHARED,
                                   &q->prod)) {
            SPML_GPI_ERROR("producer_open pe %d failed", remote_pe);
            return;
        }
        q->prod_open = 1;
    }
}

void mca_spml_gpi_rmkey_free(sshmem_mkey_t *mkey, int pe)
{
    /* remote copies: memheap frees u.data and the cache entry itself
     * after this callback. Bind views live in the process-scoped domain;
     * S1 keeps them until fini (segments deregister rarely; leaks would
     * be per-deregister only). */
    (void) mkey; (void) pe;
}

void *mca_spml_gpi_rmkey_ptr(const void *dst_addr, sshmem_mkey_t *mkey, int pe)
{
    uint64_t off;
    int seg;
    uint64_t base;

    (void) mkey;
    /* The remote segment is mapped into MY address space by bind_map at
     * rmkey_unpack, at an arbitrary va recorded in in[pe].binds[A]. Translate
     * the (globally symmetric) dst_addr through that view. NULL when the
     * segment is not mapped yet (shmem_ptr then reports no direct access). */
    if (pe < 0 || (unsigned) pe >= g_nprocs) {
        return NULL;
    }
    seg = gpi_seg_of(dst_addr, &off);
    if (seg < 0) {
        return NULL;
    }
    base = g_pe[pe].in.binds[seg].base_addr;
    if (0 == base) {
        return NULL;
    }
    return (void *) (uintptr_t) (base + off);
}
/* ------------------------------------------------------------------ *
 * RMA core
 *
 * Stamp formulas, on the channel the descriptor executes on (always
 * producer→owner in[?] sense):
 *   put:  src_h = PROD(srcseg)   dst_h = OWN(dstseg)
 *   get:  src_h = OWN(srcseg)    dst_h = PROD(dstseg)
 * self traffic runs on in[me] where PROD(j) == OWN(j) (same identity
 * mapping stamped twice at register), so the peer formulas specialize
 * correctly with srcseg == dstseg.
 * ------------------------------------------------------------------ */

static inline gpi_producer_t *gpi_tx_producer(gpi_pe_t *q, int peer)
{
    return (peer == (int) g_my_pe) ? &q->self_prod : &q->prod;
}

static inline int gpi_tx_ready(gpi_pe_t *q, int peer)
{
    return (peer == (int) g_my_pe) ? q->self_prod_open : q->prod_open;
}

/* publish one counted descriptor to peer `peer` */
static inline void gpi_post(gpi_pe_t *q, int peer, const gpi_desc_t *d)
{
    gpi_producer_t *pr = gpi_tx_producer(q, peer);
    uint64_t base = gpi_reserve(pr, 1);
    if (UINT64_MAX == base) { /* only n==0/n>depth fail; defensive */
        SPML_GPI_ERROR("gpi_reserve pe %d failed", peer);
        return;
    }
    gpi_publish_one(pr, d, base);
    q->counted_posted++;
    q->dirty = 1;
}

int mca_spml_gpi_put_nb(shmem_ctx_t ctx, void *dst_addr, size_t size,
                        void *src_addr, int dst, void **handle)
{
    gpi_pe_t *q;
    gpi_desc_t d;
    uint64_t src_off, dst_off;
    int srcseg, dstseg;

    (void) ctx;
    if (NULL != handle) {
        *handle = NULL; /* completion is quiet/wait_nb, not per-op */
    }
    if (0 == size) {
        return OSHMEM_SUCCESS;
    }
    if (dst < 0 || (unsigned) dst >= g_nprocs) {
        return OSHMEM_ERR_BAD_PARAM;
    }
    srcseg = gpi_seg_of(src_addr, &src_off);
    dstseg = gpi_seg_of(dst_addr, &dst_off); /* identity: local scan valid */
    if (srcseg < 0 || dstseg < 0) {
        SPML_GPI_ERROR("put: %p or %p outside the symmetric heap",
                       src_addr, dst_addr);
        return OSHMEM_ERR_BAD_PARAM;
    }
    q = &g_pe[dst];
    if (!gpi_tx_ready(q, dst)) {
        SPML_GPI_ERROR("put to pe %d before the channel is open", dst);
        return OSHMEM_ERR_NOT_AVAILABLE;
    }

    gpi_desc_set_write(&d, (uint32_t) dst,
                       GPI_SPML_BIND_PROD(srcseg), src_off,
                       GPI_SPML_BIND_OWN(dstseg), dst_off,
                       size, GPI_SPML_COUNTER_ID, GPI_SPML_DESC_FLAGS);
    gpi_post(q, dst, &d);
    return OSHMEM_SUCCESS;
}

int mca_spml_gpi_get_nb(shmem_ctx_t ctx, void *dst_addr, size_t size,
                        void *src_addr, int src, void **handle)
{
    gpi_pe_t *q;
    gpi_desc_t d;
    uint64_t dst_off, src_off;
    int srcseg, dstseg;

    (void) ctx;
    if (NULL != handle) {
        *handle = NULL;
    }
    if (0 == size) {
        return OSHMEM_SUCCESS;
    }
    if (src < 0 || (unsigned) src >= g_nprocs) {
        return OSHMEM_ERR_BAD_PARAM;
    }
    dstseg = gpi_seg_of(dst_addr, &dst_off);
    srcseg = gpi_seg_of(src_addr, &src_off);
    if (srcseg < 0 || dstseg < 0) {
        SPML_GPI_ERROR("get: %p or %p outside the symmetric heap",
                       dst_addr, src_addr);
        return OSHMEM_ERR_BAD_PARAM;
    }
    q = &g_pe[src];
    if (!gpi_tx_ready(q, src)) {
        return OSHMEM_ERR_NOT_AVAILABLE;
    }

    /* READ executes on src's engine over its in[me]: the source is the
     * channel OWNER's memory (partition B), the destination is the
     * PRODUCER's memory src mapped (partition A). */
    gpi_desc_set_read(&d, (uint32_t) src,
                      GPI_SPML_BIND_PROD(dstseg), dst_off,
                      GPI_SPML_BIND_OWN(srcseg), src_off,
                      size, GPI_SPML_COUNTER_ID, GPI_SPML_DESC_FLAGS);
    gpi_post(q, src, &d);
    return OSHMEM_SUCCESS;
}

int mca_spml_gpi_put(shmem_ctx_t ctx, void *dst_addr, size_t size,
                     void *src_addr, int dst)
{
    int rc = mca_spml_gpi_put_nb(ctx, dst_addr, size, src_addr, dst, NULL);
    if (OSHMEM_SUCCESS != rc) {
        return rc;
    }
    return mca_spml_gpi_quiet(ctx);
}

int mca_spml_gpi_get(shmem_ctx_t ctx, void *dst_addr, size_t size,
                     void *src_addr, int src)
{
    int rc = mca_spml_gpi_get_nb(ctx, dst_addr, size, src_addr, src, NULL);
    if (OSHMEM_SUCCESS != rc) {
        return rc;
    }
    return mca_spml_gpi_quiet(ctx);
}

/* ------------------------------------------------------------------ *
 * put with signal
 *
 * libgpi's emu has no SET signal core reachable from its switch (the
 * apply_signal SET branch needs !add && !counted; every caller passes
 * one or the other) and apply_signal resolves the signal id against the
 * CHANNEL signal table, not memory — so both cases ride memory ops on
 * the owner's partition B handle:
 *   SET: 8-byte WRITE_INLINE of the value (exact store)
 *   ADD: FETCH_OP ADD with the 64-bit operand in the handle pair words;
 *        the fetched-old bounce write is sunk into the channel signal
 *        word (GPI_SPML_BOUNCE_ID) so user memory is untouched.
 * Both are counted like every other descriptor, so a later quiet covers
 * them and ring FIFO gives data-before-signal order. sig_addr must be
 * 8-byte aligned (FETCH_OP operates on uint64).
 * ------------------------------------------------------------------ */

static int gpi_put_signal_op(shmem_ctx_t ctx, void *dst_addr, size_t size,
                             void *src_addr, uint64_t *sig_addr,
                             uint64_t signal, int sig_op, int pe)
{
    gpi_pe_t *q;
    gpi_desc_t d;
    uint64_t sig_off;
    int sigseg, rc;

    if (NULL == sig_addr || (uintptr_t) sig_addr & 7u) {
        return OSHMEM_ERR_BAD_PARAM;
    }
    rc = mca_spml_gpi_put_nb(ctx, dst_addr, size, src_addr, pe, NULL);
    if (OSHMEM_SUCCESS != rc) {
        return rc;
    }
    sigseg = gpi_seg_of(sig_addr, &sig_off);
    if (sigseg < 0) {
        return OSHMEM_ERR_BAD_PARAM;
    }
    q = &g_pe[pe];
    if (!gpi_tx_ready(q, pe)) {
        return OSHMEM_ERR_NOT_AVAILABLE;
    }
    /* the data op already posted; FIFO gives data-before-signal order */

    if (SHMEM_SIGNAL_SET == sig_op) {
        gpi_desc_set_write_inline(&d, (uint32_t) pe, signal,
                                  GPI_SPML_BIND_OWN(sigseg), sig_off,
                                  8, GPI_SPML_COUNTER_ID,
                                  GPI_SPML_DESC_FLAGS);
    } else {
        gpi_desc_set_fetch_op(&d, (uint32_t) pe, GPI_FETCH_ADD,
                              GPI_SPML_BIND_OWN(sigseg), sig_off, signal,
                              GPI_SPML_BOUNCE_ID, 0,
                              GPI_SPML_COUNTER_ID, GPI_SPML_DESC_FLAGS);
    }
    gpi_post(q, pe, &d);
    return OSHMEM_SUCCESS;
}

int mca_spml_gpi_put_signal(shmem_ctx_t ctx, void *dst_addr, size_t size,
                            void *src_addr, uint64_t *sig_addr,
                            uint64_t signal, int sig_op, int dst)
{
    int rc = gpi_put_signal_op(ctx, dst_addr, size, src_addr, sig_addr,
                               signal, sig_op, dst);
    if (OSHMEM_SUCCESS != rc) {
        return rc;
    }
    return mca_spml_gpi_quiet(ctx);
}

int mca_spml_gpi_put_signal_nb(shmem_ctx_t ctx, void *dst_addr, size_t size,
                               void *src_addr, uint64_t *sig_addr,
                               uint64_t signal, int sig_op, int dst)
{
    return gpi_put_signal_op(ctx, dst_addr, size, src_addr, sig_addr,
                             signal, sig_op, dst);
}

/* ------------------------------------------------------------------ *
 * fence / quiet
 * ------------------------------------------------------------------ */

int mca_spml_gpi_fence(shmem_ctx_t ctx)
{
    (void) ctx;
    /* Per-destination ordering is structural: one FIFO ring per ordered
     * pair, executed in order. fence is a no-op. */
    return OSHMEM_SUCCESS;
}

int mca_spml_gpi_quiet(shmem_ctx_t ctx)
{
    gpi_desc_t d;
    unsigned q;
    int pending;

    (void) ctx;
    if (NULL == g_pe) {
        return OSHMEM_SUCCESS;
    }

    /* one counted CHANNEL_FLUSH per dirty outbound channel; FIFO means it
     * retires after every earlier descriptor on that ring */
    for (q = 0; q < g_nprocs; q++) {
        gpi_pe_t *p = &g_pe[q];
        if (!p->dirty) {
            continue;
        }
        if (!gpi_tx_ready(p, (int) q)) {
            p->dirty = 0; /* nothing ever posted through it */
            continue;
        }
        gpi_desc_set_ctrl(&d, GPI_CTRL_CHANNEL_FLUSH, GPI_SPML_COUNTER_ID,
                          GPI_SPML_DESC_FLAGS);
        gpi_post(p, (int) q, &d);
        p->dirty = 0;
    }

    /* poll every channel's counter to its posted target. Completion of
     * out[q] advances PE q's counter table — mapped into my address space
     * by the import. Self traffic completes through in[me] via the
     * progress hook below. */
    for (;;) {
        pending = 0;
        for (q = 0; q < g_nprocs; q++) {
            gpi_pe_t *p = &g_pe[q];
            volatile uint64_t *ctr;
            uint64_t word;

            if (p->counted_posted == 0) {
                continue;
            }
            if (q == g_my_pe) {
                if (!p->emu_started) {
                    /* engine never ran: force a progress pass */
                    gpi_spml_progress();
                }
                ctr = p->in.counters;
            } else {
                if (!p->out_imported) {
                    continue; /* never opened: nothing in flight */
                }
                ctr = p->out.counters;
            }
            word = *ctr;
            if (word & GPI_COUNTER_FLAG_ERROR) {
                p->chan_error = 1;
                SPML_GPI_ERROR("channel to pe %u: engine error flagged "
                               "(counted %lu of %lu)", q,
                               (unsigned long) (word & GPI_COUNTER_SUCCESS_MASK),
                               (unsigned long) p->counted_posted);
                return OSHMEM_ERROR;
            }
            if ((word & GPI_COUNTER_SUCCESS_MASK) < p->counted_posted) {
                pending = 1;
            }
        }
        if (!pending) {
            break;
        }
        opal_progress();
    }
    if (g_pe[g_my_pe].chan_error) {
        return OSHMEM_ERROR;
    }
    return OSHMEM_SUCCESS;
}

/* ------------------------------------------------------------------ *
 * scalar ivars
 * ------------------------------------------------------------------ */

int mca_spml_gpi_wait(void *addr, int cmp, void *value, int datatype)
{
    return mca_spml_base_wait(addr, cmp, value, datatype);
}

int mca_spml_gpi_wait_nb(void *value)
{
    if (NULL == value) {
        /* put_nb/get_nb hand out NULL handles: nothing per-op to wait on */
        return OSHMEM_SUCCESS;
    }
    return mca_spml_base_wait_nb(value);
}

int mca_spml_gpi_test(void *addr, int cmp, void *value, int datatype,
                      int *out_value)
{
    return mca_spml_base_test(addr, cmp, value, datatype, out_value);
}

/* ------------------------------------------------------------------ *
 * ivar vectors (OpenSHMEM 1.6 wait/test all/any/some)
 *
 * Built on the per-element base helpers. status[]: this oshmem baseline
 * ships no SHMEM_STATUS_* headers (ucx aborts on the whole family), so we
 * carry the spec bit locally: an element is waited/tested only while its
 * status entry has the waitable bit set (SHMEM_STATUS_FETCH=0 and
 * SHMEM_STATUS_INVALID=1 both fail the bit test).
 * ------------------------------------------------------------------ */

#define GPI_SPML_STATUS_IS_WAITABLE 2

static inline int gpi_ivar_skip(const int *status, size_t i)
{
    return status && !(status[i] & GPI_SPML_STATUS_IS_WAITABLE);
}

static size_t gpi_type_size(int datatype)
{
    switch (datatype) {
    case SHMEM_SHORT:
    case SHMEM_USHORT:
        return sizeof(short);
    case SHMEM_INT:
    case SHMEM_UINT:
    case SHMEM_INT32_T:
    case SHMEM_UINT32_T:
        return 4;
    case SHMEM_LONG:
    case SHMEM_ULONG:
    case SHMEM_LLONG:
    case SHMEM_ULLONG:
    case SHMEM_INT64_T:
    case SHMEM_UINT64_T:
    case SHMEM_SIZE_T:
    case SHMEM_PTRDIFF_T:
        return 8;
    case SHMEM_FINT:
        return sizeof(ompi_fortran_integer_t);
    case SHMEM_FINT4:
        return 4;
    case SHMEM_FINT8:
        return 8;
    default:
        return 8;
    }
}

static int gpi_ivar_true(void *elem, int cmp, const void *value, int datatype)
{
    int out = 0;
    mca_spml_base_test(elem, cmp, (void *) value, datatype, &out);
    return out;
}

void mca_spml_gpi_wait_until_all(void *ivars, int cmp, void *cmp_value,
                                 size_t nelems, const int *status,
                                 int datatype)
{
    size_t sz = gpi_type_size(datatype);
    size_t i;

    for (i = 0; i < nelems; i++) {
        if (gpi_ivar_skip(status, i)) {
            continue;
        }
        mca_spml_base_wait((char *) ivars + i * sz, cmp, cmp_value, datatype);
    }
}

void mca_spml_gpi_wait_until_all_vector(void *ivars, int cmp,
                                        void *cmp_values, size_t nelems,
                                        const int *status, int datatype)
{
    size_t sz = gpi_type_size(datatype);
    size_t i;

    for (i = 0; i < nelems; i++) {
        if (gpi_ivar_skip(status, i)) {
            continue;
        }
        mca_spml_base_wait((char *) ivars + i * sz, cmp,
                           (char *) cmp_values + i * sz, datatype);
    }
}

static size_t gpi_ivar_scan(void *ivars, int cmp, const void *cmp_values,
                            size_t nelems, const int *status, int datatype,
                            int vector, size_t *indices)
{
    size_t sz = gpi_type_size(datatype);
    size_t i, n = 0;

    for (i = 0; i < nelems; i++) {
        const void *val = vector ? (const char *) cmp_values + i * sz
                                 : cmp_values;
        if (gpi_ivar_skip(status, i)) {
            continue;
        }
        if (gpi_ivar_true((char *) ivars + i * sz, cmp, val, datatype)) {
            if (NULL == indices) {
                return i + 1; /* any-semantics: first hit */
            }
            indices[n++] = i;
        }
    }
    return n;
}

size_t mca_spml_gpi_wait_until_any(void *ivars, int cmp, void *cmp_value,
                                   size_t nelems, const int *status,
                                   int datatype)
{
    size_t hit;
    for (;;) {
        hit = gpi_ivar_scan(ivars, cmp, cmp_value, nelems, status, datatype,
                            0, NULL);
        if (hit) {
            return hit - 1;
        }
        opal_progress();
    }
}

size_t mca_spml_gpi_wait_until_any_vector(void *ivars, int cmp,
                                          void *cmp_values, size_t nelems,
                                          const int *status, int datatype)
{
    size_t hit;
    for (;;) {
        hit = gpi_ivar_scan(ivars, cmp, cmp_values, nelems, status, datatype,
                            1, NULL);
        if (hit) {
            return hit - 1;
        }
        opal_progress();
    }
}

size_t mca_spml_gpi_wait_until_some(void *ivars, int cmp, void *cmp_value,
                                    size_t nelems, size_t *indices,
                                    const int *status, int datatype)
{
    size_t n;
    for (;;) {
        n = gpi_ivar_scan(ivars, cmp, cmp_value, nelems, status, datatype,
                          0, indices);
        if (n) {
            return n;
        }
        opal_progress();
    }
}

size_t mca_spml_gpi_wait_until_some_vector(void *ivars, int cmp,
                                           void *cmp_values, size_t nelems,
                                           size_t *indices, const int *status,
                                           int datatype)
{
    size_t n;
    for (;;) {
        n = gpi_ivar_scan(ivars, cmp, cmp_values, nelems, status, datatype,
                          1, indices);
        if (n) {
            return n;
        }
        opal_progress();
    }
}

int mca_spml_gpi_test_all(void *ivars, int cmp, void *cmp_value,
                          size_t nelems, const int *status, int datatype)
{
    size_t sz = gpi_type_size(datatype);
    size_t i;

    for (i = 0; i < nelems; i++) {
        if (gpi_ivar_skip(status, i)) {
            continue;
        }
        if (!gpi_ivar_true((char *) ivars + i * sz, cmp, cmp_value, datatype)) {
            return 0;
        }
    }
    return 1;
}

int mca_spml_gpi_test_all_vector(void *ivars, int cmp, void *cmp_values,
                                 size_t nelems, const int *status,
                                 int datatype)
{
    size_t sz = gpi_type_size(datatype);
    size_t i;

    for (i = 0; i < nelems; i++) {
        if (gpi_ivar_skip(status, i)) {
            continue;
        }
        if (!gpi_ivar_true((char *) ivars + i * sz, cmp,
                           (char *) cmp_values + i * sz, datatype)) {
            return 0;
        }
    }
    return 1;
}

size_t mca_spml_gpi_test_any(void *ivars, int cmp, void *cmp_value,
                             size_t nelems, const int *status, int datatype)
{
    size_t hit = gpi_ivar_scan(ivars, cmp, cmp_value, nelems, status,
                               datatype, 0, NULL);
    return hit ? hit - 1 : SIZE_MAX;
}

size_t mca_spml_gpi_test_any_vector(void *ivars, int cmp, void *cmp_values,
                                    size_t nelems, const int *status,
                                    int datatype)
{
    size_t hit = gpi_ivar_scan(ivars, cmp, cmp_values, nelems, status,
                               datatype, 1, NULL);
    return hit ? hit - 1 : SIZE_MAX;
}

size_t mca_spml_gpi_test_some(void *ivars, int cmp, void *cmp_value,
                              size_t nelems, size_t *indices,
                              const int *status, int datatype)
{
    return gpi_ivar_scan(ivars, cmp, cmp_value, nelems, status, datatype,
                         0, indices);
}

size_t mca_spml_gpi_test_some_vector(void *ivars, int cmp, void *cmp_values,
                                     size_t nelems, size_t *indices,
                                     const int *status, int datatype)
{
    return gpi_ivar_scan(ivars, cmp, cmp_values, nelems, status, datatype,
                         1, indices);
}

/* ------------------------------------------------------------------ *
 * send/recv — MPI plumbing, same as ucx (these are internal bootstrap
 * paths, not the RMA data plane)
 * ------------------------------------------------------------------ */

int mca_spml_gpi_recv(void *buf, size_t count, int src_pe)
{
    return MCA_PML_CALL(recv(buf, count, &ompi_mpi_unsigned_char.dt, src_pe,
                             0, &ompi_mpi_comm_world.comm, NULL));
}

int mca_spml_gpi_send(void *buf, size_t count, int dst_pe,
                      mca_spml_base_put_mode_t mode)
{
    return MCA_PML_CALL(send(buf, count, &ompi_mpi_unsigned_char.dt, dst_pe,
                             0, (mca_pml_base_send_mode_t) mode,
                             &ompi_mpi_comm_world.comm));
}

/* ------------------------------------------------------------------ *
 * teams + collectives (S1: minimal local; WORLD barrier via MPI; no
 * team-scoped collectives — the scoll stack owns those, non-abstaining
 * NOT_IMPLEMENTED so callers get an error, not a trap)
 * ------------------------------------------------------------------ */

int mca_spml_gpi_ctx_create(long options, shmem_ctx_t *ctx)
{
    (void) options; (void) ctx;
    return OSHMEM_ERR_NOT_IMPLEMENTED; /* S1: default ctx only */
}

void mca_spml_gpi_ctx_destroy(shmem_ctx_t ctx)
{
    (void) ctx; /* only the default ctx exists; nothing to free */
}

int mca_spml_gpi_team_sync(shmem_team_t team)
{
    if (SHMEM_TEAM_WORLD == team) {
        oshmem_shmem_barrier();
        return OSHMEM_SUCCESS;
    }
    return OSHMEM_ERR_NOT_IMPLEMENTED;
}

int mca_spml_gpi_team_my_pe(shmem_team_t team)
{
    return (SHMEM_TEAM_WORLD == team) ? shmem_my_pe() : -1;
}

int mca_spml_gpi_team_n_pes(shmem_team_t team)
{
    return (SHMEM_TEAM_WORLD == team) ? shmem_n_pes() : -1;
}

int mca_spml_gpi_team_get_config(shmem_team_t team, long config_mask,
                                 shmem_team_config_t *config)
{
    (void) team; (void) config_mask; (void) config;
    return OSHMEM_ERR_NOT_IMPLEMENTED;
}

int mca_spml_gpi_team_translate_pe(shmem_team_t src_team, int src_pe,
                                   shmem_team_t dest_team)
{
    if (SHMEM_TEAM_WORLD == src_team && SHMEM_TEAM_WORLD == dest_team) {
        return src_pe;
    }
    return -1;
}

int mca_spml_gpi_team_split_strided(shmem_team_t parent_team, int start,
                                    int stride, int size,
                                    const shmem_team_config_t *config,
                                    long config_mask,
                                    shmem_team_t *new_team)
{
    (void) parent_team; (void) start; (void) stride; (void) size;
    (void) config; (void) config_mask; (void) new_team;
    return OSHMEM_ERR_NOT_IMPLEMENTED;
}

int mca_spml_gpi_team_split_2d(shmem_team_t parent_team, int xrange,
                               const shmem_team_config_t *xaxis_config,
                               long xaxis_mask, shmem_team_t *xaxis_team,
                               const shmem_team_config_t *yaxis_config,
                               long yaxis_mask, shmem_team_t *yaxis_team)
{
    (void) parent_team; (void) xrange; (void) xaxis_config;
    (void) xaxis_mask; (void) xaxis_team; (void) yaxis_config;
    (void) yaxis_mask; (void) yaxis_team;
    return OSHMEM_ERR_NOT_IMPLEMENTED;
}

int mca_spml_gpi_team_destroy(shmem_team_t team)
{
    (void) team;
    return OSHMEM_ERR_NOT_IMPLEMENTED;
}

int mca_spml_gpi_team_get(shmem_ctx_t ctx, shmem_team_t *team)
{
    (void) ctx;
    if (NULL != team) {
        *team = SHMEM_TEAM_WORLD;
        return OSHMEM_SUCCESS;
    }
    return OSHMEM_ERR_BAD_PARAM;
}

int mca_spml_gpi_team_create_ctx(shmem_team_t team, long options,
                                 shmem_ctx_t *ctx)
{
    (void) options;
    if (SHMEM_TEAM_WORLD == team && NULL != ctx) {
        *ctx = oshmem_ctx_default;
        return OSHMEM_SUCCESS;
    }
    return OSHMEM_ERR_NOT_IMPLEMENTED;
}

int mca_spml_gpi_team_alltoall(shmem_team_t team, void *dest,
                               const void *source, size_t nelems,
                               int datatype)
{
    (void) team; (void) dest; (void) source; (void) nelems; (void) datatype;
    return OSHMEM_ERR_NOT_IMPLEMENTED;
}

int mca_spml_gpi_team_alltoalls(shmem_team_t team, void *dest,
                                const void *source, ptrdiff_t dst,
                                ptrdiff_t sst, size_t nelems, int datatype)
{
    (void) team; (void) dest; (void) source; (void) dst; (void) sst;
    (void) nelems; (void) datatype;
    return OSHMEM_ERR_NOT_IMPLEMENTED;
}

int mca_spml_gpi_team_broadcast(shmem_team_t team, void *dest,
                                const void *source, size_t nelems,
                                int PE_root, int datatype)
{
    (void) team; (void) dest; (void) source; (void) nelems;
    (void) PE_root; (void) datatype;
    return OSHMEM_ERR_NOT_IMPLEMENTED;
}

int mca_spml_gpi_team_collect(shmem_team_t team, void *dest,
                              const void *source, size_t nelems,
                              int datatype)
{
    (void) team; (void) dest; (void) source; (void) nelems; (void) datatype;
    return OSHMEM_ERR_NOT_IMPLEMENTED;
}

int mca_spml_gpi_team_fcollect(shmem_team_t team, void *dest,
                               const void *source, size_t nelems,
                               int datatype)
{
    (void) team; (void) dest; (void) source; (void) nelems; (void) datatype;
    return OSHMEM_ERR_NOT_IMPLEMENTED;
}

int mca_spml_gpi_team_reduce(shmem_team_t team, void *dest,
                             const void *source, size_t nreduce,
                             int operation, int datatype)
{
    (void) team; (void) dest; (void) source; (void) nreduce;
    (void) operation; (void) datatype;
    return OSHMEM_ERR_NOT_IMPLEMENTED;
}

/* ------------------------------------------------------------------ *
 * misc
 * ------------------------------------------------------------------ */

int mca_spml_gpi_oob_get_mkeys(shmem_ctx_t ctx, int pe, uint32_t seg,
                               sshmem_mkey_t *mkeys)
{
    (void) ctx; (void) pe; (void) seg; (void) mkeys;
    /* no self-computed mkeys: memheap falls through to its OOB pack, which
     * ships exactly the blob register() built */
    return OSHMEM_ERR_NOT_IMPLEMENTED;
}

void mca_spml_gpi_memuse_hook(void *addr, size_t size)
{
    (void) addr; (void) size; /* sshmem, not memheap growth accounting */
}

int mca_spml_gpi_put_all_nb(void *dest, const void *source, size_t size,
                            long *counter)
{
    int my_pe = oshmem_my_proc_id();
    long val = 1;
    int peer, dst_pe, rc;

    /* mca_spml_base_put_all_nb is NOT_IMPLEMENTED; rotate like ucx. The
     * atomic lands on dst_pe's copy of counter (identity va), matching
     * the ucx vtable contract for this slot. */
    for (peer = 0; peer < (int) g_nprocs; peer++) {
        dst_pe = (peer + my_pe) % (int) g_nprocs;
        rc = mca_spml_gpi_put_nb(oshmem_ctx_default,
                                 (void *) ((uintptr_t) dest
                                           + (uintptr_t) my_pe * size),
                                 size,
                                 (void *) ((uintptr_t) source
                                           + (uintptr_t) dst_pe * size),
                                 dst_pe, NULL);
        if (OSHMEM_SUCCESS != rc) {
            return rc;
        }
        rc = MCA_ATOMIC_CALL(add(oshmem_ctx_default, (void *) counter, val,
                                 sizeof(val), dst_pe));
        if (OSHMEM_SUCCESS != rc) {
            return rc;
        }
    }
    return mca_spml_gpi_quiet(oshmem_ctx_default);
}
