# SPML/UCX Memory Use and Profiling Plan

This document records every point where SPML/UCX may use memory, where each is initialized in the `shmem_init` path, and a plan to profile SPML/UCX memory usage as the number of processes scales.

---

## 1. Init flow: when SPML/UCX runs during `shmem_init`

Call chain:

```
shmem_init() / pshmem_init()
  → _shmem_init() [oshmem/shmem/c/shmem_init.c]
  → oshmem_shmem_init() [oshmem/runtime/oshmem_shmem_init.c]
       → ompi_mpi_init()
       → _shmem_init() [oshmem/runtime/oshmem_shmem_init.c]
```

Inside `_shmem_init()` the order relevant to SPML/UCX is:

| Step | Call | When SPML/UCX is involved |
|------|------|---------------------------|
| 1 | `mca_base_framework_open(&oshmem_spml_base_framework)` | SPML framework opened |
| 2 | `mca_spml_base_select(...)` | **SPML UCX component init**: `mca_spml_ucx_component_init()` → `spml_ucx_init()` — first SPML/UCX memory allocations |
| 3 | `MCA_SPML_CALL(enable(true))` | `mca_spml_ucx_enable()` — no allocations |
| 4 | `MCA_SPML_CALL(add_procs(oshmem_group_all, ...))` | **Bulk of SPML/UCX init allocations**: `mca_spml_ucx_add_procs()` |
| 5 | … memheap, scoll, atomic, etc. … | — |
| 6 | (after `_shmem_init` returns) | `get_all_mkeys()` → memheap exchanges keys → **SPML `rmkey_unpack`** per (PE, segment) → more SPML/UCX cache growth |
| 7 | `oshmem_shmem_preconnect_all()` | Uses existing connections; can trigger more mkey use |

So SPML/UCX memory is initialized in three main phases:

1. **Component init** (during `mca_spml_base_select`): `spml_ucx_init()` in `spml_ucx_component.c`.
2. **Add procs** (during `MCA_SPML_CALL(add_procs(...))`): `mca_spml_ucx_add_procs()` in `spml_ucx.c`.
3. **Key exchange** (during `get_all_mkeys()`): memheap calls `MCA_SPML_CALL(rmkey_unpack(...))` → `mca_spml_ucx_rmkey_unpack()` → `mca_spml_ucx_ctx_mkey_add()` and peer mkey cache growth in `spml_ucx.c`.

---

## 2. Every memory use point in SPML/UCX

### 2.1 Component init — `spml_ucx_init()` (spml_ucx_component.c)

Invoked from `mca_spml_ucx_component_init()` when SPML UCX is selected during `mca_spml_base_select()`.

| Location | Allocation | Scaling / notes |
|----------|------------|------------------|
| `spml_ucx_init()` ~L301 | **UCX internal**: `ucp_init(..., &mca_spml_ucx.ucp_context)` | UCX context; size depends on `params.estimated_num_eps` (= `ompi_proc_world_size()`), optional `estimated_num_ppn`. |
| `spml_ucx_init()` ~L320–322 | `active_array.ctxs` = `calloc(MCA_SPML_UCX_CTXS_ARRAY_SIZE, sizeof(mca_spml_ucx_ctx_t *))` | Fixed: 64 pointers. |
| `spml_ucx_init()` ~L321–322 | `idle_array.ctxs` = `calloc(MCA_SPML_UCX_CTXS_ARRAY_SIZE, ...)` | Fixed: 64 pointers. |
| `spml_ucx_init()` ~L337 | `mca_spml_ucx_ctx_default.ucp_worker` = `calloc(ucp_workers, sizeof(ucp_worker_h))` | **O(ucp_workers)**; default 1. |
| `spml_ucx_init()` ~L339–341 | **UCX internal**: `ucp_worker_create(ucp_context, ..., &ucp_worker[i])` per worker | Per-worker UCX structures. |
| `spml_ucx_init()` ~L344 | `mca_spml_ucx_rkey_store_init(&mca_spml_ucx_ctx_default.rkey_store)` | No heap yet; store is empty. |
| `spml_ucx_init()` ~L354–365 (async_progress) | `opal_progress_thread_init()`, `opal_event_alloc()`, `tick_event` | Small fixed overhead if async progress enabled. |

**Summary (component init):**  
Scaling is dominated by **UCX `ucp_init` / `ucp_worker_create`** (driven by `estimated_num_eps` = world size and `ucp_workers`). The rest is small and mostly constant.

---

### 2.2 Add procs — `mca_spml_ucx_add_procs()` (spml_ucx.c)

Invoked from `_shmem_init()` as `MCA_SPML_CALL(add_procs(oshmem_group_all, oshmem_group_all->proc_count))`.

