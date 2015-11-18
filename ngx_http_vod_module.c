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
#include "vod/mp4/mp4_parser.h"
#include "vod/mp4/mp4_clipper.h"
#include "vod/read_cache.h"
#include "vod/filters/audio_filter.h"
#include "vod/filters/rate_filter.h"
#include "vod/filters/filter.h"
#include "vod/media_set_parser.h"

// constants
#define MAX_MOOV_START_READS (3)		// maximum number of attempts to find the moov atom start for non-fast-start files

// enums
enum {
	STATE_READ_DRM_INFO,
	STATE_PARSE_MOOV_INITIAL,
	STATE_PARSE_MOOV_OPEN_FILE,
	STATE_PARSE_MOOV_READ_HEADER,
	STATE_PARSE_MOOV_READ_DATA,
	STATE_OPEN_FILE,
	STATE_FILTER_FRAMES,
	STATE_PROCESS_FRAMES,
	STATE_DUMP_OPEN_FILE,
	STATE_DUMP_FILE_PART,
};

// typedefs
struct ngx_http_vod_ctx_s;
typedef struct ngx_http_vod_ctx_s ngx_http_vod_ctx_t;

typedef ngx_int_t(*ngx_http_vod_open_file_t)(ngx_http_request_t* r, ngx_str_t* path, void** context);
typedef ngx_int_t(*ngx_http_vod_async_read_func_t)(void* context, u_char *buf, size_t size, off_t offset);
typedef ngx_int_t(*ngx_http_vod_dump_part_t)(void* context, off_t start, off_t end);
typedef ngx_int_t(*ngx_http_vod_dump_request_t)(struct ngx_http_vod_ctx_s* context);

typedef struct {
	ngx_http_request_t* r;
	ngx_chain_t* chain_head;
	ngx_chain_t* chain_end;
	size_t total_size;
} ngx_http_vod_write_segment_context_t;

typedef struct {
	ngx_http_request_t* r;
	ngx_str_t cur_remote_suburi;
} ngx_http_vod_http_reader_state_t;

struct ngx_http_vod_ctx_s {
	// base params
	ngx_http_vod_submodule_context_t submodule_context;
	off_t alignment;
	int state;
	u_char request_key[BUFFER_CACHE_KEY_SIZE];
	ngx_perf_counters_t* perf_counters;
	ngx_perf_counter_context(perf_counter_context);
	ngx_perf_counter_context(total_perf_counter_context);

	// moov read state
	u_char* read_buffer;
	size_t buffer_size;
	off_t read_offset;
	off_t atom_start_offset;
	int moov_start_reads;
	off_t moov_offset;
	size_t moov_size;
	u_char* ftyp_ptr;
	size_t ftyp_size;

	// clipper
	mp4_clipper_parse_result_t clipper_parse_result;

	// reading abstraction (over file / http)
	ngx_http_vod_open_file_t open_file;
	ngx_http_vod_async_read_func_t async_reader;
	ngx_http_vod_dump_part_t dump_part;
	ngx_http_vod_dump_request_t dump_request;

	// read state - file
#if (NGX_THREADS)
	void* async_open_context;
#endif

	// read state - http
	ngx_child_request_buffers_t child_request_buffers;
	ngx_str_t* file_key_prefix;
	ngx_str_t upstream_extra_args;

	// segment requests only
	size_t content_length;
	read_cache_state_t read_cache_state;
	ngx_http_vod_frame_processor_t frame_processor;
	void* frame_processor_state;
	ngx_chain_t out;
	ngx_http_vod_write_segment_context_t write_segment_buffer_context;
};

// forward declarations
static ngx_int_t ngx_http_vod_run_state_machine(ngx_http_vod_ctx_t *ctx);
static ngx_int_t ngx_http_vod_process_init(ngx_cycle_t *cycle);

// globals
ngx_module_t  ngx_http_vod_module = {
    NGX_MODULE_V1,
    &ngx_http_vod_module_ctx,         /* module context */
    ngx_http_vod_commands,            /* module directives */
    NGX_HTTP_MODULE,                  /* module type */
    NULL,                             /* init master */
    NULL,                             /* init module */
    ngx_http_vod_process_init,        /* init process */
    NULL,                             /* init thread */
    NULL,                             /* exit thread */
    NULL,                             /* exit process */
    NULL,                             /* exit master */
    NGX_MODULE_V1_PADDING
};

static ngx_str_t options_content_type = ngx_string("text/plain");
static ngx_str_t mp4_content_type = ngx_string("video/mp4");

////// Variables

ngx_int_t
ngx_http_vod_set_filepath_var(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data)
{
	ngx_http_vod_ctx_t *ctx;
	ngx_str_t* value;

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);

	if (ctx == NULL ||
		ctx->submodule_context.cur_sequence < ctx->submodule_context.media_set.sequences ||
		ctx->submodule_context.cur_sequence >= ctx->submodule_context.media_set.sequences_end)
	{
		v->not_found = 1;
		return NGX_OK;
	}

	value = &ctx->submodule_context.cur_sequence->mapped_uri;
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

ngx_int_t
ngx_http_vod_set_suburi_var(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data)
{
	ngx_http_vod_ctx_t *ctx;
	ngx_str_t* value;

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);

	if (ctx == NULL ||
		ctx->submodule_context.cur_sequence < ctx->submodule_context.media_set.sequences ||
		ctx->submodule_context.cur_sequence >= ctx->submodule_context.media_set.sequences_end)
	{
		v->not_found = 1;
		return NGX_OK;
	}

	value = &ctx->submodule_context.cur_sequence->stripped_uri;
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

////// Perf counter wrappers

static ngx_flag_t
ngx_buffer_cache_fetch_perf(
	ngx_perf_counters_t* perf_counters,
	ngx_shm_zone_t *shm_zone,
	u_char* key,
	u_char** buffer,
	size_t* buffer_size)
{
	ngx_perf_counter_context(pcctx);
	ngx_flag_t result;
	
	ngx_perf_counter_start(pcctx);

	result = ngx_buffer_cache_fetch(shm_zone, key, buffer, buffer_size);

	ngx_perf_counter_end(perf_counters, pcctx, PC_FETCH_CACHE);

	return result;
}

static ngx_flag_t
ngx_buffer_cache_fetch_copy_perf(
	ngx_http_request_t* r,
	ngx_perf_counters_t* perf_counters,
	ngx_shm_zone_t *shm_zone,
	u_char* key,
	u_char** buffer,
	size_t* buffer_size)
{
	ngx_perf_counter_context(pcctx);
	ngx_flag_t result;
	u_char* original_buffer;
	u_char* buffer_copy;
	size_t original_size;

	ngx_perf_counter_start(pcctx);

	result = ngx_buffer_cache_fetch(shm_zone, key, &original_buffer, &original_size);

	ngx_perf_counter_end(perf_counters, pcctx, PC_FETCH_CACHE);

	if (!result)
	{
		return 0;
	}

	buffer_copy = ngx_palloc(r->pool, original_size + 1);
	if (buffer_copy == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_buffer_cache_fetch_copy_perf: ngx_palloc failed");
		return 0;
	}

	ngx_memcpy(buffer_copy, original_buffer, original_size);
	buffer_copy[original_size] = '\0';

	*buffer = buffer_copy;
	*buffer_size = original_size;

	return 1;
}

static ngx_flag_t
ngx_buffer_cache_store_perf(
	ngx_perf_counters_t* perf_counters,
	ngx_shm_zone_t *shm_zone,
	u_char* key,
	u_char* source_buffer,
	size_t buffer_size)
{
	ngx_perf_counter_context(pcctx);
	ngx_flag_t result;

	ngx_perf_counter_start(pcctx);

	result = ngx_buffer_cache_store(shm_zone, key, source_buffer, buffer_size);

	ngx_perf_counter_end(perf_counters, pcctx, PC_STORE_CACHE);

	return result;
}

static ngx_flag_t 
ngx_buffer_cache_store_gather_perf(
	ngx_perf_counters_t* perf_counters,
	ngx_shm_zone_t *shm_zone,
	u_char* key,
	ngx_str_t* buffers,
	size_t buffer_count)
{
	ngx_perf_counter_context(pcctx);
	ngx_flag_t result;

	ngx_perf_counter_start(pcctx);

	result = ngx_buffer_cache_store_gather(shm_zone, key, buffers, buffer_count);

	ngx_perf_counter_end(perf_counters, pcctx, PC_STORE_CACHE);

	return result;
}

