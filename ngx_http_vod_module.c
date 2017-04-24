#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <nginx.h>
#include <ngx_event.h>
#include <ngx_md5.h>

#include "ngx_http_vod_module.h"
#include "ngx_http_vod_submodule.h"
#include "ngx_http_vod_request_parse.h"
#include "ngx_child_http_request.h"
#include "ngx_http_vod_utils.h"
#include "ngx_perf_counters.h"
#include "ngx_http_vod_conf.h"
#include "ngx_file_reader.h"
#include "ngx_buffer_cache.h"
#include "vod/mp4/mp4_format.h"
#include "vod/mkv/mkv_format.h"
#include "vod/subtitle/webvtt_format.h"
#include "vod/subtitle/dfxp_format.h"
#include "vod/subtitle/cap_format.h"
#include "vod/input/read_cache.h"
#include "vod/filters/audio_filter.h"
#include "vod/filters/dynamic_clip.h"
#include "vod/filters/concat_clip.h"
#include "vod/filters/rate_filter.h"
#include "vod/filters/filter.h"
#include "vod/media_set_parser.h"
#include "vod/manifest_utils.h"
#include "vod/thumb/thumb_grabber.h"
#include "ngx_http_vod_thumb.h"

// macros
#define DEFINE_VAR(name) \
	{ ngx_string("vod_" #name), ngx_http_vod_set_##name##_var, 0 }

// constants
#define OPEN_FILE_FALLBACK_ENABLED (0x80000000)
#define MAX_STALE_RETRIES (2)

enum {
	// mapping state machine
	STATE_MAP_INITIAL,
	STATE_MAP_OPEN,
	STATE_MAP_READ,

	// main state machine
	STATE_READ_DRM_INFO,
	STATE_READ_METADATA_INITIAL,
	STATE_READ_METADATA_OPEN_FILE,
	STATE_READ_METADATA_READ,
	STATE_READ_FRAMES_OPEN_FILE,
	STATE_READ_FRAMES_READ,
	STATE_OPEN_FILE,
	STATE_FILTER_FRAMES,
	STATE_PROCESS_FRAMES,
	STATE_DUMP_OPEN_FILE,
	STATE_DUMP_FILE_PART,
};

enum {
	READER_FILE,
	READER_HTTP,
	READER_COUNT
};

// typedefs
struct ngx_http_vod_ctx_s;
typedef struct ngx_http_vod_ctx_s ngx_http_vod_ctx_t;

typedef ngx_int_t(*ngx_http_vod_state_machine_t)(ngx_http_vod_ctx_t* ctx);
typedef ngx_int_t(*ngx_http_vod_open_file_t)(ngx_http_request_t* r, ngx_str_t* path, uint32_t flags, void** context);
typedef ngx_int_t(*ngx_http_vod_async_read_func_t)(void* context, ngx_buf_t *buf, size_t size, off_t offset);
typedef ngx_int_t(*ngx_http_vod_dump_part_t)(void* context, off_t start, off_t end);
typedef size_t(*ngx_http_vod_get_size_t)(void* context);
typedef ngx_int_t(*ngx_http_vod_enable_directio_t)(void* context);

typedef ngx_int_t(*ngx_http_vod_dump_request_t)(ngx_http_vod_ctx_t* context);
typedef ngx_int_t(*ngx_http_vod_mapping_apply_t)(ngx_http_vod_ctx_t *ctx, ngx_str_t* mapping, int* cache_index);
typedef ngx_int_t(*ngx_http_vod_mapping_get_uri_t)(ngx_http_vod_ctx_t *ctx, ngx_str_t* uri);

typedef struct {
	uint32_t type;
	uint32_t part_count;
} multipart_cache_header_t;

typedef struct {
	size_t content_type_len;
	uint32_t media_set_type;
} response_cache_header_t;

typedef struct {
	ngx_http_request_t* r;
	ngx_chain_t* chain_head;
	ngx_chain_t* chain_end;
	size_t total_size;
} ngx_http_vod_write_segment_context_t;

typedef struct {
	ngx_http_request_t* r;
	ngx_str_t cur_remote_suburi;
	ngx_str_t upstream_location;
} ngx_http_vod_http_reader_state_t;

typedef struct {
	off_t alignment;
	size_t extra_size;
} ngx_http_vod_alloc_params_t;

typedef struct {
	u_char cache_key[MEDIA_CLIP_KEY_SIZE];
	ngx_str_t* cache_key_prefix;
	ngx_buffer_cache_t** caches;
	uint32_t cache_count;
	uint32_t stale_retries;
	void* reader_context;
	size_t max_response_size;
	ngx_http_vod_mapping_get_uri_t get_uri;
	ngx_http_vod_mapping_apply_t apply;
} ngx_http_vod_mapping_context_t;

typedef struct {
	ngx_http_vod_open_file_t open;
	ngx_http_vod_dump_part_t dump_part;
	ngx_http_vod_dump_request_t dump_request;
	ngx_http_vod_get_size_t get_size;
	ngx_http_vod_enable_directio_t enable_directio;
} ngx_http_vod_reader_t;

struct ngx_http_vod_ctx_s {
	// base params
	ngx_http_vod_submodule_context_t submodule_context;
	const struct ngx_http_vod_request_s* request;
	ngx_http_vod_alloc_params_t alloc_params[READER_COUNT];
	off_t alignment;
	int state;
	u_char request_key[BUFFER_CACHE_KEY_SIZE];
	u_char child_request_key[BUFFER_CACHE_KEY_SIZE];
	ngx_http_vod_state_machine_t state_machine;

	// iterators
	media_sequence_t* cur_sequence;
	media_clip_source_t* cur_source;
	media_clip_t* cur_clip;

	// performance counters
	int perf_counter_async_read;
	ngx_perf_counters_t* perf_counters;
	ngx_perf_counter_context(perf_counter_context);
	ngx_perf_counter_context(total_perf_counter_context);

	// mapping
	ngx_http_vod_mapping_context_t mapping;

	// read metadata state
	ngx_buf_t read_buffer;
	uint32_t read_flags;
	media_format_t* format;
	ngx_buf_t prefix_buffer;
	off_t requested_offset;
	off_t read_offset;
	void* metadata_reader_context;
	ngx_str_t* metadata_parts;
	size_t metadata_part_count;

	// read frames state
	media_base_metadata_t* base_metadata;
	media_format_read_request_t frames_read_req;

	// clipper
	media_clipper_parse_result_t* clipper_parse_result;

	// reading abstraction (over file / http)
	ngx_http_vod_reader_t* reader;
	ngx_http_vod_async_read_func_t read;
	int alloc_params_index;

	// read state - file
#if (NGX_THREADS)
	void* async_open_context;
#endif

	// read state - http
	ngx_str_t* file_key_prefix;
	ngx_str_t upstream_extra_args;

	// segment requests only
	size_t content_length;
	read_cache_state_t read_cache_state;
	ngx_http_vod_frame_processor_t frame_processor;
	void* frame_processor_state;
	ngx_chain_t out;
	ngx_http_vod_write_segment_context_t write_segment_buffer_context;
	media_notification_t* notification;
	uint32_t frames_bytes_read;
};

// typedefs
typedef struct {
	ngx_str_t name;
	ngx_http_get_variable_pt handler;
	uintptr_t data;
} ngx_http_vod_variable_t;

// forward declarations
static ngx_int_t ngx_http_vod_run_state_machine(ngx_http_vod_ctx_t *ctx);
static ngx_int_t ngx_http_vod_send_notification(ngx_http_vod_ctx_t *ctx);
static ngx_int_t ngx_http_vod_init_process(ngx_cycle_t *cycle);
static void ngx_http_vod_exit_process();

static ngx_int_t ngx_http_vod_init_file_reader_with_fallback(ngx_http_request_t *r, ngx_str_t* path, uint32_t flags, void** context);
static ngx_int_t ngx_http_vod_init_file_reader(ngx_http_request_t *r, ngx_str_t* path, uint32_t flags, void** context);
static ngx_int_t ngx_http_vod_dump_file(ngx_http_vod_ctx_t* ctx);

static ngx_int_t ngx_http_vod_http_reader_open_file(ngx_http_request_t* r, ngx_str_t* path, uint32_t flags, void** context);
static ngx_int_t ngx_http_vod_dump_http_part(void* context, off_t start, off_t end);
static ngx_int_t ngx_http_vod_dump_http_request(ngx_http_vod_ctx_t *ctx);

// globals
ngx_module_t  ngx_http_vod_module = {
    NGX_MODULE_V1,
    &ngx_http_vod_module_ctx,         /* module context */
    ngx_http_vod_commands,            /* module directives */
    NGX_HTTP_MODULE,                  /* module type */
    NULL,                             /* init master */
    NULL,                             /* init module */
    ngx_http_vod_init_process,        /* init process */
    NULL,                             /* init thread */
    NULL,                             /* exit thread */
    ngx_http_vod_exit_process,        /* exit process */
    ngx_http_vod_exit_process,        /* exit master */
    NGX_MODULE_V1_PADDING
};

static ngx_str_t options_content_type = ngx_string("text/plain");
static ngx_str_t empty_string = ngx_null_string;

static media_format_t* media_formats[] = {
	&mp4_format,
	// XXXXX add &mkv_format,
	&webvtt_format,
#if (VOD_HAVE_LIBXML2)
	&dfxp_format,
#endif
	&cap_format,
	NULL
};

static ngx_http_vod_reader_t reader_file_with_fallback = {
	ngx_http_vod_init_file_reader_with_fallback,
	ngx_file_reader_dump_file_part,
	ngx_http_vod_dump_file,
	ngx_file_reader_get_size,
	(ngx_http_vod_enable_directio_t)ngx_file_reader_enable_directio,
};

static ngx_http_vod_reader_t reader_file = {
	ngx_http_vod_init_file_reader,
	ngx_file_reader_dump_file_part,
	ngx_http_vod_dump_file,
	ngx_file_reader_get_size,
	(ngx_http_vod_enable_directio_t)ngx_file_reader_enable_directio,
};

static ngx_http_vod_reader_t reader_http = {
	ngx_http_vod_http_reader_open_file,
	ngx_http_vod_dump_http_part,
	ngx_http_vod_dump_http_request,
	NULL,
	NULL,
};

static const u_char wvm_file_magic[] = { 0x00, 0x00, 0x01, 0xba, 0x44, 0x00, 0x04, 0x00, 0x04, 0x01 };

////// Variables

static ngx_int_t
ngx_http_vod_set_status_var(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data)
{
	// this variable is explicitly set when an error is mapped, if we got here, there's no value
	v->not_found = 1;
	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_set_filepath_var(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data)
{
	ngx_http_vod_ctx_t *ctx;
	ngx_str_t* value;

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);
	if (ctx == NULL ||
		ctx->cur_sequence < ctx->submodule_context.media_set.sequences ||
		ctx->cur_sequence >= ctx->submodule_context.media_set.sequences_end)
	{
		v->not_found = 1;
		return NGX_OK;
	}

	value = &ctx->cur_sequence->mapped_uri;
	if (value->len == 0)
	{
		v->not_found = 1;
		return NGX_OK;
	}

	v->valid = 1;
	v->no_cacheable = 1;
	v->not_found = 0;
	v->len = value->len;
	v->data = value->data;

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_set_suburi_var(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data)
{
	ngx_http_vod_ctx_t *ctx;
	media_sequence_t* sequence;
	ngx_str_t* value;

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);
	if (ctx == NULL)
	{
		v->not_found = 1;
		return NGX_OK;
	}

	if (ctx->cur_sequence >= ctx->submodule_context.media_set.sequences &&
		ctx->cur_sequence < ctx->submodule_context.media_set.sequences_end)
	{
		sequence = ctx->cur_sequence;
	}
	else if (ctx->cur_source != NULL)
	{
		sequence = ctx->cur_source->sequence;
	}
	else
	{
		v->not_found = 1;
		return NGX_OK;
	}

	value = &sequence->stripped_uri;
	if (value->len == 0)
	{
		v->not_found = 1;
		return NGX_OK;
	}

	v->valid = 1;
	v->no_cacheable = 1;
	v->not_found = 0;
	v->len = value->len;
	v->data = value->data;

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_set_set_id_var(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data)
{
	ngx_http_vod_ctx_t *ctx;
	ngx_str_t* value;

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);
	if (ctx == NULL)
	{
		v->not_found = 1;
		return NGX_OK;
	}

	value = &ctx->submodule_context.media_set.id;
	if (value->len == 0)
	{
		v->not_found = 1;
		return NGX_OK;
	}

	v->valid = 1;
	v->no_cacheable = 1;
	v->not_found = 0;
	v->len = value->len;
	v->data = value->data;

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_set_sequence_id_var(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data)
{
	ngx_http_vod_ctx_t *ctx;
	ngx_str_t* value;

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);
	if (ctx == NULL ||
		ctx->cur_sequence < ctx->submodule_context.media_set.sequences ||
		ctx->cur_sequence >= ctx->submodule_context.media_set.sequences_end)
	{
		v->not_found = 1;
		return NGX_OK;
	}

	value = &ctx->cur_sequence->id;
	if (value->len == 0)
	{
		value = &ctx->cur_sequence->stripped_uri;
		if (value->len == 0)
		{
			v->not_found = 1;
			return NGX_OK;
		}
	}

	v->valid = 1;
	v->no_cacheable = 1;
	v->not_found = 0;
	v->len = value->len;
	v->data = value->data;

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_set_clip_id_var(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data)
{
	ngx_http_vod_ctx_t *ctx;
	media_clip_t* cur_clip;
	ngx_str_t* value;

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);
	if (ctx == NULL)
	{
		goto not_found;
	}

	cur_clip = ctx->cur_clip;
	if (cur_clip == NULL)
	{
		goto not_found;
	}

	switch (cur_clip->type)
	{
	case MEDIA_CLIP_SOURCE:
		value = &((media_clip_source_t*)cur_clip)->mapped_uri;
		break;

	case MEDIA_CLIP_DYNAMIC:
		value = &((media_clip_dynamic_t*)cur_clip)->id;
		break;

	default:
		goto not_found;
	}

	v->valid = 1;
	v->no_cacheable = 1;
	v->not_found = 0;
	v->len = value->len;
	v->data = value->data;

	return NGX_OK;

not_found:

	v->not_found = 1;
	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_set_dynamic_mapping_var(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data)
{
	ngx_http_vod_ctx_t *ctx;
	vod_status_t rc;
	ngx_str_t value;

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);
	if (ctx == NULL)
	{
		v->not_found = 1;
		return NGX_OK;
	}

	rc = dynamic_clip_get_mapping_string(
		&ctx->submodule_context.request_context,
		ctx->submodule_context.media_set.dynamic_clips_head,
		&value);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_set_dynamic_mapping_var: dynamic_clip_get_mapping_string failed %i", rc);
		return NGX_ERROR;
	}

	v->data = value.data;
	v->len = value.len;
	v->valid = 1;
	v->no_cacheable = 1;
	v->not_found = 0;

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_set_request_params_var(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data)
{
	request_params_t* request_params;
	ngx_http_vod_ctx_t *ctx;
	vod_status_t rc;
	ngx_str_t value;

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);
	if (ctx == NULL)
	{
		v->not_found = 1;
		return NGX_OK;
	}

	request_params = &ctx->submodule_context.request_params;

	rc = manifest_utils_build_request_params_string(
		&ctx->submodule_context.request_context,
		request_params->tracks_mask,		// the media set may not be ready yet, include all tracks that were passed on the request
		request_params->segment_index,
		request_params->sequences_mask,
		request_params->sequence_tracks_mask,
		request_params->tracks_mask,
		&value);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_set_request_params_var: manifest_utils_build_request_params_string failed %i", rc);
		return NGX_ERROR;
	}

	if (value.len > 0 && value.data[0] == '-')
	{
		value.data++;
		value.len--;
	}

	v->data = value.data;
	v->len = value.len;
	v->valid = 1;
	v->no_cacheable = 1;
	v->not_found = 0;

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_set_notification_id_var(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data)
{
	ngx_http_vod_ctx_t *ctx;
	ngx_str_t* value;

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);
	if (ctx == NULL || ctx->notification == NULL)
	{
		v->not_found = 1;
		return NGX_OK;
	}

	value = &ctx->notification->id;
	v->data = value->data;
	v->len = value->len;
	v->valid = 1;
	v->no_cacheable = 1;
	v->not_found = 0;

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_set_uint32_var(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data)
{
	ngx_http_vod_ctx_t *ctx;
	uint32_t int_value;
	u_char* p;

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);
	if (ctx == NULL)
	{
		v->not_found = 1;
		return NGX_OK;
	}

	p = ngx_pnalloc(r->pool, NGX_INT32_LEN);
	if (p == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_set_uint32_var: ngx_pnalloc failed");
		return NGX_ERROR;
	}

	int_value = *(uint32_t*)(((u_char*)ctx) + data);

	v->data = p;
	v->len = ngx_sprintf(p, "%uD", int_value) - p;
	v->valid = 1;
	v->no_cacheable = 1;
	v->not_found = 0;

	return NGX_OK;
}

static ngx_http_vod_variable_t ngx_http_vod_variables[] = {
	DEFINE_VAR(status),
	DEFINE_VAR(filepath),
	DEFINE_VAR(suburi),
	DEFINE_VAR(set_id),
	DEFINE_VAR(sequence_id),
	DEFINE_VAR(clip_id),
	DEFINE_VAR(dynamic_mapping),
	DEFINE_VAR(request_params),
	DEFINE_VAR(notification_id),
	{ ngx_string("vod_frames_bytes_read"), ngx_http_vod_set_uint32_var, offsetof(ngx_http_vod_ctx_t, frames_bytes_read) },
	{ ngx_string("vod_segment_duration"), ngx_http_vod_set_uint32_var, offsetof(ngx_http_vod_ctx_t, submodule_context.media_set.segment_duration) },
};

ngx_int_t
ngx_http_vod_preconfiguration(ngx_conf_t *cf)
{
	ngx_http_vod_variable_t* vars_cur = ngx_http_vod_variables;
	ngx_http_vod_variable_t* vars_end = vars_cur + vod_array_entries(ngx_http_vod_variables);
	ngx_http_variable_t  *var;
	ngx_int_t rc;

	for (; vars_cur < vars_end; vars_cur++)
	{
		var = ngx_http_add_variable(cf, &vars_cur->name, NGX_HTTP_VAR_NOCACHEABLE);
		if (var == NULL)
		{
			return NGX_ERROR;
		}

		var->get_handler = vars_cur->handler;
		var->data = vars_cur->data;
	}

	rc = ngx_http_get_variable_index(cf, &ngx_http_vod_variables[0].name);
	if (rc == NGX_ERROR) 
	{
		return NGX_ERROR;
	}

	ngx_http_vod_set_status_index(rc);

	dfxp_init_process();

	webvtt_init_process(cf->log);

	return NGX_OK;
}