| Location | Allocation | Scaling / notes |
|----------|------------|------------------|
| ~L648–649 | `wk_local_addr` = `calloc(ucp_workers, sizeof(ucp_address_t *))`, `wk_addr_len` = `calloc(ucp_workers, sizeof(size_t))` | **O(ucp_workers)**; temporary for address exchange. |
| ~L651 | `mca_spml_ucx_ctx_default.ucp_peers` = `calloc(nprocs, sizeof(ucp_peer_t))` | **O(nprocs)**. |
| ~L655–658 | `mca_spml_ucx_init_put_op_mask(&mca_spml_ucx_ctx_default, nprocs)` → `put_proc_indexes` = `malloc(nprocs * sizeof(int))`, `opal_bitmap_init(..., nprocs)` | **O(nprocs)** (strong ordering only). |
| ~L661–665 | **UCX**: `ucp_worker_get_address(worker[i], &wk_local_addr[i], &tmp_len)` | Address buffers; size varies by transport. |
| ~L667–670 | `oshmem_shmem_xchng(...)` — see below | **O(nprocs × ucp_workers)** for exchanged data. |
| ~L678–681 | `remote_addrs_tbl` = `calloc(ucp_workers)`, then per worker `calloc(nprocs, sizeof(char *))` | **O(ucp_workers × nprocs)** pointers. |
| ~L686–689 | For each (worker, pe): `remote_addrs_tbl[w][n]` = `malloc(wk_rsizes[i])` | **O(ucp_workers × nprocs)** buffers; size = remote address length. |
| ~L698–709 | **UCX**: `ucp_ep_create(worker[0], ..., &ucp_peers[i].ucp_conn)` per PE; `mca_spml_ucx_peer_mkey_cache_init(..., i)` | **O(nprocs)** EPs and peer structures; mkey arrays start empty. |
| ~L469–471 (del_procs) | `del_procs` = `malloc(sizeof(*del_procs) * nprocs)` | Used in error/teardown path; **O(nprocs)**. |

**`oshmem_shmem_xchng()` (used by add_procs) — temporary buffers:**

| Location | Allocation | Scaling / notes |
|----------|------------|------------------|
| ~L522 | `rcv_offsets` = `calloc(ucp_workers * nprocs, sizeof(*rcv_offsets))` | **O(nprocs × ucp_workers)**. |
| ~L528 | `rcv_sizes` = `calloc(ucp_workers * nprocs, sizeof(*rcv_sizes))` | **O(nprocs × ucp_workers)**. |
| ~L544–545 | `rcv_buf` = `calloc(1, total_size)` where total_size is sum of all received address lengths | **O(nprocs × ucp_workers × addr_len)**. |
| ~L554–555 | `_rcv_offsets`, `_rcv_sizes` = `calloc(nprocs, ...)` | **O(nprocs)**. |
| ~L569 | `_local_data` = `calloc(_local_size, 1)` | **O(local address size)**. |

**Summary (add_procs):**  
**O(nprocs)** for peers, EPs, and (if strong ordering) put_op_mask; **O(nprocs × ucp_workers)** for remote address table and exchange buffers. UCX `ucp_ep_create` adds internal memory per EP.

---

### 2.3 Key exchange — `get_all_mkeys()` → `mca_spml_ucx_rmkey_unpack()` / mkey cache

After `_shmem_init()` returns, `oshmem_shmem_init()` calls `MCA_MEMHEAP_CALL(get_all_mkeys())`. For each (PE, segment) memheap calls `MCA_SPML_CALL(rmkey_unpack(ctx, mkey, segno, pe, tr_id))` → `mca_spml_ucx_rmkey_unpack()` → `mca_spml_ucx_ctx_mkey_add()`.

| Location | Allocation | Scaling / notes |
|----------|------------|------------------|
| `mca_spml_ucx_ctx_mkey_add()` → `mca_spml_ucx_ctx_mkey_new()` → `mca_spml_ucx_peer_mkey_cache_add()` ~L321–337 | `ucp_peer->mkeys` extended with `realloc(..., mkeys_cnt * sizeof(...))`; new entry = `malloc(sizeof(spml_ucx_cached_mkey_t))` | **Per (PE, segment)**. Total **O(nprocs × n_segments)** cached mkeys and pointers. |
| `mca_spml_ucx_ctx_mkey_add()` ~L409–411 | **UCX**: `ucp_ep_rkey_unpack(..., mkey->u.data, &rkey)` | UCX rkey per (PE, segment). |
| `mca_spml_ucx_ctx_mkey_add()` ~L414–416 | If not local: `mca_spml_ucx_rkey_store_get(&rkey_store, ...)` | When `symmetric_rkey_max_count` > 0, rkey store grows; **O(unique rkeys)** up to cap. |
| memheap_base_mkey.c ~L174 | `mkey->u.data = malloc(mkey->len)` (in unpack path) | Memheap allocates packed rkey buffer; SPML then unpacks it; size is transport-dependent. |

