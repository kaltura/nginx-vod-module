#include "ngx_http_vod_conf.h"
#include "ngx_http_vod_request_parse.h"
#include "ngx_child_http_request.h"
#include "ngx_http_vod_submodule.h"
#include "ngx_http_vod_module.h"
#include "ngx_http_vod_status.h"
#include "ngx_perf_counters.h"
#include "ngx_buffer_cache.h"
#include "vod/media_set_parser.h"
#include "vod/buffer_pool.h"
#include "vod/common.h"
#include "vod/udrm.h"

#if (NGX_HAVE_LIB_AV_CODEC)
#include "ngx_http_vod_thumb.h"
#endif // NGX_HAVE_LIB_AV_CODEC

// globals
static ngx_str_t ngx_http_vod_last_modified_default_types[] = {
	ngx_null_string
};

static ngx_int_t
ngx_http_vod_init_parsers(ngx_conf_t *cf)
{
	vod_status_t rc;
	
	rc = media_set_parser_init(cf->pool, cf->temp_pool);
	if (rc != VOD_OK)
	{
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
			"failed to initialize media set parsers %i", rc);
		return NGX_ERROR;
	}

	rc = udrm_init_parser(cf->pool, cf->temp_pool);
	if (rc != VOD_OK)
	{
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
			"failed to initialize udrm parser %i", rc);
		return NGX_ERROR;
	}

	rc = ngx_child_request_init(cf);
	if (rc != VOD_OK)
	{
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
			"failed to initialize hide headers hash %i", rc);
		return NGX_ERROR;
	}

	return NGX_OK;
}

static void *
ngx_http_vod_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_vod_loc_conf_t  *conf;
	const ngx_http_vod_submodule_t** cur_module;
	int type;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_vod_loc_conf_t));
    if (conf == NULL) 
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, cf->log, 0,
			"ngx_http_vod_create_loc_conf: ngx_pcalloc failed");
		return NGX_CONF_ERROR;
    }

	// base params
	conf->submodule.parse_uri_file_name = NGX_CONF_UNSET_PTR;
	conf->request_handler = NGX_CONF_UNSET_PTR;
	conf->segmenter.segment_duration = NGX_CONF_UNSET_UINT;
	conf->segmenter.live_window_duration = NGX_CONF_UNSET;
	conf->segmenter.bootstrap_segments = NGX_CONF_UNSET_PTR;
	conf->segmenter.align_to_key_frames = NGX_CONF_UNSET;
	conf->segmenter.get_segment_count = NGX_CONF_UNSET_PTR;
	conf->segmenter.get_segment_durations = NGX_CONF_UNSET_PTR;
	conf->segmenter.manifest_duration_policy = NGX_CONF_UNSET_UINT;
	conf->segmenter.gop_look_ahead = NGX_CONF_UNSET_UINT;
	conf->segmenter.gop_look_behind = NGX_CONF_UNSET_UINT;
	conf->force_playlist_type_vod = NGX_CONF_UNSET;
	conf->force_continuous_timestamps = NGX_CONF_UNSET;
	conf->force_sequence_index = NGX_CONF_UNSET;
	conf->initial_read_size = NGX_CONF_UNSET_SIZE;
	conf->max_metadata_size = NGX_CONF_UNSET_SIZE;
	conf->max_frames_size = NGX_CONF_UNSET_SIZE;
	conf->max_frame_count = NGX_CONF_UNSET_UINT;
	conf->segment_max_frame_count = NGX_CONF_UNSET_UINT;
	conf->cache_buffer_size = NGX_CONF_UNSET_SIZE;
	conf->max_upstream_headers_size = NGX_CONF_UNSET_SIZE;
	conf->ignore_edit_list = NGX_CONF_UNSET;
	conf->parse_hdlr_name = NGX_CONF_UNSET;
	conf->parse_udta_name = NGX_CONF_UNSET;
	conf->max_mapping_response_size = NGX_CONF_UNSET_SIZE;

	conf->metadata_cache = NGX_CONF_UNSET_PTR;
	conf->dynamic_mapping_cache = NGX_CONF_UNSET_PTR;
	for (type = 0; type < CACHE_TYPE_COUNT; type++)
	{
		conf->response_cache[type] = NGX_CONF_UNSET_PTR;
		conf->mapping_cache[type] = NGX_CONF_UNSET_PTR;
	}

	for (type = 0; type < EXPIRES_TYPE_COUNT; type++)
	{
		conf->expires[type] = NGX_CONF_UNSET;
	}
	conf->last_modified_time = NGX_CONF_UNSET;

	conf->drm_enabled = NGX_CONF_UNSET;
	conf->drm_single_key = NGX_CONF_UNSET;
	conf->drm_clear_lead_segment_count = NGX_CONF_UNSET_UINT;
	conf->drm_max_info_length = NGX_CONF_UNSET_SIZE;
	conf->drm_info_cache = NGX_CONF_UNSET_PTR;
	conf->min_single_nalu_per_frame_segment = NGX_CONF_UNSET_UINT;

