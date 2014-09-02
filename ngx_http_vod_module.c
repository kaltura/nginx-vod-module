#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <nginx.h>
#include <ngx_event.h>
#include <ngx_md5.h>

#include "ngx_http_vod_module.h"
#include "ngx_http_vod_request_parse.h"
#include "ngx_child_http_request.h"
#include "ngx_http_vod_utils.h"
#include "ngx_http_vod_conf.h"
#include "ngx_file_reader.h"
#include "ngx_buffer_cache.h"
#include "vod/aes_encrypt.h"
#include "vod/m3u8_builder.h"
#include "vod/mp4_parser.h"
#include "vod/read_cache.h"
#include "vod/muxer.h"

// enums
enum {
	STATE_INITIAL_READ,
	STATE_MOOV_ATOM_READ,
	STATE_MOOV_ATOM_PARSED,
	STATE_FRAME_DATA_READ,
};

// typedefs
typedef ngx_int_t (*ngx_http_vod_async_read_func_t)(void* context, u_char *buf, size_t size, off_t offset);
typedef ngx_int_t (*ngx_http_vod_dump_request_t)(void* context);

typedef struct {
	ngx_http_request_t* r;
	ngx_chain_t* chain_end;
	uint32_t total_size;
} ngx_http_vod_write_ts_context_t;

typedef struct {
	// file read state (either remote or local)
	ngx_file_reader_state_t file_reader;
	ngx_http_vod_async_read_func_t async_reader;
	ngx_http_vod_dump_request_t ngx_dump_request;
	void* async_reader_context;
	ngx_child_request_buffers_t child_request_buffers;
	u_char* read_buffer;
	size_t buffer_size;

	// input params
	ngx_http_request_t *r;
	ngx_str_t original_uri;
	ngx_http_vod_request_params_t request_params;
	request_context_t request_context;
	u_char file_key[BUFFER_CACHE_KEY_SIZE];
	off_t alignment;

	// common state
	int state;
	off_t moov_offset;
	size_t moov_size;
	mpeg_metadata_t mpeg_metadata;

	// TS requests only
	read_cache_state_t read_cache_state;
	muxer_state_t muxer;
	ngx_chain_t  out;
	ngx_http_vod_write_ts_context_t write_ts_buffer_context;
	aes_encrypt_context_t* encrypted_write_context;
} ngx_http_vod_ctx_t;

// content types
static u_char m3u8_content_type[] = "application/vnd.apple.mpegurl";
static u_char mpeg_ts_content_type[] = "video/MP2T";
static u_char encryption_key_content_type[] = "application/octet-stream";

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

static ngx_int_t
ngx_http_vod_process_enc_key_request(ngx_http_request_t *r, u_char* file_key)
{
	ngx_str_t response;
	u_char* encryption_key;

	encryption_key = ngx_palloc(r->pool, BUFFER_CACHE_KEY_SIZE);
	if (encryption_key == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_process_enc_key_request: ngx_palloc failed");
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	ngx_memcpy(encryption_key, file_key, BUFFER_CACHE_KEY_SIZE);

	response.data = encryption_key;
	response.len = BUFFER_CACHE_KEY_SIZE;

	return ngx_http_vod_send_response(r, &response, (u_char *)encryption_key_content_type, sizeof(encryption_key_content_type) - 1);
}

static vod_status_t
ngx_http_vod_init_aes_encryption(ngx_http_vod_ctx_t *ctx, write_callback_t write_callback, void* callback_context)
{
	ngx_pool_cleanup_t  *cln;
	vod_status_t rc;

	ctx->encrypted_write_context = ngx_palloc(ctx->r->pool, sizeof(*ctx->encrypted_write_context));
	if (ctx->encrypted_write_context == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ctx->request_context.log, 0,
			"ngx_http_vod_init_aes_encryption: ngx_palloc failed");
		return VOD_ALLOC_FAILED;
	}

	cln = ngx_pool_cleanup_add(ctx->r->pool, 0);
	if (cln == NULL) 
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ctx->request_context.log, 0,
			"ngx_http_vod_init_aes_encryption: ngx_pool_cleanup_add failed");
		return VOD_ALLOC_FAILED;
	}

	cln->handler = (ngx_pool_cleanup_pt)aes_encrypt_cleanup;
	cln->data = ctx->encrypted_write_context;

	rc = aes_encrypt_init(ctx->encrypted_write_context, &ctx->request_context, write_callback, callback_context, ctx->file_key, ctx->request_params.segment_index);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->request_context.log, 0,
			"ngx_http_vod_init_aes_encryption: aes_encrypt_init failed %i", rc);
		return rc;
	}

	return VOD_OK;
}

