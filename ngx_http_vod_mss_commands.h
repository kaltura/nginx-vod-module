#define BASE_OFFSET offsetof(ngx_http_vod_loc_conf_t, mss)

	{ ngx_string("vod_mss_manifest_file_name_prefix"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	BASE_OFFSET + offsetof(ngx_http_vod_mss_loc_conf_t, manifest_file_name_prefix),
	NULL },

	{ ngx_string("vod_mss_duplicate_bitrate_threshold"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_num_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	BASE_OFFSET + offsetof(ngx_http_vod_mss_loc_conf_t, manifest_conf.duplicate_bitrate_threshold),
	NULL },
	
#undef BASE_OFFSET