#if (NGX_THREADS)
	conf->open_file_thread_pool = NGX_CONF_UNSET_PTR;
#endif // NGX_THREADS

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
	int type;
	char* err;

	// base params
	ngx_conf_merge_str_value(conf->upstream_location, prev->upstream_location, "");
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
	ngx_conf_merge_str_value(conf->remote_upstream_location, prev->remote_upstream_location, "");
	ngx_conf_merge_ptr_value(conf->request_handler, prev->request_handler, ngx_http_vod_local_request_handler);
	ngx_conf_merge_str_value(conf->multi_uri_suffix, prev->multi_uri_suffix, ".urlset");

	ngx_conf_merge_uint_value(conf->segmenter.segment_duration, prev->segmenter.segment_duration, 10000);
	ngx_conf_merge_value(conf->segmenter.live_window_duration, prev->segmenter.live_window_duration, 30000);
	ngx_conf_merge_ptr_value(conf->segmenter.bootstrap_segments, prev->segmenter.bootstrap_segments, NULL);
	ngx_conf_merge_value(conf->segmenter.align_to_key_frames, prev->segmenter.align_to_key_frames, 0);
	ngx_conf_merge_ptr_value(conf->segmenter.get_segment_count, prev->segmenter.get_segment_count, segmenter_get_segment_count_last_short);
	ngx_conf_merge_ptr_value(conf->segmenter.get_segment_durations, prev->segmenter.get_segment_durations, segmenter_get_segment_durations_estimate);
	ngx_conf_merge_uint_value(conf->segmenter.manifest_duration_policy, prev->segmenter.manifest_duration_policy, MDP_MAX);
	ngx_conf_merge_uint_value(conf->segmenter.gop_look_ahead, prev->segmenter.gop_look_ahead, 1000);
	ngx_conf_merge_uint_value(conf->segmenter.gop_look_behind, prev->segmenter.gop_look_behind, 10000);
	ngx_conf_merge_value(conf->force_playlist_type_vod, prev->force_playlist_type_vod, 0);
	ngx_conf_merge_value(conf->force_continuous_timestamps, prev->force_continuous_timestamps, 0);
	ngx_conf_merge_value(conf->force_sequence_index, prev->force_sequence_index, 0);

	if (conf->secret_key == NULL)
	{
		conf->secret_key = prev->secret_key;
	}

	if (conf->encryption_iv_seed == NULL)
	{
		conf->encryption_iv_seed = prev->encryption_iv_seed;
	}

	if (conf->base_url == NULL)
	{
		conf->base_url = prev->base_url;
	}

	if (conf->segments_base_url == NULL)
	{
		conf->segments_base_url = prev->segments_base_url;
	}

	ngx_conf_merge_ptr_value(conf->metadata_cache, prev->metadata_cache, NULL);
	ngx_conf_merge_ptr_value(conf->dynamic_mapping_cache, prev->dynamic_mapping_cache, NULL);

	for (type = 0; type < CACHE_TYPE_COUNT; type++)
	{
		ngx_conf_merge_ptr_value(conf->response_cache[type], prev->response_cache[type], NULL);
		ngx_conf_merge_ptr_value(conf->mapping_cache[type], prev->mapping_cache[type], NULL);
	}

	for (type = 0; type < EXPIRES_TYPE_COUNT; type++)
	{
		ngx_conf_merge_value(conf->expires[type], prev->expires[type], -1);
	}

	ngx_conf_merge_size_value(conf->initial_read_size, prev->initial_read_size, 4096);
	ngx_conf_merge_size_value(conf->max_metadata_size, prev->max_metadata_size, 128 * 1024 * 1024);
	ngx_conf_merge_size_value(conf->max_frames_size, prev->max_frames_size, 16 * 1024 * 1024);
	ngx_conf_merge_uint_value(conf->max_frame_count, prev->max_frame_count, 1024 * 1024);
	ngx_conf_merge_uint_value(conf->segment_max_frame_count, prev->segment_max_frame_count, 64 * 1024);
	ngx_conf_merge_size_value(conf->cache_buffer_size, prev->cache_buffer_size, 256 * 1024);
	ngx_conf_merge_size_value(conf->max_upstream_headers_size, prev->max_upstream_headers_size, 4 * 1024);

	if (conf->output_buffer_pool == NULL)
	{
		conf->output_buffer_pool = prev->output_buffer_pool;
	}

	ngx_conf_merge_value(conf->ignore_edit_list, prev->ignore_edit_list, 0);
	ngx_conf_merge_value(conf->parse_hdlr_name, prev->parse_hdlr_name, 0);
	ngx_conf_merge_value(conf->parse_udta_name, prev->parse_udta_name, 0);

	conf->parse_flags = 0;
	if (!conf->ignore_edit_list)
	{
		conf->parse_flags |= PARSE_FLAG_EDIT_LIST;
	}
	if (conf->parse_hdlr_name)
	{
		conf->parse_flags |= PARSE_FLAG_HDLR_NAME;
	}
	if (conf->parse_udta_name)
	{
		conf->parse_flags |= PARSE_FLAG_UDTA_NAME;
	}

	if (conf->upstream_extra_args == NULL)
	{
		conf->upstream_extra_args = prev->upstream_extra_args;
	}

	ngx_conf_merge_str_value(conf->path_response_prefix, prev->path_response_prefix, "{\"sequences\":[{\"clips\":[{\"type\":\"source\",\"path\":\"");
	ngx_conf_merge_str_value(conf->path_response_postfix, prev->path_response_postfix, "\"}]}]}");
	ngx_conf_merge_size_value(conf->max_mapping_response_size, prev->max_mapping_response_size, 1024);
	if (conf->notification_uri == NULL)
	{
		conf->notification_uri = prev->notification_uri;
	}
	if (conf->dynamic_clip_map_uri == NULL)
	{
		conf->dynamic_clip_map_uri = prev->dynamic_clip_map_uri;
	}
	if (conf->source_clip_map_uri == NULL)
	{
		conf->source_clip_map_uri = prev->source_clip_map_uri;
	}
	if (conf->redirect_segments_url == NULL)
	{
		conf->redirect_segments_url = prev->redirect_segments_url;
	}
	if (conf->media_set_map_uri == NULL)
	{
		conf->media_set_map_uri = prev->media_set_map_uri;
	}
	if (conf->apply_dynamic_mapping == NULL)
	{
		conf->apply_dynamic_mapping = prev->apply_dynamic_mapping;
	}
	if (conf->media_set_override_json == NULL)
	{
		conf->media_set_override_json = prev->media_set_override_json;
	}

	ngx_conf_merge_str_value(conf->fallback_upstream_location, prev->fallback_upstream_location, "");
	ngx_conf_merge_str_value(conf->proxy_header.key, prev->proxy_header.key, "X-Kaltura-Proxy");
	ngx_conf_merge_str_value(conf->proxy_header.value, prev->proxy_header.value, "dumpApiRequest");

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
	ngx_conf_merge_value(conf->drm_single_key, prev->drm_single_key, 0);
	ngx_conf_merge_uint_value(conf->drm_clear_lead_segment_count, prev->drm_clear_lead_segment_count, 1);
	ngx_conf_merge_str_value(conf->drm_upstream_location, prev->drm_upstream_location, "");
	ngx_conf_merge_size_value(conf->drm_max_info_length, prev->drm_max_info_length, 4096);
	ngx_conf_merge_ptr_value(conf->drm_info_cache, prev->drm_info_cache, NULL);
	if (conf->drm_request_uri == NULL)
	{
		conf->drm_request_uri = prev->drm_request_uri;
	}
	ngx_conf_merge_uint_value(conf->min_single_nalu_per_frame_segment, prev->min_single_nalu_per_frame_segment, 0);
	
	ngx_conf_merge_str_value(conf->clip_to_param_name, prev->clip_to_param_name, "clipTo");
	ngx_conf_merge_str_value(conf->clip_from_param_name, prev->clip_from_param_name, "clipFrom");
	ngx_conf_merge_str_value(conf->tracks_param_name, prev->tracks_param_name, "tracks");
	ngx_conf_merge_str_value(conf->time_shift_param_name, prev->time_shift_param_name, "shift");
	ngx_conf_merge_str_value(conf->speed_param_name, prev->speed_param_name, "speed");
	ngx_conf_merge_str_value(conf->lang_param_name, prev->lang_param_name, "lang");

	if (conf->perf_counters_zone == NULL)
	{
		conf->perf_counters_zone = prev->perf_counters_zone;
	}

