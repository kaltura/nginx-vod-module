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
#include "ngx_http_vod_conf.h"
#include "ngx_file_reader.h"
#include "ngx_buffer_cache.h"
#include "vod/aes_encrypt.h"
#include "vod/mp4_parser.h"
#include "vod/read_cache.h"

// constants
#define MAX_MOOV_START_READS (3)		// maximum number of attempts to find the moov atom start for non-fast-start files

// enums
enum {
	STATE_INITIAL,
	STATE_INITIAL_READ,
	STATE_MOOV_ATOM_READ,
	STATE_MOOV_ATOM_PARSED,
	STATE_FRAME_DATA_READ,
};

// typedefs
typedef ngx_int_t(*ngx_http_vod_open_file_t)(ngx_http_request_t* r, ngx_str_t* path);
typedef ngx_int_t(*ngx_http_vod_async_read_func_t)(void* context, u_char *buf, size_t size, off_t offset);
typedef ngx_int_t(*ngx_http_vod_dump_request_t)(void* context);

typedef struct {
	ngx_http_request_t* r;
	ngx_chain_t* chain_end;
	uint32_t total_size;
} ngx_http_vod_write_segment_context_t;

typedef struct {
	// base params
	ngx_http_vod_submodule_context_t submodule_context;
	off_t alignment;
	int state;

	// moov read state
	u_char* read_buffer;
	size_t buffer_size;
	off_t read_offset;
	off_t atom_start_offset;
	int moov_start_reads;
	off_t moov_offset;
	size_t moov_size;

	// reading abstraction (over file / http)
	ngx_http_vod_open_file_t open_file;
	ngx_http_vod_async_read_func_t async_reader;
	ngx_http_vod_dump_request_t dump_request;
	void* async_reader_context;
	ngx_flag_t file_opened;

	// read state - file
	ngx_file_reader_state_t file_reader;

	// read state - http
	ngx_child_request_buffers_t child_request_buffers;
	ngx_str_t* file_key_prefix;
	ngx_str_t cur_remote_suburi;

	// segment requests only
	read_cache_state_t read_cache_state;
	ngx_http_vod_frame_processor_t frame_processor;
	void* frame_processor_state;
	ngx_chain_t out;
	ngx_http_vod_write_segment_context_t write_segment_buffer_context;
	aes_encrypt_context_t* encrypted_write_context;
} ngx_http_vod_ctx_t;

// globals
ngx_module_t  ngx_http_vod_module = {
    NGX_MODULE_V1,
    &ngx_http_vod_module_ctx,         /* module context */
    ngx_http_vod_commands,            /* module directives */
    NGX_HTTP_MODULE,                  /* module type */
    NULL,                             /* init master */
    NULL,                             /* init module */
    NULL,                             /* init process */
    NULL,                             /* init thread */
    NULL,                             /* exit thread */
    NULL,                             /* exit process */
    NULL,                             /* exit master */
    NGX_MODULE_V1_PADDING
};

////// Encryption support

static vod_status_t
ngx_http_vod_init_aes_encryption(ngx_http_vod_ctx_t *ctx, write_callback_t write_callback, void* callback_context)
{
	ngx_pool_cleanup_t *cln;
	vod_status_t rc;

	ctx->encrypted_write_context = ngx_palloc(ctx->submodule_context.r->pool, sizeof(*ctx->encrypted_write_context));
	if (ctx->encrypted_write_context == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
			"ngx_http_vod_init_aes_encryption: ngx_palloc failed");
		return VOD_ALLOC_FAILED;
	}

	cln = ngx_pool_cleanup_add(ctx->submodule_context.r->pool, 0);
	if (cln == NULL) 
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
			"ngx_http_vod_init_aes_encryption: ngx_pool_cleanup_add failed");
		return VOD_ALLOC_FAILED;
	}

	cln->handler = (ngx_pool_cleanup_pt)aes_encrypt_cleanup;
	cln->data = ctx->encrypted_write_context;

	rc = aes_encrypt_init(
		ctx->encrypted_write_context, 
		&ctx->submodule_context.request_context, 
		write_callback, 
		callback_context, 
		ctx->submodule_context.request_params.file_key, 
		ctx->submodule_context.request_params.segment_index);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
			"ngx_http_vod_init_aes_encryption: aes_encrypt_init failed %i", rc);
		return rc;
	}

	return VOD_OK;
}

////// Common mp4 processing