static ngx_int_t
ngx_http_vod_send_header(ngx_http_request_t* r, off_t content_length_n, ngx_str_t* content_type)
{
	ngx_http_vod_loc_conf_t* conf;
	ngx_int_t rc;

	if (content_type != NULL)
	{
		r->headers_out.content_type = *content_type;
		r->headers_out.content_type_len = content_type->len;
	}
	
	r->headers_out.status = NGX_HTTP_OK;
	r->headers_out.content_length_n = content_length_n;

	conf = ngx_http_get_module_loc_conf(r, ngx_http_vod_module);
	if (conf->last_modified_time != -1 &&
		ngx_http_test_content_type(r, &conf->last_modified_types) != NULL)
	{
		r->headers_out.last_modified_time = conf->last_modified_time;
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
	if (ctx->submodule_context.r->header_sent && rc != NGX_OK)
	{
		rc = NGX_ERROR;
	}

	ngx_perf_counter_end(ctx->perf_counters, ctx->total_perf_counter_context, PC_TOTAL);

	ngx_http_finalize_request(ctx->submodule_context.r, rc);
}

////// DRM

static void
ngx_http_vod_drm_info_request_finished(void* context, ngx_int_t rc, off_t content_length, ngx_buf_t* response)
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

	ngx_perf_counter_end(ctx->perf_counters, ctx->perf_counter_context, PC_GET_DRM_INFO);

	drm_info.data = response->pos;
	drm_info.len = content_length;
	*response->last = '\0';		// this is ok since ngx_child_http_request always allocates the content-length + 1

	ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "ngx_http_vod_drm_info_request_finished: result %V", &drm_info);

	// parse the drm info
	rc = conf->submodule.parse_drm_info(&ctx->submodule_context, &drm_info, &ctx->submodule_context.cur_sequence->drm_info);
	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_drm_info_request_finished: invalid drm info response %V", &drm_info);
		rc = NGX_HTTP_SERVICE_UNAVAILABLE;
		goto finalize_request;
	}

	// save to cache
	if (conf->drm_info_cache_zone != NULL)
	{
		if (ngx_buffer_cache_store_perf(
			ctx->perf_counters,
			conf->drm_info_cache_zone,
			ctx->submodule_context.cur_sequence->uri_key,
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

	ctx->submodule_context.cur_sequence++;

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

	for (;
		ctx->submodule_context.cur_sequence < ctx->submodule_context.media_set.sequences_end;
		ctx->submodule_context.cur_sequence++)
	{
		if (conf->drm_info_cache_zone != NULL)
		{
			// try to read the drm info from cache
			if (ngx_buffer_cache_fetch_copy_perf(r, ctx->perf_counters, conf->drm_info_cache_zone, ctx->submodule_context.cur_sequence->uri_key, &drm_info.data, &drm_info.len))
			{
				ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
					"ngx_http_vod_state_machine_get_drm_info: drm info cache hit, size is %uz", drm_info.len);

				rc = conf->submodule.parse_drm_info(&ctx->submodule_context, &drm_info, &ctx->submodule_context.cur_sequence->drm_info);
				if (rc != NGX_OK)
				{
					ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
						"ngx_http_vod_state_machine_get_drm_info: invalid drm info in cache %V", &drm_info);
					return rc;
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

		ngx_memzero(&child_params, sizeof(child_params));
		child_params.method = NGX_HTTP_GET;
		child_params.escape_uri = 1;

		if (conf->drm_request_uri != NULL)
		{
			if (ngx_http_complex_value(
				r,
				conf->drm_request_uri,
				&child_params.base_uri) != NGX_OK)
			{
				ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
					"ngx_http_vod_state_machine_get_drm_info: ngx_http_complex_value failed");
				return NGX_ERROR;
			}
		}
		else
		{
			child_params.base_uri = ctx->submodule_context.cur_sequence->stripped_uri;
		}

		ngx_perf_counter_start(ctx->perf_counter_context);

		rc = ngx_child_request_start(
			r,
			&ctx->child_request_buffers,
			ngx_http_vod_drm_info_request_finished,
			r,
			&conf->drm_upstream,
			&conf->child_request_location,
			&child_params,
			conf->drm_max_info_length,
			NULL);
		if (rc != NGX_AGAIN)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_state_machine_get_drm_info: async_reader failed %i", rc);
		}
		return rc;
	}

	return NGX_OK;
}

////// Common mp4 processing

static ngx_int_t 
ngx_http_vod_read_moov_atom(ngx_http_vod_ctx_t *ctx)
{
	u_char* new_read_buffer = NULL;
	media_clip_source_t* cur_source = *ctx->submodule_context.cur_source;
	const u_char* ftyp_ptr;
	size_t ftyp_size;
	size_t new_buffer_size;
	off_t absolute_moov_offset;
	vod_status_t rc;

	for (;;)
	{
		if (ctx->buffer_size <= (size_t)ctx->atom_start_offset)
		{
			ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_read_moov_atom: read buffer size %uz is smaller than the atom start offset %O", ctx->buffer_size, ctx->atom_start_offset);
			return ngx_http_vod_status_to_ngx_error(VOD_BAD_DATA);
		}

		if (ctx->ftyp_ptr == NULL)
		{
			// try to find the ftyp atom
			rc = mp4_parser_get_ftyp_atom_into(&ctx->submodule_context.request_context, ctx->read_buffer + ctx->atom_start_offset, ctx->buffer_size - ctx->atom_start_offset, &ftyp_ptr, &ftyp_size);
			if (rc != VOD_OK)
			{
				ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
					"ngx_http_vod_read_moov_atom: mp4_parser_get_ftyp_atom_into failed %i", rc);
				return ngx_http_vod_status_to_ngx_error(rc);
			}

			if (ftyp_size > 0 && 
				ftyp_ptr + ftyp_size <= ctx->read_buffer + ctx->buffer_size)
			{
				// got a full ftyp atom
				ctx->ftyp_ptr = ngx_pnalloc(ctx->submodule_context.request_context.pool, ftyp_size);
				if (ctx->ftyp_ptr == NULL)
				{
					ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
						"ngx_http_vod_read_moov_atom: ngx_pnalloc failed");
					return NGX_HTTP_INTERNAL_SERVER_ERROR;
				}

				ngx_memcpy(ctx->ftyp_ptr, ftyp_ptr, ftyp_size);
				ctx->ftyp_size = ftyp_size;
			}
			else
			{
				ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
					"ngx_http_vod_read_moov_atom: ftyp atom not found");
			}
		}

		// get moov atom offset and size
		rc = mp4_parser_get_moov_atom_info(&ctx->submodule_context.request_context, ctx->read_buffer + ctx->atom_start_offset, ctx->buffer_size - ctx->atom_start_offset, &ctx->moov_offset, &ctx->moov_size);
		if (rc != VOD_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_read_moov_atom: mp4_parser_get_moov_atom_info failed %i", rc);
			return ngx_http_vod_status_to_ngx_error(rc);
		}

		// update moov offset to be relative to the read buffer start
		ctx->moov_offset += ctx->atom_start_offset;

		// check whether we found the moov atom start
		if (ctx->moov_size > 0)
		{
			break;
		}

		if (ctx->moov_offset < (off_t)ctx->buffer_size)
		{
			ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_read_moov_atom: moov start offset %O is smaller than the buffer size %uz", ctx->moov_offset, ctx->buffer_size);
			return ngx_http_vod_status_to_ngx_error(VOD_UNEXPECTED);
		}

		if (ctx->moov_start_reads <= 0)
		{
			ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_read_moov_atom: exhausted all moov read attempts");
			return ngx_http_vod_status_to_ngx_error(VOD_BAD_DATA);
		}

		ctx->moov_start_reads--;

		// perform another read attempt
		absolute_moov_offset = ctx->read_offset + ctx->moov_offset;
		ctx->read_offset = absolute_moov_offset & (~(ctx->alignment - 1));
		ctx->atom_start_offset = absolute_moov_offset - ctx->read_offset;

		ngx_perf_counter_start(ctx->perf_counter_context);

		rc = ctx->async_reader(
			cur_source->reader_context,
			ctx->read_buffer, 
			ctx->submodule_context.conf->initial_read_size, 
			ctx->read_offset);
		if (rc < 0)		// inc. NGX_AGAIN
		{
			if (rc != NGX_AGAIN)
			{
				ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
					"ngx_http_vod_read_moov_atom: async_reader failed %i", rc);
			}
			return rc;
		}

		ngx_perf_counter_end(ctx->perf_counters, ctx->perf_counter_context, PC_READ_FILE);

		ctx->buffer_size = rc;
	}

	// check whether we already have the whole atom
	if (ctx->moov_offset + ctx->moov_size <= ctx->buffer_size)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
			"ngx_http_vod_read_moov_atom: already read the full moov atom");
		return NGX_OK;
	}

	// validate the moov size
	if (ctx->moov_size > ctx->submodule_context.conf->max_moov_size)
	{
		ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
			"ngx_http_vod_read_moov_atom: moov size %uD exceeds the max %uz", ctx->moov_size, ctx->submodule_context.conf->max_moov_size);
		return ngx_http_vod_status_to_ngx_error(VOD_BAD_DATA);
	}

	// calculate the new buffer size (round the moov end up to alignment)
	new_buffer_size = ((ctx->moov_offset + ctx->moov_size) + ctx->alignment - 1) & (~(ctx->alignment - 1));

	new_read_buffer = ngx_pmemalign(ctx->submodule_context.r->pool, new_buffer_size + 1, ctx->alignment);
	if (new_read_buffer == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
			"ngx_http_vod_read_moov_atom: ngx_pmemalign failed");
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	// copy the previously read data
	ngx_memcpy(new_read_buffer, ctx->read_buffer, ctx->buffer_size);
	ngx_pfree(ctx->submodule_context.r->pool, ctx->read_buffer);
	ctx->read_buffer = new_read_buffer;

	// read the rest of the atom
	ctx->submodule_context.request_context.log->action = "reading moov atom";
	ctx->state = STATE_PARSE_MOOV_READ_DATA;

	ngx_perf_counter_start(ctx->perf_counter_context);

	rc = ctx->async_reader(
		cur_source->reader_context,
		ctx->read_buffer + ctx->buffer_size, 
		new_buffer_size - ctx->buffer_size, 
		ctx->read_offset + ctx->buffer_size);
	if (rc < 0)
	{
		if (rc != NGX_AGAIN)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_read_moov_atom: async_reader failed %i", rc);
		}
		return rc;
	}

	ngx_perf_counter_end(ctx->perf_counters, ctx->perf_counter_context, PC_READ_FILE);

	ctx->buffer_size += rc;
	return NGX_OK;
}

