#ifndef _NGX_HTTP_VOD_REQUEST_PARSE_H_INCLUDED_
#define _NGX_HTTP_VOD_REQUEST_PARSE_H_INCLUDED_

#include <ngx_http.h>
#include "ngx_http_vod_conf.h"
#include "vod/mp4_parser.h"

#define REQUEST_TYPE_PLAYLIST (-1)
#define REQUEST_TYPE_ENCRYPTION_KEY (-2)
#define REQUEST_TYPE_IFRAME_PLAYLIST (-3)

typedef struct {
	int segment_index;		// or REQUEST_TYPE_XXX
	uint32_t required_tracks[MEDIA_TYPE_COUNT];
	uint32_t clip_to;
	uint32_t clip_from;
} request_params_t;

ngx_int_t parse_request_uri(ngx_http_request_t *r, ngx_http_vod_loc_conf_t *conf, request_params_t* request_params);

#endif // _NGX_HTTP_VOD_REQUEST_PARSE_H_INCLUDED_
