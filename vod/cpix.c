#include "cpix.h"
#include "parse_utils.h"
#include "udrm.h"
#include "xml.h"


#define CPIX_MAX_KEY_DRM_SYSTEMS (5)


typedef struct {
	ngx_str_node_t sn;
	vod_int_t index;
	int64_t start;
	int64_t end;
} cpix_period_t;

typedef struct {
	uint32_t media_type_mask;
	uint32_t min_bitrate;
	uint32_t max_bitrate;
	ngx_list_t periods;		// cpix_period_t*

	// video
	uint32_t min_pixels;
	uint32_t max_pixels;
	uint32_t min_fps;
	uint32_t max_fps;

	// audio
	uint32_t min_channels;
	uint32_t max_channels;
} cpix_usage_rules_t;

typedef struct {
	ngx_str_node_t sn;
	drm_info_t info;
	drm_system_info_t systems[CPIX_MAX_KEY_DRM_SYSTEMS];
	cpix_usage_rules_t usage_rules;
} cpix_content_key_t;

struct cpix_data_s {
	ngx_list_t keys;	// cpix_content_key_t
};


typedef struct {
	request_context_t* request_context;

	ngx_list_t keys;	// cpix_content_key_t
	ngx_rbtree_t keys_rbtree;
	ngx_rbtree_node_t keys_sentinel;

	ngx_rbtree_t periods_rbtree;	// cpix_period_t
	ngx_rbtree_node_t periods_sentinel;
} cpix_parse_ctx_t;

typedef struct {
	request_context_t* request_context;
	vod_str_t secret_key;
} cpix_parse_content_key_ctx_t;

typedef struct {
	request_context_t* request_context;
	drm_system_info_t* info;
} cpix_parse_drm_system_ctx_t;

typedef struct {
	cpix_parse_ctx_t* ctx;
	cpix_usage_rules_t* rules;
} cpix_parse_rule_ctx_t;


static vod_status_t cpix_parse_node_content_key_plain_value(void* context, xmlNode* node);
static vod_status_t cpix_parse_node_drm_system_content_prot(void* context, xmlNode* node);
static vod_status_t cpix_parse_node_drm_system_hls_signaling(void* context, xmlNode* node);
static vod_status_t cpix_parse_node_drm_system_mss_content_prot(void* context, xmlNode* node);
static vod_status_t cpix_parse_node_rule_period_filter(void* context, xmlNode* node);
static vod_status_t cpix_parse_node_rule_video_filter(void* context, xmlNode* node);
static vod_status_t cpix_parse_node_rule_audio_filter(void* context, xmlNode* node);
static vod_status_t cpix_parse_node_rule_bitrate_filter(void* context, xmlNode* node);
static vod_status_t cpix_parse_node_content_key(void* context, xmlNode* node);
static vod_status_t cpix_parse_node_drm_system(void* context, xmlNode* node);
static vod_status_t cpix_parse_node_period(void* context, xmlNode* node);
static vod_status_t cpix_parse_node_rule(void* context, xmlNode* node);

static void cpix_parse_init_usage_rules(request_context_t* request_context, cpix_usage_rules_t* rules);


static xml_node_handler_t cpix_node_content_key_secret[] = {
	XML_NODE_HANDLER("PlainValue", cpix_parse_node_content_key_plain_value),
	XML_NODE_LAST
};

static xml_node_handler_t cpix_node_content_key_data[] = {
	XML_NODE_CONTAINER("Secret", cpix_node_content_key_secret),
	XML_NODE_LAST
};

static xml_node_handler_t cpix_node_content_key[] = {
	XML_NODE_CONTAINER("Data", cpix_node_content_key_data),
	XML_NODE_LAST
};

static xml_node_handler_t cpix_node_drm_system[] = {
	XML_NODE_HANDLER("ContentProtectionData", cpix_parse_node_drm_system_content_prot),
	XML_NODE_HANDLER("HLSSignalingData", cpix_parse_node_drm_system_hls_signaling),
	XML_NODE_HANDLER("SmoothStreamingProtectionHeaderData", cpix_parse_node_drm_system_mss_content_prot),
	XML_NODE_LAST
};

static xml_node_handler_t cpix_node_usage_rule[] = {
	XML_NODE_HANDLER("KeyPeriodFilter", cpix_parse_node_rule_period_filter),
	XML_NODE_HANDLER("VideoFilter", cpix_parse_node_rule_video_filter),
	XML_NODE_HANDLER("AudioFilter", cpix_parse_node_rule_audio_filter),
	XML_NODE_HANDLER("BitrateFilter", cpix_parse_node_rule_bitrate_filter),
	XML_NODE_LAST
};