////// Perf counter wrappers

static ngx_flag_t
ngx_buffer_cache_fetch_perf(
	ngx_perf_counters_t* perf_counters,
	ngx_buffer_cache_t* cache,
	u_char* key,
	ngx_str_t* buffer)
{
	ngx_perf_counter_context(pcctx);
	ngx_flag_t result;
	
	ngx_perf_counter_start(pcctx);

	result = ngx_buffer_cache_fetch(cache, key, buffer);

	ngx_perf_counter_end(perf_counters, pcctx, PC_FETCH_CACHE);

	return result;
}

static int
ngx_buffer_cache_fetch_multi_perf(
	ngx_perf_counters_t* perf_counters,
	ngx_buffer_cache_t** caches,
	uint32_t cache_count,
	u_char* key,
	ngx_str_t* buffer)
{
	ngx_perf_counter_context(pcctx);
	ngx_buffer_cache_t* cache;
	ngx_flag_t result;
	uint32_t cache_index;

	ngx_perf_counter_start(pcctx);

	for (cache_index = 0; cache_index < cache_count; cache_index++)
	{
		cache = caches[cache_index];
		if (cache == NULL)
		{
			continue;
		}

		result = ngx_buffer_cache_fetch(cache, key, buffer);
		if (!result)
		{
			continue;
		}

		ngx_perf_counter_end(perf_counters, pcctx, PC_FETCH_CACHE);

		return cache_index;
	}

	ngx_perf_counter_end(perf_counters, pcctx, PC_FETCH_CACHE);

	return -1;
}

static int
ngx_buffer_cache_fetch_copy_perf(
	ngx_http_request_t* r,
	ngx_perf_counters_t* perf_counters,
	ngx_buffer_cache_t** caches,
	uint32_t cache_count,
	u_char* key,
	ngx_str_t* buffer)
{
	ngx_str_t original_buffer;
	u_char* buffer_copy;
	int result;

	result = ngx_buffer_cache_fetch_multi_perf(
		perf_counters,
		caches,
		cache_count,
		key,
		&original_buffer);
	if (result < 0)
	{
		return result;
	}

	buffer_copy = ngx_palloc(r->pool, original_buffer.len + 1);
	if (buffer_copy == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_buffer_cache_fetch_copy_perf: ngx_palloc failed");
		return -1;
	}

	ngx_memcpy(buffer_copy, original_buffer.data, original_buffer.len);
	buffer_copy[original_buffer.len] = '\0';

	buffer->data = buffer_copy;
	buffer->len = original_buffer.len;

	return result;
}

static ngx_flag_t
ngx_buffer_cache_store_perf(
	ngx_perf_counters_t* perf_counters,
	ngx_buffer_cache_t* cache,
	u_char* key,
	u_char* source_buffer,
	size_t buffer_size)
{
	ngx_perf_counter_context(pcctx);
	ngx_flag_t result;

	ngx_perf_counter_start(pcctx);

	result = ngx_buffer_cache_store(cache, key, source_buffer, buffer_size);

	ngx_perf_counter_end(perf_counters, pcctx, PC_STORE_CACHE);

	return result;
}

static ngx_flag_t 
ngx_buffer_cache_store_gather_perf(
	ngx_perf_counters_t* perf_counters,
	ngx_buffer_cache_t* cache,
	u_char* key,
	ngx_str_t* buffers,
	size_t buffer_count)
{
	ngx_perf_counter_context(pcctx);
	ngx_flag_t result;

	ngx_perf_counter_start(pcctx);

	result = ngx_buffer_cache_store_gather(cache, key, buffers, buffer_count);

	ngx_perf_counter_end(perf_counters, pcctx, PC_STORE_CACHE);

	return result;
}

////// Multipart cache functions

static ngx_flag_t 
ngx_buffer_cache_store_multipart_perf(
	ngx_http_vod_ctx_t *ctx,
	ngx_buffer_cache_t* cache,
	u_char* key,
	multipart_cache_header_t* header,
	ngx_str_t* parts)
{
	ngx_str_t* buffers;
	ngx_str_t* cur_part;
	ngx_str_t* parts_end;
	uint32_t part_count = header->part_count;
	u_char* p;
	size_t* cur_size;
	
	p = ngx_palloc(
		ctx->submodule_context.request_context.pool, 
		sizeof(buffers[0]) * (part_count + 1) + sizeof(*header) + sizeof(size_t) * part_count);
	if (p == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
			"ngx_buffer_cache_store_multipart_perf: ngx_palloc failed");
		return 0;
	}

	buffers = (void*)p;
	p += sizeof(buffers[0]) * (part_count + 1);

	buffers[0].data = p;
	buffers[0].len = sizeof(*header) + sizeof(size_t) * part_count;
	ngx_memcpy(buffers + 1, parts, part_count * sizeof(buffers[0]));

	p = ngx_copy(p, header, sizeof(*header));

	cur_size = (void*)p;
	parts_end = parts + part_count;
	for (cur_part = parts; cur_part < parts_end; cur_part++)
	{
		*cur_size++ = cur_part->len;
	}

	return ngx_buffer_cache_store_gather_perf(
		ctx->perf_counters,
		cache,
		key,
		buffers,
		part_count + 1);
}

static ngx_flag_t
ngx_buffer_cache_fetch_multipart_perf(
	ngx_http_vod_ctx_t *ctx,
	ngx_buffer_cache_t* cache,
	u_char* key,
	multipart_cache_header_t* header,
	ngx_str_t** out_parts)
{
	vod_str_t* cur_part;
	vod_str_t* parts;
	ngx_str_t cache_buffer;
	uint32_t part_count;
	size_t* part_sizes;
	size_t cur_size;
	u_char* end;
	u_char* p;

	if (!ngx_buffer_cache_fetch_perf(
		ctx->perf_counters,
		cache,
		key,
		&cache_buffer))
	{
		return 0;
	}

	if (cache_buffer.len < sizeof(*header))
	{
		ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
			"ngx_buffer_cache_fetch_multipart_perf: size %uz smaller than header size", cache_buffer.len);
		return 0;
	}

	p = cache_buffer.data;
	end = p + cache_buffer.len;

	*header = *(multipart_cache_header_t*)p;
	p += sizeof(*header);

	part_count = header->part_count;
	if ((size_t)(end - p) < part_count * sizeof(part_sizes[0]))
	{
		ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
			"ngx_buffer_cache_fetch_multipart_perf: size %uz too small to hold %uD parts", 
			cache_buffer.len, part_count);
		return 0;
	}
	part_sizes = (void*)p;
	p += part_count * sizeof(part_sizes[0]);

	parts = ngx_palloc(ctx->submodule_context.request_context.pool, sizeof(parts[0]) * part_count);
	if (parts == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
			"ngx_buffer_cache_fetch_multipart_perf: ngx_palloc failed");
		return 0;
	}

	cur_part = parts;

	for (; part_count > 0; part_count--)
	{
		cur_size = *part_sizes++;
		if ((size_t)(end - p) < cur_size)
		{
			ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
				"ngx_buffer_cache_fetch_multipart_perf: size left %uz smaller than part size %uz", 
				(size_t)(end - p), cur_size);
			return 0;
		}

		cur_part->data = p;
		p += cur_size;

		cur_part->len = cur_size;
		cur_part++;
	}

	*out_parts = parts;
	return 1;
}

////// Utility functions

static ngx_int_t
ngx_http_vod_send_header(
	ngx_http_request_t* r, 
	off_t content_length_n, 
	ngx_str_t* content_type, 
	uint32_t media_set_type,
	const ngx_http_vod_request_t* request)
{
	ngx_http_vod_loc_conf_t* conf;
	ngx_int_t rc;
	time_t expires;

	conf = ngx_http_get_module_loc_conf(r, ngx_http_vod_module);

	if (content_type != NULL)
	{
		r->headers_out.content_type = *content_type;
		r->headers_out.content_type_len = content_type->len;
	}
	
	r->headers_out.status = NGX_HTTP_OK;
	r->headers_out.content_length_n = content_length_n;

	// last modified
	if (request == NULL || 
		(request->request_class & (REQUEST_CLASS_SEGMENT | REQUEST_CLASS_THUMB)) != 0 ||
		media_set_type == MEDIA_SET_VOD)
	{
		if (conf->last_modified_time != -1 &&
			ngx_http_test_content_type(r, &conf->last_modified_types) != NULL)
		{
			r->headers_out.last_modified_time = conf->last_modified_time;
		}

		expires = conf->expires[EXPIRES_TYPE_VOD];
	}
	else
	{
		r->headers_out.last_modified_time = ngx_time();

		if ((request->flags & REQUEST_FLAG_TIME_DEPENDENT_ON_LIVE) != 0)
		{
			expires = conf->expires[EXPIRES_TYPE_LIVE_TIME_DEPENDENT];
		}
		else
		{
			expires = conf->expires[EXPIRES_TYPE_LIVE];
		}
	}

	// expires
	if (expires >= 0)
	{
		rc = ngx_http_vod_set_expires(r, expires);
		if (rc != NGX_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_send_header: ngx_http_vod_set_expires failed %i", rc);
			return rc;
		}
	}

	// set the etag
	rc = ngx_http_set_etag(r);
	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_send_header: ngx_http_set_etag failed %i", rc);
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	// send the response headers
	rc = ngx_http_send_header(r);
	if (rc == NGX_ERROR || rc > NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_send_header: ngx_http_send_header failed %i", rc);
		return rc;
	}

	return VOD_OK;
}

static void
ngx_http_vod_finalize_request(ngx_http_vod_ctx_t *ctx, ngx_int_t rc)
{
	if (ctx->submodule_context.r->header_sent && rc != NGX_OK && rc != NGX_AGAIN)
	{
		rc = NGX_ERROR;
	}

	ngx_perf_counter_end(ctx->perf_counters, ctx->total_perf_counter_context, PC_TOTAL);

	ngx_http_finalize_request(ctx->submodule_context.r, rc);
}

static ngx_int_t
ngx_http_vod_alloc_read_buffer(ngx_http_vod_ctx_t *ctx, size_t size, int alloc_params_index)
{
	ngx_http_vod_alloc_params_t* alloc_params = ctx->alloc_params + alloc_params_index;
	u_char* start = ctx->read_buffer.start;

	size += alloc_params->extra_size + VOD_BUFFER_PADDING_SIZE;		// for null termination / ffmpeg padding

	if (start == NULL ||										// no buffer
		start + size > ctx->read_buffer.end ||					// buffer too small
		((intptr_t)start & (alloc_params->alignment - 1)) != 0)	// buffer not conforming to alignment
	{
		if (alloc_params->alignment > 1)
		{
			start = ngx_pmemalign(ctx->submodule_context.request_context.pool, size, alloc_params->alignment);
		}
		else
		{
			start = ngx_palloc(ctx->submodule_context.request_context.pool, size);
		}

		if (start == NULL)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_alloc_read_buffer: failed to allocate read buffer of size %uz", size);
			return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, VOD_ALLOC_FAILED);
		}

		ctx->read_buffer.start = start;
		ctx->read_buffer.end = start + size;
		ctx->read_buffer.temporary = 1;
	}

	ctx->read_buffer.pos = start;
	ctx->read_buffer.last = start;

	return NGX_OK;
}

////// DRM

static void
ngx_http_vod_copy_drm_info(ngx_http_vod_ctx_t *ctx)
{
	media_sequence_t* ref_sequence = ctx->cur_sequence;
	media_sequence_t* cur_sequence;

	for (cur_sequence = ref_sequence + 1;
		cur_sequence < ctx->submodule_context.media_set.sequences_end;
		cur_sequence++)
	{
		cur_sequence->drm_info = ref_sequence->drm_info;
	}
}

static void
ngx_http_vod_drm_info_request_finished(void* context, ngx_int_t rc, ngx_buf_t* response, ssize_t content_length)
{
	ngx_http_vod_loc_conf_t *conf;
	ngx_http_vod_ctx_t *ctx;
	ngx_http_request_t *r = context;
	ngx_str_t drm_info;

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);
	conf = ctx->submodule_context.conf;

	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_drm_info_request_finished: upstream request failed %i", rc);
		goto finalize_request;
	}

	if (response->last >= response->end)
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_drm_info_request_finished: not enough room in buffer for null terminator");
		rc = NGX_HTTP_BAD_GATEWAY;
		goto finalize_request;
	}

	ngx_perf_counter_end(ctx->perf_counters, ctx->perf_counter_context, PC_GET_DRM_INFO);

	drm_info.data = response->pos;
	drm_info.len = content_length;
	*response->last = '\0';

	ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
		"ngx_http_vod_drm_info_request_finished: result %V", &drm_info);

	// parse the drm info
	rc = conf->submodule.parse_drm_info(&ctx->submodule_context, &drm_info, &ctx->cur_sequence->drm_info);
	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_drm_info_request_finished: invalid drm info response %V", &drm_info);
		rc = NGX_HTTP_SERVICE_UNAVAILABLE;
		goto finalize_request;
	}

	// save to cache
	if (conf->drm_info_cache != NULL)
	{
		if (ngx_buffer_cache_store_perf(
			ctx->perf_counters,
			conf->drm_info_cache,
			ctx->child_request_key,
			drm_info.data,
			drm_info.len))
		{
			ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_drm_info_request_finished: stored in drm info cache");
		}
		else
		{
			ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_drm_info_request_finished: failed to store drm info in cache");
		}
	}

	if (conf->drm_single_key)
	{
		ngx_http_vod_copy_drm_info(ctx);
		ctx->cur_sequence = ctx->submodule_context.media_set.sequences_end;
	}
	else
	{
		ctx->cur_sequence++;
	}

	rc = ngx_http_vod_run_state_machine(ctx);
	if (rc == NGX_AGAIN)
	{
		return;
	}

	if (rc != NGX_OK && rc != NGX_DONE)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_drm_info_request_finished: ngx_http_vod_run_state_machine failed %i", rc);
	}

finalize_request:

	ngx_http_vod_finalize_request(ctx, rc);
}

static ngx_int_t
ngx_http_vod_state_machine_get_drm_info(ngx_http_vod_ctx_t *ctx)
{
	ngx_child_request_params_t child_params;
	ngx_http_vod_loc_conf_t* conf = ctx->submodule_context.conf;
	ngx_http_request_t* r = ctx->submodule_context.r;
	ngx_int_t rc;
	ngx_str_t drm_info;
	ngx_str_t base_uri;
	ngx_md5_t md5;

	for (;
		ctx->cur_sequence < ctx->submodule_context.media_set.sequences_end;
		ctx->cur_sequence++)
	{
		// get the request uri
		if (conf->drm_request_uri != NULL)
		{
			if (ngx_http_complex_value(
				r,
				conf->drm_request_uri,
				&base_uri) != NGX_OK)
			{
				ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
					"ngx_http_vod_state_machine_get_drm_info: ngx_http_complex_value failed");
				return NGX_ERROR;
			}
		}
		else
		{
			base_uri = ctx->cur_sequence->stripped_uri;
		}

		if (conf->drm_info_cache != NULL)
		{
			// generate a request key
			ngx_md5_init(&md5);
			ngx_md5_update(&md5, conf->drm_upstream_location.data, conf->drm_upstream_location.len);
			ngx_md5_update(&md5, base_uri.data, base_uri.len);
			ngx_md5_final(ctx->child_request_key, &md5);

			// try to read the drm info from cache
			if (ngx_buffer_cache_fetch_perf(
				ctx->perf_counters, 
				conf->drm_info_cache, 
				ctx->child_request_key,
				&drm_info))
			{
				ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
					"ngx_http_vod_state_machine_get_drm_info: drm info cache hit, size is %uz", drm_info.len);

				rc = conf->submodule.parse_drm_info(&ctx->submodule_context, &drm_info, &ctx->cur_sequence->drm_info);
				if (rc != NGX_OK)
				{
					ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
						"ngx_http_vod_state_machine_get_drm_info: invalid drm info in cache %V", &drm_info);
					return rc;
				}

				if (conf->drm_single_key)
				{
					ngx_http_vod_copy_drm_info(ctx);
					ctx->cur_sequence = ctx->submodule_context.media_set.sequences_end - 1;
				}

				continue;
			}
			else
			{
				ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
					"ngx_http_vod_state_machine_get_drm_info: drm info cache miss");
			}
		}

		r->connection->log->action = "getting drm info";

		rc = ngx_http_vod_alloc_read_buffer(ctx, conf->drm_max_info_length, READER_HTTP);
		if (rc != NGX_OK)
		{
			return rc;
		}

		// start the drm request
		ngx_memzero(&child_params, sizeof(child_params));
		child_params.method = NGX_HTTP_GET;
		child_params.base_uri = base_uri;

		ngx_perf_counter_start(ctx->perf_counter_context);

		rc = ngx_child_request_start(
			r,
			ngx_http_vod_drm_info_request_finished,
			r,
			&conf->drm_upstream_location,
			&child_params,
			&ctx->read_buffer);
		if (rc != NGX_AGAIN)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_state_machine_get_drm_info: ngx_child_request_start failed %i", rc);
		}
		return rc;
	}

	return NGX_OK;
}

////// Common media processing

static void
ngx_http_vod_update_source_tracks(
	request_context_t* request_context,
	media_clip_source_t* cur_source)
{
	media_track_t* cur_track;
	file_info_t file_info;
	int64_t original_clip_time;

	file_info.source = cur_source;
	file_info.uri = cur_source->uri;
	file_info.drm_info = cur_source->sequence->drm_info;

	original_clip_time = cur_source->range != NULL ?
		cur_source->range->original_clip_time : cur_source->clip_time;

	for (cur_track = cur_source->track_array.first_track;
		cur_track < cur_source->track_array.last_track;
		cur_track++)
	{
		cur_track->clip_start_time = cur_source->clip_time;
		cur_track->original_clip_time = original_clip_time;
		cur_track->file_info = file_info;

		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"ngx_http_vod_update_source_tracks: first frame dts is %uL",
			rescale_time(cur_track->first_frame_time_offset, cur_track->media_info.timescale, 1000) +
			cur_track->clip_start_time);
	}
}

