/*
 * Copyright (c) 2013-2014 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2014 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2014      Intel, Inc. All rights reserved
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "oshmem_config.h"

#include "oshmem/util/oshmem_util.h"
#include "oshmem/mca/sshmem/sshmem.h"
#include "oshmem/mca/sshmem/base/base.h"
#include "oshmem/mca/memheap/memheap.h"
#include "oshmem/mca/memheap/base/base.h"

#include <sharp.h>
#include <shmemx.h>

int is_initialized = 0;
int sharp_init(void) {
    is_initialized = 1;
    return sharp_create_node_info();
}

void sharp_finalize(void) {
    sharp_destroy_node_info();
}




#define ONLY_MSPACES 1
#include "oshmem/mca/memheap/base/mymalloc.c"

struct sharp_ctx {
    sharp_allocator_obj_t * a_obj;
    mspace area;
};

#if 1


static int sharp_realloc(map_segment_t *s, size_t size,
                         void* old_ptr, void** new_ptr)
{
    struct sharp_ctx * ctx = s->context;    
    *new_ptr = mspace_malloc(ctx->area, size); 

    return OSHMEM_SUCCESS;
}

static int sharp_gpu_realloc(map_segment_t *s, size_t size,
                             void * old_ptr, void ** new_ptr)
{
    struct sharp_ctx * ctx = s->context;
    *new_ptr = sharp_allocator_alloc(ctx->a_obj, size);

    return OSHMEM_SUCCESS;
}

static int sharp_free(map_segment_t *s, void* ptr)
{
    struct sharp_ctx * ctx = s->context;    
    mspace_free(ctx->area, ptr);
    return OSHMEM_SUCCESS;
}

static int sharp_gpu_free(map_segment_t *s, void * ptr)
{
    struct sharp_ctx * ctx = s->context;
    sharp_allocator_free(ctx->a_obj, ptr);

    return OSHMEM_SUCCESS;
}

static segment_allocator_t sharp_allocator = {
    .realloc = sharp_realloc,
    .free    = sharp_free
};

static segment_allocator_t sharp_gpu_allocator = {
    .realloc = sharp_gpu_realloc,
    .free    = sharp_gpu_free
};

#endif

int mca_memheap_base_alloc_init(mca_memheap_map_t *map, size_t size, long hint)
{
    int ret = OSHMEM_SUCCESS;
    char * seg_filename = NULL;

    assert(map);
    if (hint == 0) {
        assert(HEAP_SEG_INDEX == map->n_segments);
    } else {
        assert(HEAP_SEG_INDEX < map->n_segments);
    }

    map_segment_t *s = &map->mem_segs[map->n_segments];
    seg_filename = oshmem_get_unique_file_name(oshmem_my_proc_id());
    if (hint == SHMEM_HINT_DEVICE_NIC_MEM || hint == 0 || hint == SHMEM_HINT_INTERLEAVE) {
        ret = mca_sshmem_segment_create(s, seg_filename, size, hint);
    } else {
        sharp_allocator_info_params_t info_obj;
        sharp_hint_t sharp_hints = 0;
        sharp_constraint_t sharp_constraints;
        sharp_allocator_obj_t * a_obj;

        if(!is_initialized) {
            sharp_init();
        }

        // create a new memsegment
        int nr_segs = map->n_segments;
        map_segment_t * mysegment = &map->mem_segs[nr_segs];

        size_t alloc_size = size;
        
        // alloc space with sharp
        /* do we have a method to alloc memory on the nic? */
        if (hint == SHMEM_HINT_DEVICE_GPU_MEM || hint == SHMEM_HINT_HIGH_BW_MEM) {
            sharp_hints |= SHARP_HINT_GPU;
            sharp_constraints |= SHARP_ACCESS_INTRAP;
        } else if (hint == SHMEM_HINT_LOW_LAT_MEM) {
            sharp_hints |= SHARP_HINT_CPU;
            sharp_constraints |= SHARP_ACCESS_INTERP;
        }

        if (hint == SHMEM_HINT_NEAR_NIC_MEM) {
            sharp_hints |= SHARP_HINT_LATENCY_OPT;
            sharp_constraints |= SHARP_ACCESS_INTERP;
        }

        if (hint == SHMEM_HINT_NUMA_0) {
            sharp_constraints |= SHARP_CONSTRAINT_NUMA_0;
        } else if (hint == SHMEM_HINT_NUMA_1) {
            sharp_constraints |= SHARP_CONSTRAINT_NUMA_1;
        }

        if (hint == SHMEM_HINT_LOCAL) {
            sharp_hints |= SHARP_HINT_CPU;
            sharp_constraints |= SHARP_ACCESS_INTERP | (1 << 7);
        }

        info_obj.allocator_hints = sharp_hints;
        info_obj.allocator_constraints = sharp_constraints;

        a_obj = sharp_init_allocator_obj(&info_obj);
        
        mysegment->super.va_base = sharp_allocator_alloc(a_obj, alloc_size);
        if (mysegment->super.va_base == NULL) {
            if (sharp_hints == SHARP_HINT_GPU) {
                // fail silently.. not really a problem, either no GPU or no GPU memory.. fail later
                return 0;
            }
        }
        mysegment->seg_size = alloc_size;
        mysegment->super.va_end = mysegment->super.va_base + mysegment->seg_size;
        mysegment->type = MAP_SEGMENT_ALLOC_SHARP;
        mysegment->alloc_hints = hint;
        struct sharp_ctx * sctx = calloc(1, sizeof(struct sharp_ctx));
        sctx->a_obj = a_obj;
        if (hint != SHMEM_HINT_DEVICE_GPU_MEM) {
            mspace area = create_mspace_with_base(mysegment->super.va_base, alloc_size, 0);
            mysegment->allocator = &sharp_allocator;
            sctx->area = area;
        } else {
            mysegment->allocator = &sharp_gpu_allocator;
            sctx->area = NULL;
        }
        mysegment->context = sctx;
        map->n_segments++;
    }


    if (OSHMEM_SUCCESS == ret) {
        map->n_segments++;
        MEMHEAP_VERBOSE(1,
                        "Memheap alloc memory: %llu byte(s), %d segments by method: %d",
                        (unsigned long long)size, map->n_segments, s->type);
    }

    free(seg_filename);

    return ret;
}

void mca_memheap_base_alloc_exit(mca_memheap_map_t *map)
{
    int i;

    if (!map) {
        return;
    }

    for (i = 0; i < map->n_segments; ++i) {
        map_segment_t *s = &map->mem_segs[i];
        if (s->type != MAP_SEGMENT_STATIC && s->type != MAP_SEGMENT_ALLOC_SHARP) {
            mca_sshmem_segment_detach(s, NULL);
            mca_sshmem_unlink(s);
        }
    }
}



int register_sharp(map_segment_t *s, int *num_btl);

int mca_memheap_alloc_with_hint(size_t size, long hint, void** ptr)
{
    int i;

    for (i = 0; i < mca_memheap_base_map.n_segments; i++) {
        map_segment_t *s = &mca_memheap_base_map.mem_segs[i];
        if (s->allocator && (hint == s->alloc_hints)) {
            printf("segment selected: %d of %d, hint: %d alloc hint: %d\n", i, mca_memheap_base_map.n_segments, hint, s->alloc_hints);
            /* Do not fall back to default allocator since it will break the
             * symmetry between PEs
             */
            return s->allocator->realloc(s, size, NULL, ptr);
        }
    }

    return MCA_MEMHEAP_CALL(alloc(size, ptr));

}