static ngx_int_t 
ngx_http_vod_parse_moov_atom(ngx_http_vod_ctx_t *ctx, u_char* moov_buffer, size_t moov_size, ngx_flag_t fetched_from_cache)
{
	media_parse_params_t parse_params;
	mp4_base_metadata_t mp4_base_metadata;
	const ngx_http_vod_request_t* request = ctx->submodule_context.request;
	media_clip_source_t* cur_source = *ctx->submodule_context.cur_source;
	request_context_t* request_context = &ctx->submodule_context.request_context;
	segmenter_conf_t* segmenter = &ctx->submodule_context.conf->segmenter;
	get_clip_ranges_result_t clip_ranges;
	uint64_t last_segment_end;
	media_range_t range;
	vod_status_t rc;
	file_info_t file_info;
	uint32_t tracks_mask[MEDIA_TYPE_COUNT];
	uint32_t duration_millis;
	vod_fraction_t rate;

	if (request == NULL)
	{
		// Note: the other fields in parse_params are not required here
		parse_params.clip_from = cur_source->clip_from;
		parse_params.clip_to = cur_source->clip_to;

		rc = mp4_clipper_parse_moov(
			request_context,
			&parse_params,
			fetched_from_cache,
			moov_buffer,
			moov_size,
			&ctx->clipper_parse_result);
		if (rc != VOD_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, request_context->log, 0,
				"ngx_http_vod_parse_moov_atom: mp4_clipper_parse_moov failed %i", rc);
			return ngx_http_vod_status_to_ngx_error(rc);
		}

		return NGX_OK;
	}

	ngx_perf_counter_start(ctx->perf_counter_context);

	// init the parsing params
	parse_params.parse_type = request->parse_type;
	if (request->request_class == REQUEST_CLASS_MANIFEST && 
		ctx->submodule_context.media_set.total_clip_count <= 1)
	{
		parse_params.parse_type |= segmenter->parse_type;
	}

	tracks_mask[MEDIA_TYPE_VIDEO] = cur_source->tracks_mask[MEDIA_TYPE_VIDEO] & ctx->submodule_context.request_params.tracks_mask[MEDIA_TYPE_VIDEO];
	tracks_mask[MEDIA_TYPE_AUDIO] = cur_source->tracks_mask[MEDIA_TYPE_AUDIO] & ctx->submodule_context.request_params.tracks_mask[MEDIA_TYPE_AUDIO];
	parse_params.required_tracks_mask = tracks_mask;
	parse_params.clip_from = cur_source->clip_from;
	parse_params.clip_to = cur_source->clip_to;
	parse_params.sequence_offset = cur_source->sequence_offset;

	file_info.source = cur_source;
	file_info.uri = cur_source->uri;
	file_info.drm_info = cur_source->sequence->drm_info;

	// parse the basic metadata
	rc = mp4_parser_parse_basic_metadata(
		&ctx->submodule_context.request_context,
		&parse_params,
		moov_buffer,
		moov_size,
		&file_info,
		&mp4_base_metadata);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, request_context->log, 0,
			"ngx_http_vod_parse_moov_atom: mp4_parser_parse_basic_metadata failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(rc);
	}

	if (mp4_base_metadata.tracks.nelts == 0)
	{
		ngx_memzero(&cur_source->track_array, sizeof(cur_source->track_array));
		return VOD_OK;
	}

	if (request->request_class != REQUEST_CLASS_SEGMENT)
	{
		request_context->simulation_only = TRUE;

		parse_params.max_frame_count = 1024 * 1024;
		range.timescale = 1000;
		range.start = 0;
		if (cur_source->clip_to == UINT_MAX)
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
				rate.nom = 1;
				rate.denom = 1;
			}

			// get the last segment end
			if (cur_source->clip_to == UINT_MAX)
			{
				last_segment_end = ULLONG_MAX;
			}
			else
			{
				last_segment_end = ((cur_source->clip_to - cur_source->clip_from) * rate.denom) / rate.nom;
			}

			// get the start/end offsets
			duration_millis = rescale_time(mp4_base_metadata.duration * rate.denom, mp4_base_metadata.timescale * rate.nom, 1000);

			rc = segmenter_get_start_end_ranges_no_discontinuity(
				&ctx->submodule_context.request_context,
				segmenter,
				ctx->submodule_context.request_params.segment_index,
				&duration_millis,
				1,
				duration_millis,
				last_segment_end,
				&clip_ranges);
			if (rc != VOD_OK)
			{
				ngx_log_debug1(NGX_LOG_DEBUG_HTTP, request_context->log, 0,
					"ngx_http_vod_parse_moov_atom: segmenter_get_start_end_ranges_no_discontinuity failed %i", rc);
				return ngx_http_vod_status_to_ngx_error(rc);
			}

			if (clip_ranges.clip_count == 0)
			{
				ngx_memzero(&cur_source->track_array, sizeof(cur_source->track_array));
				return VOD_OK;
			}

			parse_params.range = clip_ranges.clip_ranges;
			parse_params.range->start = (parse_params.range->start * rate.nom) / rate.denom;
			if (parse_params.range->end != ULLONG_MAX)
			{
				parse_params.range->end = (parse_params.range->end * rate.nom) / rate.denom;
			}
		}
	}

	// parse the frames
	rc = mp4_parser_parse_frames(
		&ctx->submodule_context.request_context,
		&mp4_base_metadata,
		&parse_params,
		segmenter->align_to_key_frames,
		&cur_source->track_array);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, request_context->log, 0,
			"ngx_http_vod_parse_moov_atom: mp4_parser_parse_frames failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(rc);
	}

	ngx_perf_counter_end(ctx->perf_counters, ctx->perf_counter_context, PC_MP4_PARSE);

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_state_machine_parse_moov_atoms(ngx_http_vod_ctx_t *ctx)
{
	ngx_http_vod_loc_conf_t* conf = ctx->submodule_context.conf;
	media_clip_source_t* cur_source;
	ngx_http_request_t* r = ctx->submodule_context.r;
	ngx_str_t cache_buffers[3];
	u_char* cache_buffer;
	size_t cache_size;
	u_char* moov_buffer;
	size_t moov_size;
	u_char* uncomp_buffer;
	ngx_int_t rc;

	for (;
		ctx->submodule_context.cur_source < ctx->submodule_context.media_set.sources_end;
		ctx->submodule_context.cur_source++)
	{
		cur_source = *ctx->submodule_context.cur_source;

		switch (ctx->state)
		{
		case STATE_PARSE_MOOV_INITIAL:
			if (conf->moov_cache_zone != NULL)
			{
				// try to read the moov atom from cache
				if (ngx_buffer_cache_fetch_perf(ctx->perf_counters, conf->moov_cache_zone, cur_source->file_key, &cache_buffer, &cache_size) &&
					cache_size > sizeof(size_t))
				{
					ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
						"ngx_http_vod_state_machine_parse_moov_atoms: moov atom cache hit, size is %uz", cache_size);

					ctx->ftyp_size = *(size_t*)cache_buffer;
					ctx->ftyp_ptr = cache_buffer + sizeof(ctx->ftyp_size);

					moov_buffer = ctx->ftyp_ptr + ctx->ftyp_size;
					moov_size = cache_size - ctx->ftyp_size - sizeof(ctx->ftyp_size);

					rc = ngx_http_vod_parse_moov_atom(ctx, moov_buffer, moov_size, 1);
					if (rc != NGX_OK)
					{
						ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
							"ngx_http_vod_state_machine_parse_moov_atoms: ngx_http_vod_parse_moov_atom failed %i", rc);
						return rc;
					}
					continue;
				}
				else
				{
					ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
						"ngx_http_vod_state_machine_parse_moov_atoms: moov atom cache miss");
				}
			}

			// open the file
			ctx->state = STATE_PARSE_MOOV_OPEN_FILE;

			rc = ctx->open_file(r, &cur_source->mapped_uri, &cur_source->reader_context);
			if (rc != NGX_OK)
			{
				if (rc != NGX_AGAIN && rc != NGX_DONE)
				{
					ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
						"ngx_http_vod_state_machine_parse_moov_atoms: open_file failed %i", rc);
				}
				return rc;
			}
			// fallthrough

		case STATE_PARSE_MOOV_OPEN_FILE:
			// allocate the initial read buffer
			ctx->read_buffer = ngx_pmemalign(r->pool, conf->initial_read_size + 1, ctx->alignment);
			if (ctx->read_buffer == NULL)
			{
				ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
					"ngx_http_vod_state_machine_parse_moov_atoms: ngx_pmemalign failed");
				return NGX_HTTP_INTERNAL_SERVER_ERROR;
			}

			// read the file header
			r->connection->log->action = "reading mp4 header";
			ctx->moov_start_reads = MAX_MOOV_START_READS;
			ctx->state = STATE_PARSE_MOOV_READ_HEADER;

			ngx_perf_counter_start(ctx->perf_counter_context);

			rc = ctx->async_reader(cur_source->reader_context, ctx->read_buffer, conf->initial_read_size, 0);
			if (rc < 0)		// inc. NGX_AGAIN
			{
				if (rc != NGX_AGAIN)
				{
					ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
						"ngx_http_vod_state_machine_parse_moov_atoms: async_reader failed %i", rc);
				}
				return rc;
			}

			// read completed synchronously
			ngx_perf_counter_end(ctx->perf_counters, ctx->perf_counter_context, PC_READ_FILE);
			ctx->buffer_size = rc;
			// fallthrough

		case STATE_PARSE_MOOV_READ_HEADER:
			// read the entire atom
			rc = ngx_http_vod_read_moov_atom(ctx);
			if (rc != NGX_OK)
			{
				if (rc != NGX_AGAIN)
				{
					ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
						"ngx_http_vod_state_machine_parse_moov_atoms: ngx_http_vod_read_moov_atom failed %i", rc);
				}
				return rc;
			}
			// fallthrough

		case STATE_PARSE_MOOV_READ_DATA:
			// make sure we got the whole moov atom
			if (ctx->buffer_size < ctx->moov_offset + ctx->moov_size)
			{
				ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
					"ngx_http_vod_state_machine_parse_moov_atoms: buffer size %uD is smaller than moov end offset %uD", ctx->buffer_size, ctx->moov_offset + ctx->moov_size);
				return ngx_http_vod_status_to_ngx_error(VOD_BAD_DATA);
			}

			// uncompress the moov atom if needed
			rc = mp4_parser_uncompress_moov(
				&ctx->submodule_context.request_context,
				ctx->read_buffer + ctx->moov_offset,
				ctx->moov_size,
				conf->max_moov_size,
				&uncomp_buffer,
				&ctx->moov_offset, 
				&ctx->moov_size);
			if (rc != VOD_OK)
			{
				ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
					"ngx_http_vod_state_machine_parse_moov_atoms: mp4_parser_uncompress_moov failed %i", rc);
				return ngx_http_vod_status_to_ngx_error(rc);
			}

			if (uncomp_buffer != NULL)
			{
				// free the compressed buffer
				ngx_pfree(ctx->submodule_context.r->pool, ctx->read_buffer);

				// replace the compressed buffer with the uncompressed
				ctx->read_buffer = uncomp_buffer;
			}

			// parse the moov atom
			rc = ngx_http_vod_parse_moov_atom(ctx, ctx->read_buffer + ctx->moov_offset, ctx->moov_size, 0);
			if (rc != NGX_OK)
			{
				ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
					"ngx_http_vod_state_machine_parse_moov_atoms: ngx_http_vod_parse_moov_atom failed %i", rc);
				return rc;
			}

			// save the moov atom to cache
			if (conf->moov_cache_zone != NULL)
			{
				cache_buffers[0].data = (u_char*)&ctx->ftyp_size;
				cache_buffers[0].len = sizeof(ctx->ftyp_size);
				cache_buffers[1].data = ctx->ftyp_ptr;
				cache_buffers[1].len = ctx->ftyp_size;
				cache_buffers[2].data = ctx->read_buffer + ctx->moov_offset;
				cache_buffers[2].len = ctx->moov_size;

				if (ngx_buffer_cache_store_gather_perf(
					ctx->perf_counters,
					conf->moov_cache_zone,
					cur_source->file_key,
					cache_buffers,
					3))
				{
					ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
						"ngx_http_vod_state_machine_parse_moov_atoms: stored moov atom in cache");
				}
				else
				{
					ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
						"ngx_http_vod_state_machine_parse_moov_atoms: failed to store moov atom in cache");
				}
			}

			if (ctx->submodule_context.request != NULL)
			{
				// no longer need the moov atom buffer
				ngx_pfree(ctx->submodule_context.r->pool, ctx->read_buffer);
				ctx->read_buffer = NULL;
			}

			// reset the state
			ctx->state = STATE_PARSE_MOOV_INITIAL;
			ctx->buffer_size = 0;
			ctx->read_offset = 0;
			ctx->atom_start_offset = 0;
			break;

		default:
			ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_state_machine_parse_moov_atoms: invalid state %d", ctx->state);
			return NGX_HTTP_INTERNAL_SERVER_ERROR;
		}
	}

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_validate_streams(ngx_http_vod_ctx_t *ctx)
{
	if (ctx->submodule_context.media_set.total_track_count == 0)
	{
		if (ctx->submodule_context.request->request_class == REQUEST_CLASS_SEGMENT)
		{
			ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_validate_streams: no matching streams were found, probably invalid segment index");
			return NGX_HTTP_NOT_FOUND;
		}
		else
		{
			ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_validate_streams: no matching streams were found");
			return NGX_HTTP_BAD_REQUEST;
		}
	}
	
	if ((ctx->submodule_context.request->flags & (REQUEST_FLAG_SINGLE_TRACK_PER_MEDIA_TYPE | REQUEST_FLAG_SINGLE_TRACK)) != 0)
	{
		if (ctx->submodule_context.media_set.sequence_count != 1)
		{
			ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_validate_streams: request has more than one sequence while only one is supported");
			return NGX_HTTP_BAD_REQUEST;
		}

		if ((ctx->submodule_context.request->flags & REQUEST_FLAG_SINGLE_TRACK) != 0 &&
			ctx->submodule_context.media_set.sequences[0].total_track_count != 1)
		{
			ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_validate_streams: got %ui streams while only a single stream is supported",
				ctx->submodule_context.media_set.sequences[0].total_track_count);
			return NGX_HTTP_BAD_REQUEST;
		}

		if (ctx->submodule_context.media_set.sequences[0].track_count[MEDIA_TYPE_VIDEO] > 1 ||
			ctx->submodule_context.media_set.sequences[0].track_count[MEDIA_TYPE_AUDIO] > 1)
		{
			ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_validate_streams: one stream at most per media type is allowed video=%uD audio=%uD",
				ctx->submodule_context.media_set.sequences[0].track_count[MEDIA_TYPE_VIDEO],
				ctx->submodule_context.media_set.sequences[0].track_count[MEDIA_TYPE_AUDIO]);
			return NGX_HTTP_BAD_REQUEST;
		}
	}
	
	return NGX_OK;
}

