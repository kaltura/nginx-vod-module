#include "ngx_buffer_cache.h"

// constants
#define ENTRY_LOCK_EXPIRATION (60)
#define ENTRIES_ALLOC_MARGIN (20)
#define BUFFER_ALIGNMENT (16)

// enums
enum {
	CES_FREE,
	CES_ALLOCATED,
	CES_READY,
};

// typedefs
typedef struct {
	ngx_rbtree_node_t node;
	u_char* start_offset;
	size_t buffer_size;
	ngx_atomic_t state;
	time_t access_time;
	u_char key[BUFFER_CACHE_KEY_SIZE];
} ngx_buffer_cache_entry_t;

typedef struct {
	ngx_rbtree_t rbtree;
	ngx_rbtree_node_t sentinel;
	ngx_buffer_cache_entry_t* entries_start;
	ngx_buffer_cache_entry_t* entries_end;
	ngx_buffer_cache_entry_t* oldest_entry;
	u_char* buffers_start;
	u_char* buffers_end;
	u_char* buffers_read;
	u_char* buffers_write;
} ngx_buffer_cache_t;

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
	2. entries - a cyclic queue of ngx_buffer_cache_entry_t, each entry has a key and 
		points to a buffer in the buffers section. the entries are connected with a 
		red/black tree for fast lookup by key. the entries section grows as needed until 
		it bumps into the buffers section
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

	// initialize the cache state
	cache->entries_start = (ngx_buffer_cache_entry_t*)p;
	cache->entries_end = cache->entries_start;
	cache->oldest_entry = cache->entries_start;
	cache->buffers_start = shm_zone->shm.addr + shm_zone->shm.size;
	cache->buffers_end = cache->buffers_start;
	cache->buffers_read = cache->buffers_start;
	cache->buffers_write = cache->buffers_start;
	ngx_rbtree_init(&cache->rbtree, &cache->sentinel, ngx_buffer_cache_rbtree_insert_value);

	// set the cache struct as the data of the shared pool
	shpool->data = cache;

	return NGX_OK;
}

/* Note: must be called with the mutex locked */
static ngx_buffer_cache_entry_t*
ngx_buffer_cache_free_oldest_entry(ngx_buffer_cache_t *cache)
{
	ngx_buffer_cache_entry_t* entry = cache->oldest_entry;

	// if the oldest entry is locked fail
	if (entry->state == CES_FREE || 
		ngx_time() < entry->access_time + ENTRY_LOCK_EXPIRATION)
	{
		return NULL;
	}

	// update the state
	entry->state = CES_FREE;

	// move to the next entry
	cache->oldest_entry++;
	if (cache->oldest_entry >= cache->entries_end)
	{
		cache->oldest_entry = cache->entries_start;
	}

	// remove from rb tree
	ngx_rbtree_delete(&cache->rbtree, &entry->node);

	// update the read buffer pointer if the entry was allocated
	if (entry->start_offset != NULL)
	{
		cache->buffers_read = entry->start_offset;
		if (cache->buffers_read == cache->buffers_write)
		{
			// queue is empty, start from the beginning
			cache->buffers_read = cache->buffers_end;
			cache->buffers_write = cache->buffers_end;
		}
	}

	return entry;
}

/* Note: must be called with the mutex locked */
static ngx_buffer_cache_entry_t*
ngx_buffer_cache_allocate_cache_entry(ngx_buffer_cache_t *cache, u_char* key, uint32_t hash)
{
	ngx_buffer_cache_entry_t* entry;

	if ((u_char*)(cache->entries_end + 1) < cache->buffers_start)
	{
		// enlarge the entries buffer
		entry = cache->entries_end;
		cache->entries_end++;
	}
	else
	{
		// free the oldest entry
		entry = ngx_buffer_cache_free_oldest_entry(cache);
		if (entry == NULL)
		{
			return NULL;
		}
	}

	entry->state = CES_ALLOCATED;
	entry->node.key = hash;
	memcpy(entry->key, key, BUFFER_CACHE_KEY_SIZE);
	entry->access_time = 0;
	entry->start_offset = NULL;
	entry->buffer_size = 0;

	ngx_rbtree_insert(&cache->rbtree, &entry->node);

	return entry;
}