static ngx_int_t 
ngx_http_vod_parse_metadata(
	ngx_http_vod_ctx_t *ctx, 
	ngx_flag_t fetched_from_cache)
{
	get_clip_ranges_params_t get_ranges_params;
	media_parse_params_t parse_params;
	const ngx_http_vod_request_t* request = ctx->request;
	media_clip_source_t* cur_source = ctx->cur_source;
	request_context_t* request_context = &ctx->submodule_context.request_context;
	segmenter_conf_t* segmenter = &ctx->submodule_context.conf->segmenter;
	get_clip_ranges_result_t clip_ranges;
	uint64_t last_segment_end;
	media_range_t range;
	vod_status_t rc;
	uint32_t* request_tracks_mask;
	uint32_t tracks_mask[MEDIA_TYPE_COUNT];
	uint32_t duration_millis;
	uint32_t media_type;
	vod_fraction_t rate;

	// initialize clipping params
	if (cur_source->clip_to == ULLONG_MAX)
	{
		if (cur_source->clip_from >= UINT_MAX)
		{
			ngx_log_error(NGX_LOG_ERR, request_context->log, 0,
				"ngx_http_vod_parse_metadata: clip from value %uL too large", cur_source->clip_from);
			return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, VOD_BAD_REQUEST);
		}

		parse_params.clip_to = UINT_MAX;
	}
	else
	{
		if (cur_source->clip_to >= UINT_MAX)
		{
			ngx_log_error(NGX_LOG_ERR, request_context->log, 0,
				"ngx_http_vod_parse_metadata: clip to value %uL too large", cur_source->clip_to);
			return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, VOD_BAD_REQUEST);
		}

		parse_params.clip_to = cur_source->clip_to;
	}
	parse_params.clip_from = cur_source->clip_from;

	if (request == NULL)
	{
		// Note: the other fields in parse_params are not required here

		if (ctx->format->clipper_parse == NULL)
		{
			ngx_log_error(NGX_LOG_ERR, request_context->log, 0,
				"ngx_http_vod_parse_metadata: clipping not supported for %V", &ctx->format->name);
			return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, VOD_BAD_REQUEST);
		}

		rc = ctx->format->clipper_parse(
			request_context,
			&parse_params,
			ctx->metadata_parts,
			ctx->metadata_part_count,
			fetched_from_cache,
			&ctx->clipper_parse_result);
		if (rc != VOD_OK)
		{
			ngx_log_debug2(NGX_LOG_DEBUG_HTTP, request_context->log, 0,
				"ngx_http_vod_parse_metadata: clipper_parse(%V) failed %i", &ctx->format->name, rc);
			return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, rc);
		}

		return NGX_OK;
	}

	ngx_perf_counter_start(ctx->perf_counter_context);

	// init the parsing params
	parse_params.parse_type = request->parse_type;
	if (request->request_class == REQUEST_CLASS_MANIFEST && 
		ctx->submodule_context.media_set.timing.durations == NULL)
	{
		parse_params.parse_type |= segmenter->parse_type;
	}

	if (!ctx->submodule_context.conf->ignore_edit_list)
	{
		parse_params.parse_type |= PARSE_FLAG_EDIT_LIST;
	}
	parse_params.codecs_mask = request->codecs_mask;

	if (ctx->submodule_context.request_params.sequence_tracks_mask != NULL)
	{
		request_tracks_mask = ctx->submodule_context.request_params.sequence_tracks_mask + 
			cur_source->sequence->index * MEDIA_TYPE_COUNT;
	}
	else
	{
		request_tracks_mask = ctx->submodule_context.request_params.tracks_mask;
	}

	for (media_type = 0; media_type < MEDIA_TYPE_COUNT; media_type++)
	{
		tracks_mask[media_type] = cur_source->tracks_mask[media_type] & request_tracks_mask[media_type];
	}
	parse_params.required_tracks_mask = tracks_mask;
	parse_params.langs_mask = ctx->submodule_context.request_params.langs_mask;
	parse_params.source = cur_source;
	
	// parse the basic metadata
	rc = ctx->format->parse_metadata(
		request_context,
		&parse_params,
		ctx->metadata_parts,
		ctx->metadata_part_count,
		&ctx->base_metadata);
	if (rc != VOD_OK)
	{
		ngx_log_debug2(NGX_LOG_DEBUG_HTTP, request_context->log, 0,
			"ngx_http_vod_parse_metadata: parse_metadata(%V) failed %i", &ctx->format->name, rc);
		return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, rc);
	}

	if (ctx->base_metadata->tracks.nelts == 0)
	{
		ngx_memzero(&cur_source->track_array, sizeof(cur_source->track_array));
		return VOD_OK;
	}

	if ((request->request_class & (REQUEST_CLASS_MANIFEST | REQUEST_CLASS_OTHER)) != 0)
	{
		request_context->simulation_only = TRUE;

		parse_params.max_frame_count = 1024 * 1024;
		range.timescale = 1000;
		range.original_clip_time = 0;
		range.start = 0;
		if (cur_source->clip_to == ULLONG_MAX)
		{
			range.end = ULLONG_MAX;
		}
		else
		{
			range.end = cur_source->clip_to - cur_source->clip_from;
		}
		parse_params.range = &range;
	}
	else
	{
		request_context->simulation_only = FALSE;

		parse_params.max_frame_count = 64 * 1024;

		if (cur_source->range != NULL)
		{
			// the range was already determined while parsing the media set
			parse_params.range = cur_source->range;
		}
		else
		{
			// get the rate
			if (cur_source->base.parent != NULL && cur_source->base.parent->type == MEDIA_CLIP_RATE_FILTER)
			{
				rate = ((media_clip_rate_filter_t*)cur_source->base.parent)->rate;
			}
			else
			{
				rate.num = 1;
				rate.denom = 1;
			}

			// get the last segment end
			if (cur_source->clip_to == ULLONG_MAX)
			{
				last_segment_end = ULLONG_MAX;
			}
			else
			{
				last_segment_end = ((cur_source->clip_to - cur_source->clip_from) * rate.denom) / rate.num;
			}

			// get the start/end offsets
			duration_millis = rescale_time(ctx->base_metadata->duration * rate.denom, ctx->base_metadata->timescale * rate.num, 1000);

			get_ranges_params.request_context = request_context;
			get_ranges_params.conf = segmenter;
			get_ranges_params.last_segment_end = last_segment_end;
			get_ranges_params.key_frame_durations = NULL;
			get_ranges_params.allow_last_segment = TRUE;

			ngx_memzero(&get_ranges_params.timing, sizeof(get_ranges_params.timing));
			get_ranges_params.timing.durations = &duration_millis;
			get_ranges_params.timing.total_count = 1;
			get_ranges_params.timing.total_duration = duration_millis;
			get_ranges_params.timing.times = &get_ranges_params.timing.first_time;
			get_ranges_params.timing.original_times = &get_ranges_params.timing.first_time;

			if (ctx->submodule_context.request_params.segment_index != INVALID_SEGMENT_INDEX)
			{
				// segment request
				get_ranges_params.segment_index = ctx->submodule_context.request_params.segment_index;

				rc = segmenter_get_start_end_ranges_no_discontinuity(
					&get_ranges_params,
					&clip_ranges);
			}
			else
			{
				// thumbnail request
				get_ranges_params.time = ctx->submodule_context.request_params.segment_time;

				rc = segmenter_get_start_end_ranges_gop(
					&get_ranges_params,
					&clip_ranges);
			}

			if (rc != VOD_OK)
			{
				ngx_log_debug1(NGX_LOG_DEBUG_HTTP, request_context->log, 0,
					"ngx_http_vod_parse_metadata: segmenter_get_start_end_ranges_no_discontinuity failed %i", rc);
				return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, rc);
			}

			if (clip_ranges.clip_count == 0)
			{
				ngx_memzero(&cur_source->track_array, sizeof(cur_source->track_array));
				return VOD_OK;
			}

			if (clip_ranges.clip_ranges->end == ULLONG_MAX)
			{
				ctx->submodule_context.media_set.segment_duration =
					duration_millis - clip_ranges.clip_ranges->start;
			}
			else
			{
				ctx->submodule_context.media_set.segment_duration =
					clip_ranges.clip_ranges->end - clip_ranges.clip_ranges->start;
			}

			parse_params.range = clip_ranges.clip_ranges;
			parse_params.range->start = (parse_params.range->start * rate.num) / rate.denom;
			if (parse_params.range->end != ULLONG_MAX)
			{
				parse_params.range->end = (parse_params.range->end * rate.num) / rate.denom;
			}
		}
	}

	parse_params.max_frames_size = ctx->submodule_context.conf->max_frames_size;

	// parse the frames
	rc = ctx->format->read_frames(
		request_context,
		ctx->base_metadata,
		&parse_params,
		segmenter,
		&ctx->read_cache_state,
		NULL,
		&ctx->frames_read_req,
		&cur_source->track_array);
	switch (rc)
	{
	case VOD_OK:
		break;	// handled outside the switch

	case VOD_AGAIN:
		return NGX_AGAIN;

	default:
		ngx_log_debug2(NGX_LOG_DEBUG_HTTP, request_context->log, 0,
			"ngx_http_vod_parse_metadata: read_frames(%V) failed %i", &ctx->format->name, rc);
		return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, rc);
	}

	ngx_http_vod_update_source_tracks(request_context, cur_source);

	ngx_perf_counter_end(ctx->perf_counters, ctx->perf_counter_context, PC_MEDIA_PARSE);

	return rc;
}