////// Metadata request handling

static ngx_int_t
ngx_http_vod_handle_metadata_request(ngx_http_vod_ctx_t *ctx)
{
	ngx_http_vod_loc_conf_t* conf;
	ngx_str_t cache_buffers[3];
	ngx_str_t content_type;
	ngx_str_t response;
	ngx_int_t rc;

	ngx_perf_counter_start(ctx->perf_counter_context);

	rc = ctx->submodule_context.request->handle_metadata_request(
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
	if (conf->response_cache_zone != NULL)
	{
		cache_buffers[0].data = (u_char*)&content_type.len;
		cache_buffers[0].len = sizeof(content_type.len);
		cache_buffers[1] = content_type;
		cache_buffers[2] = response;

		if (ngx_buffer_cache_store_gather_perf(ctx->perf_counters, conf->response_cache_zone, ctx->request_key, cache_buffers, 3))
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
	
	rc = ngx_http_vod_send_header(ctx->submodule_context.r, response.len, &content_type);
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

	for (;
		ctx->submodule_context.cur_source < ctx->submodule_context.media_set.sources_end;
		ctx->submodule_context.cur_source++)
	{
		cur_source = *ctx->submodule_context.cur_source;

		// open the file if not already opened
		if (cur_source->reader_context != NULL)
		{
			continue;
		}

		path = &cur_source->mapped_uri;

		rc = ctx->open_file(ctx->submodule_context.r, path, &cur_source->reader_context);
		if (rc != NGX_OK)
		{
			if (rc != NGX_AGAIN && rc != NGX_DONE)
			{
				ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
					"ngx_http_vod_state_machine_open_files: open_file failed %i", rc);
			}
			return rc;
		}
	}

	return NGX_OK;
}

static void
ngx_http_vod_enable_directio(ngx_http_vod_ctx_t *ctx)
{
	media_clip_source_t** cur_source_ptr;

	for (cur_source_ptr = ctx->submodule_context.media_set.sources;
		cur_source_ptr < ctx->submodule_context.media_set.sources_end;
		cur_source_ptr++)
	{
		ngx_file_reader_enable_directio(cur_source_ptr[0]->reader_context);
	}
}

static vod_status_t
ngx_http_vod_write_segment_header_buffer(void* ctx, u_char* buffer, uint32_t size, bool_t* reuse_buffer)
{
	ngx_http_vod_write_segment_context_t* context = (ngx_http_vod_write_segment_context_t*)ctx;
	ngx_chain_t *chain;
	ngx_buf_t *b;

	if (context->r->header_sent)
	{
		ngx_log_error(NGX_LOG_ERR, context->r->connection->log, 0,
			"ngx_http_vod_write_segment_header_buffer: called after the headers were already sent");
		return VOD_UNEXPECTED;
	}

	// caller should not reuse the buffer since we keep the original buffer
	*reuse_buffer = FALSE;
	
	b = ngx_calloc_buf(context->r->pool);
	if (b == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, context->r->connection->log, 0,
			"ngx_http_vod_write_segment_header_buffer: ngx_pcalloc failed (1)");
		return VOD_ALLOC_FAILED;
	}

	b->pos = buffer;
	b->last = buffer + size;
	b->temporary = 1;

	chain = ngx_alloc_chain_link(context->r->pool);
	if (chain == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, context->r->connection->log, 0,
			"ngx_http_vod_write_segment_header_buffer: ngx_pcalloc failed (2)");
		return VOD_ALLOC_FAILED;
	}

	chain->buf = context->chain_head->buf;
	chain->next = context->chain_head->next;

	context->chain_head->buf = b;
	context->chain_head->next = chain;

	context->total_size += size;

	return VOD_OK;
}

