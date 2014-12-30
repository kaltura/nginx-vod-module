#include "ngx_buffer_cache_internal.h"

/*
	shared memory layout:
		shared memory start
		fixed size headers
		entries_start
		...
		entries_end

		buffers_start
		...
		buffers_end
		shared memory end

	the shared memory is composed of 3 sections:
	1. fixed size headers - contains the ngx_slab_pool_t struct allocated by nginx,
		the log context string and ngx_buffer_cache_t
	2. entries - an array of ngx_buffer_cache_entry_t, each entry has a key and 
		points to a buffer in the buffers section. the entries are connected with a 
		red/black tree for fast lookup by key. the entries section grows as needed until 
		it bumps into the buffers section. each entry is a member of one of 2 doubly 
		linked lists - the free queue and the used queue. the entries move between these 
		queues as they are allocated / deallocated
	3. buffers - a cyclic queue of variable size buffers. the buffers section starts
		at the end of the shared memory and grows towards its beginning until it bumps
		into the entries section. the buffers section has 2 pointers:
		a. when a buffer is allocated, it is allocated before the write head
		b. when an entry is freed, the read head of the buffers section moves

*/

// Note: code taken from ngx_str_rbtree_insert_value, updated the node comparison
static void
ngx_buffer_cache_rbtree_insert_value(
	ngx_rbtree_node_t *temp, 
	ngx_rbtree_node_t *node, 
	ngx_rbtree_node_t *sentinel)
{
	ngx_buffer_cache_entry_t *n, *t;
	ngx_rbtree_node_t **p;

	for (;;) 
	{
		n = (ngx_buffer_cache_entry_t *)node;
		t = (ngx_buffer_cache_entry_t *)temp;

		if (node->key != temp->key) 
		{
			p = (node->key < temp->key) ? &temp->left : &temp->right;
		}
		else 
		{
			p = (ngx_memcmp(n->key, t->key, BUFFER_CACHE_KEY_SIZE) < 0)
				? &temp->left : &temp->right;
		}

		if (*p == sentinel) 
		{
			break;
		}

		temp = *p;
	}

	*p = node;
	node->parent = temp;
	node->left = sentinel;
	node->right = sentinel;
	ngx_rbt_red(node);
}

// Note: code taken from ngx_str_rbtree_lookup, updated the node comparison
static ngx_buffer_cache_entry_t *
ngx_buffer_cache_rbtree_lookup(ngx_rbtree_t *rbtree, const u_char* key, uint32_t hash)
{
	ngx_buffer_cache_entry_t *n;
	ngx_rbtree_node_t *node, *sentinel;
	ngx_int_t rc;

	node = rbtree->root;
	sentinel = rbtree->sentinel;

	while (node != sentinel) 
	{
		n = (ngx_buffer_cache_entry_t *)node;

		if (hash != node->key) 
		{
			node = (hash < node->key) ? node->left : node->right;
			continue;
		}

		rc = ngx_memcmp(key, n->key, BUFFER_CACHE_KEY_SIZE);
		if (rc < 0) 
		{
			node = node->left;
			continue;
		}

		if (rc > 0) 
		{
			node = node->right;
			continue;
		}

		return n;
	}

	return NULL;
}

static void
ngx_buffer_cache_reset(ngx_buffer_cache_t *cache)
{
	cache->entries_end = cache->entries_start;
	cache->buffers_start = cache->buffers_end;
	cache->buffers_read = cache->buffers_end;
	cache->buffers_write = cache->buffers_end;
	ngx_rbtree_init(&cache->rbtree, &cache->sentinel, ngx_buffer_cache_rbtree_insert_value);
	ngx_queue_init(&cache->used_queue);
	ngx_queue_init(&cache->free_queue);

	// update stats (everything is evicted)
	cache->stats.evicted = cache->stats.store_ok;
	cache->stats.evicted_bytes = cache->stats.store_bytes;
}