static ngx_int_t
ngx_http_vod_identify_format(ngx_http_vod_ctx_t* ctx)
{
	media_format_t** cur_format_ptr;
	media_format_t* cur_format;
	vod_status_t rc;
	vod_str_t buffer;

	buffer.data = ctx->read_buffer.pos;
	buffer.len = ctx->read_buffer.last - ctx->read_buffer.pos;

	for (cur_format_ptr = media_formats; ; cur_format_ptr++)
	{
		cur_format = *cur_format_ptr;
		if (cur_format == NULL)
		{
			if (buffer.len > sizeof(wvm_file_magic) &&
				ngx_memcmp(buffer.data, wvm_file_magic, sizeof(wvm_file_magic)) == 0)
			{
				ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
					"ngx_http_vod_identify_format: wvm format is not supported");
			}
			else
			{
				ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
					"ngx_http_vod_identify_format: failed to identify the file format");
			}
			return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, VOD_BAD_DATA);
		}

		rc = cur_format->init_metadata_reader(
			&ctx->submodule_context.request_context,
			&buffer,
			ctx->submodule_context.conf->initial_read_size,
			ctx->submodule_context.conf->max_metadata_size,
			&ctx->metadata_reader_context);
		if (rc == VOD_NOT_FOUND)
		{
			continue;
		}

		if (rc != VOD_OK)
		{
			ngx_log_debug2(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_identify_format: init_metadata_reader(%V) failed %i", &cur_format->name, rc);
			return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, rc);
		}

		ctx->format = cur_format;
		break;
	}

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_init_format(ngx_http_vod_ctx_t* ctx, uint32_t format_id)
{
	media_format_t** cur_format_ptr;
	media_format_t* cur_format;

	for (cur_format_ptr = media_formats; ; cur_format_ptr++)
	{
		cur_format = *cur_format_ptr;
		if (cur_format == NULL)
		{
			ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_init_format: format id %uD not found", format_id);
			return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, VOD_UNEXPECTED);
		}

		if (cur_format->id != format_id)
		{
			continue;
		}

		ctx->format = cur_format;
		break;
	}

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_async_read(ngx_http_vod_ctx_t* ctx, media_format_read_request_t* read_req)
{
	size_t prefix_size;
	size_t buffer_size;
	size_t read_size;
	off_t read_offset;
	ngx_int_t rc;

	// align the read size and offset
	read_offset = read_req->read_offset & (~(ctx->alignment - 1));
	if (read_req->read_size == 0)
	{
		read_size = ctx->submodule_context.conf->initial_read_size;
	}
	else
	{
		read_size = read_req->read_size + read_req->read_offset - read_offset;
	}

	read_size = (read_size + ctx->alignment - 1) & (~(ctx->alignment - 1));

	// optimization for the case in which the current range is a prefix of the new range
	buffer_size = ctx->read_buffer.last - ctx->read_buffer.pos;
	prefix_size = 0;

	if (read_offset >= ctx->read_offset && 
		read_offset < (off_t)(ctx->read_offset + buffer_size) &&
		ctx->read_buffer.start != NULL)
	{
		prefix_size = ctx->read_offset + buffer_size - read_offset;
		ctx->prefix_buffer = ctx->read_buffer;
		ctx->prefix_buffer.pos = ctx->prefix_buffer.last - prefix_size;
		ctx->read_buffer.start = NULL;
	}
	else if ((read_req->flags & MEDIA_READ_FLAG_REALLOC_BUFFER) != 0)
	{
		ctx->read_buffer.start = NULL;
	}

	// allocate the read buffer
	rc = ngx_http_vod_alloc_read_buffer(ctx, read_size, ctx->alloc_params_index);
	if (rc != NGX_OK)
	{
		return rc;
	}

	if (ctx->prefix_buffer.start != NULL)
	{
		ctx->read_buffer.start += prefix_size;
		ctx->read_buffer.pos = ctx->read_buffer.start;
		ctx->read_buffer.last = ctx->read_buffer.start;
	}

	// perform the read
	ctx->read_offset = read_offset;
	ctx->requested_offset = read_req->read_offset;
	ctx->read_flags = read_req->flags;

	ngx_perf_counter_start(ctx->perf_counter_context);

	rc = ctx->read(
		ctx->cur_source->reader_context,
		&ctx->read_buffer,
		read_size - prefix_size,
		read_offset + prefix_size);
	if (rc != NGX_OK)
	{
		if (rc != NGX_AGAIN)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_async_read: async_read failed %i", rc);
		}

		return rc;
	}

	ngx_perf_counter_end(ctx->perf_counters, ctx->perf_counter_context, PC_READ_FILE);

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_get_async_read_result(ngx_http_vod_ctx_t* ctx, vod_str_t* read_buffer)
{
	size_t prefix_size;
	size_t buffer_size;
	off_t buffer_offset;

	if (ctx->prefix_buffer.start != NULL)
	{
		// prepend the prefix buffer
		prefix_size = ctx->prefix_buffer.last - ctx->prefix_buffer.pos;
		ctx->read_buffer.start -= prefix_size;

		ctx->read_buffer.pos -= prefix_size;
		ngx_memcpy(ctx->read_buffer.pos, ctx->prefix_buffer.pos, prefix_size);

		if ((ctx->read_flags & MEDIA_READ_FLAG_REALLOC_BUFFER) == 0)	// should not free the buffer if there are references to it
		{
			ngx_pfree(ctx->submodule_context.r->pool, ctx->prefix_buffer.start);
		}
		ctx->prefix_buffer.start = NULL;
	}

	// adjust the buffer pointer following the alignment
	buffer_offset = ctx->requested_offset - ctx->read_offset;
	buffer_size = ctx->read_buffer.last - ctx->read_buffer.pos;

	if (buffer_size < (size_t)buffer_offset)
	{
		ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
			"ngx_http_vod_get_async_read_result: buffer size %uz is smaller than buffer offset %O", 
			buffer_size, buffer_offset);
		return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, VOD_BAD_DATA);
	}

	// null terminate the buffer
	if (ctx->read_buffer.last >= ctx->read_buffer.end)
	{
		ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
			"ngx_http_vod_get_async_read_result: not enough room for null terminator");
		return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, VOD_UNEXPECTED);
	}

	*ctx->read_buffer.last = '\0';

	read_buffer->data = ctx->read_buffer.pos + buffer_offset;
	read_buffer->len = buffer_size - buffer_offset;

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_read_metadata(ngx_http_vod_ctx_t* ctx)
{
	media_format_read_metadata_result_t result;
	vod_str_t read_buffer;
	ngx_int_t rc;

	if (ctx->metadata_reader_context == NULL)
	{
		// identify the format
		rc = ngx_http_vod_identify_format(ctx);
		if (rc != NGX_OK)
		{
			return rc;
		}
	}

	for (;;)
	{
		rc = ngx_http_vod_get_async_read_result(ctx, &read_buffer);
		if (rc != NGX_OK)
		{
			return rc;
		}

		// run the read state machine
		rc = ctx->format->read_metadata(
			ctx->metadata_reader_context,
			ctx->requested_offset,
			&read_buffer,
			&result);
		if (rc == VOD_OK)
		{
			ctx->metadata_parts = result.parts;
			ctx->metadata_part_count = result.part_count;
			break;
		}

		if (rc != VOD_AGAIN)
		{
			ngx_log_debug2(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_read_metadata: read_metadata(%V) failed %i", &ctx->format->name, rc);
			return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, rc);
		}

		// issue another read request
		rc = ngx_http_vod_async_read(ctx, &result.read_req);
		if (rc != NGX_OK)
		{
			return rc;
		}
	}

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_read_frames(ngx_http_vod_ctx_t *ctx)
{
	media_format_read_request_t read_req;
	request_context_t* request_context = &ctx->submodule_context.request_context;
	vod_str_t read_buffer;
	ngx_int_t rc;

	for (;;)
	{
		rc = ngx_http_vod_get_async_read_result(ctx, &read_buffer);
		if (rc != NGX_OK)
		{
			return rc;
		}

		// run the read state machine
		rc = ctx->format->read_frames(
			request_context,
			ctx->base_metadata,
			NULL,
			&ctx->submodule_context.conf->segmenter,
			&ctx->read_cache_state,
			&read_buffer,
			&read_req,
			&ctx->cur_source->track_array);
		if (rc == VOD_OK)
		{
			break;
		}

		if (rc != VOD_AGAIN)
		{
			ngx_log_debug2(NGX_LOG_DEBUG_HTTP, request_context->log, 0,
				"ngx_http_vod_read_frames: read_frames(%V) failed %i", &ctx->format->name, rc);
			return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, rc);
		}

		// issue another read request
		rc = ngx_http_vod_async_read(ctx, &read_req);
		if (rc != NGX_OK)
		{
			return rc;
		}
	}

	ngx_http_vod_update_source_tracks(request_context, ctx->cur_source);

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_state_machine_parse_metadata(ngx_http_vod_ctx_t *ctx)
{
	ngx_http_vod_loc_conf_t* conf = ctx->submodule_context.conf;
	multipart_cache_header_t multipart_header;
	media_clip_source_t* cur_source;
	ngx_http_request_t* r = ctx->submodule_context.r;
	ngx_int_t rc;

	if (ctx->cur_source == NULL)
	{
		return NGX_OK;
	}

	for (;;)
	{
		switch (ctx->state)
		{
		case STATE_READ_METADATA_INITIAL:
			cur_source = ctx->cur_source;

			if (conf->metadata_cache != NULL)
			{
				// try to read the metadata from cache
				if (ngx_buffer_cache_fetch_multipart_perf(
					ctx,
					conf->metadata_cache,
					cur_source->file_key,
					&multipart_header,
					&ctx->metadata_parts))
				{
					ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
						"ngx_http_vod_state_machine_parse_metadata: metadata cache hit");

					rc = ngx_http_vod_init_format(ctx, multipart_header.type);
					if (rc != NGX_OK)
					{
						return rc;
					}

					rc = ngx_http_vod_parse_metadata(ctx, 1);
					if (rc == NGX_OK)
					{
						ctx->cur_source = cur_source->next;
						if (ctx->cur_source == NULL)
						{
							return NGX_OK;
						}
						break;
					}

					if (rc != NGX_AGAIN)
					{
						ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
							"ngx_http_vod_state_machine_parse_metadata: ngx_http_vod_parse_metadata failed %i", rc);
						return rc;
					}

					ctx->state = STATE_READ_FRAMES_OPEN_FILE;
				}
				else
				{
					ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
						"ngx_http_vod_state_machine_parse_metadata: metadata cache miss");
					ctx->state = STATE_READ_METADATA_OPEN_FILE;
				}
			}
			else
			{
				ctx->state = STATE_READ_METADATA_OPEN_FILE;
			}

			// open the file
			rc = ctx->reader->open(r, &cur_source->mapped_uri, 0, &cur_source->reader_context);
			if (rc != NGX_OK)
			{
				if (rc != NGX_AGAIN && rc != NGX_DONE)
				{
					ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
						"ngx_http_vod_state_machine_parse_metadata: open_file failed %i", rc);
				}
				return rc;
			}
			break;

		case STATE_READ_METADATA_OPEN_FILE:
			// allocate the initial read buffer
			rc = ngx_http_vod_alloc_read_buffer(ctx, conf->initial_read_size, ctx->alloc_params_index);
			if (rc != NGX_OK)
			{
				return rc;
			}

			// read the file header
			r->connection->log->action = "reading media header";
			ctx->state = STATE_READ_METADATA_READ;
			ctx->metadata_reader_context = NULL;

			ctx->read_offset = 0;
			ctx->requested_offset = 0;
			ctx->read_flags = MEDIA_READ_FLAG_ALLOW_EMPTY_READ;

			cur_source = ctx->cur_source;

			ngx_perf_counter_start(ctx->perf_counter_context);

			rc = ctx->read(cur_source->reader_context, &ctx->read_buffer, conf->initial_read_size, 0);
			if (rc != NGX_OK)
			{
				if (rc != NGX_AGAIN)
				{
					ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
						"ngx_http_vod_state_machine_parse_metadata: async_read failed %i", rc);
				}
				return rc;
			}

			// read completed synchronously
			ngx_perf_counter_end(ctx->perf_counters, ctx->perf_counter_context, PC_READ_FILE);
			// fallthrough

		case STATE_READ_METADATA_READ:
			// read the metadata
			rc = ngx_http_vod_read_metadata(ctx);
			if (rc != NGX_OK)
			{
				if (rc != NGX_AGAIN)
				{
					ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
						"ngx_http_vod_state_machine_parse_metadata: ngx_http_vod_read_metadata failed %i", rc);
				}
				return rc;
			}

			// parse the metadata
			rc = ngx_http_vod_parse_metadata(ctx, 0);
			if (rc != NGX_OK && rc != NGX_AGAIN)
			{
				ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
					"ngx_http_vod_state_machine_parse_metadata: ngx_http_vod_parse_metadata failed %i", rc);
				return rc;
			}

			// save the metadata to cache
			cur_source = ctx->cur_source;

			if (conf->metadata_cache != NULL)
			{
				multipart_header.type = ctx->format->id;
				multipart_header.part_count = ctx->metadata_part_count;

				if (ngx_buffer_cache_store_multipart_perf(
					ctx,
					conf->metadata_cache,
					cur_source->file_key,
					&multipart_header,
					ctx->metadata_parts))
				{
					ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
						"ngx_http_vod_state_machine_parse_metadata: stored metadata in cache");
				}
				else
				{
					ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
						"ngx_http_vod_state_machine_parse_metadata: failed to store metadata in cache");
				}
			}

			if (ctx->request != NULL)
			{
				// no longer need the metadata buffer
				ngx_pfree(ctx->submodule_context.r->pool, ctx->read_buffer.start);
				ctx->read_buffer.start = NULL;
			}

			if (rc == NGX_OK)
			{
				// move to the next source
				ctx->state = STATE_READ_METADATA_INITIAL;

				ctx->cur_source = cur_source->next;
				if (ctx->cur_source == NULL)
				{
					return NGX_OK;
				}
				break;
			}
			// fallthrough

		case STATE_READ_FRAMES_OPEN_FILE:
			ctx->state = STATE_READ_FRAMES_READ;
			ctx->read_buffer.start = NULL;			// don't reuse buffers from the metadata phase

			rc = ngx_http_vod_async_read(ctx, &ctx->frames_read_req);
			if (rc != NGX_OK)
			{
				return rc;
			}
			// fallthrough

		case STATE_READ_FRAMES_READ:
			rc = ngx_http_vod_read_frames(ctx);
			if (rc != NGX_OK)
			{
				return rc;
			}

			// move to the next source
			ctx->state = STATE_READ_METADATA_INITIAL;

			ctx->cur_source = ctx->cur_source->next;
			if (ctx->cur_source == NULL)
			{
				return NGX_OK;
			}
			break;

		default:
			ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_state_machine_parse_metadata: invalid state %d", ctx->state);
			return ngx_http_vod_status_to_ngx_error(r, VOD_UNEXPECTED);
		}
	}

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_validate_streams(ngx_http_vod_ctx_t *ctx)
{
	if (ctx->submodule_context.media_set.total_track_count == 0)
	{
		if (ctx->request->request_class == REQUEST_CLASS_SEGMENT)
		{
			ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_validate_streams: no matching streams were found, probably invalid segment index");
			return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, VOD_NO_STREAMS);
		}
		else
		{
			ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_validate_streams: no matching streams were found");
			return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, VOD_BAD_REQUEST);
		}
	}
	
	if ((ctx->request->flags & REQUEST_FLAG_SINGLE_TRACK) != 0)
	{
		if (ctx->submodule_context.media_set.sequence_count != 1)
		{
			ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_validate_streams: request has more than one sequence while only one is supported");
			return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, VOD_BAD_REQUEST);
		}

		if (ctx->submodule_context.media_set.total_track_count != 1)
		{
			ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_validate_streams: got %uD streams while only a single stream is supported",
				ctx->submodule_context.media_set.total_track_count);
			return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, VOD_BAD_REQUEST);
		}
	}
	else if ((ctx->request->flags & REQUEST_FLAG_SINGLE_TRACK_PER_MEDIA_TYPE) != 0)
	{
		if (ctx->submodule_context.media_set.sequence_count != 1 && ctx->submodule_context.media_set.sequence_count != 2)
		{
			ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_validate_streams: invalid sequence count %uD", ctx->submodule_context.media_set.sequence_count);
			return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, VOD_BAD_REQUEST);
		}

		if (ctx->submodule_context.media_set.track_count[MEDIA_TYPE_VIDEO] > 1 ||
			ctx->submodule_context.media_set.track_count[MEDIA_TYPE_AUDIO] > 1)
		{
			ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_validate_streams: one stream at most per media type is allowed video=%uD audio=%uD",
				ctx->submodule_context.media_set.track_count[MEDIA_TYPE_VIDEO],
				ctx->submodule_context.media_set.track_count[MEDIA_TYPE_AUDIO]);
			return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, VOD_BAD_REQUEST);
		}

		if (ctx->submodule_context.media_set.track_count[MEDIA_TYPE_SUBTITLE] > 0 &&
			ctx->submodule_context.media_set.total_track_count != 1)
		{
			ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_validate_streams: cannot have more than a single subtitle track and it cannot be mixed with other track types");
			return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, VOD_BAD_REQUEST);
		}
	}
	
	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_update_track_timescale(
	ngx_http_vod_ctx_t *ctx, 
	media_track_t* track, 
	uint32_t new_timescale, 
	uint32_t pts_delay)
{
	frame_list_part_t* part;
	input_frame_t* last_frame;
	input_frame_t* cur_frame;
	uint64_t next_scaled_dts;
	uint64_t last_frame_dts;
	uint64_t clip_start_dts;
	uint64_t clip_end_pts;
	uint64_t clip_end_dts;
	uint64_t scaled_dts;
	uint64_t scaled_pts;
	uint64_t dts;
	uint64_t pts;
	uint32_t cur_timescale = track->media_info.timescale;

	// frames
	dts = track->first_frame_time_offset;
	scaled_dts = rescale_time(dts, cur_timescale, new_timescale);
	clip_start_dts = scaled_dts;

	track->first_frame_time_offset = scaled_dts;
	track->total_frames_duration = 0;

	// initialize the first part
	part = &track->frames;
	cur_frame = part->first_frame;
	last_frame = part->last_frame;
	if (part->clip_to != UINT_MAX && cur_frame < last_frame)
	{
		clip_end_dts = rescale_time(part->clip_to, 1000, new_timescale);
		if (track->media_info.media_type == MEDIA_TYPE_VIDEO)
		{
			clip_end_pts = clip_end_dts + rescale_time(track->media_info.u.video.initial_pts_delay,
				cur_timescale, new_timescale);
		}
		else
		{
			clip_end_pts = ULLONG_MAX;
		}
	}
	else
	{
		clip_end_dts = ULLONG_MAX;
		clip_end_pts = ULLONG_MAX;
	}

	for (;; cur_frame++)
	{
		if (cur_frame >= last_frame)
		{
			if (clip_end_dts != ULLONG_MAX)
			{
				clip_end_dts = rescale_time(part->clip_to, 1000, new_timescale);
				last_frame_dts = scaled_dts - cur_frame[-1].duration;

				if (clip_end_dts > last_frame_dts)
				{
					cur_frame[-1].duration = clip_end_dts - last_frame_dts;
					scaled_dts = clip_end_dts;
				}
				else
				{
					ngx_log_error(NGX_LOG_WARN, ctx->submodule_context.request_context.log, 0,
						"ngx_http_vod_update_track_timescale: last frame dts %uL greater than clip end dts %uL",
						last_frame_dts, clip_end_dts);
				}

				track->total_frames_duration += scaled_dts - clip_start_dts;

				dts = 0;
				scaled_dts = 0;
				clip_start_dts = 0;
			}

			if (part->next == NULL)
			{
				break;
			}

			// initialize the next part
			part = part->next;
			cur_frame = part->first_frame;
			last_frame = part->last_frame;
			if (part->clip_to != UINT_MAX && cur_frame < last_frame)
			{
				clip_end_dts = rescale_time(part->clip_to, 1000, new_timescale);
				if (track->media_info.media_type == MEDIA_TYPE_VIDEO)
				{
					clip_end_pts = clip_end_dts + rescale_time(track->media_info.u.video.initial_pts_delay,
						cur_timescale, new_timescale);
				}
			}
			else
			{
				clip_end_dts = ULLONG_MAX;
				clip_end_pts = ULLONG_MAX;
			}
		}

		// get the pts delay
		pts = dts + cur_frame->pts_delay;
		scaled_pts = rescale_time(pts, cur_timescale, new_timescale);
		if (scaled_pts > clip_end_pts)
		{
			scaled_pts = ngx_max(clip_end_pts, scaled_dts);
		}
		cur_frame->pts_delay = scaled_pts - scaled_dts + pts_delay;

		// get the duration
		dts += cur_frame->duration;
		next_scaled_dts = rescale_time(dts, cur_timescale, new_timescale);
		cur_frame->duration = next_scaled_dts - scaled_dts;
		scaled_dts = next_scaled_dts;
	}

	track->total_frames_duration += scaled_dts - clip_start_dts;
	track->clip_from_frame_offset = rescale_time(track->clip_from_frame_offset, cur_timescale, new_timescale);

	// media info
	track->media_info.duration = rescale_time(track->media_info.duration, cur_timescale, new_timescale);
	track->media_info.full_duration = rescale_time(track->media_info.full_duration, cur_timescale, new_timescale);
	if (track->media_info.full_duration == 0)
	{
		ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
			"ngx_http_vod_update_track_timescale: full duration is zero following rescale");
		return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, VOD_BAD_DATA);
	}

	if (track->media_info.media_type == MEDIA_TYPE_VIDEO)
	{
		if (track->media_info.min_frame_duration != 0)
		{
			track->media_info.min_frame_duration =
				rescale_time(track->media_info.min_frame_duration, cur_timescale, new_timescale);
			if (track->media_info.min_frame_duration == 0)
			{
				ngx_log_error(NGX_LOG_WARN, ctx->submodule_context.request_context.log, 0,
					"ngx_http_vod_update_track_timescale: min frame duration is zero following rescale");
				track->media_info.min_frame_duration = 1;
			}
		}

		track->media_info.u.video.initial_pts_delay =
			rescale_time(track->media_info.u.video.initial_pts_delay, cur_timescale, new_timescale);
	}

	track->media_info.timescale = new_timescale;
	track->media_info.frames_timescale = new_timescale;

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_update_timescale(ngx_http_vod_ctx_t *ctx)
{
	media_set_t* media_set = &ctx->submodule_context.media_set;
	media_track_t* track;
	ngx_int_t rc;

	for (track = media_set->filtered_tracks; track < media_set->filtered_tracks_end; track++)
	{
		rc = ngx_http_vod_update_track_timescale(
			ctx, 
			track, 
			ctx->request->timescale, 
			ctx->submodule_context.request_params.pts_delay);
		if (rc != NGX_OK)
		{
			return rc;
		}
	}

	return NGX_OK;
}

////// Metadata request handling

static ngx_int_t
ngx_http_vod_handle_metadata_request(ngx_http_vod_ctx_t *ctx)
{
	ngx_http_vod_loc_conf_t* conf;
	response_cache_header_t cache_header;
	ngx_buffer_cache_t* cache;
	ngx_str_t cache_buffers[3];
	ngx_str_t content_type;
	ngx_str_t response = ngx_null_string;
	ngx_int_t rc;
	int cache_type;

	rc = ngx_http_vod_update_timescale(ctx);
	if (rc != NGX_OK)
	{
		return rc;
	}

	ngx_perf_counter_start(ctx->perf_counter_context);

	rc = ctx->request->handle_metadata_request(
		&ctx->submodule_context,
		&response,
		&content_type);
	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
			"ngx_http_vod_handle_metadata_request: handle_metadata_request failed %i", rc);
		return rc;
	}

	ngx_perf_counter_end(ctx->perf_counters, ctx->perf_counter_context, PC_BUILD_MANIFEST);

	conf = ctx->submodule_context.conf;
	if (ctx->submodule_context.media_set.type != MEDIA_SET_LIVE ||
		(ctx->request->flags & REQUEST_FLAG_TIME_DEPENDENT_ON_LIVE) == 0)
	{
		cache_type = CACHE_TYPE_VOD;
	}
	else
	{
		cache_type = CACHE_TYPE_LIVE;
	}

	cache = conf->response_cache[cache_type];
	if (cache != NULL && response.data != NULL)
	{
		cache_header.content_type_len = content_type.len;
		cache_header.media_set_type = ctx->submodule_context.media_set.type;
		cache_buffers[0].data = (u_char*)&cache_header;
		cache_buffers[0].len = sizeof(cache_header);
		cache_buffers[1] = content_type;
		cache_buffers[2] = response;

		if (ngx_buffer_cache_store_gather_perf(ctx->perf_counters, cache, ctx->request_key, cache_buffers, 3))
		{
			ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_handle_metadata_request: stored in response cache");
		}
		else
		{
			ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_handle_metadata_request: failed to store response in cache");
		}
	}

	rc = ngx_http_vod_send_header(
		ctx->submodule_context.r, 
		response.len, 
		&content_type, 
		ctx->submodule_context.media_set.type, 
		ctx->request);
	if (rc != NGX_OK)
	{
		return rc;
	}
	
	return ngx_http_vod_send_response(ctx->submodule_context.r, &response, NULL);
}

////// Segment request handling

static ngx_int_t
ngx_http_vod_state_machine_open_files(ngx_http_vod_ctx_t *ctx)
{
	media_clip_source_t* cur_source;
	ngx_str_t* path;
	ngx_int_t rc;

	for (cur_source = ctx->cur_source;
		cur_source != NULL;
		cur_source = cur_source->next)
	{
		// open the file if not already opened
		if (cur_source->reader_context != NULL)
		{
			continue;
		}

		path = &cur_source->mapped_uri;

		rc = ctx->reader->open(ctx->submodule_context.r, path, 0, &cur_source->reader_context);
		if (rc != NGX_OK)
		{
			if (rc != NGX_AGAIN && rc != NGX_DONE)
			{
				ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
					"ngx_http_vod_state_machine_open_files: open_file failed %i", rc);
			}

			ctx->cur_source = cur_source;
			return rc;
		}
	}

	ctx->cur_source = NULL;
	return NGX_OK;
}

static void
ngx_http_vod_enable_directio(ngx_http_vod_ctx_t *ctx)
{
	media_clip_source_t* cur_source;

	for (cur_source = ctx->submodule_context.media_set.sources_head;
		cur_source != NULL;
		cur_source = cur_source->next)
	{
		ngx_file_reader_enable_directio(cur_source->reader_context);
	}
}

static vod_status_t
ngx_http_vod_write_segment_header_buffer(void* ctx, u_char* buffer, uint32_t size)
{
	ngx_http_vod_write_segment_context_t* context = (ngx_http_vod_write_segment_context_t*)ctx;
	ngx_chain_t *chain_head;
	ngx_chain_t *chain;
	ngx_buf_t *b;

	if (context->r->header_sent)
	{
		ngx_log_error(NGX_LOG_ERR, context->r->connection->log, 0,
			"ngx_http_vod_write_segment_header_buffer: called after the headers were already sent");
		return VOD_UNEXPECTED;
	}

	b = ngx_calloc_buf(context->r->pool);
	if (b == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, context->r->connection->log, 0,
			"ngx_http_vod_write_segment_header_buffer: ngx_calloc_buf failed");
		return VOD_ALLOC_FAILED;
	}

	b->pos = buffer;
	b->last = buffer + size;
	b->temporary = 1;

	chain = ngx_alloc_chain_link(context->r->pool);
	if (chain == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, context->r->connection->log, 0,
			"ngx_http_vod_write_segment_header_buffer: ngx_alloc_chain_link failed");
		return VOD_ALLOC_FAILED;
	}

	chain_head = context->chain_head;

	chain->buf = chain_head->buf;
	chain->next = chain_head->next;

	chain_head->buf = b;
	chain_head->next = chain;

	if (chain_head == context->chain_end)
	{
		context->chain_end = chain;
	}

	context->total_size += size;

	return VOD_OK;
}