static vod_status_t 
ngx_http_vod_write_segment_buffer(void* ctx, u_char* buffer, uint32_t size, bool_t* reuse_buffer)
{
	ngx_http_vod_write_segment_context_t* context = (ngx_http_vod_write_segment_context_t*)ctx;
	ngx_buf_t *b;
	ngx_chain_t *chain;
	ngx_chain_t out;
	ngx_int_t rc;

	// caller should not reuse the buffer since we keep the original buffer
	*reuse_buffer = FALSE;

	// create a wrapping ngx_buf_t
	b = ngx_calloc_buf(context->r->pool);
	if (b == NULL) 
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, context->r->connection->log, 0,
			"ngx_http_vod_write_segment_buffer: ngx_pcalloc failed (1)");
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
					"ngx_http_vod_write_segment_buffer: ngx_pcalloc failed (2)");
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
	bool_t reuse_buffer;
	ngx_int_t rc;

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

	rc = ctx->submodule_context.request->init_frame_processor(
		&ctx->submodule_context,
		&ctx->read_cache_state,
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

	rc = read_cache_allocate_buffer_slots(&ctx->read_cache_state, 0);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_init_frame_processing: read_cache_allocate_buffer_slots failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(rc);
	}

	ngx_perf_counter_end(ctx->perf_counters, ctx->perf_counter_context, PC_INIT_FRAME_PROCESS);

	r->headers_out.content_type_len = content_type.len;
	r->headers_out.content_type.len = content_type.len;
	r->headers_out.content_type.data = content_type.data;

	// if the frame processor can't determine the size in advance we have to build the whole response before we can start sending it
	if (ctx->content_length == 0)
	{
		return NGX_OK;
	}

	// send the response header
	rc = ngx_http_vod_send_header(r, ctx->content_length, NULL);
	if (rc != NGX_OK)
	{
		return rc;
	}

	if (r->header_only || r->method == NGX_HTTP_HEAD)
	{
		return NGX_DONE;
	}

	// write the initial buffer if provided
	if (output_buffer.len != 0)
	{
		rc = segment_writer.write_tail(segment_writer.context, output_buffer.data, output_buffer.len, &reuse_buffer);
		if (rc != VOD_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_init_frame_processing: write_callback failed %i", rc);
			return ngx_http_vod_status_to_ngx_error(rc);
		}
	}

	return NGX_OK;
}

static ngx_int_t 
ngx_http_vod_process_mp4_frames(ngx_http_vod_ctx_t *ctx)
{
	media_clip_source_t* source;
	uint64_t read_offset;
	u_char* read_buffer;
	uint32_t read_size;
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
				"ngx_http_vod_process_mp4_frames: frame_processor failed %i", rc);
			return ngx_http_vod_status_to_ngx_error(rc);
		}

		// get a buffer to read into
		rc = read_cache_get_read_buffer(&ctx->read_cache_state, (void**)&source, &read_offset, &read_buffer, &read_size);
		if (rc != VOD_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_process_mp4_frames: read_cache_get_read_buffer failed %i", rc);
			return ngx_http_vod_status_to_ngx_error(rc);
		}

		// perform the read
		ngx_perf_counter_start(ctx->perf_counter_context);

		rc = ctx->async_reader(source->reader_context, read_buffer, read_size, read_offset);
		if (rc < 0)
		{
			if (rc != NGX_AGAIN)
			{
				ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
					"ngx_http_vod_process_mp4_frames: async_reader failed %i", rc);
			}
			return rc;
		}

		ngx_perf_counter_end(ctx->perf_counters, ctx->perf_counter_context, PC_READ_FILE);

		// read completed synchronously, update the read cache
		read_cache_read_completed(&ctx->read_cache_state, rc);
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
	ctx->write_segment_buffer_context.chain_end->next = NULL;
	ctx->write_segment_buffer_context.chain_end->buf->last_buf = 1;

	// send the response header
	rc = ngx_http_vod_send_header(r, ctx->write_segment_buffer_context.total_size, NULL);
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
ngx_http_vod_process_init(ngx_cycle_t *cycle)
{
	audio_filter_process_init(cycle->log);
	return NGX_OK;
}

////// Clipping

static ngx_int_t
ngx_http_vod_send_clip_header(ngx_http_vod_ctx_t *ctx)
{
	ngx_http_request_t* r = ctx->submodule_context.r;
	ngx_chain_t* out;
	size_t response_size;
	ngx_int_t rc;
	off_t range_start;
	off_t range_end;
	off_t header_size;
	off_t mdat_size;

	rc = mp4_clipper_build_header(
		&ctx->submodule_context.request_context,
		ctx->ftyp_ptr,
		ctx->ftyp_size,
		&ctx->clipper_parse_result,
		&out,
		&response_size);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_send_clip_header: mp4_clipper_build_header failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(rc);
	}

	// send the response header
	rc = ngx_http_vod_send_header(r, response_size, &mp4_content_type);
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

		mdat_size = ctx->clipper_parse_result.max_last_offset - ctx->clipper_parse_result.min_first_offset;
		header_size = response_size - mdat_size;

		if (range_end < header_size)
		{
			ctx->clipper_parse_result.max_last_offset = 0;
		}
		else if (mdat_size > range_end - header_size)
		{
			ctx->clipper_parse_result.max_last_offset = ctx->clipper_parse_result.min_first_offset + range_end - header_size;
		}

		if (range_start > header_size)
		{
			ctx->clipper_parse_result.min_first_offset += range_start - header_size;
		}
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

		ctx->state = STATE_PARSE_MOOV_INITIAL;
		ctx->submodule_context.cur_sequence = ctx->submodule_context.media_set.sequences;
		// fallthrough

	case STATE_PARSE_MOOV_INITIAL:
	case STATE_PARSE_MOOV_OPEN_FILE:
	case STATE_PARSE_MOOV_READ_HEADER:
	case STATE_PARSE_MOOV_READ_DATA:

		rc = ngx_http_vod_state_machine_parse_moov_atoms(ctx);
		if (rc != NGX_OK)
		{
			return rc;
		}

		if (ctx->submodule_context.request == NULL)
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
				&ctx->submodule_context.media_set);
			if (rc != VOD_OK)
			{
				ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
					"ngx_http_vod_run_state_machine: filter_init_filtered_clips failed %i", rc);
				return ngx_http_vod_status_to_ngx_error(rc);
			}

			rc = ngx_http_vod_validate_streams(ctx);
			if (rc != NGX_OK)
			{
				return rc;
			}

			// handle metadata requests
			if (ctx->submodule_context.request->handle_metadata_request != NULL)
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
		ctx->submodule_context.cur_source = ctx->submodule_context.media_set.sources;
		// fallthrough

	case STATE_OPEN_FILE:
		rc = ngx_http_vod_state_machine_open_files(ctx);
		if (rc != NGX_OK)
		{
			return rc;
		}

		// enable directio if enabled in the configuration (ignore errors)
		// Note that directio is set on transfer only to allow the kernel to cache the "moov" atom
		if (ctx->submodule_context.conf->request_handler != ngx_http_vod_remote_request_handler)
		{
			ngx_http_vod_enable_directio(ctx);
		}

		if (ctx->submodule_context.request == NULL)
		{
			if (ctx->clipper_parse_result.min_first_offset < ctx->clipper_parse_result.max_last_offset)
			{
				ctx->submodule_context.cur_source = ctx->submodule_context.media_set.sources;

				ctx->state = STATE_DUMP_FILE_PART;

				rc = ctx->dump_part(
					ctx->submodule_context.cur_source[0]->reader_context,
					ctx->clipper_parse_result.min_first_offset,
					ctx->clipper_parse_result.max_last_offset);
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
			ctx->submodule_context.cur_source = ctx->submodule_context.media_set.sources;

			rc = filter_init_state(
				&ctx->submodule_context.request_context,
				&ctx->read_cache_state,
				&ctx->submodule_context.media_set, 
				&ctx->frame_processor_state);
			if (rc != VOD_OK)
			{
				ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
					"ngx_http_vod_run_state_machine: filter_init_state failed %i", rc);
				return ngx_http_vod_status_to_ngx_error(rc);
			}

			ctx->frame_processor = filter_run_state_machine;
		}

		// fallthrough

	case STATE_FILTER_FRAMES:
		// if audio filtering already started, process frames
		if (ctx->frame_processor != NULL)
		{
			rc = ngx_http_vod_process_mp4_frames(ctx);
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

		ctx->submodule_context.request_context.log->action = "processing frames";
		ctx->state = STATE_PROCESS_FRAMES;
		// fallthrough

	case STATE_PROCESS_FRAMES:
		rc = ngx_http_vod_process_mp4_frames(ctx);
		if (rc != NGX_OK)
		{
			if (rc != NGX_AGAIN)
			{
				ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
					"ngx_http_vod_run_state_machine: ngx_http_vod_process_mp4_frames failed %i", rc);
			}
			return rc;
		}

		return ngx_http_vod_finalize_segment_response(ctx);

	case STATE_DUMP_OPEN_FILE:
		return ctx->dump_request(ctx);

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

	ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
		"ngx_http_vod_run_state_machine: invalid state %d", ctx->state);
	return NGX_HTTP_INTERNAL_SERVER_ERROR;
}

static void
ngx_http_vod_handle_read_completed(void* context, ngx_int_t rc, ssize_t bytes_read)
{
	ngx_http_vod_ctx_t *ctx = (ngx_http_vod_ctx_t *)context;
	ssize_t expected_size;

	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
			"ngx_http_vod_handle_read_completed: read failed %i", rc);
		goto finalize_request;
	}

	if (ctx->state == STATE_DUMP_FILE_PART)
	{
		expected_size = ctx->clipper_parse_result.max_last_offset - ctx->clipper_parse_result.min_first_offset;
		if (bytes_read != expected_size)
		{
			ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_handle_read_completed: read size %z different than expected %z, probably a truncated file", 
				bytes_read, expected_size);
			rc = ngx_http_vod_status_to_ngx_error(VOD_BAD_DATA);
			goto finalize_request;
		}
	}
	else if (bytes_read <= 0)
	{
		ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
			"ngx_http_vod_handle_read_completed: bytes read is zero");
		rc = ngx_http_vod_status_to_ngx_error(VOD_BAD_DATA);
		goto finalize_request;
	}

	ngx_perf_counter_end(ctx->perf_counters, ctx->perf_counter_context, PC_ASYNC_READ_FILE);

	// update the bytes read
	switch (ctx->state)
	{
	case STATE_PARSE_MOOV_READ_HEADER:
		ctx->buffer_size = bytes_read;
		break;

	case STATE_PARSE_MOOV_READ_DATA:
		ctx->buffer_size += bytes_read;
		break;

	case STATE_FILTER_FRAMES:
	case STATE_PROCESS_FRAMES:
		read_cache_read_completed(&ctx->read_cache_state, bytes_read);
		break;
	}

	// run the state machine
	rc = ngx_http_vod_run_state_machine(ctx);
	if (rc == NGX_AGAIN)
	{
		return;
	}

