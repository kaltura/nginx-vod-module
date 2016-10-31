#define BASE_OFFSET offsetof(ngx_http_vod_loc_conf_t, hds)

	{ ngx_string("vod_hds_absolute_manifest_urls"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_flag_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	BASE_OFFSET + offsetof(ngx_http_vod_hds_loc_conf_t, absolute_manifest_urls),
	NULL },

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

	{ ngx_string("vod_hds_generate_moof_atom"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_flag_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	BASE_OFFSET + offsetof(ngx_http_vod_hds_loc_conf_t, fragment_config.generate_moof_atom),
	NULL },

#undef BASE_OFFSET
