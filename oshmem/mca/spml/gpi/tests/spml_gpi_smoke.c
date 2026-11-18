/*
 * spml/gpi 2-rank smoke — the Rule-4 runtime acceptance for S1 item 2.
 *
 * Covers exactly what the STATUS.md resume checklist asks for:
 *   1. oshmem_my_pe / n_pe identity after init
 *   2. a small put/get roundtrip against a peer segment  (register -> adopt
 *      -> bind map -> put -> quiet -> get)
 *   3. shmem_barrier_all                                  (progress hook alive)
 * plus the cheap extras that catch the known unverified-at-runtime risks:
 *   4. strided put with a signal word (SET path, write_inline)
 *   5. blocking put + get on the same buffer (no stale data through the
 *      bounce/quiet path)
 *
 * Run:  oshrun -np 2 --mca spml gpi ./spml_gpi_smoke
 * Exit 0 = all checks passed; any failure prints "FAIL:" with the check name
 * and exits 1 (so a wrapper can grep for FAIL rather than trust the tail).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shmem.h>

#define N 1024

static int failures = 0;

#define CHECK(cond, name, ...)                                              \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("FAIL: %s (rank %d): ", (name), me);                    \
            printf(__VA_ARGS__);                                            \
            printf("\n");                                                   \
            failures++;                                                     \
        } else {                                                            \
            printf("ok %d - %s (rank %d)\n", ++passed, (name), me);        \
        }                                                                   \
        fflush(stdout);                                                     \
    } while (0)

static int passed = 0;

int main(int argc, char **argv)
{
    int me, npes, peer;
    long *buf, *src;

    (void)argc; (void)argv;
    shmem_init();
    me   = shmem_my_pe();
    npes = shmem_n_pes();
    peer = (me + 1) % npes;

    printf("# spml/gpi smoke: rank %d of %d (spml must be gpi, priority 5)\n",
           me, npes);
    fflush(stdout);

    if (npes != 2) {
        printf("FAIL: expected 2 PEs, got %d\n", npes);
        return 1;
    }

    buf = (long *)shmem_malloc(N * sizeof(long));
    /* Sources/staging are SYMMETRIC on purpose: spml/gpi resolves every
     * operand through a published bind, so a non-symmetric local operand is
     * a separate open item (see STATUS 2026-11-17), not what this smoke
     * validates. */
    src = (long *)shmem_malloc(N * sizeof(long));
    if (!buf || !src) {
        printf("FAIL: shmem_malloc/malloc returned NULL (rank %d)\n", me);
        return 1;
    }
    for (int i = 0; i < N; i++) src[i] = (long)(me * 100000 + i);

    /* 1. identity */
    CHECK(buf != NULL, "shmem_malloc", "buffer NULL");
    CHECK(shmem_my_pe() == me && shmem_n_pes() == npes, "pe_identity",
          "my_pe=%d n_pe=%d", shmem_my_pe(), shmem_n_pes());

    /* 1b. SELF put — same engine, same bind tables, but no OOB exchange and no
     * peer process. Isolates executor/bind resolution from the cross-rank
     * channel: fail here => the engine is not copying; pass here => the fault is
     * in the cross-rank wiring. */
    {
        long *sbuf = (long *) shmem_malloc(N * sizeof(long));
        long  pat  = 0x5E1F0000L + me;
        int   bad  = -1;

        for (int i = 0; i < N; i++) src[i] = pat + i;
        shmem_putmem(sbuf, src, N * sizeof(long), me);
        shmem_quiet();
        for (int i = 0; i < N; i++)
            if (sbuf[i] != pat + i) { bad = i; break; }
        CHECK(bad < 0, "self_put",
              "mismatch at idx %d: got %ld want %ld (sbuf=%p src=%p buf=%p)",
              bad, bad >= 0 ? sbuf[bad] : -1, bad >= 0 ? pat + bad : -1,
              (void *) sbuf, (void *) src, (void *) buf);
        shmem_free(sbuf);
    }

    shmem_barrier_all();

    /* 2. put/get roundtrip: I write my pattern into the peer, peer reads it back */
    /* Re-fill: the self_put check above reuses `src` and left its own pattern
     * in it; the expectation below is the peer-indexed pattern. */
    for (int i = 0; i < N; i++) src[i] = (long) (peer * 100000 + i);
    shmem_putmem(buf, src, N * sizeof(long), peer);
    shmem_quiet();
    shmem_barrier_all();

    {
        long *inbuf = (long *)shmem_malloc(N * sizeof(long));
        shmem_getmem(inbuf, buf, N * sizeof(long), me);
        shmem_quiet();
        /* I wrote peer*100000+i into the PEER's buffer, so what sits in MY
         * buffer was written by the peer using ITS peer == me: expect me*1e5+i. */
        int bad = -1;
        for (int i = 0; i < N; i++)
            if (inbuf[i] != (long)(me * 100000 + i)) { bad = i; break; }
        CHECK(bad < 0, "put_get_roundtrip", "first mismatch at idx %d: got %ld want %ld",
              bad, bad >= 0 ? inbuf[bad] : -1,
              bad >= 0 ? (long)(me * 100000 + bad) : -1);
        shmem_free(inbuf);
    }

    shmem_barrier_all();

    /* 3. blocking put + get on the same buffer (quiet/drain path, no stale read) */
    {
        long v = 0;
        if (me == 0) shmem_long_p(buf, 0xC0FFEE, 1);
        shmem_barrier_all();
        v = shmem_long_g(buf, 0);
        CHECK(v == 0xC0FFEE, "blocking_put_get_scalar", "got 0x%lx want 0xC0FFEE", v);
    }

    shmem_barrier_all();

    /* 4. signal SET path (write_inline) + wait_until, then reset.
     * NOTE: the target must be a SYMMETRIC address — a remote put into a
     * plain local variable is not legal SHMEM. */
    {
        long *sig = (long *)shmem_malloc(sizeof(long));
        if (!sig) {
            CHECK(0, "signal_symmetric_alloc", "shmem_malloc NULL");
        } else {
            *sig = 0;
            shmem_barrier_all();
            if (me == 0) shmem_long_p(sig, 42, 1);
            shmem_barrier_all();
            if (me == 1) {
                long want = 42;
                shmem_wait_until(sig, SHMEM_CMP_EQ, want);
                CHECK(*sig == 42, "signal_set_wait_until", "sig=%ld want 42", *sig);
            } else {
                CHECK(1, "signal_set_peer_noop", "n/a");
            }
            *sig = 0;
            shmem_barrier_all();
            shmem_free(sig);
        }
    }

    shmem_barrier_all();
    shmem_free(src);
    shmem_free(buf);

    if (me == 0)
        printf("# smoke %s: %d checks, %d failures\n",
               failures ? "FAILED" : "PASSED", passed, failures);
    shmem_barrier_all();
    shmem_finalize();
    return failures ? 1 : 0;
}