static ngx_int_t 
ngx_http_vod_read_moov_atom(ngx_http_vod_ctx_t *ctx)
{
	u_char* new_read_buffer = NULL;
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
		rc = ctx->async_reader(
			ctx->async_reader_context, 
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

		ctx->buffer_size = rc;
	}

	// check whether we already have the whole atom
	if (ctx->moov_offset + ctx->moov_size < ctx->buffer_size)
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

	new_read_buffer = ngx_pmemalign(ctx->submodule_context.r->pool, new_buffer_size, ctx->alignment);
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
	ctx->state = STATE_MOOV_ATOM_READ;
	rc = ctx->async_reader(
		ctx->async_reader_context, 
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

	ctx->buffer_size += rc;
	return NGX_OK;
}

static ngx_int_t 
ngx_http_vod_parse_moov_atom(ngx_http_vod_ctx_t *ctx, u_char* moov_buffer, size_t moov_size)
{
	mpeg_base_metadata_t mpeg_base_metadata;
	const ngx_http_vod_request_t* request = ctx->submodule_context.request_params.request;
	ngx_http_vod_suburi_params_t* suburi_params = ctx->submodule_context.cur_suburi;
	request_context_t* request_context = &ctx->submodule_context.request_context;
	vod_status_t rc;
	file_info_t file_info;
	uint32_t segment_count;
	uint32_t max_segment_duration;

	// init the request context
	request_context->parse_type = request->parse_type;
	request_context->stream_comparator = request->stream_comparator;
	request_context->stream_comparator_context = 
		(u_char*)ctx->submodule_context.conf + request->stream_comparator_conf_offset;

	file_info.file_index = suburi_params->file_index;
	file_info.uri = suburi_params->uri;

	// parse the basic metadata
	rc = mp4_parser_parse_basic_metadata(
		&ctx->submodule_context.request_context,
		suburi_params->required_tracks,
		suburi_params->clip_from,
		suburi_params->clip_to,
		moov_buffer,
		moov_size,
		&file_info,
		&mpeg_base_metadata);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, request_context->log, 0,
			"ngx_http_vod_parse_moov_atom: mp4_parser_parse_basic_metadata failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(rc);
	}

	if (request->request_class == REQUEST_CLASS_MANIFEST)
	{
		request_context->max_frame_count = 1024 * 1024;
		request_context->simulation_only = TRUE;
		request_context->start = suburi_params->clip_from;
		request_context->end = suburi_params->clip_to;
	}
	else
	{
		request_context->max_frame_count = 16 * 1024;
		request_context->simulation_only = FALSE;

		// validate the requested segment index
		if (request->request_class == REQUEST_CLASS_SEGMENT_LAST_SHORT)
		{
			segment_count = DIV_CEIL(mpeg_base_metadata.duration_millis, ctx->submodule_context.conf->segment_duration);
		}
		else
		{
			if (mpeg_base_metadata.duration_millis > ctx->submodule_context.conf->segment_duration)
			{
				segment_count = mpeg_base_metadata.duration_millis / ctx->submodule_context.conf->segment_duration;
			}
			else
			{
				segment_count = 1;
			}
		}

		if (ctx->submodule_context.request_params.segment_index >= segment_count)
		{
			ngx_log_error(NGX_LOG_ERR, request_context->log, 0,
				"ngx_http_vod_parse_moov_atom: requested segment index %uD exceeds the segment count %uD", ctx->submodule_context.request_params.segment_index, segment_count);
			return NGX_HTTP_NOT_FOUND;
		}

		// get the start / end offsets
		if (ctx->submodule_context.request_params.segment_index + 1 < segment_count)
		{
			// not the last segment
			max_segment_duration = ctx->submodule_context.conf->segment_duration;
		}
		else
		{
			// last segment
			max_segment_duration = 2 * ctx->submodule_context.conf->segment_duration;
		}
		
		request_context->start = suburi_params->clip_from + ctx->submodule_context.request_params.segment_index * ctx->submodule_context.conf->segment_duration;
		request_context->end = MIN(request_context->start + max_segment_duration, suburi_params->clip_to);
		if (request_context->end <= request_context->start)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"ngx_http_vod_parse_moov_atom: segment index %uD too big for clip from %uD and clip to %uD",
				ctx->submodule_context.request_params.segment_index, suburi_params->clip_from, suburi_params->clip_to);
			return VOD_BAD_REQUEST;
		}
	}

	// parse the frames
	rc = mp4_parser_parse_frames(
		&ctx->submodule_context.request_context,
		&mpeg_base_metadata,
		suburi_params->clip_from,
		suburi_params->clip_to,
		&ctx->submodule_context.mpeg_metadata);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, request_context->log, 0,
			"ngx_http_vod_parse_moov_atom: mp4_parser_parse_frames failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(rc);
	}

	return NGX_OK;
}