////// Common mp4 processing

static ngx_int_t 
ngx_http_vod_read_moov_atom(ngx_http_vod_ctx_t *ctx)
{
	ngx_http_vod_loc_conf_t   *conf;
	u_char* new_read_buffer = NULL;
	size_t new_buffer_size;
	vod_status_t rc;

	// get moov atom offset and size
	rc = mp4_parser_get_moov_atom_info(&ctx->request_context, ctx->read_buffer, ctx->buffer_size, &ctx->moov_offset, &ctx->moov_size);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->request_context.log, 0,
			"ngx_http_vod_read_moov_atom: mp4_parser_get_moov_atom_info failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(rc);
	}

	// check whether we already have the whole atom
	if (ctx->moov_offset + ctx->moov_size < ctx->buffer_size)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ctx->request_context.log, 0,
			"ngx_http_vod_read_moov_atom: already read the full moov atom");
		return NGX_OK;
	}

	// validate the moov size
	conf = ngx_http_get_module_loc_conf(ctx->r, ngx_http_vod_module);
	if (ctx->moov_size > conf->max_moov_size)
	{
		ngx_log_error(NGX_LOG_ERR, ctx->request_context.log, 0,
			"ngx_http_vod_read_moov_atom: moov size %uD exceeds the max %uz", ctx->moov_size, conf->max_moov_size);
		return ngx_http_vod_status_to_ngx_error(VOD_BAD_DATA);
	}

	// calculate the new buffer size (round the moov end up to alignment)
	new_buffer_size = ((ctx->moov_offset + ctx->moov_size) + ctx->alignment - 1) & (~(ctx->alignment - 1));

	new_read_buffer = ngx_pmemalign(ctx->r->pool, new_buffer_size, ctx->alignment);
	if (new_read_buffer == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ctx->request_context.log, 0,
			"ngx_http_vod_read_moov_atom: ngx_pmemalign failed");
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	// copy the previously read data
	ngx_memcpy(new_read_buffer, ctx->read_buffer, ctx->buffer_size);
	ngx_pfree(ctx->r->pool, ctx->read_buffer);
	ctx->read_buffer = new_read_buffer;

	// read the rest of the atom
	ctx->request_context.log->action = "reading moov atom";
	ctx->state = STATE_MOOV_ATOM_READ;
	rc = ctx->async_reader(ctx->async_reader_context, ctx->read_buffer + ctx->buffer_size, new_buffer_size - ctx->buffer_size, ctx->buffer_size);
	if (rc < 0)
	{
		if (rc != NGX_AGAIN)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->request_context.log, 0,
				"ngx_http_vod_read_moov_atom: async_reader failed %i", rc);
		}
		return rc;
	}

	ctx->buffer_size += rc;
	return NGX_OK;
}

static vod_status_t 
ngx_http_vod_parse_moov_atom(ngx_http_vod_ctx_t *ctx, u_char* moov_buffer, size_t moov_size)
{
	ngx_http_vod_loc_conf_t   *conf;
	vod_status_t rc;

	// initialize the request context
	switch (ctx->request_params.request_type)
	{
	case REQUEST_TYPE_HLS_ENCRYPTION_KEY:
	case REQUEST_TYPE_HLS_PLAYLIST:
		ctx->request_context.start = 0;
		ctx->request_context.end = 0;
		ctx->request_context.max_frame_count = 0;
		ctx->request_context.simulation_only = TRUE;
		ctx->request_context.parse_type = PARSE_INDEX;
		break;

	case REQUEST_TYPE_HLS_IFRAME_PLAYLIST:
		ctx->request_context.start = ctx->request_params.clip_from;
		ctx->request_context.end = ctx->request_params.clip_to;
		ctx->request_context.max_frame_count = 1024 * 1024;
		ctx->request_context.simulation_only = TRUE;
		ctx->request_context.parse_type = PARSE_IFRAMES;
		break;

	case REQUEST_TYPE_HLS_SEGMENT:
		conf = ngx_http_get_module_loc_conf(ctx->r, ngx_http_vod_module);
		ctx->request_context.start = ctx->request_params.clip_from + ctx->request_params.segment_index * conf->segment_duration;
		ctx->request_context.end = MIN(ctx->request_context.start + conf->segment_duration, ctx->request_params.clip_to);
		if (ctx->request_context.end <= ctx->request_context.start)
		{
			vod_log_error(VOD_LOG_ERR, ctx->request_context.log, 0,
				"ngx_http_vod_parse_moov_atom: segment index %uD too big for clip from %uD and clip to %uD", 
				ctx->request_params.segment_index, ctx->request_params.clip_from, ctx->request_params.clip_to);
			return VOD_BAD_REQUEST;
		}
		ctx->request_context.max_frame_count = 16 * 1024;
		ctx->request_context.simulation_only = FALSE;
		ctx->request_context.parse_type = PARSE_SEGMENT;
		break;
	}

	// parse the moov atom
	rc = mp4_parser_parse_moov_atom(
		&ctx->request_context, 
		ctx->request_params.required_tracks, 
		moov_buffer, 
		moov_size,
		&ctx->mpeg_metadata);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->request_context.log, 0,
			"ngx_http_vod_parse_moov_atom: mp4_parser_parse_moov_atom failed %i", rc);
		return rc;
	}

	return VOD_OK;
}