#if (NGX_THREADS)
	ngx_conf_merge_ptr_value(conf->open_file_thread_pool, prev->open_file_thread_pool, NULL);
#endif // NGX_THREADS

	// validate vod_upstream / vod_upstream_host_header used when needed
	if (conf->request_handler == ngx_http_vod_remote_request_handler)
	{
		if (conf->upstream_location.len == 0)
		{
			ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
				"\"vod_upstream_location\" is mandatory for remote mode");
			return NGX_CONF_ERROR;
		}
	}

	if (conf->segmenter.segment_duration <= 0)
	{
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
			"\"vod_segment_duration\" must be positive");
		return NGX_CONF_ERROR;
	}

#if (NGX_HAVE_LIB_AV_CODEC)
	if (conf->submodule.name == thumb.name)
	{
		conf->segmenter.align_to_key_frames = 1;
	}
#endif // NGX_HAVE_LIB_AV_CODEC

	rc = segmenter_init_config(&conf->segmenter, cf->pool);
	if (rc != VOD_OK)
	{
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
			"failed to initialize the segmenter %i", rc);
		return NGX_CONF_ERROR;
	}

	if (conf->drm_enabled)
	{
		if (conf->drm_upstream_location.len == 0)
		{
			ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
				"\"vod_drm_upstream_location\" is mandatory for drm");
			return NGX_CONF_ERROR;
		}
	}

	// validate the lengths of uri parameters
	if (conf->clip_to_param_name.len > MAX_URI_PARAM_NAME_LEN)
	{
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
			"\"vod_clip_to_param_name\" should not be more than %d characters", MAX_URI_PARAM_NAME_LEN);
		return NGX_CONF_ERROR;
	}

	if (conf->clip_from_param_name.len > MAX_URI_PARAM_NAME_LEN)
	{
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
			"\"vod_clip_from_param_name\" should not be more than %d characters", MAX_URI_PARAM_NAME_LEN);
		return NGX_CONF_ERROR;
	}

	if (conf->tracks_param_name.len > MAX_URI_PARAM_NAME_LEN)
	{
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
			"\"vod_tracks_param_name\" should not be more than %d characters", MAX_URI_PARAM_NAME_LEN);
		return NGX_CONF_ERROR;
	}

	if (conf->time_shift_param_name.len > MAX_URI_PARAM_NAME_LEN)
	{
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
			"\"vod_time_shift_param_name\" should not be more than %d characters", MAX_URI_PARAM_NAME_LEN);
		return NGX_CONF_ERROR;
	}

	if (conf->speed_param_name.len > MAX_URI_PARAM_NAME_LEN)
	{
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
			"\"vod_speed_param_name\" should not be more than %d characters", MAX_URI_PARAM_NAME_LEN);
		return NGX_CONF_ERROR;
	}

	if (conf->lang_param_name.len > MAX_URI_PARAM_NAME_LEN)
	{
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
			"\"vod_lang_param_name\" should not be more than %d characters", MAX_URI_PARAM_NAME_LEN);
		return NGX_CONF_ERROR;
	}

	// calculate the proxy header lower case and hash
	conf->proxy_header.lowcase_key = ngx_palloc(cf->pool, conf->proxy_header.key.len);
	if (conf->proxy_header.lowcase_key == NULL)
	{
		return NGX_CONF_ERROR;
	}

	ngx_strlow(conf->proxy_header.lowcase_key, conf->proxy_header.key.data, conf->proxy_header.key.len);
	conf->proxy_header.hash = ngx_hash_key(conf->proxy_header.lowcase_key, conf->proxy_header.key.len);

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
ngx_http_vod_set_signed_slot(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
	char  *p = conf;

	ngx_int_t        *np;
	ngx_str_t        *value;
	ngx_conf_post_t  *post;


	np = (ngx_int_t *)(p + cmd->offset);

	if (*np != NGX_CONF_UNSET) {
		return "is duplicate";
	}

	value = cf->args->elts;
	if (value[1].len > 0 && value[1].data[0] == '-')
	{
		*np = ngx_atoi(value[1].data + 1, value[1].len - 1);
		if (*np == NGX_ERROR) {
			return "invalid number";
		}
		*np = -(*np);
	}
	else
	{
		*np = ngx_atoi(value[1].data, value[1].len);
		if (*np == NGX_ERROR) {
			return "invalid number";
		}
	}

	if (cmd->post) {
		post = cmd->post;
		return post->post_handler(cf, post, np);
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
	ngx_buffer_cache_t **cache = (ngx_buffer_cache_t **)((u_char*)conf + cmd->offset);
	ngx_str_t  *value;
	ssize_t size;
	time_t expiration;

	value = cf->args->elts;

	if (*cache != NGX_CONF_UNSET_PTR)
	{
		return "is duplicate";
	}

	if (ngx_strcmp(value[1].data, "off") == 0) 
	{
		*cache = NULL;
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

	if (cf->args->nelts > 3)
	{
		expiration = ngx_parse_time(&value[3], 1);
		if (expiration == (time_t)NGX_ERROR) 
		{
			ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
				"invalid expiration %V", &value[3]);
			return NGX_CONF_ERROR;
		}
	}
	else
	{
		expiration = 0;
	}

	*cache = ngx_buffer_cache_create(cf, &value[1], size, expiration, &ngx_http_vod_module);
	if (*cache == NULL)
	{
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
			"failed to create cache");
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

static char*
ngx_http_vod_buffer_pool_command(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
	buffer_pool_t** buffer_pool = (buffer_pool_t **)((u_char*)conf + cmd->offset);
	ngx_str_t  *value;
	ngx_int_t count;
	ssize_t buffer_size;

	if (*buffer_pool != NULL)
	{
		return "is duplicate";
	}

	value = cf->args->elts;

	buffer_size = ngx_parse_size(&value[1]);
	if (buffer_size == NGX_ERROR)
	{
		return "invalid size";
	}

	count = ngx_atoi(value[2].data, value[2].len);
	if (count == NGX_ERROR)
	{
		return "invalid count";
	}
	
	*buffer_pool = buffer_pool_create(cf->pool, cf->log, buffer_size, count);
	if (*buffer_pool == NULL)
	{
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
ngx_http_vod_status(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
	ngx_http_core_loc_conf_t *clcf;

	clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
	clcf->handler = ngx_http_vod_status_handler;

	return NGX_CONF_OK;
}

static ngx_conf_enum_t manifest_duration_policies[] = {
	{ ngx_string("max"), MDP_MAX },
	{ ngx_string("min"), MDP_MIN },
	{ ngx_null_string, 0 }
};

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

	{ ngx_string("vod_live_window_duration"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_http_vod_set_signed_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, segmenter.live_window_duration),
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

	{ ngx_string("vod_gop_look_ahead"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_num_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, segmenter.gop_look_ahead),
	NULL },

	{ ngx_string("vod_gop_look_behind"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_num_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, segmenter.gop_look_behind),
	NULL },

	{ ngx_string("vod_segment_count_policy"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_http_vod_segment_count_policy_command,
	NGX_HTTP_LOC_CONF_OFFSET,
	0,
	NULL },

	{ ngx_string("vod_manifest_duration_policy"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_enum_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, segmenter.manifest_duration_policy),
	manifest_duration_policies },

	{ ngx_string("vod_manifest_segment_durations_mode"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_http_vod_manifest_segment_durations_mode_command,
	NGX_HTTP_LOC_CONF_OFFSET,
	0,
	NULL },

	{ ngx_string("vod_force_playlist_type_vod"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_flag_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, force_playlist_type_vod),
	NULL },

	{ ngx_string("vod_force_continuous_timestamps"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_flag_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, force_continuous_timestamps),
	NULL },

	{ ngx_string("vod_force_sequence_index"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_flag_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, force_sequence_index),
	NULL },

	{ ngx_string("vod_secret_key"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_http_set_complex_value_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, secret_key),
	NULL },

	{ ngx_string("vod_encryption_iv_seed"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_http_set_complex_value_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, encryption_iv_seed),
	NULL },

	{ ngx_string("vod_base_url"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_http_set_complex_value_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, base_url),
	NULL },

	{ ngx_string("vod_segments_base_url"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_http_set_complex_value_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, segments_base_url),
	NULL },
	
	// mp4 reading parameters
	{ ngx_string("vod_metadata_cache"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE123,
	ngx_http_vod_cache_command,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, metadata_cache),
	NULL },

	{ ngx_string("vod_response_cache"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE123,
	ngx_http_vod_cache_command,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, response_cache[CACHE_TYPE_VOD]),
	NULL },

	{ ngx_string("vod_live_response_cache"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE123,
	ngx_http_vod_cache_command,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, response_cache[CACHE_TYPE_LIVE]),
	NULL },

	{ ngx_string("vod_initial_read_size"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_size_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, initial_read_size),
	NULL },

	{ ngx_string("vod_max_metadata_size"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_size_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, max_metadata_size),
	NULL },

	{ ngx_string("vod_max_frames_size"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_size_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, max_frames_size),
	NULL },

	{ ngx_string("vod_max_frame_count"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_num_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, max_frame_count),
	NULL },

	{ ngx_string("vod_segment_max_frame_count"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_num_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, segment_max_frame_count),
	NULL },

	{ ngx_string("vod_cache_buffer_size"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_size_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, cache_buffer_size),
	NULL },

	{ ngx_string("vod_ignore_edit_list"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_flag_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, ignore_edit_list),
	NULL },

	{ ngx_string("vod_parse_hdlr_name"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_flag_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, parse_hdlr_name),
	NULL },

	{ ngx_string("vod_parse_udta_name"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_flag_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, parse_udta_name),
	NULL },

	// upstream parameters - only for mapped/remote modes
	{ ngx_string("vod_max_upstream_headers_size"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_size_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, max_upstream_headers_size),
	NULL },

	{ ngx_string("vod_upstream_location"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, upstream_location),
	NULL },

	{ ngx_string("vod_remote_upstream_location"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, remote_upstream_location),
	NULL },

	{ ngx_string("vod_upstream_extra_args"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_http_set_complex_value_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, upstream_extra_args),
	NULL },

	// path request parameters - mapped mode only
	{ ngx_string("vod_mapping_cache"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE123,
	ngx_http_vod_cache_command,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, mapping_cache[CACHE_TYPE_VOD]),
	NULL },

	{ ngx_string("vod_live_mapping_cache"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE123,
	ngx_http_vod_cache_command,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, mapping_cache[CACHE_TYPE_LIVE]),
	NULL },

	{ ngx_string("vod_dynamic_mapping_cache"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE123,
	ngx_http_vod_cache_command,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, dynamic_mapping_cache),
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

	{ ngx_string("vod_max_mapping_response_size"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_size_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, max_mapping_response_size),
	NULL },

	{ ngx_string("vod_notification_uri"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_http_set_complex_value_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, notification_uri),
	NULL },

	{ ngx_string("vod_dynamic_clip_map_uri"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_http_set_complex_value_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, dynamic_clip_map_uri),
	NULL },

	{ ngx_string("vod_source_clip_map_uri"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_http_set_complex_value_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, source_clip_map_uri),
	NULL },

	{ ngx_string("vod_redirect_segments_url"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_http_set_complex_value_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, redirect_segments_url),
	NULL },

	{ ngx_string("vod_media_set_map_uri"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_http_set_complex_value_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, media_set_map_uri),
	NULL },

	{ ngx_string("vod_apply_dynamic_mapping"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_http_set_complex_value_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, apply_dynamic_mapping),
	NULL },

	{ ngx_string("vod_media_set_override_json"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_http_set_complex_value_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, media_set_override_json),
	NULL },

	// fallback upstream - only for local/mapped modes
	{ ngx_string("vod_fallback_upstream_location"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, fallback_upstream_location),
	NULL },

	{ ngx_string("vod_proxy_header_name"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, proxy_header.key),
	NULL },

	{ ngx_string("vod_proxy_header_value"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, proxy_header.value),
	NULL },

	// expires
	{ ngx_string("vod_expires"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_sec_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, expires[EXPIRES_TYPE_VOD]),
	NULL },
	
	{ ngx_string("vod_expires_live"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_sec_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, expires[EXPIRES_TYPE_LIVE]),
	NULL },

	{ ngx_string("vod_expires_live_time_dependent"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_sec_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, expires[EXPIRES_TYPE_LIVE_TIME_DEPENDENT]),
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

#if (NGX_HAVE_OPENSSL_EVP)
	// drm
	{ ngx_string("vod_drm_enabled"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_flag_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, drm_enabled),
	NULL },

	{ ngx_string("vod_drm_single_key"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_flag_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, drm_single_key),
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

	{ ngx_string("vod_drm_upstream_location"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, drm_upstream_location),
	NULL },

	{ ngx_string("vod_drm_info_cache"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE123,
	ngx_http_vod_cache_command,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, drm_info_cache),
	NULL },

	{ ngx_string("vod_drm_request_uri"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_http_set_complex_value_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, drm_request_uri),
	NULL },

	{ ngx_string("vod_min_single_nalu_per_frame_segment"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_num_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, min_single_nalu_per_frame_segment),
	NULL },
#endif // NGX_HAVE_OPENSSL_EVP

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

	{ ngx_string("vod_time_shift_param_name"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, time_shift_param_name),
	NULL },

	{ ngx_string("vod_speed_param_name"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, speed_param_name),
	NULL },
	
	{ ngx_string("vod_lang_param_name"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, lang_param_name),
	NULL },

	{ ngx_string("vod_performance_counters"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_http_vod_perf_counters_command,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, perf_counters_zone),
	NULL },

	{ ngx_string("vod_output_buffer_pool"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE2,
	ngx_http_vod_buffer_pool_command,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, output_buffer_pool),
	NULL },

#if (NGX_THREADS)
	{ ngx_string("vod_open_file_thread_pool"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_NOARGS | NGX_CONF_TAKE1,
	ngx_http_vod_thread_pool_command,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, open_file_thread_pool),
	NULL },
#endif // NGX_THREADS

#include "ngx_http_vod_dash_commands.h"
#include "ngx_http_vod_hds_commands.h"
#include "ngx_http_vod_hls_commands.h"
#include "ngx_http_vod_mss_commands.h"

#if (NGX_HAVE_LIB_AV_CODEC)
#include "ngx_http_vod_thumb_commands.h"
#include "ngx_http_vod_volume_map_commands.h"
#endif // NGX_HAVE_LIB_AV_CODEC

	ngx_null_command
};

ngx_http_module_t  ngx_http_vod_module_ctx = {
	ngx_http_vod_preconfiguration,      /* preconfiguration */
	ngx_http_vod_init_parsers,          /* postconfiguration */

	NULL,                               /* create main configuration */
	NULL,                               /* init main configuration */

	NULL,                               /* create server configuration */
	NULL,                               /* merge server configuration */

	ngx_http_vod_create_loc_conf,       /* create location configuration */
	ngx_http_vod_merge_loc_conf         /* merge location configuration */
};