////// Segment request handling

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
	b = ngx_pcalloc(context->r->pool, sizeof(ngx_buf_t));
	if (b == NULL) 
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, context->r->connection->log, 0,
			"ngx_http_vod_write_segment_buffer: ngx_pcalloc failed (1)");
		return VOD_ALLOC_FAILED;
	}

	b->pos = buffer;
	b->last = buffer + size;
	b->memory = 1;
	b->last_buf = 0;	// not the last buffer in the buffer chain

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
			chain = ngx_pcalloc(context->r->pool, sizeof(ngx_chain_t));
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
	ngx_http_vod_loc_conf_t* conf;
	ngx_http_request_t* r = ctx->submodule_context.r;
	write_callback_t write_callback;
	ngx_str_t output_buffer = ngx_null_string;
	ngx_str_t content_type;
	size_t response_size = 0;
	bool_t reuse_buffer;
	void* write_context;
	ngx_int_t rc;

	conf = ctx->submodule_context.conf;

	// open the file in case the moov was loaded from cache
	if (!ctx->file_opened)
	{
		rc = ctx->open_file(r, &ctx->submodule_context.cur_suburi->stripped_uri);
		if (rc != NGX_OK)
		{
			if (rc != NGX_AGAIN)		// NGX_AGAIN will be returned in case of fallback
			{
				ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
					"ngx_http_vod_init_frame_processing: open_file failed %i", rc);
			}
			return rc;
		}
		ctx->file_opened = 1;
	}

	// enable directio if enabled in the configuration (ignore errors)
	// Note that directio is set on transfer only to allow the kernel to cache the "moov" atom
	if (conf->request_handler != ngx_http_vod_remote_request_handler)
	{
		ngx_file_reader_enable_directio(&ctx->file_reader);
	}

	// initialize the read cache
	read_cache_init(
		&ctx->read_cache_state, 
		&ctx->submodule_context.request_context, 
		conf->cache_buffer_size, 
		ctx->alignment);

	// initialize the response writer
	ctx->out.buf = NULL;
	ctx->out.next = NULL;
	ctx->write_segment_buffer_context.r = r;
	ctx->write_segment_buffer_context.chain_end = &ctx->out;
	ctx->write_segment_buffer_context.total_size = 0;

	if (conf->secret_key.len != 0)
	{
		rc = ngx_http_vod_init_aes_encryption(ctx, ngx_http_vod_write_segment_buffer, &ctx->write_segment_buffer_context);
		if (rc != VOD_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_init_frame_processing: ngx_http_vod_init_aes_encryption failed %i", rc);
			return ngx_http_vod_status_to_ngx_error(rc);
		}

		write_callback = (write_callback_t)aes_encrypt_write;
		write_context = ctx->encrypted_write_context;
	}
	else
	{
		write_callback = ngx_http_vod_write_segment_buffer;
		write_context = &ctx->write_segment_buffer_context;
	}

	// initialize the protocol specific frame processor
	rc = ctx->submodule_context.request_params.request->init_frame_processor(
		&ctx->submodule_context,
		&ctx->read_cache_state,
		write_callback,
		write_context,
		&ctx->frame_processor,
		&ctx->frame_processor_state,
		&output_buffer,
		&response_size,
		&content_type);
	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_init_frame_processing: init_frame_processor failed %i", rc);
		return rc;
	}

	r->headers_out.content_type_len = content_type.len;
	r->headers_out.content_type.len = content_type.len;
	r->headers_out.content_type.data = content_type.data;

	// if the frame processor can't determine the size in advance we have to build the whole response before we can start sending it
	if (response_size == 0)
	{
		return NGX_OK;
	}

	// calculate the response size
	if (conf->secret_key.len != 0)
	{
		response_size = AES_ROUND_TO_BLOCK(response_size);
	}

	// set the status line
	r->headers_out.status = NGX_HTTP_OK;
	r->headers_out.content_length_n = response_size;

	// set the etag
	rc = ngx_http_set_etag(r);
	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_init_frame_processing: ngx_http_set_etag failed %i", rc);
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	// send the response headers
	rc = ngx_http_send_header(r);
	if (rc == NGX_ERROR || rc > NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_init_frame_processing: ngx_http_send_header failed %i", rc);
		return rc;
	}

	if (r->header_only || r->method == NGX_HTTP_HEAD)
	{
		return NGX_DONE;
	}

	// write the initial buffer if provided
	if (output_buffer.len != 0)
	{
		rc = write_callback(write_context, output_buffer.data, output_buffer.len, &reuse_buffer);
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
ngx_http_vod_finalize_segment_response(ngx_http_vod_ctx_t *ctx)
{
	ngx_http_request_t *r = ctx->submodule_context.r;
	ngx_int_t rc;

	// flush the encryption buffer
	if (ctx->encrypted_write_context != NULL)
	{
		rc = aes_encrypt_flush(ctx->encrypted_write_context);
		if (rc != VOD_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_finalize_segment_response: aes_encrypt_flush failed %i", rc);
			return ngx_http_vod_status_to_ngx_error(rc);
		}
	}

	// if we already sent the headers and all the buffers, just signal completion and return
	if (r->header_sent)
	{
		if (ctx->write_segment_buffer_context.total_size != r->headers_out.content_length_n)
		{
			ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
				"ngx_http_vod_finalize_segment_response: actual content length %uD is different than reported length %O", 
				ctx->write_segment_buffer_context.total_size, r->headers_out.content_length_n);
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

	// set the status line
	r->headers_out.status = NGX_HTTP_OK;
	r->headers_out.content_length_n = ctx->write_segment_buffer_context.total_size;

	// set the etag
	rc = ngx_http_set_etag(r);
	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_finalize_segment_response: ngx_http_set_etag failed %i", rc);
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	// send the response headers
	rc = ngx_http_send_header(r);
	if (rc == NGX_ERROR || rc > NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_finalize_segment_response: ngx_http_send_header failed %i", rc);
		return rc;
	}
	
	if (r->header_only || r->method == NGX_HTTP_HEAD)
	{
		return rc;
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

static ngx_int_t 
ngx_http_vod_process_mp4_frames(ngx_http_vod_ctx_t *ctx)
{
	uint64_t required_offset;
	uint64_t read_offset;
	u_char* read_buffer;
	uint32_t read_size;
	vod_status_t rc;

	for (;;)
	{
		rc = ctx->frame_processor(ctx->frame_processor_state, &required_offset);
		switch (rc)
		{
		case VOD_OK:
			// we're done
			return ngx_http_vod_finalize_segment_response(ctx);

		case VOD_AGAIN:
			// handled outside the switch
			break;

		default:
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_process_mp4_frames: frame_processor failed %i", rc);
			return ngx_http_vod_status_to_ngx_error(rc);
		}

		// get a buffer to read into
		rc = read_cache_get_read_buffer(&ctx->read_cache_state, required_offset, &read_offset, &read_buffer, &read_size);
		if (rc != VOD_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_process_mp4_frames: read_cache_get_read_buffer failed %i", rc);
			return ngx_http_vod_status_to_ngx_error(rc);
		}

		// perform the read
		rc = ctx->async_reader(ctx->async_reader_context, read_buffer, read_size, read_offset);
		if (rc < 0)
		{
			if (rc != NGX_AGAIN)
			{
				ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
					"ngx_http_vod_process_mp4_frames: async_reader failed %i", rc);
			}
			return rc;
		}

		// read completed synchronously, update the read cache
		read_cache_read_completed(&ctx->read_cache_state, rc);
	}
}

////// Common

static void
ngx_http_vod_init_file_key(ngx_http_request_t* r, ngx_str_t* path1, ngx_str_t* path2)
{
	ngx_http_vod_loc_conf_t* conf;
	ngx_http_vod_ctx_t *ctx;
	ngx_md5_t md5;

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);
	conf = ctx->submodule_context.conf;

	ngx_md5_init(&md5);
	ngx_md5_update(&md5, conf->secret_key.data, conf->secret_key.len);
	if (path1 != NULL)
	{
		ngx_md5_update(&md5, path1->data, path1->len);
	}
	if (path2 != NULL)
	{
		ngx_md5_update(&md5, path2->data, path2->len);
	}
	ngx_md5_final(ctx->submodule_context.request_params.file_key, &md5);
}

static ngx_int_t
ngx_http_vod_run_state_machine(ngx_http_vod_ctx_t *ctx)
{
	ngx_http_vod_loc_conf_t* conf;
	ngx_http_request_t* r = ctx->submodule_context.r;
	ngx_str_t content_type;
	ngx_str_t response;
	u_char* moov_buffer;
	size_t moov_size;
	ngx_int_t rc;

	if (ctx->state == STATE_FRAME_DATA_READ)
	{
		goto process_frames;
	}

	conf = ctx->submodule_context.conf;

	for (;
		ctx->submodule_context.cur_file_index < ctx->submodule_context.request_params.suburi_count;
		ctx->submodule_context.cur_suburi++, ctx->submodule_context.cur_file_index++)
	{
		switch (ctx->state)
		{
		case STATE_INITIAL:
			ngx_http_vod_init_file_key(r, ctx->file_key_prefix, &ctx->submodule_context.cur_suburi->stripped_uri);

			if (conf->moov_cache_zone != NULL)
			{
				// try to read the moov atom from cache
				if (ngx_buffer_cache_fetch(conf->moov_cache_zone, ctx->submodule_context.request_params.file_key, &moov_buffer, &moov_size))
				{
					ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
						"ngx_http_vod_run_state_machine: moov atom cache hit, size is %uz", moov_size);

					rc = ngx_http_vod_parse_moov_atom(ctx, moov_buffer, moov_size);
					if (rc != NGX_OK)
					{
						ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
							"ngx_http_vod_run_state_machine: ngx_http_vod_parse_moov_atom failed %i", rc);
						return rc;
					}
					continue;
				}
				else
				{
					ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
						"ngx_http_vod_run_state_machine: moov atom cache miss");
				}
			}

			// open the file
			rc = ctx->open_file(r, &ctx->submodule_context.cur_suburi->stripped_uri);
			if (rc != NGX_OK)
			{
				if (rc != NGX_AGAIN)		// NGX_AGAIN will be returned in case of fallback
				{
					ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
						"ngx_http_vod_run_state_machine: open_file failed %i", rc);
				}
				return rc;
			}

			ctx->file_opened = 1;

			// allocate the initial read buffer
			ctx->read_buffer = ngx_pmemalign(r->pool, conf->initial_read_size, ctx->alignment);
			if (ctx->read_buffer == NULL)
			{
				ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
					"ngx_http_vod_run_state_machine: ngx_pmemalign failed");
				return NGX_HTTP_INTERNAL_SERVER_ERROR;
			}

			// read the file header
			r->connection->log->action = "reading mp4 header";
			ctx->moov_start_reads = MAX_MOOV_START_READS;
			ctx->state = STATE_INITIAL_READ;
			rc = ctx->async_reader(ctx->async_reader_context, ctx->read_buffer, conf->initial_read_size, 0);
			if (rc < 0)		// inc. NGX_AGAIN
			{
				if (rc != NGX_AGAIN)
				{
					ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
						"ngx_http_vod_run_state_machine: async_reader failed %i", rc);
				}
				return rc;
			}

			// read completed synchronously
			ctx->buffer_size = rc;
			// fallthrough

		case STATE_INITIAL_READ:
			// read the entire atom
			rc = ngx_http_vod_read_moov_atom(ctx);
			if (rc == NGX_AGAIN)
			{
				return NGX_AGAIN;
			}

			if (rc != NGX_OK)
			{
				ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
					"ngx_http_vod_run_state_machine: ngx_http_vod_read_moov_atom failed %i", rc);
				return rc;
			}
			// fallthrough

		case STATE_MOOV_ATOM_READ:
			// make sure we got the whole moov atom
			if (ctx->buffer_size < ctx->moov_offset + ctx->moov_size)
			{
				ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
					"ngx_http_vod_run_state_machine: buffer size %uD is smaller than moov end offset %uD", ctx->buffer_size, ctx->moov_offset + ctx->moov_size);
				return ngx_http_vod_status_to_ngx_error(VOD_BAD_DATA);
			}

			// parse the moov atom
			rc = ngx_http_vod_parse_moov_atom(ctx, ctx->read_buffer + ctx->moov_offset, ctx->moov_size);
			if (rc != NGX_OK)
			{
				ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
					"ngx_http_vod_run_state_machine: ngx_http_vod_parse_moov_atom failed %i", rc);
				return rc;
			}

			// save the moov atom to cache
			if (conf->moov_cache_zone != NULL)
			{
				if (ngx_buffer_cache_store(
					conf->moov_cache_zone,
					ctx->submodule_context.request_params.file_key,
					ctx->read_buffer + ctx->moov_offset,
					ctx->moov_size))
				{
					ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
						"ngx_http_vod_run_state_machine: stored moov atom in cache");
				}
				else
				{
					ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
						"ngx_http_vod_run_state_machine: failed to store moov atom in cache");
				}
			}

			// no longer need the moov atom buffer
			ngx_pfree(ctx->submodule_context.r->pool, ctx->read_buffer);
			ctx->read_buffer = NULL;

			// reset the state
			ctx->state = STATE_INITIAL;
			ctx->buffer_size = 0;
			ctx->read_offset = 0;
			ctx->atom_start_offset = 0;
			break;

		default:
			ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_run_state_machine: invalid state %d", ctx->state);
			return NGX_HTTP_INTERNAL_SERVER_ERROR;
		}
	}

	// Note: at this point we finished reading all moov atoms

	rc = mp4_parser_finalize_mpeg_metadata(&ctx->submodule_context.request_context, &ctx->submodule_context.mpeg_metadata);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
			"ngx_http_vod_run_state_machine: mp4_parser_finalize_mpeg_metadata failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(rc);
	}

	if ((ctx->submodule_context.request_params.request->flags & REQUEST_FLAG_SINGLE_STREAM) != 0 &&
		ctx->submodule_context.mpeg_metadata.streams.nelts != 1)
	{
		ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
			"ngx_http_vod_run_state_machine: got %ui streams while only a single stream is supported", ctx->submodule_context.mpeg_metadata.streams.nelts);
		return NGX_HTTP_BAD_REQUEST;
	}

	// restore cur_suburi / cur_file_index
	// Note: cur_suburi should only be used in the request handler if it has REQUEST_FLAG_SINGLE_STREAM / REQUEST_FLAG_SINGLE_FILE.
	//		requests that require frame processing (e.g. hls segment) must have one of these flags enabled
	ctx->submodule_context.cur_suburi = ctx->submodule_context.request_params.suburis;
	ctx->submodule_context.cur_file_index = 0;

	// handle metadata requests
	if (ctx->submodule_context.request_params.request->handle_metadata_request != NULL)
	{
		rc = ctx->submodule_context.request_params.request->handle_metadata_request(
			&ctx->submodule_context,
			&response,
			&content_type);
		if (rc != NGX_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
				"ngx_http_vod_run_state_machine: handle_metadata_request failed %i", rc);
			return rc;
		}

		return ngx_http_vod_send_response(ctx->submodule_context.r, &response, content_type.data, content_type.len);
	}

	// initialize the processing of the video frames
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
	ctx->state = STATE_FRAME_DATA_READ;