////// TS segment request handling

static vod_status_t 
ngx_http_vod_write_ts_buffer(void* ctx, u_char* buffer, uint32_t size, bool_t* reuse_buffer)
{
	ngx_http_vod_write_ts_context_t* context = (ngx_http_vod_write_ts_context_t*)ctx;
	ngx_buf_t   *b;
	ngx_chain_t  *chain;
	ngx_chain_t  out;
	ngx_int_t rc;

	// caller should not reuse the buffer since keep the original buffer
	*reuse_buffer = FALSE;

	// create a wrapping ngx_buf_t
	b = ngx_pcalloc(context->r->pool, sizeof(ngx_buf_t));
	if (b == NULL) 
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, context->r->connection->log, 0,
			"ngx_http_vod_write_ts_buffer: ngx_pcalloc failed (1)");
		return VOD_ALLOC_FAILED;
	}

	b->pos = buffer;
	b->last = buffer + size;
	b->memory = 1;    // this buffer is in memory
	b->last_buf = 0;  // not the last buffer in the buffer chain

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
				"ngx_http_vod_write_ts_buffer: ngx_http_output_filter failed %i", rc);
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
					"ngx_http_vod_write_ts_buffer: ngx_pcalloc failed (2)");
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
	ngx_http_request_t* r = ctx->r;
	ngx_http_vod_loc_conf_t   *conf;
	write_callback_t write_callback;
	uint32_t response_size;
	bool_t simulation_supported;
	void* write_context;
	ngx_int_t rc;

	// enable directio if enabled in the configuration (ignore errors)
	// Note that directio is set on transfer only to allow the kernel to cache the "moov" atom
	conf = ngx_http_get_module_loc_conf(ctx->r, ngx_http_vod_module);
	if (conf->request_handler != ngx_http_vod_remote_request_handler)
	{
		ngx_file_reader_enable_directio(&ctx->file_reader);
	}

	// initialize the read cache
	read_cache_init(&ctx->read_cache_state, &ctx->request_context, conf->cache_buffer_size, ctx->alignment);

	// initialize the response writer
	ctx->out.buf = NULL;
	ctx->out.next = NULL;
	ctx->write_ts_buffer_context.r = r;
	ctx->write_ts_buffer_context.chain_end = &ctx->out;
	ctx->write_ts_buffer_context.total_size = 0;

	if (conf->secret_key.len != 0)
	{
		rc = ngx_http_vod_init_aes_encryption(ctx, ngx_http_vod_write_ts_buffer, &ctx->write_ts_buffer_context);
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
		write_callback = ngx_http_vod_write_ts_buffer;
		write_context = &ctx->write_ts_buffer_context;
	}

	// initialize the muxer
	rc = muxer_init(
		&ctx->muxer, 
		&ctx->request_context, 
		ctx->request_params.segment_index, 
		&ctx->mpeg_metadata, 
		&ctx->read_cache_state, 
		write_callback, 
		write_context, 
		&simulation_supported);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_init_frame_processing: muxer_init failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(rc);
	}

	// set the 'Content-type' header
	r->headers_out.content_type_len = sizeof(mpeg_ts_content_type)-1;
	r->headers_out.content_type.len = sizeof(mpeg_ts_content_type)-1;
	r->headers_out.content_type.data = (u_char *)mpeg_ts_content_type;

	// if simulation is not supported we have to build the whole response before we can start sending it
	if (!simulation_supported)
	{
		return NGX_OK;
	}

	// calculate the response size
	response_size = muxer_simulate_get_segment_size(&ctx->muxer);
	if (conf->secret_key.len != 0)
	{
		response_size = AES_ROUND_TO_BLOCK(response_size);
	}
	muxer_simulation_reset(&ctx->muxer);

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

	return NGX_OK;
}