static xml_node_handler_t cpix_node_content_key_list[] = {
	XML_NODE_HANDLER("ContentKey", cpix_parse_node_content_key),
	XML_NODE_LAST
};

static xml_node_handler_t cpix_node_drm_system_list[] = {
	XML_NODE_HANDLER("DRMSystem", cpix_parse_node_drm_system),
	XML_NODE_LAST
};

static xml_node_handler_t cpix_node_period_list[] = {
	XML_NODE_HANDLER("ContentKeyPeriod", cpix_parse_node_period),
	XML_NODE_LAST
};

static xml_node_handler_t cpix_node_usage_rule_list[] = {
	XML_NODE_HANDLER("ContentKeyUsageRule", cpix_parse_node_rule),
	XML_NODE_LAST
};

static xml_node_handler_t cpix_node_cpix[] = {
	XML_NODE_CONTAINER("ContentKeyList", cpix_node_content_key_list),
	XML_NODE_CONTAINER("DRMSystemList", cpix_node_drm_system_list),
	XML_NODE_CONTAINER("ContentKeyPeriodList", cpix_node_period_list),
	XML_NODE_CONTAINER("ContentKeyUsageRuleList", cpix_node_usage_rule_list),
	XML_NODE_LAST
};

static xml_node_handler_t cpix_node_root[] = {
	XML_NODE_CONTAINER("CPIX", cpix_node_cpix),
	XML_NODE_LAST
};


static cpix_content_key_t*
cpix_parse_get_content_key(cpix_parse_ctx_t* ctx, vod_str_t* kid)
{
	uint32_t hash;

	hash = ngx_crc32_short(kid->data, kid->len);

	return (cpix_content_key_t *) ngx_str_rbtree_lookup(&ctx->keys_rbtree, kid, hash);
}


static vod_status_t
cpix_parse_node_content_key_plain_value(void* context, xmlNode* node)
{
	cpix_parse_content_key_ctx_t* ctx = context;

	xml_get_text_element_content_str(node, &ctx->secret_key);

	return VOD_OK;
}


static vod_status_t
cpix_parse_node_content_key(void* context, xmlNode* node)
{
	cpix_parse_content_key_ctx_t key_ctx;
	cpix_content_key_t* key;
	request_context_t* request_context;
	cpix_parse_ctx_t* ctx;
	vod_status_t rc;
	drm_info_t* info;
	vod_str_t kid;
	vod_str_t iv;
	uint32_t hash;

	ctx = context;
	request_context = ctx->request_context;

	key = ngx_list_push(&ctx->keys);
	if (key == NULL)
	{
		return VOD_ALLOC_FAILED;
	}

	info = &key->info;
	info->pssh_array.first = info->pssh_array.last = key->systems;
	info->pssh_array.count = 0;
	info->iv_set = FALSE;

	key_ctx.request_context = request_context;
	key_ctx.secret_key.len = 0;

	rc = xml_parse_nodes(&key_ctx, node->children, cpix_node_content_key);
	if (rc != VOD_OK)
	{
		return rc;
	}

	rc = parse_utils_parse_fixed_base64_string(&key_ctx.secret_key, info->key, sizeof(info->key));
	if (rc != VOD_OK)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"cpix_parse_node_content_key: failed to parse key %i", rc);
		return VOD_BAD_DATA;
	}

	xml_get_prop_str(node, "kid", &kid);
	rc = parse_utils_parse_guid_string(&kid, info->key_id);
	if (rc != VOD_OK)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"cpix_parse_node_content_key: failed to parse kid \"%V\"", &kid);
		return VOD_BAD_DATA;
	}

	xml_get_prop_str(node, "explicitIV", &iv);
	if (iv.len != 0)
	{
		rc = parse_utils_parse_fixed_base64_string(&iv, info->iv, sizeof(info->iv));
		if (rc != VOD_OK)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"cpix_parse_node_content_key: failed to parse explicitIV \"%V\"", &iv);
			return VOD_BAD_DATA;
		}

		info->iv_set = TRUE;
	}

	cpix_parse_init_usage_rules(ctx->request_context, &key->usage_rules);

	hash = vod_crc32_short(kid.data, kid.len);

	key->sn.node.key = hash;
	key->sn.str.len = kid.len;
	key->sn.str.data = kid.data;

	ngx_rbtree_insert(&ctx->keys_rbtree, &key->sn.node);

	return VOD_OK;
}


