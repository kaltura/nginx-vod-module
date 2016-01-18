#include "vod/json_parser.h"
#include "vod/parse_utils.h"
#include "ngx_http_vod_udrm.h"

// enums
enum {
	DRM_INFO_PARAM_KEY,
	DRM_INFO_PARAM_KEY_ID,
	DRM_INFO_PARAM_PSSH,

	DRM_INFO_PARAM_COUNT
};

enum {
	PSSH_PARAM_SYSTEM_ID,
	PSSH_PARAM_DATA,

	PSSH_PARAM_COUNT
};

// constants
static json_object_key_def_t drm_info_keys_def[] = {
	{ vod_string("key"),	VOD_JSON_STRING,	DRM_INFO_PARAM_KEY },
	{ vod_string("key_id"),	VOD_JSON_STRING,	DRM_INFO_PARAM_KEY_ID },
	{ vod_string("pssh"),	VOD_JSON_ARRAY,		DRM_INFO_PARAM_PSSH },
	{ vod_null_string, 0, 0}
};

static json_object_key_def_t pssh_keys_def[] = {
	{ vod_string("uuid"), VOD_JSON_STRING, PSSH_PARAM_SYSTEM_ID },
	{ vod_string("data"), VOD_JSON_STRING, PSSH_PARAM_DATA },
	{ vod_null_string, 0, 0 }
};

// globals
static vod_hash_t drm_info_keys_hash;
static vod_hash_t pssh_keys_hash;