process_frames:

	rc = ngx_http_vod_process_mp4_frames(ctx);
	if (rc != NGX_OK && rc != NGX_AGAIN)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
			"ngx_http_vod_run_state_machine: ngx_http_vod_process_mp4_frames failed %i", rc);
	}
	return rc;
}

static void
ngx_http_vod_finalize_request(ngx_http_request_t *r, ngx_int_t rc)
{
	if (r->header_sent && rc != NGX_OK)
	{
		rc = NGX_ERROR;
	}

	ngx_http_finalize_request(r, rc);
}

static void
ngx_http_vod_handle_read_completed(void* context, ngx_int_t rc, ssize_t bytes_read)
{
	ngx_http_vod_ctx_t *ctx = (ngx_http_vod_ctx_t *)context;

	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->submodule_context.request_context.log, 0,
			"ngx_http_vod_handle_read_completed: read failed %i", rc);
		goto finalize_request;
	}

	if (bytes_read <= 0)
	{
		ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
			"ngx_http_vod_handle_read_completed: bytes read is zero");
		rc = ngx_http_vod_status_to_ngx_error(VOD_BAD_DATA);
		goto finalize_request;
	}

	// update the bytes read
	switch (ctx->state)
	{
	case STATE_INITIAL_READ:
		ctx->buffer_size = bytes_read;
		break;

	case STATE_MOOV_ATOM_READ:
		ctx->buffer_size += bytes_read;
		break;

	case STATE_FRAME_DATA_READ:
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

	ngx_http_vod_finalize_request(ctx->submodule_context.r, rc);
}

