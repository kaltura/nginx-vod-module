#include "ngx_http_vod_conf.h"
#include "ngx_http_vod_request_parse.h"
#include "ngx_child_http_request.h"
#include "ngx_http_vod_submodule.h"
#include "ngx_http_vod_module.h"
#include "ngx_http_vod_status.h"
#include "ngx_perf_counters.h"
#include "ngx_buffer_cache.h"
#include "vod/common.h"

static ngx_str_t  ngx_http_vod_last_modified_default_types[] = {
	ngx_null_string
};

static void *
ngx_http_vod_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_vod_loc_conf_t  *conf;
	const ngx_http_vod_submodule_t** cur_module;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_vod_loc_conf_t));
    if (conf == NULL) 
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, cf->log, 0,
			"ngx_http_vod_create_loc_conf: ngx_pcalloc failed");
		return NGX_CONF_ERROR;
    }

	// base params
	ngx_init_upstream_conf(&conf->upstream);
	ngx_init_upstream_conf(&conf->fallback_upstream);
	conf->submodule.parse_uri_file_name = NGX_CONF_UNSET_PTR;
	conf->request_handler = NGX_CONF_UNSET_PTR;
	conf->segmenter.segment_duration = NGX_CONF_UNSET_UINT;
	conf->segmenter.bootstrap_segments = NGX_CONF_UNSET_PTR;
	conf->segmenter.align_to_key_frames = NGX_CONF_UNSET;
	conf->segmenter.get_segment_count = NGX_CONF_UNSET_PTR;
	conf->segmenter.get_segment_durations = NGX_CONF_UNSET_PTR;
	conf->duplicate_bitrate_threshold = NGX_CONF_UNSET_UINT;
	conf->initial_read_size = NGX_CONF_UNSET_SIZE;
	conf->max_moov_size = NGX_CONF_UNSET_SIZE;
	conf->cache_buffer_size = NGX_CONF_UNSET_SIZE;
	conf->max_path_length = NGX_CONF_UNSET_SIZE;

	conf->last_modified_time = NGX_CONF_UNSET;

	conf->drm_enabled = NGX_CONF_UNSET;
	conf->drm_clear_lead_segment_count = NGX_CONF_UNSET_UINT;
	ngx_init_upstream_conf(&conf->drm_upstream);
	conf->drm_max_info_length = NGX_CONF_UNSET_SIZE;

#if (NGX_THREADS)
	conf->open_file_thread_pool = NGX_CONF_UNSET_PTR;
#endif

	// submodules
	for (cur_module = submodules; *cur_module != NULL; cur_module++)
	{
		(*cur_module)->create_loc_conf(cf, (u_char*)conf + (*cur_module)->conf_offset);
	}

    return conf;
}