static vod_status_t 
ngx_http_vod_write_segment_buffer(void* ctx, u_char* buffer, uint32_t size)
{
	ngx_http_vod_write_segment_context_t* context = (ngx_http_vod_write_segment_context_t*)ctx;
	ngx_buf_t *b;
	ngx_chain_t *chain;
	ngx_chain_t out;
	ngx_int_t rc;

	// create a wrapping ngx_buf_t
	b = ngx_calloc_buf(context->r->pool);
	if (b == NULL) 
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, context->r->connection->log, 0,
			"ngx_http_vod_write_segment_buffer: ngx_calloc_buf failed");
		return VOD_ALLOC_FAILED;
	}

	b->pos = buffer;
	b->last = buffer + size;
	b->temporary = 1;

	if (context->r->header_sent)
	{
		// headers already sent, output the chunk
		out.buf = b;
		out.next = NULL;

		rc = ngx_http_output_filter(context->r, &out);
		if (rc != NGX_OK && rc != NGX_AGAIN)
		{
			// either the connection dropped, or some allocation failed
			// in case the connection dropped, the error code doesn't matter anyway
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, context->r->connection->log, 0,
				"ngx_http_vod_write_segment_buffer: ngx_http_output_filter failed %i", rc);
			return VOD_ALLOC_FAILED;
		}
	}
	else
	{
		// headers not sent yet, add the buffer to the chain
		if (context->chain_end->buf != NULL)
		{
			chain = ngx_alloc_chain_link(context->r->pool);
			if (chain == NULL) 
			{
				ngx_log_debug0(NGX_LOG_DEBUG_HTTP, context->r->connection->log, 0,
					"ngx_http_vod_write_segment_buffer: ngx_alloc_chain_link failed");
				return VOD_ALLOC_FAILED;
			}

			context->chain_end->next = chain;
			context->chain_end = chain;
		}
		context->chain_end->buf = b;
	}

	context->total_size += size;

	return VOD_OK;
}

static ngx_int_t 
ngx_http_vod_init_frame_processing(ngx_http_vod_ctx_t *ctx)
{
	ngx_http_request_t* r = ctx->submodule_context.r;
	segment_writer_t segment_writer;
	ngx_str_t output_buffer = ngx_null_string;
	ngx_str_t content_type;
	ngx_int_t rc;
	off_t range_start;
	off_t range_end;

	rc = ngx_http_vod_update_timescale(ctx);
	if (rc != NGX_OK)
	{
		return rc;
	}

	// initialize the response writer
	ctx->out.buf = NULL;
	ctx->out.next = NULL;
	ctx->write_segment_buffer_context.r = r;
	ctx->write_segment_buffer_context.chain_head = &ctx->out;
	ctx->write_segment_buffer_context.chain_end = &ctx->out;
	ctx->write_segment_buffer_context.total_size = 0;

	segment_writer.write_tail = ngx_http_vod_write_segment_buffer;
	segment_writer.write_head = ngx_http_vod_write_segment_header_buffer;
	segment_writer.context = &ctx->write_segment_buffer_context;

	// initialize the protocol specific frame processor
	ngx_perf_counter_start(ctx->perf_counter_context);

	rc = ctx->request->init_frame_processor(
		&ctx->submodule_context,
		&segment_writer,
		&ctx->frame_processor,
		&ctx->frame_processor_state,
		&output_buffer,
		&ctx->content_length,
		&content_type);
	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_init_frame_processing: init_frame_processor failed %i", rc);
		return rc;
	}

	ngx_perf_counter_end(ctx->perf_counters, ctx->perf_counter_context, PC_INIT_FRAME_PROCESS);

	r->headers_out.content_type_len = content_type.len;
	r->headers_out.content_type.len = content_type.len;
	r->headers_out.content_type.data = content_type.data;

	// if the frame processor can't determine the size in advance we have to build the whole response before we can start sending it
	if (ctx->content_length != 0)
	{
		// send the response header
		rc = ngx_http_vod_send_header(r, ctx->content_length, NULL, MEDIA_SET_VOD, NULL);
		if (rc != NGX_OK)
		{
			return rc;
		}

		if (r->header_only || r->method == NGX_HTTP_HEAD)
		{
			return NGX_DONE;
		}
	}

	// write the initial buffer if provided
	if (output_buffer.len != 0)
	{
		rc = segment_writer.write_tail(segment_writer.context, output_buffer.data, output_buffer.len);
		if (rc != VOD_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_init_frame_processing: write_tail failed %i", rc);
			return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, rc);
		}

		// in case of a range request that is fully contained in the output buffer (e.g. 0-0), we're done
		if (ctx->content_length != 0 &&
			ctx->submodule_context.r->headers_in.range != NULL &&
			ngx_http_vod_range_parse(
				&ctx->submodule_context.r->headers_in.range->value,
				ctx->content_length,
				&range_start,
				&range_end) == NGX_OK &&
			(size_t)range_end <= output_buffer.len)
		{
			return NGX_DONE;
		}
	}

	rc = read_cache_allocate_buffer_slots(&ctx->read_cache_state, 0);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_init_frame_processing: read_cache_allocate_buffer_slots failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, rc);
	}

	return NGX_OK;
}

static ngx_int_t 
ngx_http_vod_process_media_frames(ngx_http_vod_ctx_t *ctx)
{
	read_cache_get_read_buffer_t read_buf;
	size_t cache_buffer_size;
	vod_status_t rc;

	for (;;)
	{
		ngx_perf_counter_start(ctx->perf_counter_context);

		rc = ctx->frame_processor(ctx->frame_processor_state);

		ngx_perf_counter_end(ctx->perf_counters, ctx->perf_counter_context, PC_PROCESS_FRAMES);

		switch (rc)
		{
		case VOD_OK:
			// we're done
			return NGX_OK;

		case VOD_AGAIN:
			// handled outside the switch
			break;

		default:
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_process_media_frames: frame_processor failed %i", rc);
			return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, rc);
		}

		// get a buffer to read into
		read_cache_get_read_buffer(
			&ctx->read_cache_state,
			&read_buf);

		cache_buffer_size = ctx->submodule_context.conf->cache_buffer_size;

		ctx->read_buffer.start = read_buf.buffer;
		if (read_buf.buffer != NULL)
		{
			ctx->read_buffer.end = read_buf.buffer + cache_buffer_size;
		}

		rc = ngx_http_vod_alloc_read_buffer(ctx, cache_buffer_size, ctx->alloc_params_index);
		if (rc != NGX_OK)
		{
			return rc;
		}
		
		// perform the read
		ngx_perf_counter_start(ctx->perf_counter_context);

		rc = ctx->read(
			read_buf.source->reader_context, 
			&ctx->read_buffer, 
			read_buf.size, 
			read_buf.offset);
		if (rc != NGX_OK)
		{
			if (rc != NGX_AGAIN)
			{
				ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
					"ngx_http_vod_process_media_frames: async_read failed %i", rc);
			}
			return rc;
		}

		ngx_perf_counter_end(ctx->perf_counters, ctx->perf_counter_context, PC_READ_FILE);

		// read completed synchronously, update the read cache
		read_cache_read_completed(&ctx->read_cache_state, &ctx->read_buffer);
	}
}

static ngx_int_t
ngx_http_vod_finalize_segment_response(ngx_http_vod_ctx_t *ctx)
{
	ngx_http_request_t *r = ctx->submodule_context.r;
	ngx_int_t rc;

	// if we already sent the headers and all the buffers, just signal completion and return
	if (r->header_sent)
	{
		if (ctx->write_segment_buffer_context.total_size != ctx->content_length)
		{
			ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
				"ngx_http_vod_finalize_segment_response: actual content length %uz is different than reported length %uz",
				ctx->write_segment_buffer_context.total_size, ctx->content_length);
		}

		rc = ngx_http_send_special(r, NGX_HTTP_LAST);
		if (rc != NGX_OK && rc != NGX_AGAIN)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_finalize_segment_response: ngx_http_send_special failed %i", rc);
			return rc;
		}
		return NGX_OK;
	}

	// mark the current buffer as last
	if (ctx->write_segment_buffer_context.chain_end->buf == NULL)
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_finalize_segment_response: no buffers were written");
		return ngx_http_vod_status_to_ngx_error(r, VOD_UNEXPECTED);
	}

	ctx->write_segment_buffer_context.chain_end->next = NULL;
	ctx->write_segment_buffer_context.chain_end->buf->last_buf = 1;

	// send the response header
	rc = ngx_http_vod_send_header(r, ctx->write_segment_buffer_context.total_size, NULL, MEDIA_SET_VOD, NULL);
	if (rc != NGX_OK)
	{
		return rc;
	}

	if (r->header_only || r->method == NGX_HTTP_HEAD)
	{
		return NGX_OK;
	}

	// send the response buffer chain
	rc = ngx_http_output_filter(r, &ctx->out);
	if (rc != NGX_OK && rc != NGX_AGAIN)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_finalize_segment_response: ngx_http_output_filter failed %i", rc);
		return rc;
	}
	return NGX_OK;
}

////// Audio filtering

static ngx_int_t
ngx_http_vod_init_process(ngx_cycle_t *cycle)
{
	vod_status_t rc;

	audio_filter_process_init(cycle->log);
	
#if (VOD_HAVE_LIB_AV_CODEC)
	thumb_grabber_process_init(cycle->log);
#endif // (VOD_HAVE_LIB_AV_CODEC)

	rc = language_code_process_init(cycle->pool, cycle->log);
	if (rc != VOD_OK)
	{
		return NGX_ERROR;
	}

	return NGX_OK;
}

static void 
ngx_http_vod_exit_process()
{
	webvtt_exit_process();

	dfxp_exit_process();
}

////// Clipping

static ngx_int_t
ngx_http_vod_send_clip_header(ngx_http_vod_ctx_t *ctx)
{
	ngx_http_request_t* r = ctx->submodule_context.r;
	ngx_chain_t* out;
	uint64_t first_offset;
	uint64_t last_offset;
	size_t response_size;
	ngx_str_t content_type;
	ngx_int_t rc;
	off_t range_start;
	off_t range_end;
	off_t header_size;
	off_t mdat_size;

	rc = ctx->format->clipper_build_header(
		&ctx->submodule_context.request_context,
		ctx->metadata_parts,
		ctx->metadata_part_count,
		ctx->clipper_parse_result,
		&out,
		&response_size, 
		&content_type);
	if (rc != VOD_OK)
	{
		ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_send_clip_header: clipper_build_header(%V) failed %i", &ctx->format->name, rc);
		return ngx_http_vod_status_to_ngx_error(r, rc);
	}

	// send the response header
	rc = ngx_http_vod_send_header(r, response_size, &content_type, MEDIA_SET_VOD, NULL);
	if (rc != NGX_OK)
	{
		return rc;
	}

	if (r->header_only || r->method == NGX_HTTP_HEAD)
	{
		return NGX_DONE;
	}

	rc = ngx_http_output_filter(r, out);
	if (rc != NGX_OK && rc != NGX_AGAIN)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_send_clip_header: ngx_http_output_filter failed %i", rc);
		return rc;
	}

	if (ctx->submodule_context.conf->request_handler == ngx_http_vod_remote_request_handler && 
		ctx->submodule_context.r->headers_in.range)
	{
		// in case of range request in remote mode, apply the requested range to the mdat dump offsets.
		// nginx's range filter module does not touch the dumped part since it is written in the context
		// a subrequest. 

		// TODO: apply the range on the mp4 header as well and return 206, to avoid making assumptions on
		//		nginx subrequest/range filter implementations

		rc = ngx_http_vod_range_parse(
			&ctx->submodule_context.r->headers_in.range->value,
			response_size,
			&range_start,
			&range_end);
		if (rc != NGX_OK)
		{
			ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
				"ngx_http_vod_send_clip_header: failed to parse range header \"%V\"",
				&ctx->submodule_context.r->headers_in.range->value);
			return rc;
		}

		first_offset = ctx->clipper_parse_result->first_offset;
		last_offset = ctx->clipper_parse_result->last_offset;

		mdat_size = last_offset - first_offset;
		header_size = response_size - mdat_size;

		if (range_end < header_size)
		{
			last_offset = 0;
		}
		else if (mdat_size > range_end - header_size)
		{
			last_offset = first_offset + range_end - header_size;
		}

		if (range_start > header_size)
		{
			first_offset += range_start - header_size;
		}

		ctx->clipper_parse_result->first_offset = first_offset;
		ctx->clipper_parse_result->last_offset = last_offset;
	}

	return NGX_OK;
}

////// Common

static ngx_int_t
ngx_http_vod_run_state_machine(ngx_http_vod_ctx_t *ctx)
{
	ngx_int_t rc;

	switch (ctx->state)
	{
	case STATE_READ_DRM_INFO:
		rc = ngx_http_vod_state_machine_get_drm_info(ctx);
		if (rc != NGX_OK)
		{
			return rc;
		}

		ctx->state = STATE_READ_METADATA_INITIAL;
		ctx->cur_sequence = ctx->submodule_context.media_set.sequences;
		// fallthrough

	case STATE_READ_METADATA_INITIAL:
	case STATE_READ_METADATA_OPEN_FILE:
	case STATE_READ_METADATA_READ:
	case STATE_READ_FRAMES_OPEN_FILE:
	case STATE_READ_FRAMES_READ:

		rc = ngx_http_vod_state_machine_parse_metadata(ctx);
		if (rc != NGX_OK)
		{
			return rc;
		}

		if (ctx->request == NULL)
		{
			rc = ngx_http_vod_send_clip_header(ctx);
			if (rc != NGX_OK)
			{
				if (rc == NGX_DONE)
				{
					rc = NGX_OK;
				}
				else
				{
					ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
						"ngx_http_vod_run_state_machine: ngx_http_vod_send_clip_header failed %i", rc);
				}
				return rc;
			}
		}
		else
		{
			rc = filter_init_filtered_clips(
				&ctx->submodule_context.request_context,
				&ctx->submodule_context.media_set, 
				(ctx->request->parse_type & PARSE_FLAG_FRAMES_DURATION) != 0);
			if (rc != VOD_OK)
			{
				ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
					"ngx_http_vod_run_state_machine: filter_init_filtered_clips failed %i", rc);
				return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, rc);
			}

			rc = ngx_http_vod_validate_streams(ctx);
			if (rc != NGX_OK)
			{
				return rc;
			}

			// handle metadata requests
			if (ctx->request->handle_metadata_request != NULL)
			{
				return ngx_http_vod_handle_metadata_request(ctx);
			}

			// initialize the read cache
			read_cache_init(
				&ctx->read_cache_state,
				&ctx->submodule_context.request_context,
				ctx->submodule_context.conf->cache_buffer_size,
				ctx->alignment);
		}

		ctx->state = STATE_OPEN_FILE;
		ctx->cur_source = ctx->submodule_context.media_set.sources_head;
		// fallthrough

	case STATE_OPEN_FILE:
		rc = ngx_http_vod_state_machine_open_files(ctx);
		if (rc != NGX_OK)
		{
			return rc;
		}

		// enable directio if enabled in the configuration (ignore errors)
		// Note that directio is set on transfer only to allow the kernel to cache the "moov" atom
		if (ctx->reader->enable_directio != NULL)
		{
			ngx_http_vod_enable_directio(ctx);
		}

		if (ctx->request == NULL)
		{
			if (ctx->clipper_parse_result->first_offset < ctx->clipper_parse_result->last_offset)
			{
				ctx->cur_source = ctx->submodule_context.media_set.sources_head;

				ctx->state = STATE_DUMP_FILE_PART;

				rc = ctx->reader->dump_part(
					ctx->cur_source->reader_context,
					ctx->clipper_parse_result->first_offset,
					ctx->clipper_parse_result->last_offset);
				if (rc != NGX_OK)
				{
					return rc;
				}
			}

			rc = ngx_http_send_special(ctx->submodule_context.r, NGX_HTTP_LAST);
			if (rc != NGX_OK && rc != NGX_AGAIN)
			{
				ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
					"ngx_http_vod_run_state_machine: ngx_http_send_special failed %i", rc);
				return rc;
			}

			return NGX_OK;
		}

		if (ctx->submodule_context.media_set.audio_filtering_needed)
		{
			// initialize the filtering of audio frames
			ctx->state = STATE_FILTER_FRAMES;
			ctx->cur_source = ctx->submodule_context.media_set.sources_head;

			rc = filter_init_state(
				&ctx->submodule_context.request_context,
				&ctx->read_cache_state,
				&ctx->submodule_context.media_set, 
				&ctx->frame_processor_state);
			if (rc != VOD_OK)
			{
				ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
					"ngx_http_vod_run_state_machine: filter_init_state failed %i", rc);
				return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, rc);
			}

			ctx->frame_processor = filter_run_state_machine;
		}

		// fallthrough

	case STATE_FILTER_FRAMES:
		// if audio filtering already started, process frames
		if (ctx->frame_processor != NULL)
		{
			rc = ngx_http_vod_process_media_frames(ctx);
			if (rc != NGX_OK)
			{
				return rc;
			}
		}

		// initialize the processing of the video/audio frames
		rc = ngx_http_vod_init_frame_processing(ctx);
		if (rc != NGX_OK)
		{
			if (rc == NGX_DONE)
			{
				rc = NGX_OK;
			}
			else
			{
				ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
					"ngx_http_vod_run_state_machine: ngx_http_vod_init_frame_processing failed %i", rc);
			}
			return rc;
		}

		if (ctx->frame_processor_state == NULL)
		{
			return ngx_http_vod_finalize_segment_response(ctx);
		}

		ctx->submodule_context.request_context.log->action = "processing frames";
		ctx->state = STATE_PROCESS_FRAMES;
		// fallthrough

	case STATE_PROCESS_FRAMES:
		rc = ngx_http_vod_process_media_frames(ctx);
		if (rc != NGX_OK)
		{
			if (rc != NGX_AGAIN)
			{
				ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
					"ngx_http_vod_run_state_machine: ngx_http_vod_process_media_frames failed %i", rc);
			}
			return rc;
		}

		return ngx_http_vod_finalize_segment_response(ctx);

	case STATE_DUMP_OPEN_FILE:
		return ctx->reader->dump_request(ctx);

	case STATE_DUMP_FILE_PART:
		rc = ngx_http_send_special(ctx->submodule_context.r, NGX_HTTP_LAST);
		if (rc != NGX_OK && rc != NGX_AGAIN)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_run_state_machine: ngx_http_send_special failed %i", rc);
			return rc;
		}

		return NGX_OK;
	}

	ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
		"ngx_http_vod_run_state_machine: invalid state %d", ctx->state);
	return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, VOD_UNEXPECTED);
}