static ngx_int_t
ngx_buffer_cache_init(ngx_shm_zone_t *shm_zone, void *data)
{
	ngx_buffer_cache_t *cache;
	ngx_slab_pool_t *shpool;
	u_char* p;

	if (data) 
	{
		shm_zone->data = data;
		return NGX_OK;
	}

	shpool = (ngx_slab_pool_t *)shm_zone->shm.addr;

	if (shm_zone->shm.exists) 
	{
		shm_zone->data = shpool->data;
		return NGX_OK;
	}

	// start following the ngx_slab_pool_t that was allocated at the beginning of the chunk
	p = shm_zone->shm.addr + sizeof(ngx_slab_pool_t);

	// initialize the log context
	shpool->log_ctx = p;
	p = ngx_sprintf(shpool->log_ctx, " in buffer cache \"%V\"%Z", &shm_zone->shm.name);

	// allocate the cache state
	cache = (ngx_buffer_cache_t*)p;
	p += sizeof(*cache);

	// initialize fixed cache fields
	cache->entries_start = (ngx_buffer_cache_entry_t*)p;
	cache->buffers_end = shm_zone->shm.addr + shm_zone->shm.size;
	cache->access_time = 0;

	// reset the stats
	ngx_memzero(&cache->stats, sizeof(cache->stats));

	// reset the cache status
	ngx_buffer_cache_reset(cache);
	cache->reset = 0;

	// set the cache struct as the data of the shared pool
	shpool->data = cache;

	return NGX_OK;
}

/* Note: must be called with the mutex locked */
static ngx_buffer_cache_entry_t*
ngx_buffer_cache_free_oldest_entry(ngx_buffer_cache_t *cache)
{
	ngx_buffer_cache_entry_t* entry;

	// verify we have an entry to free
	if (ngx_queue_empty(&cache->used_queue))
	{
		return NULL;
	}

	// verify the entry is not locked
	entry = container_of(ngx_queue_head(&cache->used_queue), ngx_buffer_cache_entry_t, queue_node);
	if (ngx_time() < entry->access_time + ENTRY_LOCK_EXPIRATION)
	{
		return NULL;
	}

	// update the state
	entry->state = CES_FREE;

	// remove from rb tree
	ngx_rbtree_delete(&cache->rbtree, &entry->node);

	// move from used_queue to free_queue
	ngx_queue_remove(&entry->queue_node);
	ngx_queue_insert_tail(&cache->free_queue, &entry->queue_node);

	if (ngx_queue_empty(&cache->used_queue))
	{
		// queue is empty reset the read/write pointers
		cache->buffers_read = cache->buffers_end;
		cache->buffers_write = cache->buffers_end;
	}
	else
	{
		// update the read buffer pointer
		cache->buffers_read = entry->start_offset;
	}

	// update stats
	cache->stats.evicted++;
	cache->stats.evicted_bytes += entry->buffer_size;

	return entry;
}

/* Note: must be called with the mutex locked */
static ngx_buffer_cache_entry_t*
ngx_buffer_cache_get_free_entry(ngx_buffer_cache_t *cache)
{
	ngx_buffer_cache_entry_t* entry;

	if (!ngx_queue_empty(&cache->free_queue))
	{
		// return the free queue head
		return container_of(ngx_queue_head(&cache->free_queue), ngx_buffer_cache_entry_t, queue_node);
	}
	
	if ((u_char*)(cache->entries_end + 1) < cache->buffers_start)
	{
		// enlarge the entries buffer
		entry = cache->entries_end;
		cache->entries_end++;

		// initialize the state and add to free queue
		entry->state = CES_FREE;
		ngx_queue_insert_tail(&cache->free_queue, &entry->queue_node);
		return entry;
	}
	
	return ngx_buffer_cache_free_oldest_entry(cache);
}