static ngx_int_t
ngx_http_vod_start_processing_mp4_file(ngx_http_request_t *r)
{
	ngx_http_vod_ctx_t *ctx;
	ngx_int_t rc;

	// update request flags
	r->root_tested = !r->error_page;
	r->allow_ranges = 1;

	// handle serve requests
	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);
	if (ctx->submodule_context.request_params.request == NULL)
	{
		rc = ctx->open_file(r, &ctx->submodule_context.cur_suburi->stripped_uri);
		if (rc != NGX_OK)
		{
			if (rc != NGX_AGAIN)		// NGX_AGAIN will be returned in case of fallback
			{
				ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
					"ngx_http_vod_start_processing_mp4_file: open_file failed %i", rc);
			}
			return rc;
		}

		return ctx->dump_request(ctx->async_reader_context);
	}

	// preliminary initialization of the request context
	ctx->submodule_context.request_context.pool = r->pool;
	ctx->submodule_context.request_context.log = r->connection->log;

	rc = mp4_parser_init_mpeg_metadata(
		&ctx->submodule_context.request_context,
		&ctx->submodule_context.mpeg_metadata);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_start_processing_mp4_file: mp4_parser_init_mpeg_metadata failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(rc);
	}

	ctx->state = STATE_INITIAL;
	return ngx_http_vod_run_state_machine(ctx);
}

////// Local & mapped modes

static void
ngx_http_vod_file_read_completed(void* context, ngx_int_t rc, ssize_t bytes_read)
{
	ngx_http_vod_ctx_t *ctx = (ngx_http_vod_ctx_t *)context;
	ngx_connection_t *c;

	c = ctx->submodule_context.r->connection;

	ngx_http_vod_handle_read_completed(context, rc, bytes_read);

	ngx_http_run_posted_requests(c);
}