static vod_status_t
cpix_parse_node_drm_system_content_prot(void* context, xmlNode* node)
{
	cpix_parse_drm_system_ctx_t* ctx = context;

	return xml_get_text_element_content_base64(ctx->request_context, node, &ctx->info->dash_data);
}


static vod_status_t
cpix_parse_node_drm_system_hls_signaling(void* context, xmlNode* node)
{
	cpix_parse_drm_system_ctx_t* ctx = context;
	vod_str_t playlist;

	static vod_str_t master = vod_string("master");
	static vod_str_t media = vod_string("media");

	xml_get_prop_str(node, "playlist", &playlist);
	if (playlist.len == 0 || vod_str_equals(playlist, media))
	{
		return xml_get_text_element_content_base64(ctx->request_context, node, &ctx->info->hls_media_signaling);
	}
	else if (vod_str_equals(playlist, master))
	{
		return xml_get_text_element_content_base64(ctx->request_context, node, &ctx->info->hls_master_signaling);
	}
	else
	{
		vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
			"cpix_parse_node_drm_system_hls_signaling: invalid playlist \"%V\"", &playlist);
		return VOD_BAD_DATA;
	}
}


static vod_status_t
cpix_parse_node_drm_system_mss_content_prot(void* context, xmlNode* node)
{
	cpix_parse_drm_system_ctx_t* ctx = context;
	vod_str_t content;
	u_char* copy;

	xml_get_text_element_content_str(node, &content);
	if (content.len == 0)
	{
		return VOD_OK;
	}

	copy = vod_alloc(ctx->request_context->pool, content.len);
	if (copy == NULL)
	{
		return VOD_ALLOC_FAILED;
	}

	ngx_memcpy(copy, content.data, content.len);

	ctx->info->mss_data.data = copy;
	ctx->info->mss_data.len = content.len;

	return VOD_OK;
}


static vod_status_t
cpix_parse_node_drm_system(void* context, xmlNode* node)
{
	cpix_parse_drm_system_ctx_t sys_ctx;
	cpix_content_key_t* key;
	drm_system_info_t* system;
	cpix_parse_ctx_t* ctx;
	vod_status_t rc;
	vod_str_t system_id;
	vod_str_t kid;

	ctx = context;

	xml_get_prop_str(node, "kid", &kid);
	key = cpix_parse_get_content_key(ctx, &kid);
	if (key == NULL)
	{
		vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
			"cpix_parse_node_drm_system: key \"%V\" not found", &kid);
		return VOD_BAD_DATA;
	}

	if (key->info.pssh_array.count >= CPIX_MAX_KEY_DRM_SYSTEMS)
	{
		vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
			"cpix_parse_node_drm_system: key \"%V\" drm system count exceeds limit", &kid);
		return VOD_BAD_DATA;
	}

	system = key->info.pssh_array.last++;
	key->info.pssh_array.count++;

	vod_memzero(system, sizeof(*system));

	xml_get_prop_str(node, "systemId", &system_id);
	rc = parse_utils_parse_guid_string(&system_id, system->system_id);
	if (rc != VOD_OK)
	{
		vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
			"cpix_parse_node_drm_system: failed to parse systemId \"%V\"", &system_id);
		return VOD_BAD_DATA;
	}

	sys_ctx.request_context = ctx->request_context;
	sys_ctx.info = system;

	return xml_parse_nodes(&sys_ctx, node->children, cpix_node_drm_system);
}


static cpix_period_t*
cpix_parse_get_period(cpix_parse_ctx_t* ctx, vod_str_t* id)
{
	uint32_t hash;

	hash = ngx_crc32_short(id->data, id->len);

	return (cpix_period_t *) ngx_str_rbtree_lookup(&ctx->periods_rbtree, id, hash);
}