static ngx_int_t 
ngx_http_vod_output_ts_response(ngx_http_vod_ctx_t *ctx)
{
	ngx_http_request_t *r = ctx->r;
	ngx_int_t    rc;

	// flush the encryption buffer
	if (ctx->encrypted_write_context != NULL)
	{
		rc = aes_encrypt_flush(ctx->encrypted_write_context);
		if (rc != VOD_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_output_ts_response: aes_encrypt_flush failed %i", rc);
			return ngx_http_vod_status_to_ngx_error(rc);
		}
	}

	// if we already sent the headers and all the buffers, just signal completion and return
	if (r->header_sent)
	{
		if (ctx->write_ts_buffer_context.total_size != r->headers_out.content_length_n)
		{
			ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
				"ngx_http_vod_output_ts_response: actual content length %uD is different than reported length %O", 
				ctx->write_ts_buffer_context.total_size, r->headers_out.content_length_n);
		}

		rc = ngx_http_send_special(r, NGX_HTTP_LAST);
		if (rc != NGX_OK && rc != NGX_AGAIN)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_output_ts_response: ngx_http_send_special failed %i", rc);
			return rc;
		}
		return NGX_OK;
	}

	// mark the current buffer as last
	ctx->write_ts_buffer_context.chain_end->next = NULL;
	ctx->write_ts_buffer_context.chain_end->buf->last_buf = 1;

	// set the status line
	r->headers_out.status = NGX_HTTP_OK;
	r->headers_out.content_length_n = ctx->write_ts_buffer_context.total_size;

	// set the etag
	rc = ngx_http_set_etag(r);
	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_output_ts_response: ngx_http_set_etag failed %i", rc);
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	// send the response headers
	rc = ngx_http_send_header(r);
	if (rc == NGX_ERROR || rc > NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_output_ts_response: ngx_http_send_header failed %i", rc);
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
			"ngx_http_vod_output_ts_response: ngx_http_output_filter failed %i", rc);
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
		rc = muxer_process(&ctx->muxer, &required_offset);
		switch (rc)
		{
		case VOD_OK:
			// we're done
			return NGX_OK;

		case VOD_AGAIN:
			// handled outside the switch
			break;

		default:
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->request_context.log, 0,
				"ngx_http_vod_process_mp4_frames: muxer_process failed %i", rc);
			return ngx_http_vod_status_to_ngx_error(rc);
		}

		// get a buffer to read into
		rc = read_cache_get_read_buffer(&ctx->read_cache_state, required_offset, &read_offset, &read_buffer, &read_size);
		if (rc != VOD_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->request_context.log, 0,
				"ngx_http_vod_process_mp4_frames: read_cache_get_read_buffer failed %i", rc);
			return ngx_http_vod_status_to_ngx_error(rc);
		}

		// perform the read
		rc = ctx->async_reader(ctx->async_reader_context, read_buffer, read_size, read_offset);
		if (rc < 0)
		{
			if (rc != NGX_AGAIN)
			{
				ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->request_context.log, 0,
					"ngx_http_vod_process_mp4_frames: async_reader failed %i", rc);
			}
			return rc;
		}

		// read completed synchronously, update the read cache
		read_cache_read_completed(&ctx->read_cache_state, rc);
	}
}

////// Common