static char *
ngx_http_vod_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_vod_loc_conf_t *prev = parent;
    ngx_http_vod_loc_conf_t *conf = child;
	const ngx_http_vod_submodule_t** cur_module;
	ngx_int_t rc;
	size_t proxy_header_len;
	u_char* p;
	char* err;

	// base params
	ngx_conf_merge_str_value(conf->child_request_location, prev->child_request_location, "");
	if (conf->submodule.parse_uri_file_name == NGX_CONF_UNSET_PTR) 
	{
		if (prev->submodule.parse_uri_file_name != NGX_CONF_UNSET_PTR)
		{
			conf->submodule = prev->submodule;
		}
		else
		{
			// zero module = serve files
			ngx_memzero(&conf->submodule, sizeof(conf->submodule));
		}
	}
	ngx_conf_merge_ptr_value(conf->request_handler, prev->request_handler, ngx_http_vod_local_request_handler);
	ngx_conf_merge_str_value(conf->multi_uri_suffix, prev->multi_uri_suffix, ".urlset");

	ngx_conf_merge_uint_value(conf->segmenter.segment_duration, prev->segmenter.segment_duration, 10000);
	ngx_conf_merge_ptr_value(conf->segmenter.bootstrap_segments, prev->segmenter.bootstrap_segments, NULL);
	ngx_conf_merge_value(conf->segmenter.align_to_key_frames, prev->segmenter.align_to_key_frames, 0);
	ngx_conf_merge_ptr_value(conf->segmenter.get_segment_count, prev->segmenter.get_segment_count, segmenter_get_segment_count_last_short);
	ngx_conf_merge_ptr_value(conf->segmenter.get_segment_durations, prev->segmenter.get_segment_durations, segmenter_get_segment_durations_estimate);

	ngx_conf_merge_str_value(conf->secret_key, prev->secret_key, "");
	ngx_conf_merge_uint_value(conf->duplicate_bitrate_threshold, prev->duplicate_bitrate_threshold, 4096);
	ngx_conf_merge_str_value(conf->https_header_name, prev->https_header_name, "");
	ngx_conf_merge_str_value(conf->segments_base_url, prev->segments_base_url, "");
	conf->segments_base_url_has_scheme =
		(ngx_strncasecmp(conf->segments_base_url.data, (u_char *) "http://", 7) == 0 ||
		ngx_strncasecmp(conf->segments_base_url.data, (u_char *) "https://", 8) == 0);


	if (conf->moov_cache_zone == NULL) 
	{
		conf->moov_cache_zone = prev->moov_cache_zone;
	}

	if (conf->response_cache_zone == NULL)
	{
		conf->response_cache_zone = prev->response_cache_zone;
	}
	
	ngx_conf_merge_size_value(conf->initial_read_size, prev->initial_read_size, 4096);
	ngx_conf_merge_size_value(conf->max_moov_size, prev->max_moov_size, 128 * 1024 * 1024);
	ngx_conf_merge_size_value(conf->cache_buffer_size, prev->cache_buffer_size, 256 * 1024);

	err = ngx_merge_upstream_conf(cf, &conf->upstream, &prev->upstream);
	if (err != NGX_CONF_OK)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, cf->log, 0,
			"ngx_http_vod_merge_loc_conf: ngx_merge_upstream_conf failed (1)");
		return err;
	}
	ngx_conf_merge_str_value(conf->upstream_host_header, prev->upstream_host_header, "");
	
	if (conf->upstream_extra_args == NULL) 
	{
		conf->upstream_extra_args = prev->upstream_extra_args;
	}

	if (conf->path_mapping_cache_zone == NULL)
	{
		conf->path_mapping_cache_zone = prev->path_mapping_cache_zone;
	}
	ngx_conf_merge_str_value(conf->path_response_prefix, prev->path_response_prefix, "<?xml version=\"1.0\" encoding=\"utf-8\"?><xml><result>");
	ngx_conf_merge_str_value(conf->path_response_postfix, prev->path_response_postfix, "</result></xml>");
	ngx_conf_merge_size_value(conf->max_path_length, prev->max_path_length, 1024);

	err = ngx_merge_upstream_conf(cf, &conf->fallback_upstream, &prev->fallback_upstream);
	if (err != NGX_CONF_OK)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, cf->log, 0,
			"ngx_http_vod_merge_loc_conf: ngx_merge_upstream_conf failed (2)");
		return err;
	}
	ngx_conf_merge_str_value(conf->proxy_header_name, prev->proxy_header_name, "X-Kaltura-Proxy");
	ngx_conf_merge_str_value(conf->proxy_header_value, prev->proxy_header_value, "dumpApiRequest");

	ngx_conf_merge_value(conf->last_modified_time, prev->last_modified_time, -1);
	if (ngx_http_merge_types(
		cf, 
		&conf->last_modified_types_keys, 
		&conf->last_modified_types,
		&prev->last_modified_types_keys, 
		&prev->last_modified_types,
		ngx_http_vod_last_modified_default_types) != NGX_OK)
	{
		return NGX_CONF_ERROR;
	}

	ngx_conf_merge_value(conf->drm_enabled, prev->drm_enabled, 0);
	ngx_conf_merge_uint_value(conf->drm_clear_lead_segment_count, prev->drm_clear_lead_segment_count, 1);
	err = ngx_merge_upstream_conf(cf, &conf->drm_upstream, &prev->drm_upstream);
	if (err != NGX_CONF_OK)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, cf->log, 0,
			"ngx_http_vod_merge_loc_conf: ngx_merge_upstream_conf failed (3)");
		return err;
	}
	ngx_conf_merge_size_value(conf->drm_max_info_length, prev->drm_max_info_length, 4096);
	if (conf->drm_info_cache_zone == NULL)
	{
		conf->drm_info_cache_zone = prev->drm_info_cache_zone;
	}
	if (conf->drm_request_uri == NULL) 
	{
		conf->drm_request_uri = prev->drm_request_uri;
	}

	ngx_conf_merge_str_value(conf->clip_to_param_name, prev->clip_to_param_name, "clipTo");
	ngx_conf_merge_str_value(conf->clip_from_param_name, prev->clip_from_param_name, "clipFrom");
	ngx_conf_merge_str_value(conf->tracks_param_name, prev->tracks_param_name, "tracks");
	ngx_conf_merge_str_value(conf->speed_param_name, prev->speed_param_name, "speed");

	if (conf->perf_counters_zone == NULL)
	{
		conf->perf_counters_zone = prev->perf_counters_zone;
	}

