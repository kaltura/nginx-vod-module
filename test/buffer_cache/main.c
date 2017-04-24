// include
#include "ngx_cycle.h"
#include "ngx_buffer_cache_internal.h"

// macros
#define RAND(min, max) (rand() % ((max) - (min) + 1) + (min))
//#define VERBOSE

// globals
ngx_time_t ngx_time;
ngx_shm_zone_t shm_zone;
volatile ngx_cycle_t  *ngx_cycle;
volatile ngx_time_t	 *ngx_cached_time = &ngx_time;

// nginx function stubs
#if (NGX_HAVE_VARIADIC_MACROS)

void
ngx_log_error_core(ngx_uint_t level, ngx_log_t *log, ngx_err_t err,
	const char *fmt, ...)

#else

void
ngx_log_error_core(ngx_uint_t level, ngx_log_t *log, ngx_err_t err,
	const char *fmt, va_list args)

#endif
{
}

void ngx_cdecl
ngx_conf_log_error(ngx_uint_t level, ngx_conf_t *cf, ngx_err_t err,
	const char *fmt, ...)
{
}

void
ngx_shmtx_lock(ngx_shmtx_t *mtx)
{
}

void
ngx_shmtx_unlock(ngx_shmtx_t *mtx)
{
}

ngx_shm_zone_t *
ngx_shared_memory_add(ngx_conf_t *cf, ngx_str_t *name, size_t size, void *tag)
{
	return &shm_zone;
}

// buffer cache initialization
static ngx_flag_t
init_buffer_cache(size_t size)
{
	ngx_conf_t cf;
	ngx_log_t log;

	ngx_time.sec = 0;
	ngx_memzero(&shm_zone, sizeof(shm_zone));
	shm_zone.shm.size = size;
	shm_zone.shm.addr = malloc(shm_zone.shm.size);
	if (shm_zone.shm.addr == NULL)
	{
		return 0;
	}

	ngx_memzero(&cf, sizeof(cf));
	ngx_memzero(&log, sizeof(log));
	cf.log = &log;
	cf.pool = ngx_create_pool(NGX_DEFAULT_POOL_SIZE, &log);
	ngx_buffer_cache_create(&cf, NULL, 0, 0, NULL);

	shm_zone.init(&shm_zone, NULL);
	return 1;
}

static void
free_buffer_cache()
{
	free(shm_zone.shm.addr);
	shm_zone.shm.addr = NULL;
}

// debugging functions
void print_queue(ngx_queue_t* queue, const char* name, u_char* relative_offset)
{
	ngx_buffer_cache_entry_t* entry;
	ngx_queue_t* cur;

	printf("%s:\n", name);
	for (cur = queue->next; cur != queue; cur = cur->next)
	{
		entry = container_of(cur, ngx_buffer_cache_entry_t, queue_node);

		printf("\tSO=%lx BS=%zx ST=%lu\n", entry->start_offset - relative_offset, entry->buffer_size, entry->state);
	}
}

void print_cache_status(ngx_buffer_cache_sh_t *cache)
{
	printf("ES=%lx EE=%lx BS=%lx BW=%lx BR=%lx BE=%lx\n", 
		(u_char*)cache->entries_start - (u_char*)cache->entries_start,
		(u_char*)cache->entries_end - (u_char*)cache->entries_start,
		(u_char*)cache->buffers_start - (u_char*)cache->entries_start,
		(u_char*)cache->buffers_write - (u_char*)cache->entries_start,
		(u_char*)cache->buffers_read - (u_char*)cache->entries_start,
		(u_char*)cache->buffers_end - (u_char*)cache->entries_start);

	print_queue(&cache->used_queue, "used queue", (u_char*)cache->entries_start);
}

// test functions
void generate_random_buffer(unsigned int seed, u_char* buffer, size_t size)
{
	short filler = rand_r(&seed) & 0xFFFF;
	short* end_pos = (short*)(buffer + (size & ~1));
	short* cur_pos;
	
	for (cur_pos = (short*)buffer; cur_pos < end_pos; cur_pos++)
	{
		*cur_pos = filler;
	}
	
	if (size & 1)
	{
		buffer[size - 1] = rand_r(&seed) & 0xFF;
	}
}

