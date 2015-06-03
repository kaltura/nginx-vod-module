#include "ngx_simple_json_parser.h"
#include "ngx_http_vod_udrm.h"

// drm info json keys
ngx_str_t drm_info_key = ngx_string("key");
ngx_str_t drm_info_key_id = ngx_string("key_id");
ngx_str_t drm_info_pssh = ngx_string("pssh");
ngx_str_t drm_info_system_id = ngx_string("uuid");
ngx_str_t drm_info_data = ngx_string("data");

ngx_int_t
ngx_http_vod_udrm_parse_response(
	request_context_t* request_context,
	ngx_str_t* drm_info, 
	void** output)
{
	mp4_encrypt_info_t* result;
	ngx_json_value_t* cur_input_pssh;
	mp4_encrypt_system_info_t* cur_output_pssh;
	ngx_json_value_t parsed_info;
	ngx_json_value_t* element;
	ngx_array_t *pssh_array;
	ngx_int_t rc;
	ngx_uint_t i;
	
	result = ngx_palloc(request_context->pool, sizeof(*result));
	if (result == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, request_context->log, 0,
			"ngx_http_vod_udrm_parse_response: ngx_palloc failed (1)");
		return NGX_ERROR;
	}

	// note: drm_info is guaranteed to be null terminated
	rc = ngx_json_parse(request_context->pool, drm_info->data, &parsed_info);
	if (rc != NGX_JSON_OK)
	{
		ngx_log_error(NGX_LOG_ERR, request_context->log, 0,
			"ngx_http_vod_udrm_parse_response: ngx_json_parse failed %i", rc);
		return NGX_ERROR;
	}

	if (parsed_info.type != NGX_JSON_ARRAY || parsed_info.v.arr.nelts != 1)
	{
		ngx_log_error(NGX_LOG_ERR, request_context->log, 0,
			"ngx_http_vod_udrm_parse_response: expected a single element array");
		return NGX_ERROR;
	}

	element = (ngx_json_value_t*)parsed_info.v.arr.elts;

	rc = ngx_json_get_element_fixed_binary_string(element, &drm_info_key, result->key, sizeof(result->key));
	if (rc != NGX_JSON_OK)
	{
		ngx_log_error(NGX_LOG_ERR, request_context->log, 0,
			"ngx_http_vod_udrm_parse_response: ngx_json_get_element_fixed_binary_string(key) failed %i", rc);
		return NGX_ERROR;
	}

	rc = ngx_json_get_element_fixed_binary_string(element, &drm_info_key_id, result->key_id, sizeof(result->key_id));
	if (rc != NGX_JSON_OK)
	{
		ngx_log_error(NGX_LOG_ERR, request_context->log, 0,
			"ngx_http_vod_udrm_parse_response: ngx_json_get_element_fixed_binary_string(key_id) failed %i", rc);
		return NGX_ERROR;
	}

	rc = ngx_json_get_element_array(element, &drm_info_pssh, &pssh_array);
	if (rc != NGX_JSON_OK)
	{
		ngx_log_error(NGX_LOG_ERR, request_context->log, 0,
			"ngx_http_vod_udrm_parse_response: ngx_json_get_element_array(pssh) failed %i", rc);
		return NGX_ERROR;
	}

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
		cur_input_pssh = (ngx_json_value_t*)pssh_array->elts + i;
		cur_output_pssh = result->pssh_array.first + i;

		rc = ngx_json_get_element_guid_string(cur_input_pssh, &drm_info_system_id, cur_output_pssh->system_id);
		if (rc != NGX_JSON_OK)
		{
			ngx_log_error(NGX_LOG_ERR, request_context->log, 0,
				"ngx_http_vod_udrm_parse_response: ngx_json_get_element_guid_string(uuid) failed %i", rc);
			return NGX_ERROR;
		}

		rc = ngx_json_get_element_binary_string(request_context->pool, cur_input_pssh, &drm_info_data, &cur_output_pssh->data);
		if (rc != NGX_JSON_OK)
		{
			ngx_log_error(NGX_LOG_ERR, request_context->log, 0,
				"ngx_http_vod_udrm_parse_response: ngx_json_get_element_binary_string(data) failed %i", rc);
			return NGX_ERROR;
		}
	}

	*output = result;

	return NGX_OK;
}