finalize_request:

	ngx_http_vod_finalize_request(ctx, rc);
}

static void
ngx_http_vod_init_uri_key(ngx_http_vod_loc_conf_t* conf, media_sequence_t* cur_sequence, ngx_str_t* prefix)
{
	ngx_md5_t md5;

	ngx_md5_init(&md5);
	if (prefix != NULL)
	{
		ngx_md5_update(&md5, prefix->data, prefix->len);
	}
	ngx_md5_update(&md5, cur_sequence->stripped_uri.data, cur_sequence->stripped_uri.len);
	ngx_md5_final(cur_sequence->uri_key, &md5);
}

static void
ngx_http_vod_init_file_key(ngx_http_vod_loc_conf_t* conf, media_clip_source_t* cur_source, ngx_str_t* prefix)
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
ngx_http_vod_start_processing_mp4_file(ngx_http_request_t *r)
{
	ngx_http_vod_loc_conf_t* conf;
	media_clip_source_t** cur_source_ptr;
	media_clip_source_t* cur_source;
	ngx_http_vod_ctx_t *ctx;
	ngx_int_t rc;

	// update request flags
	r->root_tested = !r->error_page;
	r->allow_ranges = 1;

	// handle serve requests
	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);
	if (ctx->submodule_context.request == NULL &&
		ctx->submodule_context.media_set.sources[0]->clip_from == 0 &&
		ctx->submodule_context.media_set.sources[0]->clip_to == UINT_MAX)
	{
		ctx->state = STATE_DUMP_OPEN_FILE;
		ctx->submodule_context.cur_source = ctx->submodule_context.media_set.sources;
		cur_source = *ctx->submodule_context.cur_source;

		rc = ctx->open_file(r, &cur_source->mapped_uri, &cur_source->reader_context);
		if (rc != NGX_OK)
		{
			if (rc != NGX_AGAIN && rc != NGX_DONE)
			{
				ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
					"ngx_http_vod_start_processing_mp4_file: open_file failed %i", rc);
			}
			return rc;
		}

		return ctx->dump_request(ctx);
	}

	// initialize the file keys
	conf = ctx->submodule_context.conf;

	for (cur_source_ptr = ctx->submodule_context.media_set.sources;
		cur_source_ptr < ctx->submodule_context.media_set.sources_end;
		cur_source_ptr++)
	{
		ngx_http_vod_init_file_key(conf, *cur_source_ptr, ctx->file_key_prefix);
	}

	// initialize the uri / encryption keys
	if (conf->drm_enabled || conf->secret_key != NULL)
	{
		for (ctx->submodule_context.cur_sequence = ctx->submodule_context.media_set.sequences;
			ctx->submodule_context.cur_sequence < ctx->submodule_context.media_set.sequences_end;
			ctx->submodule_context.cur_sequence++)
		{
			if (conf->drm_enabled)
			{
				ngx_http_vod_init_uri_key(conf, ctx->submodule_context.cur_sequence, ctx->file_key_prefix);
			}

			rc = ngx_http_vod_init_encryption_key(r, conf, ctx->submodule_context.cur_sequence);
			if (rc != NGX_OK)
			{
				return rc;
			}
		}
	}

	// restart the file index/uri params
	ctx->submodule_context.cur_source = ctx->submodule_context.media_set.sources;

	if (ctx->submodule_context.conf->drm_enabled)
	{
		ctx->state = STATE_READ_DRM_INFO;
		ctx->submodule_context.cur_sequence = ctx->submodule_context.media_set.sequences;
	}
	else
	{
		ctx->state = STATE_PARSE_MOOV_INITIAL;
	}

	return ngx_http_vod_run_state_machine(ctx);
}

////// Mapped & remote modes

static ngx_int_t
ngx_http_vod_init_upstream_vars(ngx_http_vod_ctx_t *ctx)
{
	if (ctx->upstream_extra_args.len != 0 || 
		ctx->submodule_context.conf->upstream_extra_args == NULL)
	{
		return NGX_OK;
	}

	if (ngx_http_complex_value(
		ctx->submodule_context.r, 
		ctx->submodule_context.conf->upstream_extra_args, 
		&ctx->upstream_extra_args) != NGX_OK)
	{
		return NGX_ERROR;
	}

	return NGX_OK;
}

////// Local & mapped modes

static ngx_int_t
ngx_http_vod_dump_request_to_fallback(ngx_http_request_t *r)
{
	ngx_http_vod_loc_conf_t* conf;
	ngx_child_request_params_t child_params;
	ngx_int_t rc;

	conf = ngx_http_get_module_loc_conf(r, ngx_http_vod_module);

	if (conf->fallback_upstream.upstream == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_dump_request_to_fallback: no fallback configured");
		return NGX_ERROR;
	}

	if (ngx_http_vod_header_exists(r, &conf->proxy_header_name))
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_dump_request_to_fallback: proxy header exists");
		return NGX_ERROR;
	}

	// dump the request to the fallback upstream
	ngx_memzero(&child_params, sizeof(child_params));
	child_params.method = r->method;
	child_params.base_uri = r->unparsed_uri;
	child_params.extra_headers = conf->proxy_header;
	child_params.proxy_range = 1;
	child_params.proxy_accept_encoding = 1;

	rc = ngx_dump_request(
		r,
		&conf->fallback_upstream,
		&child_params);
	return rc;
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
			if (rc != NGX_DONE)
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
	rc = ngx_http_vod_run_state_machine(ctx);
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
	return ngx_http_vod_file_open_completed_internal(context, rc, 0);
}

static void
ngx_http_vod_file_open_completed_with_fallback(void* context, ngx_int_t rc)
{
	return ngx_http_vod_file_open_completed_internal(context, rc, 1);
}
#endif // NGX_THREADS

static ngx_int_t
ngx_http_vod_init_file_reader_internal(ngx_http_request_t *r, ngx_str_t* path, void** context, ngx_flag_t fallback)
{
	ngx_file_reader_state_t* state;
	ngx_http_core_loc_conf_t *clcf;
	ngx_http_vod_ctx_t *ctx;
	ngx_int_t rc;

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);

	clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

	state = ngx_pcalloc(r->pool, sizeof(*state));
	if (state == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_init_file_reader_internal: ngx_pcalloc failed");
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
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
			path);
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
			path);
#if (NGX_THREADS)
	}