#if (NGX_THREADS)
	ngx_conf_merge_ptr_value(conf->open_file_thread_pool, prev->open_file_thread_pool, NULL);
#endif

	// validate vod_upstream / vod_upstream_host_header used when needed
	if (conf->request_handler == ngx_http_vod_remote_request_handler || conf->request_handler == ngx_http_vod_mapped_request_handler)
	{
		if (conf->child_request_location.len == 0)
		{
			ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
				"\"vod_child_request_path\" is mandatory for remote/mapped modes");
			return NGX_CONF_ERROR;
		}

		if (conf->upstream.upstream == NULL)
		{
			ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
				"\"vod_upstream\" is mandatory for remote/mapped modes");
			return NGX_CONF_ERROR;
		}
	}
	else
	{
		if (conf->upstream.upstream != NULL)
		{
			ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
				"\"vod_upstream\" does not apply to local mode");
			return NGX_CONF_ERROR;
		}

		if (conf->upstream_host_header.len != 0)
		{
			ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
				"\"vod_upstream_host_header\" does not apply to local mode");
			return NGX_CONF_ERROR;
		}
	}
	
	if (conf->request_handler == ngx_http_vod_remote_request_handler)
	{
		if (conf->fallback_upstream.upstream != NULL)
		{
			ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
				"\"vod_fallback_upstream\" does not apply to remote mode");
			return NGX_CONF_ERROR;
		}
	}

	if (conf->segmenter.segment_duration <= 0)
	{
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
			"\"segment_duration\" must be positive");
		return NGX_CONF_ERROR;
	}

	rc = segmenter_init_config(&conf->segmenter, cf->pool);
	if (rc != VOD_OK)
	{
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
			"failed to initialize the segmenter %i", rc);
		return NGX_CONF_ERROR;
	}

	if (conf->drm_enabled && conf->drm_upstream.upstream == NULL)
	{
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
			"\"drm_upstream\" must be configured when drm is enabled");
		return NGX_CONF_ERROR;
	}

	// validate the lengths of uri parameters
	if (conf->clip_to_param_name.len > MAX_URI_PARAM_NAME_LEN)
	{
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
			"\"clip_to_param_name\" should not be more than %d characters", MAX_URI_PARAM_NAME_LEN);
		return NGX_CONF_ERROR;
	}

	if (conf->clip_from_param_name.len > MAX_URI_PARAM_NAME_LEN)
	{
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
			"\"clip_from_param_name\" should not be more than %d characters", MAX_URI_PARAM_NAME_LEN);
		return NGX_CONF_ERROR;
	}

	if (conf->tracks_param_name.len > MAX_URI_PARAM_NAME_LEN)
	{
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
			"\"tracks_param_name\" should not be more than %d characters", MAX_URI_PARAM_NAME_LEN);
		return NGX_CONF_ERROR;
	}

	if (conf->speed_param_name.len > MAX_URI_PARAM_NAME_LEN)
	{
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
			"\"speed_param_name\" should not be more than %d characters", MAX_URI_PARAM_NAME_LEN);
		return NGX_CONF_ERROR;
	}
	
	// combine the proxy header name and value to a single line
	proxy_header_len = conf->proxy_header_name.len + sizeof(": ") - 1 + conf->proxy_header_value.len + sizeof(CRLF);
	conf->proxy_header.data = ngx_palloc(cf->pool, proxy_header_len);
	if (conf->proxy_header.data == NULL)
	{
		return NGX_CONF_ERROR;
	}
	p = conf->proxy_header.data;
	p = ngx_copy(p, conf->proxy_header_name.data, conf->proxy_header_name.len);
	*p++ = ':';		*p++ = ' ';
	p = ngx_copy(p, conf->proxy_header_value.data, conf->proxy_header_value.len);
	*p++ = '\r';		*p++ = '\n';
	*p = '\0';

	conf->proxy_header.len = p - conf->proxy_header.data;

	// init the hash table of the uri params (clipTo, clipFrom etc.)
	rc = ngx_http_vod_init_uri_params_hash(cf, conf);
	if (rc != NGX_OK)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, cf->log, 0,
			"ngx_http_vod_merge_loc_conf: ngx_http_vod_init_uri_params_hash failed");
		return NGX_CONF_ERROR;
	}

	// merge the submodules configuration
	for (cur_module = submodules; *cur_module != NULL; cur_module++)
	{
		err = (*cur_module)->merge_loc_conf(
			cf, 
			conf, 
			(u_char*)conf + (*cur_module)->conf_offset, 
			(u_char*)prev + (*cur_module)->conf_offset);
		if (err != NGX_CONF_OK)
		{
			ngx_log_debug0(NGX_LOG_DEBUG_HTTP, cf->log, 0,
				"ngx_http_vod_merge_loc_conf: submodule merge_loc_conf failed");
			return err;
		}
	}

    return NGX_CONF_OK;
}

