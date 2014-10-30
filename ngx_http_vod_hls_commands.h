#define BASE_OFFSET offsetof(ngx_http_vod_loc_conf_t, hls)

	{ ngx_string("vod_hls_absolute_master_urls"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_flag_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	BASE_OFFSET + offsetof(ngx_http_vod_hls_loc_conf_t, absolute_master_urls),
	NULL },

	{ ngx_string("vod_hls_absolute_index_urls"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_flag_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	BASE_OFFSET + offsetof(ngx_http_vod_hls_loc_conf_t, absolute_index_urls),
	NULL },

	{ ngx_string("vod_hls_absolute_iframe_urls"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_flag_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	BASE_OFFSET + offsetof(ngx_http_vod_hls_loc_conf_t, absolute_iframe_urls),
	NULL },

	{ ngx_string("vod_hls_master_file_name_prefix"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	BASE_OFFSET + offsetof(ngx_http_vod_hls_loc_conf_t, master_file_name_prefix),
	NULL },
	
	{ ngx_string("vod_hls_index_file_name_prefix"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	BASE_OFFSET + offsetof(ngx_http_vod_hls_loc_conf_t, m3u8_config.index_file_name_prefix),
	NULL },

	{ ngx_string("vod_hls_iframes_file_name_prefix"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	BASE_OFFSET + offsetof(ngx_http_vod_hls_loc_conf_t, iframes_file_name_prefix),
	NULL },

	{ ngx_string("vod_hls_segment_file_name_prefix"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	BASE_OFFSET + offsetof(ngx_http_vod_hls_loc_conf_t, m3u8_config.segment_file_name_prefix),
	NULL },

	{ ngx_string("vod_hls_encryption_key_file_name"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	BASE_OFFSET + offsetof(ngx_http_vod_hls_loc_conf_t, m3u8_config.encryption_key_file_name),
	NULL },

#undef BASE_OFFSET