#endif
	if (rc != NGX_OK)
	{
		if (fallback && rc == NGX_HTTP_NOT_FOUND)
		{
			// try the fallback
			rc = ngx_http_vod_dump_request_to_fallback(r);
			if (rc != NGX_DONE)
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
ngx_http_vod_init_file_reader(ngx_http_request_t *r, ngx_str_t* path, void** context)
{
	return ngx_http_vod_init_file_reader_internal(r, path, context, 0);
}

static ngx_int_t
ngx_http_vod_init_file_reader_with_fallback(ngx_http_request_t *r, ngx_str_t* path, void** context)
{
	return ngx_http_vod_init_file_reader_internal(r, path, context, 1);
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
	ngx_file_reader_state_t* state = ctx->submodule_context.cur_source[0]->reader_context;
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
	rc = ngx_http_vod_send_header(r, state->file_size, NULL);
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

////// Local mode only

ngx_int_t
ngx_http_vod_local_request_handler(ngx_http_request_t *r)
{
	media_clip_source_t** cur_source_ptr;
	media_clip_source_t* cur_source;
	ngx_http_core_loc_conf_t *clcf;
	ngx_http_vod_ctx_t *ctx;
	ngx_str_t original_uri;
	ngx_int_t rc;
	u_char *last;
	size_t root;
	ngx_str_t path;

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);

	// map all uris to paths
	original_uri = r->uri;
	for (cur_source_ptr = ctx->submodule_context.media_set.sources;
		cur_source_ptr < ctx->submodule_context.media_set.sources_end;
		cur_source_ptr++)
	{
		cur_source = *cur_source_ptr;

		r->uri = cur_source->stripped_uri;
		last = ngx_http_map_uri_to_path(r, &path, &root, 0);
		r->uri = original_uri;
		if (last == NULL)
		{
			ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_local_request_handler: ngx_http_map_uri_to_path failed");
			return NGX_HTTP_INTERNAL_SERVER_ERROR;
		}

		path.len = last - path.data;

		cur_source->mapped_uri = path;
	}

	// initialize for reading files
	clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);
	ctx->alignment = clcf->directio_alignment;

	ctx->open_file = ngx_http_vod_init_file_reader_with_fallback;
	ctx->async_reader = (ngx_http_vod_async_read_func_t)ngx_async_file_read;
	ctx->dump_part = (ngx_http_vod_dump_part_t)ngx_file_reader_dump_file_part;
	ctx->dump_request = ngx_http_vod_dump_file;

	// start the state machine
	rc = ngx_http_vod_start_processing_mp4_file(r);
	if (rc != NGX_AGAIN && rc != NGX_DONE && rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_local_request_handler: ngx_http_vod_start_processing_mp4_file failed %i", rc);
	}

	return rc;
}

////// Mapped mode only

static void ngx_http_vod_path_request_finished(void* context, ngx_int_t rc, off_t content_length, ngx_buf_t* response);

static ngx_int_t
ngx_http_vod_apply_mapping(ngx_http_vod_ctx_t *ctx, ngx_str_t* mapping)
{
	ngx_http_vod_loc_conf_t* conf = ctx->submodule_context.conf;
	media_clip_source_t *cur_source = *ctx->submodule_context.cur_source;
	media_clip_source_t* mapped_source;
	media_set_t mapped_media_set;
	ngx_str_t path;
	ngx_int_t rc;
	bool_t parse_all_clips;

	// optimization for the case of simple mapping response
	if (mapping->len >= conf->path_response_prefix.len + conf->path_response_postfix.len &&
		ngx_memcmp(mapping->data, conf->path_response_prefix.data, conf->path_response_prefix.len) == 0 &&
		ngx_memcmp(mapping->data + mapping->len - conf->path_response_postfix.len, 
			conf->path_response_postfix.data, conf->path_response_postfix.len) == 0 &&
		memchr(mapping->data + conf->path_response_prefix.len, '"', 
			mapping->len - conf->path_response_prefix.len - conf->path_response_postfix.len) == NULL)
	{
		path.len = mapping->len - conf->path_response_prefix.len - conf->path_response_postfix.len;
		if (path.len <= 0)
		{
			// file not found
			ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_apply_mapping: empty path returned from upstream %V",
				&cur_source->stripped_uri);

			// try the fallback
			rc = ngx_http_vod_dump_request_to_fallback(ctx->submodule_context.r);
			if (rc != NGX_DONE)
			{
				rc = NGX_HTTP_NOT_FOUND;
			}
			return rc;
		}

		// copy the path to make it null terminated
		path.data = ngx_palloc(ctx->submodule_context.request_context.pool, path.len + 1);
		if (path.data == NULL)
		{
			ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_apply_mapping: ngx_palloc failed");
			return NGX_HTTP_INTERNAL_SERVER_ERROR;
		}
		ngx_memcpy(path.data, mapping->data + conf->path_response_prefix.len, path.len);
		path.data[path.len] = '\0';

		// move to the next suburi
		cur_source->sequence->mapped_uri = path;
		cur_source->mapped_uri = path;

		return NGX_OK;
	}

	// TODO: in case the new media set may replace the existing one, propagate clip from, clip to, rate

	parse_all_clips = ctx->submodule_context.request != NULL ? (ctx->submodule_context.request->parse_type & PARSE_FLAG_ALL_CLIPS) : 0;

	rc = media_set_parse_json(
		&ctx->submodule_context.request_context,
		mapping->data,
		&ctx->submodule_context.request_params,
		&ctx->submodule_context.conf->segmenter,
		&cur_source->uri,
		parse_all_clips,
		&mapped_media_set);

	if (rc == VOD_NOT_FOUND)
	{
		// file not found, try the fallback
		rc = ngx_http_vod_dump_request_to_fallback(ctx->submodule_context.r);
		if (rc != NGX_DONE)
		{
			rc = NGX_HTTP_NOT_FOUND;
		}
		return rc;
	}

	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
			"ngx_http_vod_apply_mapping: media_set_parse_json failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(rc);
	}

	if (mapped_media_set.sequence_count == 1 &&
		mapped_media_set.durations == NULL &&
		mapped_media_set.sequences[0].clips[0]->type == MEDIA_CLIP_SOURCE)
	{
		mapped_source = (media_clip_source_t*)*mapped_media_set.sequences[0].clips;

		if (mapped_source->clip_from == 0 &&
			mapped_source->clip_to == UINT_MAX &&
			mapped_source->tracks_mask[MEDIA_TYPE_AUDIO] == 0xffffffff &&
			mapped_source->tracks_mask[MEDIA_TYPE_VIDEO] == 0xffffffff)
		{
			// mapping result is a simple file path, set the uri of the current source
			cur_source->sequence->mapped_uri = mapped_source->mapped_uri;
			cur_source->mapped_uri = mapped_source->mapped_uri;
			return NGX_OK;
		}
	}

	if (ctx->submodule_context.request == NULL)
	{
		ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
			"ngx_http_vod_apply_mapping: unsupported - non-trivial mapping in progressive download");
		return NGX_HTTP_BAD_REQUEST;
	}

	if (ctx->submodule_context.media_set.sequence_count == 1 &&
		ctx->submodule_context.media_set.sequences[0].clips[0]->type == MEDIA_CLIP_SOURCE)
	{
		// media set was a single source, replace it with the mapping result
		ctx->submodule_context.media_set = mapped_media_set;

		// cur_source is pointing to the old media set, move it to the end of the new one
		ctx->submodule_context.cur_source = mapped_media_set.sources_end;

		return NGX_OK;
	}

	ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
		"ngx_http_vod_apply_mapping: unsupported - multi uri/filtered request %V did not return a simple json",
		&cur_source->stripped_uri);
	return NGX_HTTP_BAD_REQUEST;
}

static ngx_int_t
ngx_http_vod_run_mapped_mode_state_machine(ngx_http_request_t *r)
{
	ngx_child_request_params_t child_params;
	ngx_http_core_loc_conf_t *clcf;
	ngx_http_vod_loc_conf_t *conf;
	media_clip_source_t* cur_source;
	ngx_http_vod_ctx_t *ctx;
	ngx_str_t mapping;
	ngx_int_t rc;

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);
	conf = ctx->submodule_context.conf;

	// map all uris to paths
	for (;
		ctx->submodule_context.cur_source < ctx->submodule_context.media_set.sources_end;
		ctx->submodule_context.cur_source++)
	{
		cur_source = *ctx->submodule_context.cur_source;

		if (conf->path_mapping_cache_zone != NULL)
		{
			ngx_http_vod_init_file_key(
				conf,
				cur_source,
				(r->headers_in.host != NULL ? &r->headers_in.host->value : NULL));

			// try getting the file path from cache
			if (ngx_buffer_cache_fetch_copy_perf(
				ctx->submodule_context.r,
				ctx->perf_counters,
				conf->path_mapping_cache_zone,
				cur_source->file_key,
				&mapping.data,
				&mapping.len))
			{
				mapping.len--;		// remove the null

				ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
					"ngx_http_vod_run_mapped_mode_state_machine: path mapping cache hit %V", &mapping);

				rc = ngx_http_vod_apply_mapping(ctx, &mapping);
				if (rc != NGX_OK)
				{
					return rc;
				}
				continue;
			}
			else
			{
				ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
					"ngx_http_vod_run_mapped_mode_state_machine: path mapping cache miss");
			}
		}

		// initialize the upstream variables
		rc = ngx_http_vod_init_upstream_vars(ctx);
		if (rc != NGX_OK)
		{
			return rc;
		}

		// get the mp4 file path from upstream
		r->connection->log->action = "getting mapping";

		ngx_memzero(&child_params, sizeof(child_params));
		child_params.method = NGX_HTTP_GET;
		child_params.base_uri = cur_source->stripped_uri;
		child_params.extra_args = ctx->upstream_extra_args;
		child_params.host_name = conf->upstream_host_header;
		child_params.escape_uri = 1;

		ngx_perf_counter_start(ctx->perf_counter_context);

		rc = ngx_child_request_start(
			r,
			&ctx->child_request_buffers,
			ngx_http_vod_path_request_finished,
			r,
			&conf->upstream,
			&conf->child_request_location,
			&child_params,
			conf->max_mapping_response_size,
			NULL);
		if (rc != NGX_AGAIN)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_run_mapped_mode_state_machine: ngx_child_request_start failed %i", rc);
			return rc;
		}

		return rc;
	}

	// free the child request buffers - no need to issue any more http requests at this point
	ngx_child_request_free_buffers(r->pool, &ctx->child_request_buffers);

	// initialize for reading files
	clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);
	ctx->alignment = clcf->directio_alignment;

	ctx->open_file = ngx_http_vod_init_file_reader;
	ctx->async_reader = (ngx_http_vod_async_read_func_t)ngx_async_file_read;
	ctx->dump_part = (ngx_http_vod_dump_part_t)ngx_file_reader_dump_file_part;
	ctx->dump_request = ngx_http_vod_dump_file;

	// run the main state machine
	return ngx_http_vod_start_processing_mp4_file(r);
}

static void 
ngx_http_vod_path_request_finished(void* context, ngx_int_t rc, off_t content_length, ngx_buf_t* response)
{
	ngx_http_vod_loc_conf_t* conf;
	ngx_http_vod_ctx_t *ctx;
	ngx_http_request_t *r = context;
	ngx_str_t mapping;
	u_char* file_key;

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);

	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_path_request_finished: upstream request failed %i", rc);
		goto finalize_request;
	}

	ngx_perf_counter_end(ctx->perf_counters, ctx->perf_counter_context, PC_MAP_PATH);

	*response->last = '\0';		// this is ok since ngx_child_http_request always allocate the content-length + 1
	
	ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "ngx_http_vod_path_request_finished: result %s", response->pos);

	if (content_length == 0)
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_path_request_finished: empty path mapping response");
		rc = NGX_HTTP_NOT_FOUND;
		goto finalize_request;
	}

	file_key = ctx->submodule_context.cur_source[0]->file_key;		// save the file key before cur_source changes

	mapping.data = response->pos;
	mapping.len = response->last - response->pos;
	rc = ngx_http_vod_apply_mapping(ctx, &mapping);
	if (rc != NGX_OK)
	{
		goto finalize_request;
	}

	// save to cache
	conf = ctx->submodule_context.conf;

	if (conf->path_mapping_cache_zone != NULL)
	{
		if (ngx_buffer_cache_store_perf(
			ctx->perf_counters,
			conf->path_mapping_cache_zone,
			file_key,
			response->pos,
			response->last + 1 - response->pos))		// store with the null
		{
			ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_path_request_finished: stored in path mapping cache");
		}
		else
		{
			ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_path_request_finished: failed to store path mapping in cache");
		}
	}

	ctx->submodule_context.cur_source++;

	// run the state machine
	rc = ngx_http_vod_run_mapped_mode_state_machine(r);
	if (rc == NGX_AGAIN)
	{
		return;
	}

	if (rc != NGX_OK && rc != NGX_DONE)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_path_request_finished: ngx_http_vod_run_mapped_mode_state_machine failed %i", rc);
	}