static void
ngx_http_vod_handle_read_completed(void* context, ngx_int_t rc, ngx_buf_t* buf, ssize_t bytes_read)
{
	ngx_http_vod_ctx_t *ctx = (ngx_http_vod_ctx_t *)context;
	ssize_t expected_size;

	if (rc != NGX_OK)
	{
		if (ctx->state == STATE_MAP_READ && 
			ctx->mapping.stale_retries > 0 && 
			errno == ESTALE)
		{
			ctx->mapping.stale_retries--;
			ctx->state = STATE_MAP_INITIAL;

			rc = ctx->state_machine(ctx);
			if (rc == NGX_AGAIN)
			{
				return;
			}

			goto finalize_request;
		}

		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
			"ngx_http_vod_handle_read_completed: read failed %i", rc);
		goto finalize_request;
	}

	if (ctx->state == STATE_DUMP_FILE_PART)
	{
		expected_size = ctx->clipper_parse_result->last_offset - ctx->clipper_parse_result->first_offset;
		if (bytes_read != expected_size)
		{
			ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_handle_read_completed: read size %z different than expected %z, probably a truncated file", 
				bytes_read, expected_size);
			rc = ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, VOD_BAD_DATA);
			goto finalize_request;
		}
	}
	else if (bytes_read <= 0)
	{
		switch (ctx->state)
		{
		case STATE_MAP_READ:		// the mapping state machine handles the case of empty mapping
			break;

		case STATE_READ_METADATA_READ:
			if ((ctx->read_flags & MEDIA_READ_FLAG_ALLOW_EMPTY_READ) != 0)
			{
				break;
			}
			// fallthrough

		default:
			ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_handle_read_completed: bytes read is zero");
			rc = ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, VOD_BAD_DATA);
			goto finalize_request;
		}
	}

	ngx_perf_counter_end(ctx->perf_counters, ctx->perf_counter_context, ctx->perf_counter_async_read);

	switch (ctx->state)
	{
	case STATE_FILTER_FRAMES:
	case STATE_PROCESS_FRAMES:
		if (buf == NULL)
		{
			buf = &ctx->read_buffer;
		}
		ctx->frames_bytes_read += (buf->last - buf->pos);
		read_cache_read_completed(&ctx->read_cache_state, buf);
		break;

	default:
		if (buf != NULL)
		{
			ctx->read_buffer = *buf;
		}
		break;
	}

	// run the state machine
	rc = ctx->state_machine(ctx);
	if (rc == NGX_AGAIN)
	{
		return;
	}

finalize_request:

	ngx_http_vod_finalize_request(ctx, rc);
}

static void
ngx_http_vod_init_file_key(media_clip_source_t* cur_source, ngx_str_t* prefix)
{
	ngx_md5_t md5;

	ngx_md5_init(&md5);
	if (prefix != NULL)
	{
		ngx_md5_update(&md5, prefix->data, prefix->len);
	}
	ngx_md5_update(&md5, cur_source->mapped_uri.data, cur_source->mapped_uri.len);
	ngx_md5_final(cur_source->file_key, &md5);
}

static ngx_int_t
ngx_http_vod_init_encryption_key(
	ngx_http_request_t *r, 
	ngx_http_vod_loc_conf_t* conf, 
	media_sequence_t* cur_sequence)
{
	ngx_str_t encryption_key_seed;
	ngx_md5_t md5;

	if (conf->secret_key != NULL)
	{
		// calculate the encryption key seed
		if (ngx_http_complex_value(
			r,
			conf->secret_key,
			&encryption_key_seed) != NGX_OK)
		{
			ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_init_encryption_key: ngx_http_complex_value failed");
			return NGX_ERROR;
		}
	}
	else
	{
		encryption_key_seed = cur_sequence->mapped_uri;
	}

	// hash the seed to get the key
	ngx_md5_init(&md5);
	ngx_md5_update(&md5, encryption_key_seed.data, encryption_key_seed.len);
	ngx_md5_final(cur_sequence->encryption_key, &md5);

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_start_processing_media_file(ngx_http_vod_ctx_t *ctx)
{
	ngx_http_vod_loc_conf_t* conf;
	media_clip_source_t* cur_source;
	ngx_http_request_t *r;
	ngx_int_t rc;

	// update request flags
	r = ctx->submodule_context.r;
	r->root_tested = !r->error_page;
	r->allow_ranges = 1;

	// set the state machine
	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);
	ctx->state_machine = ngx_http_vod_run_state_machine;

	// handle serve requests
	if (ctx->request == NULL &&
		ctx->submodule_context.media_set.sources_head->clip_from == 0 &&
		ctx->submodule_context.media_set.sources_head->clip_to == ULLONG_MAX)
	{
		ctx->state = STATE_DUMP_OPEN_FILE;

		cur_source = ctx->submodule_context.media_set.sources_head;
		ctx->cur_source = cur_source;

		rc = ctx->reader->open(r, &cur_source->mapped_uri, 0, &cur_source->reader_context);
		if (rc != NGX_OK)
		{
			if (rc != NGX_AGAIN && rc != NGX_DONE)
			{
				ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
					"ngx_http_vod_start_processing_media_file: open_file failed %i", rc);
			}
			return rc;
		}

		return ctx->reader->dump_request(ctx);
	}

	// initialize the file keys
	conf = ctx->submodule_context.conf;

	for (cur_source = ctx->submodule_context.media_set.sources_head;
		cur_source != NULL;
		cur_source = cur_source->next)
	{
		ngx_http_vod_init_file_key(cur_source, ctx->file_key_prefix);
	}

	// initialize the uri / encryption keys
	if (conf->drm_enabled || conf->secret_key != NULL)
	{
		for (ctx->cur_sequence = ctx->submodule_context.media_set.sequences;
			ctx->cur_sequence < ctx->submodule_context.media_set.sequences_end;
			ctx->cur_sequence++)
		{
			rc = ngx_http_vod_init_encryption_key(r, conf, ctx->cur_sequence);
			if (rc != NGX_OK)
			{
				return rc;
			}
		}
	}

	// restart the file index/uri params
	ctx->cur_source = ctx->submodule_context.media_set.sources_head;

	if (ctx->submodule_context.conf->drm_enabled)
	{
		ctx->state = STATE_READ_DRM_INFO;
		ctx->cur_sequence = ctx->submodule_context.media_set.sequences;
	}
	else
	{
		ctx->state = STATE_READ_METADATA_INITIAL;
	}

	return ngx_http_vod_run_state_machine(ctx);
}

////// Local & mapped modes

static ngx_int_t
ngx_http_vod_map_uris_to_paths(ngx_http_vod_ctx_t *ctx)
{
	media_clip_source_t* cur_source;
	ngx_http_request_t *r = ctx->submodule_context.r;
	ngx_str_t original_uri;
	u_char *last;
	size_t root;
	ngx_str_t path;

	original_uri = r->uri;
	for (cur_source = ctx->submodule_context.media_set.sources_head;
		cur_source != NULL;
		cur_source = cur_source->next)
	{
		r->uri = cur_source->stripped_uri;
		last = ngx_http_map_uri_to_path(r, &path, &root, 0);
		r->uri = original_uri;
		if (last == NULL)
		{
			ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_map_uris_to_paths: ngx_http_map_uri_to_path failed");
			return NGX_HTTP_INTERNAL_SERVER_ERROR;
		}

		path.len = last - path.data;

		cur_source->mapped_uri = path;
	}

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_dump_request_to_fallback(ngx_http_request_t *r)
{
	ngx_http_vod_loc_conf_t* conf;
	ngx_child_request_params_t child_params;

	conf = ngx_http_get_module_loc_conf(r, ngx_http_vod_module);

	if (conf->fallback_upstream_location.len == 0)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_dump_request_to_fallback: no fallback configured");
		return NGX_ERROR;
	}

	if (ngx_http_vod_header_exists(r, &conf->proxy_header.key))
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_dump_request_to_fallback: proxy header exists");
		return NGX_ERROR;
	}

	// dump the request to the fallback upstream
	ngx_memzero(&child_params, sizeof(child_params));
	child_params.method = r->method;
	child_params.base_uri = r->uri;
	child_params.extra_args = r->args;
	child_params.extra_header = conf->proxy_header;
	child_params.proxy_range = 1;
	child_params.proxy_all_headers = 1;

	return ngx_child_request_start(
		r,
		NULL,
		NULL,
		&conf->fallback_upstream_location,
		&child_params,
		NULL);
}

#if (NGX_THREADS)
static void
ngx_http_vod_file_open_completed_internal(void* context, ngx_int_t rc, ngx_flag_t fallback)
{
	ngx_http_vod_ctx_t *ctx = (ngx_http_vod_ctx_t *)context;

	if (rc != NGX_OK)
	{
		if (fallback && rc == NGX_HTTP_NOT_FOUND)
		{
			// try the fallback
			rc = ngx_http_vod_dump_request_to_fallback(ctx->submodule_context.r);
			if (rc != NGX_AGAIN)
			{
				rc = NGX_HTTP_NOT_FOUND;
			}
			goto finalize_request;
		}

		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.r->connection->log, 0,
			"ngx_http_vod_file_open_completed_internal: read failed %i", rc);
		goto finalize_request;
	}

	ngx_perf_counter_end(ctx->perf_counters, ctx->perf_counter_context, PC_ASYNC_OPEN_FILE);

	// run the state machine
	rc = ctx->state_machine(ctx);
	if (rc == NGX_AGAIN)
	{
		return;
	}

finalize_request:

	ngx_http_vod_finalize_request(ctx, rc);
}

static void
ngx_http_vod_file_open_completed(void* context, ngx_int_t rc)
{
	ngx_http_vod_file_open_completed_internal(context, rc, 0);
}

static void
ngx_http_vod_file_open_completed_with_fallback(void* context, ngx_int_t rc)
{
	ngx_http_vod_file_open_completed_internal(context, rc, 1);
}
#endif // NGX_THREADS

static ngx_int_t
ngx_http_vod_init_file_reader_internal(ngx_http_request_t *r, ngx_str_t* path, void** context, uint32_t flags)
{
	ngx_file_reader_state_t* state;
	ngx_http_core_loc_conf_t *clcf;
	ngx_http_vod_ctx_t *ctx;
	ngx_flag_t fallback = (flags & OPEN_FILE_FALLBACK_ENABLED) != 0;
	ngx_int_t rc;

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);

	clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

	state = ngx_pcalloc(r->pool, sizeof(*state));
	if (state == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_init_file_reader_internal: ngx_pcalloc failed");
		return ngx_http_vod_status_to_ngx_error(r, VOD_ALLOC_FAILED);
	}

	*context = state;

	ngx_perf_counter_start(ctx->perf_counter_context);

#if (NGX_THREADS)
	if (ctx->submodule_context.conf->open_file_thread_pool != NULL)
	{
		rc = ngx_file_reader_init_async(
			state,
			&ctx->async_open_context,
			ctx->submodule_context.conf->open_file_thread_pool,
			fallback ? ngx_http_vod_file_open_completed_with_fallback : ngx_http_vod_file_open_completed,
			ngx_http_vod_handle_read_completed,
			ctx,
			r,
			clcf,
			path,
			flags);
	}
	else
	{
#endif
		rc = ngx_file_reader_init(
			state,
			ngx_http_vod_handle_read_completed,
			ctx,
			r,
			clcf,
			path,
			flags);
#if (NGX_THREADS)
	}
#endif
	if (rc != NGX_OK)
	{
		if (fallback && rc == NGX_HTTP_NOT_FOUND)
		{
			// try the fallback
			rc = ngx_http_vod_dump_request_to_fallback(r);
			if (rc != NGX_AGAIN)
			{
				return NGX_HTTP_NOT_FOUND;
			}
			return rc;
		}

		if (rc != NGX_AGAIN)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_init_file_reader_internal: ngx_file_reader_init failed %i", rc);
		}
		return rc;
	}

	ngx_perf_counter_end(ctx->perf_counters, ctx->perf_counter_context, PC_OPEN_FILE);

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_init_file_reader(ngx_http_request_t *r, ngx_str_t* path, uint32_t flags, void** context)
{
	return ngx_http_vod_init_file_reader_internal(r, path, context, flags);
}

static ngx_int_t
ngx_http_vod_init_file_reader_with_fallback(ngx_http_request_t *r, ngx_str_t* path, uint32_t flags, void** context)
{
	return ngx_http_vod_init_file_reader_internal(r, path, context, flags | OPEN_FILE_FALLBACK_ENABLED);
}

// Note: this function initializes r->exten in order to have nginx select the correct mime type for the request
//		the code was copied from nginx's ngx_http_set_exten
static void
ngx_http_vod_set_request_extension(ngx_http_request_t *r, ngx_str_t* path)
{
	ngx_int_t  i;

	ngx_str_null(&r->exten);

	for (i = path->len - 1; i > 1; i--) {
		if (path->data[i] == '.' && path->data[i - 1] != '/') {

			r->exten.len = path->len - i - 1;
			r->exten.data = &path->data[i + 1];

			return;

		}
		else if (path->data[i] == '/') {
			return;
		}
	}

	return;
}

static ngx_int_t
ngx_http_vod_dump_file(ngx_http_vod_ctx_t* ctx)
{
	ngx_http_request_t* r = ctx->submodule_context.r;
	ngx_file_reader_state_t* state = ctx->cur_source->reader_context;
	ngx_int_t                  rc;

	ngx_http_vod_set_request_extension(r, &state->file.name);

	rc = ngx_http_set_content_type(r);
	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, state->log, 0,
			"ngx_http_vod_dump_file: ngx_http_set_content_type failed %i", rc);
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	// send the response header
	rc = ngx_http_vod_send_header(r, state->file_size, NULL, MEDIA_SET_VOD, NULL);
	if (rc != NGX_OK)
	{
		return rc;
	}

	if (r->header_only || r->method == NGX_HTTP_HEAD)
	{
		return NGX_OK;
	}

	ngx_file_reader_enable_directio(state);		// ignore errors

	return ngx_file_reader_dump_file_part(state, 0, 0);
}

////// Remote & mapped modes

static ngx_int_t
ngx_http_vod_async_http_read(ngx_http_vod_http_reader_state_t *state, ngx_buf_t *buf, size_t size, off_t offset)
{
	ngx_http_vod_ctx_t *ctx;
	ngx_child_request_params_t child_params;

	ctx = ngx_http_get_module_ctx(state->r, ngx_http_vod_module);

	ngx_memzero(&child_params, sizeof(child_params));
	child_params.method = NGX_HTTP_GET;
	child_params.base_uri = state->cur_remote_suburi;
	child_params.extra_args = ctx->upstream_extra_args;
	child_params.range_start = offset;
	child_params.range_end = offset + size;

	return ngx_child_request_start(
		state->r,
		ngx_http_vod_handle_read_completed,
		ctx,
		&state->upstream_location,
		&child_params,
		buf);
}

static ngx_int_t
ngx_http_vod_dump_http_part(void* context, off_t start, off_t end)
{
	ngx_http_vod_http_reader_state_t *state = context;
	ngx_http_vod_loc_conf_t *conf;
	ngx_http_vod_ctx_t *ctx;
	ngx_child_request_params_t child_params;

	ctx = ngx_http_get_module_ctx(state->r, ngx_http_vod_module);
	conf = ctx->submodule_context.conf;

	ngx_memzero(&child_params, sizeof(child_params));
	child_params.method = NGX_HTTP_GET;
	child_params.base_uri = state->cur_remote_suburi;
	child_params.extra_args = ctx->upstream_extra_args;
	child_params.range_start = start;
	child_params.range_end = end;

	return ngx_child_request_start(
		state->r,
		ngx_http_vod_handle_read_completed,
		ctx,
		&conf->upstream_location,
		&child_params,
		NULL);
}

static ngx_int_t
ngx_http_vod_dump_http_request(ngx_http_vod_ctx_t *ctx)
{
	ngx_http_request_t* r;
	ngx_http_vod_loc_conf_t *conf;
	ngx_child_request_params_t child_params;

	conf = ctx->submodule_context.conf;
	r = ctx->submodule_context.r;

	ngx_memzero(&child_params, sizeof(child_params));
	child_params.method = r->method;
	child_params.base_uri = r->uri;
	child_params.extra_args = ctx->upstream_extra_args;
	child_params.proxy_range = 1;
	child_params.proxy_all_headers = 1;

	return ngx_child_request_start(
		r,
		NULL,
		NULL,
		&conf->upstream_location,
		&child_params,
		NULL);
}

static ngx_int_t
ngx_http_vod_http_reader_open_file(ngx_http_request_t* r, ngx_str_t* path, uint32_t flags, void** context)
{
	ngx_http_vod_http_reader_state_t* state;
	ngx_http_vod_ctx_t *ctx;

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);

	// initialize the upstream variables
	if (ctx->upstream_extra_args.len == 0 &&
		ctx->submodule_context.conf->upstream_extra_args != NULL)
	{
		if (ngx_http_complex_value(
			ctx->submodule_context.r,
			ctx->submodule_context.conf->upstream_extra_args,
			&ctx->upstream_extra_args) != NGX_OK)
		{
			return NGX_ERROR;
		}
	}

	state = ngx_palloc(r->pool, sizeof(*state));
	if (state == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_http_reader_open_file: ngx_palloc failed");
		return ngx_http_vod_status_to_ngx_error(r, VOD_ALLOC_FAILED);
	}

	// Note: for http, no need to open any files, just save the remote uri
	state->r = r;
	state->cur_remote_suburi = *path;
	if (ctx->state == STATE_MAP_OPEN || ctx->submodule_context.conf->remote_upstream_location.len == 0)
	{
		state->upstream_location = ctx->submodule_context.conf->upstream_location;
	}
	else
	{
		state->upstream_location = ctx->submodule_context.conf->remote_upstream_location;
	}
	*context = state;

	return NGX_OK;
}

////// Local mode only

ngx_int_t
ngx_http_vod_local_request_handler(ngx_http_request_t *r)
{
	ngx_http_vod_ctx_t *ctx;
	ngx_int_t rc;

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);

	// map all uris to paths
	rc = ngx_http_vod_map_uris_to_paths(ctx);
	if (rc != NGX_OK)
	{
		return rc;
	}

	// initialize for reading files
	ctx->reader = &reader_file_with_fallback;
	ctx->read = (ngx_http_vod_async_read_func_t)ngx_async_file_read;
	ctx->alloc_params_index = READER_FILE;
	ctx->alignment = ctx->alloc_params[READER_FILE].alignment;
	ctx->perf_counter_async_read = PC_ASYNC_READ_FILE;

	// start the state machine
	rc = ngx_http_vod_start_processing_media_file(ctx);
	if (rc != NGX_AGAIN && rc != NGX_DONE && rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_local_request_handler: ngx_http_vod_start_processing_media_file failed %i", rc);
	}

	return rc;
}