static char *
ngx_http_vod_set_time_slot(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
	char  *p = conf;

	time_t           *sp;
	ngx_str_t        *value;
	ngx_conf_post_t  *post;


	sp = (time_t *)(p + cmd->offset);
	if (*sp != NGX_CONF_UNSET) {
		return "is duplicate";
	}

	value = cf->args->elts;

	*sp = ngx_http_parse_time(value[1].data, value[1].len);
	if (*sp == (time_t)NGX_ERROR) {
		return "invalid value";
	}

	if (cmd->post) {
		post = cmd->post;
		return post->post_handler(cf, post, sp);
	}

	return NGX_CONF_OK;
}

static char *
ngx_http_vod_mode_command(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
	ngx_http_vod_loc_conf_t    *vod_conf = conf;
	ngx_str_t                       *value;

	value = cf->args->elts;

	if (ngx_strcasecmp(value[1].data, (u_char *) "local") == 0) 
	{
		vod_conf->request_handler = ngx_http_vod_local_request_handler;
	}
	else if (ngx_strcasecmp(value[1].data, (u_char *) "remote") == 0) 
	{
		vod_conf->request_handler = ngx_http_vod_remote_request_handler;
	}
	else if (ngx_strcasecmp(value[1].data, (u_char *) "mapped") == 0) 
	{
		vod_conf->request_handler = ngx_http_vod_mapped_request_handler;
	}
	else 
	{
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
			"invalid value \"%s\" in \"%s\" directive, "
			"it must be \"local\", \"remote\" or \"mapped\"",
			value[1].data, cmd->name.data);
		return NGX_CONF_ERROR;
	}

	return NGX_CONF_OK;
}

static char *
ngx_http_vod_segment_count_policy_command(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
	ngx_http_vod_loc_conf_t    *vod_conf = conf;
	ngx_str_t                       *value;

	value = cf->args->elts;

	if (ngx_strcasecmp(value[1].data, (u_char *) "last_short") == 0)
	{
		vod_conf->segmenter.get_segment_count = segmenter_get_segment_count_last_short;
	}
	else if (ngx_strcasecmp(value[1].data, (u_char *) "last_long") == 0)
	{
		vod_conf->segmenter.get_segment_count = segmenter_get_segment_count_last_long;
	}
	else if (ngx_strcasecmp(value[1].data, (u_char *) "last_rounded") == 0)
	{
		vod_conf->segmenter.get_segment_count = segmenter_get_segment_count_last_rounded;
	}
	else
	{
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
			"invalid value \"%s\" in \"%s\" directive, "
			"it must be \"last_short\", \"last_long\" or \"last_rounded\"",
			value[1].data, cmd->name.data);
		return NGX_CONF_ERROR;
	}

	return NGX_CONF_OK;
}