**Summary (key exchange):**  
SPML/UCX memory grows by **O(nprocs × n_segments)** for peer mkey cache and UCX rkeys, plus rkey store if symmetric rkey dedup is enabled.

---

### 2.4 Runtime / registration — not init, but scale with usage

These are not part of `shmem_init` but affect total SPML/UCX memory as jobs scale or use more segments/contexts.

| Location | Allocation | Scaling / notes |
|----------|------------|------------------|
| `mca_spml_ucx_rkey_store_insert()` ~L224–234 | `realloc(store->array, size * sizeof(...))` when store is full | Grows up to `symmetric_rkey_max_count` (if set). |
| `mca_spml_ucx_register()` ~L877 | `mkeys` = `calloc(1, sizeof(sshmem_mkey_t))` | Per registration. |
| `mca_spml_ucx_register()` ~L905 | **UCX**: `ucp_mem_map(ucp_context, ..., &mem_h)` (if not UCX-allocated segment) | Per segment registration. |
| `mca_spml_ucx_register()` ~L915–916 | **UCX**: `ucp_rkey_pack(..., &mkeys[].u.data, &len)` | Packed rkey buffer per segment. |
| `mca_spml_ucx_ctx_create_common()` ~L1042–1066 | `ucx_ctx` = `malloc(...)`, `ucp_worker` = `calloc(1, ...)`, `ucp_peers` = `calloc(nprocs, ...)`, `put_proc_indexes` (strong ordering), `opal_bitmap_init(..., nprocs)` | **Per context**: **O(nprocs)**. |
| `mca_spml_ucx_ctx_create_common()` ~L1080–1094 | **UCX**: `ucp_ep_create()` per PE; `mca_spml_ucx_ctx_mkey_add()` per (PE, segment) from `memheap_map->mem_segs` | **O(nprocs × n_segments)** per additional context. |
| `_ctx_add()` ~L1002 | `array->ctxs` = `realloc(..., (ctxs_num + MCA_SPML_UCX_CTXS_ARRAY_INC) * ...)` | When active/idle context arrays are full; **O(number of contexts)**. |
| `mca_spml_ucx_team_split_strided()` ~L1899, L1906 | `ucx_new_team` = `malloc(...)`, `config` = `calloc(1, ...)` | Per team. |
| `mca_spml_ucx_del_procs()` ~L467, L369–378 | `del_procs`; later `free(remote_addrs_tbl[w][n])`, `free(remote_addrs_tbl[w])`, `free(remote_addrs_tbl)`, `free(ucp_peers)` | Teardown; frees add_procs and mkey-related data. |

---

## 3. Scaling summary (for profiling)

Approximate scaling of SPML/UCX-related memory:

| Component | Scaling | When allocated (init phase) |
|-----------|---------|-----------------------------|
| UCX context + default workers | ~constant + O(ucp_workers) + UCX internal (estimated_num_eps) | Component init |
| active_array / idle_array ctxs | Constant (64 ptrs each) | Component init |
| ucp_peers (default ctx) | **O(nprocs)** | Add procs |
| put_proc_indexes / put_op_bitmap | **O(nprocs)** (strong ordering) | Add procs |
| remote_addrs_tbl | **O(nprocs × ucp_workers)** (ptrs + buffer sizes) | Add procs |
| UCX EPs (default ctx) | **O(nprocs)** (UCX internal) | Add procs |
| Peer mkey cache (per PE) | **O(nprocs × n_segments)** (mkeys + UCX rkeys) | get_all_mkeys (after add_procs) |
| Rkey store | Up to symmetric_rkey_max_count | get_all_mkeys + later rkey_unpack |
| Extra contexts | **O(nprocs)** per ctx (peers, EPs, mkeys) | ctx_create (runtime) |

So the main scaling factors to profile are:

- **nprocs** (number of PEs)
- **ucp_workers** (default 1)
- **n_segments** (memheap segments: at least heap + static/data)
- **symmetric_rkey_max_count** (if > 0)

---

## 4. Plan to profile SPML/UCX memory usage with scale

### 4.1 Goals

- Measure **process virtual and resident memory** attributable to SPML/UCX as **nprocs** (and optionally ucp_workers / n_segments) increases.
- Identify which init phase (component init, add_procs, get_all_mkeys) contributes most to memory growth.
- Produce reproducible numbers for a few fixed scales (e.g., 2, 8, 32, 128, 512, 2K PEs) to guide tuning and reporting.

### 4.2 Approach