static ngx_int_t 
ngx_http_vod_run_state_machine(ngx_http_vod_ctx_t *ctx)
{
	ngx_http_vod_loc_conf_t *conf;
	uint32_t duration_millis;
	uint32_t segment_count;
	ngx_str_t response;
	ngx_int_t rc;

	conf = ngx_http_get_module_loc_conf(ctx->r, ngx_http_vod_module);

	switch (ctx->state)
	{
	case STATE_INITIAL_READ:
		// read the entire atom
		rc = ngx_http_vod_read_moov_atom(ctx);
		if (rc == NGX_AGAIN)
		{
			return NGX_AGAIN;
		}

		if (rc != NGX_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->request_context.log, 0,
				"ngx_http_vod_run_state_machine: ngx_http_vod_read_moov_atom failed %i", rc);
			return rc;
		}
		// fallthrough

	case STATE_MOOV_ATOM_READ:
		// make sure we got the whole moov atom
		if (ctx->buffer_size < ctx->moov_offset + ctx->moov_size)
		{
			ngx_log_error(NGX_LOG_ERR, ctx->request_context.log, 0,
				"ngx_http_vod_run_state_machine: buffer size %uD is smaller than moov end offset %uD", ctx->buffer_size, ctx->moov_offset + ctx->moov_size);
			return ngx_http_vod_status_to_ngx_error(VOD_BAD_DATA);
		}

		// parse the moov atom
		rc = ngx_http_vod_parse_moov_atom(ctx, ctx->read_buffer + ctx->moov_offset, ctx->moov_size);
		if (rc != VOD_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->request_context.log, 0,
				"ngx_http_vod_run_state_machine: ngx_http_vod_parse_moov_atom failed %i", rc);
			return ngx_http_vod_status_to_ngx_error(rc);
		}

		// save the moov atom to cache
		if (conf->moov_cache_zone != NULL)
		{
			if (ngx_buffer_cache_store(
				conf->moov_cache_zone,
				ctx->file_key,
				ctx->read_buffer + ctx->moov_offset,
				ctx->moov_size))
			{
				ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ctx->request_context.log, 0,
					"ngx_http_vod_run_state_machine: stored moov atom in cache");
			}
			else
			{
				ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ctx->request_context.log, 0,
					"ngx_http_vod_run_state_machine: failed to store moov atom in cache");
			}
		}

		// no longer need the moov atom buffer
		ngx_pfree(ctx->r->pool, ctx->read_buffer);
		ctx->read_buffer = NULL;
		// fallthrough

	case STATE_MOOV_ATOM_PARSED:
		// handle playlist requests
		if (ctx->request_params.request_type != REQUEST_TYPE_HLS_SEGMENT)
		{
			switch (ctx->request_params.request_type)
			{
			case REQUEST_TYPE_HLS_ENCRYPTION_KEY:
				// note: this type of requests can be served without even reading the file, however reading
				// the file blocks the possibility of an attacker to get the server to sign arbitrary strings.
				// also, the moov atom will usually return from cache
				return ngx_http_vod_process_enc_key_request(ctx->r, ctx->file_key);

			case REQUEST_TYPE_HLS_PLAYLIST:
				rc = m3u8_builder_build_index_playlist(
					&ctx->request_context,
					&conf->m3u8_config,
					conf->segment_duration,
					ctx->request_params.clip_to,
					ctx->request_params.clip_from,
					&ctx->mpeg_metadata,
					&response);
				break;

			case REQUEST_TYPE_HLS_IFRAME_PLAYLIST:
				rc = m3u8_builder_build_iframe_playlist(
					&ctx->request_context,
					&conf->m3u8_config, conf->segment_duration,
					&ctx->mpeg_metadata,
					&response);
				break;
			}

			if (rc != VOD_OK)
			{
				ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->request_context.log, 0,
					"ngx_http_vod_run_state_machine: build_playlist_m3u8 failed %i", rc);
				return ngx_http_vod_status_to_ngx_error(rc);
			}

			return ngx_http_vod_send_response(ctx->r, &response, m3u8_content_type, sizeof(m3u8_content_type)-1);
		}

		// validate the requested segment index
		duration_millis = DIV_CEIL(ctx->mpeg_metadata.duration, 90);

		if (duration_millis <= ctx->request_params.clip_from)
		{
			ngx_log_error(NGX_LOG_ERR, ctx->request_context.log, 0,
				"ngx_http_vod_run_state_machine: clip from %uD exceeds video duration %uD", ctx->request_params.clip_from, duration_millis);
			return NGX_HTTP_NOT_FOUND;
		}

		duration_millis = MIN(duration_millis, ctx->request_params.clip_to) - ctx->request_params.clip_from;
		segment_count = DIV_CEIL(duration_millis, conf->segment_duration);
		if (ctx->request_params.segment_index >= segment_count)
		{
			ngx_log_error(NGX_LOG_ERR, ctx->request_context.log, 0,
				"ngx_http_vod_run_state_machine: requested segment index %uD exceeds the segment count %uD", ctx->request_params.segment_index, segment_count);
			return NGX_HTTP_NOT_FOUND;
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
				ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->request_context.log, 0,
					"ngx_http_vod_run_state_machine: ngx_http_vod_init_frame_processing failed %i", rc);
			}
			return rc;
		}

		ctx->request_context.log->action = "processing frames";
		ctx->state = STATE_FRAME_DATA_READ;
		// fallthrough

	case STATE_FRAME_DATA_READ:
		// process some frames
		rc = ngx_http_vod_process_mp4_frames(ctx);
		if (rc == NGX_OK)
		{
			// we're done
			rc = ngx_http_vod_output_ts_response(ctx);
		}
		else if (rc != NGX_AGAIN)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->request_context.log, 0,
				"ngx_http_vod_run_state_machine: ngx_http_vod_process_mp4_frames failed %i", rc);
		}
		return rc;

	default:
		ngx_log_error(NGX_LOG_ERR, ctx->request_context.log, 0,
			"ngx_http_vod_run_state_machine: invalid state %d", ctx->state);
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}
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
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->request_context.log, 0,
			"ngx_http_vod_handle_read_completed: read failed %i", rc);
		goto finalize_request;
	}

	if (bytes_read <= 0)
	{
		ngx_log_error(NGX_LOG_ERR, ctx->request_context.log, 0,
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

	ngx_http_vod_finalize_request(ctx->r, rc);
}

static ngx_int_t 
ngx_http_vod_start_processing_mp4_file(ngx_http_request_t *r)
{
	ngx_http_vod_loc_conf_t   *conf;
	ngx_http_vod_ctx_t        *ctx;
	u_char* moov_buffer;
	size_t moov_size;
	ngx_int_t rc;

	// update request flags
	r->root_tested = !r->error_page;
	r->allow_ranges = 1;

	// handle serve requests
	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);
	if (ctx->request_params.request_type == REQUEST_TYPE_SERVE_FILE)
	{
		return ctx->ngx_dump_request(ctx->async_reader_context);
	}

	// preliminary initialization of the request context
	ctx->request_context.pool = r->pool;
	ctx->request_context.log = r->connection->log;

	// try to fetch the moov atom from cache
	conf = ngx_http_get_module_loc_conf(r, ngx_http_vod_module);
	if (conf->moov_cache_zone != NULL)
	{
		if (ngx_buffer_cache_fetch(conf->moov_cache_zone, ctx->file_key, &moov_buffer, &moov_size))
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_start_processing_mp4_file: moov atom cache hit, size is %uz", moov_size);

			// parse the moov atom
			rc = ngx_http_vod_parse_moov_atom(ctx, moov_buffer, moov_size);
			if (rc != VOD_OK)
			{
				ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
					"ngx_http_vod_start_processing_mp4_file: ngx_http_vod_parse_moov_atom failed %i", rc);
				return ngx_http_vod_status_to_ngx_error(rc);
			}

			// run the state machine
			ctx->state = STATE_MOOV_ATOM_PARSED;
			return ngx_http_vod_run_state_machine(ctx);
		}
		else
		{
			ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_start_processing_mp4_file: moov atom cache miss");
		}
	}

	// allocate the initial read buffer
	ctx->read_buffer = ngx_pmemalign(r->pool, conf->initial_read_size, ctx->alignment);
	if (ctx->read_buffer == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_start_processing_mp4_file: ngx_pmemalign failed");
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	// read the file header
	r->connection->log->action = "reading mp4 header";
	ctx->state = STATE_INITIAL_READ;
	rc = ctx->async_reader(ctx->async_reader_context, ctx->read_buffer, conf->initial_read_size, 0);
	if (rc < 0)		// inc. NGX_AGAIN
	{
		if (rc != NGX_AGAIN)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_start_processing_mp4_file: async_reader failed %i", rc);
		}
		return rc;
	}

	// read completed synchronously
	ctx->buffer_size = rc;
	return ngx_http_vod_run_state_machine(ctx);
}

