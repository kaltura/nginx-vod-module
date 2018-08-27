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
		the log context string and ngx_buffer_cache_sh_t
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
ngx_buffer_cache_reset(ngx_buffer_cache_sh_t *cache)
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
	ngx_buffer_cache_sh_t *sh;
	ngx_buffer_cache_t *ocache = data;
	ngx_buffer_cache_t *cache;
	u_char* p;

	cache = shm_zone->data;

	if (ocache)
	{
		cache->sh = ocache->sh;
		cache->shpool = ocache->shpool;
		return NGX_OK;
	}

	cache->shpool = (ngx_slab_pool_t *)shm_zone->shm.addr;

	if (shm_zone->shm.exists) 
	{
		cache->sh = cache->shpool->data;
		return NGX_OK;
	}

	// start following the ngx_slab_pool_t that was allocated at the beginning of the chunk
	p = shm_zone->shm.addr + sizeof(ngx_slab_pool_t);

	// initialize the log context
	cache->shpool->log_ctx = p;
	p = ngx_sprintf(cache->shpool->log_ctx, " in buffer cache \"%V\"%Z", &shm_zone->shm.name);

	// allocate the shared cache state
	p = ngx_align_ptr(p, sizeof(void *));
	sh = (ngx_buffer_cache_sh_t*)p;
	p += sizeof(*sh);
	cache->sh = sh;

	cache->shpool->data = sh;

	// initialize fixed cache fields
	p = ngx_align_ptr(p, sizeof(void *));
	sh->entries_start = (ngx_buffer_cache_entry_t*)p;
	sh->buffers_end = shm_zone->shm.addr + shm_zone->shm.size;
	sh->access_time = 0;

	// reset the stats
	ngx_memzero(&sh->stats, sizeof(sh->stats));

	// reset the cache status
	ngx_buffer_cache_reset(sh);
	sh->reset = 0;

	return NGX_OK;
}

/* Note: must be called with the mutex locked */
static ngx_buffer_cache_entry_t*
ngx_buffer_cache_free_oldest_entry(ngx_buffer_cache_sh_t *cache, uint32_t expiration)
{
	ngx_buffer_cache_entry_t* entry;

	// verify we have an entry to free
	if (ngx_queue_empty(&cache->used_queue))
	{
		return NULL;
	}

	// verify the entry is not locked
	entry = container_of(ngx_queue_head(&cache->used_queue), ngx_buffer_cache_entry_t, queue_node);
	if (entry->ref_count > 0 &&
		ngx_time() < entry->access_time + ENTRY_LOCK_EXPIRATION)
	{
		return NULL;
	}

	// make sure the entry is expired, if that is the requirement
	if (expiration && ngx_time() < (time_t)(entry->write_time + expiration))
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
ngx_buffer_cache_get_free_entry(ngx_buffer_cache_sh_t *cache)
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
	
	return ngx_buffer_cache_free_oldest_entry(cache, 0);
}

/* Note: must be called with the mutex locked */
static u_char*
ngx_buffer_cache_get_free_buffer(
	ngx_buffer_cache_sh_t *cache,
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
		if (ngx_buffer_cache_free_oldest_entry(cache, 0) == NULL)
		{
			break;
		}
	}

	return NULL;
}

ngx_flag_t
ngx_buffer_cache_fetch(
	ngx_buffer_cache_t* cache,
	u_char* key,
	ngx_str_t* buffer,
	uint32_t* token)
{
	ngx_buffer_cache_entry_t* entry;
	ngx_buffer_cache_sh_t *sh = cache->sh;
	ngx_flag_t result = 0;
	uint32_t hash;

	hash = ngx_crc32_short(key, BUFFER_CACHE_KEY_SIZE);

	ngx_shmtx_lock(&cache->shpool->mutex);

	if (!sh->reset)
	{
		entry = ngx_buffer_cache_rbtree_lookup(&sh->rbtree, key, hash);
		if (entry != NULL && entry->state == CES_READY && 
			(cache->expiration == 0 || ngx_time() < (time_t)(entry->write_time + cache->expiration)))
		{
			result = 1;

			// update stats
			sh->stats.fetch_hit++;
			sh->stats.fetch_bytes += entry->buffer_size;

			// copy buffer pointer and size
			buffer->data = entry->start_offset;
			buffer->len = entry->buffer_size;
			*token = entry->write_time;

			// Note: setting the access time of the entry and cache to prevent it 
			//		from being freed while the caller uses the buffer
			sh->access_time = entry->access_time = ngx_time();
			(void)ngx_atomic_fetch_add(&entry->ref_count, 1);
		}
		else
		{
			// update stats
			sh->stats.fetch_miss++;
		}
	}

	ngx_shmtx_unlock(&cache->shpool->mutex);

	return result;
}

void
ngx_buffer_cache_release(
	ngx_buffer_cache_t* cache,
	u_char* key,
	uint32_t token)
{
	ngx_buffer_cache_entry_t* entry;
	ngx_buffer_cache_sh_t *sh = cache->sh;
	uint32_t hash;

	hash = ngx_crc32_short(key, BUFFER_CACHE_KEY_SIZE);

	ngx_shmtx_lock(&cache->shpool->mutex);

	if (!sh->reset)
	{
		entry = ngx_buffer_cache_rbtree_lookup(&sh->rbtree, key, hash);
		if (entry != NULL && entry->state == CES_READY && (uint32_t)entry->write_time == token)
		{
			(void)ngx_atomic_fetch_add(&entry->ref_count, -1);
		}
	}

	ngx_shmtx_unlock(&cache->shpool->mutex);
}

