#include "json_parser.h"
#include "parse_utils.h"
#include "udrm.h"

// enums
enum {
	DRM_INFO_PARAM_IV,
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
	{ vod_string("iv"),		VOD_JSON_STRING,	DRM_INFO_PARAM_IV },
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

vod_status_t
udrm_parse_response(
	request_context_t* request_context,
	vod_str_t* drm_info, 
	bool_t base64_decode_pssh,
	void** output)
{
	vod_array_part_t* part;
	vod_json_object_t* cur_input_pssh;
	vod_json_object_t* element;
	drm_system_info_t* cur_output_pssh;
	vod_json_value_t parsed_info;
	vod_json_value_t* drm_info_values[DRM_INFO_PARAM_COUNT];
	vod_json_value_t* pssh_values[PSSH_PARAM_COUNT];
	vod_json_array_t *pssh_array;
	drm_info_t* result;
	vod_int_t rc;
	u_char error[128];
	
	// note: drm_info is guaranteed to be null terminated
	rc = vod_json_parse(request_context->pool, drm_info->data, &parsed_info, error, sizeof(error));
	if (rc != VOD_JSON_OK)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"udrm_parse_response: vod_json_parse failed %i: %s", rc, error);
		return VOD_BAD_DATA;
	}

	if (parsed_info.type != VOD_JSON_ARRAY || 
		parsed_info.v.arr.count != 1 || 
		parsed_info.v.arr.type != VOD_JSON_OBJECT)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"udrm_parse_response: expected an array containing a single object");
		return VOD_BAD_DATA;
	}

	element = (vod_json_object_t*)parsed_info.v.arr.part.first;

	vod_memzero(drm_info_values, sizeof(drm_info_values));
	
	vod_json_get_object_values(element, &drm_info_keys_hash, drm_info_values);

	if (drm_info_values[DRM_INFO_PARAM_KEY] == NULL ||
		drm_info_values[DRM_INFO_PARAM_KEY_ID] == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"udrm_parse_response: missing fields, \"key\", \"key_id\" are mandatory");
		return VOD_BAD_DATA;
	}

	result = vod_alloc(request_context->pool, sizeof(*result));
	if (result == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"udrm_parse_response: vod_alloc failed (1)");
		return VOD_ALLOC_FAILED;
	}

	rc = parse_utils_parse_fixed_base64_string(&drm_info_values[DRM_INFO_PARAM_KEY]->v.str, result->key, sizeof(result->key));
	if (rc != VOD_JSON_OK)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"udrm_parse_response: parse_utils_parse_fixed_base64_string(key) failed %i", rc);
		return VOD_BAD_DATA;
	}

	rc = parse_utils_parse_fixed_base64_string(&drm_info_values[DRM_INFO_PARAM_KEY_ID]->v.str, result->key_id, sizeof(result->key_id));
	if (rc != VOD_JSON_OK)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"udrm_parse_response: parse_utils_parse_fixed_base64_string(key_id) failed %i", rc);
		return VOD_BAD_DATA;
	}

	if (drm_info_values[DRM_INFO_PARAM_IV] != NULL)
	{
		rc = parse_utils_parse_fixed_base64_string(&drm_info_values[DRM_INFO_PARAM_IV]->v.str, result->iv, sizeof(result->iv));
		if (rc != VOD_JSON_OK)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"udrm_parse_response: parse_utils_parse_fixed_base64_string(iv) failed %i", rc);
			return VOD_BAD_DATA;
		}

		result->iv_set = TRUE;
	}
	else
	{
		result->iv_set = FALSE;
	}

	if (drm_info_values[DRM_INFO_PARAM_PSSH] == NULL)
	{
		result->pssh_array.count = 0;
		result->pssh_array.first = NULL;
		result->pssh_array.last = NULL;
		*output = result;
		return VOD_OK;
	}

	pssh_array = &drm_info_values[DRM_INFO_PARAM_PSSH]->v.arr;
	if (pssh_array->type != VOD_JSON_OBJECT)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"udrm_parse_response: invalid pssh element type %d expected object", pssh_array->type);
		return VOD_BAD_DATA;
	}

	result->pssh_array.count = pssh_array->count;
	result->pssh_array.first = vod_alloc(
		request_context->pool, 
		sizeof(*result->pssh_array.first) * result->pssh_array.count);
	if (result->pssh_array.first == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"udrm_parse_response: vod_alloc failed (2)");
		return VOD_ALLOC_FAILED;
	}
	result->pssh_array.last = result->pssh_array.first + result->pssh_array.count;

	part = &pssh_array->part;
	for (cur_input_pssh = part->first, cur_output_pssh = result->pssh_array.first;
		;
		cur_input_pssh++, cur_output_pssh++)
	{
		if ((void*)cur_input_pssh >= part->last)
		{
			if (part->next == NULL)
			{
				break;
			}

			part = part->next;
			cur_input_pssh = part->first;
		}

		vod_memzero(pssh_values, sizeof(pssh_values));
		
		vod_json_get_object_values(cur_input_pssh, &pssh_keys_hash, pssh_values);
		
		if (pssh_values[PSSH_PARAM_SYSTEM_ID] == NULL ||
			pssh_values[PSSH_PARAM_DATA] == NULL)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"udrm_parse_response: missing pssh fields, \"uuid\", \"data\" are mandatory");
			return VOD_BAD_DATA;
		}

		rc = parse_utils_parse_guid_string(&pssh_values[PSSH_PARAM_SYSTEM_ID]->v.str, cur_output_pssh->system_id);
		if (rc != VOD_JSON_OK)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"udrm_parse_response: parse_utils_parse_guid_string(uuid) failed %i", rc);
			return VOD_BAD_DATA;
		}

		if (base64_decode_pssh)
		{
			rc = parse_utils_parse_variable_base64_string(request_context->pool, &pssh_values[PSSH_PARAM_DATA]->v.str, &cur_output_pssh->data);
			if (rc != VOD_JSON_OK)
			{
				vod_log_error(VOD_LOG_ERR, request_context->log, 0,
					"udrm_parse_response: parse_utils_parse_variable_base64_string(data) failed %i", rc);
				return VOD_BAD_DATA;
			}
		}
		else
		{
			cur_output_pssh->data.data = vod_pstrdup(request_context->pool, &pssh_values[PSSH_PARAM_DATA]->v.str);
			if (cur_output_pssh->data.data == NULL)
			{
				vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
					"udrm_parse_response: vod_pstrdup failed");
				return VOD_ALLOC_FAILED;
			}
			cur_output_pssh->data.len = pssh_values[PSSH_PARAM_DATA]->v.str.len;
		}
	}

	*output = result;

	return VOD_OK;
}

vod_status_t
udrm_init_parser(vod_pool_t* pool, vod_pool_t* temp_pool)
{
	vod_status_t rc;
	
	rc = vod_json_init_hash(
		pool,
		temp_pool,
		"drm_info_keys_hash",
		drm_info_keys_def,
		sizeof(drm_info_keys_def[0]),
		&drm_info_keys_hash);
	if (rc != VOD_OK)
	{
		return rc;
	}

	rc = vod_json_init_hash(
		pool,
		temp_pool,
		"pssh_keys_hash",
		pssh_keys_def,
		sizeof(pssh_keys_def[0]),
		&pssh_keys_hash);
	if (rc != VOD_OK)
	{
		return rc;
	}

	return VOD_OK;
}