#if (NGX_THREADS)
static char *
ngx_http_vod_thread_pool_command(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
	ngx_thread_pool_t **tp = (ngx_thread_pool_t **)((u_char*)conf + cmd->offset);
	ngx_str_t  *value;

	if (*tp != NGX_CONF_UNSET_PTR) {
		return "is duplicate";
	}

	value = cf->args->elts;

	if (cf->args->nelts > 1)
	{
		*tp = ngx_thread_pool_add(cf, &value[1]);
	}
	else
	{
		*tp = ngx_thread_pool_add(cf, NULL);
	}

	if (*tp == NULL)
	{
		return NGX_CONF_ERROR;
	}

	return NGX_CONF_OK;
}
#endif // NGX_THREADS

static char *
ngx_http_vod_manifest_segment_durations_mode_command(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
	ngx_http_vod_loc_conf_t    *vod_conf = conf;
	ngx_str_t                       *value;

	value = cf->args->elts;

	if (ngx_strcasecmp(value[1].data, (u_char *) "estimate") == 0)
	{
		vod_conf->segmenter.get_segment_durations = segmenter_get_segment_durations_estimate;
	}
	else if (ngx_strcasecmp(value[1].data, (u_char *) "accurate") == 0)
	{
		vod_conf->segmenter.get_segment_durations = segmenter_get_segment_durations_accurate;
	}
	else
	{
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
			"invalid value \"%s\" in \"%s\" directive, "
			"it must be \"estimate\" or \"accurate\"",
			value[1].data, cmd->name.data);
		return NGX_CONF_ERROR;
	}

	return NGX_CONF_OK;
}

static char *
ngx_http_vod_cache_command(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
	ngx_shm_zone_t **zone = (ngx_shm_zone_t **)((u_char*)conf + cmd->offset);
	ngx_str_t  *value;
	ssize_t size;

	value = cf->args->elts;

	if (*zone != NULL) 
	{
		return "is duplicate";
	}

	if (ngx_strcmp(value[1].data, "off") == 0) 
	{
		*zone = NULL;
		return NGX_CONF_OK;
	}

	if (cf->args->nelts < 3)
	{
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
			"size not specified in \"%V\"", &cmd->name);
		return NGX_CONF_ERROR;
	}

	size = ngx_parse_size(&value[2]);
	if (size == NGX_ERROR)
	{
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
			"invalid size %V", &value[2]);
		return NGX_CONF_ERROR;
	}

	*zone = ngx_buffer_cache_create_zone(cf, &value[1], size, &ngx_http_vod_module);
	if (*zone == NULL)
	{
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
			"failed to create cache zone");
		return NGX_CONF_ERROR;
	}

	return NGX_CONF_OK;
}

static char *
ngx_http_vod_perf_counters_command(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
	ngx_shm_zone_t **zone = (ngx_shm_zone_t **)((u_char*)conf + cmd->offset);
	ngx_str_t  *value;

	value = cf->args->elts;

	if (*zone != NULL)
	{
		return "is duplicate";
	}

	if (ngx_strcmp(value[1].data, "off") == 0)
	{
		*zone = NULL;
		return NGX_CONF_OK;
	}

	*zone = ngx_perf_counters_create_zone(cf, &value[1], &ngx_http_vod_module);
	if (*zone == NULL)
	{
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
			"failed to create perf counters cache zone");
		return NGX_CONF_ERROR;
	}

	return NGX_CONF_OK;
}