////// Mapped mode only

static ngx_int_t
ngx_http_vod_map_run_step(ngx_http_vod_ctx_t *ctx)
{
	ngx_buffer_cache_t* cache;
	ngx_buf_t* response;
	ngx_str_t* prefix;
	ngx_str_t mapping;
	ngx_str_t uri;
	ngx_md5_t md5;
	ngx_int_t rc;
	size_t read_size;
	int cache_index;

	switch (ctx->state)
	{
	case STATE_MAP_INITIAL:
		// get the uri
		rc = ctx->mapping.get_uri(ctx, &uri);
		if (rc != NGX_OK)
		{
			return rc;
		}

		// calculate the cache key
		prefix = ctx->mapping.cache_key_prefix;
		ngx_md5_init(&md5);
		if (prefix != NULL)
		{
			ngx_md5_update(&md5, prefix->data, prefix->len);
		}
		ngx_md5_update(&md5, uri.data, uri.len);
		ngx_md5_final(ctx->mapping.cache_key, &md5);

		// try getting the mapping from cache
		if (ngx_buffer_cache_fetch_multi_perf(
			ctx->perf_counters,
			ctx->mapping.caches,
			ctx->mapping.cache_count,
			ctx->mapping.cache_key,
			&mapping) >= 0)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_map_run_step: mapping cache hit %V", &mapping);

			rc = ctx->mapping.apply(ctx, &mapping, &cache_index);
			if (rc != NGX_OK)
			{
				return rc;
			}

			break;
		}
		else
		{
			ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_map_run_step: mapping cache miss");
		}

		// open the mapping file
		ctx->submodule_context.request_context.log->action = "getting mapping";

		ctx->state = STATE_MAP_OPEN;

		rc = ctx->reader->open(ctx->submodule_context.r, &uri, OPEN_FILE_NO_CACHE, &ctx->mapping.reader_context);
		if (rc != NGX_OK)
		{
			if (rc != NGX_AGAIN && rc != NGX_DONE)
			{
				ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
					"ngx_http_vod_map_run_step: open_file failed %i", rc);
			}
			return rc;
		}

		// fallthrough

	case STATE_MAP_OPEN:

		if (ctx->reader->get_size != NULL)
		{
			read_size = ctx->reader->get_size(ctx->mapping.reader_context);
			if (read_size <= 0)
			{
				ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
					"ngx_http_vod_map_run_step: empty mapping response");
				return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, VOD_EMPTY_MAPPING);
			}
			else if (read_size > ctx->mapping.max_response_size)
			{
				ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
					"ngx_http_vod_map_run_step: mapping size %uz greater than limit %uz", 
					read_size, ctx->mapping.max_response_size);
				return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, VOD_BAD_MAPPING);
			}
		}
		else
		{
			read_size = ctx->mapping.max_response_size;
		}

		rc = ngx_http_vod_alloc_read_buffer(ctx, read_size, ctx->alloc_params_index);
		if (rc != NGX_OK)
		{
			return rc;
		}

		// read the mapping
		ctx->state = STATE_MAP_READ;
		ngx_perf_counter_start(ctx->perf_counter_context);

		rc = ctx->read(ctx->mapping.reader_context, &ctx->read_buffer, read_size, 0);
		if (rc != NGX_OK)
		{
			if (rc != NGX_AGAIN)
			{
				ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
					"ngx_http_vod_map_run_step: async_read failed %i", rc);
			}
			return rc;
		}

		ngx_perf_counter_end(ctx->perf_counters, ctx->perf_counter_context, PC_MAP_PATH);

		// fallthrough

	case STATE_MAP_READ:

		response = &ctx->read_buffer;

		if (response->last == response->pos)
		{
			ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_map_run_step: empty mapping response");
			return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, VOD_EMPTY_MAPPING);
		}

		// apply the mapping
		if (response->last >= response->end)
		{
			ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_map_run_step: not enough room in buffer for null terminator");
			return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, VOD_BAD_MAPPING);
		}

		*response->last = '\0';

		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
			"ngx_http_vod_map_run_step: mapping result %s", response->pos);

		mapping.data = response->pos;
		mapping.len = response->last - response->pos;
		rc = ctx->mapping.apply(ctx, &mapping, &cache_index);
		if (rc != NGX_OK)
		{
			return rc;
		}

		// save to cache
		cache = ctx->mapping.caches[cache_index];
		if (cache != NULL)
		{
			if (ngx_buffer_cache_store_perf(
				ctx->perf_counters,
				cache,
				ctx->mapping.cache_key,
				response->pos,
				response->last - response->pos))
			{
				ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
					"ngx_http_vod_map_run_step: stored in mapping cache");
			}
			else
			{
				ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
					"ngx_http_vod_map_run_step: failed to store mapping in cache");
			}
		}

		ctx->state = STATE_MAP_INITIAL;
		break;

	default:
		ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
			"ngx_http_vod_map_run_step: invalid state %d", ctx->state);
		return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, VOD_UNEXPECTED);
	}

	return NGX_OK;
}

/// map source clip

static ngx_int_t
ngx_http_vod_map_source_clip_done(ngx_http_vod_ctx_t *ctx)
{
	ngx_http_vod_loc_conf_t* conf;
	conf = ctx->submodule_context.conf;

	if (conf->remote_upstream_location.len == 0)
	{
		// initialize for reading files
		ctx->reader = &reader_file;
		ctx->read = (ngx_http_vod_async_read_func_t)ngx_async_file_read;
		ctx->alloc_params_index = READER_FILE;
		ctx->alignment = ctx->alloc_params[READER_FILE].alignment;
	}
	else
	{
		// initialize for http read
		ctx->reader = &reader_http;
		ctx->read = (ngx_http_vod_async_read_func_t)ngx_http_vod_async_http_read;
		ctx->alloc_params_index = READER_HTTP;
		ctx->alignment = ctx->alloc_params[READER_HTTP].alignment;
	}
	ctx->perf_counter_async_read = PC_ASYNC_READ_FILE;
	// run the main state machine
	return ngx_http_vod_start_processing_media_file(ctx);
}

static ngx_int_t
ngx_http_vod_map_source_clip_get_uri(ngx_http_vod_ctx_t *ctx, ngx_str_t* uri)
{
	if (ngx_http_complex_value(
		ctx->submodule_context.r,
		ctx->submodule_context.conf->source_clip_map_uri,
		uri) != NGX_OK)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
			"ngx_http_vod_map_source_clip_get_uri: ngx_http_complex_value failed");
		return NGX_ERROR;
	}

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_map_source_clip_apply(ngx_http_vod_ctx_t *ctx, ngx_str_t* mapping, int* cache_index)
{
	media_clip_source_t* cur_clip = vod_container_of(ctx->cur_clip, media_clip_source_t, base);
	vod_status_t rc;

	rc = media_set_map_source(&ctx->submodule_context.request_context, mapping->data, cur_clip);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
			"ngx_http_vod_map_source_clip_apply: media_set_map_source failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, rc);
	}

	*cache_index = 0;

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_map_source_clip_state_machine(ngx_http_vod_ctx_t *ctx)
{
	media_clip_source_t* cur_clip;
	ngx_int_t rc;

	// map the uris
	for (;;)
	{
		rc = ngx_http_vod_map_run_step(ctx);
		if (rc != NGX_OK)
		{
			return rc;
		}

		cur_clip = (media_clip_source_t*)ctx->cur_clip;
		if (cur_clip->next == NULL)
		{
			break;
		}

		ctx->cur_clip = &cur_clip->next->base;
	}

	// merge the mapped sources list with the sources list
	cur_clip->next = ctx->submodule_context.media_set.sources_head;
	ctx->submodule_context.media_set.sources_head = ctx->submodule_context.media_set.mapped_sources_head;
	ctx->cur_clip = NULL;

	return ngx_http_vod_map_source_clip_done(ctx);
}

static ngx_int_t
ngx_http_vod_map_source_clip_start(ngx_http_vod_ctx_t *ctx)
{
	ngx_http_vod_loc_conf_t* conf = ctx->submodule_context.conf;

	if (conf->source_clip_map_uri == NULL)
	{
		ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
			"ngx_http_vod_map_source_clip_start: media set contains mapped source clips and \"vod_source_clip_map_uri\" was not configured");
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	ctx->mapping.caches = conf->mapping_cache;
	ctx->mapping.cache_count = 1;
	ctx->mapping.get_uri = ngx_http_vod_map_source_clip_get_uri;
	ctx->mapping.apply = ngx_http_vod_map_source_clip_apply;

	ctx->cur_clip = &ctx->submodule_context.media_set.mapped_sources_head->base;
	ctx->state_machine = ngx_http_vod_map_source_clip_state_machine;

	return ngx_http_vod_map_source_clip_state_machine(ctx);
}

/// send notifications

static void
ngx_http_vod_notification_finished(void* context, ngx_int_t rc, ngx_buf_t* buf, ssize_t bytes_read)
{
	ngx_http_vod_ctx_t *ctx = context;

	// ignore errors

	rc = ngx_http_vod_send_notification(ctx);
	if (rc != NGX_AGAIN)
	{
		ngx_http_vod_finalize_request(ctx, rc);
	}
}

static ngx_int_t
ngx_http_vod_send_notification(ngx_http_vod_ctx_t *ctx)
{
	ngx_child_request_params_t child_params;
	ngx_http_vod_loc_conf_t *conf;
	media_notification_t* notification;
	ngx_int_t rc;

	notification = ctx->submodule_context.media_set.notifications_head;
	if (notification == NULL)
	{
		// sent all notifications, redirect the segment request
		return ngx_http_send_response(
			ctx->submodule_context.r,
			NGX_HTTP_MOVED_TEMPORARILY,
			NULL,
			ctx->submodule_context.conf->redirect_segments_url);
	}

	// remove the notification from list
	ctx->submodule_context.media_set.notifications_head = notification->next;

	// get the notification uri
	conf = ctx->submodule_context.conf;
	if (conf->notification_uri == NULL)
	{
		ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
			"ngx_http_vod_send_notification: no notification uri was configured");
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	ngx_memzero(&child_params, sizeof(child_params));
	ctx->notification = notification;

	if (ngx_http_complex_value(
		ctx->submodule_context.r,
		conf->notification_uri,
		&child_params.base_uri) != NGX_OK)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
			"ngx_http_vod_send_notification: ngx_http_complex_value failed");
		return NGX_ERROR;
	}

	ctx->notification = NULL;

	// send the notification
	rc = ngx_http_vod_alloc_read_buffer(ctx, conf->max_upstream_headers_size, READER_HTTP);
	if (rc != NGX_OK)
	{
		return rc;
	}

	child_params.method = NGX_HTTP_GET;
	child_params.extra_args = ctx->upstream_extra_args;
	child_params.range_start = 0;
	child_params.range_end = 1;

	return ngx_child_request_start(
		ctx->submodule_context.r,
		ngx_http_vod_notification_finished,
		ctx,
		&conf->upstream_location,
		&child_params,
		&ctx->read_buffer);
}

/// map dynamic clip

static ngx_int_t
ngx_http_vod_map_dynamic_clip_done(ngx_http_vod_ctx_t *ctx)
{
	// if it's a segment request and redirect segment urls is set, send notifications
	if (ctx->submodule_context.conf->redirect_segments_url != NULL &&
		ctx->request->request_class != REQUEST_CLASS_MANIFEST)
	{
		return ngx_http_vod_send_notification(ctx);
	}

	// map source clips
	if (ctx->submodule_context.media_set.mapped_sources_head != NULL)
	{
		return ngx_http_vod_map_source_clip_start(ctx);
	}

	return ngx_http_vod_map_source_clip_done(ctx);
}

static ngx_int_t
ngx_http_vod_map_dynamic_clip_get_uri(ngx_http_vod_ctx_t *ctx, ngx_str_t* uri)
{
	if (ngx_http_complex_value(
		ctx->submodule_context.r,
		ctx->submodule_context.conf->dynamic_clip_map_uri,
		uri) != NGX_OK)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
			"ngx_http_vod_map_dynamic_clip_get_uri: ngx_http_complex_value failed");
		return NGX_ERROR;
	}

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_map_dynamic_clip_apply(ngx_http_vod_ctx_t *ctx, ngx_str_t* mapping, int* cache_index)
{
	vod_status_t rc;
	
	rc = dynamic_clip_apply_mapping_json(
		vod_container_of(ctx->cur_clip, media_clip_dynamic_t, base),
		&ctx->submodule_context.request_context,
		mapping->data,
		&ctx->submodule_context.media_set);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
			"ngx_http_vod_map_dynamic_clip_apply: dynamic_clip_apply_mapping_json failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, rc);
	}

	*cache_index = 0;

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_map_dynamic_clip_state_machine(ngx_http_vod_ctx_t *ctx)
{
	ngx_int_t rc;

	// map the uris
	while (ctx->cur_clip != NULL)
	{
		rc = ngx_http_vod_map_run_step(ctx);
		if (rc != NGX_OK)
		{
			return rc;
		}

		ctx->cur_clip = &((media_clip_dynamic_t*)ctx->cur_clip)->next->base;
	}

	return ngx_http_vod_map_dynamic_clip_done(ctx);
}

static ngx_int_t
ngx_http_vod_map_dynamic_clip_start(ngx_http_vod_ctx_t *ctx)
{
	ngx_http_vod_loc_conf_t* conf = ctx->submodule_context.conf;

	// map the dynamic clips by calling the upstream
	if (conf->dynamic_clip_map_uri == NULL)
	{
		ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
			"ngx_http_vod_map_dynamic_clip_start: media set contains dynamic clips and \"vod_dynamic_clip_map_uri\" was not configured");
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	ctx->mapping.caches = &conf->dynamic_mapping_cache;
	ctx->mapping.cache_count = 1;
	ctx->mapping.get_uri = ngx_http_vod_map_dynamic_clip_get_uri;
	ctx->mapping.apply = ngx_http_vod_map_dynamic_clip_apply;

	ctx->cur_clip = &ctx->submodule_context.media_set.dynamic_clips_head->base;
	ctx->state_machine = ngx_http_vod_map_dynamic_clip_state_machine;

	return ngx_http_vod_map_dynamic_clip_state_machine(ctx);

}

static ngx_int_t
ngx_http_vod_map_dynamic_clip_apply_from_string(ngx_http_vod_ctx_t *ctx)
{
	vod_status_t rc;
	ngx_str_t mapping;

	if (ngx_http_complex_value(
		ctx->submodule_context.r,
		ctx->submodule_context.conf->apply_dynamic_mapping,
		&mapping) != NGX_OK)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
			"ngx_http_vod_map_dynamic_clip_apply_from_string: ngx_http_complex_value failed");
		return NGX_ERROR;
	}

	rc = dynamic_clip_apply_mapping_string(
		&ctx->submodule_context.request_context,
		&ctx->submodule_context.media_set,
		&mapping);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
			"ngx_http_vod_map_dynamic_clip_apply_from_string: dynamic_clip_apply_mapping_string failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, rc);
	}

	return NGX_OK;
}

/// map media set

static ngx_int_t
ngx_http_vod_map_media_set_get_uri(ngx_http_vod_ctx_t *ctx, ngx_str_t* uri)
{
	if (ctx->submodule_context.conf->media_set_map_uri != NULL)
	{
		if (ngx_http_complex_value(
			ctx->submodule_context.r,
			ctx->submodule_context.conf->media_set_map_uri,
			uri) != NGX_OK)
		{
			ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_map_media_set_get_uri: ngx_http_complex_value failed");
			return NGX_ERROR;
		}
	}
	else
	{
		*uri = ctx->cur_source->mapped_uri;
	}

	return NGX_OK;
}