static vod_status_t
cpix_parse_node_period(void* context, xmlNode* node)
{
	request_context_t* request_context;
	cpix_parse_ctx_t* ctx = context;
	cpix_period_t* period;
	vod_int_t duration_int;
	vod_int_t start_int;
	vod_int_t end_int;
	vod_str_t id;
	vod_str_t index;
	vod_str_t start;
	vod_str_t end;
	vod_str_t duration;
	uint32_t hash;

	request_context = ctx->request_context;

	xml_get_prop_str(node, "id", &id);
	if (id.len == 0)
	{
		// although the "id" attribute is marked as optional in the spec, the periods
		// are referenced by id, so it's pointless to create one without id
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"cpix_parse_node_period: missing period id attribute");
		return VOD_BAD_DATA;
	}

	period = vod_alloc(request_context->pool, sizeof(*period));
	if (period == NULL)
	{
		return VOD_ALLOC_FAILED;
	}

	xml_get_prop_str(node, "index", &index);
	xml_get_prop_str(node, "startOffset", &start);
	xml_get_prop_str(node, "endOffset", &end);
	xml_get_prop_str(node, "duration", &duration);

	if (start.len != 0)
	{
		// offset based period
		start_int = xml_parse_duration(&start);
		if (start_int < 0)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"cpix_parse_node_period: failed to parse startOffset \"%V\"", &start);
			return VOD_BAD_DATA;
		}

		period->start = start_int * 1000;

		if (end.len != 0)
		{
			end_int = xml_parse_duration(&end);
			if (end_int < 0)
			{
				vod_log_error(VOD_LOG_ERR, request_context->log, 0,
					"cpix_parse_node_period: failed to parse endOffset \"%V\"", &end);
				return VOD_BAD_DATA;
			}

			if (end_int <= start_int)
			{
				vod_log_error(VOD_LOG_ERR, request_context->log, 0,
					"cpix_parse_node_period: endOffset \"%V\" is less than startOffset \"%V\"", &end, &start);
				return VOD_BAD_DATA;
			}

			period->end = end_int * 1000;
		}
		else if (duration.len != 0)
		{
			duration_int = xml_parse_duration(&duration);
			if (duration_int < 0)
			{
				vod_log_error(VOD_LOG_ERR, request_context->log, 0,
					"cpix_parse_node_period: failed to parse duration \"%V\"", &duration);
				return VOD_BAD_DATA;
			}

			period->end = period->start + duration_int * 1000;
		}
		else
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"cpix_parse_node_period: expecting either \"endOffset\" or \"duration\" attribute with \"startOffset\"");
			return VOD_BAD_DATA;
		}

		period->index = -1;
	}
	else
	{
		// index based period
		period->index = vod_atoi(index.data, index.len);
		if (period->index == NGX_ERROR)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"cpix_parse_node_period: failed to parse index \"%V\"", &index);
			return VOD_BAD_DATA;
		}

		period->start = -1;
		period->end = -1;
	}

	hash = vod_crc32_short(id.data, id.len);

	period->sn.node.key = hash;
	period->sn.str.len = id.len;
	period->sn.str.data = id.data;

	ngx_rbtree_insert(&ctx->periods_rbtree, &period->sn.node);

	return VOD_OK;
}


static void
cpix_parse_init_usage_rules(request_context_t* request_context, cpix_usage_rules_t* rules)
{
	rules->media_type_mask = 0xffffffff;
	rules->min_bitrate = 0;
	rules->max_bitrate = NGX_MAX_UINT32_VALUE;
	ngx_list_init(&rules->periods, request_context->pool, 3, sizeof(cpix_period_t*));

	rules->min_pixels = 0;
	rules->max_pixels = NGX_MAX_UINT32_VALUE;
	rules->min_fps = 0;
	rules->max_fps = NGX_MAX_UINT32_VALUE;

	rules->min_channels = 0;
	rules->max_channels = NGX_MAX_UINT32_VALUE;
}


static vod_status_t
cpix_parse_node_rule_period_filter(void* context, xmlNode* node)
{
	cpix_parse_rule_ctx_t* ctx = context;
	cpix_period_t** period_ptr;
	cpix_period_t* period;
	vod_str_t id;

	xml_get_prop_str(node, "periodId", &id);

	period = cpix_parse_get_period(ctx->ctx, &id);
	if (period == NULL)
	{
		vod_log_error(VOD_LOG_ERR, ctx->ctx->request_context->log, 0,
			"cpix_parse_node_rule_period_filter: period \"%V\" not found", &id);
		return VOD_BAD_DATA;
	}

	period_ptr = ngx_list_push(&ctx->rules->periods);
	if (period_ptr == NULL)
	{
		return VOD_ALLOC_FAILED;
	}

	*period_ptr = period;
	return VOD_OK;
}