/* Note: must be called with the mutex locked */
static u_char*
ngx_buffer_cache_allocate_buffer(
	ngx_buffer_cache_t *cache,
	ngx_buffer_cache_entry_t* entry,
	size_t size)
{
	u_char* buffer_start;

	buffer_start = (u_char*)((intptr_t)(cache->buffers_write - size) & (~(BUFFER_ALIGNMENT - 1)));

	// try to enlarge the buffer
	if (cache->buffers_write == cache->buffers_start &&
		buffer_start > (u_char*)(cache->entries_end + ENTRIES_ALLOC_MARGIN))
	{
		cache->buffers_start = buffer_start;
		goto allocate;
	}

	// if size is larger than the whole buffer fail
	if (size + BUFFER_ALIGNMENT >= (size_t)(cache->buffers_end - cache->buffers_start))
	{
		return NULL;
	}

	for (;;)
	{
		// Layout:	S	W/////R		E
		if (cache->buffers_write <= cache->buffers_read)
		{
			if (buffer_start >= cache->buffers_start)
			{
				break;
			}

			// cannot allocate here, move the write position to the end
			cache->buffers_write = cache->buffers_end;
			buffer_start = (u_char*)((intptr_t)(cache->buffers_write - size) & (~(BUFFER_ALIGNMENT - 1)));
			continue;
		}

		// Layout:	S////R		W///E
		if (buffer_start > cache->buffers_read)
		{
			break;
		}

		// not enough room, free an entry
		if (ngx_buffer_cache_free_oldest_entry(cache) == NULL)
		{
			return NULL;
		}
	}

allocate:

	// update the entry and write position
	entry->start_offset = buffer_start;
	entry->buffer_size = size;
	cache->buffers_write = buffer_start;
	return buffer_start;
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

	shpool = (ngx_slab_pool_t *)shm_zone->shm.addr;
	cache = shpool->data;

	hash = ngx_crc32_short(key, BUFFER_CACHE_KEY_SIZE);

	ngx_shmtx_lock(&shpool->mutex);

	entry = ngx_buffer_cache_rbtree_lookup(&cache->rbtree, key, hash);
	if (entry != NULL && entry->state == CES_READY)
	{
		// Note: setting the access time of the entry to prevent it from being freed 
		//		while the caller uses the buffer
		entry->access_time = ngx_time();
		*buffer = entry->start_offset;
		*buffer_size = entry->buffer_size;
		result = 1;
	}

	ngx_shmtx_unlock(&shpool->mutex);
	return result;
}

ngx_flag_t
ngx_buffer_cache_store(
	ngx_shm_zone_t *shm_zone, 
	u_char* key, 
	const u_char* source_buffer, 
	size_t buffer_size)
{
	ngx_buffer_cache_entry_t* entry;
	ngx_buffer_cache_t *cache;
	ngx_slab_pool_t *shpool;
	uint32_t hash;
	u_char* target_buffer;

	shpool = (ngx_slab_pool_t *)shm_zone->shm.addr;
	cache = shpool->data;

	hash = ngx_crc32_short(key, BUFFER_CACHE_KEY_SIZE);

	ngx_shmtx_lock(&shpool->mutex);

	// make sure the entry does not already exist
	entry = ngx_buffer_cache_rbtree_lookup(&cache->rbtree, key, hash);
	if (entry != NULL)
	{
		goto error;
	}

	// allocate a new entry
	entry = ngx_buffer_cache_allocate_cache_entry(cache, key, hash);
	if (entry == NULL)
	{
		goto error;
	}

	// allocate a buffer to hold the data
	target_buffer = ngx_buffer_cache_allocate_buffer(
		cache,
		entry,
		buffer_size);
	if (target_buffer == NULL)
	{
		goto error;
	}

	// Note: the memcpy is performed after releasing the lock to avoid holding the lock for a long time
	//		setting the access time of the entry prevents it from being freed
	entry->access_time = ngx_time();

	ngx_shmtx_unlock(&shpool->mutex);

	ngx_memcpy(target_buffer, source_buffer, buffer_size);

	// Note: no need to obtain the lock since state is ngx_atomic_t
	entry->state = CES_READY;

	return 1;

error:
	ngx_shmtx_unlock(&shpool->mutex);
	return 0;
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