finalize_request:

	ngx_http_vod_finalize_request(ctx, rc);
}

ngx_int_t
ngx_http_vod_mapped_request_handler(ngx_http_request_t *r)
{
	ngx_int_t rc;

	rc = ngx_http_vod_run_mapped_mode_state_machine(r);
	if (rc != NGX_AGAIN && rc != NGX_DONE && rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_mapped_request_handler: ngx_http_vod_run_mapped_mode_state_machine failed %i", rc);
	}

	return rc;
}

////// Remote mode only

static void
ngx_http_vod_http_read_completed(void* context, ngx_int_t rc, off_t content_length, ngx_buf_t* response)
{
	ngx_http_vod_ctx_t *ctx = (ngx_http_vod_ctx_t *)context;

	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
			"ngx_http_vod_http_read_completed: upstream request failed %i", rc);
		ngx_http_vod_finalize_request(ctx, rc);
		return;
	}

	ngx_http_vod_handle_read_completed(context, NGX_OK, content_length);
}

static ngx_int_t
ngx_http_vod_async_http_read(ngx_http_vod_http_reader_state_t *state, u_char *buf, size_t size, off_t offset)
{
	ngx_http_vod_loc_conf_t *conf;
	ngx_http_vod_ctx_t *ctx;
	ngx_child_request_params_t child_params;

	ctx = ngx_http_get_module_ctx(state->r, ngx_http_vod_module);
	conf = ctx->submodule_context.conf;

	ngx_memzero(&child_params, sizeof(child_params));
	child_params.method = NGX_HTTP_GET;
	child_params.base_uri = state->cur_remote_suburi;
	child_params.extra_args = ctx->upstream_extra_args;
	child_params.host_name = conf->upstream_host_header;
	child_params.range_start = offset;
	child_params.range_end = offset + size;
	child_params.escape_uri = 1;

	return ngx_child_request_start(
		state->r, 
		&ctx->child_request_buffers,
		ngx_http_vod_http_read_completed, 
		ctx,
		&conf->upstream,
		&conf->child_request_location,
		&child_params,
		size, 
		buf);
}

static ngx_int_t
ngx_http_vod_dump_http_part(ngx_http_vod_http_reader_state_t *state, off_t start, off_t end)
{
	ngx_http_vod_loc_conf_t *conf;
	ngx_http_vod_ctx_t *ctx;
	ngx_child_request_params_t child_params;

	ctx = ngx_http_get_module_ctx(state->r, ngx_http_vod_module);
	conf = ctx->submodule_context.conf;

	ngx_memzero(&child_params, sizeof(child_params));
	child_params.method = NGX_HTTP_GET;
	child_params.base_uri = state->cur_remote_suburi;
	child_params.extra_args = ctx->upstream_extra_args;
	child_params.host_name = conf->upstream_host_header;
	child_params.range_start = start;
	child_params.range_end = end;
	child_params.escape_uri = 1;

	return ngx_child_request_start(
		state->r,
		&ctx->child_request_buffers,
		ngx_http_vod_http_read_completed,
		ctx,
		&conf->upstream,
		&conf->child_request_location,
		&child_params,
		0,
		NULL);
}

ngx_int_t 
ngx_http_vod_dump_http_request(ngx_http_vod_ctx_t *ctx)
{
	ngx_http_request_t* r;
	ngx_http_vod_loc_conf_t *conf;
	ngx_child_request_params_t child_params;

	conf = ctx->submodule_context.conf;
	r = ctx->submodule_context.r;

	ngx_memzero(&child_params, sizeof(child_params));
	child_params.method = r->method;
	child_params.base_uri = r->unparsed_uri;
	child_params.extra_args = ctx->upstream_extra_args;
	child_params.host_name = conf->upstream_host_header;
	child_params.proxy_range = 1;
	child_params.proxy_accept_encoding = 1;

	return ngx_dump_request(
		r,
		&conf->upstream,
		&child_params);
}

static ngx_int_t
ngx_http_vod_http_reader_open_file(ngx_http_request_t* r, ngx_str_t* path, void** context)
{
	ngx_http_vod_http_reader_state_t* state;
	ngx_http_vod_ctx_t *ctx;
	ngx_int_t rc;

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);

	// initialize the upstream variables
	rc = ngx_http_vod_init_upstream_vars(ctx);
	if (rc != NGX_OK)
	{
		return rc;
	}

	state = ngx_palloc(r->pool, sizeof(*state));
	if (state == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_http_reader_open_file: ngx_palloc failed");
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	// Note: since this remote mode, no need to open any files, just save the remote uri
	state->r = r;
	state->cur_remote_suburi = *path;
	*context = state;

	return NGX_OK;
}

ngx_int_t
ngx_http_vod_remote_request_handler(ngx_http_request_t *r)
{
	ngx_http_vod_ctx_t *ctx;
	ngx_int_t rc;

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);

	ctx->alignment = sizeof(int64_t);		// don't care about alignment when working remote

	ctx->open_file = ngx_http_vod_http_reader_open_file;
	ctx->async_reader = (ngx_http_vod_async_read_func_t)ngx_http_vod_async_http_read;
	ctx->dump_part = (ngx_http_vod_dump_part_t)ngx_http_vod_dump_http_part;
	ctx->dump_request = ngx_http_vod_dump_http_request;
	ctx->file_key_prefix = (r->headers_in.host != NULL ? &r->headers_in.host->value : NULL);

	rc = ngx_http_vod_start_processing_mp4_file(r);
	if (rc != NGX_AGAIN && rc != NGX_DONE && rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_remote_request_handler: ngx_http_vod_start_processing_mp4_file failed %i", rc);
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
		return NGX_HTTP_BAD_REQUEST;
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

	if (media_set->sequence_count != 1 && 
		((*request)->flags & (REQUEST_FLAG_SINGLE_TRACK_PER_MEDIA_TYPE | REQUEST_FLAG_SINGLE_TRACK)) != 0)
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_parse_uri: request has more than one sub uri while only one is supported");
		return NGX_HTTP_BAD_REQUEST;
	}

	return NGX_OK;
}

ngx_int_t
ngx_http_vod_handler(ngx_http_request_t *r)
{
	ngx_perf_counter_context(pcctx);
	ngx_perf_counters_t* perf_counters;
	ngx_http_vod_ctx_t *ctx;
	request_params_t request_params;
	media_set_t media_set;
	const ngx_http_vod_request_t* request;
	ngx_http_vod_loc_conf_t *conf;
	u_char request_key[BUFFER_CACHE_KEY_SIZE];
	u_char* cache_buffer;
	size_t cache_buffer_size;
	ngx_md5_t md5;
	ngx_str_t content_type;
	ngx_str_t response;
	ngx_int_t rc;

	ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "ngx_http_vod_handler: started");

	ngx_perf_counter_start(pcctx);
	conf = ngx_http_get_module_loc_conf(r, ngx_http_vod_module);
	perf_counters = ngx_perf_counter_get_state(conf->perf_counters_zone);

	if (r->method == NGX_HTTP_OPTIONS)
	{
		response.data = NULL;
		response.len = 0;

		rc = ngx_http_vod_send_header(r, response.len, &options_content_type);
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
			rc = NGX_HTTP_BAD_REQUEST;
			goto done;
		}
	}

	if (request != NULL && 
		request->handle_metadata_request != NULL &&
		conf->response_cache_zone != NULL)
	{
		// calc request key from host + uri
		ngx_md5_init(&md5);

		if (r->headers_in.host != NULL)
		{
			ngx_md5_update(&md5, r->headers_in.host->value.data, r->headers_in.host->value.len);
		}

		if (conf->https_header_name.len)
		{
			if (ngx_http_vod_header_exists(r, &conf->https_header_name))
			{
				ngx_md5_update(&md5, "1", sizeof("1") - 1);
			}
			else
			{
				ngx_md5_update(&md5, "0", sizeof("0") - 1);
			}
		}

		ngx_md5_update(&md5, r->uri.data, r->uri.len);
		ngx_md5_final(request_key, &md5);

		// try to fetch from cache
		if (ngx_buffer_cache_fetch_copy_perf(r, perf_counters, conf->response_cache_zone, request_key, &cache_buffer, &cache_buffer_size) &&
			cache_buffer_size > sizeof(size_t))
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_handler: response cache hit, size is %uz", cache_buffer_size);

			// extract the content type
			content_type.len = *(size_t*)cache_buffer;
			cache_buffer += sizeof(size_t);
			cache_buffer_size -= sizeof(size_t);
			content_type.data = cache_buffer;

			if (cache_buffer_size >= content_type.len)
			{
				// extract the response buffer
				response.data = cache_buffer + content_type.len;
				response.len = cache_buffer_size - content_type.len;

				// update request flags
				r->root_tested = !r->error_page;
				r->allow_ranges = 1;

				// return the response
				rc = ngx_http_vod_send_header(r, response.len, &content_type);
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
	ctx->submodule_context.request = request;
	ctx->submodule_context.cur_source = media_set.sources;
	ctx->submodule_context.request_context.pool = r->pool;
	ctx->submodule_context.request_context.log = r->connection->log;
	ctx->perf_counters = perf_counters;
	ngx_perf_counter_copy(ctx->total_perf_counter_context, pcctx);

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
