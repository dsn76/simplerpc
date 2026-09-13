
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "libsrpc_fixblockalloc.h"


srpc_pool_fba_t * srpc_pool_create(size_t elm_num, size_t elm_size)
{
    srpc_pool_fba_t *pool = NULL;
    size_t blk_size = (elm_size + 15) / 16 * 16;
    size_t fifo_size = (elm_num + 1) * sizeof(pool->fifo[0]);
    fifo_size = (fifo_size + 63) / 64 * 64;
    size_t pool_size = blk_size * elm_num + sizeof(*pool) + fifo_size;

    pool = malloc(pool_size);
    if(!pool) goto end;

    memset(pool, 0, pool_size);

    pool->pool_size = pool_size;
    pool->blk_nums = elm_num;
    pool->blk_size = blk_size;
    pool->offset = fifo_size;

    for(size_t i = 0; i < elm_num; i++){
        pool->fifo[pool->free++] = (uint32_t)i;
    }

end:
    return(pool);
}


int srpc_pool_destroy(srpc_pool_fba_t *pool)
{
    int rc = 0;

    while(pool) {
        void *ptr = pool;
        pool = pool->nextpool;
        free(ptr);
    }

    return(rc);
}


void * srpc_pool_alloc(srpc_pool_fba_t *pool)
{
    void *ptr = NULL;
    uint32_t blk, oi, ni;

    while(1) {
        if(!pool || !pool->pool_size || !pool->offset || !pool->blk_size) goto end;
        oi = ni = pool->alloc;
        if(ni == pool->free) { // empty
            if(!pool->nextpool) pool->nextpool = srpc_pool_create(pool->blk_nums, pool->blk_size);
            pool = pool->nextpool;
            continue;
        }
        break;
    }

    ni++; if(ni >= (pool->blk_nums+1)) ni = 0;
    blk = pool->fifo[oi];
    pool->alloc = ni;

    ptr =  (char*)pool + pool->offset + ((uintptr_t)blk * (uintptr_t)pool->blk_size);

end:
    return(ptr);
}


int srpc_pool_free(srpc_pool_fba_t *pool, void *ptr)
{
    int rc = 0;
    uint32_t blk, oi, ni;

    if(!ptr || !pool) {
        rc = -EINVAL;
        goto end;
    }

    while(1) {

        if(!pool || !pool->pool_size || !pool->offset || !pool->blk_size) {
            rc = -EAFNOSUPPORT;
            goto end;
        }

        if((uintptr_t)ptr > (uintptr_t)pool && (uintptr_t)ptr < ((uintptr_t)pool + pool->pool_size)) break;

        pool = pool->nextpool;
    }

    blk = (uint32_t)(((uintptr_t)ptr - (uintptr_t)pool - pool->offset) / pool->blk_size);
    if(blk >= pool->blk_nums) return(-EBADF);

    oi = pool->free;
    ni = oi + 1;
    if(ni >= (pool->blk_nums+1)) ni = 0;
    if(ni == pool->alloc) return(-ENOSPC);
    pool->free = ni;
    pool->fifo[oi] = blk;

end:
    return(rc);
}