/* Note: must be called with the mutex locked */
static u_char*
ngx_buffer_cache_get_free_buffer(
	ngx_buffer_cache_t *cache,
	size_t size)
{
	u_char* buffer_start;

	// check whether it's possible to allocate the requested size
	if ((u_char*)(cache->entries_end + ENTRIES_ALLOC_MARGIN) + size + BUFFER_ALIGNMENT > cache->buffers_end)
	{
		return NULL;
	}

	buffer_start = (u_char*)((intptr_t)(cache->buffers_write - size) & (~(BUFFER_ALIGNMENT - 1)));

	for (;;)
	{
		// Layout:	S	W/////R		E
		if (cache->buffers_write < cache->buffers_read || 
			(cache->buffers_write == cache->buffers_read && ngx_queue_empty(&cache->used_queue)))
		{
			if (buffer_start >= cache->buffers_start)
			{
				// have enough room here
				return buffer_start;
			}

			if (buffer_start > (u_char*)(cache->entries_end + ENTRIES_ALLOC_MARGIN))
			{
				// enlarge the buffer
				cache->buffers_start = buffer_start;
				return buffer_start;
			}

			// cannot allocate here, move the write position to the end
			cache->buffers_write = cache->buffers_end;
			buffer_start = (u_char*)((intptr_t)(cache->buffers_write - size) & (~(BUFFER_ALIGNMENT - 1)));
			continue;
		}

		// Layout:	S////R		W///E
		if (buffer_start > cache->buffers_read)
		{
			// have enough room here
			return buffer_start;
		}

		// not enough room, free an entry
		if (ngx_buffer_cache_free_oldest_entry(cache) == NULL)
		{
			break;
		}
	}

	return NULL;
}

ngx_flag_t
ngx_buffer_cache_fetch(
	ngx_shm_zone_t *shm_zone,
	u_char* key,
	u_char** buffer,
	size_t* buffer_size)
{
	ngx_buffer_cache_entry_t* entry;
	ngx_buffer_cache_t *cache;
	ngx_slab_pool_t *shpool;
	ngx_flag_t result = 0;
	uint32_t hash;

	hash = ngx_crc32_short(key, BUFFER_CACHE_KEY_SIZE);

	shpool = (ngx_slab_pool_t *)shm_zone->shm.addr;
	cache = shpool->data;

	ngx_shmtx_lock(&shpool->mutex);

	if (!cache->reset)
	{
		entry = ngx_buffer_cache_rbtree_lookup(&cache->rbtree, key, hash);
		if (entry != NULL && entry->state == CES_READY)
		{
			result = 1;

			// update stats
			cache->stats.fetch_hit++;
			cache->stats.fetch_bytes += entry->buffer_size;

			// copy buffer pointer and size
			*buffer = entry->start_offset;
			*buffer_size = entry->buffer_size;

			// Note: setting the access time of the entry and cache to prevent it 
			//		from being freed while the caller uses the buffer
			cache->access_time = entry->access_time = ngx_time();
		}
		else
		{
			// update stats
			cache->stats.fetch_miss++;
		}
	}

	ngx_shmtx_unlock(&shpool->mutex);

	return result;
}