ngx_flag_t
ngx_buffer_cache_store_gather(
	ngx_buffer_cache_t* cache, 
	u_char* key, 
	ngx_str_t* buffers,
	size_t buffer_count)
{
	ngx_buffer_cache_entry_t* entry;
	ngx_buffer_cache_sh_t *sh = cache->sh;
	ngx_str_t* cur_buffer;
	ngx_str_t* last_buffer;
	size_t buffer_size;
	uint32_t hash;
	uint32_t evictions;
	u_char* target_buffer;

	hash = ngx_crc32_short(key, BUFFER_CACHE_KEY_SIZE);

	ngx_shmtx_lock(&cache->shpool->mutex);

	if (sh->reset)
	{
		// a previous store operation was killed in progress, need to reset the cache
		// since the data structures may be corrupt. we can only reset the cache after
		// the access time expires since other processes may still be reading from / 
		// writing to the cache
		if (ngx_time() < sh->access_time + CACHE_LOCK_EXPIRATION)
		{
			ngx_shmtx_unlock(&cache->shpool->mutex);
			return 0;
		}

		// reset the cache, leave the reset flag enabled
		ngx_buffer_cache_reset(sh);

		// update stats
		sh->stats.reset++;
	}
	else
	{
		// remove expired entries
		if (cache->expiration)
		{
			for (evictions = MAX_EVICTIONS_PER_STORE; evictions > 0; evictions--)
			{
				if (!ngx_buffer_cache_free_oldest_entry(sh, cache->expiration))
				{
					break;
				}
			}
		}

		// make sure the entry does not already exist
		entry = ngx_buffer_cache_rbtree_lookup(&sh->rbtree, key, hash);
		if (entry != NULL)
		{
			sh->stats.store_exists++;
			ngx_shmtx_unlock(&cache->shpool->mutex);
			return 0;
		}

		// enable the reset flag before we start making any changes
		sh->reset = 1;
	}

	// allocate a new entry
	entry = ngx_buffer_cache_get_free_entry(sh);
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
	target_buffer = ngx_buffer_cache_get_free_buffer(sh, buffer_size + 1);
	if (target_buffer == NULL)
	{
		goto error;
	}

	// initialize the entry
	entry->state = CES_ALLOCATED;
	entry->ref_count = 1;
	entry->node.key = hash;
	memcpy(entry->key, key, BUFFER_CACHE_KEY_SIZE);
	entry->start_offset = target_buffer;
	entry->buffer_size = buffer_size;

	// update the write position
	sh->buffers_write = target_buffer;

	// move from free_queue to used_queue
	ngx_queue_remove(&entry->queue_node);
	ngx_queue_insert_tail(&sh->used_queue, &entry->queue_node);

	// insert to rbtree
	ngx_rbtree_insert(&sh->rbtree, &entry->node);

	// update stats
	sh->stats.store_ok++;
	sh->stats.store_bytes += buffer_size;

	// Note: the memcpy is performed after releasing the lock to avoid holding the lock for a long time
	//		setting the access time of the entry and cache prevents it from being freed
	sh->access_time = entry->access_time = ngx_time();
	entry->write_time = ngx_time();

	sh->reset = 0;
	ngx_shmtx_unlock(&cache->shpool->mutex);

	for (cur_buffer = buffers; cur_buffer < last_buffer; cur_buffer++)
	{
		target_buffer = ngx_copy(target_buffer, cur_buffer->data, cur_buffer->len);
	}
	*target_buffer = '\0';

	// Note: no need to obtain the lock since state is ngx_atomic_t
	entry->state = CES_READY;
	(void)ngx_atomic_fetch_add(&entry->ref_count, -1);

	return 1;

error:
	sh->stats.store_err++;
	sh->reset = 0;
	ngx_shmtx_unlock(&cache->shpool->mutex);
	return 0;
}

ngx_flag_t
ngx_buffer_cache_store(
	ngx_buffer_cache_t* cache,
	u_char* key,
	u_char* source_buffer,
	size_t buffer_size)
{
	ngx_str_t buffer;

	buffer.data = source_buffer;
	buffer.len = buffer_size;

	return ngx_buffer_cache_store_gather(cache, key, &buffer, 1);
}

void
ngx_buffer_cache_get_stats(
	ngx_buffer_cache_t* cache,
	ngx_buffer_cache_stats_t* stats)
{
	ngx_buffer_cache_sh_t *sh = cache->sh;

	ngx_shmtx_lock(&cache->shpool->mutex);

	memcpy(stats, &sh->stats, sizeof(sh->stats));

	stats->entries = sh->entries_end - sh->entries_start;
	stats->data_size = sh->buffers_end - sh->buffers_start;

	ngx_shmtx_unlock(&cache->shpool->mutex);
}

void
ngx_buffer_cache_reset_stats(ngx_buffer_cache_t* cache)
{
	ngx_shmtx_lock(&cache->shpool->mutex);

	ngx_memzero(&cache->sh->stats, sizeof(cache->sh->stats));

	ngx_shmtx_unlock(&cache->shpool->mutex);
}

ngx_buffer_cache_t*
ngx_buffer_cache_create(ngx_conf_t *cf, ngx_str_t *name, size_t size, time_t expiration, void *tag)
{
	ngx_buffer_cache_t* cache;

	cache = ngx_pcalloc(cf->pool, sizeof(ngx_buffer_cache_t));
	if (cache == NULL) 
	{
		return NGX_CONF_ERROR;
	}

	cache->expiration = expiration;

	cache->shm_zone = ngx_shared_memory_add(cf, name, size, tag);
	if (cache->shm_zone == NULL)
	{
		return NULL;
	}

	if (cache->shm_zone->data) 
	{
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
			"duplicate zone \"%V\"", name);
		return NGX_CONF_ERROR;
	}

	cache->shm_zone->init = ngx_buffer_cache_init;
	cache->shm_zone->data = cache;

	return cache;
}
