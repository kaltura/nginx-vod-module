#ifndef _NGX_HTTP_VOD_SUBMODULE_H_INCLUDED_
#define _NGX_HTTP_VOD_SUBMODULE_H_INCLUDED_

// includes
#include "ngx_http_vod_request_parse.h"
#include "vod/common.h"
#include "vod/read_cache.h"

// macros
#define DEFINE_SUBMODULE(x) const ngx_http_vod_submodule_t x = {	\
		(u_char*)#x,											\
		sizeof(#x) - 1,											\
		offsetof(ngx_http_vod_loc_conf_t, x),					\
		(ngx_http_vod_create_loc_conf_t)ngx_http_vod_##x##_create_loc_conf,	\
		(ngx_http_vod_merge_loc_conf_t)ngx_http_vod_##x##_merge_loc_conf,	\
		ngx_http_vod_##x##_get_file_path_components,			\
		ngx_http_vod_##x##_parse_uri_file_name,					\
		ngx_http_vod_##x##_parse_drm_info,						\
	}

#define ngx_http_vod_submodule_size_only(submodule_context)		\
	((submodule_context)->r->header_only || (submodule_context)->r->method == NGX_HTTP_HEAD)

// request flags
#define REQUEST_FLAG_SINGLE_FILE (0x1)
#define REQUEST_FLAG_SINGLE_STREAM (0x2)
#define REQUEST_FLAG_SINGLE_STREAM_PER_MEDIA_TYPE (0x4)

// request classes
enum {
	REQUEST_CLASS_MANIFEST,
	REQUEST_CLASS_SEGMENT,
	REQUEST_CLASS_OTHER,		// dash init segment, hls iframes manifest, hls master manifest, hls encryption key
};

struct ngx_http_vod_loc_conf_s;

// typedefs
typedef vod_status_t (*ngx_http_vod_frame_processor_t)(void* context);

typedef void (*ngx_http_vod_create_loc_conf_t)(
	ngx_conf_t *cf, 
	void *conf);

typedef char* (*ngx_http_vod_merge_loc_conf_t)(
	ngx_conf_t *cf, 
	struct ngx_http_vod_loc_conf_s *base, 
	void *conf, 
	void *prev);

typedef struct {
	request_context_t request_context;
	ngx_http_vod_request_params_t request_params;
	mpeg_metadata_t mpeg_metadata;
	ngx_http_request_t* r;
	struct ngx_http_vod_loc_conf_s* conf;
	ngx_http_vod_suburi_params_t* cur_suburi;
} ngx_http_vod_submodule_context_t;

// submodule request
struct ngx_http_vod_request_s {
	int flags;
	int parse_type;
	stream_comparator_t stream_comparator;
	int stream_comparator_conf_offset;
	int request_class;
	
	ngx_int_t (*handle_metadata_request)(
		// in
		ngx_http_vod_submodule_context_t* submodule_context,
		// out
		ngx_str_t* response,
		ngx_str_t* content_type);
		
	ngx_int_t (*init_frame_processor)(
		// in
		ngx_http_vod_submodule_context_t* submodule_context,
		read_cache_state_t* read_cache_state,
		segment_writer_t* segment_writer,
		// out
		ngx_http_vod_frame_processor_t* frame_processor,
		void** frame_processor_state,
		ngx_str_t* output_buffer,
		size_t* response_size,
		ngx_str_t* content_type);
};

typedef struct ngx_http_vod_request_s ngx_http_vod_request_t;

// submodule
typedef struct {
	u_char* name;
	size_t name_len;

	int conf_offset;

	ngx_http_vod_create_loc_conf_t create_loc_conf;

	ngx_http_vod_merge_loc_conf_t merge_loc_conf;

	int (*get_file_path_components)
		(ngx_str_t* uri);

	ngx_int_t (*parse_uri_file_name)(
		ngx_http_request_t *r,
		struct ngx_http_vod_loc_conf_s *conf,
		u_char* start_pos,
		u_char* end_pos,
		ngx_http_vod_request_params_t* request_params);

	ngx_int_t (*parse_drm_info)(
		ngx_http_vod_submodule_context_t* submodule_context,
		ngx_str_t* drm_info,
		void** output);
} ngx_http_vod_submodule_t;

// globals
extern const ngx_http_vod_submodule_t* submodules[];

#endif // _NGX_HTTP_VOD_SUBMODULE_H_INCLUDED_