static ngx_int_t
ngx_http_vod_init_file_reader(ngx_http_request_t *r, ngx_str_t* path)
{
	ngx_http_core_loc_conf_t *clcf;
	ngx_http_vod_ctx_t *ctx;
	ngx_int_t rc;

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);

	clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

	rc = ngx_file_reader_init(&ctx->file_reader, ngx_http_vod_file_read_completed, ctx, r, clcf, path);
	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_init_file_reader: ngx_file_reader_init failed %i", rc);
		return rc;
	}

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_dump_request_to_fallback(
	ngx_http_request_t *r)
{
	ngx_http_vod_loc_conf_t* conf;
	ngx_child_request_params_t child_params;
	ngx_http_vod_ctx_t *ctx;
	ngx_int_t rc;

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);
	conf = ctx->submodule_context.conf;

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
	child_params.base_uri = r->uri;
	child_params.extra_headers = conf->proxy_header;
	child_params.proxy_range = 1;
	child_params.proxy_accept_encoding = 1;

	rc = ngx_dump_request(
		r,
		&conf->fallback_upstream,
		&child_params);
	return rc;
}

////// Local mode only

static ngx_int_t
ngx_http_vod_init_file_reader_with_fallback(ngx_http_request_t *r, ngx_str_t* path)
{
	ngx_int_t rc;

	rc = ngx_http_vod_init_file_reader(r, path);
	if (rc == NGX_HTTP_NOT_FOUND)
	{
		// try the fallback
		rc = ngx_http_vod_dump_request_to_fallback(r);
		if (rc != NGX_AGAIN)
		{
			return NGX_HTTP_NOT_FOUND;
		}
	}
	return rc;
}

ngx_int_t
ngx_http_vod_local_request_handler(ngx_http_request_t *r)
{
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
	for (;
		ctx->submodule_context.cur_file_index < ctx->submodule_context.request_params.suburi_count;
		ctx->submodule_context.cur_suburi++, ctx->submodule_context.cur_file_index++)
	{
		r->uri = ctx->submodule_context.cur_suburi->stripped_uri;
		last = ngx_http_map_uri_to_path(r, &path, &root, 0);
		r->uri = original_uri;
		if (last == NULL)
		{
			ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_local_request_handler: ngx_http_map_uri_to_path failed");
			return NGX_HTTP_INTERNAL_SERVER_ERROR;
		}

		path.len = last - path.data;

		ctx->submodule_context.cur_suburi->stripped_uri = path;
	}

	// restart the file index/uri params
	ctx->submodule_context.cur_suburi = ctx->submodule_context.request_params.suburis;
	ctx->submodule_context.cur_file_index = 0;

	// initialize for reading files
	clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);
	ctx->alignment = clcf->directio_alignment;

	ctx->open_file = ngx_http_vod_init_file_reader_with_fallback;
	ctx->async_reader = (ngx_http_vod_async_read_func_t)ngx_async_file_read;
	ctx->dump_request = (ngx_http_vod_dump_request_t)ngx_file_reader_dump_file;
	ctx->async_reader_context = &ctx->file_reader;

	// start the state machine
	rc = ngx_http_vod_start_processing_mp4_file(r);
	if (rc != NGX_AGAIN)
	{
		if (rc != NGX_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_local_request_handler: ngx_http_vod_start_processing_mp4_file failed %i", rc);
		}
		return rc;
	}

#if defined nginx_version && nginx_version >= 8011
	r->main->count++;
#endif

	return NGX_DONE;
}

////// Mapped mode only

static void ngx_http_vod_path_request_finished(void* context, ngx_int_t rc, ngx_buf_t* response);

static ngx_int_t
ngx_http_vod_run_mapped_mode_state_machine(ngx_http_request_t *r)
{
	ngx_child_request_params_t child_params;
	ngx_http_core_loc_conf_t *clcf;
	ngx_http_vod_loc_conf_t *conf;
	ngx_http_vod_ctx_t *ctx;
	ngx_str_t path;
	ngx_int_t rc;
	u_char* path_buffer;
	size_t path_size;

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);
	conf = ctx->submodule_context.conf;

	// map all uris to paths
	for (;
		ctx->submodule_context.cur_file_index < ctx->submodule_context.request_params.suburi_count;
		ctx->submodule_context.cur_suburi++, ctx->submodule_context.cur_file_index++)
	{
		if (conf->path_mapping_cache_zone != NULL)
		{
			ngx_http_vod_init_file_key(
				r,
				(r->headers_in.host != NULL ? &r->headers_in.host->value : NULL),
				&ctx->submodule_context.cur_suburi->stripped_uri);

			// try getting the file path from cache
			if (ngx_buffer_cache_fetch(
				conf->path_mapping_cache_zone,
				ctx->submodule_context.request_params.file_key,
				&path_buffer,
				&path_size))
			{
				// copy the path since the cache buffer should not be held for long
				path.len = path_size;
				path.data = ngx_palloc(r->pool, path.len + 1);
				if (path.data == NULL)
				{
					ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
						"ngx_http_vod_run_mapped_mode_state_machine: ngx_palloc failed");
					return NGX_HTTP_INTERNAL_SERVER_ERROR;
				}

				ngx_memcpy(path.data, path_buffer, path.len);
				path.data[path.len] = '\0';

				ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
					"ngx_http_vod_run_mapped_mode_state_machine: path mapping cache hit %V", &path);

				// replace the stripped uri with the corresponding path
				ctx->submodule_context.cur_suburi->stripped_uri = path;
				continue;
			}
			else
			{
				ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
					"ngx_http_vod_run_mapped_mode_state_machine: path mapping cache miss");
			}
		}

		// get the mp4 file path from upstream
		r->connection->log->action = "getting file path";

		ngx_memzero(&child_params, sizeof(child_params));
		child_params.method = NGX_HTTP_GET;
		child_params.base_uri = ctx->submodule_context.cur_suburi->stripped_uri;
		child_params.extra_args = conf->upstream_extra_args;
		child_params.host_name = conf->upstream_host_header;

		rc = ngx_child_request_start(
			r,
			&ctx->child_request_buffers,
			ngx_http_vod_path_request_finished,
			r,
			&conf->upstream,
			&conf->child_request_location,
			&child_params,
			conf->path_response_prefix.len + conf->max_path_length + conf->path_response_postfix.len,
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

	// restart the file index/uri params
	ctx->submodule_context.cur_suburi = ctx->submodule_context.request_params.suburis;
	ctx->submodule_context.cur_file_index = 0;

	// initialize for reading files
	clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);
	ctx->alignment = clcf->directio_alignment;

	ctx->open_file = ngx_http_vod_init_file_reader;
	ctx->async_reader = (ngx_http_vod_async_read_func_t)ngx_async_file_read;
	ctx->dump_request = (ngx_http_vod_dump_request_t)ngx_file_reader_dump_file;
	ctx->async_reader_context = &ctx->file_reader;

	// run the main state machine
	return ngx_http_vod_start_processing_mp4_file(r);
}