ngx_flag_t
ngx_buffer_cache_store_gather(
	ngx_shm_zone_t *shm_zone, 
	u_char* key, 
	ngx_str_t* buffers,
	size_t buffer_count)
{
	ngx_buffer_cache_entry_t* entry;
	ngx_buffer_cache_t *cache;
	ngx_slab_pool_t *shpool;
	ngx_str_t* cur_buffer;
	ngx_str_t* last_buffer;
	size_t buffer_size;
	uint32_t hash;
	u_char* target_buffer;

	hash = ngx_crc32_short(key, BUFFER_CACHE_KEY_SIZE);

	shpool = (ngx_slab_pool_t *)shm_zone->shm.addr;
	cache = shpool->data;

	ngx_shmtx_lock(&shpool->mutex);

	if (cache->reset)
	{
		// a previous store operation was killed in progress, need to reset the cache
		// since the data structures may be corrupt. we can only reset the cache after
		// the access time expires since other processes may still be reading from / 
		// writing to the cache
		if (ngx_time() < cache->access_time + CACHE_LOCK_EXPIRATION)
		{
			ngx_shmtx_unlock(&shpool->mutex);
			return 0;
		}

		// reset the cache, leave the reset flag enabled
		ngx_buffer_cache_reset(cache);

		// update stats
		cache->stats.reset++;
	}
	else
	{
		// make sure the entry does not already exist
		entry = ngx_buffer_cache_rbtree_lookup(&cache->rbtree, key, hash);
		if (entry != NULL)
		{
			cache->stats.store_exists++;
			ngx_shmtx_unlock(&shpool->mutex);
			return 0;
		}

		// enable the reset flag before we start making any changes
		cache->reset = 1;
	}

	// allocate a new entry
	entry = ngx_buffer_cache_get_free_entry(cache);
	if (entry == NULL)
	{
		goto error;
	}

	// calculate the buffer size
	last_buffer = buffers + buffer_count;
	buffer_size = 0;
	for (cur_buffer = buffers; cur_buffer < last_buffer; cur_buffer++)
	{
		buffer_size += cur_buffer->len;
	}

	// allocate a buffer to hold the data
	target_buffer = ngx_buffer_cache_get_free_buffer(cache, buffer_size);
	if (target_buffer == NULL)
	{
		goto error;
	}

	// initialize the entry
	entry->state = CES_ALLOCATED;
	entry->node.key = hash;
	memcpy(entry->key, key, BUFFER_CACHE_KEY_SIZE);
	entry->start_offset = target_buffer;
	entry->buffer_size = buffer_size;

	// update the write position
	cache->buffers_write = target_buffer;

	// move from free_queue to used_queue
	ngx_queue_remove(&entry->queue_node);
	ngx_queue_insert_tail(&cache->used_queue, &entry->queue_node);

	// insert to rbtree
	ngx_rbtree_insert(&cache->rbtree, &entry->node);

	// update stats
	cache->stats.store_ok++;
	cache->stats.store_bytes += buffer_size;

	// Note: the memcpy is performed after releasing the lock to avoid holding the lock for a long time
	//		setting the access time of the entry and cache prevents it from being freed
	cache->access_time = entry->access_time = ngx_time();

	cache->reset = 0;
	ngx_shmtx_unlock(&shpool->mutex);

	for (cur_buffer = buffers; cur_buffer < last_buffer; cur_buffer++)
	{
		target_buffer = ngx_copy(target_buffer, cur_buffer->data, cur_buffer->len);
	}

	// Note: no need to obtain the lock since state is ngx_atomic_t
	entry->state = CES_READY;

	return 1;

error:
	cache->stats.store_err++;
	cache->reset = 0;
	ngx_shmtx_unlock(&shpool->mutex);
	return 0;
}

ngx_flag_t
ngx_buffer_cache_store(
	ngx_shm_zone_t *shm_zone,
	u_char* key,
	u_char* source_buffer,
	size_t buffer_size)
{
	ngx_str_t buffer;

	buffer.data = source_buffer;
	buffer.len = buffer_size;

	return ngx_buffer_cache_store_gather(shm_zone, key, &buffer, 1);
}

void
ngx_buffer_cache_get_stats(
	ngx_shm_zone_t *shm_zone,
	ngx_buffer_cache_stats_t* stats)
{
	ngx_buffer_cache_t *cache;
	ngx_slab_pool_t *shpool;

	shpool = (ngx_slab_pool_t *)shm_zone->shm.addr;
	cache = shpool->data;

	ngx_shmtx_lock(&shpool->mutex);

	memcpy(stats, &cache->stats, sizeof(cache->stats));

	stats->entries = cache->entries_end - cache->entries_start;
	stats->data_size = cache->buffers_end - cache->buffers_start;

	ngx_shmtx_unlock(&shpool->mutex);
}

void
ngx_buffer_cache_reset_stats(ngx_shm_zone_t *shm_zone)
{
	ngx_buffer_cache_t *cache;
	ngx_slab_pool_t *shpool;

	shpool = (ngx_slab_pool_t *)shm_zone->shm.addr;
	cache = shpool->data;

	ngx_shmtx_lock(&shpool->mutex);

	ngx_memzero(&cache->stats, sizeof(cache->stats));

	ngx_shmtx_unlock(&shpool->mutex);
}

ngx_shm_zone_t* 
ngx_buffer_cache_create_zone(ngx_conf_t *cf, ngx_str_t *name, size_t size, void *tag)
{
	ngx_shm_zone_t* result;

	result = ngx_shared_memory_add(cf, name, size, tag);
	if (result == NULL)
	{
		return NULL;
	}

	result->init = ngx_buffer_cache_init;
	return result;
}