#if (NGX_HAVE_LIB_AV_CODEC)
static ngx_int_t
ngx_http_vod_handle_thumb_redirect(
	ngx_http_vod_ctx_t *ctx, 
	media_set_t* media_set)
{
	ngx_http_request_t* r = ctx->submodule_context.r;
	ngx_str_t location;
	ngx_int_t rc;

	rc = ngx_http_vod_thumb_get_url(
		&ctx->submodule_context,
		media_set->has_multi_sequences ? (uint32_t)(1 << media_set->sequences[0].index) : 0xffffffff,
		&location);
	if (rc != NGX_OK)
	{
		return rc;
	}

	if (ngx_http_discard_request_body(r) != NGX_OK)
	{
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	r->headers_out.status = NGX_HTTP_MOVED_TEMPORARILY;

	ngx_http_clear_location(r);

	r->headers_out.location = ngx_list_push(&r->headers_out.headers);
	if (r->headers_out.location == NULL)
	{
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	r->headers_out.location->hash = 1;
	ngx_str_set(&r->headers_out.location->key, "Location");
	r->headers_out.location->value = location;

	return r->headers_out.status;
}
#endif // (NGX_HAVE_LIB_AV_CODEC)

static ngx_int_t
ngx_http_vod_map_media_set_apply(ngx_http_vod_ctx_t *ctx, ngx_str_t* mapping, int* cache_index)
{
	ngx_http_vod_loc_conf_t* conf = ctx->submodule_context.conf;
	ngx_perf_counter_context(perf_counter_context);
	media_clip_source_t *cur_source = ctx->cur_source;
	media_clip_source_t* mapped_source;
	media_sequence_t* sequence;
	media_set_t mapped_media_set;
	ngx_str_t src_path;
	ngx_str_t path;
	ngx_int_t rc;
	uint32_t request_flags;

	// optimization for the case of simple mapping response
	if (mapping->len >= conf->path_response_prefix.len + conf->path_response_postfix.len &&
		ngx_memcmp(mapping->data, conf->path_response_prefix.data, conf->path_response_prefix.len) == 0 &&
		ngx_memcmp(mapping->data + mapping->len - conf->path_response_postfix.len,
		conf->path_response_postfix.data, conf->path_response_postfix.len) == 0 &&
		memchr(mapping->data + conf->path_response_prefix.len, '"',
		mapping->len - conf->path_response_prefix.len - conf->path_response_postfix.len) == NULL)
	{
		src_path.len = mapping->len - conf->path_response_prefix.len - conf->path_response_postfix.len;
		if (src_path.len <= 0)
		{
			// file not found
			ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_map_media_set_apply: empty path returned from upstream %V",
				&cur_source->stripped_uri);

			// try the fallback
			rc = ngx_http_vod_dump_request_to_fallback(ctx->submodule_context.r);
			if (rc != NGX_AGAIN)
			{
				rc = ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, VOD_EMPTY_MAPPING);
			}
			return rc;
		}

		src_path.data = mapping->data + conf->path_response_prefix.len;

		// copy the path to decode it and make it null terminated
		path.data = ngx_palloc(ctx->submodule_context.request_context.pool, src_path.len + 1);
		if (path.data == NULL)
		{
			ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_map_media_set_apply: ngx_palloc failed");
			return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, VOD_ALLOC_FAILED);
		}

		path.len = 0;

		rc = vod_json_decode_string(&path, &src_path);
		if (rc != VOD_JSON_OK)
		{
			ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_map_media_set_apply: vod_json_decode_string failed %i", rc);
			return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, VOD_BAD_MAPPING);
		}

		path.data[path.len] = '\0';

		// move to the next suburi
		cur_source->sequence->mapped_uri = path;
		cur_source->mapped_uri = path;

		*cache_index = CACHE_TYPE_VOD;

		return NGX_OK;
	}

	// TODO: in case the new media set may replace the existing one, propagate clip from, clip to, rate

	ngx_perf_counter_start(perf_counter_context);

	request_flags = ctx->request != NULL ? ctx->request->flags : 0;

	if (conf->force_continuous_timestamps)
	{
		request_flags |= REQUEST_FLAG_NO_DISCONTINUITY;
	}

	rc = media_set_parse_json(
		&ctx->submodule_context.request_context,
		mapping->data,
		&ctx->submodule_context.request_params,
		&ctx->submodule_context.conf->segmenter,
		&cur_source->uri,
		request_flags,
		&mapped_media_set);

	switch (rc)
	{
	case VOD_NOT_FOUND:
		// file not found, try the fallback
		rc = ngx_http_vod_dump_request_to_fallback(ctx->submodule_context.r);
		if (rc != NGX_AGAIN)
		{
			rc = NGX_HTTP_NOT_FOUND;
		}
		return rc;

	case VOD_OK:
		break;		// handled outside the switch

#if (NGX_HAVE_LIB_AV_CODEC)
	case VOD_REDIRECT:
		return ngx_http_vod_handle_thumb_redirect(ctx, &mapped_media_set);
#endif // (NGX_HAVE_LIB_AV_CODEC)

	default:
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
			"ngx_http_vod_map_media_set_apply: media_set_parse_json failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, rc);
	}

	ngx_perf_counter_end(ctx->perf_counters, perf_counter_context, PC_PARSE_MEDIA_SET);

	if (mapped_media_set.sequence_count == 1 &&
		mapped_media_set.timing.durations == NULL &&
		mapped_media_set.sequences[0].clips[0]->type == MEDIA_CLIP_SOURCE &&
		!mapped_media_set.has_multi_sequences)
	{
		mapped_source = (media_clip_source_t*)*mapped_media_set.sequences[0].clips;

		if (mapped_source->clip_from == 0 &&
			mapped_source->clip_to == ULLONG_MAX &&
			mapped_source->tracks_mask[MEDIA_TYPE_AUDIO] == 0xffffffff &&
			mapped_source->tracks_mask[MEDIA_TYPE_VIDEO] == 0xffffffff)
		{
			// mapping result is a simple file path, set the uri of the current source
			ctx->submodule_context.media_set.id = mapped_media_set.id;
			sequence = cur_source->sequence;
			sequence->mapped_uri = mapped_source->mapped_uri;
			sequence->language = mapped_media_set.sequences->language;
			sequence->label = mapped_media_set.sequences->label;
			sequence->id = mapped_media_set.sequences->id;
			cur_source->mapped_uri = mapped_source->mapped_uri;
			cur_source->encryption_key = mapped_source->encryption_key;

			*cache_index = CACHE_TYPE_VOD;

			return NGX_OK;
		}
	}

	if (ctx->request == NULL)
	{
		ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
			"ngx_http_vod_map_media_set_apply: unsupported - non-trivial mapping in progressive download");
		return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, VOD_BAD_REQUEST);
	}

	if (ctx->submodule_context.media_set.sequence_count == 1 &&
		ctx->submodule_context.media_set.sequences[0].clips[0]->type == MEDIA_CLIP_SOURCE)
	{
		// media set was a single source, replace it with the mapping result
		ctx->submodule_context.media_set = mapped_media_set;

		// cur_source is pointing to the old media set, move it to the end of the new one
		ctx->cur_source = NULL;

		// Note: this is ok because CACHE_TYPE_xxx matches MEDIA_TYPE_xxx in order
		*cache_index = mapped_media_set.type;

		return NGX_OK;
	}

	ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
		"ngx_http_vod_map_media_set_apply: unsupported - multi uri/filtered request %V did not return a simple json",
		&cur_source->stripped_uri);
	return ngx_http_vod_status_to_ngx_error(ctx->submodule_context.r, VOD_BAD_REQUEST);
}

static ngx_int_t
ngx_http_vod_map_media_set_state_machine(ngx_http_vod_ctx_t *ctx)
{
	ngx_int_t rc;

	// map the uris
	while (ctx->cur_source != NULL)
	{
		rc = ngx_http_vod_map_run_step(ctx);
		if (rc != NGX_OK)
		{
			return rc;
		}
		
		// Note: cur_source can be null in case the media set is replaced
		if (ctx->cur_source == NULL)
		{
			break;
		}

		ctx->cur_source = ctx->cur_source->next;
	}

	// check whether dynamic clip mapping is needed
	if (ctx->submodule_context.media_set.dynamic_clips_head == NULL)
	{
		return ngx_http_vod_map_dynamic_clip_done(ctx);
	}

	// apply the mapping passed on vod_apply_dynamic_mapping
	if (ctx->submodule_context.conf->apply_dynamic_mapping != NULL)
	{
		rc = ngx_http_vod_map_dynamic_clip_apply_from_string(ctx);
		if (rc != NGX_OK)
		{
			return rc;
		}

		if (ctx->submodule_context.media_set.dynamic_clips_head == NULL)
		{
			return ngx_http_vod_map_dynamic_clip_done(ctx);
		}
	}

	return ngx_http_vod_map_dynamic_clip_start(ctx);
}

/// mapped mode main

ngx_int_t
ngx_http_vod_mapped_request_handler(ngx_http_request_t *r)
{
	ngx_http_vod_loc_conf_t* conf;
	ngx_http_vod_ctx_t *ctx;
	ngx_int_t rc;

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);
	conf = ctx->submodule_context.conf;

	if (conf->upstream_location.len == 0)
	{
		// map the uris to files
		rc = ngx_http_vod_map_uris_to_paths(ctx);
		if (rc != NGX_OK)
		{
			return rc;
		}

		// initialize for reading files
		ctx->reader = &reader_file;
		ctx->read = (ngx_http_vod_async_read_func_t)ngx_async_file_read;
		ctx->alloc_params_index = READER_FILE;
		ctx->alignment = ctx->alloc_params[READER_FILE].alignment;
		ctx->mapping.stale_retries = MAX_STALE_RETRIES;
	}
	else
	{
		// initialize for http read
		ctx->reader = &reader_http;
		ctx->read = (ngx_http_vod_async_read_func_t)ngx_http_vod_async_http_read;
		ctx->alloc_params_index = READER_HTTP;
		ctx->alignment = ctx->alloc_params[READER_HTTP].alignment;
	}

	// initialize the mapping context
	ctx->mapping.cache_key_prefix = (r->headers_in.host != NULL ? &r->headers_in.host->value : NULL);
	ctx->mapping.caches = conf->mapping_cache;
	ctx->mapping.cache_count = CACHE_TYPE_COUNT;
	ctx->mapping.max_response_size = conf->max_mapping_response_size;
	ctx->mapping.get_uri = ngx_http_vod_map_media_set_get_uri;
	ctx->mapping.apply = ngx_http_vod_map_media_set_apply;

	ctx->perf_counter_async_read = PC_MAP_PATH;
	ctx->state_machine = ngx_http_vod_map_media_set_state_machine;

	rc = ngx_http_vod_map_media_set_state_machine(ctx);
	if (rc != NGX_AGAIN && rc != NGX_DONE && rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_mapped_request_handler: ngx_http_vod_map_media_set_state_machine failed %i", rc);
	}

	return rc;
}

////// Remote mode only

ngx_int_t
ngx_http_vod_remote_request_handler(ngx_http_request_t *r)
{
	ngx_http_vod_ctx_t *ctx;
	ngx_int_t rc;

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);

	ctx->reader = &reader_http;
	ctx->read = (ngx_http_vod_async_read_func_t)ngx_http_vod_async_http_read;
	ctx->alloc_params_index = READER_HTTP;
	ctx->alignment = ctx->alloc_params[READER_HTTP].alignment;
	ctx->perf_counter_async_read = PC_ASYNC_READ_FILE;
	ctx->file_key_prefix = (r->headers_in.host != NULL ? &r->headers_in.host->value : NULL);

	rc = ngx_http_vod_start_processing_media_file(ctx);
	if (rc != NGX_AGAIN && rc != NGX_DONE && rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_remote_request_handler: ngx_http_vod_start_processing_media_file failed %i", rc);
	}

	return rc;
}

////// Main

static ngx_int_t
ngx_http_vod_parse_uri(
	ngx_http_request_t *r, 
	ngx_http_vod_loc_conf_t *conf, 
	request_params_t* request_params,
	media_set_t* media_set,
	const ngx_http_vod_request_t** request)
{
	ngx_str_t uri_path;
	ngx_str_t uri_file_name;
	ngx_int_t rc;
	int file_components;
	
	file_components = conf->submodule.get_file_path_components(&r->uri);

	if (!ngx_http_vod_split_uri_file_name(&r->uri, file_components, &uri_path, &uri_file_name))
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_parse_uri: ngx_http_vod_split_uri_file_name failed");
		return ngx_http_vod_status_to_ngx_error(r, VOD_BAD_REQUEST);
	}

	request_params->segment_index = INVALID_SEGMENT_INDEX;
	request_params->segment_time = INVALID_SEGMENT_TIME;

	rc = conf->submodule.parse_uri_file_name(r, conf, uri_file_name.data, uri_file_name.data + uri_file_name.len, request_params, request);
	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_parse_uri: parse_uri_file_name failed %i", rc);
		return rc;
	}

	rc = ngx_http_vod_parse_uri_path(
		r, 
		&conf->multi_uri_suffix, 
		&conf->uri_params_hash, 
		&uri_path, 
		request_params, 
		media_set);
	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_parse_uri: ngx_http_vod_parse_uri_path failed %i", rc);
		return rc;
	}

	if (media_set->sequence_count != 1)
	{
		if (((*request)->flags & REQUEST_FLAG_SINGLE_TRACK) != 0)
		{
			ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
				"ngx_http_vod_parse_uri: request has more than one sub uri while only one is supported");
			return ngx_http_vod_status_to_ngx_error(r, VOD_BAD_REQUEST);
		}

		if (media_set->sequence_count != 2 &&
			((*request)->flags & REQUEST_FLAG_SINGLE_TRACK_PER_MEDIA_TYPE) != 0)
		{
			ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
				"ngx_http_vod_parse_uri: request has more than two sub uris while only a single track per media type is allowed");
			return ngx_http_vod_status_to_ngx_error(r, VOD_BAD_REQUEST);
		}
	}

	return NGX_OK;
}

ngx_int_t
ngx_http_vod_handler(ngx_http_request_t *r)
{
	ngx_perf_counter_context(pcctx);
	response_cache_header_t cache_header;
	ngx_perf_counters_t* perf_counters;
	ngx_http_vod_ctx_t *ctx;
	request_params_t request_params;
	media_set_t media_set;
	const ngx_http_vod_request_t* request;
	ngx_http_core_loc_conf_t *clcf;
	ngx_http_vod_loc_conf_t *conf;
	u_char request_key[BUFFER_CACHE_KEY_SIZE];
	ngx_md5_t md5;
	ngx_str_t cache_buffer;
	ngx_str_t content_type;
	ngx_str_t response;
	ngx_str_t base_url;
	ngx_int_t rc;
	int cache_type;
#if (NGX_DEBUG)
	ngx_str_t time_str;
#endif

	ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "ngx_http_vod_handler: started");

	ngx_perf_counter_start(pcctx);
	conf = ngx_http_get_module_loc_conf(r, ngx_http_vod_module);
	perf_counters = ngx_perf_counter_get_state(conf->perf_counters_zone);

	if (r->method == NGX_HTTP_OPTIONS)
	{
		response.data = NULL;
		response.len = 0;

		rc = ngx_http_vod_send_header(r, response.len, &options_content_type, MEDIA_SET_VOD, NULL);
		if (rc != NGX_OK)
		{
			return rc;
		}

		rc = ngx_http_vod_send_response(r, &response, NULL);
		goto done;
	}

	// we respond to 'GET' and 'HEAD' requests only
	if (!(r->method & (NGX_HTTP_GET | NGX_HTTP_HEAD)))
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_handler: unsupported method %ui", r->method);
		rc = NGX_HTTP_NOT_ALLOWED;
		goto done;
	}

	// discard request body, since we don't need it here
	rc = ngx_http_discard_request_body(r);
	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_handler: ngx_http_discard_request_body failed %i", rc);
		goto done;
	}

	// parse the uri
	ngx_memzero(&request_params, sizeof(request_params));
	ngx_memzero(&media_set, sizeof(media_set));
	if (conf->submodule.parse_uri_file_name != NULL)
	{
		rc = ngx_http_vod_parse_uri(r, conf, &request_params, &media_set, &request);
		if (rc != NGX_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_handler: ngx_http_vod_parse_uri failed %i", rc);
			goto done;
		}
	}
	else
	{
		request = NULL;
		request_params.sequences_mask = 1;
		request_params.tracks_mask[MEDIA_TYPE_VIDEO] = 0xffffffff;
		request_params.tracks_mask[MEDIA_TYPE_AUDIO] = 0xffffffff;

		rc = ngx_http_vod_parse_uri_path(
			r,
			&conf->multi_uri_suffix,
			&conf->pd_uri_params_hash,
			&r->uri,
			&request_params,
			&media_set);
		if (rc != NGX_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_handler: ngx_http_vod_parse_uri_path failed %i", rc);
			goto done;
		}

		if (media_set.sequence_count != 1)
		{
			ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
				"ngx_http_vod_handler: request has more than one sub uri while only one is supported");
			rc = ngx_http_vod_status_to_ngx_error(r, VOD_BAD_REQUEST);
			goto done;
		}
	}

	if (request != NULL &&
		request->handle_metadata_request != NULL)
	{
		// calc request key from host + uri
		ngx_md5_init(&md5);

		base_url.len = 0;
		rc = ngx_http_vod_get_base_url(r, conf->base_url, &empty_string, &base_url);
		if (rc != NGX_OK)
		{
			return rc;
		}
		ngx_md5_update(&md5, base_url.data, base_url.len);

		if (conf->segments_base_url != NULL)
		{
			base_url.len = 0;
			rc = ngx_http_vod_get_base_url(r, conf->segments_base_url, &empty_string, &base_url);
			if (rc != NGX_OK)
			{
				return rc;
			}
			ngx_md5_update(&md5, base_url.data, base_url.len);
		}

		ngx_md5_update(&md5, r->uri.data, r->uri.len);

		ngx_md5_final(request_key, &md5);

		// try to fetch from cache
		cache_type = ngx_buffer_cache_fetch_copy_perf(
			r,
			perf_counters,
			conf->response_cache,
			CACHE_TYPE_COUNT,
			request_key,
			&cache_buffer);
		if (cache_type >= 0 &&
			cache_buffer.len > sizeof(cache_header))
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_handler: response cache hit, size is %uz", cache_buffer.len);

			// extract the content type
			ngx_memcpy(&cache_header, cache_buffer.data, sizeof(cache_header));
			cache_buffer.data += sizeof(cache_header);
			cache_buffer.len -= sizeof(cache_header);

			content_type.data = cache_buffer.data;
			content_type.len = cache_header.content_type_len;

			if (cache_buffer.len >= content_type.len)
			{
				// extract the response buffer
				response.data = cache_buffer.data + content_type.len;
				response.len = cache_buffer.len - content_type.len;

				// update request flags
				r->root_tested = !r->error_page;
				r->allow_ranges = 1;

				// return the response
				rc = ngx_http_vod_send_header(r, response.len, &content_type, cache_header.media_set_type, request);
				if (rc != NGX_OK)
				{
					return rc;
				}

				rc = ngx_http_vod_send_response(r, &response, NULL);
				goto done;
			}
		}
		else
		{
			ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_handler: response cache miss");
		}
	}

	// initialize the context
	ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_vod_ctx_t));
	if (ctx == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_handler: ngx_pcalloc failed");
		rc = NGX_HTTP_INTERNAL_SERVER_ERROR;
		goto done;
	}

	ngx_memcpy(ctx->request_key, request_key, sizeof(request_key));
	ctx->submodule_context.r = r;
	ctx->submodule_context.conf = conf;
	ctx->submodule_context.request_params = request_params;
	ctx->submodule_context.media_set = media_set;
	ctx->submodule_context.media_set.segmenter_conf = &conf->segmenter;
	ctx->submodule_context.media_set.version = request_params.version;
	ctx->request = request;
	ctx->cur_source = media_set.sources_head;
	ctx->submodule_context.request_context.pool = r->pool;
	ctx->submodule_context.request_context.log = r->connection->log;
	ctx->submodule_context.request_context.output_buffer_pool = conf->output_buffer_pool;
	ctx->perf_counters = perf_counters;
	ngx_perf_counter_copy(ctx->total_perf_counter_context, pcctx);

#if (NGX_DEBUG)
	// in debug builds allow overriding the server time
	if (ngx_http_arg(r, (u_char *) "time", sizeof("time") - 1, &time_str) == NGX_OK)
	{
		ctx->submodule_context.request_context.time = ngx_atotm(time_str.data, time_str.len);
	}
#endif

	clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);
	ctx->alloc_params[READER_FILE].alignment = clcf->directio_alignment;
	ctx->alloc_params[READER_HTTP].alignment = 1;	// don't care about alignment in case of remote
	ctx->alloc_params[READER_HTTP].extra_size = conf->max_upstream_headers_size + 1;	// the + 1 is discussed here: http://trac.nginx.org/nginx/ticket/680

	ngx_http_set_ctx(r, ctx, ngx_http_vod_module);

	// call the mode specific handler (remote/mapped/local)
	rc = conf->request_handler(r);

done:

	if (rc != NGX_AGAIN)
	{
		ngx_perf_counter_end(perf_counters, pcctx, PC_TOTAL);
	}

	ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "ngx_http_vod_handler: done");

	return rc;
}