static void 
ngx_http_vod_path_request_finished(void* context, ngx_int_t rc, ngx_buf_t* response)
{
	ngx_http_vod_loc_conf_t *conf;
	ngx_http_vod_ctx_t *ctx;
	ngx_http_request_t *r = context;
	ngx_str_t path;

	if (rc != NGX_OK)
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_path_request_finished: upstream request failed %i", rc);
		goto finalize_request;
	}

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);

	ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "ngx_http_vod_path_request_finished: result %s", response->pos);

	path.data = response->pos;
	path.len = response->last - path.data;

	if (path.len == 0)
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_path_request_finished: empty path mapping response");
		rc = NGX_HTTP_NOT_FOUND;
		goto finalize_request;
	}

	// strip off the prefix and postfix of the path response
	conf = ctx->submodule_context.conf;
	if (path.len < conf->path_response_prefix.len + conf->path_response_postfix.len ||
		ngx_memcmp(path.data, conf->path_response_prefix.data, conf->path_response_prefix.len) != 0 ||
		ngx_memcmp(path.data + path.len - conf->path_response_postfix.len, conf->path_response_postfix.data, conf->path_response_postfix.len) != 0)
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_path_request_finished: unexpected path mapping response %V", &path);
		rc = NGX_HTTP_SERVICE_UNAVAILABLE;
		goto finalize_request;
	}

	path.data += conf->path_response_prefix.len;
	path.len -= conf->path_response_prefix.len + conf->path_response_postfix.len;
	path.data[path.len] = '\0';

	if (path.len <= 0)
	{
		// file not found
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_path_request_finished: empty path returned from upstream");

		// try the fallback
		rc = ngx_http_vod_dump_request_to_fallback(r);
		if (rc != NGX_AGAIN)
		{
			rc = NGX_HTTP_NOT_FOUND;
			goto finalize_request;
		}
		return;
	}

	// save to cache
	if (conf->path_mapping_cache_zone != NULL)
	{
		if (ngx_buffer_cache_store(
			conf->path_mapping_cache_zone,
			ctx->submodule_context.request_params.file_key,
			path.data,
			path.len))
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

	// save the path
	ctx->submodule_context.cur_suburi->stripped_uri = path;

	// move to the next suburi
	ctx->submodule_context.cur_suburi++;
	ctx->submodule_context.cur_file_index++;

	// run the state machine
	rc = ngx_http_vod_run_mapped_mode_state_machine(r);
	if (rc != NGX_AGAIN)
	{
		if (rc != NGX_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_path_request_finished: ngx_http_vod_run_mapped_mode_state_machine failed %i", rc);
		}
		goto finalize_request;
	}

	return;

finalize_request:

	ngx_http_vod_finalize_request(r, rc);
}

ngx_int_t
ngx_http_vod_mapped_request_handler(ngx_http_request_t *r)
{
	ngx_int_t rc;

	rc = ngx_http_vod_run_mapped_mode_state_machine(r);
	if (rc != NGX_AGAIN)
	{
		if (rc != NGX_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_mapped_request_handler: ngx_http_vod_run_mapped_mode_state_machine failed %i", rc);
		}
		return rc;
	}

#if defined nginx_version && nginx_version >= 8011
	r->main->count++;
#endif

	return NGX_DONE;
}

////// Remote mode only

static void
ngx_http_vod_http_read_completed(void* context, ngx_int_t rc, ngx_buf_t* response)
{
	ngx_http_vod_ctx_t *ctx = (ngx_http_vod_ctx_t *)context;

	if (rc != NGX_OK)
	{
		ngx_log_error(NGX_LOG_ERR, ctx->submodule_context.request_context.log, 0,
			"ngx_http_vod_http_read_completed: upstream request failed %i", rc);
		ngx_http_vod_finalize_request(ctx->submodule_context.r, rc);
		return;
	}

	ngx_http_vod_handle_read_completed(context, NGX_OK, response->last - response->pos);
}