static void
ngx_http_vod_init_file_key(ngx_http_request_t* r, ngx_str_t* path1, ngx_str_t* path2)
{
	ngx_http_vod_loc_conf_t   *conf;
	ngx_http_vod_ctx_t        *ctx;
	ngx_md5_t md5;

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);
	conf = ngx_http_get_module_loc_conf(r, ngx_http_vod_module);

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
	ngx_md5_final(ctx->file_key, &md5);
}

////// Local & mapped modes

static ngx_int_t
ngx_http_vod_init_file_reader(ngx_http_request_t *r, ngx_str_t* path)
{
	ngx_http_core_loc_conf_t  *clcf;
	ngx_http_vod_ctx_t        *ctx;
	ngx_int_t rc;

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);

	clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

	rc = ngx_file_reader_init(&ctx->file_reader, ngx_http_vod_handle_read_completed, ctx, r, clcf, path);
	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_init_file_reader: ngx_file_reader_init failed %i", rc);
		return rc;
	}

	ctx->alignment = clcf->directio_alignment;

	ctx->async_reader = (ngx_http_vod_async_read_func_t)ngx_async_file_read;
	ctx->ngx_dump_request = (ngx_http_vod_dump_request_t)ngx_file_reader_dump_file;
	ctx->async_reader_context = &ctx->file_reader;

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_dump_request_to_fallback(
	ngx_http_request_t *r)
{
	ngx_child_request_params_t child_params;
	ngx_http_vod_loc_conf_t *conf;
	ngx_http_vod_ctx_t *ctx;
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
	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);
	ngx_memzero(&child_params, sizeof(child_params));
	child_params.method = r->method;
	child_params.base_uri = ctx->original_uri;
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