static char *
ngx_http_vod(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
	const ngx_http_vod_submodule_t** cur_module;
	ngx_http_vod_loc_conf_t *vod_conf = conf;
	ngx_http_core_loc_conf_t *clcf;
	ngx_flag_t found_module;
	ngx_str_t *value;
	ngx_str_t module_names;
	size_t module_names_size;
	u_char* p;

	clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
	clcf->handler = ngx_http_vod_handler;

	value = cf->args->elts;

	// file serve
	if (ngx_strcasecmp(value[1].data, (u_char *) "none") == 0)
	{
		ngx_memzero(&vod_conf->submodule, sizeof(vod_conf->submodule));
		return NGX_CONF_OK;
	}

	// submodule
	found_module = 0;
	module_names_size = 1;
	for (cur_module = submodules; *cur_module != NULL; cur_module++)
	{
		if (ngx_strcasecmp(value[1].data, (*cur_module)->name) == 0)
		{
			vod_conf->submodule = **cur_module;
			found_module = 1;
			break;
		}
		module_names_size += (*cur_module)->name_len + 1;
	}

	if (!found_module)
	{
		// combine the module names
		module_names.data = ngx_palloc(cf->pool, module_names_size);
		if (module_names.data == NULL)
		{
			ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "failed to allocate modules names");
			return NGX_CONF_ERROR;
		}

		p = module_names.data;
		for (cur_module = submodules; *cur_module != NULL; cur_module++)
		{
			*p++ = ',';
			p = ngx_copy(p, (*cur_module)->name, (*cur_module)->name_len);
		}
		*p = '\0';
		module_names.len = p - module_names.data;

		// print the error message
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
			"invalid value \"%s\" in \"%s\" directive, "
			"it must be one of: none%V",
			value[1].data, cmd->name.data, &module_names);
		return NGX_CONF_ERROR;
	}

	return NGX_CONF_OK;
}

static char *
ngx_http_vod_child_request(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
	ngx_http_core_loc_conf_t *clcf;

	clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
	clcf->handler = ngx_child_request_internal_handler;

	return NGX_CONF_OK;
}

static char *
ngx_http_vod_status(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
	ngx_http_core_loc_conf_t *clcf;

	clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
	clcf->handler = ngx_http_vod_status_handler;

	return NGX_CONF_OK;
}