static vod_status_t
cpix_parse_node_rule_video_filter(void* context, xmlNode* node)
{
	cpix_parse_rule_ctx_t* ctx = context;
	cpix_usage_rules_t* rules = ctx->rules;
	request_context_t* request_context = ctx->ctx->request_context;
	vod_int_t val;

	rules->media_type_mask &= (1 << MEDIA_TYPE_VIDEO);

	val = xml_get_prop_int(request_context, node, "minPixels");
	if (val >= 0)
	{
		rules->min_pixels = vod_max(rules->min_pixels, val);
	}

	val = xml_get_prop_int(request_context, node, "maxPixels");
	if (val >= 0)
	{
		rules->max_pixels = vod_min(rules->max_pixels, val);
	}

	val = xml_get_prop_int(request_context, node, "minFps");
	if (val >= 0)
	{
		rules->min_fps = vod_max(rules->min_fps, val);
	}

	val = xml_get_prop_int(request_context, node, "maxFps");
	if (val >= 0)
	{
		rules->max_fps = vod_min(rules->max_fps, val);
	}

	return VOD_OK;
}


static vod_status_t
cpix_parse_node_rule_audio_filter(void* context, xmlNode* node)
{
	cpix_parse_rule_ctx_t* ctx = context;
	cpix_usage_rules_t* rules = ctx->rules;
	request_context_t* request_context = ctx->ctx->request_context;
	vod_int_t val;

	rules->media_type_mask &= (1 << MEDIA_TYPE_AUDIO);

	val = xml_get_prop_int(request_context, node, "minChannels");
	if (val >= 0)
	{
		rules->min_channels = vod_max(rules->min_channels, val);
	}

	val = xml_get_prop_int(request_context, node, "maxChannels");
	if (val >= 0)
	{
		rules->max_channels = vod_min(rules->max_channels, val);
	}

	return VOD_OK;
}


static vod_status_t
cpix_parse_node_rule_bitrate_filter(void* context, xmlNode* node)
{
	cpix_parse_rule_ctx_t* ctx = context;
	cpix_usage_rules_t* rules = ctx->rules;
	request_context_t* request_context = ctx->ctx->request_context;
	vod_int_t val;

	val = xml_get_prop_int(request_context, node, "minBitrate");
	if (val >= 0)
	{
		rules->min_bitrate = vod_max(rules->min_bitrate, val);
	}

	val = xml_get_prop_int(request_context, node, "maxBitrate");
	if (val >= 0)
	{
		rules->max_bitrate = vod_min(rules->max_bitrate, val);
	}

	return VOD_OK;
}


static vod_status_t
cpix_parse_node_rule(void* context, xmlNode* node)
{
	cpix_parse_rule_ctx_t rule_ctx;
	cpix_content_key_t* key;
	cpix_parse_ctx_t* ctx = context;
	vod_str_t kid;

	ctx = context;

	xml_get_prop_str(node, "kid", &kid);
	key = cpix_parse_get_content_key(ctx, &kid);
	if (key == NULL)
	{
		vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
			"cpix_parse_node_rule: key \"%V\" not found", &kid);
		return VOD_BAD_DATA;
	}

	rule_ctx.ctx = ctx;
	rule_ctx.rules = &key->usage_rules;

	return xml_parse_nodes(&rule_ctx, node->children, cpix_node_usage_rule);
}


vod_status_t
cpix_parse(
	request_context_t* request_context,
	vod_str_t* source,
	cpix_data_t** res)
{
	cpix_parse_ctx_t ctx;
	cpix_data_t* data;
	vod_status_t rc;
	xmlDoc* doc;

	// note: source is guaranteed to be null terminated
	rc = xml_parse(request_context, source->data, &doc);
	if (rc != VOD_OK)
	{
		return rc;
	}

	if (ngx_list_init(&ctx.keys, request_context->pool, 5, sizeof(cpix_content_key_t)) != NGX_OK)
	{
		return VOD_ALLOC_FAILED;
	}

	ctx.request_context = request_context;
	vod_rbtree_init(&ctx.keys_rbtree, &ctx.keys_sentinel, ngx_str_rbtree_insert_value);
	vod_rbtree_init(&ctx.periods_rbtree, &ctx.periods_sentinel, ngx_str_rbtree_insert_value);

	rc = xml_parse_nodes(&ctx, xmlDocGetRootElement(doc), cpix_node_root);
	if (rc != VOD_OK)
	{
		return rc;
	}

	if (ctx.keys.part.nelts == 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"cpix_parse: no content keys found");
		return VOD_BAD_DATA;
	}

	data = vod_alloc(request_context->pool, sizeof(*data));
	if (data == NULL)
	{
		return VOD_ALLOC_FAILED;
	}

	data->keys = ctx.keys;
	*res = data;

	return VOD_OK;
}