ngx_int_t
ngx_http_vod_local_request_handler(ngx_http_request_t *r)
{
	ngx_int_t                       rc;
	u_char                    *last;
	size_t                     root;
	ngx_str_t                  path;

	last = ngx_http_map_uri_to_path(r, &path, &root, 0);
	if (last == NULL) 
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_local_request_handler: ngx_http_map_uri_to_path failed");
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	path.len = last - path.data;

	rc = ngx_http_vod_init_file_reader(r, &path);
	if (rc == NGX_HTTP_NOT_FOUND)
	{
		// try the fallback
		rc = ngx_http_vod_dump_request_to_fallback(r);
		if (rc != NGX_AGAIN)
		{
			return NGX_HTTP_NOT_FOUND;
		}

#if defined nginx_version && nginx_version >= 8011
		r->main->count++;
#endif

		return NGX_DONE;
	}

	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_local_request_handler: ngx_http_vod_init_file_reader failed %i", rc);
		return rc;
	}

	ngx_http_vod_init_file_key(r, &path, NULL);

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
	conf = ngx_http_get_module_loc_conf(r, ngx_http_vod_module);
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

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);
	ngx_child_request_free_buffers(r->pool, &ctx->child_request_buffers);

	if (conf->path_mapping_cache_zone != NULL)
	{
		if (ngx_buffer_cache_store(
			conf->path_mapping_cache_zone,
			ctx->file_key,
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

	rc = ngx_http_vod_init_file_reader(r, &path);
	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_path_request_finished: ngx_http_vod_init_file_reader failed %i", rc);
		goto finalize_request;
	}

	ngx_http_vod_init_file_key(r, &path, NULL);

	rc = ngx_http_vod_start_processing_mp4_file(r);
	if (rc != NGX_AGAIN)
	{
		if (rc != NGX_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_path_request_finished: ngx_http_vod_start_processing_mp4_file failed %i", rc);
		}
		goto finalize_request;
	}

	return;

finalize_request:

	ngx_http_vod_finalize_request(r, rc);
	return;
}

ngx_int_t
ngx_http_vod_mapped_request_handler(ngx_http_request_t *r)
{
	ngx_http_vod_loc_conf_t   *conf;
	ngx_child_request_params_t child_params;
	ngx_http_vod_ctx_t *ctx;
	ngx_str_t path;
	ngx_int_t rc;
	u_char* path_buffer;
	size_t path_size;

	conf = ngx_http_get_module_loc_conf(r, ngx_http_vod_module);
	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);

	// try to fetch the file path from cache
	if (conf->path_mapping_cache_zone != NULL)
	{
		ngx_http_vod_init_file_key(r, (r->headers_in.host != NULL ? &r->headers_in.host->value : NULL), &r->uri);

		if (ngx_buffer_cache_fetch(conf->path_mapping_cache_zone, ctx->file_key, &path_buffer, &path_size))
		{
			// copy the path since the cache buffer should not be held for long
			path.len = path_size;
			path.data = ngx_palloc(r->pool, path.len + 1);
			if (path.data == NULL)
			{
				ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
					"ngx_http_vod_mapped_request_handler: ngx_palloc failed");
				return NGX_HTTP_INTERNAL_SERVER_ERROR;
			}

			ngx_memcpy(path.data, path_buffer, path.len);
			path.data[path.len] = '\0';

			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_mapped_request_handler: path mapping cache hit %V", &path);

			// init the file reader
			rc = ngx_http_vod_init_file_reader(r, &path);
			if (rc != NGX_OK)
			{
				ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
					"ngx_http_vod_mapped_request_handler: ngx_http_vod_init_file_reader failed %i", rc);
				return rc;
			}

			// recalculate the file key based on the path
			ngx_http_vod_init_file_key(r, &path, NULL);

			// start processing the file
			rc = ngx_http_vod_start_processing_mp4_file(r);
			if (rc != NGX_AGAIN)
			{
				if (rc != NGX_OK)
				{
					ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
						"ngx_http_vod_mapped_request_handler: ngx_http_vod_start_processing_mp4_file failed %i", rc);
				}
				return rc;
			}

