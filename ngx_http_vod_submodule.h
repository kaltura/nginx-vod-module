#ifndef _NGX_HTTP_VOD_SUBMODULE_H_INCLUDED_
#define _NGX_HTTP_VOD_SUBMODULE_H_INCLUDED_

// includes
#include "ngx_http_vod_request_parse.h"
#include "vod/common.h"

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

// request classes
#define REQUEST_CLASS_MANIFEST	(0x01)
#define REQUEST_CLASS_SEGMENT	(0x02)
#define REQUEST_CLASS_THUMB		(0x04)
#define REQUEST_CLASS_OTHER		(0x08)		// dash init segment, hls iframes manifest, hls master manifest, hls encryption key

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
	media_set_t media_set;
	request_params_t request_params;
	ngx_http_request_t* r;
	struct ngx_http_vod_loc_conf_s* conf;
} ngx_http_vod_submodule_context_t;

// submodule request
struct ngx_http_vod_request_s {
	int flags;
	int parse_type;
	int request_class;
	int codecs_mask;
	uint32_t timescale;
	
	ngx_int_t (*handle_metadata_request)(
		// in
		ngx_http_vod_submodule_context_t* submodule_context,
		// out
		ngx_str_t* response,
		ngx_str_t* content_type);
		
	ngx_int_t (*init_frame_processor)(
		// in
		ngx_http_vod_submodule_context_t* submodule_context,
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
		request_params_t* request_params,
		const ngx_http_vod_request_t** request);

	ngx_int_t (*parse_drm_info)(
		ngx_http_vod_submodule_context_t* submodule_context,
		ngx_str_t* drm_info,
		void** output);

} ngx_http_vod_submodule_t;

// globals
extern const ngx_http_vod_submodule_t* submodules[];

#endif // _NGX_HTTP_VOD_SUBMODULE_H_INCLUDED_
