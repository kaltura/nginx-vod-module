#define BASE_OFFSET offsetof(ngx_http_vod_loc_conf_t, volume_map)

	{ ngx_string("vod_volume_map_file_name_prefix"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	BASE_OFFSET + offsetof(ngx_http_vod_volume_map_loc_conf_t, file_name_prefix),
	NULL },

	{ ngx_string("vod_volume_map_interval"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_num_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	BASE_OFFSET + offsetof(ngx_http_vod_volume_map_loc_conf_t, interval),
	NULL },

#undef BASE_OFFSET