#if defined nginx_version && nginx_version >= 8011
			r->main->count++;
#endif

			return NGX_DONE;
		}
		else
		{
			ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_mapped_request_handler: path mapping cache miss");
		}
	}

	// get the mp4 file path from upstream
	r->connection->log->action = "getting file path";

	ngx_memzero(&child_params, sizeof(child_params));
	child_params.method = NGX_HTTP_GET;
	child_params.base_uri = r->uri;
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
			"ngx_http_vod_mapped_request_handler: ngx_child_request_start failed %i", rc);
		return rc;
	}

#if defined nginx_version && nginx_version >= 8011
	r->main->count++;
#endif

	return NGX_DONE;
}

////// Remote mode only

static void
ngx_http_vod_http_read_complete(void* context, ngx_int_t rc, ngx_buf_t* response)
{
	ngx_http_vod_ctx_t *ctx = (ngx_http_vod_ctx_t *)context;

	if (rc != NGX_OK)
	{
		ngx_log_error(NGX_LOG_ERR, ctx->request_context.log, 0,
			"ngx_http_vod_http_read_complete: upstream request failed %i", rc);
		ngx_http_vod_finalize_request(ctx->r, rc);
		return;
	}

	ngx_http_vod_handle_read_completed(context, NGX_OK, response->last - response->pos);
}

static ngx_int_t
ngx_http_vod_async_http_read(ngx_http_request_t *r, u_char *buf, size_t size, off_t offset)
{
	ngx_http_vod_loc_conf_t   *conf;
	ngx_http_vod_ctx_t *ctx;
	ngx_child_request_params_t child_params;

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);

	conf = ngx_http_get_module_loc_conf(r, ngx_http_vod_module);

	ngx_memzero(&child_params, sizeof(child_params));
	child_params.method = NGX_HTTP_GET;
	child_params.base_uri = r->uri;
	child_params.extra_args = conf->upstream_extra_args;
	child_params.host_name = conf->upstream_host_header;
	child_params.range_start = offset;
	child_params.range_end = offset + size;

	return ngx_child_request_start(
		r, 
		&ctx->child_request_buffers,
		ngx_http_vod_http_read_complete, 
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

ngx_int_t
ngx_http_vod_remote_request_handler(ngx_http_request_t *r)
{
	ngx_http_vod_ctx_t *ctx;
	ngx_int_t rc;

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);

	ctx->async_reader = (ngx_http_vod_async_read_func_t)ngx_http_vod_async_http_read;
	ctx->ngx_dump_request = (ngx_http_vod_dump_request_t)ngx_http_vod_dump_http_request;
	ctx->async_reader_context = r;

	ctx->alignment = sizeof(int64_t);		// don't care about alignment when working remote

	ngx_http_vod_init_file_key(r, (r->headers_in.host != NULL ? &r->headers_in.host->value : NULL), &r->uri);

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

ngx_int_t
ngx_http_vod_handler(ngx_http_request_t *r)
{
	ngx_int_t                       rc;
	ngx_http_vod_ctx_t        *ctx;
	ngx_http_vod_request_params_t request_params;
	ngx_http_vod_loc_conf_t   *conf;
	ngx_str_t original_uri;

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

	conf = ngx_http_get_module_loc_conf(r, ngx_http_vod_module);

	original_uri = r->uri;

	rc = conf->request_parser(r, conf, &request_params);
	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_handler: request parser failed %i", rc);
		return rc;
	}

	ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_vod_ctx_t));
	if (ctx == NULL) 
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_handler: ngx_pcalloc failed");
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	ctx->r = r;
	ctx->request_params = request_params;
	ctx->original_uri = original_uri;

	ngx_http_set_ctx(r, ctx, ngx_http_vod_module);

	rc = conf->request_handler(r);

	ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "ngx_http_vod_handler: done");

	return rc;
}