int validate_random_buffer(unsigned int seed, const u_char* buffer, size_t size)
{
	short filler = rand_r(&seed) & 0xFFFF;
	short* end_pos = (short*)(buffer + (size & ~1));
	short* cur_pos;
	
	for (cur_pos = (short*)buffer; cur_pos < end_pos; cur_pos++)
	{
		if (*cur_pos != filler)
		{
			return 0;
		}
	}
	
	if ((size & 1) && buffer[size - 1] != (rand_r(&seed) & 0xFF))
	{
		return 0;
	}
	
	return 1;
}

int run_test_cycle(time_t seed, size_t cache_size, int iterations, int size_factor)
{
	ngx_buffer_cache_stats_t stats;
	u_char key[BUFFER_CACHE_KEY_SIZE];
	ngx_str_t fetch_buffer;
	u_char* store_buffer;
	size_t* sizes_buffer;
	size_t size;
	size_t max_size;
	int min_existing_index = 0;
	int i, j;

	printf("starting test - seed %llu cache_size %zu iterations %d size factor %d\n", (unsigned long long)seed, cache_size, iterations, size_factor);

	srand(seed);
	
	sizes_buffer = malloc(sizeof(sizes_buffer[0]) * iterations);
	if (sizes_buffer == NULL)
	{
		printf("Error: failed to allocate sizes buffer\n");
		return 0;
	}
	
	store_buffer = malloc(cache_size);
	if (store_buffer == NULL)
	{
		printf("Error: failed to allocate store buffer\n");
		return 0;
	}

	if (!init_buffer_cache(cache_size))
	{
		printf("Error: failed to initialize the buffer cache\n");
		return 0;
	}

	for (i = 0; i < iterations; i++)
	{
		ngx_buffer_cache_sh_t *sh;
		ngx_buffer_cache_t *cache;

		cache = shm_zone.data;
		sh = cache->sh;

#ifdef VERBOSE
		printf("%d. ", i);
		print_cache_status(sh);
#endif

		ngx_time.sec += ENTRY_LOCK_EXPIRATION + 1;
		((uint32_t*)&key)[0] = i;

		if (RAND(0, iterations) == 0)
		{
#ifdef VERBOSE
			printf("resetting the cache\n");
#endif
			sh->reset = 1;
		}
		
		max_size = (sh->buffers_end - (u_char*)(sh->entries_end + ENTRIES_ALLOC_MARGIN + 1) - BUFFER_ALIGNMENT) / size_factor;
		size = RAND(0, max_size);
		sizes_buffer[i] = size;
		generate_random_buffer(i, store_buffer, size);
		
#ifdef VERBOSE
		printf("storing size=%zx\n", size);
#endif
				
		if (!ngx_buffer_cache_store(cache, key, store_buffer, size))
		{
			printf("Error: store failed\n");
			return 0;
		}
		
		for (j = min_existing_index; j <= i; j++)
		{
			((uint32_t*)&key)[0] = j;
			if (ngx_buffer_cache_fetch(cache, key, &fetch_buffer))
			{
				if (sizes_buffer[j] != fetch_buffer.len)
				{
					printf("Error: invalid buffer size\n");
					return 0;
				}
				
				if (!validate_random_buffer(j, fetch_buffer.data, fetch_buffer.len))
				{
					printf("Error: invalid buffer content\n");
					return 0;
				}
			}
			else
			{
				min_existing_index++;
			}			
		}

#ifdef VERBOSE
		printf("validated %d buffers\n", i + 1 - min_existing_index);
#endif

		ngx_buffer_cache_get_stats(cache, &stats);
		if (stats.store_ok != i + 1)
		{
			printf("Error: invalid store_ok value, actual=%lu expected=%d\n", stats.store_ok, i + 1);
			return 0;
		}
		
		if (stats.store_ok - stats.evicted != i + 1 - min_existing_index)
		{
			printf("Error: unexpected number of items in the cache, stats=%lu fetched=%d\n", stats.store_ok - stats.evicted, i + 1 - min_existing_index);
			return 0;
		}
		
#ifndef VERBOSE
		if (((i + 1) & 0xF) == 0)
		{
			printf(".");
		}
#endif
	}

	free_buffer_cache();

	free(store_buffer);
	
	free(sizes_buffer);
	
	printf("\n");

	return 1;
}

int main()
{
	setbuf(stdout, NULL);		// disable stdout buffering (for progress indication)
	
	while (run_test_cycle(time(NULL), RAND(2 * 1024 * 1024, 16 * 1024 * 1024), 1000, 1 << RAND(0, 6)));

	return 0;
}