1. **Instrumentation**
   - Add optional **memory snapshots** at the end of:
     - `spml_ucx_init()` (component init),
     - `mca_spml_ucx_add_procs()` (add procs),
     - and immediately after `get_all_mkeys()` in `oshmem_shmem_init()`.
   - Snapshot = read process memory from `/proc/self/status` (or equivalent): e.g. `VmRSS`, `VmSize`, and optionally `VmPeak`.
   - Guard by an MCA parameter (e.g. `spml_ucx_profile_memory`) or env (e.g. `OSHMEM_SPML_UCX_PROFILE_MEMORY=1`) so it’s off by default.

2. **Stub / minimal executable**
   - Small OSHMEM program that only does `shmem_init()` then `shmem_finalize()` (no heap alloc, no barriers beyond what init does).
   - Run with 1, 2, 8, 32, 128, 512, 2048 PEs (or whatever the cluster allows).
   - Record for each run: nprocs, VmRSS/VmSize (and VmPeak if available) at each of the three points above.
   - Optionally run with `ucp_workers=2` and with a second memheap segment (if easily configurable) to test O(ucp_workers) and O(n_segments).

3. **Reporting**
   - Per-process report (e.g. rank 0 or all ranks to a file): columns = [nprocs, phase, VmRSS_KB, VmSize_KB, VmPeak_KB].
   - Simple script or notebook to plot:
     - Memory vs nprocs for each phase,
     - and (if available) memory per PE vs nprocs to spot linear vs super-linear growth.

4. **Optional deeper profiling**
   - If needed, use a heap profiler (e.g. heaptrack, valgrind massif) on the same stub, filtering or labeling allocations from:
     - `oshmem/mca/spml/ucx/*.c` and
     - UCX (e.g. `libucp`) to separate SPML bookkeeping from UCX internal.
   - Or wrap `malloc`/`calloc`/`realloc` in the SPML UCX component with a small shim that adds to a thread-local or global “SPML UCX bytes” counter and optionally logs large allocations; then report the counter at the same three init phases.

### 4.3 Implementation steps (checklist)

- [x] Add MCA or env to enable memory profiling (e.g. `spml_ucx_profile_memory`).
- [x] Add a small helper that reads `/proc/self/status` (or `getrusage` / platform-specific) and returns VmRSS, VmSize, (VmPeak).
- [x] Call this helper at end of `spml_ucx_init()` when profiling enabled; log or write to file (e.g. rank 0 or all ranks with rank in filename).
- [x] Call same helper at end of `mca_spml_ucx_add_procs()` when profiling enabled.
- [x] In `oshmem_shmem_init()`, call the helper immediately after `get_all_mkeys()` when profiling enabled (and SPML is UCX).
- [ ] Add a script or document recommended scales (nprocs) and how to plot memory vs nprocs and per-PE.

**Usage (implemented):**

- Enable profiling: set MCA parameter `--mca spml_ucx_profile_memory 1` (or `true`).
- By default, each snapshot is printed to **stdout** as one line: `oshmem_memory_profile: phase,rank,nprocs,vmrss_kb,vmsize_kb,vmpeak_kb`.
- To write to a file instead, set `OSHMEM_SPML_UCX_MEMORY_PROFILE_FILE=/path/to/file.csv`; then all ranks append CSV lines (with a header when the file is new).
- Phases: `spml_ucx_init`, `spml_ucx_add_procs`, `post_get_all_mkeys`.

### 4.4 Success criteria

- We have numbers for at least three scales (e.g. 8, 128, 512 PEs) at the three init points.
- We can state approximate “bytes per PE” or “bytes per (PE × segment)” for the main SPML/UCX structures (peers, remote_addrs_tbl, mkey cache) from the plots or tables.
- The plan is implementable without changing default behavior (profiling off unless explicitly enabled).

---

## 5. File reference

| Topic | File(s) |
|-------|--------|
| shmem_init entry | `oshmem/shmem/c/shmem_init.c` |
| OSHMEM runtime init | `oshmem/runtime/oshmem_shmem_init.c` |
| _shmem_init (order of MCA calls) | `oshmem/runtime/oshmem_shmem_init.c` |
| SPML select & component init | `oshmem/mca/spml/base/spml_base_select.c`, `oshmem/mca/spml/ucx/spml_ucx_component.c` |
| SPML UCX init (context, workers, arrays) | `oshmem/mca/spml/ucx/spml_ucx_component.c` (`spml_ucx_init`) |
| SPML UCX add_procs, mkeys, rkey store | `oshmem/mca/spml/ucx/spml_ucx.c` |
| SPML UCX types and struct sizes | `oshmem/mca/spml/ucx/spml_ucx.h` |
| get_all_mkeys → rmkey_unpack | `oshmem/mca/memheap/base/memheap_base_mkey.c` |

This document and the checklist in Section 4.3 can be used to implement the profiling changes and to report SPML/UCX memory scaling in a consistent way.