ngx_command_t ngx_http_vod_commands[] = {

	// basic parameters
	{ ngx_string("vod"),
	NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_http_vod,
	NGX_HTTP_LOC_CONF_OFFSET,
	0,
	NULL },

	{ ngx_string("vod_mode"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_http_vod_mode_command,
	NGX_HTTP_LOC_CONF_OFFSET,
	0,
	NULL },

	{ ngx_string("vod_status"),
	NGX_HTTP_LOC_CONF | NGX_CONF_NOARGS,
	ngx_http_vod_status,
	0,
	0,
	NULL },

	// output generation parameters
	{ ngx_string("vod_multi_uri_suffix"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, multi_uri_suffix),
	NULL },

	{ ngx_string("vod_segment_duration"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_num_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, segmenter.segment_duration),
	NULL },

	{ ngx_string("vod_bootstrap_segment_durations"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_array_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, segmenter.bootstrap_segments),
	NULL },

	{ ngx_string("vod_align_segments_to_key_frames"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_flag_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, segmenter.align_to_key_frames),
	NULL },

	{ ngx_string("vod_segment_count_policy"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_http_vod_segment_count_policy_command,
	NGX_HTTP_LOC_CONF_OFFSET,
	0,
	NULL },

	{ ngx_string("vod_manifest_segment_durations_mode"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_http_vod_manifest_segment_durations_mode_command,
	NGX_HTTP_LOC_CONF_OFFSET,
	0,
	NULL },

	{ ngx_string("vod_secret_key"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, secret_key),
	NULL },

	{ ngx_string("vod_duplicate_bitrate_threshold"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_num_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, duplicate_bitrate_threshold),
	NULL },

	{ ngx_string("vod_https_header_name"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, https_header_name),
	NULL },

	{ ngx_string("vod_segments_base_url"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, segments_base_url),
	NULL },
	
	// mp4 reading parameters
	{ ngx_string("vod_moov_cache"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1 | NGX_CONF_TAKE2,
	ngx_http_vod_cache_command,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, moov_cache_zone),
	NULL },

	{ ngx_string("vod_response_cache"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1 | NGX_CONF_TAKE2,
	ngx_http_vod_cache_command,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, response_cache_zone),
	NULL },

	{ ngx_string("vod_initial_read_size"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_size_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, initial_read_size),
	NULL },

	{ ngx_string("vod_max_moov_size"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_size_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, max_moov_size),
	NULL },

	{ ngx_string("vod_cache_buffer_size"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_size_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, cache_buffer_size),
	NULL },

	// upstream parameters - only for mapped/remote modes
	{ ngx_string("vod_child_request"),
	NGX_HTTP_LOC_CONF | NGX_CONF_NOARGS,
	ngx_http_vod_child_request,
	0,
	0,
	NULL },

	{ ngx_string("vod_child_request_path"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, child_request_location),
	NULL },

	DEFINE_UPSTREAM_COMMANDS(upstream, "")

	{ ngx_string("vod_upstream_host_header"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, upstream_host_header),
	NULL },

	{ ngx_string("vod_upstream_extra_args"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_http_set_complex_value_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, upstream_extra_args),
	NULL },

	// path request parameters - mapped mode only
	{ ngx_string("vod_path_mapping_cache"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1 | NGX_CONF_TAKE2,
	ngx_http_vod_cache_command,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, path_mapping_cache_zone),
	NULL },

	{ ngx_string("vod_path_response_prefix"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, path_response_prefix),
	NULL },

	{ ngx_string("vod_path_response_postfix"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, path_response_postfix),
	NULL },

	{ ngx_string("vod_max_path_length"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_size_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, max_path_length),
	NULL },

	// fallback upstream - only for local/mapped modes
	DEFINE_UPSTREAM_COMMANDS(fallback_upstream, "fallback_")

	{ ngx_string("vod_proxy_header_name"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, proxy_header_name),
	NULL },

	{ ngx_string("vod_proxy_header_value"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, proxy_header_value),
	NULL },

	// last modified
	{ ngx_string("vod_last_modified"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_http_vod_set_time_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, last_modified_time),
	NULL },

	{ ngx_string("vod_last_modified_types"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_1MORE,
	ngx_http_types_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, last_modified_types_keys),
	NULL },

	// drm
	{ ngx_string("vod_drm_enabled"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_flag_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, drm_enabled),
	NULL },

	{ ngx_string("vod_drm_clear_lead_segment_count"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_num_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, drm_clear_lead_segment_count),
	NULL },

	{ ngx_string("vod_drm_max_info_length"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_size_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, drm_max_info_length),
	NULL },

	DEFINE_UPSTREAM_COMMANDS(drm_upstream, "drm_")

	{ ngx_string("vod_drm_info_cache"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1 | NGX_CONF_TAKE2,
	ngx_http_vod_cache_command,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, drm_info_cache_zone),
	NULL },

	{ ngx_string("vod_drm_request_uri"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_http_set_complex_value_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, drm_request_uri),
	NULL },
	
	// request format settings
	{ ngx_string("vod_clip_to_param_name"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, clip_to_param_name),
	NULL },

	{ ngx_string("vod_clip_from_param_name"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, clip_from_param_name),
	NULL },

	{ ngx_string("vod_tracks_param_name"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, tracks_param_name),
	NULL },

	{ ngx_string("vod_speed_param_name"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, speed_param_name),
	NULL },
	
	{ ngx_string("vod_performance_counters"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_http_vod_perf_counters_command,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, perf_counters_zone),
	NULL },

#if (NGX_THREADS)
	{ ngx_string("vod_open_file_thread_pool"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_NOARGS | NGX_CONF_TAKE1,
	ngx_http_vod_thread_pool_command,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, open_file_thread_pool),
	NULL },
#endif

#include "ngx_http_vod_dash_commands.h"
#include "ngx_http_vod_hds_commands.h"
#include "ngx_http_vod_hls_commands.h"
#include "ngx_http_vod_mss_commands.h"

	ngx_null_command
};

ngx_http_module_t  ngx_http_vod_module_ctx = {
	NULL,                               /* preconfiguration */
	NULL,                               /* postconfiguration */

	NULL,                               /* create main configuration */
	NULL,                               /* init main configuration */

	NULL,                               /* create server configuration */
	NULL,                               /* merge server configuration */

	ngx_http_vod_create_loc_conf,       /* create location configuration */
	ngx_http_vod_merge_loc_conf         /* merge location configuration */
};