static bool_t
cpix_usage_rules_match_media_info(
	cpix_usage_rules_t* rules,
	media_info_t* media_info)
{
	uint32_t value;

	if ((rules->media_type_mask & (1 << media_info->media_type)) == 0)
	{
		return FALSE;
	}

	value = media_info->bitrate;
	if (value < rules->min_bitrate || value > rules->max_bitrate)
	{
		return FALSE;
	}

	switch (media_info->media_type)
	{
	case MEDIA_TYPE_VIDEO:
		value = (uint32_t) media_info->u.video.width * (uint32_t) media_info->u.video.height;
		if (value < rules->min_pixels || value > rules->max_pixels)
		{
			return FALSE;
		}

		if (media_info->min_frame_duration > 0)
		{
			value = media_info->timescale / media_info->min_frame_duration;
			if (value < rules->min_fps || value > rules->max_fps)
			{
				return FALSE;
			}
		}

		break;

	case MEDIA_TYPE_AUDIO:
		value = media_info->u.audio.channels;
		if (value < rules->min_channels || value > rules->max_channels)
		{
			return FALSE;
		}

		break;
	}

	return TRUE;
}


static bool_t
cpix_usage_rules_match_period(
	cpix_usage_rules_t* rules,
	uint32_t clip_index,
	uint64_t clip_time)
{
	ngx_list_part_t* part;
	cpix_period_t** data;
	cpix_period_t* period;
	vod_uint_t i;

	if (rules->periods.part.nelts == 0)
	{
		return TRUE;
	}

	part = &rules->periods.part;
	data = part->elts;

	for (i = 0 ;; i++)
	{
		if (i >= part->nelts)
		{
			if (part->next == NULL)
			{
				break;
			}

			part = part->next;
			data = part->elts;
			i = 0;
		}

		period = data[i];

		if (period->index >= 0 && clip_index != period->index)
		{
			continue;
		}

		if (period->start >= 0 && clip_time < (uint64_t) period->start)
		{
			continue;
		}

		if (period->end >= 0 && clip_time >= (uint64_t) period->end)
		{
			continue;
		}

		return TRUE;
	}

	return FALSE;
}


static drm_info_t*
cpix_get_track_info(
	request_context_t* request_context,
	cpix_data_t* cpix,
	media_track_t* track,
	uint32_t clip_index,
	uint64_t clip_time)
{
	cpix_content_key_t* cur;
	cpix_content_key_t* data;
	ngx_list_part_t* part;
	vod_uint_t i;

	part = &cpix->keys.part;
	data = part->elts;

	for (i = 0 ;; i++)
	{
		if (i >= part->nelts)
		{
			if (part->next == NULL)
			{
				break;
			}

			part = part->next;
			data = part->elts;
			i = 0;
		}

		cur = &data[i];

		if (!cpix_usage_rules_match_media_info(&cur->usage_rules, &track->media_info))
		{
			continue;
		}

		if (!cpix_usage_rules_match_period(&cur->usage_rules, clip_index, clip_time))
		{
			continue;
		}

		vod_log_debug4(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"cpix_get_track_info: assigned key \"%V\" to track: media_type=%uD, clip_index=%uD, clip_time=%L",
			&cur->sn.str, track->media_info.media_type, clip_index, clip_time);

		return &cur->info;
	}

	return NULL;
}


vod_status_t
cpix_init_drm_info(
	request_context_t* request_context,
	media_set_t* media_set,
	cpix_data_t* cpix)
{
	media_clip_timing_t* timing = &media_set->timing;
	media_track_t* track;
	drm_info_t* drm_info;
	uint32_t clip_index = 0;

	for (track = media_set->filtered_tracks; track < media_set->filtered_tracks_end; track++)
	{
		if (track->media_info.media_type > MEDIA_TYPE_AUDIO) // subtitles
		{
			continue;
		}

		if (timing->times != NULL)
		{
			// derive the clip index from clip_start_time
			for (; clip_index + 1 < timing->total_count && track->clip_start_time >= (int64_t) timing->times[clip_index + 1]; clip_index++);
		}

		drm_info = cpix_get_track_info(request_context, cpix, track, clip_index,
			timing->times != NULL ? timing->times[clip_index] : 0);
		if (drm_info == NULL)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"cpix_init_drm_info: failed to match content key to track");
			return VOD_BAD_REQUEST;
		}

		track->file_info.drm_info = drm_info;
	}

	return VOD_OK;
}
