#define BASE_OFFSET offsetof(ngx_http_vod_loc_conf_t, thumb)

	{ ngx_string("vod_thumb_file_name_prefix"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	BASE_OFFSET + offsetof(ngx_http_vod_thumb_loc_conf_t, file_name_prefix),
	NULL },

	{ ngx_string("vod_thumb_accurate_positioning"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_flag_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	BASE_OFFSET + offsetof(ngx_http_vod_thumb_loc_conf_t, accurate),
	NULL },

#undef BASE_OFFSET
