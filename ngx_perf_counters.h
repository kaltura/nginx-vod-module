#ifndef _NGX_PERF_COUNTERS_H_INCLUDED_
#define _NGX_PERF_COUNTERS_H_INCLUDED_

// includes
#include <ngx_core.h>

// comment the line below to remove the support for performance counters
#define NGX_PERF_COUNTERS_ENABLED

// get tick count
#if (NGX_HAVE_CLOCK_GETTIME)

typedef struct timespec ngx_tick_count_t;

#define ngx_get_tick_count(tp)  (void) clock_gettime(CLOCK_MONOTONIC, tp)

#define ngx_tick_count_diff(start, end) \
	(((end).tv_sec - (start).tv_sec) * 1000000 + ((end).tv_nsec - (start).tv_nsec) / 1000)

#else

typedef struct timeval ngx_tick_count_t;

#define ngx_get_tick_count(tp) ngx_gettimeofday(tp)

#define ngx_tick_count_diff(start, end) \
	(((end).tv_sec - (start).tv_sec) * 1000000 + ((end).tv_usec - (start).tv_usec))
	
#endif // NGX_HAVE_CLOCK_GETTIME

#ifdef NGX_PERF_COUNTERS_ENABLED

// perf counters macros
#define ngx_perf_counter_get_state(shm_zone)						\
	(shm_zone != NULL ? ((ngx_slab_pool_t *)shm_zone->shm.addr)->data : NULL)

#define ngx_perf_counter_context(ctx)								\
	ngx_perf_counter_context_t ctx

#define ngx_perf_counter_start(ctx)									\
	ngx_get_tick_count(&ctx.start);

// Note: the calculation of 'max' has a race condition, the value can decrease since the condition
//		and the assignment are not performed atomically. however, the value of max is expected to
//		converge quickly so that its updates will be performed less and less frequently, so it 
//		should be accurate enough.
#define ngx_perf_counter_end(state, ctx, type)						\
	if (state != NULL)												\
	{																\
		ngx_tick_count_t __end;										\
		ngx_atomic_t __delta;										\
																	\
		ngx_get_tick_count(&__end);									\
																	\
		__delta = ngx_tick_count_diff(ctx.start, __end);			\
		(void)ngx_atomic_fetch_add(&state->counters[type].sum, __delta);	\
		(void)ngx_atomic_fetch_add(&state->counters[type].count, 1);		\
		if (__delta > state->counters[type].max)					\
		{															\
			struct timeval __tv;									\
			ngx_gettimeofday(&__tv);								\
			state->counters[type].max = __delta;					\
			state->counters[type].max_time = __tv.tv_sec;			\
			state->counters[type].max_pid = ngx_pid;				\
		}															\
	}

#define ngx_perf_counter_copy(target, source)	target = source

// typedefs
enum {
#define PC(id, name) PC_##id,
	#include "ngx_perf_counters_x.h"
#undef PC

	PC_COUNT
};

typedef struct {
	ngx_tick_count_t start;
} ngx_perf_counter_context_t;

#else

// empty macros
#define ngx_perf_counter_get_state(shm_zone) (NULL)
#define ngx_perf_counter_context(ctx)
#define ngx_perf_counter_start(ctx)
#define ngx_perf_counter_end(state, ctx, type)
#define ngx_perf_counter_copy(target, source)

#define PC_COUNT (0)

#endif // NGX_PERF_COUNTERS_ENABLED

// typedefs
typedef struct {
	ngx_atomic_t sum;
	ngx_atomic_t count;
	ngx_atomic_t max;
	ngx_atomic_t max_time;
	ngx_atomic_t max_pid;
} ngx_perf_counter_t;

typedef struct {
	ngx_perf_counter_t counters[PC_COUNT];
} ngx_perf_counters_t;

// globals
extern const ngx_str_t perf_counters_open_tags[];
extern const ngx_str_t perf_counters_close_tags[];

// functions
ngx_shm_zone_t* ngx_perf_counters_create_zone(ngx_conf_t *cf, ngx_str_t *name, void *tag);

#endif // _NGX_PERF_COUNTERS_H_INCLUDED_
