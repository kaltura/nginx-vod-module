#define BASE_OFFSET offsetof(ngx_http_vod_loc_conf_t, hds)

	{ ngx_string("vod_hds_manifest_file_name_prefix"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	BASE_OFFSET + offsetof(ngx_http_vod_hds_loc_conf_t, manifest_file_name_prefix),
	NULL },

	{ ngx_string("vod_hds_fragment_file_name_prefix"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	BASE_OFFSET + offsetof(ngx_http_vod_hds_loc_conf_t, manifest_config.fragment_file_name_prefix),
	NULL },
	
#undef BASE_OFFSET
