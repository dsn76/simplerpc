#ifndef FILE_LIBSRPC_FIXBLOCKALLOC_H
#define FILE_LIBSRPC_FIXBLOCKALLOC_H

#include <stdint.h>
#include <stddef.h>

typedef struct srpc_pool_fba_s srpc_pool_fba_t;

typedef struct srpc_pool_fba_s {	// Структура быстрого менеджера блоков фиксированного размера.
        srpc_pool_fba_t *nextpool;  // Следующий пул, если в этом закончились свободные блоки.
		size_t		pool_size;		// Размер MemPool.
		uintptr_t	offset;			// Смещение первого блока относительно начала пула.
		size_t		blk_size;		// Размер блока.
		size_t		blk_nums;		// Число блоков размером blk_size в fifo.
		uint32_t	alloc, free;	// Индексы alloc, free.
		uint32_t	fifo[];			// FIFO очередь индексов свободных блоков.
        // далее размещаются сами выделяемые блоки
	} srpc_pool_fba_t;


#define FBA_FOREACH_POOL(pool, ptr) for(ptr = pool->nextpool; ptr; ptr = ptr->nextpool)
#define FBA_FOREACH_BLOCK(pool, ptr) int i; for(i = 0, ptr = (void*)pool + pool->offset; i < pool->blk_nums; i++, ptr = (void*)pool + pool->offset + ((uintptr_t)i * (uintptr_t)pool->blk_size))

srpc_pool_fba_t * srpc_pool_create(size_t elm_num, size_t elm_size); // Выделяет память под пул и инициализирует её.
int    srpc_pool_destroy(srpc_pool_fba_t *pool); // Проверяет, освобождены ли все блоки, если да, делает free, иначе возвращает ошибку.
void * srpc_pool_alloc(srpc_pool_fba_t *pool); // выделяет один блок из пула.
int    srpc_pool_free(srpc_pool_fba_t *pool, void *ptr); // возвращает один блок в пул.


#endif // FILE_LIBSRPC_FIXBLOCKALLOC_H