ngx_int_t
ngx_http_vod_udrm_parse_response(
	request_context_t* request_context,
	ngx_str_t* drm_info, 
	void** output)
{
	mp4_encrypt_info_t* result;
	vod_json_value_t* cur_input_pssh;
	mp4_encrypt_system_info_t* cur_output_pssh;
	vod_json_value_t parsed_info;
	vod_json_value_t* element;
	vod_json_value_t* drm_info_values[DRM_INFO_PARAM_COUNT];
	vod_json_value_t* pssh_values[PSSH_PARAM_COUNT];
	ngx_array_t *pssh_array;
	ngx_int_t rc;
	ngx_uint_t i;
	u_char error[128];
	
	// note: drm_info is guaranteed to be null terminated
	rc = vod_json_parse(request_context->pool, drm_info->data, &parsed_info, error, sizeof(error));
	if (rc != VOD_JSON_OK)
	{
		ngx_log_error(NGX_LOG_ERR, request_context->log, 0,
			"ngx_http_vod_udrm_parse_response: ngx_json_parse failed %i: %s", rc, error);
		return NGX_ERROR;
	}

	if (parsed_info.type != VOD_JSON_ARRAY || parsed_info.v.arr.nelts != 1)
	{
		ngx_log_error(NGX_LOG_ERR, request_context->log, 0,
			"ngx_http_vod_udrm_parse_response: expected a single element array");
		return NGX_ERROR;
	}

	element = (vod_json_value_t*)parsed_info.v.arr.elts;

	ngx_memzero(drm_info_values, sizeof(drm_info_values));
	
	vod_json_get_object_values(element, &drm_info_keys_hash, drm_info_values);

	if (drm_info_values[DRM_INFO_PARAM_KEY] == NULL ||
		drm_info_values[DRM_INFO_PARAM_KEY_ID] == NULL)
	{
		ngx_log_error(NGX_LOG_ERR, request_context->log, 0,
			"ngx_http_vod_udrm_parse_response: missing fields, \"key\", \"key_id\" are mandatory");
		return NGX_ERROR;
	}

	result = ngx_palloc(request_context->pool, sizeof(*result));
	if (result == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, request_context->log, 0,
			"ngx_http_vod_udrm_parse_response: ngx_palloc failed (1)");
		return NGX_ERROR;
	}

	rc = parse_utils_parse_fixed_base64_string(&drm_info_values[DRM_INFO_PARAM_KEY]->v.str, result->key, sizeof(result->key));
	if (rc != VOD_JSON_OK)
	{
		ngx_log_error(NGX_LOG_ERR, request_context->log, 0,
			"ngx_http_vod_udrm_parse_response: parse_utils_parse_fixed_base64_string(key) failed %i", rc);
		return NGX_ERROR;
	}

	rc = parse_utils_parse_fixed_base64_string(&drm_info_values[DRM_INFO_PARAM_KEY_ID]->v.str, result->key_id, sizeof(result->key_id));
	if (rc != VOD_JSON_OK)
	{
		ngx_log_error(NGX_LOG_ERR, request_context->log, 0,
			"ngx_http_vod_udrm_parse_response: parse_utils_parse_fixed_base64_string(key_id) failed %i", rc);
		return NGX_ERROR;
	}

	if (drm_info_values[DRM_INFO_PARAM_PSSH] == NULL)
	{
		result->pssh_array.count = 0;
		result->pssh_array.first = NULL;
		result->pssh_array.last = NULL;
		*output = result;
		return NGX_OK;
	}

	pssh_array = &drm_info_values[DRM_INFO_PARAM_PSSH]->v.arr;

	result->pssh_array.count = pssh_array->nelts;
	result->pssh_array.first = ngx_palloc(
		request_context->pool, 
		sizeof(*result->pssh_array.first) * result->pssh_array.count);
	if (result->pssh_array.first == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, request_context->log, 0,
			"ngx_http_vod_udrm_parse_response: ngx_palloc failed (2)");
		return NGX_ERROR;
	}
	result->pssh_array.last = result->pssh_array.first + result->pssh_array.count;

	for (i = 0; i < pssh_array->nelts; i++)
	{
		cur_input_pssh = (vod_json_value_t*)pssh_array->elts + i;
		cur_output_pssh = result->pssh_array.first + i;

		ngx_memzero(pssh_values, sizeof(pssh_values));
		
		vod_json_get_object_values(cur_input_pssh, &pssh_keys_hash, pssh_values);
		
		if (pssh_values[PSSH_PARAM_SYSTEM_ID] == NULL ||
			pssh_values[PSSH_PARAM_DATA] == NULL)
		{
			ngx_log_error(NGX_LOG_ERR, request_context->log, 0,
				"ngx_http_vod_udrm_parse_response: missing pssh fields, \"uuid\", \"data\" are mandatory");
			return NGX_ERROR;
		}

		rc = parse_utils_parse_guid_string(&pssh_values[PSSH_PARAM_SYSTEM_ID]->v.str, cur_output_pssh->system_id);
		if (rc != VOD_JSON_OK)
		{
			ngx_log_error(NGX_LOG_ERR, request_context->log, 0,
				"ngx_http_vod_udrm_parse_response: parse_utils_parse_guid_string(uuid) failed %i", rc);
			return NGX_ERROR;
		}

		rc = parse_utils_parse_variable_base64_string(request_context->pool, &pssh_values[PSSH_PARAM_DATA]->v.str, &cur_output_pssh->data);
		if (rc != VOD_JSON_OK)
		{
			ngx_log_error(NGX_LOG_ERR, request_context->log, 0,
				"ngx_http_vod_udrm_parse_response: parse_utils_parse_variable_base64_string(data) failed %i", rc);
			return NGX_ERROR;
		}
	}

	*output = result;

	return NGX_OK;
}

ngx_int_t
ngx_http_vod_udrm_init_parser(ngx_conf_t* cf)
{
	vod_status_t rc;
	
	rc = vod_json_init_hash(
		cf->pool,
		cf->temp_pool,
		"drm_info_keys_hash",
		drm_info_keys_def,
		sizeof(drm_info_keys_def[0]),
		&drm_info_keys_hash);
	if (rc != VOD_OK)
	{
		return NGX_ERROR;
	}

	rc = vod_json_init_hash(
		cf->pool,
		cf->temp_pool,
		"pssh_keys_hash",
		pssh_keys_def,
		sizeof(pssh_keys_def[0]),
		&pssh_keys_hash);
	if (rc != VOD_OK)
	{
		return NGX_ERROR;
	}

	return NGX_OK;
}
