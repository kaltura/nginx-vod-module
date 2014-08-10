#ifndef _NGX_HTTP_VOD_REQUEST_PARSE_H_INCLUDED_
#define _NGX_HTTP_VOD_REQUEST_PARSE_H_INCLUDED_

#include <ngx_http.h>
#include "ngx_http_vod_conf.h"
#include "vod/mp4_parser.h"

enum {
	REQUEST_TYPE_HLS_PLAYLIST,
	REQUEST_TYPE_HLS_IFRAME_PLAYLIST,
	REQUEST_TYPE_HLS_ENCRYPTION_KEY,
	REQUEST_TYPE_HLS_SEGMENT,
	REQUEST_TYPE_SERVE_FILE,
};

typedef struct {
	int request_type;
	uint32_t segment_index;
	uint32_t required_tracks[MEDIA_TYPE_COUNT];
	uint32_t clip_to;
	uint32_t clip_from;
} request_params_t;

ngx_int_t parse_request_uri(ngx_http_request_t *r, ngx_http_vod_loc_conf_t *conf, request_params_t* request_params);

#endif // _NGX_HTTP_VOD_REQUEST_PARSE_H_INCLUDED_