static ngx_int_t
ngx_http_vod_async_http_read(ngx_http_request_t *r, u_char *buf, size_t size, off_t offset)
{
	ngx_http_vod_loc_conf_t *conf;
	ngx_http_vod_ctx_t *ctx;
	ngx_child_request_params_t child_params;

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);
	conf = ctx->submodule_context.conf;

	ngx_memzero(&child_params, sizeof(child_params));
	child_params.method = NGX_HTTP_GET;
	child_params.base_uri = ctx->cur_remote_suburi;
	child_params.extra_args = conf->upstream_extra_args;
	child_params.host_name = conf->upstream_host_header;
	child_params.range_start = offset;
	child_params.range_end = offset + size;

	return ngx_child_request_start(
		r, 
		&ctx->child_request_buffers,
		ngx_http_vod_http_read_completed, 
		ctx,
		&conf->upstream,
		&conf->child_request_location,
		&child_params,
		size, 
		buf);
}

ngx_int_t 
ngx_http_vod_dump_http_request(ngx_http_request_t *r)
{
	ngx_http_vod_loc_conf_t *conf;
	ngx_child_request_params_t child_params;

	conf = ngx_http_get_module_loc_conf(r, ngx_http_vod_module);

	ngx_memzero(&child_params, sizeof(child_params));
	child_params.method = r->method;
	child_params.base_uri = r->uri;
	child_params.extra_args = conf->upstream_extra_args;
	child_params.host_name = conf->upstream_host_header;
	child_params.proxy_range = 1;
	child_params.proxy_accept_encoding = 1;

	return ngx_dump_request(
		r,
		&conf->upstream,
		&child_params);
}

static ngx_int_t
ngx_http_vod_http_reader_open_file(ngx_http_request_t* r, ngx_str_t* path)
{
	ngx_http_vod_ctx_t *ctx;

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);

	// Note: since this remote mode, no need to open any files, just save the remote uri
	ctx->cur_remote_suburi = *path;

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
	ctx->dump_request = (ngx_http_vod_dump_request_t)ngx_http_vod_dump_http_request;
	ctx->async_reader_context = r;
	ctx->file_key_prefix = (r->headers_in.host != NULL ? &r->headers_in.host->value : NULL);

	rc = ngx_http_vod_start_processing_mp4_file(r);
	if (rc != NGX_AGAIN)
	{
		if (rc != NGX_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_remote_request_handler: ngx_http_vod_start_processing_mp4_file failed %i", rc);
		}
		return rc;
	}

#if defined nginx_version && nginx_version >= 8011
	r->main->count++;
#endif

	return NGX_DONE;
}

////// Main

static ngx_int_t
ngx_http_vod_parse_uri(ngx_http_request_t *r, ngx_http_vod_loc_conf_t *conf, ngx_http_vod_request_params_t* request_params)
{
	ngx_str_t uri_path;
	ngx_str_t uri_file_name;
	ngx_int_t rc;
	int file_components;
	
	file_components = conf->get_file_path_components(&r->uri);

	if (!ngx_http_vod_split_uri_file_name(&r->uri, file_components, &uri_path, &uri_file_name))
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_parse_uri: ngx_http_vod_split_uri_file_name failed");
		return NGX_HTTP_BAD_REQUEST;
	}

	rc = conf->parse_uri_file_name(r, conf, uri_file_name.data, uri_file_name.data + uri_file_name.len, request_params);
	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_parse_uri: parse_uri_file_name failed %i", rc);
		return rc;
	}

	rc = ngx_http_vod_parse_uri_path(r, conf, &uri_path, request_params);
	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_parse_uri: ngx_http_vod_parse_uri_path failed %i", rc);
		return rc;
	}

	if (request_params->suburi_count != 1 && 
		(request_params->request->flags & (REQUEST_FLAG_SINGLE_FILE | REQUEST_FLAG_SINGLE_STREAM)) != 0)
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
	ngx_int_t rc;
	ngx_http_vod_ctx_t *ctx;
	ngx_http_vod_request_params_t request_params;
	ngx_http_vod_loc_conf_t *conf;

	ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "ngx_http_vod_handler: started");

	// we respond to 'GET' and 'HEAD' requests only
	if (!(r->method & (NGX_HTTP_GET | NGX_HTTP_HEAD))) 
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_handler: unsupported method %ui", r->method);
		return NGX_HTTP_NOT_ALLOWED;
	}

	// discard request body, since we don't need it here
	rc = ngx_http_discard_request_body(r);
	if (rc != NGX_OK) 
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_handler: ngx_http_discard_request_body failed %i", rc);
		return rc;
	}

	// parse the uri
	conf = ngx_http_get_module_loc_conf(r, ngx_http_vod_module);

	ngx_memzero(&request_params, sizeof(request_params));
	if (conf->parse_uri_file_name != NULL)
	{
		rc = ngx_http_vod_parse_uri(r, conf, &request_params);
		if (rc != NGX_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_handler: ngx_http_vod_parse_uri failed %i", rc);
			return rc;
		}
	}

	// initialize the context
	ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_vod_ctx_t));
	if (ctx == NULL) 
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_handler: ngx_pcalloc failed");
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	ctx->submodule_context.r = r;
	ctx->submodule_context.conf = conf;
	ctx->submodule_context.request_params = request_params;
	ctx->submodule_context.cur_suburi = request_params.suburis;
	ctx->submodule_context.cur_file_index = 0;

	ngx_http_set_ctx(r, ctx, ngx_http_vod_module);

	// call the mode specific handler (remote/mapped/local)
	rc = conf->request_handler(r);

	ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "ngx_http_vod_handler: done");

	return rc;
}
