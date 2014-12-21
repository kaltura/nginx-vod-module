#define BASE_OFFSET offsetof(ngx_http_vod_loc_conf_t, dash)

	{ ngx_string("vod_dash_absolute_manifest_urls"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_flag_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	BASE_OFFSET + offsetof(ngx_http_vod_dash_loc_conf_t, absolute_manifest_urls),
	NULL },
	
	{ ngx_string("vod_dash_manifest_file_name_prefix"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	BASE_OFFSET + offsetof(ngx_http_vod_dash_loc_conf_t, manifest_file_name_prefix),
	NULL },

	{ ngx_string("vod_dash_init_file_name_prefix"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	BASE_OFFSET + offsetof(ngx_http_vod_dash_loc_conf_t, mpd_config.init_file_name_prefix),
	NULL },

	{ ngx_string("vod_dash_fragment_file_name_prefix"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	BASE_OFFSET + offsetof(ngx_http_vod_dash_loc_conf_t, mpd_config.fragment_file_name_prefix),
	NULL },

	{ ngx_string("vod_dash_include_segment_timeline"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_flag_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	BASE_OFFSET + offsetof(ngx_http_vod_dash_loc_conf_t, mpd_config.segment_timeline),
	NULL },

#undef BASE_OFFSET
